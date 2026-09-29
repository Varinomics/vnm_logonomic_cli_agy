#include "agy_preparation.h"
#include "agy_launcher.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/session_contracts.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

namespace fs = std::filesystem;
constexpr std::size_t k_max_hooks_bytes = 256 * 1024;

bool same_name(std::string_view left, std::string_view right, bool windows)
{
    if (!windows) {
        return left == right;
    }
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

bool owned_path(const fs::path& path, bool directory)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || (directory ? !fs::is_directory(status) : !fs::is_regular_file(status))) {
        return false;
    }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
#endif
    return true;
}

std::string hook_command(std::string_view event, bool windows)
{
    if (windows) {
        const std::string script =
            "if ($env:AGY_LOGONOMIC_HOOKS_ENABLED -eq '1' -and "
            "$env:LOGONOMIC_PROVIDER_HELPERS_DIR -and $env:LOGONOMIC_SESSION_CAPABILITY) { "
            "& (Join-Path $env:LOGONOMIC_PROVIDER_HELPERS_DIR 'agy_helper.exe') " + std::string(event) + " }";
        std::string utf16;
        for (const char character : script) {
            utf16 += character;
            utf16 += '\0';
        }
        return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
            addon::encode_base64(utf16);
    }
    return "sh -c 'if [ \"$AGY_LOGONOMIC_HOOKS_ENABLED\" = 1 ] && "
        "[ -n \"$LOGONOMIC_PROVIDER_HELPERS_DIR\" ] && [ -n \"$LOGONOMIC_SESSION_CAPABILITY\" ]; then "
        "\"$LOGONOMIC_PROVIDER_HELPERS_DIR/agy_helper\" " + std::string(event) + "; fi'";
}

addon::Json_value handlers(std::string_view name, bool windows)
{
    auto handler = addon::Json_value::object();
    handler.set("type", addon::Json_value("command"));
    handler.set("command", addon::Json_value(hook_command(name, windows)));
    // Two delivery attempts, response acknowledgement, and title request may
    // each consume the helper client's bounded wrapper deadline and reap.
    handler.set("timeout", addon::Json_value(65));
    auto result = addon::Json_value::array();
    result.push_back(std::move(handler));
    return result;
}

} // namespace

addon::Json_value hook_definition(bool windows)
{
    auto result = addon::Json_value::object();
    result.set("PreInvocation", handlers("PreInvocation", windows));
    auto tool = addon::Json_value::object();
    tool.set("matcher", addon::Json_value("invoke_subagent"));
    tool.set("hooks", handlers("PostToolUse", windows));
    auto tools = addon::Json_value::array();
    tools.push_back(std::move(tool));
    result.set("PostToolUse", std::move(tools));
    result.set("Stop", handlers("Stop", windows));
    return result;
}

