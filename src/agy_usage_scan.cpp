#include "agy_usage_scan.h"
#include "agy_file_identity.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_temporal.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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
constexpr std::uint64_t k_max_exact_integer = 9007199254740991ull;
constexpr std::size_t k_max_records = 100000;
constexpr std::size_t k_max_cache_bytes = 64 * 1024 * 1024;
constexpr std::uint64_t k_max_memory_snapshot_bytes = 512ull * 1024 * 1024;
constexpr std::uint64_t k_cache_fresh_for_ms = 300000;
constexpr std::string_view k_cache_format = "agy-native-usage@1";
constexpr std::string_view k_temporal_sidecar = "usage-temporal.jsonl";
constexpr std::string_view k_native_index_sidecar = "native-history-index.json";
constexpr std::string_view k_step_only_prefix = "step:";

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

struct native_timestamp_t
{
    std::int64_t seconds = 0;
    std::optional<std::uint64_t> nanos;
};

std::optional<native_timestamp_t> native_timestamp(std::string_view timestamp)
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
    return native_timestamp_t{*seconds, nanos};
}

std::optional<std::string> completed_date(std::string_view timestamp)
{
    const auto decoded = native_timestamp(timestamp);
    if (!decoded) return std::nullopt;
    const auto instant = std::chrono::sys_seconds(std::chrono::seconds(decoded->seconds));
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

std::optional<std::string> native_instant(std::string_view timestamp)
{
    const auto decoded = native_timestamp(timestamp);
    // The native Timestamp's real UTC range must be proved before calendar
    // conversion; chrono::year alone can wrap an out-of-range epoch value.
    if (!decoded || decoded->seconds < -62135596800ll || decoded->seconds > 253402300799ll) {
        return std::nullopt;
    }
    const auto instant = std::chrono::sys_seconds(std::chrono::seconds(decoded->seconds));
    const auto day = std::chrono::floor<std::chrono::days>(instant);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{instant - day};
    std::array<char, 31> text{};
    const int length = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02lld:%02lld:%02lld",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<long long>(time.hours().count()), static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()));
    std::string result(text.data(), static_cast<std::size_t>(length));
    if (decoded->nanos) {
        std::snprintf(text.data(), text.size(), ".%09llu", static_cast<unsigned long long>(*decoded->nanos));
        result += text.data();
    }
    result += 'Z';
    return result;
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
    std::string change_identity;
    friend bool operator==(const Native_file_state&, const Native_file_state&) = default;

    bool same_hashed_contents(const Native_file_state& other) const
    {
        return exists == other.exists && size == other.size && modified == other.modified && sha256 == other.sha256;
    }
};

std::optional<std::string> native_file_sha256(const fs::path& path,
                                              const std::function<bool()>& is_cancelled)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    addon::Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (file) {
        if (cancelled(is_cancelled)) return std::nullopt;
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!file.eof()) return std::nullopt;
    return addon::to_hex(hash.finish());
}

std::optional<Native_file_state> native_file_state(const fs::path& path,
    const std::function<bool()>& is_cancelled, bool hash_contents)
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
    if (const auto identity = native_file_change_identity(path)) state.change_identity = addon::to_hex(*identity);
    if (hash_contents) {
        const auto digest = native_file_sha256(path, is_cancelled);
        if (!digest) return std::nullopt;
        state.sha256 = *digest;
    }
    return state;
}

