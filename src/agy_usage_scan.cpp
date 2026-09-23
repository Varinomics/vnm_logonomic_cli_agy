#include "agy_usage_scan.h"

#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {

constexpr int k_sqlite_read_only = 0x00000001;
constexpr int k_sqlite_read_write = 0x00000002;
constexpr int k_sqlite_create = 0x00000004;
constexpr int k_sqlite_row = 100;
constexpr int k_sqlite_done = 101;
constexpr int k_sqlite_busy = 5;
constexpr int k_sqlite_locked = 6;
constexpr std::uint64_t k_max_exact_integer = 9007199254740991ull;
constexpr std::size_t k_max_records = 100000;
constexpr std::size_t k_max_cache_bytes = 64 * 1024 * 1024;
constexpr std::uint64_t k_max_memory_snapshot_bytes = 512ull * 1024 * 1024;
constexpr std::uint64_t k_cache_fresh_for_ms = 300000;
constexpr std::string_view k_cache_format = "agy-native-usage@1";

using Sqlite_database = sqlite3;
using Sqlite_statement = sqlite3_stmt;
using Sqlite_backup = sqlite3_backup;

class Sqlite_api
{
public:
    using Open = decltype(&sqlite3_open_v2);
    using Close = decltype(&sqlite3_close_v2);
    using Prepare = decltype(&sqlite3_prepare_v2);
    using Step = decltype(&sqlite3_step);
    using Finalize = decltype(&sqlite3_finalize);
    using Column_type = decltype(&sqlite3_column_type);
    using Column_int64 = decltype(&sqlite3_column_int64);
    using Column_text = decltype(&sqlite3_column_text);
    using Column_blob = decltype(&sqlite3_column_blob);
    using Column_bytes = decltype(&sqlite3_column_bytes);
    using Busy_timeout = decltype(&sqlite3_busy_timeout);
    using Progress_handler = decltype(&sqlite3_progress_handler);
    using Backup_init = decltype(&sqlite3_backup_init);
    using Backup_step = decltype(&sqlite3_backup_step);
    using Backup_finish = decltype(&sqlite3_backup_finish);

    bool available() const { return true; }
    Open open() const { return m_open; }
    Close close() const { return m_close; }
    Prepare prepare() const { return m_prepare; }
    Step step() const { return m_step; }
    Finalize finalize() const { return m_finalize; }
    Column_type column_type() const { return m_column_type; }
    Column_int64 column_int64() const { return m_column_int64; }
    Column_text column_text() const { return m_column_text; }
    Column_blob column_blob() const { return m_column_blob; }
    Column_bytes column_bytes() const { return m_column_bytes; }
    Busy_timeout busy_timeout() const { return m_busy_timeout; }
    Progress_handler progress_handler() const { return m_progress_handler; }
    Backup_init backup_init() const { return m_backup_init; }
    Backup_step backup_step() const { return m_backup_step; }
    Backup_finish backup_finish() const { return m_backup_finish; }

private:
    Open m_open = &sqlite3_open_v2;
    Close m_close = &sqlite3_close_v2;
    Prepare m_prepare = &sqlite3_prepare_v2;
    Step m_step = &sqlite3_step;
    Finalize m_finalize = &sqlite3_finalize;
    Column_type m_column_type = &sqlite3_column_type;
    Column_int64 m_column_int64 = &sqlite3_column_int64;
    Column_text m_column_text = &sqlite3_column_text;
    Column_blob m_column_blob = &sqlite3_column_blob;
    Column_bytes m_column_bytes = &sqlite3_column_bytes;
    Busy_timeout m_busy_timeout = &sqlite3_busy_timeout;
    Progress_handler m_progress_handler = &sqlite3_progress_handler;
    Backup_init m_backup_init = &sqlite3_backup_init;
    Backup_step m_backup_step = &sqlite3_backup_step;
    Backup_finish m_backup_finish = &sqlite3_backup_finish;
};

Sqlite_api& sqlite_api()
{
    static Sqlite_api api;
    return api;
}

struct Sqlite_connection
{
    Sqlite_api* api = nullptr;
    Sqlite_database* database = nullptr;

    ~Sqlite_connection()
    {
        if (database) (void)api->close()(database);
    }
};

struct Sqlite_query
{
    Sqlite_api* api = nullptr;
    Sqlite_statement* statement = nullptr;

    ~Sqlite_query()
    {
        if (statement) (void)api->finalize()(statement);
    }
};

std::string path_utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<std::string> environment_value(const addon::Launch_environment& environment,
                                             std::string_view name)
{
    for (const auto& member : environment.effective_environment.as_object()) {
#ifdef _WIN32
        if (member.name.size() != name.size() || !std::equal(member.name.begin(), member.name.end(),
                name.begin(), [](unsigned char left, unsigned char right) {
                    return std::tolower(left) == std::tolower(right);
                }))
        {
            continue;
        }
#else
        if (member.name != name) continue;
#endif
        if (member.value.is_string() && !member.value.as_string().empty()) return member.value.as_string();
    }
    return std::nullopt;
}

