#include "agy_resume.h"
#include "agy_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <sqlite3.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {

constexpr std::size_t k_max_native_blob_bytes = 4 * 1024 * 1024;
constexpr std::uint64_t k_max_steps = 100000;

bool selector(std::string_view value)
{
    const auto alphanumeric = [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
    };
    return !value.empty() && value.size() <= 256 && alphanumeric(value.front()) &&
        std::all_of(value.begin(), value.end(), [alphanumeric](unsigned char character) {
            return alphanumeric(character) || character == '-' || character == '_' || character == '.';
        });
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;


// The native SQLite journal is outside the package's custody. READONLY alone
// still lets SQLite create WAL/SHM files and update a writable shared index.
// Retain SQLite's ordinary snapshot/locking machinery, but refuse every native
// write and require an existing read-only shared index. Missing/unusable
// sidecars therefore remain unknown; immutable=1 would discard live WAL data.
struct alignas(sqlite3_int64) Readonly_file
{
    sqlite3_file file;
};

sqlite3_file* underlying_file(sqlite3_file* file)
{
    return reinterpret_cast<sqlite3_file*>(reinterpret_cast<Readonly_file*>(file) + 1);
}

sqlite3_vfs* underlying_vfs()
{
    static sqlite3_vfs* native = sqlite3_vfs_find(nullptr);
    return native;
}

const sqlite3_io_methods k_readonly_methods{
    3,
    [](sqlite3_file* file) {
        auto* native = underlying_file(file);
        return native->pMethods->xClose(native);
    },
    [](sqlite3_file* file, void* buffer, int count, sqlite3_int64 offset) {
        auto* native = underlying_file(file);
        return native->pMethods->xRead(native, buffer, count, offset);
    },
    [](sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; },
    [](sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; },
    [](sqlite3_file*, int) { return SQLITE_READONLY; },
    [](sqlite3_file* file, sqlite3_int64* size) {
        auto* native = underlying_file(file);
        return native->pMethods->xFileSize(native, size);
    },
    [](sqlite3_file* file, int level) {
        auto* native = underlying_file(file);
        return native->pMethods->xLock(native, level);
    },
    [](sqlite3_file* file, int level) {
        auto* native = underlying_file(file);
        return native->pMethods->xUnlock(native, level);
    },
    [](sqlite3_file* file, int* held) {
        auto* native = underlying_file(file);
        return native->pMethods->xCheckReservedLock(native, held);
    },
    [](sqlite3_file* file, int operation, void* value) {
        auto* native = underlying_file(file);
        return native->pMethods->xFileControl(native, operation, value);
    },
    [](sqlite3_file* file) {
        auto* native = underlying_file(file);
        return native->pMethods->xSectorSize(native);
    },
    [](sqlite3_file* file) {
        auto* native = underlying_file(file);
        return native->pMethods->xDeviceCharacteristics(native);
    },
    [](sqlite3_file* file, int page, int size, int, void volatile** mapped) {
        auto* native = underlying_file(file);
        if (native->pMethods->iVersion < 2 || !native->pMethods->xShmMap) return SQLITE_IOERR_SHMMAP;
        const int code = native->pMethods->xShmMap(native, page, size, 0, mapped);
        // Another SQLite connection in this process may already own a writable
        // mapping. Never let this reader write its read marks into that mapping.
        return code == SQLITE_OK ? SQLITE_READONLY : code;
    },
    [](sqlite3_file* file, int offset, int count, int flags) {
        auto* native = underlying_file(file);
        return native->pMethods->iVersion >= 2 && native->pMethods->xShmLock
            ? native->pMethods->xShmLock(native, offset, count, flags) : SQLITE_IOERR_SHMLOCK;
    },
    [](sqlite3_file* file) {
        auto* native = underlying_file(file);
        if (native->pMethods->iVersion >= 2 && native->pMethods->xShmBarrier) native->pMethods->xShmBarrier(native);
    },
    [](sqlite3_file* file, int) {
        auto* native = underlying_file(file);
        return native->pMethods->iVersion >= 2 && native->pMethods->xShmUnmap
            ? native->pMethods->xShmUnmap(native, 0) : SQLITE_OK;
    },
    [](sqlite3_file*, sqlite3_int64, int, void** mapped) { *mapped = nullptr; return SQLITE_OK; },
    [](sqlite3_file*, sqlite3_int64, void*) { return SQLITE_OK; }
};

const char* readonly_vfs()
{
    static sqlite3_vfs vfs = [] {
        auto* native = underlying_vfs();
        if (!native) return sqlite3_vfs{};
        auto result = *native;
        result.pNext = nullptr;
        result.zName = "agy-inspection-readonly";
        result.szOsFile += static_cast<int>(sizeof(Readonly_file));
        result.xOpen = [](sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* output) {
            if (!name || (flags & (SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_EXCLUSIVE))) return SQLITE_READONLY;
            flags = (flags & ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_READONLY;
            const int code = underlying_vfs()->xOpen(underlying_vfs(), name, underlying_file(file), flags, output);
            if (underlying_file(file)->pMethods) file->pMethods = &k_readonly_methods;
            return code;
        };
        result.xDelete = [](sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; };
        return result;
    }();
    static const bool registered = vfs.zName && sqlite3_vfs_register(&vfs, 0) == SQLITE_OK;
    return registered ? vfs.zName : nullptr;
}

std::string readonly_uri(const fs::path& path)
{
    const auto value = path.generic_u8string();
    std::string uri = value.starts_with(u8"//") ? "file://" : "file:";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~')
        {
            uri += static_cast<char>(byte);
        }
        else {
            uri += '%';
            uri += hex[byte >> 4];
            uri += hex[byte & 15];
        }
    }
    return uri + "?mode=ro&readonly_shm=1";
}

Statement query(sqlite3* database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_finalize(statement);
        statement = nullptr;
    }
    return Statement(statement, sqlite3_finalize);
}

