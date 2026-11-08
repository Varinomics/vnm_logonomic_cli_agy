#pragma once

#include <logonomic_cli_addon/helper_client.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agy {

inline constexpr const char* k_activation_file_environment = "AGY_LOGONOMIC_ACTIVATION_FILE";

struct Activation
{
    std::string provider_session_id;
    std::string transition_nonce;
    bool committed = false;
};

// The bridge writes one private, generation/session-bound source snapshot.
// Hooks capture it before reading stdin or starting asynchronous host work.
class Activation_file
{
public:
    explicit Activation_file(const logonomic::cli_addon::Helper_environment& environment);
    ~Activation_file();
    bool initialize();
    const std::filesystem::path& path() const { return m_path; }
    bool select(const Activation& activation);
    bool commit(std::string_view nonce);
    void invalidate();

private:
    bool write();
    std::filesystem::path m_root;
    std::filesystem::path m_path;
    std::string m_generation_nonce;
    std::string m_session_digest;
    Activation m_current;
    std::mutex m_mutex;
};

std::optional<Activation> capture_activation(
    const std::filesystem::path& path,
    const logonomic::cli_addon::Helper_environment& environment);
bool await_activation(
    const std::filesystem::path& path,
    const logonomic::cli_addon::Helper_environment& environment,
    const Activation& captured);
bool current_activation(
    const std::filesystem::path& path,
    const logonomic::cli_addon::Helper_environment& environment,
    const Activation& captured);

} // namespace agy