std::optional<fs::path> native_conversation_root(const addon::Usage_scan_request& request)
{
    if (!request.context.launch_environment) return std::nullopt;
#ifdef _WIN32
    const auto home = environment_value(*request.context.launch_environment, "USERPROFILE");
#else
    const auto home = environment_value(*request.context.launch_environment, "HOME");
#endif
    if (!home || home->find('\0') != std::string::npos || !addon::is_valid_utf8(*home)) return std::nullopt;
    return addon::path_from_utf8(*home) / ".gemini" / "antigravity-cli" / "conversations";
}

bool cancelled(const std::function<bool()>& is_cancelled)
{
    return is_cancelled && is_cancelled();
}

int sqlite_progress(void* opaque)
{
    const auto* is_cancelled = static_cast<const std::function<bool()>*>(opaque);
    return cancelled(*is_cancelled) ? 1 : 0;
}

bool parse_varint(std::string_view bytes, std::size_t* offset, std::uint64_t* value)
{
    *value = 0;
    unsigned shift = 0;
    while (*offset < bytes.size() && shift < 70) {
        const auto byte = static_cast<unsigned char>(bytes[(*offset)++]);
        if (shift == 63 && (byte & 0xfe) != 0) return false;
        *value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

template<typename Visitor>
bool visit_proto_fields(std::string_view bytes, Visitor&& visitor)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint64_t key = 0;
        if (!parse_varint(bytes, &offset, &key) || key == 0) return false;
        const auto field_number = static_cast<std::uint32_t>(key >> 3);
        const auto wire_type = static_cast<std::uint8_t>(key & 7);
        if (field_number == 0 || field_number > 0x1fffffff) return false;
        std::uint64_t value = 0;
        std::string_view data;
        if (wire_type == 0) {
            if (!parse_varint(bytes, &offset, &value)) return false;
        }
        else if (wire_type == 1) {
            if (bytes.size() - offset < 8) return false;
            data = bytes.substr(offset, 8);
            offset += 8;
        }
        else if (wire_type == 2) {
            std::uint64_t size = 0;
            if (!parse_varint(bytes, &offset, &size) || size > bytes.size() - offset) return false;
            data = bytes.substr(offset, static_cast<std::size_t>(size));
            offset += static_cast<std::size_t>(size);
        }
        else if (wire_type == 5) {
            if (bytes.size() - offset < 4) return false;
            data = bytes.substr(offset, 4);
            offset += 4;
        }
        else {
            return false;
        }
        if (!visitor(field_number, wire_type, value, data)) return false;
    }
    return true;
}

std::optional<std::string> completed_date(std::string_view timestamp)
{
    std::optional<std::int64_t> seconds;
    std::optional<std::uint64_t> nanos;
    const bool valid = visit_proto_fields(timestamp, [&](std::uint32_t number, std::uint8_t wire,
            std::uint64_t value, std::string_view) {
        if (number == 1) {
            if (wire != 0 || seconds) return false;
            seconds = static_cast<std::int64_t>(value);
        }
        else if (number == 2) {
            if (wire != 0 || nanos) return false;
            nanos = value;
        }
        return true;
    });
    if (!valid || !seconds || (nanos && *nanos > 999999999)) return std::nullopt;
    const auto instant = std::chrono::sys_seconds(std::chrono::seconds(*seconds));
    const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(instant)};
    if (!date.ok() || static_cast<int>(date.year()) < 1 || static_cast<int>(date.year()) > 9999) return std::nullopt;
    std::array<char, 11> buffer{};
    const auto year = static_cast<unsigned>(static_cast<int>(date.year()));
    const auto month = static_cast<unsigned>(date.month());
    const auto day = static_cast<unsigned>(date.day());
    buffer[0] = static_cast<char>('0' + year / 1000 % 10);
    buffer[1] = static_cast<char>('0' + year / 100 % 10);
    buffer[2] = static_cast<char>('0' + year / 10 % 10);
    buffer[3] = static_cast<char>('0' + year % 10);
    buffer[4] = '-';
    buffer[5] = static_cast<char>('0' + month / 10);
    buffer[6] = static_cast<char>('0' + month % 10);
    buffer[7] = '-';
    buffer[8] = static_cast<char>('0' + day / 10);
    buffer[9] = static_cast<char>('0' + day % 10);
    return std::string(buffer.data(), 10);
}