bool hooks_preinstalled(
    const addon::Launcher_request&                  launcher,
    const std::optional<addon::Launch_environment>& launch_environment)
{
    if (!launch_environment) {
        return false;
    }
    const bool windows = launch_environment->platform_id == "windows-x86_64";
    const std::string home_name = windows ? "USERPROFILE" : "HOME";
    const auto command = recognize_launcher(launcher.script_body);
    if (!command) {
        return false;
    }
    for (const auto& [name, value] : command->environment) {
        (void)value;
        if (same_name(name, home_name, windows)) {
            return false;
        }
    }
    std::istringstream assignments(launcher.environment_text);
    for (std::string line; std::getline(assignments, line);) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        auto name = line.substr(0, equals);
        const auto first = name.find_first_not_of(" \t");
        if (first != std::string::npos) {
            name = name.substr(first, name.find_last_not_of(" \t") - first + 1);
        }
        if (same_name(name, home_name, windows)) {
            return false;
        }
    }
    // A shell expression which mutates the home outside recognized assignments
    // cannot prove which user-global hook file the launched process will read.
    if (launcher.runner_key != "direct") {
        std::string script = launcher.script_body;
        if (windows) {
            std::transform(script.begin(), script.end(), script.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        }
        if (script.find(home_name) != std::string::npos) {
            return false;
        }
    }
    std::string home;
    for (const auto& member : launch_environment->effective_environment.as_object()) {
        if (same_name(member.name, home_name, windows)) {
            home = member.value.as_string();
        }
    }
    const auto directory = (addon::path_from_utf8(home) / ".gemini" / "config").lexically_normal();
    if (home.empty() || !directory.is_absolute()) {
        return false;
    }
    for (auto parent = directory;; parent = parent.parent_path()) {
        if (!owned_path(parent, true)) {
            return false;
        }
        if (parent == parent.root_path()) {
            break;
        }
    }
    const auto path = directory / "hooks.json";
    if (!owned_path(path, false)) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(k_max_hooks_bytes + 1, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    if (stream.bad() || bytes.size() > k_max_hooks_bytes) {
        return false;
    }
    addon::Json_parse_error failure;
    const auto hooks = addon::parse_json(bytes, {}, &failure);
    const auto* installed = hooks ? hooks->find("logonomic-hook-relay") : nullptr;
    return installed && addon::serialize_json_canonical(*installed) ==
        addon::serialize_json_canonical(hook_definition(windows));
}

namespace {

struct hook_delivery_t
{
    bool installed = false;
    bool lifecycle = false;
};

// The package's hooks report into a launch only through the stanza the user
// installed. Lifecycle observations further need a verified native Antigravity
// that honours the observed hook contract; without it only titles are delivered.
// nullopt: the host cancelled the version probe.
std::optional<hook_delivery_t> hook_delivery(
    const addon::Launcher_request&                  launcher,
    const std::optional<addon::Launch_environment>& launch_environment,
    std::string_view                                working_directory,
    addon::Invocation_context&                      context)
{
    if (!hooks_preinstalled(launcher, launch_environment)) {
        return hook_delivery_t{};
    }
    const auto launch = verified_launch(launcher, launch_environment->effective_environment,
        working_directory, context);
    if (context.cancel_requested()) {
        return std::nullopt;
    }
    return hook_delivery_t{true, launch && version_is_at_least(launch->version, 1, 1, 11)};
}

// The installed stanza stays inert in a launch that does not enable it.
void enable_hooks(std::vector<std::pair<std::string, std::string>>& set, const hook_delivery_t& hooks)
{
    if (hooks.installed) {
        set.emplace_back("AGY_LOGONOMIC_HOOKS_ENABLED", "1");
        set.emplace_back("AGY_LOGONOMIC_LIFECYCLE_ENABLED", hooks.lifecycle ? "1" : "0");
    }
}

void prepare_launch(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    std::string invocation;
    for (const auto& argument : request->launcher_argv) {
        if (!invocation.empty()) {
            invocation += ' ';
        }
        invocation += '"' + argument + '"';
    }
    const auto hooks = recognize_launcher(invocation)
        ? hook_delivery(request->launcher, request->launch_environment, request->working_directory, context)
        : hook_delivery_t{};
    if (!hooks) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    patch.disposition = addon::Launch_disposition{hooks->installed, hooks->lifecycle, hooks->installed};
    enable_hooks(patch.set, *hooks);
    context.succeed(addon::encode_launch_patch(patch, 3));
}

// Resume keeps the launcher's options only when they leave the session
// interactive: an option that selects another conversation, runs a single
// prompt or does not start a session refuses the resume.
const std::vector<std::string_view>& resume_refused_options()
{
    static const std::vector<std::string_view> refused{
        "c", "continue", "conversation", "new-project", "p", "print", "prompt", "i", "prompt-interactive",
        "h", "help", "version", "log-file"};
    return refused;
}

// Antigravity's conversation identities are UUIDs. The selector becomes the
// argv word after --conversation, so it is limited to identity characters and
// cannot read as an option.
bool conversation_selector(std::string_view identity)
{
    return !identity.empty() && identity.size() <= 256 && std::isalnum(static_cast<unsigned char>(identity.front())) &&
        std::all_of(identity.begin(), identity.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_' || character == '.';
        });
}

void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    // The host default record, which a legacy import also produces.
    const auto* selector = request->resume_record.find("provider_session_id");
    if (request->resume_record.as_object().size() != 1 || !selector || !selector->is_string() ||
        !conversation_selector(selector->as_string()))
    {
        context.fail(addon::Result_status::FAILED, addon::reason::k_resume_record_invalid);
        return;
    }
    const auto& environment = *request->context.launch_environment;
    const auto command = recognize_launcher(request->context.launcher.script_body);
    if (!command) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized");
        return;
    }
    // The launcher's other commands, such as a sourced environment script,
    // would not run before the resumed process.
    const bool windows = environment.platform_id == "windows-x86_64";
    const bool literal_environment = std::all_of(command->environment.begin(), command->environment.end(),
        [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); });
    if (!command->sole_command || !literal_environment ||
        !literal_options(command->arguments, windows, resume_refused_options()))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_launcher_unavailable");
        return;
    }
    const auto program = resolve_native_launcher(*command, environment.effective_environment,
        request->context.working_directory);
    if (!program) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_launcher_program_unavailable);
        return;
    }
    const auto hooks = hook_delivery(request->context.launcher, request->context.launch_environment,
        request->context.working_directory, context);
    if (!hooks) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    addon::Resume_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/agy_launch_bridge.exe";
