#include "agy_preparation.h"
#include "agy_launcher.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

constexpr std::size_t k_max_hooks_bytes = 256 * 1024;

bool same_name(std::string_view left, std::string_view right, bool windows)
{
    if (!windows) {
        return left == right;
    }
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
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

namespace {

bool hooks_manageable(
    const addon::Launcher_request&                  launcher,
    const std::optional<addon::Launch_environment>& launch_environment)
{
    if (!launch_environment) {
        return false;
    }
    const bool windows = launch_environment->platform_id == "windows-x86_64";
    const std::string home_name = windows ? "USERPROFILE" : "HOME";
    const auto command = recognize_launcher(launcher.script_body);
    // Unmodelled commands can mutate the launched process's home, including
    // through a sourced script whose body is absent from the launcher record.
    if (!command || !command->sole_command) {
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
    return !home.empty() && addon::path_from_utf8(home).is_absolute();
}

struct hook_layout_t
{
    std::size_t root_close        = 0;
    std::size_t root_member_count = 0;
    std::size_t value_begin       = 0;
    std::size_t value_end         = 0;
    bool has_owned_member = false;
};

// A syntax-only numeric scan preserves valid user numbers outside the SDK's
// int64/double model. Strings still use its strict UTF-8/escape decoder.
class Hook_configuration_parser
{
public:
    explicit Hook_configuration_parser(std::string_view content)
    :
        m_content(content)
    {}

    std::optional<hook_layout_t> parse()
    {
        if (!addon::is_valid_utf8(m_content)) {
            return std::nullopt;
        }
        skip_whitespace();
        if (!consume('{') || !object(1, true)) {
            return std::nullopt;
        }
        skip_whitespace();
        return m_offset == m_content.size() ? std::optional(m_layout) : std::nullopt;
    }

private:
    bool consume(char token)
    {
        if (m_offset == m_content.size() || m_content[m_offset] != token) {
            return false;
        }
        ++m_offset;
        return true;
    }

    void skip_whitespace()
    {
        while (m_offset < m_content.size() &&
            (m_content[m_offset] == ' '  || m_content[m_offset] == '\t' ||
             m_content[m_offset] == '\r' || m_content[m_offset] == '\n'))
        {
            ++m_offset;
        }
    }

    std::optional<std::string> string()
    {
        const auto begin = m_offset;
        if (!consume('"')) {
            return std::nullopt;
        }
        while (m_offset < m_content.size()) {
            const char character = m_content[m_offset++];
            if (character == '"') {
                const auto decoded = addon::parse_json(m_content.substr(begin, m_offset - begin), {}, nullptr);
                return decoded ? std::optional(decoded->as_string()) : std::nullopt;
            }
            if (character == '\\') {
                if (m_offset == m_content.size()) {
                    return std::nullopt;
                }
                ++m_offset;
            }
        }
        return std::nullopt;
    }

    bool digits()
    {
        const auto begin = m_offset;
        while (m_offset < m_content.size() &&
            m_content[m_offset] >= '0' && m_content[m_offset] <= '9')
        {
            ++m_offset;
        }
        return m_offset != begin;
    }

    bool number()
    {
        consume('-');
        if (!consume('0')) {
            if (m_offset == m_content.size() ||
                m_content[m_offset] < '1' || m_content[m_offset] > '9')
            {
                return false;
            }
            digits();
        }
        if (consume('.') && !digits()) {
            return false;
        }
        if (consume('e') || consume('E')) {
            if (!consume('+')) {
                consume('-');
            }
            if (!digits()) {
                return false;
            }
        }
        return true;
    }

    bool value(std::size_t depth)
    {
        skip_whitespace();
        if (m_offset == m_content.size()) {
            return false;
        }
        switch (m_content[m_offset]) {
            case '{': ++m_offset; return object(depth + 1, false);
            case '[': ++m_offset; return array(depth + 1);
            case '"': return string().has_value();
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default:  return number();
        }
    }

    bool literal(std::string_view token)
    {
        if (m_content.substr(m_offset, token.size()) != token) {
            return false;
        }
        m_offset += token.size();
        return true;
    }

    bool object(std::size_t depth, bool root)
    {
        if (depth > addon::limits::k_max_json_depth) {
            return false;
        }
        std::set<std::string> names;
        skip_whitespace();
        if (m_offset < m_content.size() && m_content[m_offset] == '}') {
            if (root) {
                m_layout.root_close = m_offset;
            }
            ++m_offset;
            return true;
        }
        for (;;) {
            skip_whitespace();
            const auto name = string();
            if (!name || names.size() >= addon::limits::k_max_json_members || !names.insert(*name).second) {
                return false;
            }
            skip_whitespace();
            if (!consume(':')) {
                return false;
            }
            skip_whitespace();
            const auto begin = m_offset;
            if (!value(depth)) {
                return false;
            }
            if (root) {
                m_layout.root_member_count = names.size();
                if (*name == "logonomic-hook-relay") {
                    m_layout.has_owned_member = true;
                    m_layout.value_begin = begin;
                    m_layout.value_end = m_offset;
                }
            }
            skip_whitespace();
            if (m_offset < m_content.size() && m_content[m_offset] == '}') {
                if (root) {
                    m_layout.root_close = m_offset;
                }
                ++m_offset;
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    bool array(std::size_t depth)
    {
        if (depth > addon::limits::k_max_json_depth) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        for (;;) {
            if (!value(depth)) {
                return false;
            }
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    std::string_view m_content;
    std::size_t m_offset = 0;
    hook_layout_t m_layout;
};

void transform_hooks(addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_managed_configuration_transform_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (request->content.size() > k_max_hooks_bytes) {
        context.fail(addon::Result_status::FAILED, "hook_configuration_too_large");
        return;
    }
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    const auto supported = hook_definition(windows);
    const auto stanza    = addon::serialize_json(supported);
    std::string content;
    if (!request->present) {
        content = "{\"logonomic-hook-relay\":" + stanza + "}\n";
    }
    else {
        Hook_configuration_parser parser(request->content);
        const auto layout = parser.parse();
        if (!layout) {
            context.fail(addon::Result_status::FAILED, "invalid_hook_configuration");
            return;
        }
        content = request->content;
        if (layout->has_owned_member) {
            const auto installed = addon::parse_json(std::string_view(content).substr(
                layout->value_begin, layout->value_end - layout->value_begin), {}, nullptr);
            if (!installed || addon::serialize_json_canonical(*installed) !=
                addon::serialize_json_canonical(supported))
            {
                content.replace(layout->value_begin, layout->value_end - layout->value_begin, stanza);
            }
        }
        else {
            if (layout->root_member_count == addon::limits::k_max_json_members) {
                context.fail(addon::Result_status::FAILED, "invalid_hook_configuration");
                return;
            }
            content.insert(layout->root_close,
                std::string(layout->root_member_count == 0 ? "" : ",") + "\"logonomic-hook-relay\":" + stanza);
        }
    }
    if (content.size() > k_max_hooks_bytes) {
        context.fail(addon::Result_status::FAILED, "hook_configuration_too_large");
        return;
    }
    context.succeed(addon::encode_managed_configuration_transform_result({request->grant, std::move(content)}));
}

struct hook_delivery_t
{
    bool managed = false;
    bool lifecycle = false;
};

// The host commits the requested hook transform before enabling this launch.
// Lifecycle observations further need a verified native Antigravity that
// honours the observed hook contract; without it only titles are delivered.
// nullopt: the host cancelled the version probe.
std::optional<hook_delivery_t> hook_delivery(
    const addon::Launcher_request&                  launcher,
    const std::optional<addon::Launch_environment>& launch_environment,
    std::string_view                                working_directory,
    addon::Invocation_context&                      context)
{
    if (!hooks_manageable(launcher, launch_environment)) {
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
    if (hooks.managed) {
        set.emplace_back("AGY_LOGONOMIC_HOOKS_ENABLED", "1");
        set.emplace_back("AGY_LOGONOMIC_LIFECYCLE_ENABLED", hooks.lifecycle ? "1" : "0");
    }
}

bool prepare_initial_prompt(
    const addon::Prepare_request& request,
    addon::Launch_patch&           patch,
    addon::Invocation_context&    context)
{
    const auto unavailable = [&context] {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : addon::reason::k_initial_prompt_unavailable);
        return false;
    };
    const auto command = recognize_launcher(request.launcher.script_body);
    const auto& environment = *request.launch_environment;
    const bool windows = environment.platform_id == "windows-x86_64";
    static const std::vector<std::string_view> refused{
        "c", "continue", "conversation", "new-project", "p", "print", "prompt", "i", "prompt-interactive",
        "h", "help", "version"};
    if (!recognize_launcher(request.launcher_argv) || !command || !command->sole_command ||
        !literal_options(command->arguments, windows, refused) ||
        !std::all_of(command->environment.begin(), command->environment.end(),
            [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); }))
    {
        return unavailable();
    }
    const auto program = resolve_native_launcher(*command, environment.effective_environment, request.working_directory);
    if (!program) {
        return unavailable();
    }
    const auto help = addon::probe({*program, "--help"}, direct_environment(environment, *command),
        request.working_directory, 3000, context);
    if (!help || help->output.find("--prompt-interactive") == std::string::npos ||
        help->output.find("--add-dir") == std::string::npos)
    {
        return unavailable();
    }
    const auto directory = addon::path_from_utf8(request.initial_prompt->referenced_file).parent_path();
    // The replacement runs the native program without a shell, so neither
    // the brief path nor its message undergo shell quoting or expansion.
    patch.argv = std::vector<std::string>{*program};
    patch.argv->insert(patch.argv->end(), command->arguments.begin(), command->arguments.end());
    patch.argv->insert(patch.argv->end(), {"--add-dir", addon::path_to_utf8(directory),
        "--prompt-interactive=" + request.initial_prompt->text});
    for (const auto& [name, value] : command->environment) {
        addon::set_environment(patch.set, name, value);
    }
    disable_auto_updates(patch.set);
    patch.disposition->prepared = true;
    patch.initial_prompt = addon::Initial_prompt_acknowledgment{
        addon::initial_prompt_text_digest_hex(request.initial_prompt->text)};
    addon::Contract_failure failure;
    if (!addon::validate_launch_patch(patch, request.declared_assets, request.declared_assets, &failure)) {
        return unavailable();
    }
    return true;
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
    const bool recognized = recognize_launcher(request->launcher_argv).has_value();
    const auto hooks = recognized
        ? hook_delivery(request->launcher, request->launch_environment, request->working_directory, context)
        : hook_delivery_t{};
    if (!hooks) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (recognized) {
        disable_auto_updates(patch.set);
    }
    patch.disposition = addon::Launch_disposition{
        hooks->managed || !patch.set.empty(), hooks->lifecycle, hooks->managed};
    patch.apply_managed_configuration = hooks->managed;
    enable_hooks(patch.set, *hooks);
    if (request->initial_prompt && !prepare_initial_prompt(*request, patch, context)) {
        return;
    }
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
    // The host default record names the provider conversation.
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
    disable_auto_updates(result.patch.set);
    enable_hooks(result.patch.set, *hooks);
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    if (request->startup_observer) {
        if (request->startup_observer->confirmation != "observed-identity") {
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
    if (request.capability.capability_id == "managed-configuration" && request.capability.version == 3 &&
        request.operation == addon::k_managed_configuration_transform_operation)
    {
        transform_hooks(context);
    }
    else
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