std::string request_fingerprint(const addon::Usage_scan_request& request)
{
    auto frozen = request;
    frozen.snapshot_id.reset();
    frozen.cursor.reset();
    return addon::to_hex(addon::Sha256::digest(
        addon::serialize_json_canonical(addon::encode_usage_scan_request(frozen, 3))));
}

bool unsigned_utf8_less(std::string_view left, std::string_view right)
{
    return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
        [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
}

struct Native_file_state
{
    bool exists = false;
    std::uint64_t size = 0;
    fs::file_time_type modified{};
    std::string sha256;
    friend bool operator==(const Native_file_state&, const Native_file_state&) = default;
};

std::optional<std::string> native_file_sha256(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    addon::Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!file.eof()) return std::nullopt;
    return addon::to_hex(hash.finish());
}

std::optional<Native_file_state> native_file_state(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return Native_file_state{};
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;
    Native_file_state state;
    state.exists = true;
    state.size = fs::file_size(path, error);
    if (error) return std::nullopt;
    state.modified = fs::last_write_time(path, error);
    if (error) return std::nullopt;
    const auto digest = native_file_sha256(path);
    if (!digest) return std::nullopt;
    state.sha256 = *digest;
    return state;
}

std::optional<std::vector<Native_file_state>> native_database_states(const fs::path& path)
{
    std::vector<Native_file_state> states;
    for (const auto& suffix : {std::string{}, std::string("-wal")}) {
        auto sidecar = path;
        sidecar += suffix;
        const auto state = native_file_state(sidecar);
        if (!state) return std::nullopt;
        states.push_back(*state);
    }
    return states;
}

std::optional<std::uint64_t> sqlite_pragma_integer(Sqlite_api& api, Sqlite_database* database,
                                                  const char* sql)
{
    Sqlite_query query{&api};
    if (api.prepare()(database, sql, -1, &query.statement, nullptr) != SQLITE_OK ||
        api.step()(query.statement) != k_sqlite_row)
    {
        return std::nullopt;
    }
    const auto value = api.column_int64()(query.statement, 0);
    return value < 0 ? std::nullopt : std::optional<std::uint64_t>(static_cast<std::uint64_t>(value));
}

Native_usage_decode_result decode_native_usage_step_impl(
    std::string_view metadata,
    std::string_view cascade_id,
    std::uint64_t step_index)
{
    std::optional<std::string_view> timestamp;
    std::optional<std::string_view> model_usage;
    const bool metadata_valid = visit_proto_fields(metadata, [&](std::uint32_t number, std::uint8_t wire,
            std::uint64_t, std::string_view data) {
        if (number == 8 || number == 9) {
            if (wire != 2) return false;
            auto& member = number == 8 ? timestamp : model_usage;
            if (member) return false;
            member = data;
        }
        return true;
    });
    if (!metadata_valid) return {Native_usage_decode_status::INVALID, std::nullopt};
    if (!model_usage) return {Native_usage_decode_status::NO_USAGE, std::nullopt};
    if (!timestamp || cascade_id.empty() || cascade_id.size() > 128 ||
        !addon::is_valid_utf8(cascade_id) || cascade_id.find('\0') != std::string_view::npos)
    {
        return {Native_usage_decode_status::INVALID, std::nullopt};
    }

    std::optional<std::uint64_t> input;
    std::optional<std::uint64_t> output;
    std::optional<std::uint64_t> cache_read;
    std::optional<std::string> response_id;
    std::optional<std::string> model;
    const bool usage_valid = visit_proto_fields(*model_usage, [&](std::uint32_t number, std::uint8_t wire,
            std::uint64_t value, std::string_view data) {
        std::optional<std::uint64_t>* counter = nullptr;
        if (number == 2) counter = &input;
        else if (number == 3) counter = &output;
        else if (number == 5) counter = &cache_read;
        if (counter) {
            if (wire != 0 || counter->has_value() || value > k_max_exact_integer) return false;
            *counter = value;
        }
        else if (number == 1) {
            if (wire != 2 || model.has_value() || !addon::is_valid_utf8(data) ||
                data.find('\0') != std::string_view::npos)
            {
                return false;
            }
            model = std::string(data);
        }
        else if (number == 11) {
            if (wire != 2 || response_id.has_value() || data.empty() || data.size() > 128 ||
                !addon::is_valid_utf8(data) || data.find('\0') != std::string_view::npos)
            {
                return false;
            }
            response_id = std::string(data);
        }
        return true;
    });
    const auto date = completed_date(*timestamp);
    if (!usage_valid || !input || !output || !response_id || !date ||
        *input > k_max_exact_integer - *output)
    {
        return {Native_usage_decode_status::INVALID, std::nullopt};
    }

    const std::string session_key = "agy:" + std::string(cascade_id);
    const std::string record_id = session_key + ":step:" + std::to_string(step_index) + ":response:" + *response_id;
    if (record_id.size() > 256) return {Native_usage_decode_status::INVALID, std::nullopt};
    addon::Usage_record record;
    record.record_id = record_id;
    record.session_key = session_key;
    record.completed_date = *date;
    record.total_tokens = *input + *output;
    record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
    const auto coverage = [](addon::Usage_amount& amount, addon::Usage_support support) {
        amount.coverage = addon::Usage_field_coverage{
            amount.value.has_value() ? 1u : 0u, 1, false, support};
    };
    record.input = {true, *input};
    record.output = {true, *output};
    record.cache_read = {true, cache_read};
    record.cache_creation = {true, std::nullopt};
    coverage(record.input, addon::Usage_support::SUPPORTED);
    coverage(record.output, addon::Usage_support::SUPPORTED);
    coverage(record.cache_read, cache_read ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN);
    coverage(record.cache_creation, addon::Usage_support::UNKNOWN);
    record.cost_microusd = {false, std::nullopt};
    record.model = std::move(model);
    record.counter_field = "input_tokens + output_tokens";
    record.source_counter_semantics = "per_event_sum";
    record.aggregate_evidence = "complete";
    return {Native_usage_decode_status::DECODED, std::move(record)};
}

