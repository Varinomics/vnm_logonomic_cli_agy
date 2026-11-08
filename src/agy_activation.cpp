#include "agy_activation.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace {

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

std::optional<std::string> text(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_string() ? std::optional(member->as_string()) : std::nullopt;
}

} // namespace

Activation_file::Activation_file(const addon::Helper_environment& environment)
:
    m_generation_nonce(environment.generation_nonce),
    m_session_digest(digest(environment.session_capability))
{}

Activation_file::~Activation_file()
{
    if (!m_root.empty()) {
        std::error_code error;
        fs::remove_all(m_root, error);
    }
}

bool Activation_file::initialize()
{
    std::error_code error;
    const auto temporary = fs::temp_directory_path(error);
    if (error) return false;
    const auto root = temporary / ("logonomic-agy-" + addon::random_nonce_hex());
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    const bool valid = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!valid || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return false;
    const auto sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // The inherited entry restricts both the directory and its files to this
    // user. The path/nonce is correlation, never authority.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    const bool created = CreateDirectoryW(root.c_str(), &security) != FALSE;
    LocalFree(descriptor);
    if (!created) return false;
#else
    if (mkdir(root.c_str(), 0700) != 0) return false;
#endif
    m_root = root;
    m_path = root / "activation.json";
    return write(true);
}

bool Activation_file::write(bool initial)
{
    auto value = addon::Json_value::object();
    value.set("version", addon::Json_value(1));
    value.set("generation_nonce", addon::Json_value(m_generation_nonce));
    value.set("session_digest", addon::Json_value(m_session_digest));
    value.set("provider_session_id", addon::Json_value(m_current.provider_session_id));
    value.set("transition_nonce", addon::Json_value(m_current.transition_nonce));
    value.set("committed", addon::Json_value(m_current.committed));
    const auto pending = m_root / "activation.pending";
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        stream << addon::serialize_json(value);
        stream.close();
        if (!stream) return false;
    }
#ifdef _WIN32
    // Existing readers keep the old snapshot. MoveFileEx cannot replace that
    // open target even when its handles permit deletion; ReplaceFile can.
    return initial
        ? MoveFileExW(pending.c_str(), m_path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE
        : ReplaceFileW(m_path.c_str(), pending.c_str(), nullptr, 0, nullptr, nullptr) != FALSE;
#else
    (void)initial;
    std::error_code error;
    fs::rename(pending, m_path, error);
    return !error;
#endif
}

bool Activation_file::select(const Activation& activation)
{
    const std::lock_guard lock(m_mutex);
    m_current = activation;
    return write();
}

bool Activation_file::commit(std::string_view nonce)
{
    const std::lock_guard lock(m_mutex);
    if (m_current.transition_nonce != nonce) return true;
    m_current.committed = true;
    return write();
}

void Activation_file::invalidate()
{
    const std::lock_guard lock(m_mutex);
    m_current = {};
    std::error_code error;
    fs::remove(m_path, error);
}

std::optional<Activation> capture_activation(const fs::path& path, const addon::Helper_environment& environment)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || fs::is_symlink(status) || !fs::is_regular_file(status) || fs::file_size(path, error) > 2048 || error) {
        return std::nullopt;
    }
    char buffer[2049]{};
#ifdef _WIN32
    // Readers retain their source snapshot while the bridge replaces the path.
    // CRT streams omit delete sharing and would make that replacement fail.
    const auto file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    DWORD count = 0;
    const bool read = ReadFile(file, buffer, sizeof(buffer), &count, nullptr) != FALSE;
    CloseHandle(file);
    if (!read || count > 2048) {
        return std::nullopt;
    }
#else
    std::ifstream stream(path, std::ios::binary);
    stream.read(buffer, sizeof(buffer));
    const auto count = stream.gcount();
    if (stream.bad() || count > 2048) return std::nullopt;
#endif
    const auto value = addon::parse_json(std::string_view(buffer, static_cast<std::size_t>(count)), {}, nullptr);
    if (!value || !value->is_object() || value->as_object().size() != 6) return std::nullopt;
    const auto* version = value->find("version");
    const auto* committed = value->find("committed");
    const auto generation = text(*value, "generation_nonce");
    const auto session = text(*value, "session_digest");
    const auto identity = text(*value, "provider_session_id");
    const auto nonce = text(*value, "transition_nonce");
    if (!version || !version->is_number() || version->as_number() != 1 || !committed || !committed->is_boolean() ||
        !generation || *generation != environment.generation_nonce || !session || *session != digest(environment.session_capability) ||
        !identity || identity->empty() || identity->size() > 256 || identity->find('\0') != std::string::npos ||
        !nonce || nonce->size() != 32 || nonce->find_first_not_of("0123456789abcdef") != std::string::npos)
    {
        return std::nullopt;
    }
    return Activation{*identity, *nonce, committed->as_boolean()};
}

bool await_activation(const fs::path& path, const addon::Helper_environment& environment, const Activation& captured)
{
    // Admission of the captured fact is enough for its fenced lifecycle
    // delivery. Context consumption is independently fenced by the host's
    // model-context-delivery@2 activation predicate.
    if (captured.committed) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto current = capture_activation(path, environment);
        if (!current || current->provider_session_id != captured.provider_session_id ||
            current->transition_nonce != captured.transition_nonce)
        {
            return false;
        }
        if (current->committed) return true;
        // This bounded wait runs only inside an actual native hook while its
        // already captured transition is being admitted, never as an idle poll.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool current_activation(const fs::path& path, const addon::Helper_environment& environment, const Activation& captured)
{
    const auto current = capture_activation(path, environment);
    return current && current->committed && current->provider_session_id == captured.provider_session_id &&
        current->transition_nonce == captured.transition_nonce;
}

} // namespace agy
