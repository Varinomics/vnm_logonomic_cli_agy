#include "agy_usage_scan.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <sqlite3.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

namespace {

std::string test_executable;

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

void append_varint(std::string& bytes, std::uint64_t value)
{
    while (value >= 0x80) {
        bytes.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    bytes.push_back(static_cast<char>(value));
}

void append_varint_field(std::string& bytes, std::uint32_t number, std::uint64_t value)
{
    append_varint(bytes, static_cast<std::uint64_t>(number) << 3);
    append_varint(bytes, value);
}

void append_bytes_field(std::string& bytes, std::uint32_t number, std::string_view value)
{
    append_varint(bytes, (static_cast<std::uint64_t>(number) << 3) | 2);
    append_varint(bytes, value.size());
    bytes.append(value);
}

std::string metadata_with_usage(bool include_cache_read,
                                std::optional<std::string_view> response_id = "native-response-01",
                                bool numeric_model = false,
                                std::int64_t seconds = 1790084400,
                                std::optional<std::uint64_t> nanos = 123000000,
                                std::uint64_t input = 10,
                                std::uint64_t output = 7)
{
    std::string timestamp;
    append_varint_field(timestamp, 1, static_cast<std::uint64_t>(seconds));
    if (nanos) append_varint_field(timestamp, 2, *nanos);

    std::string usage;
    if (numeric_model) append_varint_field(usage, 1, 1298);
    else append_bytes_field(usage, 1, "gemini-3-pro");
    append_varint_field(usage, 2, input);
    append_varint_field(usage, 3, output);
    if (include_cache_read) append_varint_field(usage, 5, 0);
    append_bytes_field(usage, 7, "bot-native-message");
    append_varint_field(usage, 9, 3);
    append_varint_field(usage, 10, 4);
    if (response_id) append_bytes_field(usage, 11, *response_id);

    std::string metadata;
    append_bytes_field(metadata, 8, timestamp);
    append_bytes_field(metadata, 9, usage);
    return metadata;
}

std::string metadata_without_usage()
{
    std::string timestamp;
    append_varint_field(timestamp, 1, 1790084400);
    std::string metadata;
    append_bytes_field(metadata, 8, timestamp);
    return metadata;
}

std::string read_bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string sha256_file_bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    addon::Sha256 hash;
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        hash.update(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    require(!input.bad(), "hash isolated fixture bytes");
    return addon::to_hex(hash.finish());
}

void usage_scan_capability_matches_the_implemented_version()
{
    const auto source_root = fs::path(__FILE__).parent_path().parent_path();
    const auto adapter = read_bytes(source_root / "src" / "adapter.cpp");
    require(adapter.find("{\"usage-scan\", 3}") != std::string::npos,
        "adapter claims the implemented usage-scan@3 capability");
    require(fs::exists(source_root / "package" / "capabilities" / "usage-scan@3.json"),
        "usage-scan@3 package capability declaration exists");
    require(adapter.find("{addon::k_profile_storage_capability_id") != std::string::npos,
        "adapter claims the SDK-required profile-storage@1 capability");
    require(adapter.find("acknowledge_profile_storage(context)") != std::string::npos,
        "adapter handles the profile-storage@1 acknowledgment operation");
    require(fs::exists(source_root / "package" / "capabilities" / "profile-storage@1.json"),
        "profile-storage@1 package capability declaration exists");
    require(adapter.find("{addon::k_usage_temporal_capability_id, addon::k_usage_temporal_version}") != std::string::npos &&
            fs::exists(source_root / "package" / "capabilities" / "usage-temporal@1.json"),
        "adapter and package declare independently versioned usage-temporal@1");
}

class Temporary_directory
{
public:
    Temporary_directory()
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::canonical(fs::temp_directory_path()) / ("agy-usage-test-" + std::to_string(unique));
        if (!fs::create_directory(m_path)) throw std::runtime_error("create test directory");
    }
    ~Temporary_directory() { std::error_code error; fs::remove_all(m_path, error); }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

class Usage_invocation : public addon::Invocation_context
{
public:
    Usage_invocation(const addon::Usage_scan_request& scan,
        std::function<void(std::uint64_t)> progress = {})
    :
        m_progress(std::move(progress))
    {
        m_request.operation = addon::k_usage_scan_operation;
        m_request.payload = addon::encode_usage_scan_request(scan, 3);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancel || (m_cancel_when && m_cancel_when()); }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t>, std::string) override
    {
        if (m_progress) {
            m_progress(completed);
        }
    }
    bool finished() const override { return m_terminal_count != 0; }
    void succeed(addon::Json_value payload) override
    {
        ++m_terminal_count;
        m_status = addon::Result_status::SUCCESS;
        m_payload = std::move(payload);
    }
    void fail(addon::Result_status failure_status, std::string failure_reason, std::string failure_message) override
    {
        ++m_terminal_count;
        m_status = failure_status;
        m_reason = std::move(failure_reason);
        m_message = std::move(failure_message);
    }

    int m_terminal_count = 0;
    addon::Result_status m_status = addon::Result_status::FAILED;
    std::string m_reason;
    std::string m_message;
    addon::Json_value m_payload;
    bool m_cancel = false;
    std::function<bool()> m_cancel_when;

private:
    addon::Invoke_request m_request;
    std::function<void(std::uint64_t)> m_progress;
};

void write_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "write isolated cache fixture");
}

