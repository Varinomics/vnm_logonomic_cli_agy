#include "agy_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

std::string trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last  = value.find_last_not_of(" \t\r\n");
    return first == std::string_view::npos
        ? std::string{} : std::string(value.substr(first, last - first + 1));
}

std::string unquote(std::string_view value)
{
    std::string result = trim(value);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value)
{
    std::string name = lower(unquote(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

bool environment_name(std::string_view name)
{
    if (name.empty() || (name.front() != '_' &&
        !std::isalpha(static_cast<unsigned char>(name.front())))) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character == '_' || (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
    });
}

std::optional<std::pair<std::string, std::string>> assignment(std::string_view text)
{
    const std::string value = unquote(text);
    const auto separator = value.find('=');
    if (separator == std::string::npos) return std::nullopt;
    const std::string name = trim(value.substr(0, separator));
    if (!environment_name(name) || value.find('\0') != std::string::npos) return std::nullopt;
    return std::pair{name, unquote(value.substr(separator + 1))};
}

std::vector<std::string> tokens(std::string_view text)
{
    static const std::regex pattern(R"re("(?:\\.|[^"])*"|'[^']*'|\S+)re");
    const std::string input(text);
    std::vector<std::string> result;
    for (std::sregex_iterator match(input.begin(), input.end(), pattern), end;
         match != end; ++match)
    {
        result.push_back(match->str());
    }
    return result;
}

std::vector<std::string> segments(std::string_view line)
{
    std::vector<std::string> result;
    std::string current;
    char quote = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '\\' && quote == '"' && index + 1 < line.size()) {
            current += character;
            current += line[++index];
            continue;
        }
        if (character == quote) quote = 0;
        else
        if (!quote && (character == '\'' || character == '"')) quote = character;
        else
        if (!quote && (character == ';' || character == '&' || character == '|')) {
            if (!trim(current).empty()) result.push_back(trim(current));
            current.clear();
            continue;
        }
        current += character;
    }
    if (!trim(current).empty()) result.push_back(trim(current));
    return result;
}

bool shell_program(std::string_view base)
{
    return base == "cmd" || base == "powershell" || base == "pwsh" || base == "bash" || base == "sh";
}

bool shell_command_option(std::string_view argument)
{
    return argument == "/c" || argument == "-c" || argument == "-lc" || argument == "-command";
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::optional<Launcher_command> found;
    bool other_command = false;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : segments(line)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = assignment(segment.substr(4));
            else
            if (normalized.starts_with("export ")) standalone = assignment(segment.substr(7));
            else
            if (normalized.starts_with("$env:")) standalone = assignment(segment.substr(5));
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
                environment.emplace_back(match[1].str(), unquote(match[2].str()));
                segment.erase(0, match.length());
            }
            const auto words = tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = assignment(words[index]);
                const auto prefix = lower(unquote(words[index]));
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
            const std::string program = unquote(words[index]);
            const std::string base = base_name(program);
            if (base == "agy") {
                found = Launcher_command{program, environment, {}, true};
                for (++index; index < words.size(); ++index) found->arguments.push_back(unquote(words[index]));
                continue;
            }
            if (shell_program(base)) {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(unquote(words[index]));
                    if (!shell_command_option(argument)) continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    found = recognize(unquote(tail), depth - 1);
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

const std::string& semver_pattern()
{
    static const std::string pattern =
        R"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        R"((?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?)"
        R"((?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)";
    return pattern;
}

bool semver(std::string_view value)
{
    static const std::regex pattern("^" + semver_pattern() + "$", std::regex::ECMAScript);
    return std::regex_match(value.begin(), value.end(), pattern);
}

std::optional<std::string> stream_version(std::string_view value)
{
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos ||
        !addon::is_valid_utf8(value)) return std::nullopt;
    static const std::regex labelled("^(?:Antigravity|agy)(?:\\s+version)?(?:\\s*[:=]\\s*|\\s+)v?(" +
        semver_pattern() + ")(?:\\s+\\([^\\r\\n()]{1,128}\\))?\\s*$", std::regex::icase);
    std::vector<std::string> lines;
    std::string text(value);
    std::replace(text.begin(), text.end(), '\r', '\n');
    std::istringstream input(text);
    std::string version;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty()) continue;
        lines.push_back(line);
        std::smatch match;
        if (std::regex_match(line, match, labelled)) {
            if (!version.empty() && version != match[1].str()) return std::nullopt;
            version = match[1].str();
        }
    }
    if (!version.empty()) return version;
    if (lines.size() == 1) {
        auto candidate = lines.front();
        if (candidate.starts_with('v') || candidate.starts_with('V')) candidate.erase(0, 1);
        if (semver(candidate)) return candidate;
    }
    return std::string{};
}

struct Probe_output
{
    std::string output;
    std::string errors;
};

#ifdef _WIN32
using addon::wide;
using addon::quote_argument;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif

std::optional<Probe_output> probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context)
{
    if (context.cancel_requested()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Probe_output result;
    bool valid = true;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output_read, output_write, errors_read, errors_write;
    if (!CreatePipe(&output_read.value, &output_write.value, &security, 0) ||
        !CreatePipe(&errors_read.value, &errors_write.value, &security, 0)) return std::nullopt;
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errors_read.value, HANDLE_FLAG_INHERIT, 0);
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) return std::nullopt;
    std::wstring command;
    for (const auto& argument : argv) {
        if (!command.empty()) command += L' ';
        command += quote_argument(wide(argument));
    }
    std::vector<std::wstring> variables;
    for (const auto& [name, value] : environment) variables.push_back(wide(name + "=" + value));
    std::sort(variables.begin(), variables.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::wstring environment_block;
    for (const auto& variable : variables) { environment_block += variable; environment_block += L'\0'; }
    environment_block += L'\0';
    if (variables.empty()) environment_block += L'\0';
    // Restrict handle inheritance: concurrent adapter invokes must not retain
    // each other's pipe handles and keep a completed probe alive.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) return std::nullopt;
    HANDLE handles[]{input.value, output_write.value, errors_write.value};
    const bool updated = UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = input.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError  = errors_write.value;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION information{};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const auto directory = wide(working_directory);
    const bool created = updated && job.value &&
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        CreateProcessW(wide(argv.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment_block.data(), directory.empty() ? nullptr : directory.c_str(),
            &startup.StartupInfo, &information);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created) return std::nullopt;
    Handle process{information.hProcess}, thread{information.hThread};
    CloseHandle(output_write.value); output_write.value = nullptr;
    CloseHandle(errors_write.value); errors_write.value = nullptr;
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return std::nullopt;
    }
    const auto drain = [&](HANDLE pipe, std::string& output) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{};
            DWORD count = 0;
            if (!ReadFile(pipe, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            if (output.size() + count > 65536) { valid = false; break; }
            output.append(buffer.data(), count);
        }
    };
    for (;;) {
        drain(output_read.value, result.output);
        drain(errors_read.value, result.errors);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        context.wait_for_cancel(10);
    }
    TerminateJobObject(job.value, 1);
    drain(output_read.value, result.output);
    drain(errors_read.value, result.errors);
    DWORD exit_code = 1;
    valid = GetExitCodeProcess(process.value, &exit_code) && exit_code == 0 && valid;
