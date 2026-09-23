#include "agy_usage_scan.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

namespace {

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
                                std::string_view response_id = "native-response-01")
{
    std::string timestamp;
    append_varint_field(timestamp, 1, 1790084400);
    append_varint_field(timestamp, 2, 123000000);

    std::string usage;
    append_bytes_field(usage, 1, "gemini-3-pro");
    append_varint_field(usage, 2, 10);
    append_varint_field(usage, 3, 7);
    if (include_cache_read) append_varint_field(usage, 5, 0);
    append_bytes_field(usage, 7, "bot-native-message");
    append_varint_field(usage, 9, 3);
    append_varint_field(usage, 10, 4);
    append_bytes_field(usage, 11, response_id);

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
    return addon::to_hex(addon::Sha256::digest(read_bytes(path)));
}

void usage_scan_capability_matches_the_implemented_version()
{
    const auto source_root = fs::path(__FILE__).parent_path().parent_path();
    const auto adapter = read_bytes(source_root / "src" / "adapter.cpp");
    require(adapter.find("{\"usage-scan\", 3}") != std::string::npos,
        "adapter claims the implemented usage-scan@3 capability");
    require(adapter.find("{\"usage-scan\", 2}") == std::string::npos,
        "adapter does not claim unsupported usage-scan@2");
    require(fs::exists(source_root / "package" / "capabilities" / "usage-scan@3.json"),
        "usage-scan@3 package capability declaration exists");
    require(!fs::exists(source_root / "package" / "capabilities" / "usage-scan@2.json"),
        "unsupported usage-scan@2 package declaration is absent");
    require(adapter.find("{addon::k_profile_storage_capability_id") != std::string::npos,
        "adapter claims the SDK-required profile-storage@1 capability");
    require(adapter.find("acknowledge_profile_storage(context)") != std::string::npos,
        "adapter handles the profile-storage@1 acknowledgment operation");
    require(fs::exists(source_root / "package" / "capabilities" / "profile-storage@1.json"),
        "profile-storage@1 package capability declaration exists");
}

class Temporary_directory
{
public:
    Temporary_directory()
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / ("agy-usage-test-" + std::to_string(unique));
        if (!fs::create_directory(m_path)) throw std::runtime_error("create test directory");
    }
    ~Temporary_directory() { std::error_code error; fs::remove_all(m_path, error); }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void native_scan_uses_private_read_only_snapshot()
{
    Temporary_directory temporary;
    const auto conversations = temporary.path() / "conversations";
    fs::create_directory(conversations);
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
    require(insert_step(27, "PlannerResponse", planner_metadata), "insert Planner response usage event");
    require(insert_step(28, "Checkpoint", checkpoint_metadata), "insert Checkpoint usage event");
    require(insert_step(29, "Checkpoint", metadata_without_usage()), "insert Checkpoint without usage");
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
    require(records && records->size() == 2,
        "scan Planner and Checkpoint events while omitting a Checkpoint without usage");
    const auto has_record = [&records](std::string_view id) {
        return std::any_of(records->begin(), records->end(), [id](const addon::Usage_record& record) {
            return record.record_id == id;
        });
    };
    require(has_record("agy:cascade-fixture:step:27:response:planner-response") &&
            has_record("agy:cascade-fixture:step:28:response:checkpoint-response"),
        "snapshot scan preserves Planner and Checkpoint native event identities");
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
    require(record.cache_creation.applicable && !record.cache_creation.value && record.cache_creation.coverage &&
            record.cache_creation.coverage->support == addon::Usage_support::UNKNOWN &&
            record.cache_creation.coverage->covered_records == 0 &&
            record.cache_creation.coverage->total_records == 1,
        "cache-creation with no native observation remains unknown");
    require(!record.cost_microusd.applicable && !record.cost_microusd.value,
        "usage records carry no cost observation");
    require(record.completed_date == "2026-09-22", "native completion timestamp becomes a UTC date");

}

void native_scan_record_is_accepted_by_usage_scan_v3_codec()
{
    const auto decoded = agy::decode_native_usage_step(
        metadata_with_usage(true), "cascade-01", 27);
    addon::Contract_failure failure;
    const auto encoded = addon::encode_usage_record(*decoded.record, 3);
    const auto round_trip = addon::decode_usage_record(encoded, &failure, 3);
    require(round_trip && round_trip->record_id == decoded.record->record_id,
        "record satisfies the host usage-scan@3 record contract: " + failure.reason_code + " " + failure.detail);
}

void missing_usage_and_missing_cache_read_are_not_zero_filled()
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
    require(decoded.record->cache_read.applicable && !decoded.record->cache_read.value &&
            decoded.record->cache_read.coverage &&
            decoded.record->cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
            decoded.record->cache_read.coverage->covered_records == 0 &&
            decoded.record->cache_read.coverage->total_records == 1,
        "an absent cache-read counter stays unknown, not zero");
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
}

} // namespace

int main()
{
    try {
        usage_scan_capability_matches_the_implemented_version();
        native_record_uses_stable_delta_identity_and_exact_dimensions();
        missing_usage_and_missing_cache_read_are_not_zero_filled();
        malformed_or_unidentifiable_usage_is_rejected();
        native_scan_uses_private_read_only_snapshot();
        native_scan_record_is_accepted_by_usage_scan_v3_codec();
        std::cout << "PASS: Agy native token usage scanner contracts\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