class Native_usage_fixture
{
public:
    Native_usage_fixture()
    {
        const auto conversations = m_temporary.path() / ".gemini" / "antigravity-cli" / "conversations";
        fs::create_directories(conversations);
        m_database_path = conversations / "native.db";
        sqlite3* database = nullptr;
        const auto opened = sqlite3_open(m_database_path.string().c_str(), &database);
        m_database.reset(database);
        require(opened == SQLITE_OK, "open native temporal fixture");
        require(sqlite3_exec(m_database.get(),
            "CREATE TABLE trajectory_meta(trajectory_id TEXT, cascade_id TEXT);"
            "CREATE TABLE steps(idx INTEGER, step_type TEXT, metadata BLOB);"
            "INSERT INTO trajectory_meta VALUES('trajectory-temporal','cascade-temporal');",
            nullptr, nullptr, nullptr) == SQLITE_OK, "create native temporal schema");
        const auto profile = m_temporary.path() / "profile";
        m_candidate = profile / "candidate";
        fs::create_directories(m_candidate);
        m_request.context.launcher = {"agy", addon::k_runner_direct, {}};
        m_request.context.launcher_argv = {"agy"};
        m_request.context.account_context = addon::Account_context{1, "native-account-a", std::nullopt};
        m_request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(profile)};
        m_request.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
#ifdef _WIN32
        const char* home_name = "USERPROFILE";
#else
        const char* home_name = "HOME";
#endif
        m_request.context.launch_environment->effective_environment.set(home_name,
            addon::Json_value(addon::path_to_utf8(m_temporary.path())));
        m_request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(m_candidate)};
    }

    void add(std::uint64_t index, const std::string& metadata)
    {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(m_database.get(), "INSERT INTO steps VALUES(?,'PlannerResponse',?)", -1,
            &statement, nullptr) == SQLITE_OK, "prepare native temporal row");
        sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(index));
        sqlite3_bind_blob(statement, 2, metadata.data(), static_cast<int>(metadata.size()), SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        require(inserted, "insert native temporal row");
    }

    addon::Usage_scan_page scan()
    {
        return invoke(m_request);
    }

    void accept(const addon::Usage_scan_page& page, std::string mode = "incremental")
    {
        require(page.cache_candidate.has_value(), "accept a sealed provider snapshot");
        m_request.cache_selection->accepted = page.cache_candidate;
        m_candidate = m_candidate.parent_path() / ("candidate-" + std::to_string(++m_candidate_count));
        fs::create_directory(m_candidate);
        m_request.cache_selection->candidate_root = addon::path_to_utf8(m_candidate);
        m_request.mode = std::move(mode);
    }

    addon::Usage_scan_page scan_in_new_process()
    {
        return scan_in_new_process(m_request);
    }

    addon::Usage_scan_page scan_in_new_process(const addon::Usage_scan_request& request)
    {
        const auto request_path = m_candidate.parent_path() / "child-request.json";
        const auto result_path = m_candidate.parent_path() / "child-result.json";
        write_bytes(request_path, addon::serialize_json(addon::encode_usage_scan_request(request, 3)));
        const auto request_text = request_path.string();
        const auto result_text = result_path.string();
        const char* arguments[]{test_executable.c_str(), "--scan-request", request_text.c_str(),
            result_text.c_str(), nullptr};
#ifdef _WIN32
        require(_spawnv(_P_WAIT, test_executable.c_str(), arguments) == 0, "new process scans the accepted cache");
#else
        const auto process = fork();
        require(process >= 0, "fork native scanner fixture");
        if (!process) {
            execv(test_executable.c_str(), const_cast<char* const*>(arguments));
            _exit(127);
        }
        int status = 0;
        require(waitpid(process, &status, 0) == process && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "new process scans the accepted cache");
#endif
        const auto result = addon::parse_json(read_bytes(result_path), {}, nullptr);
        const auto page = result ? addon::decode_usage_scan_result(*result, nullptr, 3) : std::nullopt;
        require(page.has_value(), "new process returns a valid usage page");
        return *page;
    }

    void execute(const char* sql)
    {
        require(sqlite3_exec(m_database.get(), sql, nullptr, nullptr, nullptr) == SQLITE_OK,
            "update isolated native database fixture");
    }

    addon::Usage_scan_page continuation(const addon::Usage_scan_page& first, std::string cursor = "page:1")
    {
        auto request = m_request;
        request.snapshot_id = first.snapshot_id;
        request.cursor = std::move(cursor);
        return invoke(request);
    }

    void reseal(const addon::Usage_scan_page& first)
    {
        fs::remove(m_candidate / "cache-manifest.json");
        addon::Contract_failure failure;
        const auto reference = addon::seal_usage_cache_candidate(addon::path_to_utf8(m_candidate),
            m_request.context.profile_storage->root, m_request.context.account_context,
            first.snapshot_id, "agy-native-usage@1", [] { return false; }, &failure);
        require(reference.has_value(), "seal optional cache fixture with unchanged accounting: " + failure.detail);
    }

    void remove_native_source()
    {
        m_database.reset();
        fs::rename(m_database_path, m_database_path.string() + ".removed");
    }

    const fs::path& candidate() const { return m_candidate; }
    const fs::path& database_path() const { return m_database_path; }
    const addon::Usage_scan_request& request() const { return m_request; }

private:
    addon::Usage_scan_page invoke(const addon::Usage_scan_request& request)
    {
        Usage_invocation invocation(request);
        agy::handle_usage_scan(invocation);
        require(invocation.m_status == addon::Result_status::SUCCESS && invocation.m_terminal_count == 1,
            "actual provider invocation preserves accounting: " + invocation.m_reason);
        addon::Contract_failure failure;
        const auto page = addon::decode_usage_scan_result(invocation.m_payload, &failure, 3);
        require(page.has_value(), "actual page satisfies the SDK codec: " + failure.detail);
        require(addon::serialize_json(invocation.m_payload).size() <= addon::limits::k_max_exchange_payload_bytes,
            "actual provider page fits the existing exchange frame");
        return *page;
    }

    Temporary_directory m_temporary;
    fs::path m_database_path;
    fs::path m_candidate;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_database{nullptr, &sqlite3_close};
    addon::Usage_scan_request m_request;
    std::size_t m_candidate_count = 0;
};

void incremental_scan_reuses_persisted_snapshot_and_full_scan_reads_native_data()
{
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true, "retained-response"));
    const auto first = fixture.scan();
    fixture.accept(first);
    // An exclusive transaction leaves DB/WAL metadata unchanged but makes a
    // second SQLite reader fail, so success proves that the persisted cache is used.
    fixture.execute("BEGIN EXCLUSIVE");
    const auto repeated = fixture.scan_in_new_process();
    require(repeated.records.size() == 1 && repeated.records.front().total_tokens == 17 &&
            repeated.records.front().temporal && repeated.records.front().temporal->portions.size() == 1 &&
            repeated.records.front().temporal->parent.snapshot_id == repeated.snapshot_id &&
            repeated.records.front().temporal->portions.front().instant ==
                first.records.front().temporal->portions.front().instant,
        "incremental reuse survives process restart and reparents validated temporal evidence");
    fixture.accept(repeated, "repair");
    Usage_invocation full(fixture.request());
    agy::handle_usage_scan(full);
    require(full.m_status == addon::Result_status::FAILED && fs::is_empty(fixture.candidate()),
        "full mode reads the locked native database even when its metadata is unchanged");
    fixture.execute("ROLLBACK");
    const auto reread = fixture.scan();
    require(reread.records.size() == 1 && reread.records.front().total_tokens == 17,
        "full mode succeeds after the native reader is available");
    fixture.remove_native_source();
    fixture.accept(reread);
    const auto retained = fixture.scan();
    fixture.accept(retained, "repair");
    const auto full_retained = fixture.scan();
    require(retained.records.size() == 1 && full_retained.records.size() == 1 &&
            full_retained.records.front().temporal->portions.front().instant ==
                first.records.front().temporal->portions.front().instant,
        "incremental and full updates retain accepted accounting and instants after source deletion");
}

