#include "agy_events.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/limits.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
constexpr std::size_t k_max_hook_bytes = 4 * 1024 * 1024;

// Normal wrapper cancellation/reaping fits the hook's operational budget.
// OS-delayed termination may take longer; the SDK retains custody until exit.
static_assert(4 * (addon::limits::k_helper_client_wrapper_deadline_ms +
                  addon::limits::k_reap_deadline_ms) + 2000 < 65000);

std::optional<addon::Json_value> read_hook(bool pass_through)
{
    std::string bytes;
    bool over_bound = false;
    for (;;) {
        char buffer[8192];
        const auto count = std::fread(buffer, 1, sizeof(buffer), stdin);
        if (count == 0) {
            break;
        }
        if (pass_through) {
            (void)std::fwrite(buffer, 1, count, stdout);
        }
        if (!over_bound) {
            if (count > k_max_hook_bytes - bytes.size()) {
                std::fputs("Antigravity hook input exceeds its size bound.\n", stderr);
                if (!pass_through) {
                    return std::nullopt;
                }
                bytes.clear();
                over_bound = true;
            }
            else {
                bytes.append(buffer, count);
            }
        }
    }
    if (pass_through) {
        // Pass-through owns stdout; StatusLine emits no later response.
        // Closing flushes the bytes and delivers EOF before host IPC, so a
        // user's command reading all stdin can run while transport is pending.
        (void)std::fclose(stdout);
    }
    if (over_bound || std::ferror(stdin)) {
        return std::nullopt;
    }
    addon::Json_parse_error failure;
    const auto payload = addon::parse_json(bytes, {}, &failure);
    return payload && payload->is_object() ? payload : std::nullopt;
}

bool enabled(const char* name)
{
    const auto* value = std::getenv(name);
    return value && std::string_view(value) == "1";
}

bool permission_snapshot_enabled()
{
    const auto* nonce = std::getenv("LOGONOMIC_PROVIDER_GENERATION_NONCE");
    const auto* marker = std::getenv(addon::k_permission_snapshot_enabled_environment);
    return nonce && *nonce && marker && std::string_view(marker) == nonce;
}

void write_response(const addon::Json_value& response)
{
    const auto bytes = addon::serialize_json(response) + '\n';
    (void)std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    (void)std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        return addon::k_helper_exit_invalid_arguments;
    }
#endif
    if (argc < 2 || argc > 3) {
        return addon::k_helper_exit_invalid_arguments;
    }
    const std::string_view name(argv[1]);
    const bool status_line = name == "StatusLine";
    const bool pass_through = status_line && argc == 3 && std::string_view(argv[2]) == "--pass-through";
    if ((!status_line && name != "PreInvocation" && name != "PostToolUse" && name != "Stop") ||
        (argc == 3 && !pass_through))
    {
        return addon::k_helper_exit_invalid_arguments;
    }
#ifndef _WIN32
    if (pass_through) {
        // A user's status command may finish without reading its stdin. Its
        // closed pipe must not terminate the permission observer as well.
        std::signal(SIGPIPE, SIG_IGN);
    }
#endif
    // The hook file is user-global: outside a managed session it is inert.
    const bool managed = enabled("AGY_LOGONOMIC_HOOKS_ENABLED") &&
        (!status_line || permission_snapshot_enabled());
    if (!managed && !pass_through) {
        return 0;
    }
    addon::Contract_failure failure;
    auto environment = addon::read_helper_environment(&failure);
    const auto payload = read_hook(pass_through);
    if (!managed || !environment || !payload) {
        return 0;
    }
    // Native status scripts have a five-second budget. The shorter shared
    // deadline leaves time for the shell and the user's command to settle.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::function<bool()> cancelled;
    if (status_line) {
        cancelled = [deadline] { return std::chrono::steady_clock::now() >= deadline; };
    }
    addon::Helper_client client(std::move(*environment), std::move(cancelled));
    if (enabled("AGY_LOGONOMIC_LIFECYCLE_ENABLED")) {
        const auto event = agy::sanitize_hook(name, *payload);
        if (status_line && event.as_object().empty()) {
            return 0;
        }
        const auto key   = addon::random_nonce_hex();
        const auto nonce = addon::random_nonce_hex();
        auto outcome = client.deliver_provider_event(name, event, key, nonce);
        if (!outcome.committed && (outcome.exit_code == addon::k_helper_exit_transport_failure ||
                                  outcome.exit_code == addon::k_helper_exit_pending))
        {
            // Retry the same evidence and transition instance after lost output.
            outcome = client.deliver_provider_event(name, event, key, nonce);
        }
        if (outcome.committed) {
            (void)client.acknowledge_provider_event(key);
        }
        if (!outcome.committed || !outcome.accepted) {
            std::fputs("Antigravity lifecycle observation was not accepted.\n", stderr);
        }
    }
    if (name == "PostToolUse") {
        write_response(addon::Json_value::object());
    }
    else
    if (name == "PreInvocation") {
        const auto instruction = client.request_title_instruction("agy", "PreInvocation", false,
            addon::Model_context_delivery{});
        if (instruction.completed && instruction.accepted && !instruction.instruction.empty()) {
            auto step = addon::Json_value::object();
            step.set("ephemeralMessage", addon::Json_value(instruction.instruction));
            auto steps = addon::Json_value::array();
            steps.push_back(std::move(step));
            auto response = addon::Json_value::object();
            response.set("injectSteps", std::move(steps));
            write_response(response);
        }
    }
    // Stop's empty stdout never blocks or continues the user's agent turn.
    return 0;
}