std::optional<std::string_view> bytes(sqlite3_stmt* statement, int column, int type)
{
    if (sqlite3_column_type(statement, column) != type) {
        return std::nullopt;
    }
    const int size = sqlite3_column_bytes(statement, column);
    if (size < 0 || static_cast<std::size_t>(size) > k_max_native_blob_bytes) {
        return std::nullopt;
    }
    const auto* value = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return std::string_view(value ? value : "", static_cast<std::size_t>(size));
}

bool varint(std::string_view input, std::size_t& offset, std::uint64_t& value)
{
    value = 0;
    for (unsigned int shift = 0; shift < 64 && offset < input.size(); shift += 7) {
        const auto byte = static_cast<unsigned char>(input[offset++]);
        if (shift == 63 && byte > 1) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

template<typename Visitor>
bool fields(std::string_view input, const Visitor& visit)
{
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::uint64_t tag = 0;
        std::uint64_t value = 0;
        if (!varint(input, offset, tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1fffffff) {
            return false;
        }
        const auto wire = tag & 7;
        std::string_view data;
        if (wire == 0) {
            if (!varint(input, offset, value)) return false;
        }
        else
        if (wire == 1 || wire == 5) {
            const std::size_t count = wire == 1 ? 8 : 4;
            if (count > input.size() - offset) return false;
            offset += count;
        }
        else
        if (wire == 2) {
            if (!varint(input, offset, value) || value > input.size() - offset) return false;
            data = input.substr(offset, static_cast<std::size_t>(value));
            offset += static_cast<std::size_t>(value);
        }
        else {
            return false;
        }
        if (!visit(tag >> 3, wire, value, data)) return false;
    }
    return true;
}

std::optional<bool> text_item_content(std::string_view item)
{
    // TextOrScopeItem.chunk is a oneof: text=1, ContextScopeItem=2.
    // Only retained text is understood here. Other item kinds remain unknown;
    // opaque bytes or an empty encoded text field do not prove content.
    bool text_seen = false;
    bool content = false;
    const bool valid = fields(item, [&](auto field, auto wire, auto, auto text) {
        if (field != 1 || wire != 2 || text_seen || !addon::is_valid_utf8(text)) return false;
        text_seen = true;
        content = !text.empty();
        return true;
    });
    return valid ? std::optional(content) : std::nullopt;
}

std::optional<bool> user_content(std::string_view payload)
{
    // Native step_format 0 stores CortexStep: type=14 is UserInput and its
    // field 19 carries UserInput's user_response string (2) and repeated
    // TextOrScopeItem messages (3). Native 1.5.3 descriptors and captured
    // conversations establish these types; byte length alone does not.
    bool type_seen = false;
    bool input_seen = false;
    bool response_seen = false;
    bool content = false;
    const bool valid = fields(payload, [&](auto field, auto wire, auto number, auto data) {
        if (field == 1) {
            if (wire != 0 || type_seen || number != 14) return false;
            type_seen = true;
        }
        if (field == 19) {
            if (wire != 2 || input_seen) return false;
            input_seen = true;
            if (!fields(data, [&](auto child, auto child_wire, auto, auto text) {
                if (child == 2) {
                    if (child_wire != 2 || response_seen || !addon::is_valid_utf8(text)) return false;
                    response_seen = true;
                    content = content || !text.empty();
                }
                if (child == 3) {
                    if (child_wire != 2) return false;
                    const auto item = text_item_content(text);
                    if (!item) return false;
                    content = content || *item;
                }
                return true;
            })) return false;
        }
        return true;
    });
    return valid && type_seen && input_seen ? std::optional(content) : std::nullopt;
}

bool root_metadata(std::string_view payload, std::string_view identity)
{
    bool root_seen = false;
    bool parent_seen = false;
    return fields(payload, [&](auto field, auto wire, auto, auto data) {
        if (field == 5) {
            if (parent_seen || wire != 2 || !data.empty()) return false;
            parent_seen = true;
        }
        if (field == 6) {
            if (root_seen || wire != 2 || data != identity) return false;
            root_seen = true;
        }
        return true;
    }) && root_seen;
}

std::optional<std::string> resolved_home(const addon::Session_launch_context& context)
{
    if (!context.launch_environment) return std::nullopt;
    const auto command = recognize_launcher(context.launcher.script_body);
    const auto& frozen = *context.launch_environment;
    const bool windows = frozen.platform_id == "windows-x86_64";
    if (!command || !command->sole_command ||
        !std::all_of(command->environment.begin(), command->environment.end(),
            [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); }))
    {
        return std::nullopt;
    }
    const auto environment = direct_environment(frozen, *command);
    return native_home(addon::environment_value(environment, windows ? "USERPROFILE" : "HOME"));
}

int check_cancelled(void* value)
{
    const auto& cancelled = *static_cast<const std::function<bool()>*>(value);
    return cancelled && cancelled();
}

} // namespace