void incremental_scan_detects_wal_changes_and_old_dated_new_sources()
{
    Native_usage_fixture fixture;
    fixture.execute("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    fixture.add(1, metadata_with_usage(true, "first-response"));
    const auto first = fixture.scan();
    const auto database_size = fs::file_size(fixture.database_path());
    const auto database_time = fs::last_write_time(fixture.database_path());
    fixture.add(2, metadata_with_usage(true, "late-old-response", false, 1780000000));
    require(fs::file_size(fixture.database_path()) == database_size &&
            fs::last_write_time(fixture.database_path()) == database_time,
        "new native usage exists only in the WAL");
    fixture.accept(first);
    const auto appended = fixture.scan();
    require(appended.records.size() == 2 && appended.records[1].completed_date == "2026-05-28",
        "incremental scan includes appended WAL usage even when its event date is older");

    const auto copied = fixture.database_path().parent_path() / "imported.db";
    fixture.execute("PRAGMA wal_checkpoint(TRUNCATE)");
    fs::copy_file(fixture.database_path(), copied);
    sqlite3* imported = nullptr;
    require(sqlite3_open(copied.string().c_str(), &imported) == SQLITE_OK, "open imported native database");
    const auto changed = sqlite3_exec(imported,
        "UPDATE trajectory_meta SET cascade_id='imported-cascade';", nullptr, nullptr, nullptr);
    sqlite3_close(imported);
    require(changed == SQLITE_OK, "give imported usage its original independent session identity");
    require(!fs::exists(copied.string() + "-wal"),
        "closing the imported WAL-mode writer leaves no WAL frames on disk");
    fs::last_write_time(copied, database_time - std::chrono::hours(24 * 30));
    fixture.accept(appended);
    const auto imported_page = fixture.scan();
    require(imported_page.records.size() == 4,
        "a new file is read regardless of its older filesystem and usage timestamps");
}

void incremental_index_absence_and_unknown_version_rebuild_without_losing_history()
{
    for (const bool missing : {true, false}) {
        Native_usage_fixture fixture;
        fixture.add(1, metadata_with_usage(true));
        auto first = fixture.scan();
        const auto index = fixture.candidate() / "native-history-index.json";
        if (missing) fs::remove(index);
        else write_bytes(index, R"({"version":11})");
        fixture.reseal(first);
        first.cache_candidate->manifest_sha256_hex = sha256_file_bytes(fixture.candidate() / "cache-manifest.json");
        fixture.accept(first);
        fixture.execute("BEGIN EXCLUSIVE");
        Usage_invocation rebuilding(fixture.request());
        agy::handle_usage_scan(rebuilding);
        require(rebuilding.m_status == addon::Result_status::FAILED && fs::is_empty(fixture.candidate()),
            "missing or unknown optional metadata requires a native read before sealing");
        fixture.execute("ROLLBACK");
        const auto rebuilt = fixture.scan();
        require(rebuilt.records.size() == 1 && fs::exists(fixture.candidate() / "native-history-index.json"),
            "a successful native read rebuilds optional metadata without changing accounting");
    }
}

void incremental_scan_rejects_changed_reused_sources_and_duplicate_ownership()
{
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true, "first-response"));
    const auto first = fixture.scan();
    const auto accepted_manifest = sha256_file_bytes(fixture.candidate() / "cache-manifest.json");
    fixture.accept(first);
    Usage_invocation changing(fixture.request(), [&](std::uint64_t completed) {
        if (completed) fixture.add(2, metadata_with_usage(true, "appended-response"));
    });
    agy::handle_usage_scan(changing);
    require(changing.m_status == addon::Result_status::FAILED &&
            changing.m_reason == "native_history_changed_during_snapshot" && fs::is_empty(fixture.candidate()) &&
            sha256_file_bytes(addon::path_from_utf8(first.cache_candidate->root) / "cache-manifest.json") ==
                accepted_manifest,
        "a writer changing a reused database prevents sealing and leaves accepted evidence intact");
    const auto updated = fixture.scan();
    require(updated.records.size() == 2, "retry reads the changed database without advancing past new usage");
    fixture.accept(updated);
    fs::copy_file(fixture.database_path(), fixture.database_path().parent_path() / "duplicate.db");
    const auto copied_page = fixture.scan();
    require(copied_page.records.size() == 2, "copied databases count the same native receipts once");
    fixture.accept(copied_page);
    fixture.execute("DELETE FROM steps WHERE idx=2");
    fixture.add(2, metadata_with_usage(true, "appended-response", false, 1790084400, 123000000, 90, 7));
    Usage_invocation duplicate(fixture.request());
    agy::handle_usage_scan(duplicate);
    require(duplicate.m_status == addon::Result_status::FAILED &&
            duplicate.m_reason == "native_usage_identity_duplicate" && fs::is_empty(fixture.candidate()),
        "copies that disagree on the same response leave the accepted accounting intact");
}

void native_response_id_is_optional_and_can_arrive_later()
{
    // Exact completion timestamp and model_usage fields from a 1.5.11 loopback
    // step. The native stream independently reported input=10, output=4.
    const std::string projected = "\x42\x0c\x08\x91\x96\xf6\xd7\x06\x10\x8a\xa1\xce\xac\x01"
                                  "\x4a\x0b\x08\xc6\x02\x10\x0a\x18\x04\x30\x1e\x50\x04";
    Native_usage_fixture fixture;
    fixture.add(1, projected);
    const auto first = fixture.scan();
    require(first.records.size() == 1 && first.records[0].total_tokens == 14 &&
            first.records[0].input.value == 10 && first.records[0].output.value == 4 &&
            first.records[0].cache_read.coverage && first.records[0].cache_read.coverage->covered_records == 0 &&
            !first.records[0].model && first.records[0].temporal && first.records[0].temporal->portions.size() == 1 &&
            first.records[0].temporal->portions[0].instant == "2026-11-18T11:25:05.361992330Z",
        "a native response-less receipt preserves its observed dimensions and time");
    const auto stable_id = first.records[0].record_id;
    const auto copy = fixture.database_path().parent_path() / "copied.db";
    fs::copy_file(fixture.database_path(), copy);
    fixture.accept(first);
    fixture.execute("DELETE FROM steps WHERE idx=1");
    fixture.add(1, metadata_with_usage(true, "updated", true, 1795001105, 361992330, 10, 4));
    const auto mixed = fixture.scan();
    require(mixed.records.size() == 1 && mixed.records[0].record_id == stable_id &&
            mixed.records[0].total_tokens == 14 && mixed.records[0].cache_read.coverage->covered_records == 1,
        "compatible native copies with and without a response ID keep one accepted receipt");
    fs::remove(copy);
    fixture.accept(mixed, "repair");
    const auto repaired = fixture.scan();
    require(repaired.records.size() == 1 && repaired.records[0].record_id == stable_id,
        "repair retains the original key after the response ID arrives");
    fixture.accept(repaired);
    const auto repeated = fixture.scan_in_new_process();
    require(repeated.records.size() == 1 && repeated.records[0].record_id == stable_id,
        "incremental reread after restart retains a single accepted receipt");

    Native_usage_fixture legacy;
    legacy.add(1, metadata_with_usage(true, "historical", true, 1795001105, 361992330, 10, 4));
    const auto named = legacy.scan();
    legacy.accept(named);
    legacy.execute("DELETE FROM steps WHERE idx=1");
    legacy.add(1, projected);
    const auto restored = legacy.scan();
    require(restored.records.size() == 1 && restored.records[0].record_id == named.records[0].record_id,
        "a previously named receipt retains its historical key when replayed without a response ID");

    legacy.accept(restored);
    const auto raw_copy = legacy.database_path().parent_path() / "before-enrichment.db";
    fs::copy_file(legacy.database_path(), raw_copy);
    legacy.execute("DELETE FROM steps WHERE idx=1");
    legacy.add(1, metadata_with_usage(true, "different-response", true, 1795001105, 361992330, 10, 4));
    Usage_invocation ambiguous(legacy.request());
    agy::handle_usage_scan(ambiguous);
    require(ambiguous.m_status == addon::Result_status::FAILED &&
            ambiguous.m_reason == "native_usage_identity_ambiguous" && fs::is_empty(legacy.candidate()),
        "a response-less copy cannot be assigned to two distinct named receipts at the same cascade step");
    fs::remove(raw_copy);
    legacy.execute("DELETE FROM steps WHERE idx=1");
    legacy.add(1, projected);
    legacy.execute("DELETE FROM trajectory_meta");
    Usage_invocation missing_cascade(legacy.request());
    agy::handle_usage_scan(missing_cascade);
    require(missing_cascade.m_status == addon::Result_status::FAILED &&
            missing_cascade.m_reason == "native_usage_metadata_invalid",
        "a filename cannot establish the missing native cascade of a response-less step");
}

