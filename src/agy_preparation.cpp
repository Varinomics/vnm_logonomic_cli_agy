#include "agy_preparation.h"
#include "agy_launcher.h"
#include "agy_resume.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/permission_snapshot.h>
#include <logonomic_cli_addon/session_contracts.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
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
constexpr std::string_view k_status_line_marker = "logonomic-agy-statusline-v1";

std::string shell_quote(std::string_view value)
{
    std::string result = "'";
    for (const char character : value) {
        result += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return result + '\'';
}

std::string powershell_command(std::string_view script)
{
    // Generated script text is ASCII; user text is embedded as UTF-8 base64.
    std::string utf16;
    for (const char character : script) {
        utf16 += character;
        utf16 += '\0';
    }
    return "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
        addon::encode_base64(utf16);
}

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
        return powershell_command(script);
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

std::string status_line_command(bool windows, std::string_view original_command)
{
    if (windows) {
        // A PowerShell pipeline transcodes native stdin. Feed each native
        // process through its byte stream instead and inherit user stdout.
        // Read the retained command from the same user-global settings file:
        // encoding it inside this script can exceed cmd.exe's command limit.
        // Select raw JSON members before decoding strings, so unrelated keys
        // and numbers do not acquire PowerShell or .NET serializer semantics.
        const std::string script =
            "# " + std::string(k_status_line_marker) + "\n" + R"ps(
$ErrorActionPreference = 'Stop'
function Read-Member($text, $name) {
  $depth = 0; $start = -1
  foreach ($m in [regex]::Matches($text, '(?<s>"(?:\\.|[^"\\])*")(?<key>\s*:)?|[{}\[\],]')) {
    $token = $m.Value
    if ($depth -eq 1) {
      if ($start -ge 0 -and ($token -eq ',' -or $token -eq '}')) {
        return $text.Substring($start, $m.Index - $start)
      }
      if ($m.Groups['key'].Success -and (ConvertFrom-Json $m.Groups['s'].Value) -ceq $name) {
        $start = $m.Index + $m.Length
      }
    }
    if ($token -eq '{' -or $token -eq '[') { ++$depth }
    if ($token -eq '}' -or $token -eq ']') { --$depth }
  }
  return 'null'
}
$settings = [IO.File]::ReadAllText(
  (Join-Path $env:USERPROFILE '.gemini/antigravity-cli/settings.json'))
$relay = Read-Member $settings 'logonomic-statusline-relay'
$original = Read-Member $relay 'original' | ConvertFrom-Json
$command = if ($original) { [string](Read-Member $original 'command' | ConvertFrom-Json) } else { '' }
if ($command.StartsWith('~')) { $command = $env:USERPROFILE + $command.Substring(1) }
$managed = ($env:LOGONOMIC_PROVIDER_GENERATION_NONCE -and
    $env:LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED -ceq $env:LOGONOMIC_PROVIDER_GENERATION_NONCE -and
    $env:LOGONOMIC_PROVIDER_HELPERS_DIR -and $env:LOGONOMIC_SESSION_CAPABILITY)
if ($managed) {
  $helper = Join-Path $env:LOGONOMIC_PROVIDER_HELPERS_DIR 'agy_helper.exe'
  $managed = Test-Path -LiteralPath $helper -PathType Leaf
}
function Start-Callback($program, $arguments, $redirect) {
  $info = New-Object Diagnostics.ProcessStartInfo
  $info.FileName = $program
  $info.Arguments = $arguments
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardInput = $redirect
  return [Diagnostics.Process]::Start($info)
}
if (-not $managed -and $command.Length -eq 0) { exit 0 }
# Process.Start's text writer must not prefix the byte stream with a UTF-8 BOM.
if ([Console]::InputEncoding.CodePage -eq 65001) {
  [Console]::InputEncoding = New-Object Text.UTF8Encoding($false)
}
$bytes = New-Object IO.MemoryStream
[Console]::OpenStandardInput().CopyTo($bytes)
$user = $null
if ($command.Length -ne 0) {
  $user = Start-Callback $env:ComSpec ('/d /s /c "' + $command + '"') $true
}
$observer = $null
if ($managed) { $observer = Start-Callback $helper 'StatusLine' $true }
foreach ($process in @($observer, $user)) {
  if ($process) {
    try { $bytes.Position = 0; $bytes.CopyTo($process.StandardInput.BaseStream) } catch {}
    $process.StandardInput.Close()
  }
}
if ($observer) { $observer.WaitForExit() }
if ($user) { $user.WaitForExit(); exit $user.ExitCode }
exit 0
)ps";
        return powershell_command(script);
    }
    const std::string script =
        "command=$1; case $command in '~'*) command=\"$HOME${command#?}\";; esac; "
        "if [ -n \"$LOGONOMIC_PROVIDER_GENERATION_NONCE\" ] && "
        "[ \"$LOGONOMIC_PERMISSION_SNAPSHOT_ENABLED\" = \"$LOGONOMIC_PROVIDER_GENERATION_NONCE\" ] && "
        "[ -n \"$LOGONOMIC_PROVIDER_HELPERS_DIR\" ] && [ -n \"$LOGONOMIC_SESSION_CAPABILITY\" ] && "
        "[ -x \"$LOGONOMIC_PROVIDER_HELPERS_DIR/agy_helper\" ]; then "
        "if [ -n \"$command\" ]; then "
        "\"$LOGONOMIC_PROVIDER_HELPERS_DIR/agy_helper\" StatusLine --pass-through | sh -c \"$command\"; "
        "else \"$LOGONOMIC_PROVIDER_HELPERS_DIR/agy_helper\" StatusLine; fi; "
        "elif [ -n \"$command\" ]; then exec sh -c \"$command\"; fi";
    return "sh -c " + shell_quote(script) + " " + std::string(k_status_line_marker) + " " +
        shell_quote(original_command);
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
class Configuration_parser
{
public:
    explicit Configuration_parser(std::string_view content, std::string_view member = "logonomic-hook-relay")
    :
        m_content(content),
        m_member(member)
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
                if (*name == m_member) {
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
    std::string_view m_member;
    std::size_t m_offset = 0;
    hook_layout_t m_layout;
};

std::optional<std::string> member_bytes(std::string_view content, std::string_view name)
{
    const auto layout = Configuration_parser(content, name).parse();
    return layout && layout->has_owned_member ?
        std::optional(std::string(content.substr(layout->value_begin, layout->value_end - layout->value_begin))) :
        std::nullopt;
}

std::optional<addon::Json_value> member(std::string_view content, std::string_view name)
{
    const auto layout = Configuration_parser(content, name).parse();
    if (!layout || !layout->has_owned_member) {
        return std::nullopt;
    }
    return addon::parse_json(content.substr(layout->value_begin, layout->value_end - layout->value_begin), {}, nullptr);
}

bool replace_member(std::string& content, std::string_view name, std::string_view value)
{
    const auto layout = Configuration_parser(content, name).parse();
    if (!layout) {
        return false;
    }
    if (layout->has_owned_member) {
        const auto installed = member(content, name);
        const auto replacement = addon::parse_json(value, {}, nullptr);
        if (!installed || !replacement || addon::serialize_json_canonical(*installed) !=
            addon::serialize_json_canonical(*replacement))
        {
            content.replace(layout->value_begin, layout->value_end - layout->value_begin, value);
        }
    }
    else {
        if (layout->root_member_count == addon::limits::k_max_json_members) {
            return false;
        }
        content.insert(layout->root_close, std::string(layout->root_member_count == 0 ? "" : ",") +
            addon::serialize_json(addon::Json_value(std::string(name))) + ":" + std::string(value));
    }
    return true;
}

bool status_line_has_command(std::string_view content)
{
    const auto command = member(content, "command");
    return command && command->is_string() && !command->as_string().empty();
}

std::optional<std::string> wrap_status_line(
    const std::optional<std::string>& original, std::string_view command)
{
    std::string wrapped = original && Configuration_parser(*original).parse() ? *original : "{}";
    if (!replace_member(wrapped, "command", addon::serialize_json(addon::Json_value(std::string(command)))) ||
        ((!original || !status_line_has_command(*original)) && !replace_member(wrapped, "stack_with_default", "true")))
    {
        return std::nullopt;
    }
    return wrapped;
}

bool managed_status_line(std::string_view command)
{
    if (command.starts_with("sh -c ")) {
        return command.find(" " + std::string(k_status_line_marker) + " '") != std::string_view::npos;
    }
    const auto prefix = powershell_command("");
    if (!command.starts_with(prefix)) {
        return false;
    }
    const auto decoded = addon::decode_base64(command.substr(prefix.size()));
    std::string marker;
    for (const char character : "# " + std::string(k_status_line_marker)) {
        marker += character;
        marker += '\0';
    }
    return decoded && decoded->starts_with(marker);
}

} // namespace

std::optional<std::string> transform_status_line(
    bool present, std::string_view source, bool windows, std::string& reason)
{
    const auto fail = [&reason](const char* failure) -> std::optional<std::string> {
        reason = failure;
        return std::nullopt;
    };
    if (source.size() > k_max_hooks_bytes) {
        return fail("status_line_configuration_too_large");
    }
    std::string content = present ? std::string(source) : "{}\n";
    if (!Configuration_parser(content).parse()) {
        return fail("invalid_status_line_configuration");
    }
    auto original = member_bytes(content, "statusLine");
    const auto current_command = original ? member(*original, "command") : std::nullopt;
    const std::string installed_command = current_command && current_command->is_string() ?
        current_command->as_string() : "";
    const auto retained = member(content, "logonomic-statusline-relay");
    const auto* version = retained ? retained->find("version") : nullptr;
    const auto* previous = retained ? retained->find("original") : nullptr;
    const auto* command = retained ? retained->find("command") : nullptr;
    const bool recognized = retained && retained->is_object() && retained->as_object().size() == 3 &&
        version && version->is_integer() && version->as_integer() == 1 &&
        previous && (previous->is_string() || previous->is_null()) && command && command->is_string();
    if (recognized && installed_command == command->as_string()) {
        const auto saved = previous->is_string() ? std::optional(previous->as_string()) : std::nullopt;
        const auto expected = wrap_status_line(saved, command->as_string());
        if (original == expected) {
            original = saved;
        }
        else {
            // Native enable/disable and display-option edits belong to the
            // user even when the retained command was disabled or absent.
            const auto saved_command = saved ? member(*saved, "command") : std::nullopt;
            if (!original || !replace_member(*original, "command",
                addon::serialize_json(saved_command.value_or(addon::Json_value("")))))
            {
                return fail("invalid_status_line_configuration");
            }
            if (!saved || !status_line_has_command(*saved)) {
                const auto stacked = member(*original, "stack_with_default");
                if (stacked && stacked->is_boolean() && stacked->as_boolean()) {
                    const auto prior = saved ? member_bytes(*saved, "stack_with_default") : std::nullopt;
                    if (!replace_member(*original, "stack_with_default", prior.value_or("false"))) {
                        return fail("invalid_status_line_configuration");
                    }
                }
            }
        }
    }
    else
    if (managed_status_line(installed_command)) {
        // Without the matching retained original, wrapping again would recurse.
        return fail("invalid_status_line_relay");
    }

    const bool object = original && Configuration_parser(*original).parse().has_value();
    const auto scalar = original && !object ? addon::parse_json(*original, {}, nullptr) : std::nullopt;
    if (original && !object && (!scalar || (!scalar->is_boolean() && !scalar->is_null()))) {
        return fail("invalid_status_line_configuration");
    }
    const auto original_command = original ? member(*original, "command") : std::nullopt;
    const auto original_enabled = original ? member(*original, "enabled") : std::nullopt;
    const auto original_type = original ? member(*original, "type") : std::nullopt;
    if ((original && member_bytes(*original, "command") && (!original_command || !original_command->is_string() ||
            original_command->as_string().find('\0') != std::string::npos)) ||
        (original && member_bytes(*original, "enabled") && (!original_enabled || !original_enabled->is_boolean())) ||
        (original && member_bytes(*original, "type") && (!original_type || !original_type->is_string() ||
            original_type->as_string() != "command")))
    {
        return fail("invalid_status_line_configuration");
    }
    const bool has_command = original && status_line_has_command(*original);
    const auto replacement = status_line_command(windows, has_command ? original_command->as_string() : "");
    // cmd.exe accepts at most 8191 characters, including its invocation.
    // The Windows wrapper is ASCII; reserve room for the native shell prefix.
    // POSIX also stays below Linux's 128-KiB per-argument limit, including NUL.
    if (replacement.size() > (windows ? 8000u : 128u * 1024u - 1u)) {
        return fail("status_line_command_too_long");
    }
    const auto wrapped = wrap_status_line(original, replacement);
    if (!wrapped) {
        return fail("invalid_status_line_configuration");
    }
    auto retention = addon::Json_value::object();
    retention.set("version", addon::Json_value(1));
    retention.set("original", original ? addon::Json_value(*original) : addon::Json_value());
    retention.set("command", addon::Json_value(replacement));
    if (!replace_member(content, "statusLine", *wrapped) ||
        !replace_member(content, "logonomic-statusline-relay", addon::serialize_json(retention)))
    {
        return fail("invalid_status_line_configuration");
    }
    if (content.size() > k_max_hooks_bytes) {
        return fail("status_line_configuration_too_large");
    }
    reason.clear();
    return content;
}

namespace {

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
        Configuration_parser parser(request->content);
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

void transform_settings(addon::Invocation_context& context)
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
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    std::string reason;
    auto content = transform_status_line(request->present, request->content, windows, reason);
    if (!content) {
        context.fail(reason == "status_line_command_too_long" ? addon::Result_status::UNAVAILABLE :
            addon::Result_status::FAILED, std::move(reason));
        return;
    }
    context.succeed(addon::encode_managed_configuration_transform_result({request->grant, std::move(*content)}));
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

enum class Initial_prompt_guard
{
    PREPARE_REQUEST,
    BASE_PREPARATION,
    LAUNCHER_ARGV,
    LAUNCHER_SCRIPT,
    SOLE_COMMAND,
    LITERAL_OPTIONS,
    LITERAL_ENVIRONMENT,
    NATIVE_RESOLUTION,
    HELP_PROBE,
    HELP_FEATURES,
    LAUNCH_PATCH,
};

const char* guard_name(Initial_prompt_guard guard)
{
    switch (guard) {
        case Initial_prompt_guard::PREPARE_REQUEST:     return "prepare_request";
        case Initial_prompt_guard::BASE_PREPARATION:    return "base_preparation";
        case Initial_prompt_guard::LAUNCHER_ARGV:       return "launcher_argv";
        case Initial_prompt_guard::LAUNCHER_SCRIPT:     return "launcher_script";
        case Initial_prompt_guard::SOLE_COMMAND:        return "sole_command";
        case Initial_prompt_guard::LITERAL_OPTIONS:     return "literal_options";
        case Initial_prompt_guard::LITERAL_ENVIRONMENT: return "literal_environment";
        case Initial_prompt_guard::NATIVE_RESOLUTION:   return "native_resolution";
        case Initial_prompt_guard::HELP_PROBE:          return "help_probe";
        case Initial_prompt_guard::HELP_FEATURES:       return "help_features";
        case Initial_prompt_guard::LAUNCH_PATCH:        return "launch_patch";
        default:                                        return "unknown";
    }
}

void record_initial_prompt_failure(
    Initial_prompt_guard                     guard,
    const char*                              phase,
    std::chrono::steady_clock::time_point     started,
    const std::optional<addon::Probe_output>* probe = nullptr,
    std::string_view                         contract_failure = {}) noexcept
{
    // Keep diagnostics out of protocol results, stderr, and GUI logs. Only the
    // latest failure lives in the generation scratch directory owned by the host.
    try {
#ifdef _WIN32
        const wchar_t* scratch = _wgetenv(L"LOGONOMIC_ADDON_SCRATCH_DIR");
#else
        const char* scratch = std::getenv("LOGONOMIC_ADDON_SCRATCH_DIR");
#endif
        if (!scratch || !*scratch) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        const bool present = probe && probe->has_value();
        auto record = addon::Json_value::object();
        record.set("guard", addon::Json_value(guard_name(guard)));
        record.set("phase", addon::Json_value(phase));
        record.set("elapsed_ms", addon::Json_value(static_cast<std::int64_t>(elapsed)));
        record.set("probe_present", probe ? addon::Json_value(present) : addon::Json_value{});
        // A missing optional does not distinguish timeout, spawn, exit, or
        // custody failure. The SDK exposes no native exit status here.
        record.set("probe_failure", probe && !present ? addon::Json_value("unknown") : addon::Json_value{});
        record.set("exit_status", addon::Json_value{});
        record.set("output_bytes", present
            ? addon::Json_value(static_cast<std::uint64_t>((*probe)->output.size())) : addon::Json_value{});
        record.set("error_bytes", present
            ? addon::Json_value(static_cast<std::uint64_t>((*probe)->errors.size())) : addon::Json_value{});
        for (const auto& [name, option] : {
                std::pair{"prompt_interactive", "--prompt-interactive"}, std::pair{"add_dir", "--add-dir"}})
        {
            record.set(name, present ? addon::Json_value(
                (*probe)->output.find(option) != std::string::npos || (*probe)->errors.find(option) != std::string::npos)
                : addon::Json_value{});
        }
        record.set("contract_failure", contract_failure.empty()
            ? addon::Json_value{} : addon::Json_value(contract_failure));
        const auto content = addon::serialize_json(record) + '\n';
        const auto root    = std::filesystem::path(scratch);
        const auto target  = root / "agy-initial-prompt-failure.json";
        const auto pending = root / "agy-initial-prompt-failure.json.tmp";
        static std::mutex writer_mutex;
        const std::lock_guard lock(writer_mutex);
#ifdef _WIN32
        FILE* file = _wfopen(pending.c_str(), L"wb");
#else
        FILE* file = std::fopen(pending.c_str(), "wb");
#endif
        if (!file) {
            return;
        }
        const bool written = std::fwrite(content.data(), 1, content.size(), file) == content.size();
        const bool closed  = std::fclose(file) == 0;
        if (written && closed) {
#ifdef _WIN32
            MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
            std::rename(pending.c_str(), target.c_str());
#endif
        }
#ifdef _WIN32
        DeleteFileW(pending.c_str());
#else
        std::remove(pending.c_str());
#endif
    }
    catch (...) {
        // Diagnostics must not change preparation or surface an I/O failure.
    }
}

bool prepare_initial_prompt(
    const addon::Prepare_request&         request,
    addon::Launch_patch&                   patch,
    addon::Invocation_context&            context,
    std::chrono::steady_clock::time_point started)
{
    const auto unavailable = [
            &context,
            started
        ](
            Initial_prompt_guard guard,
            const std::optional<addon::Probe_output>* probe = nullptr,
            std::string_view contract_failure = {})
        {
            record_initial_prompt_failure(guard, "initial_prompt", started, probe, contract_failure);
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
    if (!recognize_launcher(request.launcher_argv)) {
        return unavailable(Initial_prompt_guard::LAUNCHER_ARGV);
    }
    if (!command) {
        return unavailable(Initial_prompt_guard::LAUNCHER_SCRIPT);
    }
    if (!command->sole_command) {
        return unavailable(Initial_prompt_guard::SOLE_COMMAND);
    }
    if (!literal_options(command->arguments, windows, refused)) {
        return unavailable(Initial_prompt_guard::LITERAL_OPTIONS);
    }
    if (!std::all_of(command->environment.begin(), command->environment.end(),
            [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); }))
    {
        return unavailable(Initial_prompt_guard::LITERAL_ENVIRONMENT);
    }
    const auto program = resolve_native_launcher(*command, environment.effective_environment, request.working_directory);
    if (!program) {
        return unavailable(Initial_prompt_guard::NATIVE_RESOLUTION);
    }
    const auto help = addon::probe({*program, "--help"}, direct_environment(environment, *command),
        request.working_directory, 3000, context);
    const auto help_text = help ? help->output + '\n' + help->errors : std::string{};
    if (!help) {
        return unavailable(Initial_prompt_guard::HELP_PROBE, &help);
    }
    if (help_text.find("--prompt-interactive") == std::string::npos ||
        help_text.find("--add-dir") == std::string::npos)
    {
        return unavailable(Initial_prompt_guard::HELP_FEATURES, &help);
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
        return unavailable(Initial_prompt_guard::LAUNCH_PATCH, &help, failure.reason_code);
    }
    return true;
}

std::optional<std::string> selection_bridge()
{
#ifdef _WIN32
    const auto* root = _wgetenv(L"LOGONOMIC_ADDON_PACKAGE_ROOT");
    const auto path = root ? std::filesystem::path(root) / "helpers" / "agy_resume_bridge.exe" : std::filesystem::path{};
#else
    const auto* root = std::getenv("LOGONOMIC_ADDON_PACKAGE_ROOT");
    const auto path = root ? addon::path_from_utf8(root) / "helpers" / "agy_resume_bridge" : std::filesystem::path{};
#endif
    std::error_code error;
    if (!root || !path.is_absolute() || !std::filesystem::is_regular_file(path, error) || error) return std::nullopt;
    return addon::path_to_utf8(path);
}

void observe_native_selection(const addon::Prepare_request& request, addon::Launch_patch& patch)
{
    const auto bridge = selection_bridge();
    const auto command = recognize_launcher(request.launcher.script_body);
    const bool windows = request.launch_environment->platform_id == "windows-x86_64";
    static const std::vector<std::string_view> refused{"log-file", "p", "print", "prompt", "h", "help", "version"};
    if (!bridge || !command || !command->sole_command ||
        !literal_options(command->arguments, windows, refused) || literal_switch(command->arguments, "remote-agent") ||
        !std::all_of(command->environment.begin(), command->environment.end(),
            [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); }))
    {
        return;
    }
    const auto program = resolve_native_launcher(*command, request.launch_environment->effective_environment,
        request.working_directory);
    if (!program) return;
    auto observed = patch;
    if (!observed.argv) {
        observed.argv = std::vector<std::string>{*program};
        observed.argv->insert(observed.argv->end(), command->arguments.begin(), command->arguments.end());
    }
    observed.argv->insert(observed.argv->begin(), *bridge);
    for (const auto& [name, value] : command->environment) addon::set_environment(observed.set, name, value);
    addon::set_environment(observed.set, "AGY_LOGONOMIC_OBSERVE_SELECTION", "1");
    addon::Contract_failure failure;
    if (addon::validate_launch_patch(observed, request.declared_assets, request.declared_assets, &failure)) {
        patch = std::move(observed);
    }
}

void prepare_launch(addon::Invocation_context& context)
{
    const auto started = std::chrono::steady_clock::now();
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) {
        if (context.request().payload.find(addon::k_initial_prompt_member)) {
            record_initial_prompt_failure(Initial_prompt_guard::PREPARE_REQUEST,
                "decode", started, nullptr, failure.reason_code);
        }
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
        if (request->initial_prompt) {
            record_initial_prompt_failure(Initial_prompt_guard::BASE_PREPARATION, "hooks", started);
        }
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
    if (request->initial_prompt && !prepare_initial_prompt(*request, patch, context, started)) {
        return;
    }
    if (hooks->lifecycle) observe_native_selection(*request, patch);
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

void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto identity = decode_resume_identity(request->resume_record);
    if (!identity) {
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
    const auto inspection = inspect_resume(*identity, request->context, [&context] { return context.cancel_requested(); });
    if (!resume_scope_matches(*identity, request->context)) {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_scope_mismatch");
        return;
    }
    if (context.cancel_requested() || inspection.availability == addon::Resume_availability::ABSENT) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "resume_conversation_absent");
        return;
    }
    // Optional inspection cannot veto an exact native resume on UNKNOWN.
    // The native selector and, when offered, loaded-identity startup observer
    // still validate the actual operation (including remote project storage).
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
    result.process.argv.insert(result.process.argv.end(), {"--conversation", identity->provider_session_id});
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.set = command->environment;
    disable_auto_updates(result.patch.set);
    enable_hooks(result.patch.set, *hooks);
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    if (hooks->lifecycle && !literal_switch(command->arguments, "remote-agent")) {
#ifdef _WIN32
        result.process.executable = "helpers/agy_resume_bridge.exe";
#else
        result.process.executable = "helpers/agy_resume_bridge";
#endif
        addon::set_environment(result.patch.set, "AGY_LOGONOMIC_OBSERVE_SELECTION", "1");
    }
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

void inspect_conversation(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_inspect_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto identity = decode_resume_identity(request->resume_record);
    if (!identity) {
        context.fail(addon::Result_status::FAILED, addon::reason::k_resume_record_invalid);
        return;
    }
    auto result = inspect_resume(*identity, request->context, [&context] { return context.cancel_requested(); });
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    result.request_digest_hex = addon::resume_inspect_request_digest_hex(*request);
    context.succeed(addon::encode_resume_inspect_result(result, context.request().capability.version));
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
    if (request.capability.capability_id == addon::k_permission_snapshot_capability_id &&
        request.capability.version == addon::k_permission_snapshot_version &&
        request.operation == addon::k_managed_configuration_transform_operation)
    {
        transform_settings(context);
    }
    else
    if (request.capability.capability_id == "managed-configuration" && request.capability.version == 3 &&
        request.operation == addon::k_managed_configuration_transform_operation)
    {
        transform_hooks(context);
    }
    else
    if (request.capability.capability_id == "interactive-resume" &&
        (request.capability.version == 3 || request.capability.version == 4) &&
        request.operation == addon::k_inspect_resume_operation)
    {
        inspect_conversation(context);
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