std::optional<std::vector<addon::Usage_record>> scan_native_history(
    const fs::path& conversations,
    const std::function<bool()>& is_cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& report_progress,
    std::string* out_error)
{
    auto& api = sqlite_api();
    if (!api.available()) {
        *out_error = "native_sqlite_unavailable";
        return std::nullopt;
    }
    std::error_code error;
    const auto root_status = fs::symlink_status(conversations, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::vector<addon::Usage_record>{};
    }
    if (error || !fs::is_directory(root_status) || fs::is_symlink(root_status)) {
        *out_error = "native_history_unavailable";
        return std::nullopt;
    }
    std::vector<fs::path> databases;
    for (fs::directory_iterator item(conversations, error), end; !error && item != end; item.increment(error)) {
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
        const auto status = item->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status) || !fs::is_regular_file(status) || item->path().extension() != ".db") continue;
        databases.push_back(item->path());
    }
    if (error) {
        *out_error = "native_history_unavailable";
        return std::nullopt;
    }
    std::sort(databases.begin(), databases.end(), [](const auto& left, const auto& right) {
        return unsigned_utf8_less(path_utf8(left.filename()), path_utf8(right.filename()));
    });

    std::vector<std::vector<Native_file_state>> initial_states;
    initial_states.reserve(databases.size());
    for (const auto& path : databases) {
        const auto states = native_database_states(path);
        if (!states || !states->front().exists) {
            *out_error = "native_database_unreadable";
            return std::nullopt;
        }
        initial_states.push_back(*states);
    }

    std::vector<addon::Usage_record> records;
    std::uint64_t processed_databases = 0;
    for (std::size_t database_index = 0; database_index < databases.size(); ++database_index) {
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
        const auto& path = databases[database_index];
        Sqlite_connection source{&api};
        const auto source_path = path_utf8(path);
        if (api.open()(source_path.c_str(), &source.database, k_sqlite_read_only, nullptr) != SQLITE_OK ||
            !source.database)
        {
            *out_error = "native_database_unreadable";
            return std::nullopt;
        }
        (void)api.busy_timeout()(source.database, 300);
        api.progress_handler()(source.database, 1000, sqlite_progress,
            const_cast<std::function<bool()>*>(&is_cancelled));

        const auto page_size = sqlite_pragma_integer(api, source.database, "PRAGMA page_size");
        const auto page_count = sqlite_pragma_integer(api, source.database, "PRAGMA page_count");
        if (!page_size || !page_count || *page_size == 0 ||
            *page_size > k_max_memory_snapshot_bytes ||
            *page_count > k_max_memory_snapshot_bytes / *page_size)
        {
            *out_error = "native_history_memory_snapshot_limit_exceeded";
            return std::nullopt;
        }

        Sqlite_connection snapshot{&api};
        if (api.open()(":memory:", &snapshot.database, k_sqlite_read_write | k_sqlite_create, nullptr) != SQLITE_OK ||
            !snapshot.database)
        {
            *out_error = "native_database_memory_snapshot_failed";
            return std::nullopt;
        }
        api.progress_handler()(snapshot.database, 1000, sqlite_progress,
            const_cast<std::function<bool()>*>(&is_cancelled));
        Sqlite_backup* backup = api.backup_init()(snapshot.database, "main", source.database, "main");
        if (!backup) {
            *out_error = "native_database_memory_snapshot_failed";
            return std::nullopt;
        }
        int backup_result = SQLITE_OK;
        while (backup_result == SQLITE_OK) {
            if (cancelled(is_cancelled)) {
                (void)api.backup_finish()(backup);
                *out_error = "cancelled";
                return std::nullopt;
            }
            backup_result = api.backup_step()(backup, 128);
            const int observed_pages = sqlite3_backup_pagecount(backup);
            if (observed_pages < 0 || static_cast<std::uint64_t>(observed_pages) >
                    k_max_memory_snapshot_bytes / *page_size)
            {
                (void)api.backup_finish()(backup);
                *out_error = "native_history_memory_snapshot_limit_exceeded";
                return std::nullopt;
            }
            if (backup_result == SQLITE_BUSY || backup_result == SQLITE_LOCKED) break;
        }
        const int finish_result = api.backup_finish()(backup);
        if (backup_result != SQLITE_DONE || finish_result != SQLITE_OK) {
            *out_error = cancelled(is_cancelled) ? "cancelled" : "native_database_memory_snapshot_failed";
            return std::nullopt;
        }

        Sqlite_query trajectory{&api};
        constexpr char k_trajectory_sql[] =
            "SELECT DISTINCT cascade_id FROM trajectory_meta WHERE cascade_id IS NOT NULL AND cascade_id != ''";
        if (api.prepare()(snapshot.database, k_trajectory_sql, -1, &trajectory.statement, nullptr) != 0) {
            *out_error = "native_database_schema_unsupported";
            return std::nullopt;
        }
        std::optional<std::string> cascade_id;
        for (;;) {
            const int result = api.step()(trajectory.statement);
            if (result == k_sqlite_done) break;
            if (result != k_sqlite_row) {
                *out_error = cancelled(is_cancelled) ? "cancelled" : "native_database_unreadable";
                return std::nullopt;
            }
            const auto* value = api.column_text()(trajectory.statement, 0);
            const int size = api.column_bytes()(trajectory.statement, 0);
            if (!value || size <= 0 || size > 128) {
                *out_error = "native_database_identity_invalid";
                return std::nullopt;
            }
            std::string observed(reinterpret_cast<const char*>(value), static_cast<std::size_t>(size));
            if (cascade_id && *cascade_id != observed) {
                *out_error = "native_database_identity_ambiguous";
                return std::nullopt;
            }
            cascade_id = std::move(observed);
        }
        (void)api.finalize()(trajectory.statement);
        trajectory.statement = nullptr;

        Sqlite_query steps{&api};
        constexpr char k_steps_sql[] = "SELECT idx, metadata FROM steps ORDER BY idx";
        if (api.prepare()(snapshot.database, k_steps_sql, -1, &steps.statement, nullptr) != 0) {
            *out_error = "native_database_schema_unsupported";
            return std::nullopt;
        }
        for (;;) {
            if (cancelled(is_cancelled)) {
                *out_error = "cancelled";
                return std::nullopt;
            }
            const int result = api.step()(steps.statement);
            if (result == k_sqlite_done) break;
            if (result != k_sqlite_row) {
                *out_error = cancelled(is_cancelled) ? "cancelled" : "native_database_unreadable";
                return std::nullopt;
            }
            const auto step_index_value = api.column_int64()(steps.statement, 0);
            if (step_index_value < 0 || api.column_type()(steps.statement, 1) == 5) continue;
            const auto* blob = api.column_blob()(steps.statement, 1);
            const int size = api.column_bytes()(steps.statement, 1);
            if (!blob || size < 0 || size > 4 * 1024 * 1024) {
                *out_error = "native_usage_metadata_invalid";
                return std::nullopt;
            }
            const auto effective_cascade_id = cascade_id.value_or(path_utf8(path.stem()));
            const auto decoded = decode_native_usage_step_impl(
                std::string_view(static_cast<const char*>(blob), static_cast<std::size_t>(size)),
                effective_cascade_id,
                static_cast<std::uint64_t>(step_index_value));
            if (decoded.status == Native_usage_decode_status::INVALID) {
                *out_error = "native_usage_metadata_invalid";
                return std::nullopt;
            }
            if (decoded.record) {
                records.push_back(*decoded.record);
                if (records.size() > k_max_records) {
                    *out_error = "native_usage_history_limit_exceeded";
                    return std::nullopt;
                }
            }
        }
        ++processed_databases;
        if (report_progress) report_progress(processed_databases, databases.size());
    }

    std::error_code enumeration_error;
    std::vector<fs::path> final_databases;
    for (fs::directory_iterator item(conversations, enumeration_error), end;
         !enumeration_error && item != end; item.increment(enumeration_error))
    {
        const auto status = item->symlink_status(enumeration_error);
        if (enumeration_error) break;
        if (!fs::is_symlink(status) && fs::is_regular_file(status) && item->path().extension() == ".db") {
            final_databases.push_back(item->path());
        }
    }
    std::sort(final_databases.begin(), final_databases.end(), [](const auto& left, const auto& right) {
        return unsigned_utf8_less(path_utf8(left.filename()), path_utf8(right.filename()));
    });
    if (enumeration_error || final_databases != databases) {
        *out_error = "native_history_changed_during_snapshot";
        return std::nullopt;
    }
    for (std::size_t index = 0; index < databases.size(); ++index) {
        const auto final_states = native_database_states(databases[index]);
        if (!final_states || *final_states != initial_states[index]) {
            *out_error = "native_history_changed_during_snapshot";
            return std::nullopt;
        }
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return unsigned_utf8_less(left.record_id, right.record_id);
    });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index - 1].record_id == records[index].record_id) {
            *out_error = "native_usage_identity_duplicate";
            return std::nullopt;
        }
    }
    return records;
}