#else
    int output_pipe[2], errors_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) != 0) return std::nullopt;
    if (pipe2(errors_pipe, O_CLOEXEC) != 0) {
        close(output_pipe[0]); close(output_pipe[1]); return std::nullopt;
    }
    const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        for (int descriptor : {output_pipe[0], output_pipe[1], errors_pipe[0], errors_pipe[1]}) close(descriptor);
        return std::nullopt;
    }
    std::vector<std::string> environment_strings;
    for (const auto& [name, value] : environment) environment_strings.push_back(name + "=" + value);
    std::vector<char*> arguments, variables;
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    for (auto& variable : environment_strings) variables.push_back(variable.data());
    arguments.push_back(nullptr);
    variables.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors_pipe[1], STDERR_FILENO);
    const std::string directory(working_directory);
    if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t process = 0;
    const int spawned = posix_spawn(&process, argv.front().c_str(), &actions, &attributes,
        arguments.data(), variables.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input); close(output_pipe[1]); close(errors_pipe[1]);
    if (spawned != 0) { close(output_pipe[0]); close(errors_pipe[0]); return std::nullopt; }
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errors_pipe[0], F_SETFL, O_NONBLOCK);
    const auto drain = [&](int descriptor, std::string& output) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = read(descriptor, buffer.data(), buffer.size());
            if (count <= 0) break;
            if (output.size() + static_cast<std::size_t>(count) > 65536) { valid = false; break; }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    int status = 0;
    for (;;) {
        drain(output_pipe[0], result.output);
        drain(errors_pipe[0], result.errors);
        const auto waited = waitpid(process, &status, WNOHANG);
        if (waited == process) break;
        if (waited < 0 && errno != EINTR) { valid = false; break; }
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            kill(-process, SIGKILL);
            while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        pollfd descriptors[]{{output_pipe[0], POLLIN, 0}, {errors_pipe[0], POLLIN, 0}};
        poll(descriptors, 2, 10);
    }
    kill(-process, SIGKILL);
    drain(output_pipe[0], result.output);
    drain(errors_pipe[0], result.errors);
    close(output_pipe[0]); close(errors_pipe[0]);
    valid = valid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    return valid && !context.cancel_requested() ? std::optional(std::move(result)) : std::nullopt;
}

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 8);
}

std::optional<Launcher_command> recognize_launcher(const std::vector<std::string>& argv)
{
    if (argv.empty()) return std::nullopt;
    const std::string base = base_name(argv.front());
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
    if (!resolve_program("agy", environment, {}, Launcher_whitespace::ASCII)) return std::nullopt;
    return addon::Launcher_candidate{
        "interactive", "Antigravity", "agy",
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!semver(version)) return false;
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
    const auto first  = stream_version(output);
    const auto second = stream_version(errors);
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
    append(std::to_string(modified.time_since_epoch().count()));
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
    const auto output = agy::probe(argv, environment, working_directory, 3000, context);
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
        "add-dir", "agent", "effort", "input-format", "json-schema", "log-file", "mode", "model",
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