void native_instants_preserve_supported_epoch_nanoseconds()
{
    struct Native_time_case
    {
        std::int64_t seconds;
        std::optional<std::uint64_t> nanos;
        std::string expected;
    };
    const std::vector<Native_time_case> cases{
        {-62135596800ll, std::nullopt, "0001-01-01T00:00:00Z"},
        {-1, 999999999, "1969-12-31T23:59:59.999999999Z"},
        {0, 0, "1970-01-01T00:00:00.000000000Z"},
        {1791503999, 999999999, "2026-10-08T23:59:59.999999999Z"},
        {1791504000, 1, "2026-10-09T00:00:00.000000001Z"},
        {1791590400, 765432109, "2026-10-10T00:00:00.765432109Z"},
        {253402300799ll, 999999999, "9999-12-31T23:59:59.999999999Z"},
    };
    for (const auto& test : cases) {
        const auto decoded = agy::decode_native_usage_step(metadata_with_usage(true, "timed-response", false,
            test.seconds, test.nanos), "cascade-time", 9);
        require(decoded.record && decoded.instant == test.expected && decoded.record->total_tokens == 17 &&
                decoded.record->completed_date == test.expected.substr(0, 10) && !decoded.record->temporal,
            "native epoch and nanoseconds survive before snapshot binding: " + test.expected);
    }
}

void native_temporal_pages_and_cache_keep_accounting_context()
{
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true, "day-one", false, 1791503999, 999999999));
    fixture.add(2, metadata_with_usage(true, "day-two", false, 1791504000, 1));
    fixture.add(3, metadata_with_usage(true, "zero-day-three", false, 1791590400, 765432109, 0, 0));
    const auto first = fixture.scan();
    require(first.records.size() == 3 && first.complete && first.cache_candidate &&
            first.scope.source_account_key == std::optional<std::string>("native-account-a"),
        "native scan echoes the selected source account and sealed snapshot");
    require(first.records[0].temporal && first.records[1].temporal &&
            first.records[0].temporal->portions.front().instant == "2026-10-08T23:59:59.999999999Z" &&
            first.records[1].temporal->portions.front().instant == "2026-10-09T00:00:00.000000001Z",
        "midnight portions stay linked to their individual native parents");
    std::string accounting;
    std::uint64_t total = 0;
    for (auto record : first.records) {
        require(record.temporal && record.temporal->coverage_reason == "complete" &&
                record.temporal->unavailable_tokens == 0 &&
                record.temporal->parent.record_id == record.record_id &&
                record.temporal->parent.total_tokens == record.total_tokens &&
                record.temporal->parent.snapshot_id == first.snapshot_id &&
                record.temporal->parent.source_account_key == first.scope.source_account_key &&
                record.semantics == addon::Usage_counter_semantics::DELTA_EVENT,
            "each portion binds to its exact native accounting parent and source context");
        require(record.total_tokens ? record.temporal->portions.size() == 1 &&
                record.temporal->portions.front().total_tokens == record.total_tokens
            : record.temporal->portions.empty(), "zero-token parents create no positive portions");
        total += record.total_tokens;
        record.temporal.reset();
        record.capabilities.erase(addon::k_usage_temporal_capability_id);
        accounting += addon::serialize_json_canonical(addon::encode_usage_record(record, 3)) + '\n';
    }
    require(total == 34 && read_bytes(fixture.candidate() / "usage-records.jsonl") == accounting,
        "optional temporal evidence adds no accounting bytes, totals or duplicate rows");
    const auto sidecar = read_bytes(fixture.candidate() / "usage-temporal.jsonl");
    addon::Contract_failure failure;
    const auto manifest = addon::validate_usage_cache_candidate(*first.cache_candidate,
        fixture.request().context.profile_storage->root, fixture.request().context.account_context,
        [] { return false; }, &failure);
    require(manifest.has_value(), "validate sealed native cache: " + failure.reason_code + ": " + failure.detail);
    require(std::any_of(manifest->files.begin(), manifest->files.end(), [](const auto& file) {
        return file.path == "usage-temporal.jsonl";
    }), "the existing cache manifest seals optional evidence alongside accounting");
    fixture.remove_native_source();
    const auto continued = fixture.continuation(first);
    require(continued.records.size() == 2 && continued.snapshot_id == first.snapshot_id &&
            addon::serialize_json_canonical(addon::encode_usage_record(continued.records.front(), 3)) ==
                addon::serialize_json_canonical(addon::encode_usage_record(first.records[1], 3)) &&
            read_bytes(fixture.candidate() / "usage-temporal.jsonl") == sidecar,
        "sealed continuation retains exact evidence after the original source disappears");
}

std::vector<addon::Json_value> cache_rows(std::string_view bytes)
{
    std::istringstream input{std::string(bytes)};
    std::vector<addon::Json_value> rows;
    for (std::string line; std::getline(input, line);) {
        const auto row = addon::parse_json(line, {}, nullptr);
        require(row.has_value(), "parse generated isolated cache row");
        rows.push_back(*row);
    }
    return rows;
}

std::string cache_bytes(const std::vector<addon::Json_value>& rows)
{
    std::string bytes;
    for (const auto& row : rows) bytes += addon::serialize_json_canonical(row) + '\n';
    return bytes;
}

void optional_sidecar_damage_never_rejects_valid_accounting()
{
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true, "first-response"));
    fixture.add(2, metadata_with_usage(true, "second-response"));
    fixture.add(3, metadata_with_usage(true, "zero-response", false, 1791590400, 0, 0, 0));
    const auto first = fixture.scan();
    const auto path = fixture.candidate() / "usage-temporal.jsonl";
    const auto original = cache_rows(read_bytes(path));
    const auto accounting_path = fixture.candidate() / "usage-records.jsonl";
    const auto accounting = read_bytes(accounting_path);
    struct Sidecar_case
    {
        std::string name;
        std::function<void(std::vector<addon::Json_value>&)> damage;
    };
    const std::vector<Sidecar_case> cases{
        {"future version", [](auto& rows) { rows[0].set("version", addon::Json_value(11)); }},
        {"wrong snapshot", [](auto& rows) { rows[0].set("snapshot_id", addon::Json_value("other-snapshot")); }},
        {"wrong source account", [](auto& rows) { rows[0].set("source_account_key", addon::Json_value("other-account")); }},
        {"wrong count", [](auto& rows) { rows[0].set("record_count", addon::Json_value(2)); }},
        {"wrong parent identity", [](auto& rows) { rows[2].set("record_id", addon::Json_value("different-record")); }},
        {"wrong token amount", [](auto& rows) { rows[2].set("total_tokens", addon::Json_value(18)); }},
        {"wrong parent digest", [](auto& rows) { rows[2].set("parent_sha256_hex", addon::Json_value(std::string(64, '0'))); }},
        {"offsetless timestamp", [](auto& rows) { rows[2].set("instant", addon::Json_value("2026-10-08T21:00:00")); }},
        {"invalid zero-token instant", [](auto& rows) { rows[3].set("instant", addon::Json_value("missing-time")); }},
        {"missing row", [](auto& rows) { rows.pop_back(); }},
        {"duplicate row", [](auto& rows) { rows.push_back(rows.back()); }},
        {"mixed ordered identity", [](auto& rows) { std::swap(rows[1], rows[2]); }},
    };
    for (const auto& test : cases) {
        auto rows = original;
        test.damage(rows);
        const auto damaged = cache_bytes(rows);
        write_bytes(path, damaged);
        fixture.reseal(first);
        const auto page = fixture.continuation(first);
        require(page.records.size() == 2 && read_bytes(accounting_path) == accounting && read_bytes(path) == damaged,
            "optional sidecar remains byte-exact and accounting succeeds: " + test.name);
        for (const auto& record : page.records) {
            require(record.temporal && record.temporal->portions.empty() &&
                    record.temporal->unavailable_tokens == record.total_tokens &&
                    record.temporal->coverage_reason == "unsupported",
                "damaged optional group cannot manufacture any portion: " + test.name);
        }
    }
    write_bytes(path, cache_bytes(original));
    auto corrected = cache_rows(accounting);
    corrected[1].set("model", addon::Json_value("corrected-native-model"));
    const auto corrected_accounting = cache_bytes(corrected);
    write_bytes(accounting_path, corrected_accounting);
    fixture.reseal(first);
    const auto corrected_page = fixture.continuation(first);
    require(corrected_page.records.front().record_id == first.records[1].record_id &&
            corrected_page.records.front().total_tokens == first.records[1].total_tokens &&
            corrected_page.records.front().model == std::optional<std::string>("corrected-native-model") &&
            corrected_page.records.front().temporal->portions.empty() &&
            read_bytes(accounting_path) == corrected_accounting,
        "equal identity and amount never graft evidence across a full accounting correction");
    auto future = addon::Json_value::object();
    future.set("version", addon::Json_value(23));
    future.set("future_payload", addon::Json_value("opaque temporal data"));
    auto capabilities = addon::Json_value::object();
    capabilities.set(addon::k_usage_temporal_capability_id, future);
    corrected[1].set("capabilities", capabilities);
    write_bytes(accounting_path, cache_bytes(corrected));
    fixture.reseal(first);
    const auto future_page = fixture.continuation(first);
    require(!future_page.records.front().temporal &&
            addon::serialize_json_canonical(*future_page.records.front().capabilities.find(addon::k_usage_temporal_capability_id)) ==
                addon::serialize_json_canonical(future), "future inline envelopes survive provider cache replay opaquely");
    write_bytes(accounting_path, accounting);
    write_bytes(path, addon::serialize_json_canonical(original.front()) + '\n' +
        std::string(addon::k_max_usage_temporal_bytes + 1, 'x') + '\n');
    fixture.reseal(first);
    const auto oversized = fixture.continuation(first);
    require(oversized.records.front().temporal->portions.empty() &&
            oversized.records.front().temporal->unavailable_tokens == oversized.records.front().total_tokens,
        "optional row capacity cannot narrow the accounting cache contract");
    fs::remove(path);
    fixture.reseal(first);
    const auto legacy = fixture.continuation(first);
    require(legacy.records.front().temporal->coverage_reason == "missing-evidence" &&
            legacy.records.front().temporal->unavailable_tokens == legacy.records.front().total_tokens,
        "date-only caches remain accounting-valid with an explicit unavailable temporal remainder");
}