bool write_cache_file(const fs::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    return file.good();
}

std::optional<std::string> read_cache_file(const fs::path& path, std::size_t maximum_bytes)
{
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum_bytes) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return bytes;
}

addon::Json_value cache_state(std::string_view request_hash, std::string_view snapshot_id,
                              std::uint64_t observed_at_ms, std::size_t record_count)
{
    auto value = addon::Json_value::object();
    value.set("version", addon::Json_value(1));
    value.set("request_sha256_hex", addon::Json_value(request_hash));
    value.set("snapshot_id", addon::Json_value(snapshot_id));
    value.set("observed_at_ms", addon::Json_value(observed_at_ms));
    value.set("fresh_for_ms", addon::Json_value(k_cache_fresh_for_ms));
    value.set("incomplete_historical_coverage", addon::Json_value(false));
    value.set("record_count", addon::Json_value(static_cast<std::uint64_t>(record_count)));
    return value;
}

std::optional<addon::Usage_cache_reference> seal_candidate(
    const addon::Usage_scan_request& request,
    const std::vector<addon::Usage_record>& records,
    std::string_view snapshot_id,
    std::uint64_t observed_at_ms,
    const std::function<bool()>& is_cancelled,
    std::string* out_error)
{
    const auto& selection = *request.cache_selection;
    const auto& profile = *request.context.profile_storage;
    addon::Contract_failure failure;
    if (!addon::validate_usage_cache_allocation(selection.candidate_root, profile.root, &failure)) {
        *out_error = "usage_cache_allocation_invalid";
        return std::nullopt;
    }
    if (cancelled(is_cancelled)) {
        *out_error = "cancelled";
        return std::nullopt;
    }
    const fs::path root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
    std::string serialized_records;
    for (const auto& record : records) {
        const auto line = addon::serialize_json_canonical(addon::encode_usage_record(record, 3));
        if (line.size() + 1 > k_max_cache_bytes ||
            serialized_records.size() > k_max_cache_bytes - line.size() - 1)
        {
            *out_error = "native_usage_cache_limit_exceeded";
            return std::nullopt;
        }
        serialized_records += line;
        serialized_records.push_back('\n');
    }
    const auto request_hash = request_fingerprint(request);
    const auto state = addon::serialize_json_canonical(cache_state(
        request_hash, snapshot_id, observed_at_ms, records.size()));
    if (!write_cache_file(root / "usage-records.jsonl", serialized_records) ||
        !write_cache_file(root / "scan-state.json", state))
    {
        *out_error = "native_usage_cache_write_failed";
        return std::nullopt;
    }
    auto reference = addon::seal_usage_cache_candidate(
        selection.candidate_root, profile.root, request.context.account_context,
        std::string(snapshot_id), std::string(k_cache_format), is_cancelled, &failure);
    if (!reference) {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_seal_failed";
        return std::nullopt;
    }
    return reference;
}

