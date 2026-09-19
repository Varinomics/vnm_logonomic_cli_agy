#include "agy_preparation.h"
#include "agy_launcher.h"

#include <logonomic_cli_addon/package_source.h>
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

bool hooks_preinstalled(const addon::Prepare_request& request)
{
    if (!request.launch_environment) {
        return false;
    }
    const bool windows = request.launch_environment->platform_id == "windows-x86_64";
    const std::string home_name = windows ? "USERPROFILE" : "HOME";
    const auto launcher = recognize_launcher(request.launcher.script_body);
    if (!launcher) {
        return false;
    }
    for (const auto& [name, value] : launcher->environment) {
        (void)value;
        if (same_name(name, home_name, windows)) {
            return false;
        }
    }
    std::istringstream assignments(request.launcher.environment_text);
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
    if (request.launcher.runner_key != "direct") {
        std::string script = request.launcher.script_body;
        if (windows) {
            std::transform(script.begin(), script.end(), script.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        }
        if (script.find(home_name) != std::string::npos) {
            return false;
        }
    }
    std::string home;
    for (const auto& member : request.launch_environment->effective_environment.as_object()) {
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

void handle_preparation(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_prepare_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    const bool installed = hooks_preinstalled(*request);
    std::string invocation;
    for (const auto& argument : request->launcher_argv) {
        if (!invocation.empty()) {
            invocation += ' ';
        }
        invocation += '"' + argument + '"';
    }
    if (!installed || !recognize_launcher(invocation)) {
        patch.disposition = addon::Launch_disposition{false, false, false};
        context.succeed(addon::encode_launch_patch(patch, 3));
        return;
    }
    const auto launch = installed ? verified_launch(request->launcher,
        request->launch_environment->effective_environment, request->working_directory, context) : std::nullopt;
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    const bool lifecycle = launch && version_is_at_least(launch->version, 1, 1, 11);
    patch.disposition = addon::Launch_disposition{true, lifecycle, true};
    patch.set.emplace_back("AGY_LOGONOMIC_HOOKS_ENABLED", "1");
    patch.set.emplace_back("AGY_LOGONOMIC_LIFECYCLE_ENABLED",
        lifecycle ? "1" : "0");
    context.succeed(addon::encode_launch_patch(patch, 3));
}

} // namespace agy