void opaque_sidecar_bytes_have_no_new_total_cache_ceiling()
{
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true, "first-response"));
    fixture.add(2, metadata_with_usage(true, "second-response"));
    const auto first = fixture.scan();
    const auto path = fixture.candidate() / "usage-temporal.jsonl";
    auto header = cache_rows(read_bytes(path)).front();
    header.set("version", addon::Json_value(11));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << addon::serialize_json_canonical(header) << '\n';
    std::array<char, 65536> buffer{};
    buffer.fill(static_cast<char>(0xa7));
    for (int index = 0; index < 1025; ++index) output.write(buffer.data(), buffer.size());
    output.close();
    require(static_cast<bool>(output) && fs::file_size(path) > 64u * 1024u * 1024u,
        "future optional artifact exceeds the separate existing accounting JSONL ceiling");
    const auto digest = sha256_file_bytes(path);
    fixture.reseal(first);
    const auto page = fixture.continuation(first);
    require(page.records.size() == 1 && page.records.front().total_tokens == 17 &&
            page.records.front().temporal->coverage_reason == "unsupported" && sha256_file_bytes(path) == digest,
        "large future optional bytes remain sealed and untouched while accounting is usable");
}

void native_admission_and_cancellation_still_precede_sealing()
{
    Native_usage_fixture duplicate;
    const auto metadata = metadata_with_usage(true, "same-response");
    duplicate.add(1, metadata);
    duplicate.add(1, metadata);
    Usage_invocation duplicated(duplicate.request());
    agy::handle_usage_scan(duplicated);
    require(duplicated.m_status == addon::Result_status::FAILED &&
            duplicated.m_reason == "native_usage_identity_duplicate" && fs::is_empty(duplicate.candidate()),
        "duplicate native ownership refuses before accounting or temporal sealing");
    Native_usage_fixture fixture;
    fixture.add(1, metadata_with_usage(true));
    Usage_invocation cancelled(fixture.request(), [&cancelled](std::uint64_t completed) {
        if (completed) cancelled.m_cancel = true;
    });
    agy::handle_usage_scan(cancelled);
    require(cancelled.m_status == addon::Result_status::CANCELLED && fs::is_empty(fixture.candidate()),
        "cancellation after native reading cannot publish a partial sealed candidate");
}

void cancellation_during_cache_writes_removes_partial_records()
{
    Native_usage_fixture fixture;
    fixture.execute("BEGIN");
    for (std::uint64_t index = 0; index < 300; ++index) {
        fixture.add(index, metadata_with_usage(true, "response-" + std::to_string(index)));
    }
    fixture.execute("COMMIT");
    const auto accounting_path = fixture.candidate() / "usage-records.jsonl";
    Usage_invocation invocation(fixture.request());
    invocation.m_cancel_when = [&] {
        std::error_code error;
        const auto size = fs::file_size(accounting_path, error);
        return !error && size != 0;
    };
    agy::handle_usage_scan(invocation);
    require(invocation.m_status == addon::Result_status::CANCELLED && invocation.m_terminal_count == 1,
        "cancellation during cache output returns one cancelled result");
    require(fs::is_empty(fixture.candidate()),
        "cancelled record output closes and removes its partial file before sealing");
}

