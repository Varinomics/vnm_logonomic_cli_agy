#include "agy_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <utility>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

using addon::lower;
using addon::set_environment;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;

using addon::Environment;
using addon::Launcher_whitespace;
namespace fs = std::filesystem;

bool shell_program(std::string_view base)
{
    return base == "cmd" || base == "powershell" || base == "pwsh" || base == "bash" || base == "sh";
}

bool shell_command_option(std::string_view argument)
{
    return argument == "/c" || argument == "-c" || argument == "-lc" || argument == "-command";
}

// Native replacements need the words Bash passes to export, with quoting and
// escapes removed. Dynamic values still follow the literal-assignment policy.
std::optional<Environment> export_assignments(std::string_view operands)
{
    Environment result;
    std::size_t position = 0;
    while (position < operands.size()) {
        if (operands[position] == ' ' || operands[position] == '\t') {
            ++position;
            continue;
        }
        std::string word;
        char quote = 0;
        for (; position < operands.size(); ++position) {
            const char character = operands[position];
            if (!quote && (character == ' ' || character == '\t')) break;
            if (character == '\\' && quote != '\'') {
                if (++position == operands.size()) {
                    return std::nullopt;
                }
                const char escaped = operands[position];
                if (quote == '"' && std::string_view("$`\"\\").find(escaped) == std::string_view::npos) {
                    word += '\\';
                }
                word += escaped;
                continue;
            }
            if (quote && character == quote) {
                quote = 0;
                continue;
            }
            if (!quote && (character == '\'' || character == '"')) {
                quote = character;
                continue;
            }
            if (quote != '\'' && (character == '`' ||
                (character == '$' && position + 1 < operands.size() && operands[position + 1] == '(')))
            {
                return std::nullopt;
            }
            if (!quote && std::string_view("<>()").find(character) != std::string_view::npos) {
                return std::nullopt;
            }
            word += character;
        }
        const auto separator = word.find('=');
        if (quote || separator == std::string::npos) {
            return std::nullopt;
        }
        const auto assignment = addon::environment_assignment(word, Launcher_whitespace::ASCII);
        if (!assignment || assignment->first != word.substr(0, separator)) {
            return std::nullopt;
        }
        result.emplace_back(assignment->first, word.substr(separator + 1));
    }
    return result.empty() ? std::nullopt : std::optional(std::move(result));
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::optional<Launcher_command> found;
    bool other_command = false;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = addon::trim(line, Launcher_whitespace::ASCII);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : addon::launcher_segments(line, Launcher_whitespace::ASCII)) {
            if (segment == "export" || segment.starts_with("export ") || segment.starts_with("export\t")) {
                const auto assignments = export_assignments(std::string_view(segment).substr(6));
                if (!assignments) return std::nullopt;
                if (!found) inherited.insert(inherited.end(), assignments->begin(), assignments->end());
                continue;
            }
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = addon::environment_assignment(
                segment.substr(4), Launcher_whitespace::ASCII);
            else
            if (normalized.starts_with("$env:")) standalone = addon::environment_assignment(
                segment.substr(5), Launcher_whitespace::ASCII);
            if (standalone) {
                if (!found) inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), addon::unquote(match[2].str(), Launcher_whitespace::ASCII));
                segment.erase(0, match.length());
            }
            const auto words = addon::launcher_tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = addon::environment_assignment(words[index], Launcher_whitespace::ASCII);
                const auto prefix = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                if (value) environment.push_back(*value);
                else
                if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            // The first invocation is the launcher's; any other command beside
            // it only disqualifies the script from being its sole command.
            if (found) {
                other_command = true;
                continue;
            }
            const std::string program = addon::unquote(words[index], Launcher_whitespace::ASCII);
            const std::string base = addon::base_name(program, Launcher_whitespace::ASCII);
            if (base == "agy") {
                found = Launcher_command{program, environment, {}, true};
                for (++index; index < words.size(); ++index) found->arguments.push_back(addon::unquote(words[index], Launcher_whitespace::ASCII));
                continue;
            }
            if (shell_program(base)) {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                    if (!shell_command_option(argument)) continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    found = recognize(addon::unquote(tail, Launcher_whitespace::ASCII), depth - 1);
                    if (found) {
                        found->environment.insert(found->environment.begin(), environment.begin(), environment.end());
                    }
                    break;
                }
            }
            if (!found) other_command = true;
        }
    }
    if (found && other_command) found->sole_command = false;
    return found;
}

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 8);
}