std::optional<addon::Usage_cache_reference> continuation_candidate(
    const addon::Usage_scan_request& request,
    const std::function<bool()>& is_cancelled,
    std::string* out_error)
{
    const auto& selection = *request.cache_selection;
    const auto& profile = *request.context.profile_storage;
    addon::Contract_failure failure;
    if (!addon::validate_usage_cache_root(selection.candidate_root, profile.root, &failure)) {
        *out_error = "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
    const auto manifest = read_cache_file(root / "cache-manifest.json", addon::k_max_usage_cache_manifest_bytes);
    if (!manifest) {
        *out_error = "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    const addon::Usage_cache_reference reference{
        selection.candidate_root,
        *request.snapshot_id,
        addon::to_hex(addon::Sha256::digest(*manifest))};
    if (!addon::validate_usage_cache_candidate(
            reference, profile.root, request.context.account_context, is_cancelled, &failure))
    {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    const auto state_bytes = read_cache_file(root / "scan-state.json", 4096);
    if (!state_bytes) {
        *out_error = "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    addon::Json_parse_error parse_error;
    const auto state = addon::parse_json(*state_bytes, addon::Json_parse_limits{}, &parse_error);
    const auto* hash = state ? state->find("request_sha256_hex") : nullptr;
    const auto* snapshot = state ? state->find("snapshot_id") : nullptr;
    if (!state || !state->is_object() || !hash || !hash->is_string() ||
        !snapshot || !snapshot->is_string() || snapshot->as_string() != *request.snapshot_id ||
        hash->as_string() != request_fingerprint(request))
    {
        *out_error = "usage_cache_request_mismatch";
        return std::nullopt;
    }
    return reference;
}

struct Cached_snapshot
{
    std::vector<addon::Usage_record> records;
    std::uint64_t observed_at_ms = 0;
    std::uint64_t fresh_for_ms = k_cache_fresh_for_ms;
    bool incomplete_historical_coverage = false;
};

std::optional<Cached_snapshot> read_cached_snapshot(
    const addon::Usage_scan_request& request,
    std::string* out_error)
{
    const fs::path root = addon::usage_cache_io_path(
        addon::path_from_utf8(request.cache_selection->candidate_root));
    const auto state_bytes = read_cache_file(root / "scan-state.json", 4096);
    const auto records_bytes = read_cache_file(root / "usage-records.jsonl", k_max_cache_bytes);
    if (!state_bytes || !records_bytes) {
        *out_error = "usage_cache_contents_invalid";
        return std::nullopt;
    }
    addon::Json_parse_error parse_error;
    const auto state = addon::parse_json(*state_bytes, addon::Json_parse_limits{}, &parse_error);
    const auto integer = [&state](std::string_view name) -> std::optional<std::uint64_t> {
        const auto* value = state ? state->find(name) : nullptr;
        if (!value || !value->is_integer() || value->as_integer() < 0) return std::nullopt;
        return static_cast<std::uint64_t>(value->as_integer());
    };
    const auto* request_hash = state ? state->find("request_sha256_hex") : nullptr;
    const auto* snapshot = state ? state->find("snapshot_id") : nullptr;
    const auto* incomplete = state ? state->find("incomplete_historical_coverage") : nullptr;
    const auto expected_count = integer("record_count");
    const auto observed_at = integer("observed_at_ms");
    const auto fresh_for = integer("fresh_for_ms");
    if (!state || !state->is_object() || !request_hash || !request_hash->is_string() ||
        request_hash->as_string() != request_fingerprint(request) ||
        !snapshot || !snapshot->is_string() || snapshot->as_string() != *request.snapshot_id ||
        !expected_count || !observed_at || !fresh_for || !incomplete || !incomplete->is_boolean() ||
        *expected_count > k_max_records)
    {
        *out_error = "usage_cache_contents_invalid";
        return std::nullopt;
    }
    Cached_snapshot result;
    result.observed_at_ms = *observed_at;
    result.fresh_for_ms = *fresh_for;
    result.incomplete_historical_coverage = incomplete->as_boolean();
    std::size_t offset = 0;
    while (offset < records_bytes->size()) {
        const auto end = records_bytes->find('\n', offset);
        if (end == std::string::npos || end == offset) {
            *out_error = "usage_cache_contents_invalid";
            return std::nullopt;
        }
        const auto line = std::string_view(*records_bytes).substr(offset, end - offset);
        const auto parsed = addon::parse_json(line, addon::Json_parse_limits{}, &parse_error);
        addon::Contract_failure failure;
        const auto record = parsed ? addon::decode_usage_record(*parsed, &failure, 3) : std::nullopt;
        if (!record || result.records.size() >= k_max_records ||
            (!result.records.empty() && !unsigned_utf8_less(result.records.back().record_id, record->record_id)))
        {
            *out_error = "usage_cache_contents_invalid";
            return std::nullopt;
        }
        result.records.push_back(*record);
        offset = end + 1;
    }
    if (result.records.size() != *expected_count) {
        *out_error = "usage_cache_contents_invalid";
        return std::nullopt;
    }
    return result;
}

std::optional<std::uint64_t> cursor_offset(std::string_view cursor)
{
    constexpr std::string_view prefix = "page:";
    if (!cursor.starts_with(prefix) || cursor.size() == prefix.size()) return std::nullopt;
    std::uint64_t value = 0;
    const auto first = cursor.data() + prefix.size();
    const auto last = cursor.data() + cursor.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value > k_max_records ||
        std::to_string(value) != cursor.substr(prefix.size())) return std::nullopt;
    return value;
}

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string make_snapshot_id(const std::vector<addon::Usage_record>& records,
                             std::string_view request_hash,
                             std::uint64_t observed_at_ms)
{
    std::string material(request_hash);
    material.push_back(':');
    material += std::to_string(observed_at_ms);
    for (const auto& record : records) {
        material.push_back('\n');
        material += record.record_id;
        material.push_back(':');
        material += std::to_string(record.total_tokens);
    }
    return "agy-usage-" + addon::to_hex(addon::Sha256::digest(material));
}

void fail_scan(addon::Invocation_context& context, std::string_view code)
{
    if (code == "cancelled" || context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    context.fail(addon::Result_status::FAILED, std::string(code));
}

} // namespace

Native_usage_decode_result decode_native_usage_step(
    std::string_view metadata,
    std::string_view cascade_id,
    std::uint64_t step_index)
{
    return decode_native_usage_step_impl(metadata, cascade_id, step_index);
}

std::optional<std::vector<addon::Usage_record>> scan_native_usage_directory(
    const fs::path& conversations,
    const std::function<bool()>& is_cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& report_progress,
    std::string* out_error)
{
    if (!out_error) return std::nullopt;
    out_error->clear();
    return scan_native_history(conversations, is_cancelled, report_progress, out_error);
}

void handle_usage_scan(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, 3);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!request->cache_selection || !request->context.profile_storage) {
        context.fail(addon::Result_status::FAILED, "usage_scan_context_missing");
        return;
    }
    const auto is_cancelled = [&context] { return context.cancel_requested(); };
    const auto& selection = *request->cache_selection;
    const auto& profile = *request->context.profile_storage;
    addon::Usage_scan_page page;
    page.scope.source_account_key = request->context.account_context
        ? std::optional<std::string>(request->context.account_context->adapter_account_key)
        : std::nullopt;
    page.account_context = request->context.account_context;
    page.profile_storage = profile;

    std::optional<addon::Usage_cache_reference> cache_candidate;
    Cached_snapshot snapshot;
    std::string error;
    if (!request->cursor) {
        if (selection.accepted && !addon::validate_usage_cache_candidate(
                *selection.accepted, profile.root, request->context.account_context, is_cancelled, &failure))
        {
            fail_scan(context, is_cancelled() ? "cancelled" : "usage_cache_accepted_invalid");
            return;
        }
        const auto native_root = native_conversation_root(*request);
        if (!native_root) {
            fail_scan(context, "native_history_home_unavailable");
            return;
        }
        const auto records = scan_native_history(*native_root, is_cancelled,
            [&context](std::uint64_t completed, std::uint64_t total) { context.report_progress(completed, total); },
            &error);
        if (!records) {
            fail_scan(context, error);
            return;
        }
        snapshot.records = *records;
        snapshot.observed_at_ms = now_ms();
        const auto snapshot_id = make_snapshot_id(snapshot.records, request_fingerprint(*request), snapshot.observed_at_ms);
        cache_candidate = seal_candidate(*request, snapshot.records, snapshot_id,
            snapshot.observed_at_ms, is_cancelled, &error);
        if (!cache_candidate) {
            fail_scan(context, error);
            return;
        }
        page.snapshot_id = snapshot_id;
    }
    else {
        const auto candidate = continuation_candidate(*request, is_cancelled, &error);
        if (!candidate) {
            fail_scan(context, error);
            return;
        }
        cache_candidate = *candidate;
        auto cached = read_cached_snapshot(*request, &error);
        if (!cached) {
            fail_scan(context, error);
            return;
        }
        snapshot = std::move(*cached);
        page.snapshot_id = *request->snapshot_id;
    }
    if (is_cancelled()) {
        fail_scan(context, "cancelled");
        return;
    }

    const auto start = request->cursor ? cursor_offset(*request->cursor) : std::optional<std::uint64_t>(0);
    if (!start || *start > snapshot.records.size() ||
        (*start == snapshot.records.size() && !snapshot.records.empty()))
    {
        fail_scan(context, "usage_scan_cursor_invalid");
        return;
    }
    const auto finish = std::min<std::uint64_t>(snapshot.records.size(), *start + addon::k_max_usage_page_records);
    page.records.insert(page.records.end(), snapshot.records.begin() + static_cast<std::ptrdiff_t>(*start),
        snapshot.records.begin() + static_cast<std::ptrdiff_t>(finish));
    page.complete = finish == snapshot.records.size();
    if (!page.complete) page.next_cursor = "page:" + std::to_string(finish);
    page.incomplete_historical_coverage = snapshot.incomplete_historical_coverage;
    page.cache_candidate = std::move(cache_candidate);
    page.scope.observed_at_ms = snapshot.observed_at_ms;
    page.scope.fresh_for_ms = snapshot.fresh_for_ms;
    if (is_cancelled()) {
        fail_scan(context, "cancelled");
        return;
    }
    context.succeed(addon::encode_usage_scan_result(page, 3));
}

} // namespace agy