void actual_provider_pages_pack_counts_and_bytes_without_accounting_loss()
{
    Native_usage_fixture fixture;
    for (std::uint64_t index = 0; index < 300; ++index) {
        fixture.add(index, metadata_with_usage(true, "response-" + std::to_string(index)));
    }
    const auto first = fixture.scan();
    require(first.records.size() == addon::k_max_usage_page_records && !first.complete && first.next_cursor == "page:256",
        "native count paging retains the existing 256-record boundary");
    fixture.remove_native_source();
    const auto last = fixture.continuation(first, *first.next_cursor);
    require(last.records.size() == 44 && last.complete && first.records.back().record_id < last.records.front().record_id,
        "continuation returns every remaining native parent in canonical order");
    for (const auto& record : last.records) {
        require(record.temporal && record.temporal->parent.snapshot_id == first.snapshot_id &&
                record.temporal->portions.front().total_tokens == 17,
            "temporal evidence survives actual sealed native continuation pages");
    }
    const auto accounting_path = fixture.candidate() / "usage-records.jsonl";
    auto rows = cache_rows(read_bytes(accounting_path));
    auto padding = addon::Json_value::object();
    padding.set("version", addon::Json_value(7));
    padding.set("padding", addon::Json_value(std::string(20 * 1024, 'p')));
    for (auto& row : rows) {
        auto capabilities = addon::Json_value::object();
        capabilities.set("future-native-detail", padding);
        row.set("capabilities", capabilities);
    }
    write_bytes(accounting_path, cache_bytes(rows));
    fixture.reseal(first);
    const auto packed = fixture.continuation(first, "page:0");
    require(!packed.records.empty() && packed.records.size() < addon::k_max_usage_page_records && !packed.complete &&
            packed.next_cursor == "page:" + std::to_string(packed.records.size()),
        "encoded bytes split a page before the count boundary without consuming its next record");
    const auto packed_last = fixture.continuation(first, *packed.next_cursor);
    std::uint64_t total = 0;
    for (const auto* page : {&packed, &packed_last}) {
        for (const auto& record : page->records) total += record.total_tokens;
    }
    require(packed.records.size() + packed_last.records.size() == 300 && packed_last.complete && total == 5100 &&
            addon::serialize_json_canonical(*packed_last.records.front().capabilities.find("future-native-detail")) ==
                addon::serialize_json_canonical(padding), "byte paging conserves all accounting and future capability bytes");

    auto record = first.records.front();
    record.temporal.reset();
    record.capabilities.erase(addon::k_usage_temporal_capability_id);
    auto blocks = addon::Json_value::array();
    for (int index = 0; index < 7; ++index) blocks.push_back(addon::Json_value(std::string(512 * 1024, 'b')));
    blocks.push_back(addon::Json_value(""));
    auto large = addon::Json_value::object();
    large.set("version", addon::Json_value(9));
    large.set("blocks", blocks);
    record.capabilities.set("future-native-detail", large);
    auto singleton = first;
    singleton.records = {record};
    singleton.complete = true;
    singleton.next_cursor.reset();
    const auto initial_bytes = addon::serialize_json(addon::encode_usage_scan_result(singleton, 3)).size();
    blocks.as_array().back() = addon::Json_value(std::string(
        addon::limits::k_max_exchange_payload_bytes - initial_bytes - 10, 'b'));
    large.set("blocks", blocks);
    record.capabilities.set("future-native-detail", large);
    singleton.records = {record};
    require(addon::serialize_json(addon::encode_usage_scan_result(singleton, 3)).size() ==
            addon::limits::k_max_exchange_payload_bytes - 10,
        "singleton accounting fixture approaches the existing actual frame bound");
    write_bytes(accounting_path, addon::serialize_json_canonical(addon::encode_usage_record(record, 3)) + '\n');
    const auto state_path = fixture.candidate() / "scan-state.json";
    auto state = addon::parse_json(read_bytes(state_path), {}, nullptr);
    require(state.has_value(), "read accounting scan state");
    state->set("record_count", addon::Json_value(1));
    write_bytes(state_path, addon::serialize_json_canonical(*state));
    fixture.reseal(first);
    const auto near_frame = fixture.continuation(first, "page:0");
    require(near_frame.records.size() == 1 && near_frame.complete && near_frame.records.front().total_tokens == 17 &&
            !near_frame.records.front().temporal &&
            addon::serialize_json_canonical(*near_frame.records.front().capabilities.find("future-native-detail")) ==
                addon::serialize_json_canonical(large), "optional evidence cannot refuse reliable near-frame accounting");
}

void native_scan_uses_private_read_only_snapshot()
{
    Temporary_directory temporary;
    const auto conversations = temporary.path() / ".gemini" / "antigravity-cli" / "conversations";
    fs::create_directories(conversations);
    const auto database_path = conversations / "native.db";
    sqlite3* database = nullptr;
    require(sqlite3_open(database_path.string().c_str(), &database) == SQLITE_OK,
        "open synthetic native database");
    require(sqlite3_exec(database, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;",
                nullptr, nullptr, nullptr) == SQLITE_OK,
        "enable persistent WAL mode without automatic checkpointing");
    const char* schema =
        "CREATE TABLE trajectory_meta(trajectory_id TEXT, cascade_id TEXT);"
        "CREATE TABLE steps(idx INTEGER, step_type TEXT, metadata BLOB);"
        "INSERT INTO trajectory_meta VALUES('trajectory-1','cascade-fixture');";
    require(sqlite3_exec(database, schema, nullptr, nullptr, nullptr) == SQLITE_OK,
        "create native fixture schema");
    const auto main_before_steps = read_bytes(database_path);
    const auto wal_path = fs::path(database_path.string() + "-wal");
    require(fs::is_regular_file(wal_path), "writer creates a WAL sidecar");
    const auto wal_size_before_steps = fs::file_size(wal_path);
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, "INSERT INTO steps VALUES(?,?,?)", -1, &statement, nullptr) == SQLITE_OK,
        "prepare native fixture row");
    const auto insert_step = [&statement](sqlite3_int64 index, const char* type, const std::string& metadata) {
        sqlite3_bind_int64(statement, 1, index);
        sqlite3_bind_text(statement, 2, type, -1, SQLITE_STATIC);
        sqlite3_bind_blob(statement, 3, metadata.data(), static_cast<int>(metadata.size()), SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        return inserted;
    };
    const auto planner_metadata = metadata_with_usage(true, "planner-response");
    const auto checkpoint_metadata = metadata_with_usage(true, "checkpoint-response");
    const auto numeric_model_metadata = metadata_with_usage(true, "numeric-model-response", true);
    require(insert_step(27, "PlannerResponse", planner_metadata), "insert Planner response usage event");
    require(insert_step(28, "Checkpoint", checkpoint_metadata), "insert Checkpoint usage event");
    require(insert_step(29, "Checkpoint", metadata_without_usage()), "insert Checkpoint without usage");
    require(insert_step(30, "Checkpoint", numeric_model_metadata),
        "insert native usage event with numeric model identifier");
    sqlite3_finalize(statement);
    require(read_bytes(database_path) == main_before_steps && fs::file_size(wal_path) > wal_size_before_steps,
        "step rows are appended to WAL while the main database remains unchanged");
    const std::vector<fs::path> durable_native_files{database_path, wal_path};
    auto shm_path = database_path;
    shm_path += "-shm";
    require(fs::is_regular_file(shm_path), "live WAL writer has a shared-memory coordination sidecar");
    std::vector<std::string> source_before;
    std::vector<std::string> source_hashes_before;
    for (const auto& native_file : durable_native_files) {
        require(fs::is_regular_file(native_file), "live WAL source has main and WAL files");
        source_before.push_back(read_bytes(native_file));
        source_hashes_before.push_back(sha256_file_bytes(native_file));
    }
    require(source_before[1].size() > 32, "usage rows remain in the uncheckpointed WAL sidecar");

    std::string error;
    const auto records = agy::scan_native_usage_directory(conversations, [] { return false; }, {}, &error);
    require(records && records->size() == 3,
        "scan Planner and Checkpoint events with text and numeric model identifiers: " + error);
    const auto has_record = [&records](std::string_view id) {
        return std::any_of(records->begin(), records->end(), [id](const addon::Usage_record& record) {
            return record.record_id == id;
        });
    };
    require(has_record("agy:cascade-fixture:step:27:response:planner-response") &&
            has_record("agy:cascade-fixture:step:28:response:checkpoint-response") &&
            has_record("agy:cascade-fixture:step:30:response:numeric-model-response"),
        "snapshot scan preserves Planner and Checkpoint native event identities");
    const auto numeric_record = std::find_if(records->begin(), records->end(), [](const addon::Usage_record& record) {
        return record.record_id == "agy:cascade-fixture:step:30:response:numeric-model-response";
    });
    require(numeric_record != records->end() && !numeric_record->model && numeric_record->total_tokens == 17,
        "numeric model identifier preserves usage without inventing a model name");
    for (std::size_t index = 0; index < durable_native_files.size(); ++index) {
        const auto after = read_bytes(durable_native_files[index]);
        if (after != source_before[index] ||
            sha256_file_bytes(durable_native_files[index]) != source_hashes_before[index]) {
            std::size_t offset = 0;
            while (offset < after.size() && offset < source_before[index].size() &&
                   after[offset] == source_before[index][offset]) ++offset;
            throw std::runtime_error("durable native source bytes changed: " + durable_native_files[index].string() +
                " (before=" + std::to_string(source_before[index].size()) +
                ", after=" + std::to_string(after.size()) + ", first_offset=" + std::to_string(offset) + ")");
        }
    }

    const auto cancelled = agy::scan_native_usage_directory(conversations, [] { return true; }, {}, &error);
    require(!cancelled && error == "cancelled", "cancelled scan fails without returning partial records");
    for (std::size_t index = 0; index < durable_native_files.size(); ++index) {
        require(read_bytes(durable_native_files[index]) == source_before[index] &&
                sha256_file_bytes(durable_native_files[index]) == source_hashes_before[index],
            "cancelled scan leaves durable native source byte-for-byte unchanged: " +
                durable_native_files[index].string());
    }
    addon::Usage_scan_request request;
    request.context.launcher = {"agy", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"agy"};
    const auto profile = temporary.path() / "profile";
    const auto candidate = profile / "candidate";
    fs::create_directories(candidate);
    request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(profile)};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