std::optional<std::string> native_home(std::string_view value)
{
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string_view::npos ||
        !addon::is_valid_utf8(value))
    {
        return std::nullopt;
    }
    const auto path = addon::path_from_utf8(value);
    return path.is_absolute() ? std::optional(addon::path_to_utf8(path.lexically_normal())) : std::nullopt;
}

std::optional<Resume_identity> decode_resume_identity(const addon::Json_value& record)
{
    const auto* id = record.find("provider_session_id");
    const auto* home = record.find("native_home");
    if (!record.is_object() || record.as_object().size() != (home ? 2u : 1u) ||
        !id || !id->is_string() || !selector(id->as_string()) || (home && !home->is_string()))
    {
        return std::nullopt;
    }
    Resume_identity identity{id->as_string(), std::nullopt};
    if (home) {
        identity.native_home = native_home(home->as_string());
        if (!identity.native_home) return std::nullopt;
    }
    return identity;
}

addon::Json_value encode_resume_identity(const Resume_identity& identity)
{
    auto record = addon::Json_value::object();
    record.set("provider_session_id", addon::Json_value(identity.provider_session_id));
    if (identity.native_home) record.set("native_home", addon::Json_value(*identity.native_home));
    return record;
}

bool resume_scope_matches(const Resume_identity& identity, const addon::Session_launch_context& context)
{
    if (!identity.native_home) return true;
    const auto home = resolved_home(context);
    return home && *home == *identity.native_home;
}

addon::Resume_inspect_result inspect_resume(
    const Resume_identity& identity,
    const addon::Session_launch_context& context,
    const std::function<bool()>& cancelled)
{
    addon::Resume_inspect_result result;
    const auto home = resolved_home(context);
    if (!home || (identity.native_home && *identity.native_home != *home) || (cancelled && cancelled())) {
        return result;
    }
    const auto command = recognize_launcher(context.launcher.script_body);
    const bool windows = context.launch_environment->platform_id == "windows-x86_64";
    if (!command || !literal_options(command->arguments, windows, {})) {
        // Switch extraction requires validated literal options. An unsupported
        // invocation cannot establish the native storage scope for inspection.
        return result;
    }
    if (literal_switch(command->arguments, "remote-agent")) {
        // Remote conversation storage belongs to the selected native project;
        // the local home cannot establish either availability or absence.
        return result;
    }
    const auto root = addon::path_from_utf8(*home) / ".gemini" / "antigravity-cli" / "conversations";
    const auto path = root / addon::path_from_utf8(identity.provider_session_id + ".db");
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        result.availability = addon::Resume_availability::ABSENT;
        return result;
    }
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return result;