#else
    result.process.executable = "helpers/agy_launch_bridge";
#endif
    result.process.argv = {*program};
    result.process.argv.insert(result.process.argv.end(), command->arguments.begin(), command->arguments.end());
    result.process.argv.insert(result.process.argv.end(), {"--conversation", selector->as_string()});
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set = command->environment;
    enable_hooks(result.patch.set, *hooks);
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    if (request->startup_observer) {
        if (request->startup_observer->confirmation != "observed-identity" || !request->context.profile_storage) {
            context.fail(addon::Result_status::UNAVAILABLE, "startup_observation_unavailable");
            return;
        }
#ifdef _WIN32
        result.process.executable = "helpers/agy_resume_bridge.exe";
#else
        result.process.executable = "helpers/agy_resume_bridge";
#endif
        result.startup_observer = request->startup_observer;
        addon::set_environment(result.patch.set, "AGY_LOGONOMIC_STARTUP_OBSERVER",
            addon::serialize_json(addon::encode_startup_observer_offer(*request->startup_observer)));
        addon::set_environment(result.patch.set, "AGY_LOGONOMIC_STARTUP_ROOT", request->context.profile_storage->root);
    }
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

void classify_startup(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_startup_observation_request(context.request().payload, &failure);
    const auto body = request ? addon::parse_json(request->body, {}, nullptr) : std::nullopt;
    const auto* records = body ? body->find("native_records") : nullptr;
    const auto evidence = body && body->is_object() && body->as_object().size() == 1 && records && records->is_string()
        ? observe_startup(records->as_string()) : std::nullopt;
    if (!evidence) {
        context.fail(addon::Result_status::FAILED, "invalid_startup_observation");
        return;
    }
    context.succeed(addon::encode_startup_observation_result({evidence->conversation_ids}));
}

} // namespace

void handle_preparation(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == "interactive-resume" &&
        request.operation == addon::k_classify_startup_operation)
    {
        classify_startup(context);
    }
    else
    if (request.capability.capability_id == "interactive-resume" &&
        request.operation == addon::k_prepare_resume_operation)
    {
        prepare_resume(context);
    }
    else
    if (request.capability.capability_id == "launch-preparation" &&
        request.operation == addon::k_prepare_operation)
    {
        prepare_launch(context);
    }
    else {
        context.reject_unknown_operation();
    }
}

} // namespace agy