std::optional<Launcher_command> recognize_launcher(const std::vector<std::string>& argv)
{
    if (argv.empty()) return std::nullopt;
    const std::string base = addon::base_name(argv.front(), Launcher_whitespace::ASCII);
    if (base == "agy") {
        return Launcher_command{argv.front(), {}, {argv.begin() + 1, argv.end()}, true};
    }
    if (shell_program(base)) {
        for (std::size_t index = 1; index + 1 < argv.size(); ++index) {
            if (!shell_command_option(lower(argv[index]))) continue;
            // Runner arguments already contain the complete script operand.
            // Requoting them as script text loses embedded newlines and quotes.
            std::string script = argv[++index];
            for (++index; index < argv.size(); ++index) {
                script += ' ';
                script += argv[index];
            }
            return recognize(script, 7);
        }
    }
    return std::nullopt;
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    Environment environment;
    for (const auto& member : request.ambient_environment.as_object()) {
        environment.emplace_back(member.name, member.value.as_string());
    }
    const auto selected = resolve_program("agy", environment, {}, Launcher_whitespace::ASCII);
    if (!selected) {
        return std::nullopt;
    }
    const auto runner_key = addon::preferred_discovery_runner(
        *selected,
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct);
    return addon::Launcher_candidate{
        "interactive", "Antigravity", "agy", runner_key,
        "", true, addon::Json_value::object(),
    };
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!addon::is_semantic_version(version)) return false;
    std::array<std::string_view, 3> components;
    auto remaining = version.substr(0, version.find_first_of("-+"));
    for (auto& component : components) {
        const auto separator = remaining.find('.');
        component = remaining.substr(0, separator);
        if (separator != std::string_view::npos) remaining.remove_prefix(separator + 1);
    }
    const std::array minima{major, minor, patch};
    for (std::size_t index = 0; index < minima.size(); ++index) {
        const auto minimum = std::to_string(minima[index]);
        if (components[index].size() != minimum.size()) return components[index].size() > minimum.size();
        if (components[index] != minimum) return components[index] > minimum;
    }
    const auto suffix = version.find_first_of("-+");
    return suffix == std::string_view::npos || version[suffix] != '-';
}

std::string parse_version_output(std::string_view output, std::string_view errors)
{
    constexpr std::string_view labels[] = {"Antigravity", "agy"};
    const auto first  = addon::parse_version_stream(output, labels, Launcher_whitespace::ASCII);
    const auto second = addon::parse_version_stream(errors, labels, Launcher_whitespace::ASCII);
    if (!first || !second || (!first->empty() && !second->empty() && *first != *second)) return {};
    return first->empty() ? *second : *first;
}

std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request& launcher,
    const addon::Json_value& effective_environment,
    std::string_view working_directory,
    addon::Invocation_context& context)
{
    const auto command = recognize_launcher(launcher.script_body);
    if (!command) {
        return std::nullopt;
    }
    Environment environment;
    for (const auto& member : effective_environment.as_object()) {
        set_environment(environment, member.name, member.value.as_string());
    }
    for (const auto& [name, value] : command->environment) {
        // Shell interpolation is not executable identity evidence.
        if (value.find_first_of("$%`") != std::string::npos) {
            return std::nullopt;
        }
        set_environment(environment, name, value);
    }
    disable_auto_updates(environment);
    const auto selected = resolve_program(
        command->program, environment, working_directory, Launcher_whitespace::ASCII);
    if (!selected || !native_image(*selected)) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = fs::file_size(*selected, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = fs::last_write_time(*selected, error);
    if (error) {
        return std::nullopt;
    }
    const std::vector<std::string> argv{path_to_utf8(*selected), "--version"};
    addon::Sha256 digest;
    const auto append = [&digest](std::string_view value) {
        digest.update(std::to_string(value.size()));
        digest.update(":");
        digest.update(value);
    };
    append(working_directory);
    append(argv.front());
    append(std::to_string(size));
    append(addon::file_time_ticks_text(modified));
    for (const auto& [name, value] : environment) {
        append(name);
        append(value);
    }
    const auto cache_key = addon::to_hex(digest.finish());
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard lock(cache_mutex);
        const auto cached = cache.find(cache_key);
        if (cached != cache.end() && !context.cancel_requested()) {
            return Verified_launch{argv.front(), cached->second};
        }
    }
    const auto output = addon::probe(argv, environment, working_directory, 3000, context);
    if (!output || fs::file_size(*selected, error) != size || error ||
        fs::last_write_time(*selected, error) != modified || error)
    {
        return std::nullopt;
    }
    const auto version = parse_version_output(output->output, output->errors);
    if (version.empty()) {
        return std::nullopt;
    }
    {
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 32) {
            cache.erase(cache.begin());
        }
        cache[cache_key] = version;
    }
    return Verified_launch{argv.front(), version};
}