std::optional<std::vector<Native_file_state>> native_database_states(
    const fs::path& path,
    const std::function<bool()>& is_cancelled, bool hash_contents = true)
{
    std::vector<Native_file_state> states;
    for (const auto& suffix : {std::string{}, std::string("-wal")}) {
        auto sidecar = path;
        sidecar += suffix;
        const auto state = native_file_state(sidecar, is_cancelled, hash_contents);
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
    if (!metadata_valid) return {Native_usage_decode_status::INVALID, std::nullopt, std::nullopt};
    if (!model_usage) return {Native_usage_decode_status::NO_USAGE, std::nullopt, std::nullopt};
    if (!timestamp || cascade_id.empty() || cascade_id.size() > 128 ||
        !addon::is_valid_utf8(cascade_id) || cascade_id.find('\0') != std::string_view::npos)
    {
        return {Native_usage_decode_status::INVALID, std::nullopt, std::nullopt};
    }

    std::optional<std::uint64_t> input;
    std::optional<std::uint64_t> output;
    std::optional<std::uint64_t> cache_read;
    std::optional<std::string> response_id;
    std::optional<std::string> model;
    bool model_seen = false;
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
            if (model_seen) return false;
            model_seen = true;
            // Native usage carries a numeric model identifier; no model name can be inferred from it.
            if (wire == 0) return true;
            if (wire != 2 || !addon::is_valid_utf8(data) ||
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
    if (!usage_valid || !input || !output || !date ||
        *input > k_max_exact_integer - *output)
    {
        return {Native_usage_decode_status::INVALID, std::nullopt, std::nullopt};
    }

    std::string session_key = "agy:" + std::string(cascade_id);
    const std::string step_key = session_key + ":step:" + std::to_string(step_index);
    std::string record_id = response_id ? step_key + ":response:" + *response_id
                                        : std::string(k_step_only_prefix) + step_key;
    if (record_id.size() > 256) return {Native_usage_decode_status::INVALID, std::nullopt, std::nullopt};
    addon::Usage_record record;
    record.record_id = std::move(record_id);
    record.session_key = std::move(session_key);
    record.completed_date = *date;
    record.total_tokens = *input + *output;
    record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
    const auto coverage = [](addon::Usage_amount& amount, addon::Usage_support support, bool observed) {
        amount.coverage = addon::Usage_field_coverage{
            observed ? 1u : 0u, 1, false, support};
    };
    record.input = {true, *input};
    record.output = {true, *output};
    // Zero is the known subtotal; coverage marks a missing native counter as unobserved.
    record.cache_read = {true, cache_read.value_or(0)};
    record.cache_creation = {true, 0};
    coverage(record.input, addon::Usage_support::SUPPORTED, true);
    coverage(record.output, addon::Usage_support::SUPPORTED, true);
    coverage(record.cache_read, cache_read ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN,
        cache_read.has_value());
    coverage(record.cache_creation, addon::Usage_support::UNKNOWN, false);
    record.model = std::move(model);
    record.counter_field = "input_tokens + output_tokens";
    record.source_counter_semantics = "per_event_sum";
    record.aggregate_evidence = "complete";
    return {Native_usage_decode_status::DECODED, std::move(record), native_instant(*timestamp)};
}

struct Native_temporal_observation
{
    std::string record_id;
    std::optional<std::string> instant;
};

struct Native_database_observation
{
    std::vector<Native_file_state> states;
    std::vector<std::string> record_ids;
};

using Native_history_index = std::unordered_map<std::string, Native_database_observation>;

struct Native_usage_row
{
    addon::Usage_record record;
    std::optional<std::string> instant;
};

// A response ID can be added to a persisted SQLite step. Once the host has
// accepted either spelling, keep its published key when reading that step again.
struct Step_identity
{
    std::string response_key;
    bool retained_without_response = false;
};

using Step_identities = std::unordered_map<std::string, Step_identity>;

bool observe_response_key(Step_identities& identities, const addon::Usage_record& record)
{
    if (!record.session_key || !record.record_id.starts_with(*record.session_key)) {
        return true;
    }
    const std::string_view suffix(record.record_id.data() + record.session_key->size(),
        record.record_id.size() - record.session_key->size());
    if (!suffix.starts_with(":step:")) {
        return true;
    }
    const auto delimiter = suffix.find(":response:");
    if (delimiter == std::string_view::npos) {
        return true;
    }
    auto step = identities.find(record.record_id.substr(0, record.session_key->size() + delimiter));
    if (step == identities.end()) {
        return true;
    }
    auto& key = step->second.response_key;
    if (!key.empty() && key != record.record_id) {
        return false;
    }
    key = record.record_id;
    return true;
}

bool merge_native_copies(Native_usage_row& kept, const Native_usage_row& copy)
{
    auto& left = kept.record;
    const auto& right = copy.record;
    if (left.session_key != right.session_key || left.completed_date != right.completed_date ||
        left.input.value != right.input.value || left.output.value != right.output.value ||
        (kept.instant && copy.instant && kept.instant != copy.instant) ||
        (left.model && right.model && left.model != right.model))
    {
        return false;
    }
    const auto observed = [](const addon::Usage_amount& amount) {
        return amount.coverage && amount.coverage->covered_records != 0;
    };
    if (observed(left.cache_read) && observed(right.cache_read) && left.cache_read.value != right.cache_read.value) {
        return false;
    }
    if (observed(right.cache_read)) {
        left.cache_read = right.cache_read;
    }
    if (!left.model) {
        left.model = right.model;
    }
    if (!kept.instant) {
        kept.instant = copy.instant;
    }
    return true;
}

std::optional<std::vector<addon::Usage_record>> scan_native_history(
    const fs::path& conversations,
    const std::function<bool()>& is_cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& report_progress,
    std::string* out_error,
    std::vector<Native_temporal_observation>* temporal = nullptr,
    Native_history_index* native_index = nullptr,
    const std::vector<addon::Usage_record>& accepted_records = {},
    const std::vector<Native_temporal_observation>& accepted_temporal = {})
{
    auto& api = sqlite_api();
    if (!api.available()) {
        *out_error = "native_sqlite_unavailable";
        return std::nullopt;
    }
    std::error_code error;
    const auto root_status = fs::symlink_status(conversations, error);
    if (error == std::errc::no_such_file_or_directory) {
        if (native_index) native_index->clear();
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
    std::vector<bool> reuse_database;
    initial_states.reserve(databases.size());
    for (const auto& path : databases) {
        auto states = native_database_states(path, is_cancelled, false);
        if (!states || !states->front().exists) {
            *out_error = cancelled(is_cancelled) ? "cancelled" : "native_database_unreadable";
            return std::nullopt;
        }
        const auto cached = native_index
            ? native_index->find(path_utf8(path.filename())) : Native_history_index::iterator{};
        const bool reuse = native_index && cached != native_index->end() && cached->second.states == *states &&
            std::all_of(states->begin(), states->end(), [](const auto& file) {
                return !file.exists || !file.change_identity.empty();
            });
        if (!reuse) {
            states = native_database_states(path, is_cancelled);
            if (!states || !states->front().exists) {
                *out_error = cancelled(is_cancelled) ? "cancelled" : "native_database_unreadable";
                return std::nullopt;
            }
        }
        initial_states.push_back(*states);
        reuse_database.push_back(reuse);
    }

    Native_history_index observed_index;
    std::vector<Native_usage_row> rows;
    std::uint64_t processed_databases = 0;
    for (std::size_t database_index = 0; database_index < databases.size(); ++database_index) {
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
        const auto& path = databases[database_index];
        const auto name = path_utf8(path.filename());
        if (reuse_database[database_index]) {
            const auto& cached = native_index->at(name);
            for (const auto& id : cached.record_ids) {
                const auto record = std::lower_bound(accepted_records.begin(), accepted_records.end(), id,
                    [](const auto& item, const auto& key) { return unsigned_utf8_less(item.record_id, key); });
                const auto offset = static_cast<std::size_t>(record - accepted_records.begin());
                rows.push_back({*record, accepted_temporal[offset].instant});
            }
            observed_index.emplace(name, cached);
            ++processed_databases;
            if (report_progress) report_progress(processed_databases, databases.size());
            continue;
        }
        Native_database_observation observation;
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
        const auto cascade = cascade_id.value_or(path_utf8(path.stem()));

        Sqlite_query steps{&api};
        constexpr char k_steps_sql[] = "SELECT idx, metadata FROM steps ORDER BY idx";
        if (api.prepare()(snapshot.database, k_steps_sql, -1, &steps.statement, nullptr) != 0) {
            *out_error = "native_database_schema_unsupported";
            return std::nullopt;
        }
        std::optional<sqlite3_int64> previous_usage_step;
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
            auto decoded = decode_native_usage_step_impl(
                std::string_view(static_cast<const char*>(blob), static_cast<std::size_t>(size)),
                cascade,
                static_cast<std::uint64_t>(step_index_value));
            if (decoded.status == Native_usage_decode_status::INVALID ||
                (decoded.record && decoded.record->record_id.starts_with(k_step_only_prefix) &&
                    (!cascade_id || api.column_type()(steps.statement, 0) != SQLITE_INTEGER))) {
                *out_error = "native_usage_metadata_invalid";
                return std::nullopt;
            }
            if (decoded.record) {
                if (previous_usage_step == step_index_value) {
                    *out_error = "native_usage_identity_duplicate";
                    return std::nullopt;
                }
                previous_usage_step = step_index_value;
                observation.record_ids.push_back(decoded.record->record_id);
                rows.push_back({std::move(*decoded.record), std::move(decoded.instant)});
                if (rows.size() > k_max_records) {
                    *out_error = "native_usage_history_limit_exceeded";
                    return std::nullopt;
                }
            }
        }
        observed_index.emplace(name, std::move(observation));
        ++processed_databases;
        if (report_progress) report_progress(processed_databases, databases.size());
    }

    std::error_code enumeration_error;
    std::vector<fs::path> final_databases;
    for (fs::directory_iterator item(conversations, enumeration_error), end;
         !enumeration_error && item != end; item.increment(enumeration_error))
    {
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
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
        const auto final_states = native_database_states(databases[index], is_cancelled, !reuse_database[index]);
        const auto& initial = initial_states[index];
        // SQLite's read-only open can update WAL ownership metadata or create
        // an empty WAL. For fresh reads we have both full content digests.
        // Cached reads skip SQLite and must retain their original change stamps.
        bool stable = final_states && *final_states == initial;
        if (!stable && final_states && !reuse_database[index]) {
            const bool empty_wal = initial[1].size == 0 && (*final_states)[1].size == 0;
            stable = initial[0].same_hashed_contents((*final_states)[0]) &&
                (empty_wal || initial[1].same_hashed_contents((*final_states)[1]));
        }
        if (!stable) {
            *out_error = cancelled(is_cancelled) ? "cancelled" : "native_history_changed_during_snapshot";
            return std::nullopt;
        }
        auto& observed = observed_index.at(path_utf8(databases[index].filename())).states;
        observed = *final_states;
        for (auto& state : observed) state.sha256.clear();
    }
    Step_identities identities;
    for (const auto& record : accepted_records) {
        if (record.record_id.starts_with(k_step_only_prefix)) {
            identities[record.record_id.substr(k_step_only_prefix.size())].retained_without_response = true;
        }
    }
    for (const auto& row : rows) {
        if (row.record.record_id.starts_with(k_step_only_prefix)) {
            identities.try_emplace(row.record.record_id.substr(k_step_only_prefix.size()));
        }
    }
    std::unordered_map<std::string, std::string> aliases;
    if (!identities.empty()) {
        for (const auto& record : accepted_records) {
            if (!observe_response_key(identities, record)) {
                *out_error = "native_usage_identity_ambiguous";
                return std::nullopt;
            }
        }
        for (const auto& row : rows) {
            if (!observe_response_key(identities, row.record)) {
                *out_error = "native_usage_identity_ambiguous";
                return std::nullopt;
            }
        }
        for (const auto& [step, identity] : identities) {
            if (!identity.response_key.empty()) {
                const auto unnamed = std::string(k_step_only_prefix) + step;
                if (identity.retained_without_response) {
                    aliases.emplace(identity.response_key, unnamed);
                }
                else {
                    aliases.emplace(unnamed, identity.response_key);
                }
            }
        }
        for (auto& row : rows) {
            if (const auto alias = aliases.find(row.record.record_id); alias != aliases.end()) {
                row.record.record_id = alias->second;
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return unsigned_utf8_less(left.record.record_id, right.record.record_id);
    });
    std::unordered_set<std::string> shared;
    std::vector<addon::Usage_record> records;
    for (std::size_t index = 0; index < rows.size();) {
        auto& kept = rows[index++];
        while (index < rows.size() && rows[index].record.record_id == kept.record.record_id) {
            if (!merge_native_copies(kept, rows[index++])) {
                *out_error = "native_usage_identity_duplicate";
                return std::nullopt;
            }
            shared.insert(kept.record.record_id);
        }
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
        if (temporal) {
            temporal->push_back({kept.record.record_id, std::move(kept.instant)});
        }
        records.push_back(std::move(kept.record));
    }
    // The reuse index holds accounting keys, not individual copies or their raw
    // response IDs. Re-read overlapping or aliased sources when checking new evidence.
    if (!shared.empty() || !aliases.empty()) {
        std::erase_if(observed_index, [&](const auto& entry) {
            return std::any_of(entry.second.record_ids.begin(), entry.second.record_ids.end(),
                [&](const auto& id) { return shared.contains(id) || aliases.contains(id); });
        });
    }
    if (native_index) *native_index = std::move(observed_index);
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

bool write_cache_records(
    const fs::path& path,
    const std::vector<addon::Usage_record>& records,
    const std::function<bool()>& is_cancelled,
    std::string* out_error)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        *out_error = "native_usage_cache_write_failed";
        return false;
    }
    std::size_t bytes_written = 0;
    std::string error;
    for (const auto& record : records) {
        if (cancelled(is_cancelled)) {
            error = "cancelled";
            break;
        }
        const auto line = addon::serialize_json_canonical(addon::encode_usage_record(record, 3));
        if (line.size() + 1 > k_max_cache_bytes || bytes_written > k_max_cache_bytes - line.size() - 1) {
            error = "native_usage_cache_limit_exceeded";
            break;
        }
        file.write(line.data(), static_cast<std::streamsize>(line.size()));
        file.put('\n');
        if (!file) {
            error = "native_usage_cache_write_failed";
            break;
        }
        bytes_written += line.size() + 1;
    }
    if (error.empty() && cancelled(is_cancelled)) {
        error = "cancelled";
    }
    // Closing flushes the bounded stream buffer before the candidate can be sealed.
    file.close();
    if (error.empty() && !file) {
        error = "native_usage_cache_write_failed";
    }
    if (error.empty()) {
        return true;
    }
    std::error_code remove_error;
    fs::remove(path, remove_error);
    *out_error = std::move(error);
    return false;
}

std::optional<std::string> read_cache_file(const fs::path& path, std::size_t maximum_bytes,
                                           const std::function<bool()>& is_cancelled)
{
    if (cancelled(is_cancelled)) return std::nullopt;
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum_bytes) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    constexpr std::size_t chunk_size = 64 * 1024;
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_size) {
        if (cancelled(is_cancelled)) return std::nullopt;
        const auto length = std::min(chunk_size, bytes.size() - offset);
        file.read(bytes.data() + offset, static_cast<std::streamsize>(length));
        if (!file) return std::nullopt;
    }
    if (cancelled(is_cancelled) || file.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return bytes;
}

bool write_native_index(const fs::path& root, const fs::path& conversations, const Native_history_index& index)
{
    auto value = addon::Json_value::object();
    value.set("version", addon::Json_value(2));
    value.set("source_root", addon::Json_value(path_utf8(conversations)));
    auto databases = addon::Json_value::object();
    for (const auto& [name, observation] : index) {
        auto database = addon::Json_value::object();
        auto states = addon::Json_value::array();
        for (const auto& state : observation.states) {
            auto file = addon::Json_value::object();
            file.set("exists", addon::Json_value(state.exists));
            file.set("size", addon::Json_value(state.size));
            file.set("modified", addon::Json_value(addon::file_time_ticks_text(state.modified)));
            file.set("change_identity", addon::Json_value(state.change_identity));
            states.push_back(std::move(file));
        }
        auto ids = addon::Json_value::array();
        for (const auto& id : observation.record_ids) ids.push_back(addon::Json_value(id));
        database.set("states", std::move(states));
        database.set("record_ids", std::move(ids));
        databases.set(name, std::move(database));
    }
    value.set("databases", std::move(databases));
    return write_cache_file(root / k_native_index_sidecar, addon::serialize_json_canonical(value));
}

Native_history_index read_native_index(const fs::path& root, const fs::path& conversations,
    const std::vector<addon::Usage_record>& records, const std::function<bool()>& is_cancelled)
{
    const auto bytes = read_cache_file(root / k_native_index_sidecar, k_max_cache_bytes, is_cancelled);
    addon::Json_parse_limits limits;
    limits.max_bytes = k_max_cache_bytes;
    limits.max_members = k_max_records;
    const auto value = bytes ? addon::parse_json(*bytes, limits, nullptr) : std::nullopt;
    const auto* version = value ? value->find("version") : nullptr;
    const auto* source_root = value ? value->find("source_root") : nullptr;
    const auto* databases = value ? value->find("databases") : nullptr;
    if (!version || !version->is_integer() || version->as_integer() != 2 ||
        !source_root || !source_root->is_string() || source_root->as_string() != path_utf8(conversations) ||
        !databases || !databases->is_object())
    {
        return {};
    }
    Native_history_index index;
    std::unordered_set<std::string> owned_records;
    for (const auto& database : databases->as_object()) {
        if (cancelled(is_cancelled)) return {};
        const auto* states = database.value.find("states");
        const auto* ids = database.value.find("record_ids");
        if (!states || !states->is_array() || states->as_array().size() != 2 || !ids || !ids->is_array()) {
            return {};
        }
        Native_database_observation observation;
        for (const auto& file : states->as_array()) {
            const auto* exists = file.find("exists");
            const auto* size = file.find("size");
            const auto* modified = file.find("modified");
            const auto* identity = file.find("change_identity");
            if (!exists || !exists->is_boolean() || !size || !size->is_integer() || size->as_integer() < 0 ||
                !modified || !modified->is_string() || !identity || !identity->is_string() ||
                (!identity->as_string().empty() && identity->as_string().size() != 64))
            {
                return {};
            }
            fs::file_time_type::duration::rep ticks = 0;
            const auto& text = modified->as_string();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
            observation.states.push_back({exists->as_boolean(), static_cast<std::uint64_t>(size->as_integer()),
                fs::file_time_type{fs::file_time_type::duration{ticks}}, {}, identity->as_string()});
        }
        for (const auto& id : ids->as_array()) {
            if (!id.is_string() || !owned_records.insert(id.as_string()).second) return {};
            const auto record = std::lower_bound(records.begin(), records.end(), id.as_string(),
                [](const auto& item, const auto& key) { return unsigned_utf8_less(item.record_id, key); });
            if (record == records.end() || record->record_id != id.as_string()) return {};
            observation.record_ids.push_back(id.as_string());
        }
        index.emplace(database.name, std::move(observation));
    }
    return index;
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

void write_temporal_sidecar(const fs::path& root,
    const addon::Usage_scan_request& request,
    const std::vector<addon::Usage_record>& records,
    const std::vector<Native_temporal_observation>& observations,
    std::string_view snapshot_id,
    const std::function<bool()>& is_cancelled)
{
    // Keep the existing accounting JSONL and its byte ceiling independent.
    // The cache manifest seals this optional, streamed evidence in the same
    // candidate; a partial write remains unavailable rather than billable.
    std::ofstream file(root / k_temporal_sidecar, std::ios::binary | std::ios::trunc);
    if (!file) return;
    auto header = addon::Json_value::object();
    header.set("version", addon::Json_value(1));
    header.set("snapshot_id", addon::Json_value(snapshot_id));
    header.set("source_account_key", request.context.account_context
        ? addon::Json_value(request.context.account_context->adapter_account_key) : addon::Json_value{});
    header.set("record_count", addon::Json_value(static_cast<std::uint64_t>(records.size())));
    file << addon::serialize_json_canonical(header) << '\n';
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (cancelled(is_cancelled)) return;
        const auto& record = records[index];
        const auto accounting = addon::serialize_json_canonical(addon::encode_usage_record(record, 3));
        auto row = addon::Json_value::object();
        row.set("record_id", addon::Json_value(record.record_id));
        row.set("total_tokens", addon::Json_value(record.total_tokens));
        row.set("parent_sha256_hex", addon::Json_value(addon::to_hex(addon::Sha256::digest(accounting))));
        row.set("instant", observations[index].instant
            ? addon::Json_value(*observations[index].instant) : addon::Json_value{});
        file << addon::serialize_json_canonical(row) << '\n';
        if (!file) return;
    }
}

bool read_temporal_line(std::istream& input, std::string* line,
    const std::function<bool()>& is_cancelled)
{
    line->clear();
    for (char byte; input.get(byte);) {
        if (cancelled(is_cancelled)) return false;
        if (byte == '\n') return !line->empty();
        if (line->size() >= addon::k_max_usage_temporal_bytes) return false;
        line->push_back(byte);
    }
    return false;
}

struct Cached_temporal_evidence
{
    std::vector<std::optional<std::string>> instants;
    std::string coverage_reason = "unsupported";
};

Cached_temporal_evidence read_temporal_sidecar(const fs::path& root,
    const addon::Usage_scan_request& request,
    const std::vector<addon::Usage_record>& records,
    std::string_view snapshot_id,
    const std::function<bool()>& is_cancelled)
{
    std::error_code error;
    const auto status = fs::symlink_status(root / k_temporal_sidecar, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) {
        return {{}, "missing-evidence"};
    }
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return {};
    std::ifstream file(root / k_temporal_sidecar, std::ios::binary);
    std::string line;
    if (!file || !read_temporal_line(file, &line, is_cancelled)) return {};
    const auto header = addon::parse_json(line, addon::Json_parse_limits{}, nullptr);
    const auto* version = header ? header->find("version") : nullptr;
    const auto* snapshot = header ? header->find("snapshot_id") : nullptr;
    const auto* account = header ? header->find("source_account_key") : nullptr;
    const auto* record_count = header ? header->find("record_count") : nullptr;
    if (!header || !header->is_object() || header->as_object().size() != 4 ||
        !version || !version->is_integer() || version->as_integer() != 1 ||
        !snapshot || !snapshot->is_string() || snapshot->as_string() != snapshot_id ||
        !account || (request.context.account_context
            ? !account->is_string() || account->as_string() != request.context.account_context->adapter_account_key
            : !account->is_null()) ||
        !record_count || !record_count->is_integer() || record_count->as_integer() < 0 ||
        static_cast<std::uint64_t>(record_count->as_integer()) != records.size())
    {
        return {};
    }
    Cached_temporal_evidence evidence;
    for (const auto& record : records) {
        if (!read_temporal_line(file, &line, is_cancelled)) return {};
        const auto row = addon::parse_json(line, addon::Json_parse_limits{}, nullptr);
        const auto* id = row ? row->find("record_id") : nullptr;
        const auto* total = row ? row->find("total_tokens") : nullptr;
        const auto* parent = row ? row->find("parent_sha256_hex") : nullptr;
        const auto* instant = row ? row->find("instant") : nullptr;
        const auto accounting = addon::serialize_json_canonical(addon::encode_usage_record(record, 3));
        if (!row || !row->is_object() || row->as_object().size() != 4 ||
            !id || !id->is_string() || id->as_string() != record.record_id ||
            !total || !total->is_integer() || total->as_integer() < 0 ||
            static_cast<std::uint64_t>(total->as_integer()) != record.total_tokens ||
            !parent || !parent->is_string() || parent->as_string() != addon::to_hex(addon::Sha256::digest(accounting)) ||
            !instant || (!instant->is_null() && !instant->is_string()))
        {
            return {};
        }
        if (instant->is_string()) {
            addon::Usage_temporal proof;
            // A zero-token response still needs a valid source instant.
            const auto validation_tokens = std::max<std::uint64_t>(record.total_tokens, 1);
            proof.parent = {record.record_id, validation_tokens, std::string(snapshot_id),
                request.context.account_context
                    ? std::optional<std::string>(request.context.account_context->adapter_account_key) : std::nullopt};
            proof.portions.push_back({"instant", instant->as_string(), {}, {}, validation_tokens});
            if (!addon::decode_usage_temporal(addon::encode_usage_temporal(proof), nullptr)) return {};
            evidence.instants.emplace_back(instant->as_string());
        }
        else {
            evidence.instants.emplace_back(std::nullopt);
        }
    }
    // A missing, duplicate or trailing row invalidates the whole optional
    // group, including portions preceding the damage. Never edit its bytes.
    if (file.get() != std::char_traits<char>::eof() || file.bad()) return {};
    evidence.coverage_reason = "complete";
    return evidence;
}

void apply_temporal_evidence(addon::Usage_record& record, std::size_t index,
    const Cached_temporal_evidence& evidence, std::string_view snapshot_id,
    const std::optional<std::string>& source_account_key)
{
    if (record.capabilities.find(addon::k_usage_temporal_capability_id) && !record.temporal) {
        return;
    }
    addon::Usage_temporal temporal;
    temporal.parent = {record.record_id, record.total_tokens, std::string(snapshot_id), source_account_key};
    if (!evidence.instants.empty() && evidence.instants[index]) {
        if (record.total_tokens) {
            temporal.portions.push_back({"instant", *evidence.instants[index], {}, {}, record.total_tokens});
        }
    }
    else {
        temporal.unavailable_tokens = record.total_tokens;
        temporal.coverage_reason = evidence.instants.empty() ? evidence.coverage_reason : "unsupported";
    }
    record.temporal = std::move(temporal);
}

std::optional<addon::Usage_cache_reference> seal_candidate(
    const addon::Usage_scan_request& request,
    const std::vector<addon::Usage_record>& records,
    const std::vector<Native_temporal_observation>& temporal,
    const fs::path& conversations,
    const Native_history_index& index,
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
    if (!write_cache_records(root / "usage-records.jsonl", records, is_cancelled, out_error)) {
        return std::nullopt;
    }
    const auto request_hash = request_fingerprint(request);
    const auto state = addon::serialize_json_canonical(cache_state(
        request_hash, snapshot_id, observed_at_ms, records.size()));
    if (!write_cache_file(root / "scan-state.json", state) || !write_native_index(root, conversations, index))
    {
        *out_error = "native_usage_cache_write_failed";
        return std::nullopt;
    }
    write_temporal_sidecar(root, request, records, temporal, snapshot_id, is_cancelled);
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
    const auto manifest_hash = native_file_sha256(root / "cache-manifest.json", is_cancelled);
    if (!manifest_hash) {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    const addon::Usage_cache_reference reference{
        selection.candidate_root,
        *request.snapshot_id,
        *manifest_hash};
    if (!addon::validate_usage_cache_candidate(
            reference, profile.root, request.context.account_context, is_cancelled, &failure))
    {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_candidate_invalid";
        return std::nullopt;
    }
    const auto state_bytes = read_cache_file(root / "scan-state.json", 4096, is_cancelled);
    if (!state_bytes) {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_candidate_invalid";
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
    Cached_temporal_evidence temporal;
};

class Continuation_snapshot_cache
{
public:
    std::shared_ptr<const Cached_snapshot> find(const addon::Usage_cache_reference& reference,
        std::string_view request_hash)
    {
        std::lock_guard lock(m_mutex);
        if (m_reference == reference && m_request_hash == request_hash) {
            return m_snapshot;
        }
        return {};
    }

    void remember(addon::Usage_cache_reference reference, std::string request_hash,
        std::shared_ptr<const Cached_snapshot> snapshot)
    {
        std::lock_guard lock(m_mutex);
        m_reference = std::move(reference);
        m_request_hash = std::move(request_hash);
        // Release the replaced snapshot after unlocking, when the parameter dies.
        m_snapshot.swap(snapshot);
    }

private:
    std::mutex m_mutex;
    std::optional<addon::Usage_cache_reference> m_reference;
    std::string m_request_hash;
    std::shared_ptr<const Cached_snapshot> m_snapshot;
};

Continuation_snapshot_cache& continuation_snapshot_cache()
{
    // Retain only one immutable snapshot, admitted under the existing record and
    // serialized-byte limits. In-flight invocations hold their own shared owner.
    static Continuation_snapshot_cache cache;
    return cache;
}

std::optional<Cached_snapshot> read_cached_snapshot(
    const addon::Usage_cache_reference& reference,
    const std::function<bool()>& is_cancelled,
    std::string* out_error)
{
    if (cancelled(is_cancelled)) {
        *out_error = "cancelled";
        return std::nullopt;
    }
    const fs::path root = addon::usage_cache_io_path(addon::path_from_utf8(reference.root));
    const auto state_bytes = read_cache_file(root / "scan-state.json", 4096, is_cancelled);
    const auto records_bytes = read_cache_file(root / "usage-records.jsonl", k_max_cache_bytes, is_cancelled);
    if (!state_bytes || !records_bytes) {
        *out_error = cancelled(is_cancelled) ? "cancelled" : "usage_cache_contents_invalid";
        return std::nullopt;
    }
    if (cancelled(is_cancelled)) {
        *out_error = "cancelled";
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
        !snapshot || !snapshot->is_string() || snapshot->as_string() != reference.snapshot_id ||
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
        if (cancelled(is_cancelled)) {
            *out_error = "cancelled";
            return std::nullopt;
        }
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

bool retain_accepted_history(std::vector<addon::Usage_record>& records,
    std::vector<Native_temporal_observation>& temporal,
    const std::vector<addon::Usage_record>& accepted_records,
    const std::vector<Native_temporal_observation>& accepted_temporal,
    const std::function<bool()>& is_cancelled, std::string* out_error)
{
    std::vector<addon::Usage_record> merged_records;
    std::vector<Native_temporal_observation> merged_temporal;
    std::size_t current = 0;
    std::size_t accepted = 0;
    while (current < records.size() || accepted < accepted_records.size()) {
        if (cancelled(is_cancelled) || merged_records.size() == k_max_records) {
            *out_error = cancelled(is_cancelled) ? "cancelled" : "native_usage_history_limit_exceeded";
            return false;
        }
        if (current < records.size() && (accepted == accepted_records.size() ||
                !unsigned_utf8_less(accepted_records[accepted].record_id, records[current].record_id)))
        {
            if (accepted < accepted_records.size() && accepted_records[accepted].record_id == records[current].record_id) {
                ++accepted;
            }
            merged_records.push_back(std::move(records[current]));
            merged_temporal.push_back(std::move(temporal[current++]));
        }
        else {
            merged_records.push_back(accepted_records[accepted]);
            merged_temporal.push_back(accepted_temporal[accepted++]);
        }
    }
    records = std::move(merged_records);
    temporal = std::move(merged_temporal);
    return true;
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

std::optional<std::string> make_snapshot_id(
    const std::vector<addon::Usage_record>& records,
    std::string_view request_hash,
    std::uint64_t observed_at_ms,
    const std::function<bool()>& is_cancelled)
{
    if (cancelled(is_cancelled)) return std::nullopt;
    std::string material(request_hash);
    material.push_back(':');
    material += std::to_string(observed_at_ms);
    for (const auto& record : records) {
        if (cancelled(is_cancelled)) return std::nullopt;
        material.push_back('\n');
        material += record.record_id;
        material.push_back(':');
        material += std::to_string(record.total_tokens);
    }
    const auto snapshot_id = "agy-usage-" + addon::to_hex(addon::Sha256::digest(material));
    if (cancelled(is_cancelled)) return std::nullopt;
    return snapshot_id;
}

void fail_scan(addon::Invocation_context& context, std::string_view code)
{
    if (code == "cancelled" || context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (code == "native_history_changed_during_snapshot") {
        context.fail(addon::Result_status::FAILED, std::string(code),
            "Antigravity history changed during the scan. Wait for the current activity to finish.");
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
    Cached_snapshot loaded;
    std::shared_ptr<const Cached_snapshot> snapshot;
    const auto request_hash = request_fingerprint(*request);
    std::string error;
    if (!request->cursor) {
        context.report_progress(0, std::nullopt);
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
        Cached_snapshot accepted;
        std::vector<Native_temporal_observation> accepted_temporal;
        Native_history_index native_index;
        if (selection.accepted) {
            auto cached = read_cached_snapshot(*selection.accepted, is_cancelled, &error);
            if (!cached) {
                fail_scan(context, error);
                return;
            }
            accepted = std::move(*cached);
            const auto accepted_root = addon::usage_cache_io_path(addon::path_from_utf8(selection.accepted->root));
            const auto evidence = read_temporal_sidecar(accepted_root, *request, accepted.records,
                selection.accepted->snapshot_id, is_cancelled);
            for (std::size_t index = 0; index < accepted.records.size(); ++index) {
                accepted_temporal.push_back({accepted.records[index].record_id,
                    evidence.instants.empty() ? std::nullopt : evidence.instants[index]});
            }
            if (request->mode == "incremental" && evidence.coverage_reason == "complete") {
                native_index = read_native_index(accepted_root, *native_root, accepted.records, is_cancelled);
            }
        }
        std::vector<Native_temporal_observation> temporal;
        auto records = scan_native_history(*native_root, is_cancelled,
            [&context](std::uint64_t completed, std::uint64_t total) { context.report_progress(completed, total); },
            &error, &temporal, &native_index, accepted.records, accepted_temporal);
        if (!records || !retain_accepted_history(*records, temporal, accepted.records,
                accepted_temporal, is_cancelled, &error))
        {
            fail_scan(context, error);
            return;
        }
        loaded.records = std::move(*records);
        loaded.observed_at_ms = now_ms();
        const auto snapshot_id = make_snapshot_id(loaded.records, request_hash,
            loaded.observed_at_ms, is_cancelled);
        if (!snapshot_id) {
            fail_scan(context, "cancelled");
            return;
        }
        cache_candidate = seal_candidate(*request, loaded.records, temporal, *native_root, native_index, *snapshot_id,
            loaded.observed_at_ms, is_cancelled, &error);
        if (!cache_candidate) {
            fail_scan(context, error);
            return;
        }
        page.snapshot_id = *snapshot_id;
    }
    else {
        const auto candidate = continuation_candidate(*request, is_cancelled, &error);
        if (!candidate) {
            fail_scan(context, error);
            return;
        }
        cache_candidate = *candidate;
        // Full candidate integrity and request binding must pass before reuse.
        // A resealed change has a different manifest hash and must be decoded.
        snapshot = continuation_snapshot_cache().find(*candidate, request_hash);
        if (!snapshot) {
            auto cached = read_cached_snapshot(*candidate, is_cancelled, &error);
            if (!cached) {
                fail_scan(context, error);
                return;
            }
            loaded = std::move(*cached);
        }
        page.snapshot_id = *request->snapshot_id;
    }
    if (is_cancelled()) {
        fail_scan(context, "cancelled");
        return;
    }

    // Admit only snapshots replayed from the sealed accounting bytes. The first
    // continuation keeps the existing full decode before later pages reuse it.
    const bool remember_snapshot = request->cursor.has_value() && !snapshot;
    if (!snapshot) {
        const fs::path cache_root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
        loaded.temporal = read_temporal_sidecar(cache_root, *request, loaded.records, page.snapshot_id, is_cancelled);
        snapshot = std::make_shared<const Cached_snapshot>(std::move(loaded));
    }
    const auto start = request->cursor ? cursor_offset(*request->cursor) : std::optional<std::uint64_t>(0);
    if (!start || *start > snapshot->records.size() ||
        (*start == snapshot->records.size() && !snapshot->records.empty()))
    {
        fail_scan(context, "usage_scan_cursor_invalid");
        return;
    }
    page.incomplete_historical_coverage = snapshot->incomplete_historical_coverage;
    page.cache_candidate = std::move(cache_candidate);
    page.scope.observed_at_ms = snapshot->observed_at_ms;
    page.scope.fresh_for_ms = snapshot->fresh_for_ms;
    std::uint64_t finish = *start;
    std::size_t encoded_records_bytes = 0;
    for (; finish < snapshot->records.size(); ++finish) {
        // Measure the actual candidate cursor/completion envelope, including
        // the singleton final page where optional evidence may not fit.
        page.complete = finish + 1 == snapshot->records.size();
        if (page.complete) page.next_cursor.reset();
        else page.next_cursor = "page:" + std::to_string(finish + 1);
        const auto index = static_cast<std::size_t>(finish);
        auto record = snapshot->records[index];
        apply_temporal_evidence(record, index, snapshot->temporal, page.snapshot_id, page.scope.source_account_key);
        if (!addon::append_usage_page_record(page, record, encoded_records_bytes, 3)) {
            if (!page.records.empty()) break;
            auto accounting = record;
            if (accounting.temporal) {
                accounting.temporal->portions.clear();
                accounting.temporal->unavailable_tokens = accounting.total_tokens;
                accounting.temporal->coverage_reason = "capacity";
                if (addon::append_usage_page_record(page, accounting, encoded_records_bytes, 3)) continue;
                accounting.temporal.reset();
            }
            if (addon::append_usage_page_record(page, accounting, encoded_records_bytes, 3)) continue;
            fail_scan(context, "usage_record_transport_limit_exceeded");
            return;
        }
    }
    page.complete = finish == snapshot->records.size();
    if (page.complete) page.next_cursor.reset();
    else page.next_cursor = "page:" + std::to_string(finish);
    if (is_cancelled()) {
        fail_scan(context, "cancelled");
        return;
    }
    // Optional evidence read failures remain retryable on subsequent pages.
    if (remember_snapshot && snapshot->temporal.coverage_reason == "complete") {
        continuation_snapshot_cache().remember(*page.cache_candidate, request_hash, std::move(snapshot));
    }
    context.succeed(addon::encode_usage_scan_result(page, 3));
}

} // namespace agy