#ifdef _WIN32
    const char* home_name = "USERPROFILE";
#else
    const char* home_name = "HOME";
#endif
    request.context.launch_environment->effective_environment.set(home_name,
        addon::Json_value(addon::path_to_utf8(temporary.path())));
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(candidate)};
    bool changed = false;
    Usage_invocation changing_history(request, [&](std::uint64_t completed) {
        if (completed == 0 || changed) {
            return;
        }
        require(sqlite3_exec(database, "UPDATE trajectory_meta SET trajectory_id='trajectory-new';",
            nullptr, nullptr, nullptr) == SQLITE_OK, "native activity changes the source after it was read");
        changed = true;
    });
    agy::handle_usage_scan(changing_history);
    require(changed && changing_history.m_terminal_count == 1 &&
            changing_history.m_status == addon::Result_status::FAILED &&
            changing_history.m_reason == "native_history_changed_during_snapshot",
        "native activity refuses the scan with the unchanged freshness reason");
    require(changing_history.m_message ==
            "Antigravity history changed during the scan. Wait for the current activity to finish.",
        "freshness refusal explains the activity and the useful retry condition");
    require(fs::is_empty(candidate), "refused scan leaves the unaccepted candidate empty");
    Usage_invocation stable_history(request);
    agy::handle_usage_scan(stable_history);
    require(stable_history.m_terminal_count == 1 && stable_history.m_status == addon::Result_status::SUCCESS,
        "the same request can succeed after native history stops changing: " + stable_history.m_reason);
    addon::Contract_failure failure;
    const auto page = addon::decode_usage_scan_result(stable_history.m_payload, &failure, 3);
    require(page && page->records.size() == 3,
        "native response page preserves all admitted accounting records: " + failure.detail);
    for (const auto& record : page->records) {
        require(record.temporal && record.temporal->portions.size() == 1 &&
                record.temporal->portions.front().instant == "2026-09-22T13:40:00.123000000Z" &&
                record.temporal->portions.front().total_tokens == record.total_tokens &&
                record.temporal->parent.record_id == record.record_id &&
                record.temporal->parent.total_tokens == record.total_tokens &&
                record.temporal->parent.snapshot_id == page->snapshot_id &&
                record.temporal->parent.source_account_key == page->scope.source_account_key,
            "native response retains its proved nanosecond instant bound to the accounting page");
    }
    sqlite3_close(database);
}

void native_record_uses_stable_delta_identity_and_exact_dimensions()
{
    const auto decoded = agy::decode_native_usage_step(
        metadata_with_usage(true), "cascade-01", 27);
    require(decoded.status == agy::Native_usage_decode_status::DECODED && decoded.record,
        "native usage metadata decodes");
    const auto& record = *decoded.record;
    require(record.record_id == "agy:cascade-01:step:27:response:native-response-01",
        "record identity includes cascade, step, and response id");
    require(record.session_key == "agy:cascade-01", "session identity uses the native cascade id");
    require(record.semantics == addon::Usage_counter_semantics::DELTA_EVENT,
        "native usage counters remain per-event deltas");
    require(record.total_tokens == 17 && record.input.value == 10 && record.output.value == 7,
        "total uses native input plus output without adding thinking twice");
    require(record.cache_read.applicable && record.cache_read.value == 0,
        "an explicitly present zero cache-read counter remains known");
    require(record.cache_creation.applicable && record.cache_creation.value == 0 && record.cache_creation.coverage &&
            record.cache_creation.coverage->support == addon::Usage_support::UNKNOWN &&
            record.cache_creation.coverage->covered_records == 0 &&
            record.cache_creation.coverage->total_records == 1,
        "cache-creation reports no observed subtotal and unknown coverage");
    require(record.completed_date == "2026-09-22", "native completion timestamp becomes a UTC date");

}

void continuations_preserve_snapshot_identity_integrity_and_cancellation()
{
    Native_usage_fixture fixture;
    fixture.execute("BEGIN TRANSACTION");
    for (std::uint64_t index = 0; index < 300; ++index) {
        fixture.add(index, metadata_with_usage(true, "continuation-" + std::to_string(index)));
    }
    fixture.execute("COMMIT");
    const auto first = fixture.scan();
    require(first.next_cursor.has_value(), "identity fixture requires a continuation");
    fixture.remove_native_source();
    auto request = fixture.request();
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    const auto expected = fixture.continuation(first, *first.next_cursor);
    const auto expected_bytes = addon::serialize_json_canonical(addon::encode_usage_scan_result(expected, 3));
    const auto require_failure = [](const addon::Usage_scan_request& changed, std::string_view reason) {
        Usage_invocation invocation(changed);
        agy::handle_usage_scan(invocation);
        require(invocation.m_status == addon::Result_status::FAILED && invocation.m_terminal_count == 1 &&
                invocation.m_reason == reason,
            "continuation rejects changed identity or corrupted bytes: " + invocation.m_reason);
    };

    auto changed = request;
    changed.context.account_context->adapter_account_key = "different-account";
    require_failure(changed, "usage_cache_candidate_invalid");
    changed = request;
    changed.context.launcher_argv.push_back("--different-context");
    require_failure(changed, "usage_cache_request_mismatch");
    changed = request;
    changed.snapshot_id = "different-snapshot";
    require_failure(changed, "usage_cache_candidate_invalid");
    changed = request;
    changed.cursor = "page:300";
    require_failure(changed, "usage_scan_cursor_invalid");

    Usage_invocation cancelled(request);
    std::size_t cancellation_checks = 0;
    cancelled.m_cancel_when = [&cancellation_checks] { return ++cancellation_checks >= 2; };
    agy::handle_usage_scan(cancelled);
    require(cancelled.m_status == addon::Result_status::CANCELLED && cancelled.m_terminal_count == 1 &&
            cancelled.m_reason == "cancelled",
        "continuation cancellation completes once even after an earlier page populated the snapshot");

    for (const std::string_view name : {"usage-records.jsonl", "usage-temporal.jsonl",
             "native-history-index.json", "scan-state.json", "cache-manifest.json"})
    {
        const auto path = fixture.candidate() / name;
        const auto original = read_bytes(path);
        const auto timestamp = fs::last_write_time(path);
        require(!original.empty(), "sealed fixture artifact is nonempty");
        auto damaged = original;
        damaged.front() = damaged.front() == '{' ? '[' : '{';
        write_bytes(path, damaged);
        fs::last_write_time(path, timestamp);
        require_failure(request, "usage_cache_candidate_invalid");
        write_bytes(path, original);
        fs::last_write_time(path, timestamp);
        const auto restored = fixture.continuation(first, *first.next_cursor);
        require(addon::serialize_json_canonical(addon::encode_usage_scan_result(restored, 3)) == expected_bytes,
            "restored bytes replay exactly, including optional proof and snapshot metadata");
    }
    const auto unexpected = fixture.candidate() / "unsealed-artifact.json";
    write_bytes(unexpected, "{}");
    require_failure(request, "usage_cache_candidate_invalid");
    fs::remove(unexpected);

    // A changed manifest must not reuse an earlier valid decode, even when the
    // malformed accounting row is beyond the page the caller is requesting.
    const auto records_path = fixture.candidate() / "usage-records.jsonl";
    auto records = read_bytes(records_path);
    const auto last_row = records.rfind('\n', records.size() - 2) + 1;
    records[last_row] = '[';
    write_bytes(records_path, records);
    fixture.reseal(first);
    changed = request;
    changed.cursor = "page:0";
    require_failure(changed, "usage_cache_contents_invalid");
}