std::optional<std::string> resolve_native_launcher(
    const Launcher_command& command,
    const addon::Json_value& effective_environment,
    std::string_view working_directory)
{
    Environment environment;
    for (const auto& member : effective_environment.as_object()) {
        set_environment(environment, member.name, member.value.as_string());
    }
    for (const auto& [name, value] : command.environment) {
        set_environment(environment, name, value);
    }
    const auto program = resolve_program(
        command.program, environment, working_directory, Launcher_whitespace::ASCII);
    return program && native_image(*program) ? std::optional(path_to_utf8(*program)) : std::nullopt;
}

bool literal_assignment(std::string_view value, bool windows)
{
    return std::none_of(value.begin(), value.end(), [windows](unsigned char character) {
        return character < 0x20 || character == 0x7f || (!windows && character == '\\') ||
            std::string_view("$%`~!^*?[]{}\"'").find(static_cast<char>(character)) != std::string_view::npos;
    });
}

namespace {

// A word the direct invocation receives exactly as the launcher's shell would
// have passed it.
bool literal_word(std::string_view word, bool windows)
{
    return !word.empty() && std::all_of(word.begin(), word.end(), [windows](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == ':' || character == '/' || character == '=' || character == '+' ||
            (windows && character == '\\');
    });
}

// agy's options that take the next word as their value.
const std::vector<std::string_view>& valued_options()
{
    static const std::vector<std::string_view> valued{
        "add-dir", "agent", "conversation", "effort", "input-format", "json-schema", "log-file", "mode", "model",
        "output-format", "print-timeout", "project"};
    return valued;
}

} // namespace

bool literal_options(
    const std::vector<std::string>&      arguments,
    bool                                 windows,
    const std::vector<std::string_view>& refused)
{
    const auto& valued = valued_options();
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string_view option = arguments[index];
        if (!literal_word(option, windows) || !option.starts_with('-')) {
            return false;
        }
        option.remove_prefix(option.starts_with("--") ? 2 : 1);
        const auto separator = option.find('=');
        const auto name = option.substr(0, separator);
        if (name.empty() || name.starts_with('-') ||
            std::find(refused.begin(), refused.end(), name) != refused.end())
        {
            return false;
        }
        if (separator == std::string_view::npos &&
            std::find(valued.begin(), valued.end(), name) != valued.end() &&
            (++index == arguments.size() || !literal_word(arguments[index], windows)))
        {
            return false;
        }
    }
    return true;
}

bool literal_switch(const std::vector<std::string>& arguments, std::string_view name)
{
    const auto& valued = valued_options();
    bool set = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string_view option = arguments[index];
        option.remove_prefix(option.starts_with("--") ? 2 : 1);
        const auto separator = option.find('=');
        const auto word = option.substr(0, separator);
        if (word == name) {
            const auto value = separator == std::string_view::npos
                ? std::string_view("true") : option.substr(separator + 1);
            set = value == "1" || value == "t" || value == "T" ||
                value == "true" || value == "TRUE" || value == "True";
        }
        else
        if (separator == std::string_view::npos && std::find(valued.begin(), valued.end(), word) != valued.end()) {
            ++index;
        }
    }
    return set;
}

void disable_auto_updates(std::vector<std::pair<std::string, std::string>>& environment)
{
#ifdef _WIN32
    addon::set_environment(environment, "AGY_CLI_DISABLE_AUTO_UPDATE", "true");
#else
    (void)environment;
#endif
}

std::vector<std::pair<std::string, std::string>> direct_environment(
    const addon::Launch_environment& frozen,
    const Launcher_command&          command)
{
    const bool windows = frozen.platform_id == "windows-x86_64";
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& member : frozen.effective_environment.as_object()) {
        result.emplace_back(member.name, member.value.as_string());
    }
    for (const auto& [name, value] : command.environment) {
        std::erase_if(result, [&name, windows](const auto& entry) {
            if (!windows) {
                return entry.first == name;
            }
            return entry.first.size() == name.size() && std::equal(entry.first.begin(), entry.first.end(),
                name.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        });
        result.emplace_back(name, value);
    }
    disable_auto_updates(result);
    return result;
}

} // namespace agy
