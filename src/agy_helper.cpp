#include "agy_events.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/limits.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>
#include <cstdlib>
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

std::optional<addon::Json_value> read_hook()
{
    std::string bytes;
    for (;;) {
        const int character = std::fgetc(stdin);
        if (character == EOF) {
            break;
        }
        if (bytes.size() == k_max_hook_bytes) {
            std::fputs("Antigravity hook input exceeds its size bound.\n", stderr);
            return std::nullopt;
        }
        bytes.push_back(static_cast<char>(character));
    }
    if (std::ferror(stdin)) {
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
    if (argc != 2) {
        return addon::k_helper_exit_invalid_arguments;
    }
    const std::string_view name(argv[1]);
    if (name != "PreInvocation" && name != "PostToolUse" && name != "Stop") {
        return addon::k_helper_exit_invalid_arguments;
    }
    // The hook file is user-global: outside a managed session it is inert.
    if (!enabled("AGY_LOGONOMIC_HOOKS_ENABLED")) {
        return 0;
    }
    addon::Contract_failure failure;
    auto environment = addon::read_helper_environment(&failure);
    const auto payload = read_hook();
    if (!environment || !payload) {
        return 0;
    }
    addon::Helper_client client(std::move(*environment));
    if (enabled("AGY_LOGONOMIC_LIFECYCLE_ENABLED")) {
        const auto event = agy::sanitize_hook(name, *payload);
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