void continuations_replay_after_other_snapshots_and_process_restart()
{
    Native_usage_fixture fixture;
    fixture.execute("BEGIN TRANSACTION");
    for (std::uint64_t index = 0; index < 300; ++index) {
        fixture.add(index, metadata_with_usage(true, "restart-" + std::to_string(index)));
    }
    fixture.execute("COMMIT");
    const auto first = fixture.scan();
    require(first.next_cursor.has_value(), "restart fixture requires a continuation");
    fixture.remove_native_source();
    const auto expected = fixture.continuation(first, *first.next_cursor);
    const auto expected_bytes = addon::serialize_json_canonical(addon::encode_usage_scan_result(expected, 3));

    Native_usage_fixture other;
    other.add(1, metadata_with_usage(true, "different-snapshot", false, 1790084400, 123000000, 40, 2));
    const auto other_first = other.scan();
    require(other.continuation(other_first, "page:0").records.front().total_tokens == 42,
        "interleaved replay has distinct accounting");
    const auto replay = fixture.continuation(first, *first.next_cursor);
    require(addon::serialize_json_canonical(addon::encode_usage_scan_result(replay, 3)) == expected_bytes,
        "interleaved snapshots cannot change the continuation's accounting or proof");

    auto request = fixture.request();
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    const auto restarted = fixture.scan_in_new_process(request);
    require(addon::serialize_json_canonical(addon::encode_usage_scan_result(restarted, 3)) == expected_bytes,
        "a new adapter process reconstructs the same page solely from the sealed candidate");
}

void native_scan_record_is_accepted_by_usage_scan_v3_codec()
{
    const auto decoded = agy::decode_native_usage_step(
        metadata_with_usage(true, "native-response-01", true), "cascade-01", 27);
    addon::Contract_failure failure;
    const auto encoded = addon::encode_usage_record(*decoded.record, 3);
    const auto round_trip = addon::decode_usage_record(encoded, &failure, 3);
    require(round_trip && round_trip->record_id == decoded.record->record_id,
        "record satisfies the host usage-scan@3 record contract: " + failure.reason_code + " " + failure.detail);
}

void missing_usage_and_missing_cache_read_preserve_unknown_coverage()
{
    std::string timestamp;
    append_varint_field(timestamp, 1, 1790084400);
    std::string metadata;
    append_bytes_field(metadata, 8, timestamp);
    const auto absent = agy::decode_native_usage_step(metadata, "cascade-02", 1);
    require(absent.status == agy::Native_usage_decode_status::NO_USAGE && !absent.record,
        "a step without native usage emits no record");

    const auto decoded = agy::decode_native_usage_step(
        metadata_with_usage(false), "cascade-02", 2);
    require(decoded.status == agy::Native_usage_decode_status::DECODED && decoded.record,
        "usage event without cache-read remains decodable");
    require(decoded.record->cache_read.applicable && decoded.record->cache_read.value == 0 &&
            decoded.record->cache_read.coverage &&
            decoded.record->cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            decoded.record->cache_read.coverage->covered_records == 0 &&
            decoded.record->cache_read.coverage->total_records == 1,
        "an absent cache-read counter has no observed subtotal and remains unknown");
}

void malformed_or_unidentifiable_usage_is_rejected()
{
    auto metadata = metadata_with_usage(true);
    append_bytes_field(metadata, 9, "\x01");
    const auto malformed = agy::decode_native_usage_step(metadata, "cascade-03", 4);
    require(malformed.status == agy::Native_usage_decode_status::INVALID,
        "malformed or duplicate native usage metadata cannot be silently dropped");

    const auto missing_identity = agy::decode_native_usage_step(
        metadata_with_usage(true), "cascade-03", 5);
    require(missing_identity.record && missing_identity.record->record_id.find("response:native-response-01") !=
            std::string::npos,
        "stable native response identity is retained in the event key");
    const auto no_cascade = agy::decode_native_usage_step(
        metadata_with_usage(true), "", 5);
    require(no_cascade.status == agy::Native_usage_decode_status::INVALID,
        "usage without a native session identity is refused");
    const auto empty_response = agy::decode_native_usage_step(metadata_with_usage(true, ""), "cascade-03", 5);
    require(empty_response.status == agy::Native_usage_decode_status::INVALID,
        "an explicitly malformed response ID cannot fall back to the SQLite step key");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        test_executable = fs::absolute(argv[0]).string();
        if (argc == 4 && std::string_view(argv[1]) == "--scan-request") {
            const auto payload = addon::parse_json(read_bytes(argv[2]), {}, nullptr);
            const auto request = payload ? addon::decode_usage_scan_request(*payload, nullptr, 3) : std::nullopt;
            require(request.has_value(), "read new-process scanner request");
            Usage_invocation invocation(*request);
            agy::handle_usage_scan(invocation);
            require(invocation.m_status == addon::Result_status::SUCCESS,
                "new-process scanner succeeds: " + invocation.m_reason);
            write_bytes(argv[3], addon::serialize_json(invocation.m_payload));
            return 0;
        }
        usage_scan_capability_matches_the_implemented_version();
        native_record_uses_stable_delta_identity_and_exact_dimensions();
        native_response_id_is_optional_and_can_arrive_later();
        missing_usage_and_missing_cache_read_preserve_unknown_coverage();
        malformed_or_unidentifiable_usage_is_rejected();
        native_scan_uses_private_read_only_snapshot();
        native_scan_record_is_accepted_by_usage_scan_v3_codec();
        native_instants_preserve_supported_epoch_nanoseconds();
        native_temporal_pages_and_cache_keep_accounting_context();
        incremental_scan_reuses_persisted_snapshot_and_full_scan_reads_native_data();
        incremental_scan_detects_wal_changes_and_old_dated_new_sources();
        incremental_index_absence_and_unknown_version_rebuild_without_losing_history();
        incremental_scan_rejects_changed_reused_sources_and_duplicate_ownership();
        optional_sidecar_damage_never_rejects_valid_accounting();
        opaque_sidecar_bytes_have_no_new_total_cache_ceiling();
        native_admission_and_cancellation_still_precede_sealing();
        cancellation_during_cache_writes_removes_partial_records();
        actual_provider_pages_pack_counts_and_bytes_without_accounting_loss();
        continuations_preserve_snapshot_identity_integrity_and_cancellation();
        continuations_replay_after_other_snapshots_and_process_restart();
        std::cout << "PASS: Agy native token usage scanner contracts\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