#ifndef _WIN32
    // SQLite's Unix VFS changes existing WAL/SHM ownership when euid is zero,
    // even for read-only opens. Optional inspection cannot mutate native state.
    // Keep that privileged scope unknown; native resume remains a separate path.
    if (geteuid() == 0) return result;
#endif

    const auto* vfs = readonly_vfs();
    if (!vfs) return result;
    sqlite3* opened = nullptr;
    const int code = sqlite3_open_v2(readonly_uri(path).c_str(), &opened,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOFOLLOW, vfs);
    Database database(opened, sqlite3_close_v2);
    if (code != SQLITE_OK || !database) return result;
    sqlite3_limit(database.get(), SQLITE_LIMIT_LENGTH, static_cast<int>(k_max_native_blob_bytes));
    sqlite3_busy_timeout(database.get(), 100);
    sqlite3_progress_handler(database.get(), 1000, check_cancelled, const_cast<std::function<bool()>*>(&cancelled));
    // One read transaction sees a consistent database/WAL snapshot while the
    // native process continues writing. No title, usage or file-size heuristic
    // participates in the content predicate.
    if (sqlite3_exec(database.get(), "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) return result;
    auto trajectory = query(database.get(), "SELECT trajectory_id,cascade_id,trajectory_type FROM trajectory_meta");
    if (!trajectory || sqlite3_step(trajectory.get()) != SQLITE_ROW) return result;
    const auto trajectory_id = bytes(trajectory.get(), 0, SQLITE_TEXT);
    const auto cascade_id = bytes(trajectory.get(), 1, SQLITE_TEXT);
    if (!trajectory_id || !selector(*trajectory_id) || !cascade_id || *cascade_id != identity.provider_session_id ||
        sqlite3_column_type(trajectory.get(), 2) != SQLITE_INTEGER || sqlite3_column_int64(trajectory.get(), 2) != 4 ||
        sqlite3_step(trajectory.get()) != SQLITE_DONE)
    {
        return result;
    }
    auto metadata = query(database.get(), "SELECT id,data FROM trajectory_metadata_blob");
    if (!metadata || sqlite3_step(metadata.get()) != SQLITE_ROW) return result;
    const auto metadata_id = bytes(metadata.get(), 0, SQLITE_TEXT);
    const auto metadata_blob = bytes(metadata.get(), 1, SQLITE_BLOB);
    if (!metadata_id || *metadata_id != "main" || !metadata_blob ||
        !root_metadata(*metadata_blob, identity.provider_session_id) || sqlite3_step(metadata.get()) != SQLITE_DONE)
    {
        return result;
    }
    auto steps = query(database.get(), "SELECT idx,step_type,step_payload,step_format FROM steps ORDER BY idx");
    auto parents = query(database.get(), "SELECT COUNT(*) FROM parent_references");
    if (!steps || !parents || sqlite3_step(parents.get()) != SQLITE_ROW) return result;
    const auto parent_count = sqlite3_column_int64(parents.get(), 0);
    if (parent_count < 0 || sqlite3_step(parents.get()) != SQLITE_DONE) return result;
    std::uint64_t count = 0;
    bool retained_content = false;
    for (;;) {
        if (cancelled && cancelled()) return result;
        const int step = sqlite3_step(steps.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW || count >= k_max_steps ||
            sqlite3_column_type(steps.get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_int64(steps.get(), 0) != static_cast<sqlite3_int64>(count) ||
            sqlite3_column_type(steps.get(), 1) != SQLITE_INTEGER ||
            sqlite3_column_type(steps.get(), 3) != SQLITE_INTEGER)
        {
            return result;
        }
        ++count;
        const auto payload = bytes(steps.get(), 2, SQLITE_BLOB);
        if (!payload || sqlite3_column_int64(steps.get(), 3) != 0 ||
            !fields(*payload, [](auto, auto, auto, auto) { return true; }))
        {
            return result;
        }
        if (sqlite3_column_int64(steps.get(), 1) == 14) {
            const auto input = user_content(*payload);
            if (!input) return result;
            retained_content = retained_content || *input;
        }
    }
    result.availability = addon::Resume_availability::AVAILABLE;
    if (retained_content) result.content = addon::Resume_content::NONEMPTY;
    else
    if (count == 0 && parent_count == 0) result.content = addon::Resume_content::EMPTY;
    return result;
}

} // namespace agy
