#include "agy_events.h"
#include "agy_activation.h"
#include "agy_launcher.h"
#include "agy_launcher_approval.h"
#include "agy_preparation.h"
#include "agy_quota.h"
#include "agy_resume.h"
#include "agy_sessions.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/permission_snapshot.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sidecar.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
        std::exit(1);
    }
}

addon::Json_value json(std::string_view bytes)
{
    addon::Json_parse_error failure;
    auto value = addon::parse_json(bytes, {}, &failure);
    require(value.has_value(), "test input must be valid JSON");
    return std::move(*value);
}

class Invocation : public addon::Invocation_context
{
public:
    Invocation(std::string operation, addon::Json_value payload, bool cancelled = false,
        addon::Capability_pair capability = {})
    :
        m_cancelled(cancelled)
    {
        m_request.capability = std::move(capability);
        m_request.operation  = std::move(operation);
        m_request.payload    = std::move(payload);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return m_cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return m_finished; }
    void succeed(addon::Json_value value) override
    {
        m_finished = true;
        m_status = addon::Result_status::SUCCESS;
        m_result = std::move(value);
    }
    void fail(addon::Result_status status, std::string reason, std::string detail) override
    {
        m_finished = true;
        m_status = status;
        m_reason = std::move(reason);
        m_detail = std::move(detail);
    }
    addon::Result_status status() const { return m_status; }
    const std::string& reason() const { return m_reason; }
    const std::string& detail() const { return m_detail; }
    const addon::Json_value& result() const { return m_result; }

private:
    addon::Invoke_request m_request;
    addon::Json_value m_result;
    std::string m_reason;
    std::string m_detail;
    addon::Result_status m_status = addon::Result_status::FAILED;
    bool m_cancelled = false;
    bool m_finished = false;
};

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::canonical(fs::temp_directory_path()) / ("agy-contract-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
    }
    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

class Scratch_environment
{
public:
    explicit Scratch_environment(const fs::path& path)
    {
#ifdef _WIN32
        const auto* previous = _wgetenv(L"LOGONOMIC_ADDON_SCRATCH_DIR");
#else
        const auto* previous = std::getenv("LOGONOMIC_ADDON_SCRATCH_DIR");
#endif
        if (previous) {
            m_previous = fs::path(previous);
        }
        set(path);
    }

    ~Scratch_environment() { set(m_previous); }

private:
    static void set(const std::optional<fs::path>& path)
    {
#ifdef _WIN32
        require(_wputenv_s(L"LOGONOMIC_ADDON_SCRATCH_DIR", path ? path->c_str() : L"") == 0,
            "set the test-owned generation scratch environment");
#else
        require((path ? setenv("LOGONOMIC_ADDON_SCRATCH_DIR", path->c_str(), 1)
                      : unsetenv("LOGONOMIC_ADDON_SCRATCH_DIR")) == 0,
            "set the test-owned generation scratch environment");
#endif
    }

    std::optional<fs::path> m_previous;
};

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void launcher_approval_contract()
{
    const auto& rules = agy::launcher_approval_rules();
    const auto yolo = addon::Launcher_approval_action::YOLOFY;
    const auto undo = addon::Launcher_approval_action::UNYOLOFY;
    const auto changed = addon::Launcher_approval_disposition::CHANGED;
    const auto windows = addon::transform_launcher_approval({
        {"agy", "powershell", ""}, "windows-x86_64", yolo}, rules);
    require(windows.disposition == changed && windows.script_body == "agy --dangerously-skip-permissions",
        "discovered Windows Antigravity launcher supports Yolofy");
    const std::string custom = R"(agy --mode plan --model "custom model" --project "work project")";
    const auto enabled = addon::transform_launcher_approval({
        {custom, "direct", ""}, "linux-x86_64", yolo}, rules);
    require(enabled.disposition == changed && enabled.script_body ==
        R"(agy --dangerously-skip-permissions --mode plan --model "custom model" --project "work project")",
        "Antigravity execution mode, model and project survive the approval override");
    const auto removed = addon::transform_launcher_approval({
        {enabled.script_body, "direct", ""}, "linux-x86_64", undo}, rules);
    require(removed.disposition == changed && removed.script_body == custom,
        "Unyolofy removes only Antigravity invocation-level bypass");
    const std::string prompt = R"(agy -p "--dangerously-skip-permissions")";
    const auto literal = addon::transform_launcher_approval({
        {prompt, "direct", ""}, "linux-x86_64", undo}, rules);
    require(literal.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        literal.script_body == prompt, "Antigravity prompt values preserve flag-like text");
    const auto management = addon::transform_launcher_approval({
        {"agy models", "direct", ""}, "linux-x86_64", yolo}, rules);
    require(management.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Antigravity management commands remain unchanged");
}

void launcher_contract()
{
    for (const auto* script : {"agy", "agy -p hello", "cmd /c \"agy\"",
        "command agy", "export COLOR=blue\nagy", "\"C:/Users/Jane Doe/agy.exe\""})
    {
        require(agy::recognize_launcher(script).has_value(), "recognize executable invocation");
    }
    for (const auto* script : {"echo agy", "npx -y agy", "bunx agy --version", "# agy", "other --model agy"}) {
        require(!agy::recognize_launcher(script), "provider name used as data is not an invocation");
    }
    for (const auto* script : {"export EXTRA=one unsupported; agy", "export -p; agy",
        "export EXTRA='unterminated; agy", "export EXTRA=$(command); agy"})
    {
        require(!agy::recognize_launcher(script), "unsupported export operands cannot justify a direct replacement");
    }
    const auto ordinary = agy::recognize_launcher("exported EXTRA=one; agy");
    require(ordinary && !ordinary->sole_command && ordinary->environment.empty(),
        "export requires an exact command word");
    require(agy::parse_version_output("Antigravity 1.1.11\n", "") == "1.1.11", "labelled native version");
    require(agy::parse_version_output("1.1.11\n", "agy 1.1.12\n").empty(), "conflicting version streams");
    require(!agy::version_is_at_least("1.1.10", 1, 1, 11), "lower than observed hook contract");
    require(!agy::version_is_at_least("1.1.11-rc.1", 1, 1, 11), "prerelease is below stable minimum");
    require(agy::version_is_at_least("3.2.0", 1, 1, 11), "future native version has no upper allowlist");
}

#ifdef _WIN32
struct export_fixture_t
{
    const char* prefix;
    const char* value;
};

constexpr export_fixture_t k_windows_home_exports[] = {
    {"export\tUSERPROFILE=C:/Users/Jane\\ Doe",                   "C:/Users/Jane Doe"},
    {R"(export COLOR=blue USERPROFILE='C:\Users\Jane Doe')",     R"(C:\Users\Jane Doe)"},
    {R"(export USERPROFILE="C:\Users\Jane Doe")",                R"(C:\Users\Jane Doe)"},
    {R"(export USERPROFILE="C:\\Users\\Jane Doe")",              R"(C:\Users\Jane Doe)"},
    {R"(export USERPROFILE=C:\\Users\\Jane\ Doe)",               R"(C:\Users\Jane Doe)"},
    {R"(export USERPROFILE=C:/Users/'Jane '"Doe")",               "C:/Users/Jane Doe"},
};
#endif

addon::Json_value quota_payload()
{
    return json(R"({"status":"SUCCESS","command":{"name":"usage","data":{"groups":[
        {"name":"Claude models","buckets":[{"remaining_fraction":0.25,"window":"weekly","reset_time":"2026-09-14T10:30:00Z"}]},
        {"name":"Gemini models","buckets":[{"remaining_fraction":1,"window":"weekly","reset_time":"2026-09-14T12:30:00+02:00"}]}
    ]}}})");
}

void quota_contract(const char* executable)
{
    addon::Observation_scope scope;
    scope.observed_at_ms = 1789381800000;
    auto value = quota_payload();
    const auto parsed = agy::parse_quota(addon::serialize_json(value), scope);
    require(parsed && parsed->primary && parsed->secondary, "two named model allowance groups");
    require(std::abs(parsed->primary->used_percent - 75.0) < 0.0001, "remaining fraction becomes used percent");
    require(parsed->secondary->used_percent == 0, "zero usage remains an observed zero");
    require(parsed->primary->window_minutes == 10080, "weekly allowance duration");
    require(parsed->primary->resets_at_ms == parsed->secondary->resets_at_ms, "offset reset time normalized to UTC");
    require(parsed->primary->label == "Claude models", "vendor model family label preserved");
    auto* groups = value.find("command")->find("data")->find("groups");
    groups->as_array().push_back(groups->as_array().front());
    require(!agy::parse_quota(addon::serialize_json(value), scope), "extra group is not silently discarded");
    value = quota_payload();
    auto& first = value.find("command")->find("data")->find("groups")->as_array().front();
    auto& bucket = first.find("buckets")->as_array().front();
    bucket.set("window", addon::Json_value("unobserved-window"));
    const auto unknown_window = agy::parse_quota(addon::serialize_json(value), scope);
    require(unknown_window && !unknown_window->primary->window_minutes, "unknown window duration is not guessed");
    bucket.set("remaining_fraction", addon::Json_value("0.25"));
    require(!agy::parse_quota(addon::serialize_json(value), scope), "numeric strings are not quota numbers");
    require(!agy::parse_quota("{\"status\":\"ERROR\"}", scope), "vendor failure never reports quota ready");

    Workspace workspace;
#ifdef _WIN32
    const auto native_program = workspace.path() / "agy.exe";
#else
    const auto native_program = workspace.path() / "agy";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    addon::Probe_prepare_request request;
    request.context.launcher = {"\"" + path_text(native_program) + "\" --model ignored", "direct", ""};
    request.context.launcher_argv = {path_text(native_program), "--model", "ignored"};
    request.context.working_directory = path_text(workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.context.launch_environment->platform_id = "windows-x86_64";
#else
    request.context.launch_environment->platform_id = addon::host_platform_id();
#endif
    Invocation prepare("prepare", addon::encode_probe_prepare_request(request, 2));
    agy::handle_quota(prepare);
    addon::Contract_failure failure;
    const auto process = addon::decode_structured_prepare_result(prepare.result(), &failure);
    require(process.has_value(), "quota preparation yields host-owned process proposal");
    require(process->process.argv == std::vector<std::string>{path_text(native_program), "-p", "/usage", "--output-format", "json"},
        "quota queries CLI local usage command without invoking an agent turn");
    require(process->process.stdin_behavior == addon::Stdin_behavior::CLOSED, "quota stdin closes on start");
#ifdef _WIN32
    // Oracle: https://www.agy.dev/docs/cli/troubleshooting/#resolve-self-updater-locks-and-failures
    // The documented switch prevents native updater descendants opening a console.
    require(process->process.environment ==
        std::vector<std::pair<std::string, std::string>>{{"AGY_CLI_DISABLE_AUTO_UPDATE", "true"}},
        "Windows background quota disables the native updater when no setting was inherited");
#else
    require(process->process.environment.empty(), "Linux quota retains the supplied environment");
#endif

    // The host freezes the launcher's environment in the spelling it found,
    // which on Windows is `Path` rather than `PATH`, and refuses a proposal
    // that names any variable twice under ASCII case folding.
    request.context.launch_environment->effective_environment = json(R"({
        "Path": "C:\\Windows\\system32;C:\\Users\\Jane Doe\\AppData\\Local\\agy\\bin",
        "PATHEXT": ".COM;.EXE;.BAT;.CMD",
        "USERPROFILE": "C:\\Users\\Jane Doe",
        "LOCALAPPDATA": "C:\\Users\\Jane Doe\\AppData\\Local",
        "APPDATA": "C:\\Users\\Jane Doe\\AppData\\Roaming",
        "SystemRoot": "C:\\Windows",
        "agy_cli_disable_auto_update": "false"
    })");
    Invocation populated("prepare", addon::encode_probe_prepare_request(request, 2));
    const auto original_request = addon::serialize_json(populated.request().payload);
    agy::handle_quota(populated);
    const auto admitted = addon::decode_structured_prepare_result(populated.result(), &failure);
    require(admitted && addon::validate_structured_process_proposal(admitted->process, &failure),
        "host admits the quota proposal for a populated Windows launcher environment");
    const auto& environment = admitted->process.environment;
    const auto carries = [&environment](const char* name, const char* value) {
        return std::find(environment.begin(), environment.end(), std::pair<std::string, std::string>{name, value}) !=
            environment.end();
    };
    require(carries("Path", "C:\\Windows\\system32;C:\\Users\\Jane Doe\\AppData\\Local\\agy\\bin") &&
        carries("SystemRoot", "C:\\Windows"), "quota process receives the launcher's own environment");
#ifdef _WIN32
    require(carries("AGY_CLI_DISABLE_AUTO_UPDATE", "true") &&
        !carries("agy_cli_disable_auto_update", "false") && environment.size() == 7,
        "Windows quota replaces the inherited updater setting without a case-insensitive duplicate");
#else
    require(carries("agy_cli_disable_auto_update", "false") && environment.size() == 7,
        "Linux quota preserves the inherited updater setting");
#endif
    require(addon::serialize_json(populated.request().payload) == original_request,
        "quota updater suppression leaves the caller's launcher environment unchanged");

    addon::Probe_inspect_request inspection;
    inspection.inspection.protocol_state = "await_exit";
    Invocation waiting("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(waiting);
    const auto incomplete = addon::decode_quota_inspect_result(waiting.result(), false, &failure, 2);
    require(incomplete && !incomplete->complete && !incomplete->observation, "quota waits for CLI exit");
    inspection.inspection.final = true;
    inspection.inspection.stdout_bytes = addon::serialize_json(quota_payload());
    Invocation completed("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(completed);
    const auto ready = addon::decode_quota_inspect_result(completed.result(), true, &failure, 2);
    require(ready && ready->observation && ready->observation->state == addon::Quota_status::READY &&
        ready->observation->primary && ready->observation->primary->used_percent == 75.0,
        "successful quota output remains available to the host");
    inspection.inspection.stdout_bytes = "{malformed quota";
    Invocation unexplained("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(unexplained);
    const auto neutral = addon::decode_quota_inspect_result(unexplained.result(), true, &failure, 2);
    require(neutral && neutral->observation && neutral->observation->state == addon::Quota_status::FAILED &&
        !neutral->failure_reason, "a host that did not ask for the cause receives a neutral failure");
    inspection.report_quota_failure = true;
    Invocation malformed("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(malformed);
    const auto invalid = addon::decode_quota_inspect_result(malformed.result(), true, &failure, 2);
    require(invalid && invalid->failure_reason && *invalid->failure_reason != "authentication_required",
        "malformed quota reaches the host with a non-authentication failure reason");
    inspection.inspection.stdout_bytes = addon::serialize_json(quota_payload());
    inspection.inspection.timed_out = true;
    Invocation timeout("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(timeout);
    const auto timed_out = addon::decode_quota_inspect_result(timeout.result(), true, &failure, 2);
    require(timed_out && timed_out->failure_reason && timed_out->failure_reason != invalid->failure_reason,
        "a timed-out process is distinguished from malformed quota");
    inspection.inspection.timed_out = false;
    inspection.inspection.exit_code = 7;
    Invocation exited("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(exited);
    const auto failed_process = addon::decode_quota_inspect_result(exited.result(), true, &failure, 2);
    require(failed_process && failed_process->failure_reason &&
        failed_process->failure_reason != invalid->failure_reason &&
        failed_process->failure_reason != timed_out->failure_reason && failed_process->diagnostic &&
        failed_process->diagnostic->stage == addon::Quota_failure_stage::PROCESS_EXIT &&
        failed_process->diagnostic->code == 7,
        "process failure preserves its exit evidence separately from malformed quota and timeout");
    Invocation cancelled("prepare", addon::encode_probe_prepare_request(request, 2), true);
    agy::handle_quota(cancelled);
    require(cancelled.status() == addon::Result_status::CANCELLED, "cancelled quota preparation launches nothing");
}

void launch_environment_contract()
{
    Workspace workspace;
    addon::Prepare_request request;
    request.launcher          = {"agy", "direct", ""};
    request.launcher_argv     = {"agy"};
    request.working_directory = path_text(workspace.path());
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.launch_environment->platform_id = "windows-x86_64";
    constexpr auto home_name = "USERPROFILE";
#else
    request.launch_environment->platform_id = addon::host_platform_id();
    constexpr auto home_name = "HOME";
#endif
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    request.launch_environment->effective_environment.set("agy_cli_disable_auto_update", addon::Json_value("false"));
    Invocation prepare("prepare", addon::encode_prepare_request(request, 3), false, {"launch-preparation", 3});
    const auto original_request = addon::serialize_json(prepare.request().payload);
    agy::handle_preparation(prepare);
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(prepare.result(), &failure, 3);
    require(patch && addon::validate_launch_patch(*patch, {}, {}, &failure),
        "host admits the interactive environment patch and managed hook installation request");
    require(patch->apply_managed_configuration && patch->disposition && patch->disposition->prepared &&
        !patch->disposition->lifecycle_events && patch->disposition->coordination_delivery,
        "managed hooks deliver titles while an unavailable native version withholds lifecycle evidence");
    std::vector<std::pair<std::string, std::string>> expected_environment;
#ifdef _WIN32
    expected_environment.emplace_back("AGY_CLI_DISABLE_AUTO_UPDATE", "true");
#endif
    expected_environment.emplace_back("AGY_LOGONOMIC_HOOKS_ENABLED", "1");
    expected_environment.emplace_back("AGY_LOGONOMIC_LIFECYCLE_ENABLED", "0");
    require(patch->set == expected_environment, "the host receives hook enablement and platform updater policy together");
    require(addon::serialize_json(prepare.request().payload) == original_request &&
        !fs::exists(workspace.path() / ".gemini"), "interactive preparation changes no caller environment or profile");

    // Windows command_for_launcher in Logonomic/src/logonomic_launcher_service.cpp
    // emits these runner arguments for recognized providers (keep_shell_open=false).
    const std::vector<std::pair<std::string, std::vector<std::string>>> shell_launchers{
        {"powershell", {"powershell.exe", "-NoLogo", "-Command", "agy"}},
        {"powershell", {"powershell.exe", "-NoLogo", "-Command", "$env:COLOR='blue'\nagy"}},
        {"cmd", {"cmd.exe", "/c", "agy"}},
        {"bash", {"bash", "-lc", "agy"}},
        {"bash", {"bash", "-lc", "export COLOR=blue\nagy"}},
    };
    for (const auto& [runner, argv] : shell_launchers) {
        request.launcher = {argv.back(), runner, ""};
        request.launcher_argv = argv;
        require(agy::recognize_launcher(request.launcher.script_body).has_value(),
            "the supported shell script is recognized during discovery");
        Invocation shell("prepare", addon::encode_prepare_request(request, 3), false, {"launch-preparation", 3});
        const auto original_shell = addon::serialize_json(shell.request().payload);
        agy::handle_preparation(shell);
        const auto shell_patch = addon::decode_launch_patch(shell.result(), &failure, 3);
        require(shell_patch && addon::validate_launch_patch(*shell_patch, {}, {}, &failure) &&
            addon::serialize_json(shell.result()) == addon::serialize_json(prepare.result()),
            "every host shell runner receives the same updater policy and hook disposition as a direct launch");
        require(addon::serialize_json(shell.request().payload) == original_shell && !shell_patch->argv,
            "preparation preserves the complete shell script and runner arguments");
    }

    request.launcher_argv = {"other", "--model", "agy"};
    Invocation unrelated("prepare", addon::encode_prepare_request(request, 3), false, {"launch-preparation", 3});
    agy::handle_preparation(unrelated);
    const auto unchanged = addon::decode_launch_patch(unrelated.result(), &failure, 3);
    require(unchanged && unchanged->set.empty() && !unchanged->disposition->prepared,
        "a launcher that does not invoke Antigravity receives no updater setting");
}

// Oracle: interactive-initial-prompt@1 and Antigravity's documented
// --prompt-interactive option, which runs the first turn and keeps the TUI.
void initial_prompt_contract(const char* executable)
{
    Workspace workspace;
    const auto scratch = workspace.path() / "diagnostics";
    fs::create_directory(scratch);
    const Scratch_environment scratch_environment(scratch);
    const auto diagnostic_path = scratch / "agy-initial-prompt-failure.json";
    const auto diagnostic_bytes = [
            &diagnostic_path
        ]()
        {
            std::ifstream stream(diagnostic_path, std::ios::binary);
            require(stream.good(), "the failed request leaves a readable private diagnostic");
            return std::string{std::istreambuf_iterator<char>(stream), {}};
        };
#ifdef _WIN32
    const auto native_program = workspace.path() / "agy.exe";
    constexpr auto home_name = "USERPROFILE";
#else
    const auto native_program = workspace.path() / "agy";
    constexpr auto home_name = "HOME";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    const auto program = path_text(native_program);
    const auto directory = workspace.path() / addon::path_from_utf8("brief ' $ & \xC3\xBC");
    fs::create_directories(directory);
    const auto brief = directory / "brief.md";
    std::ofstream(brief, std::ios::binary) << "Earlier work remains here.\n";

    addon::Prepare_request request;
    request.launcher = {"\"" + program + "\" --model gemini-3-pro --mode plan", "direct", ""};
    request.launcher_argv = {program, "--model", "gemini-3-pro", "--mode", "plan"};
    request.working_directory = path_text(workspace.path());
    request.launch_environment = addon::Launch_environment{};
    request.launch_environment->platform_id = addon::host_platform_id();
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    request.launch_environment->effective_environment.set("AGY_TEST_VERSION", addon::Json_value("1.4.4"));
    request.initial_prompt = addon::Initial_prompt_request{
        "Read the brief at " + path_text(brief) + "; preserve \"quotes\", % and $ literally.", path_text(brief)};
    const auto prepare = [&request](bool cancelled = false) {
        auto invocation = std::make_unique<Invocation>(addon::k_prepare_operation,
            addon::encode_prepare_request(request, 3), cancelled, addon::Capability_pair{"launch-preparation", 3});
        agy::handle_preparation(*invocation);
        return invocation;
    };
    auto invocation = prepare();
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(invocation->result(), &failure, 3);
    require(invocation->status() == addon::Result_status::SUCCESS && patch &&
        addon::validate_launch_patch(*patch, {}, {}, &failure) && patch->argv &&
        addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure),
        "the prepared interactive invocation acknowledges the exact host message");
    require(*patch->argv == std::vector<std::string>{program, "--model", "gemini-3-pro", "--mode", "plan",
        "--add-dir", path_text(directory), "--prompt-interactive=" + request.initial_prompt->text},
        "continuation preserves model and permission mode and adds only the brief's directory");
    const auto received = addon::probe(*patch->argv, agy::direct_environment(*request.launch_environment,
        *agy::recognize_launcher(request.launcher.script_body)), request.working_directory, 3000, *invocation);
    require(received && json(received->output).as_array().back() ==
        addon::Json_value("--prompt-interactive=" + request.initial_prompt->text),
        "the native child receives Unicode, spaces and shell metacharacters byte for byte");
    require(patch->apply_managed_configuration && patch->disposition->prepared &&
        patch->disposition->coordination_delivery && patch->disposition->lifecycle_events,
        "continuation retains ordinary managed hooks and lifecycle observation");
    require(!fs::exists(diagnostic_path), "successful stdout help emits no failure diagnostic");
    request.launch_environment->effective_environment.set("AGY_TEST_HELP_STDERR", addon::Json_value("1"));
    invocation = prepare();
    const auto stderr_patch = addon::decode_launch_patch(invocation->result(), &failure, 3);
    require(invocation->status() == addon::Result_status::SUCCESS && stderr_patch &&
        addon::encode_launch_patch(*stderr_patch, 3) == addon::encode_launch_patch(*patch, 3) &&
        addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *stderr_patch, &failure),
        "successful stderr help prepares the same exact prompt, options and hook policy as stdout help");
    require(!fs::exists(diagnostic_path), "successful stderr help emits no failure diagnostic");
    request.launch_environment->effective_environment.erase("AGY_TEST_HELP_STDERR");

    const auto saved_launcher = request.launcher;
    const auto saved_argv = request.launcher_argv;
    for (const auto& [runner, argv] : std::vector<std::pair<std::string, std::vector<std::string>>>{
        {"powershell", {"powershell.exe", "-NoLogo", "-Command", "$env:COLOR='blue'\n\"" + program + "\""}},
        {"cmd", {"cmd.exe", "/c", "\"" + program + "\""}},
        {"bash", {"bash", "-lc", "export COLOR=blue\n\"" + program + "\""}}})
    {
        request.launcher = {argv.back(), runner, ""};
        request.launcher_argv = argv;
        invocation = prepare();
        const auto wrapped = addon::decode_launch_patch(invocation->result(), &failure, 3);
        require(invocation->status() == addon::Result_status::SUCCESS && wrapped && wrapped->argv &&
            wrapped->argv->front() == program && wrapped->argv->back() ==
                "--prompt-interactive=" + request.initial_prompt->text,
            "literal host shell wrappers deliver the prompt through a direct native invocation");
        if (runner != "cmd") {
            require(std::find(wrapped->set.begin(), wrapped->set.end(),
                std::pair<std::string, std::string>{"COLOR", "blue"}) != wrapped->set.end(),
                "a replaced shell keeps its literal environment assignment");
        }
    }
    request.launcher = saved_launcher;
    request.launcher_argv = saved_argv;
    const auto saved_environment = request.launch_environment->effective_environment;
    request.launch_environment->effective_environment.set("COLOR", addon::Json_value("blue"));
    request.launch_environment->effective_environment.set("EXTRA", addon::Json_value(" two words "));
    request.launch_environment->effective_environment.set("EMPTY", addon::Json_value(""));
    for (const char* prefix : {
            "export\tCOLOR=blue EXTRA=' two words ' EMPTY=''",
            "export \tCOLOR='blue'\tEXTRA=' two words '\tEMPTY=''",
            "export COLOR=red COLOR=blue EXTRA=' two words ' EMPTY=''",
            R"(export COLOR=b"lu"e EXTRA=\ two\ words\  EMPTY='')",
            R"(export 'COLOR=blue' "EXTRA= two words " EMPTY="")"})
    {
        request.launcher = {std::string(prefix) + "; \"" + program + "\"", "bash", ""};
        request.launcher_argv = {"bash", "-lc", request.launcher.script_body};
        invocation = prepare();
        const auto exported = addon::decode_launch_patch(invocation->result(), &failure, 3);
        require(invocation->status() == addon::Result_status::SUCCESS && exported && exported->argv &&
            exported->argv->front() == program && exported->initial_prompt,
            "literal export words retain initial-prompt preparation through a native invocation");
        for (const auto& assignment : std::vector<std::pair<std::string, std::string>>{
            {"COLOR", "blue"}, {"EXTRA", " two words "}, {"EMPTY", ""}})
        {
            require(std::count(exported->set.begin(), exported->set.end(), assignment) == 1,
                "each exported literal retains its own exact value in the launch patch");
            const auto environment = agy::direct_environment(*request.launch_environment,
                *agy::recognize_launcher(request.launcher.script_body));
            require(std::count(environment.begin(), environment.end(), assignment) == 1,
                "native help probes receive the same exported literal values");
        }
    }
    request.launcher = saved_launcher;
    request.launcher_argv = saved_argv;
    request.launch_environment->effective_environment = saved_environment;
#ifdef _WIN32
    for (const auto& fixture : k_windows_home_exports) {
        request.launcher = {std::string(fixture.prefix) + "; \"" + program + "\"", "bash", ""};
        request.launcher_argv = {"bash", "-lc", request.launcher.script_body};
        invocation = prepare();
        const auto exported = addon::decode_launch_patch(invocation->result(), &failure, 3);
        require(invocation->status() == addon::Result_status::SUCCESS && exported && exported->argv &&
            exported->argv->front() == program,
            "a Windows Bash literal home retains native initial-prompt preparation");
        require(std::count(exported->set.begin(), exported->set.end(),
                std::pair<std::string, std::string>{"USERPROFILE", fixture.value}) == 1,
            "initial-prompt preparation keeps the Windows Bash home after quote and escape removal");
        const auto environment = agy::direct_environment(*request.launch_environment,
            *agy::recognize_launcher(request.launcher.script_body));
        require(std::count(environment.begin(), environment.end(),
                std::pair<std::string, std::string>{"USERPROFILE", fixture.value}) == 1,
            "native help probes keep the Windows Bash home after quote and escape removal");
    }
    request.launcher = saved_launcher;
    request.launcher_argv = saved_argv;
#endif
    require(prepare(true)->status() == addon::Result_status::CANCELLED,
        "cancelled initial prompt preparation cannot propose a launch");
    const auto cancelled_diagnostic = json(diagnostic_bytes());
    require(cancelled_diagnostic.find("guard")->as_string() == "base_preparation" &&
        cancelled_diagnostic.find("probe_present")->is_null(),
        "base-preparation cancellation is distinguished from an attempted help probe");
    request.launch_environment->effective_environment.set("AGY_TEST_NO_INITIAL_PROMPT", addon::Json_value("1"));
    request.launch_environment->effective_environment.set("AGY_TEST_HELP_PRIVATE",
        addon::Json_value("private-native-help-data"));
    invocation = prepare();
    require(invocation->status() == addon::Result_status::UNAVAILABLE &&
        invocation->reason() == addon::reason::k_initial_prompt_unavailable,
        "a CLI without an interactive first-message option cannot silently start an ordinary session");
    const auto feature_bytes = diagnostic_bytes();
    const auto feature_diagnostic = json(feature_bytes);
    require(feature_diagnostic.find("guard")->as_string() == "help_features" &&
        feature_diagnostic.find("probe_present")->as_boolean() &&
        !feature_diagnostic.find("prompt_interactive")->as_boolean() &&
        feature_diagnostic.find("add_dir")->as_boolean() &&
        feature_diagnostic.find("output_bytes")->as_integer() > 0 &&
        feature_diagnostic.find("error_bytes")->as_integer() > 0,
        "the failure record describes the observed feature admission and stream sizes");
    const std::vector<std::string_view> diagnostic_members{
        "guard", "phase", "elapsed_ms", "probe_present", "probe_failure", "exit_status",
        "output_bytes", "error_bytes", "prompt_interactive", "add_dir", "contract_failure"};
    for (const auto& member : feature_diagnostic.as_object()) {
        require(std::find(diagnostic_members.begin(), diagnostic_members.end(), member.name) != diagnostic_members.end(),
            "the diagnostic contains only bounded outcome metadata");
    }
    require(feature_bytes.size() < 1024 && feature_bytes.find(program) == std::string::npos &&
        feature_bytes.find(request.initial_prompt->text) == std::string::npos &&
        feature_bytes.find("private-native-help-data") == std::string::npos &&
        feature_bytes.find("AGY_TEST_HELP_PRIVATE") == std::string::npos &&
        feature_bytes.find("Usage: agy") == std::string::npos &&
        feature_diagnostic.find("elapsed_ms")->as_integer() >= 0,
        "the bounded diagnostic excludes executable paths, prompt, environment and native output");

    request.launch_environment->effective_environment.set("AGY_TEST_HELP_FAIL", addon::Json_value("1"));
    invocation = prepare();
    require(invocation->status() == addon::Result_status::UNAVAILABLE &&
        invocation->reason() == addon::reason::k_initial_prompt_unavailable,
        "a failed native help process keeps the original unavailable result");
    const auto probe_diagnostic = json(diagnostic_bytes());
    require(probe_diagnostic.find("guard")->as_string() == "help_probe" &&
        !probe_diagnostic.find("probe_present")->as_boolean() &&
        probe_diagnostic.find("probe_failure")->as_string() == "unknown" &&
        probe_diagnostic.find("exit_status")->is_null() &&
        probe_diagnostic.find("output_bytes")->is_null() && probe_diagnostic.find("error_bytes")->is_null() &&
        probe_diagnostic.find("prompt_interactive")->is_null() && probe_diagnostic.find("add_dir")->is_null(),
        "a missing SDK probe result does not invent an exit status, stream contents or cause");
    request.launch_environment->effective_environment.erase("AGY_TEST_HELP_FAIL");
    request.launch_environment->effective_environment.erase("AGY_TEST_HELP_PRIVATE");
    request.launch_environment->effective_environment.erase("AGY_TEST_NO_INITIAL_PROMPT");

    request.launcher_argv = {"unrelated-program"};
    invocation = prepare();
    const auto early_diagnostic = json(diagnostic_bytes());
    require(invocation->reason() == addon::reason::k_initial_prompt_unavailable &&
        early_diagnostic.find("guard")->as_string() == "launcher_argv" &&
        early_diagnostic.find("probe_present")->is_null() &&
        std::distance(fs::directory_iterator(scratch), fs::directory_iterator{}) == 1,
        "the latest early refusal atomically replaces one record and leaves no temporary history");
    request.launcher_argv = saved_argv;

    request.launcher.script_body = "\"" + program + "\"";
    request.launcher_argv = {program};
    while (request.launcher_argv.size() < addon::limits::k_max_argv_entries) {
        request.launcher.script_body += " --sandbox";
        request.launcher_argv.push_back("--sandbox");
    }
    invocation = prepare();
    const auto patch_diagnostic = json(diagnostic_bytes());
    require(invocation->reason() == addon::reason::k_initial_prompt_unavailable &&
        patch_diagnostic.find("guard")->as_string() == "launch_patch" &&
        !patch_diagnostic.find("contract_failure")->as_string().empty() &&
        patch_diagnostic.find("probe_present")->as_boolean(),
        "exceeding the SDK argv budget records the contract failure after successful help admission");
    request.launcher = saved_launcher;
    request.launcher_argv = saved_argv;

    auto malformed = addon::encode_prepare_request(request, 3);
    malformed.find(addon::k_initial_prompt_member)->set("version", addon::Json_value(2));
    Invocation invalid(addon::k_prepare_operation, std::move(malformed), false, {"launch-preparation", 3});
    agy::handle_preparation(invalid);
    const auto decode_diagnostic = json(diagnostic_bytes());
    require(invalid.status() == addon::Result_status::FAILED &&
        decode_diagnostic.find("guard")->as_string() == "prepare_request" &&
        decode_diagnostic.find("contract_failure")->as_string() == invalid.reason() &&
        decode_diagnostic.find("probe_present")->is_null(),
        "request decoding records its own failure before base preparation can run");

    const auto blocked_scratch = workspace.path() / "scratch-is-a-file";
    std::ofstream(blocked_scratch, std::ios::binary) << "User-owned file remains untouched.\n";
    {
        const Scratch_environment blocked_environment(blocked_scratch);
        request.launch_environment->effective_environment.set("AGY_TEST_NO_INITIAL_PROMPT", addon::Json_value("1"));
        invocation = prepare();
        require(invocation->status() == addon::Result_status::UNAVAILABLE &&
            invocation->reason() == addon::reason::k_initial_prompt_unavailable && fs::is_regular_file(blocked_scratch),
            "unwritable scratch diagnostics cannot change preparation or replace the user's storage");
        std::ifstream retained(blocked_scratch, std::ios::binary);
        require(std::string{std::istreambuf_iterator<char>(retained), {}} == "User-owned file remains untouched.\n",
            "diagnostic failure preserves the contents at the supplied scratch path");
        request.launch_environment->effective_environment.erase("AGY_TEST_NO_INITIAL_PROMPT");
    }
    for (const auto* options : {"--continue", "--conversation=other", "--conversation other", "-p hello", "--print", "-i hello",
        "--prompt-interactive=hello", "models", "--help", "--version", "--new-project"})
    {
        request.launcher.script_body = "\"" + program + "\" " + options;
        request.launcher_argv = {program};
        const auto command = agy::recognize_launcher(request.launcher.script_body);
        request.launcher_argv.insert(request.launcher_argv.end(),
            command->arguments.begin(), command->arguments.end());
        invocation = prepare();
        require(invocation->status() == addon::Result_status::UNAVAILABLE &&
            invocation->reason() == addon::reason::k_initial_prompt_unavailable,
            "conversation selectors, existing prompts and management commands cannot precede the host message");
    }
    request.launcher = saved_launcher;
    request.launcher_argv = saved_argv;
    const auto last_failure = diagnostic_bytes();
    request.initial_prompt.reset();
    invocation = prepare();
    const auto ordinary = addon::decode_launch_patch(invocation->result(), &failure, 3);
    require(invocation->status() == addon::Result_status::SUCCESS && ordinary &&
        !ordinary->argv && !ordinary->initial_prompt && diagnostic_bytes() == last_failure,
        "ordinary launches retain their original invocation and acknowledge no initial prompt");
}

// Oracle: interactive-resume@2 (the SDK's docs/interactive_resume_2.md and
// session_contracts.h) and agy's `--conversation <ID>` selector.
// The schema and the empty/root/user-input distinctions were captured from
// native AGY 1.5.3. Payload text and identities below are synthetic fixtures.
fs::path resume_database(const fs::path& home, const std::string& identity)
{
    const auto root = home / ".gemini" / "antigravity-cli" / "conversations";
    fs::create_directories(addon::usage_cache_io_path(root));
    const auto path = root / (identity + ".db");
    sqlite3* database = nullptr;
#ifdef _WIN32
    const char* native_vfs = "win32-longpath";
#else
    const char* native_vfs = nullptr;
#endif
    require(sqlite3_open_v2(path_text(addon::usage_cache_io_path(path)).c_str(), &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, native_vfs) == SQLITE_OK, "create native resume fixture");
    require(sqlite3_exec(database,
        "CREATE TABLE trajectory_meta(trajectory_id TEXT PRIMARY KEY,cascade_id TEXT,trajectory_type INTEGER,source INTEGER);"
        "CREATE TABLE steps(idx INTEGER PRIMARY KEY,step_type INTEGER NOT NULL DEFAULT 0,status INTEGER NOT NULL DEFAULT 0,"
        "has_subtrajectory NUMERIC NOT NULL DEFAULT false,metadata BLOB,error_details BLOB,permissions BLOB,task_details BLOB,"
        "render_info BLOB,step_payload BLOB,step_format INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE parent_references(idx INTEGER PRIMARY KEY,data BLOB);"
        "CREATE TABLE gen_metadata(idx INTEGER PRIMARY KEY,data BLOB,size INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE executor_metadata(idx INTEGER PRIMARY KEY,data BLOB);"
        "CREATE TABLE battle_mode_infos(idx INTEGER PRIMARY KEY,data BLOB);"
        "CREATE TABLE trajectory_metadata_blob(id TEXT PRIMARY KEY,data BLOB);",
        nullptr, nullptr, nullptr) == SQLITE_OK, "create captured native conversation schema");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, "INSERT INTO trajectory_meta VALUES(?,?,4,17)", -1, &statement, nullptr) == SQLITE_OK,
        "prepare native trajectory identity");
    sqlite3_bind_text(statement, 1, identity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, identity.c_str(), -1, SQLITE_TRANSIENT);
    require(sqlite3_step(statement) == SQLITE_DONE, "insert native trajectory identity");
    sqlite3_finalize(statement);
    require(identity.size() < 128, "fixture identity fits one protobuf length byte");
    const auto metadata = std::string(1, '\x32') + static_cast<char>(identity.size()) + identity;
    require(sqlite3_prepare_v2(database, "INSERT INTO trajectory_metadata_blob VALUES('main',?)", -1,
        &statement, nullptr) == SQLITE_OK, "prepare native root metadata");
    sqlite3_bind_blob(statement, 1, metadata.data(), static_cast<int>(metadata.size()), SQLITE_TRANSIENT);
    require(sqlite3_step(statement) == SQLITE_DONE, "insert native root metadata");
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return path;
}

// Native 1.5.3 schema and descriptors establish these independent oracles:
// a zero-step root is empty, and UserInput text is UTF-8 while items are typed
// TextOrScopeItem messages. Inspection must not create or modify native files.
void resume_inspection_readonly_contract()
{
    Workspace workspace;
    const std::string identity = "native-inspection-fixture";
    const auto database_path = resume_database(workspace.path(), identity);
    addon::Resume_inspect_request request;
    request.context.launcher = {"agy", "direct", ""};
    request.context.launcher_argv = {"agy"};
    request.context.working_directory = path_text(workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = addon::host_platform_id();
#ifdef _WIN32
    constexpr auto home_name = "USERPROFILE";
    constexpr bool reader_available = true;
#else
    constexpr auto home_name = "HOME";
    const bool reader_available = geteuid() != 0;
#endif
    request.context.launch_environment->effective_environment.set(home_name,
        addon::Json_value(path_text(workspace.path())));
    request.resume_record = agy::encode_resume_identity({identity, path_text(workspace.path())});

    struct Native_file
    {
        fs::path name;
        fs::file_time_type modified;
        std::string contents;
        bool operator==(const Native_file&) const = default;
    };
    const auto native_state = [&] {
        std::vector<Native_file> files;
        for (const auto& item : fs::directory_iterator(database_path.parent_path())) {
            require(item.is_regular_file(), "the native inspection fixture contains regular files");
            std::ifstream input(item.path(), std::ios::binary);
            require(input.good(), "read the complete native fixture for mutation detection");
            files.push_back({item.path().filename(), fs::last_write_time(item.path()),
                std::string(std::istreambuf_iterator<char>(input), {})});
        }
        std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return files;
    };
    const auto inspect = [&](std::uint16_t version = 4) {
        const auto before = native_state();
        Invocation call(addon::k_inspect_resume_operation, addon::encode_resume_inspect_request(request),
            false, {"interactive-resume", version});
        agy::handle_preparation(call);
        addon::Contract_failure failure;
        const auto result = addon::decode_resume_inspect_result(call.result(), &failure, version);
        require(call.status() == addon::Result_status::SUCCESS && result &&
            addon::validate_resume_inspection(request, *result, &failure), "inspection acknowledges its exact request");
        require(version != 3 || !call.result().find("content"), "version three does not acquire a content field");
        require(native_state() == before, "inspection leaves native filenames, bytes and modification times unchanged");
        return *result;
    };
    const auto availability = reader_available ? addon::Resume_availability::AVAILABLE : addon::Resume_availability::UNKNOWN;
    const auto content = [&](addon::Resume_content known) {
        return reader_available ? known : addon::Resume_content::UNKNOWN;
    };

    // An open writer supplies the real WAL/index. The reader shares SQLite's
    // process-wide inode mapping with it, so it must still refuse index writes.
    sqlite3* writer = nullptr;
    require(sqlite3_open(path_text(database_path).c_str(), &writer) == SQLITE_OK, "open the native fixture writer");
    require(sqlite3_exec(writer, "PRAGMA journal_mode=WAL;PRAGMA wal_autocheckpoint=0;SELECT COUNT(*) FROM steps;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "retain the native WAL/index without content");
    auto answer = inspect();
    require(answer.availability == availability && answer.content == content(addon::Resume_content::EMPTY),
        "a readable zero-step root is empty; privileged POSIX inspection remains unknown");
    require(inspect(3).availability == availability, "version three retains the same availability predicate");
    require(sqlite3_exec(writer,
        "INSERT INTO steps(idx,step_type,status,step_payload,step_format) VALUES(0,14,3,X'080e9a0107120568656c6c6f',0)",
        nullptr, nullptr, nullptr) == SQLITE_OK, "append actual user text in the live WAL only");
    answer = inspect();
    require(answer.availability == availability && answer.content == content(addon::Resume_content::NONEMPTY),
        "the read-only reader observes retained content committed only to the live WAL");

    struct Content_case
    {
        const char* payload;
        addon::Resume_availability availability;
        addon::Resume_content content;
    };
    for (const auto& sample : {
        Content_case{"080e9a01091a070a0568656c6c6f", addon::Resume_availability::AVAILABLE, addon::Resume_content::NONEMPTY},
        Content_case{"080e9a01041a020a00", addon::Resume_availability::AVAILABLE, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01031a0100", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01041a024801", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01041a021200", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01031201ff", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01051a030a01ff", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a0106120178120179", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01071a050a01781200", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a01081a060a01780a0179", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080f9a0103120178", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN},
        Content_case{"080e9a010f120178", addon::Resume_availability::UNKNOWN, addon::Resume_content::UNKNOWN}})
    {
        const std::string sql = "UPDATE steps SET step_payload=X'" + std::string(sample.payload) + "'";
        require(sqlite3_exec(writer, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
            "replace the native input with an independent typed-content fixture");
        answer = inspect();
        require(answer.availability == (reader_available ? sample.availability : addon::Resume_availability::UNKNOWN) &&
            answer.content == content(sample.content),
            "malformed, non-text, empty, invalid UTF-8 and conflicting singular items never establish retained text");
    }
    sqlite3_close(writer);
    require(!fs::exists(fs::path(database_path.string() + "-wal")) &&
        !fs::exists(fs::path(database_path.string() + "-shm")), "the closed WAL fixture begins without sidecars");
    answer = inspect();
    require(answer.availability == addon::Resume_availability::UNKNOWN && answer.content == addon::Resume_content::UNKNOWN,
        "a closed WAL without native sidecars remains unknown without creating new native state");
}

void resume_contract(const char* executable)
{
    resume_inspection_readonly_contract();
    Workspace workspace;
#ifdef _WIN32
    const auto native_program = workspace.path() / "agy.exe";
    constexpr auto platform   = "windows-x86_64";
    constexpr auto home_name  = "USERPROFILE";
    const std::string bridge  = "helpers/agy_resume_bridge.exe";
#else
    const auto native_program = workspace.path() / "agy";
    const auto platform       = addon::host_platform_id();
    constexpr auto home_name  = "HOME";
    const std::string bridge  = "helpers/agy_resume_bridge";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    const std::string conversation = "ec33ebf9-0cba-4100-8142-c61503f6c587";
    const auto database_path = resume_database(workspace.path(), conversation);
    const auto program = path_text(native_program);

    addon::Resume_prepare_request request;
    request.context.launcher = {"COLOR=blue \"" + program + "\" --model gemini-3-pro --sandbox", "direct", ""};
    request.context.launcher_argv = {program, "--model", "gemini-3-pro", "--sandbox"};
    request.context.working_directory = path_text(workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = platform;
    request.context.launch_environment->effective_environment.set(
        home_name, addon::Json_value(path_text(workspace.path())));
#ifndef _WIN32
    request.context.launch_environment->effective_environment.set(
        "AGY_TEST_CUSTODY", addon::Json_value(std::to_string(getpgrp()) + ":" + std::to_string(getsid(0))));
#endif
    request.resume_record = json(R"({"provider_session_id":"ec33ebf9-0cba-4100-8142-c61503f6c587"})");
    const addon::Capability_pair capability{"interactive-resume", 2};
    const auto prepare = [&request, &capability](bool cancelled = false) {
        auto invocation = std::make_unique<Invocation>(addon::k_prepare_resume_operation,
            addon::encode_resume_prepare_request(request, 2), cancelled, capability);
        agy::handle_preparation(*invocation);
        return invocation;
    };

    auto invocation = prepare();
    require(invocation->status() == addon::Result_status::SUCCESS, "a recorded conversation is resumable");
    addon::Contract_failure failure;
    auto resumed = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
    require(resumed && addon::validate_resume_process_proposal(resumed->process, &failure) &&
        addon::validate_launch_patch(resumed->patch, {}, {}, &failure), "host admits the resume proposal");
    require(addon::validate_resume_acknowledgment(request.resume_record, resumed->acknowledgment, &failure),
        "resume acknowledges exactly the record the host sent");
    require(resumed->process.executable == addon::Process_executable(bridge),
        "the native program runs through the package's launch bridge");
    require(resumed->process.argv == std::vector<std::string>{
        program, "--model", "gemini-3-pro", "--sandbox", "--conversation", conversation},
        "resume selects the conversation and keeps the launcher's own options");
    require(resumed->process.stdin_behavior == addon::Stdin_behavior::INHERIT, "a resumed session keeps the terminal");
    std::vector<std::pair<std::string, std::string>> expected_environment{{"COLOR", "blue"}};
#ifdef _WIN32
    expected_environment.emplace_back("AGY_CLI_DISABLE_AUTO_UPDATE", "true");
#endif
    expected_environment.emplace_back("AGY_LOGONOMIC_HOOKS_ENABLED", "1");
    expected_environment.emplace_back("AGY_LOGONOMIC_LIFECYCLE_ENABLED", "1");
    expected_environment.emplace_back("AGY_LOGONOMIC_OBSERVE_SELECTION", "1");
    require(resumed->patch.set == expected_environment && !resumed->patch.apply_managed_configuration,
        "resume enables the hooks the host installed before resume preparation and keeps literal assignments");
    const auto literal_launcher = request.context.launcher;
    const auto literal_argv = request.context.launcher_argv;
    for (const char* prefix : {"export\tCOLOR=blue", "export EXTRA='two words' COLOR=blue",
            R"(export EXTRA=two\ words COLOR=b"lu"e)"})
    {
        request.context.launcher = {std::string(prefix) + "; \"" + program +
            "\" --model gemini-3-pro --sandbox", "bash", ""};
        request.context.launcher_argv = {"bash", "-lc", request.context.launcher.script_body};
        invocation = prepare();
        const auto exported = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
        require(invocation->status() == addon::Result_status::SUCCESS && exported &&
            exported->process.argv == resumed->process.argv &&
            std::find(exported->patch.set.begin(), exported->patch.set.end(),
                std::pair<std::string, std::string>{"COLOR", "blue"}) != exported->patch.set.end(),
            "each literal export operand survives resume without changing the selected conversation");
    }
#ifdef _WIN32
    const auto exported_home = workspace.path() / "Jane Doe";
    (void)resume_database(exported_home, conversation);
    const auto exported_text = path_text(exported_home);
    std::string escaped_home;
    for (const char character : exported_text) {
        if (character == '\\') escaped_home += '\\';
        escaped_home += character;
    }
    for (const auto& prefix : {"export USERPROFILE='" + exported_text + "'",
                              "export USERPROFILE=\"" + escaped_home + "\""})
    {
        request.context.launcher = {prefix + "; \"" + program +
            "\" --model gemini-3-pro --sandbox", "bash", ""};
        request.context.launcher_argv = {"bash", "-lc", request.context.launcher.script_body};
        invocation = prepare();
        const auto exported = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
        require(invocation->status() == addon::Result_status::SUCCESS && exported &&
            exported->process.argv == resumed->process.argv &&
            std::count(exported->patch.set.begin(), exported->patch.set.end(),
                std::pair<std::string, std::string>{"USERPROFILE", exported_text}) == 1,
            "resume inspects the test-owned Windows Bash literal home it will actually launch");
    }
#endif
    request.context.launcher = literal_launcher;
    request.context.launcher_argv = literal_argv;

    request.startup_observer = addon::Startup_observer_offer{
        1, "http-json-post", "http://127.0.0.1:54321/events", "Bearer " + std::string(64, 'a'), "observed-identity"};
    request.context.profile_storage = addon::Profile_storage_context{1, path_text(workspace.path())};
    invocation = prepare();
    resumed = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
#ifdef _WIN32
    const std::string resume_bridge = "helpers/agy_resume_bridge.exe";
#else
    const std::string resume_bridge = "helpers/agy_resume_bridge";
#endif
    require(resumed && resumed->process.executable == addon::Process_executable(resume_bridge) &&
        resumed->startup_observer && addon::encode_startup_observer_offer(*resumed->startup_observer) ==
        addon::encode_startup_observer_offer(*request.startup_observer), "resume uses the exact offered observed-identity authority");
    require(resumed->process.argv == std::vector<std::string>{
        program, "--model", "gemini-3-pro", "--sandbox", "--conversation", conversation},
        "startup authority stays out of argv and the original options remain exact");
    request.context.profile_storage.reset();
    require(prepare()->status() == addon::Result_status::SUCCESS, "kernel-owned startup pipe needs no persistent profile artifact");
    request.startup_observer.reset();

    require(prepare(true)->status() == addon::Result_status::CANCELLED, "a cancelled resume launches nothing");

    for (const auto* record : {R"({"provider_session_id":"-c"})", R"({"provider_session_id":""})",
        R"({"provider_session_id":7})", R"({"conversationId":"ec33ebf9-0cba-4100-8142-c61503f6c587"})",
        R"({"provider_session_id":"ec33ebf9-0cba-4100-8142-c61503f6c587","extra":true})"})
    {
        const auto saved = request.resume_record;
        request.resume_record = json(record);
        invocation = prepare();
        require(invocation->status() == addon::Result_status::FAILED &&
            invocation->reason() == addon::reason::k_resume_record_invalid, "an unusable record is refused");
        request.resume_record = saved;
    }
    const auto saved = request.context.launcher;
    for (const auto* options : {"\"\"", "--model \"\"", "--continue", "-c", "--conversation=other", "--conversation other", "-p hello", "--print", "-i hello",
        "--prompt-interactive=hello", "models", "--model", "--model $MODEL", "--add-dir \"two words\"",
        "-- --sandbox", "--help", "--version", "--log-file owner.log"})
    {
        request.context.launcher.script_body = "\"" + program + "\" " + options;
        invocation = prepare();
        require(invocation->status() == addon::Result_status::UNAVAILABLE, "an incompatible launcher cannot resume");
    }
    // A shell runner, which is where expansion and further commands apply.
    request.context.launcher.runner_key = addon::k_runner_bash;
    for (const auto& script : {"COLOR=$THEME \"" + program + "\"", "COLOR=~/theme \"" + program + "\"",
        "export COLOR=~/theme\n\"" + program + "\"",
        "export COLOR=blue EXTRA=$THEME; \"" + program + "\"",
        "export COLOR=blue EXTRA=\"$THEME\"; \"" + program + "\""})
    {
        request.context.launcher.script_body = script;
        require(prepare()->status() == addon::Result_status::UNAVAILABLE, "an expanded assignment has no literal value");
    }
    for (const auto& script : {"source /opt/agy-env.sh; \"" + program + "\"", "\"" + program + "\" && echo done",
        "echo starting\n\"" + program + "\""})
    {
        request.context.launcher.script_body = script;
        require(prepare()->status() == addon::Result_status::UNAVAILABLE,
            "a launcher running other commands cannot be replaced by a direct invocation");
    }
    request.context.launcher.script_body = "npx agy";
    require(prepare()->status() == addon::Result_status::UNAVAILABLE, "an unrecognized launcher cannot resume");
    request.context.launcher = saved;
    request.context.launcher.script_body = "\"" + path_text(workspace.path() / "absent" / "agy") + "\"";
    require(prepare()->status() == addon::Result_status::UNAVAILABLE, "resume needs a resolvable native program");
    request.context.launcher = saved;

    addon::Resume_inspect_request inspected{request.context, request.resume_record};
#ifdef _WIN32
    const bool privileged_inspection = false;
#else
    const bool privileged_inspection = geteuid() == 0;
#endif
    const auto inspect = [&inspected](std::uint16_t version = 4) {
        auto call = std::make_unique<Invocation>(addon::k_inspect_resume_operation,
            addon::encode_resume_inspect_request(inspected), false, addon::Capability_pair{"interactive-resume", version});
        agy::handle_preparation(*call);
        return call;
    };
    auto inspection = inspect();
    auto answer = addon::decode_resume_inspect_result(inspection->result(), &failure, 4);
    require(inspection->status() == addon::Result_status::SUCCESS && answer &&
        addon::validate_resume_inspection(inspected, *answer, &failure) &&
        answer->availability == (privileged_inspection ? addon::Resume_availability::UNKNOWN : addon::Resume_availability::AVAILABLE) &&
        answer->content == (privileged_inspection ? addon::Resume_content::UNKNOWN : addon::Resume_content::EMPTY),
        "a complete native empty root is inspected, or remains unknown under privileged read-only protection");
    require(!inspect(3)->result().find("content"), "version three keeps its closed availability-only result");
    inspected.resume_record.set("native_home", addon::Json_value(path_text(workspace.path() / "other-profile")));
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == addon::Resume_availability::UNKNOWN &&
        answer->content == addon::Resume_content::UNKNOWN, "an opaque scope cannot be retargeted to another native home");
    inspected.resume_record.set("native_home", addon::Json_value(path_text(workspace.path())));
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == (privileged_inspection ? addon::Resume_availability::UNKNOWN : addon::Resume_availability::AVAILABLE),
        "the exact captured home is inspected in its resolved context");

    sqlite3* writer = nullptr;
    require(sqlite3_open(path_text(database_path).c_str(), &writer) == SQLITE_OK, "open native fixture writer");
    require(sqlite3_exec(writer,
        "PRAGMA journal_mode=WAL;PRAGMA wal_autocheckpoint=0;"
        "INSERT INTO steps(idx,step_type,step_payload) VALUES(0,14,X'080e9a0107120568656c6c6f');",
        nullptr, nullptr, nullptr) == SQLITE_OK, "append retained native user content to live WAL");
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == (privileged_inspection ? addon::Resume_availability::UNKNOWN : addon::Resume_availability::AVAILABLE) &&
        answer->content == (privileged_inspection ? addon::Resume_content::UNKNOWN : addon::Resume_content::NONEMPTY),
        "live WAL content is read without writes, or unknown under privileged read-only protection");
    require(sqlite3_exec(writer, "UPDATE steps SET step_format=99", nullptr, nullptr, nullptr) == SQLITE_OK,
        "replace the step format with an unsupported native version");
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == addon::Resume_availability::UNKNOWN &&
        answer->content == addon::Resume_content::UNKNOWN, "unsupported native formats are never empty");
    require(sqlite3_exec(writer, "DELETE FROM steps;INSERT INTO parent_references VALUES(0,X'00')",
        nullptr, nullptr, nullptr) == SQLITE_OK, "retain an unresolved parent history");
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->content == addon::Resume_content::UNKNOWN, "unread parent content cannot establish empty");
    sqlite3_close(writer);

    const auto moved = fs::path(database_path.string() + ".retained");
    fs::rename(database_path, moved);
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == addon::Resume_availability::ABSENT,
        "an exact missing native conversation is definitively absent");
    invocation = prepare();
    require(invocation->status() == addon::Result_status::UNAVAILABLE && invocation->reason() == "resume_conversation_absent",
        "explicit preparation rechecks the same exact native availability");
    const auto local_argv = request.context.launcher_argv;
    request.context.launcher.script_body = "\"" + program + "\" --remote-agent --project=remote-project";
    request.context.launcher_argv = {program, "--remote-agent", "--project=remote-project"};
    inspected.context = request.context;
    answer = addon::decode_resume_inspect_result(inspect()->result(), &failure, 4);
    require(answer && answer->availability == addon::Resume_availability::UNKNOWN,
        "local absence says nothing about a native remote project conversation");
    invocation = prepare();
    const auto remote = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
    require(invocation->status() == addon::Result_status::SUCCESS && remote &&
        remote->process.argv == std::vector<std::string>{program, "--remote-agent", "--project=remote-project",
            "--conversation", conversation} &&
        std::none_of(remote->patch.set.begin(), remote->patch.set.end(),
            [](const auto& item) { return item.first == "AGY_LOGONOMIC_OBSERVE_SELECTION"; }),
        "unknown remote inspection preserves its exact native recipe without claiming local observation");
    request.context.launcher = saved;
    request.context.launcher_argv = local_argv;
    inspected.context = request.context;
    fs::rename(moved, database_path);
    invocation = prepare();
    require(invocation->status() == addon::Result_status::SUCCESS,
        "unknown optional content remains recoverable through an exact native resume");
    request.resume_record.set("native_home", addon::Json_value(path_text(workspace.path() / "foreign-home")));
    invocation = prepare();
    require(invocation->status() == addon::Result_status::UNAVAILABLE && invocation->reason() == "resume_scope_mismatch",
        "native fallback still refuses a changed captured home");
}

addon::Classify_event_request hook_event(std::string name)
{
    addon::Classify_event_request request;
    request.event.name = std::move(name);
    request.event.body = json(R"({"conversationId":"Conversation-Parent"})");
    request.transition_nonce = std::string(32, 'a');
    return request;
}

// Synthetic native-format records exercise selected association independently
// of load readiness: A -> B -> A, duplicate selection, and delayed activity.
// These adapter fixtures do not establish actual native picker or idle behavior.
void native_selection_contract()
{
    const std::string a = "8cd7f43f-0eb0-4097-b337-e449322f2d28";
    const std::string b = "46973bfa-364b-43cd-b0ba-12bb85e5c839";
    const std::string nonce_a(32, 'a');
    const std::string nonce_b(32, 'b');
    const std::string nonce_return(32, 'c');
    Workspace workspace;
    const auto record = [](const std::string& identity) {
        return "I1108 10:36:09.061796    1884 conversation_manager.go:1107] Streaming conversation " + identity + '\n';
    };
    const auto selection = [&](const std::string& identity, const std::string& previous, const std::string& nonce) {
        addon::Classify_event_request request;
        request.event.name = "NativeConversation";
        request.event.body = addon::Json_value::object();
        request.event.body.set("conversationId", addon::Json_value(identity));
        request.event.body.set("native_record", addon::Json_value(record(identity)));
        request.event.body.set("native_home", addon::Json_value(path_text(workspace.path())));
        request.event.body.set("previous_activation_nonce", addon::Json_value(previous));
        request.transition_nonce = nonce;
        return request;
    };
    auto first = selection(a, "", nonce_a);
    const auto started = agy::classify_event(first);
    require(started && started->classification == addon::Classification::TRANSITION &&
        started->transition == addon::Transition_kind::START && started->observations.size() == 1 &&
        started->observations.front().session_transition == addon::Session_transition::STARTED &&
        started->observations.front().phase_transition == addon::Phase_transition::NONE,
        "native root selection establishes identity without inventing a turn or phase");
    require(started->observations.front().resume_record &&
        started->observations.front().resume_record->find("native_home")->as_string() == path_text(workspace.path()),
        "selection preserves its actual native storage scope in the opaque record");
    auto changed = selection(b, nonce_a, nonce_b);
    changed.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, a, nonce_a}};
    const auto cleared = agy::classify_event(changed);
    require(cleared && cleared->classification == addon::Classification::TRANSITION &&
        cleared->transition == addon::Transition_kind::RESUME && cleared->observations.front().provider_session_id == b,
        "an explicit native replacement selects B in the existing Logonomic session");
    auto returned = selection(a, nonce_b, nonce_return);
    returned.ledger = {2, addon::Ledger_transition{addon::Transition_kind::RESUME, b, nonce_b}};
    const auto resumed = agy::classify_event(returned);
    require(resumed && resumed->classification == addon::Classification::TRANSITION &&
        resumed->transition == addon::Transition_kind::RESUME && resumed->observations.front().provider_session_id == a,
        "returning to a previously selected A is an explicit RESUME, including an idle cached conversation");
    returned.ledger = {3, addon::Ledger_transition{addon::Transition_kind::RESUME, a, nonce_return}};
    returned.event.body.set("previous_activation_nonce", addon::Json_value(nonce_return));
    require(agy::classify_event(returned)->classification == addon::Classification::IGNORED,
        "selecting the unchanged native identity does not roll the epoch");
    changed.ledger = returned.ledger;
    require(agy::classify_event(changed)->classification == addon::Classification::IGNORED,
        "an old selection cannot replace a newer admitted activation");
    first.event.body.set("conversationId", addon::Json_value(b));
    require(!agy::classify_event(first), "conflicting native and classified identities are rejected");
    first = selection(a, "", nonce_a);
    first.event.body.set("previous_activation_nonce", addon::Json_value(7));
    require(!agy::classify_event(first), "a malformed previous activation is not an initial boundary");
    require(!agy::selected_conversation("I1108 10:36:09.061796    1884 conversation_manager.go:1546] Streaming subagent updates for " + b + '\n') &&
        !agy::selected_conversation("I1108 10:36:09.061796    1884 server.go:1251] Starting conversation update stream for " + b + '\n') &&
        !agy::selected_conversation(record(a).substr(0, record(a).size() - 1)),
        "child views, generic backend streams and partial records never select the root");

    auto hook = hook_event("NativeHook");
    hook.ledger = returned.ledger;
    hook.event.body = addon::Json_value::object();
    hook.event.body.set("activation_nonce", addon::Json_value(nonce_a));
    auto native = addon::Json_value::object();
    native.set("name", addon::Json_value("PreInvocation"));
    auto payload = addon::Json_value::object();
    payload.set("conversationId", addon::Json_value(a));
    payload.set("invocationNum", addon::Json_value(0));
    payload.set("initialNumSteps", addon::Json_value(1));
    native.set("body", payload);
    hook.event.body.set("event", native);
    require(agy::classify_event(hook)->classification == addon::Classification::IGNORED,
        "a late original-A hook cannot attach to the returned-A activation");
    hook.event.body.set("activation_nonce", addon::Json_value(nonce_return));
    require(agy::classify_event(hook)->classification == addon::Classification::EVENT,
        "a current source-captured hook applies within its existing epoch");
    payload.set("conversationId", addon::Json_value(b));
    native.set("body", payload);
    hook.event.body.set("event", native);
    require(agy::classify_event(hook)->classification == addon::Classification::IGNORED,
        "a child or departed provider identity cannot borrow the current activation");
    native.set("name", addon::Json_value("StatusLine"));
    hook.event.name = "StatusLine";
    payload = json("{\"conversationId\":\"" + a + "\",\"toolConfirmationPending\":true}");
    native.set("body", payload);
    hook.event.body.set("event", native);
    require(agy::classify_permission_snapshot(hook).base.classification == addon::Classification::EVENT,
        "permission snapshots remain same-activation observations");
    hook.event.body.set("activation_nonce", addon::Json_value(nonce_a));
    require(agy::classify_permission_snapshot(hook).base.classification == addon::Classification::IGNORED,
        "an original-A permission snapshot cannot affect returned A");
    hook.ledger = {};
    require(agy::classify_permission_snapshot(hook).base.classification == addon::Classification::IGNORED,
        "a status callback still cannot create an epoch");

    addon::Helper_environment environment;
    environment.session_capability = std::string(64, '1');
    environment.generation_nonce = std::string(32, '2');
    agy::Activation_file source(environment);
    require(source.initialize() && source.select({a, nonce_a, false}), "create private activation source");
    const auto captured = agy::capture_activation(source.path(), environment);
    require(captured && !captured->committed && captured->transition_nonce == nonce_a,
        "helper capture freezes source identity before asynchronous delivery");
    require(source.commit(nonce_a) && agy::await_activation(source.path(), environment, *captured),
        "a captured pending activation can wait for its exact admission");
    require(source.select({b, nonce_b, false}) && source.select({a, nonce_return, true}) &&
        captured->transition_nonce == nonce_a && !agy::await_activation(source.path(), environment, *captured),
        "the old capture cannot be upgraded to a later same-ID activation");
    auto other = environment;
    other.generation_nonce = std::string(32, '3');
    require(!agy::capture_activation(source.path(), other), "private activation evidence is generation-fenced");
    other = environment;
    other.session_capability = std::string(64, '4');
    require(!agy::capture_activation(source.path(), other), "private activation evidence belongs to one host session");
}

// Oracle: the host advances a root turn only for a working fact naming a new
// provider_turn_id (lifecycle-events@2), and agy restarts invocationNum at
// zero for every execution while initialNumSteps counts the conversation's
// steps when the execution began.
void turn_identity_contract()
{
    const auto invocation = [](const char* conversation, int number, int steps) {
        auto request = hook_event("PreInvocation");
        request.event.body.set("conversationId", addon::Json_value(conversation));
        request.event.body.set("invocationNum", addon::Json_value(number));
        request.event.body.set("initialNumSteps", addon::Json_value(steps));
        request.ledger.current_epoch = 1;
        request.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::START, conversation, std::string(32, 'b')};
        const auto result = agy::classify_event(request);
        require(result && result->observations.size() == 1 &&
            result->observations.front().phase_transition == addon::Phase_transition::WORKING,
            "every invocation reports the root working");
        return result->observations.front().provider_turn_id;
    };
    const auto first = invocation("Conversation-Parent", 0, 1);
    require(!first.empty(), "the first invocation of an execution names its root turn");
    require(invocation("Conversation-Parent", 0, 1) == first, "a redelivered turn start names the same turn");
    require(invocation("Conversation-Parent", 1, 1).empty() && invocation("Conversation-Parent", 2, 5).empty(),
        "later invocations of the execution cannot advance the turn again");
    const auto second = invocation("Conversation-Parent", 0, 12);
    require(!second.empty() && second != first, "the next execution starts a distinct turn");
    require(invocation("Conversation-Other", 0, 1) != first, "turns of another conversation are distinct");

    auto request = hook_event("PreInvocation");
    request.event.body.set("invocationNum", addon::Json_value(0));
    require(agy::classify_event(request)->observations.front().provider_turn_id.empty(),
        "without the step count no turn is named");
    request.event.body.set("initialNumSteps", addon::Json_value("1"));
    require(agy::classify_event(request)->observations.front().provider_turn_id.empty(),
        "a malformed step count names no turn");

    const auto payload = json(R"({"conversationId":"Conversation-Parent","invocationNum":0,
        "initialNumSteps":12,"transcriptPath":"/private/transcript.jsonl","modelName":"private-model"})");
    require(agy::sanitize_hook("PreInvocation", payload) ==
        json(R"({"conversationId":"Conversation-Parent","invocationNum":0,"initialNumSteps":12})"),
        "the helper forwards the invocation counters and nothing else");
}

void status_line_contract()
{
    auto request = hook_event("StatusLine");
    const auto native = json(R"({"conversation_id":"Conversation-Parent","agent_state":"tool_use",
        "tool_confirmation_pending":true,"email":"private@example.com","transcript_path":"/private/path"})");
    request.event.body = agy::sanitize_hook("StatusLine", native);
    require(request.event.body == json(R"({"conversationId":"Conversation-Parent","toolConfirmationPending":true})"),
        "permission snapshots retain only native identity and pending state");
    require(agy::classify_permission_snapshot(request).base.classification == addon::Classification::IGNORED,
        "a status callback cannot open the first session epoch");
    request.ledger.current_epoch = 2;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "Conversation-Parent", std::string(32, 'b')};
    for (const bool pending : {true, false, true}) {
        auto snapshot = native;
        if (!pending) {
            snapshot.erase("tool_confirmation_pending");
        }
        request.event.body = agy::sanitize_hook("StatusLine", snapshot);
        require(request.event.body.find("toolConfirmationPending")->as_boolean() == pending,
            "omitted native confirmation flag is an observed false level");
        const auto result = agy::classify_permission_snapshot(request);
        require(result.base.classification == addon::Classification::EVENT && result.base.observations.size() == 1 &&
            !result.base.transition, "each valid snapshot binds one observation without creating an epoch");
        const auto& observation = result.base.observations.front();
        require(observation.provider_session_id == "Conversation-Parent" &&
            observation.provider_turn_id.empty() && observation.attention_request_id.empty() &&
            observation.session_transition == addon::Session_transition::NONE &&
            observation.phase_transition == addon::Phase_transition::NONE &&
            observation.outcome_transition == addon::Outcome_transition::NONE &&
            observation.attention_transition == addon::Attention_transition::NONE,
            "the permission companion owns level pairing; the callback invents no request, turn, phase or outcome");
        const auto decoded = addon::decode_permission_snapshot_classification_result(
            addon::encode_permission_snapshot_classification_result(result), nullptr, 3);
        require(decoded && decoded->permission_snapshot && decoded->permission_snapshot->pending == pending &&
            decoded->permission_snapshot->version == 1 && decoded->permission_snapshot->observation_index == 0,
            "the SDK's strict companion decoder admits the emitted session-bound level");
        require(agy::classify_event(request)->classification == addon::Classification::IGNORED,
            "ordinary helper ingress cannot expose the snapshot without its negotiated companion");
    }
    for (const auto& body : {
        json(R"({"conversationId":"Conversation-Other","toolConfirmationPending":true})"),
        json(R"({"conversationId":"Conversation-Parent","toolConfirmationPending":"false"})"),
        json(R"({"conversationId":"Conversation-Parent"})"),
        json(R"({"conversationId":"Conversation-Parent","toolConfirmationPending":false,"unknown":true})")})
    {
        request.event.body = body;
        const auto ignored = agy::classify_permission_snapshot(request);
        require(ignored.base.classification == addon::Classification::IGNORED && ignored.base.observations.empty() &&
            !ignored.permission_snapshot,
            "stale and malformed snapshots cannot change attention or claim another conversation");
    }
}

std::unique_ptr<addon::Host_simulator> permission_adapter(const fs::path& package)
{
    addon::Simulator_options options;
    options.package_directory = package;
    auto adapter = std::make_unique<addon::Host_simulator>(std::move(options), nullptr);
    addon::Contract_failure failure;
    require(adapter->start(&failure), "the packaged adapter completes the SDK handshake");
    const auto live = adapter->live_capabilities();
    require(std::find(live.begin(), live.end(), addon::Capability_pair{
        addon::k_permission_snapshot_capability_id, addon::k_permission_snapshot_version}) != live.end(),
        "permission-snapshot must be declared and registered by the packaged adapter");
    return adapter;
}

void resume_inspection_adapter_contract(const fs::path& package)
{
    Workspace workspace;
    addon::Resume_inspect_request request;
    request.context.working_directory = path_text(workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = addon::host_platform_id();
#ifdef _WIN32
    constexpr auto home_name = "USERPROFILE";
    constexpr auto expanded_remote_script = "agy --remote-agent=%REMOTE%";
#else
    constexpr auto home_name = "HOME";
    constexpr auto expanded_remote_script = "agy --remote-agent=$REMOTE";
#endif
    request.context.launch_environment->effective_environment.set(home_name,
        addon::Json_value(path_text(workspace.path())));
    request.context.launch_environment->effective_environment.set("REMOTE", addon::Json_value("true"));
    request.resume_record = agy::encode_resume_identity({"absent-inspection-fixture", path_text(workspace.path())});
    auto adapter = permission_adapter(package);
    struct Inspection_case
    {
        const char* script;
        addon::Resume_availability availability;
        bool shell = false;
    };
    for (const auto& sample : {
        Inspection_case{expanded_remote_script, addon::Resume_availability::UNKNOWN, true},
        Inspection_case{"agy \"\"", addon::Resume_availability::UNKNOWN},
        Inspection_case{"agy --model \"\"", addon::Resume_availability::UNKNOWN},
        Inspection_case{"agy --model $MODEL", addon::Resume_availability::UNKNOWN},
        Inspection_case{"agy --model gemini-3-pro", addon::Resume_availability::ABSENT},
        Inspection_case{"agy --model --remote-agent", addon::Resume_availability::ABSENT},
        Inspection_case{"agy --remote-agent --project remote-project", addon::Resume_availability::UNKNOWN},
        Inspection_case{"agy --remote-agent=false", addon::Resume_availability::ABSENT}})
    {
        request.context.launcher = {sample.script, "direct", ""};
        const auto command = agy::recognize_launcher(sample.script);
        require(command.has_value(), "inspection fixture has a recognized native launcher");
        request.context.launcher_argv = {command->program};
        request.context.launcher_argv.insert(request.context.launcher_argv.end(),
            command->arguments.begin(), command->arguments.end());
        if (sample.shell) {
#ifdef _WIN32
            request.context.launcher.runner_key = "cmd";
            request.context.launcher_argv = {"cmd", "/c", sample.script};
#else
            request.context.launcher.runner_key = "bash";
            request.context.launcher_argv = {"bash", "-lc", sample.script};
#endif
        }
        for (const std::uint16_t version : {3, 4}) {
            // Unsupported argument shapes must not reach the literal-switch
            // reader or turn an unresolved invocation into proven local absence.
            const auto call = adapter->invoke({"interactive-resume", version}, addon::k_inspect_resume_operation,
                addon::encode_resume_inspect_request(request), 5000);
            const auto outcome = adapter->wait_outcome(call, 6000);
            const auto result = outcome ? addon::decode_resume_inspect_result(outcome->payload, nullptr, version) : std::nullopt;
            const bool valid = outcome && outcome->status == addon::Host_status::SUCCESS && result &&
                addon::validate_resume_inspection(request, *result, nullptr) &&
                result->availability == sample.availability && result->content == addon::Resume_content::UNKNOWN;
            if (!valid) {
                const auto exit_code = adapter->exit_code();
                std::cerr << "Inspection failure: script=" << addon::serialize_json(addon::Json_value(sample.script))
                    << " protocol=interactive-resume@" << version
                    << " host_status=" << (outcome ? addon::host_status_name(outcome->status) : "no-outcome")
                    << " reason=" << (outcome ? outcome->reason_code : "no-outcome")
                    << " payload=" << (outcome ? addon::serialize_json(outcome->payload) : "unavailable")
                    << " adapter_exit=" << (exit_code ? std::to_string(*exit_code) : "unavailable")
                    << " exited_by_signal=" << adapter->exited_by_signal() << '\n';
                (void)adapter->shutdown(1000, nullptr);
            }
            require(valid,
                "packaged inspection keeps unsupported and remote scopes unknown and acknowledges the exact request");
            require(version != 3 || !outcome->payload.find("content"),
                "packaged version three inspection retains its availability-only response");
        }
    }
#ifdef _WIN32
    auto home = workspace.path();
    while (path_text(home).size() < 1200) {
        home /= "retained-native-profile-with-a-long-configured-storage-directory";
    }
    const std::string identity = "long-path-native-inspection";
    const auto database_path = resume_database(home, identity);
    request.context.launcher = {"agy", "direct", ""};
    request.context.launcher_argv = {"agy"};
    request.context.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(home)));
    request.resume_record = agy::encode_resume_identity({identity, path_text(home)});
    const auto native_bytes = [&] {
        std::ifstream input(addon::usage_cache_io_path(database_path), std::ios::binary);
        require(input.good(), "read the existing long-path native history fixture");
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto inspect_long_path = [&](addon::Resume_content content) {
        const auto before = native_bytes();
        for (const auto version : {std::uint16_t{3}, std::uint16_t{4}}) {
            const auto call = adapter->invoke({"interactive-resume", version}, addon::k_inspect_resume_operation,
                addon::encode_resume_inspect_request(request), 5000);
            const auto outcome = adapter->wait_outcome(call, 6000);
            const auto result = outcome ? addon::decode_resume_inspect_result(outcome->payload, nullptr, version) : std::nullopt;
            require(outcome && outcome->status == addon::Host_status::SUCCESS && result &&
                addon::validate_resume_inspection(request, *result, nullptr) &&
                result->availability == addon::Resume_availability::AVAILABLE &&
                result->content == (version == 4 ? content : addon::Resume_content::UNKNOWN),
                "existing long-path native history is available at both inspection versions");
            require(version != 3 || !outcome->payload.find("content"),
                "long-path version three inspection retains its availability-only shape");
        }
        require(native_bytes() == before, "long-path inspection leaves native database bytes unchanged");
    };
    inspect_long_path(addon::Resume_content::EMPTY);
    sqlite3* writer = nullptr;
    require(sqlite3_open_v2(path_text(addon::usage_cache_io_path(database_path)).c_str(), &writer,
        SQLITE_OPEN_READWRITE, "win32-longpath") == SQLITE_OK, "open long-path native fixture writer");
    require(sqlite3_exec(writer,
        "INSERT INTO steps(idx,step_type,step_payload) VALUES(0,14,X'080e9a0107120568656c6c6f')",
        nullptr, nullptr, nullptr) == SQLITE_OK, "retain native user text at a long storage path");
    sqlite3_close(writer);
    inspect_long_path(addon::Resume_content::NONEMPTY);
    request.resume_record = agy::encode_resume_identity({"missing-long-path-native-inspection", path_text(home)});
    const auto missing_call = adapter->invoke({"interactive-resume", 4}, addon::k_inspect_resume_operation,
        addon::encode_resume_inspect_request(request), 5000);
    const auto missing_outcome = adapter->wait_outcome(missing_call, 6000);
    const auto missing = missing_outcome ? addon::decode_resume_inspect_result(missing_outcome->payload, nullptr, 4) : std::nullopt;
    require(missing_outcome && missing_outcome->status == addon::Host_status::SUCCESS && missing &&
        addon::validate_resume_inspection(request, *missing, nullptr) &&
        missing->availability == addon::Resume_availability::ABSENT,
        "a genuinely missing long-path native selector remains absent");
    fs::remove_all(addon::usage_cache_io_path(home));
#endif
    int exit_code = -1;
    require(adapter->shutdown(1000, &exit_code) && exit_code == 0, "resume inspection adapter shuts down");
}

void native_selection_preparation_contract(const fs::path& package, const char* executable)
{
    Workspace workspace;
#ifdef _WIN32
    const auto native_program = workspace.path() / "agy.exe";
    constexpr auto home_name = "USERPROFILE";
    const auto bridge = fs::absolute(package) / "helpers" / "agy_resume_bridge.exe";
#else
    const auto native_program = workspace.path() / "agy";
    constexpr auto home_name = "HOME";
    const auto bridge = fs::absolute(package) / "helpers" / "agy_resume_bridge";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    const auto program = path_text(native_program);
    const std::string identity = "8cd7f43f-0eb0-4097-b337-e449322f2d28";
    addon::Prepare_request request;
    request.working_directory = path_text(workspace.path());
    request.launch_environment = addon::Launch_environment{};
    request.launch_environment->platform_id = addon::host_platform_id();
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    request.launch_environment->effective_environment.set("AGY_TEST_VERSION", addon::Json_value("1.5.3"));
    auto adapter = permission_adapter(package);
    const auto live = adapter->live_capabilities();
    require(std::find(live.begin(), live.end(), addon::Capability_pair{
        addon::k_model_context_delivery_capability_id, addon::k_model_context_delivery_activation_version}) != live.end(),
        "native activation context must be declared and registered by the packaged adapter");
    for (const bool separated : {false, true}) {
        // The native flag parser accepts both spellings. Preparation must keep
        // the lifetime observer when a launcher initially selects an exact ID.
        request.launcher = {"\"" + program + "\" --conversation" + (separated ? " " : "=") + identity, "direct", ""};
        request.launcher_argv = separated ? std::vector<std::string>{program, "--conversation", identity}
                                         : std::vector<std::string>{program, "--conversation=" + identity};
        const auto call = adapter->invoke({"launch-preparation", 3}, addon::k_prepare_operation,
            addon::encode_prepare_request(request, 3), 5000);
        const auto outcome = adapter->wait_outcome(call, 6000);
        const auto patch = outcome ? addon::decode_launch_patch(outcome->payload, nullptr, 3) : std::nullopt;
        auto expected = request.launcher_argv;
        expected.insert(expected.begin(), path_text(bridge));
        require(outcome && outcome->status == addon::Host_status::SUCCESS && patch &&
            patch->argv == expected && patch->disposition && patch->disposition->lifecycle_events &&
            std::find(patch->set.begin(), patch->set.end(),
                std::pair<std::string, std::string>{"AGY_LOGONOMIC_OBSERVE_SELECTION", "1"}) != patch->set.end(),
            "both exact conversation selector spellings preserve the packaged lifetime observer");
    }
    request.launcher = {"\"" + program + "\" \"\"", "direct", ""};
    request.launcher_argv = {program, ""};
    const auto unsupported_call = adapter->invoke({"launch-preparation", 3}, addon::k_prepare_operation,
        addon::encode_prepare_request(request, 3), 5000);
    const auto unsupported = adapter->wait_outcome(unsupported_call, 6000);
    const auto retained = unsupported ? addon::decode_launch_patch(unsupported->payload, nullptr, 3) : std::nullopt;
    require(unsupported && unsupported->status == addon::Host_status::SUCCESS && retained && !retained->argv &&
        std::none_of(retained->set.begin(), retained->set.end(),
            [](const auto& item) { return item.first == "AGY_LOGONOMIC_OBSERVE_SELECTION"; }),
        "an unsupported empty argument keeps the original launch without installing a selection observer");
    int exit_code = -1;
    require(adapter->shutdown(1000, &exit_code) && exit_code == 0, "selection preparation adapter shuts down");
}

addon::Host_outcome invoke_permission(
    addon::Host_simulator& adapter, std::string operation, addon::Json_value payload)
{
    const auto id = adapter.invoke({addon::k_permission_snapshot_capability_id,
        addon::k_permission_snapshot_version}, std::move(operation), std::move(payload), 5000);
    const auto outcome = adapter.wait_outcome(id, 6000);
    require(outcome.has_value(), "the packaged permission operation produces a bounded result");
    return *outcome;
}

void permission_snapshot_adapter_contract(const fs::path& package)
{
    // Exercise sidecar/handshake intersection and the actual adapter dispatch,
    // not only the provider functions behind the two companion operations.
    auto adapter = permission_adapter(package);
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(addon::sidecar_relative_path(addon::k_permission_snapshot_capability_id,
        addon::k_permission_snapshot_version), addon::limits::k_max_sidecar_bytes, nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_permission_snapshot_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration && declaration->event_name == "StatusLine" &&
        declaration->relative_path == ".gemini/antigravity-cli/settings.json",
        "permission observation declares the native settings target and callback event");

    const std::string original = R"({"statusLine":{"command":"echo status","enabled":false},"theme":"user"})";
    const auto transformed = invoke_permission(*adapter, addon::k_managed_configuration_transform_operation,
        addon::encode_managed_configuration_transform_request({std::string(32, 'd'), true, original}));
    const auto replacement = addon::decode_managed_configuration_transform_result(transformed.payload, nullptr);
    require(transformed.status == addon::Host_status::SUCCESS && replacement && replacement->grant == std::string(32, 'd'),
        "the settings operation echoes the host grant through the live adapter");
    const auto settings = json(replacement->content);
    require(settings.find("theme")->as_string() == "user" &&
        !settings.find("statusLine")->find("enabled")->as_boolean() &&
        settings.find("logonomic-statusline-relay")->find("original")->as_string() ==
            R"({"command":"echo status","enabled":false})",
        "live settings transformation preserves the user's disabled command and unrelated options");
    const auto refused = invoke_permission(*adapter, addon::k_managed_configuration_transform_operation,
        addon::encode_managed_configuration_transform_request({std::string(32, 'd'), true, "[]"}));
    require(refused.status == addon::Host_status::FAILED && refused.reason_code == "invalid_status_line_configuration",
        "uninterpretable settings refuse replacement through the live adapter");

    auto request = hook_event("StatusLine");
    request.ledger.current_epoch = 2;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "Conversation-Parent", std::string(32, 'b')};
    for (const bool pending : {true, false, true}) {
        request.event.body.set("toolConfirmationPending", addon::Json_value(pending));
        const auto classified = invoke_permission(*adapter, addon::k_classify_event_operation,
            addon::encode_classify_event_request(request));
        const auto result = addon::decode_permission_snapshot_classification_result(classified.payload, nullptr, 3);
        require(classified.status == addon::Host_status::SUCCESS && result && result->permission_snapshot &&
            result->permission_snapshot->pending == pending && result->permission_snapshot->observation_index == 0 &&
            result->base.observations.front().provider_session_id == "Conversation-Parent",
            "live classification preserves every pending, clear and repeated pending level");
    }
    for (const auto& body : {
        json(R"({"conversationId":"Conversation-Other","toolConfirmationPending":false})"),
        json(R"({"conversationId":"Conversation-Parent","toolConfirmationPending":"false"})"),
        json(R"({"conversationId":"Conversation-Parent"})")})
    {
        request.event.body = body;
        const auto classified = invoke_permission(*adapter, addon::k_classify_event_operation,
            addon::encode_classify_event_request(request));
        const auto result = addon::decode_permission_snapshot_classification_result(classified.payload, nullptr, 3);
        require(classified.status == addon::Host_status::SUCCESS && result && !result->permission_snapshot &&
            result->base.classification == addon::Classification::IGNORED,
            "the live companion cannot clear from stale, malformed or incomplete evidence");
    }
    request = hook_event("PreInvocation");
    const auto ordinary = invoke_permission(*adapter, addon::k_classify_event_operation,
        addon::encode_classify_event_request(request));
    const auto ignored = addon::decode_permission_snapshot_classification_result(ordinary.payload, nullptr, 3);
    require(ordinary.status == addon::Host_status::SUCCESS && ignored &&
        ignored->base.classification == addon::Classification::IGNORED && !ignored->permission_snapshot,
        "the permission companion does not reclassify ordinary lifecycle events");
    const auto unknown = invoke_permission(*adapter, "unknown", addon::Json_value::object());
    require(unknown.status == addon::Host_status::UNAVAILABLE && unknown.reason_code == addon::reason::k_unknown_operation,
        "the new capability rejects an unknown operation");
    int exit_code = -1;
    require(adapter->shutdown(1000, &exit_code) && exit_code == 0,
        "the capability fixture leaves no adapter process running");
}

void lifecycle_contract()
{
    auto request = hook_event("PreInvocation");
    auto result = agy::classify_event(request);
    require(result && result->classification == addon::Classification::TRANSITION, "first hook anchors conversation");
    require(result->observations.front().phase_evidence == addon::Evidence::INFERRED, "hook timing is inferred phase evidence");
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "Conversation-Parent", std::string(32, 'b')};
    result = agy::classify_event(request);
    require(result && result->classification == addon::Classification::EVENT, "repeated invocation retains host epoch");
    request.event.name = "Stop";
    request.event.body.set("fullyIdle", addon::Json_value(true));
    request.event.body.set("executionNum", addon::Json_value(7));
    result = agy::classify_event(request);
    require(result && result->observations.front().phase_transition == addon::Phase_transition::AWAITING_INPUT,
        "fully idle hands control to user");
    require(result->observations.front().session_transition == addon::Session_transition::NONE,
        "fully idle does not end a continuing conversation");
    require(result->observations.front().outcome_transition == addon::Outcome_transition::NONE,
        "no stable turn identity means no root turn outcome claim");
    require(result->subagent_evidence &&
        result->subagent_evidence->kind == addon::Subagent_evidence_kind::RECONCILE &&
        result->subagent_evidence->provider_event_id == result->observations.front().provider_event_id,
        "authoritative idle evidence asks host to reconcile children without ending root");
    const auto stop_identity = result->observations.front().provider_event_id;
    request.transition_nonce = std::string(32, 'd');
    result = agy::classify_event(request);
    require(result->observations.front().provider_event_id == stop_identity,
        "semantic Stop identity is independent of helper delivery nonce");
    request.event.body.erase("executionNum");
    result = agy::classify_event(request);
    require(result->subagent_evidence && result->subagent_evidence->kind == addon::Subagent_evidence_kind::LOSS &&
        !result->lifecycle_degradation, "missing Stop authority degrades child evidence only");
    request.event.body.set("fullyIdle", addon::Json_value(false));
    result = agy::classify_event(request);
    require(result && result->observations.front().phase_transition == addon::Phase_transition::NONE,
        "non-idle Stop makes no idle claim");
    request.event.body.set("conversationId", addon::Json_value(""));
    require(!agy::classify_event(request), "modeled hook needs a stable bounded conversation identity");
    request.event.name = "UnmodeledEvent";
    result = agy::classify_event(request);
    require(result && result->classification == addon::Classification::IGNORED, "unmodeled hook is not degradation");

    request = hook_event("PostToolUse");
    auto batch = json(R"({"present":true,"version":1,"stepIdx":9,"children":[]})");
    for (int index = 0; index < 32; ++index) {
        auto child = addon::Json_value::object();
        child.set("conversationId", addon::Json_value("Conversation-Child-" + std::to_string(index)));
        batch.find("children")->push_back(std::move(child));
    }
    request.event.body.set("logonomicAgySubagentObservation", batch);
    result = agy::classify_event(request);
    require(result && result->observations.size() == 32 &&
        result->classification == addon::Classification::TRANSITION,
        "first32-child hook remains one atomic result with root identity on first child");
    require(result->subagent_evidence && result->subagent_evidence->batch_id == "agy-step-9" &&
        result->subagent_evidence->child_ids.size() == 32, "complete batch identity goes to host dedup ledger");
    addon::Contract_failure failure;
    require(addon::decode_classify_event_result(addon::encode_classify_event_result(*result), &failure, 3).has_value(),
        "full atomic child batch obeys helper3 public contract");
    request.event.body.set("logonomicAgySubagentObservation", json(R"({"present":false,"version":1})"));
    result = agy::classify_event(request);
    require(result && result->subagent_evidence &&
        result->subagent_evidence->kind == addon::Subagent_evidence_kind::LOSS && !result->lifecycle_degradation,
        "unavailable transcript evidence does not erase valid root lifecycle");
}

// Oracle: the host's feature gate. It rejects an observation carrying a fact
// the package's own declaration leaves unsupported, phase and all.
void declared_profile_admits_every_shape(const fs::path& package)
{
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    require(declaration.has_value(), "package lifecycle declaration is readable");
    require(declaration->features.attention == addon::Feature_support::SUPPORTED &&
        declaration->features.paired_attention == addon::Feature_support::SUPPORTED,
        "the declared profile admits the host-owned permission episode");
    const auto admitted = [&declaration](const addon::Classify_event_request& request, const char* shape) {
        const auto result = agy::classify_event(request);
        require(result && !result->observations.empty(), shape);
        for (const auto& observation : result->observations) {
            require(addon::lifecycle_profile_admits(declaration->features, observation), shape);
        }
    };
    auto request = hook_event("PreInvocation");
    admitted(request, "the declaration refuses the first hook of a conversation");
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "Conversation-Parent", std::string(32, 'b')};
    admitted(request, "the declaration refuses a turn start");
    request.event.body.set("invocationNum", addon::Json_value(0));
    request.event.body.set("initialNumSteps", addon::Json_value(12));
    admitted(request, "the declaration refuses a turn start that names its root turn");
    request.event.name = "Stop";
    request.event.body.set("fullyIdle", addon::Json_value(true));
    request.event.body.set("executionNum", addon::Json_value(7));
    admitted(request, "the declaration refuses a turn completion");
    request.event.body.set("fullyIdle", addon::Json_value(false));
    admitted(request, "the declaration refuses a stop that is not idle");
    request = hook_event("PostToolUse");
    request.event.body.set("logonomicAgySubagentObservation",
        json(R"({"present":true,"version":1,"stepIdx":9,"children":[{"conversationId":"Conversation-Child"}]})"));
    admitted(request, "the declaration refuses a subagent start");
    request = hook_event("NativeConversation");
    Workspace workspace;
    constexpr auto selected = "8cd7f43f-0eb0-4097-b337-e449322f2d28";
    request.event.body = addon::Json_value::object();
    request.event.body.set("conversationId", addon::Json_value(selected));
    request.event.body.set("native_record", addon::Json_value(std::string(
        "I1108 10:36:09.061796    1884 conversation_manager.go:1107] Streaming conversation ") + selected + "\n"));
    request.event.body.set("native_home", addon::Json_value(path_text(workspace.path())));
    request.event.body.set("previous_activation_nonce", addon::Json_value(""));
    admitted(request, "the declaration refuses an idle native conversation selection");
}

void transcript_contract()
{
    Workspace workspace;
    const auto transcript = workspace.path() / "Conversation-Parent" / "transcript.jsonl";
    fs::create_directories(transcript.parent_path());
    auto payload = json(R"({"conversationId":"Conversation-Parent","stepIdx":3,
        "toolCall":{"name":"invoke_subagent","args":{"Subagents":[{"Prompt":"private prompt"}]}}})");
    payload.set("transcriptPath", addon::Json_value(path_text(transcript)));
    auto planned = json(R"({"step_index":3,"type":"PLANNER_RESPONSE","tool_calls":[{"name":"invoke_subagent"}]})");
    auto result = json(R"({"step_index":4,"type":"GENERIC"})");
    result.set("content", addon::Json_value(
        "Child started {\"conversationId\":\"Conversation-Child\","
        "\"logAbsoluteUri\":\"file:///private/Conversation-Child/transcript.jsonl\"}"));
    {
        std::ofstream stream(transcript, std::ios::binary);
        stream << addon::serialize_json(planned) << '\n' << addon::serialize_json(result) << '\n';
    }
    auto sanitized = agy::sanitize_hook("PostToolUse", payload);
    const auto* observation = sanitized.find("logonomicAgySubagentObservation");
    require(observation && observation->find("present")->as_boolean(), "completed direct child extracted from adjacent records");
    const auto bytes = addon::serialize_json(sanitized);
    require(bytes.find("private") == std::string::npos && bytes.find("transcript") == std::string::npos,
        "transcript locations and prompts never leave helper");
    const auto records = addon::serialize_json(planned) + '\n' + addon::serialize_json(result);
    for (const auto& prefix : {std::string{}, std::string(1024 * 1024, 'x') + '\n'}) {
        for (const auto* ending : {"", "\n"}) {
            {
                std::ofstream stream(transcript, std::ios::binary);
                stream << prefix << records << ending;
            }
            sanitized = agy::sanitize_hook("PostToolUse", payload);
            require(sanitized.find("logonomicAgySubagentObservation")->find("present")->as_boolean(),
                "short and windowed transcripts accept complete final records with or without a newline");
        }
    }
    {
        std::ofstream stream(transcript, std::ios::binary);
    }
    sanitized = agy::sanitize_hook("PostToolUse", payload);
    require(!sanitized.find("logonomicAgySubagentObservation")->find("present")->as_boolean(),
        "an empty transcript cannot establish child evidence");
    for (const auto* uri : {"file://[broken/Conversation-Child",
            "file://[::::]/Conversation-Child", "file://[:::]/Conversation-Child",
            "file://host:bad/Conversation-Child"})
    {
        auto child = json(R"({"conversationId":"Conversation-Child"})");
        child.set("logAbsoluteUri", addon::Json_value(uri));
        result.set("content", addon::Json_value(addon::serialize_json(child)));
        {
            std::ofstream stream(transcript, std::ios::binary);
            stream << addon::serialize_json(planned) << '\n' << addon::serialize_json(result) << '\n';
        }
        sanitized = agy::sanitize_hook("PostToolUse", payload);
        require(!sanitized.find("logonomicAgySubagentObservation")->find("present")->as_boolean(),
            "malformed local-file authority cannot establish a child identity");
    }
    for (const auto* uri : {"file://[::1]/Conversation-Child",
            "file://[2001:db8::1]:80/Conversation-Child",
            "file://[::ffff:127.0.0.1]/Conversation-Child"})
    {
        auto child = json(R"({"conversationId":"Conversation-Child"})");
        child.set("logAbsoluteUri", addon::Json_value(uri));
        result.set("content", addon::Json_value(addon::serialize_json(child)));
        {
            std::ofstream stream(transcript, std::ios::binary);
            stream << addon::serialize_json(planned) << '\n' << addon::serialize_json(result) << '\n';
        }
        sanitized = agy::sanitize_hook("PostToolUse", payload);
        require(sanitized.find("logonomicAgySubagentObservation")->find("present")->as_boolean(),
            "valid IPv6 local-file authority preserves child evidence");
    }
    payload.set("stepIdx", addon::Json_value(8));
    sanitized = agy::sanitize_hook("PostToolUse", payload);
    require(!sanitized.find("logonomicAgySubagentObservation")->find("present")->as_boolean(),
        "unproven invoke result explicitly reports observation loss");
    payload.find("toolCall")->set("name", addon::Json_value("read_file"));
    sanitized = agy::sanitize_hook("PostToolUse", payload);
    require(!sanitized.find("logonomicAgySubagentObservation"), "unrelated tool emits no child observation");
}

std::unique_ptr<Invocation> invoke_hook_transform(bool present, const std::string& content, bool cancelled = false)
{
    auto invocation = std::make_unique<Invocation>(addon::k_managed_configuration_transform_operation,
        addon::encode_managed_configuration_transform_request({std::string(32, 'd'), present, content}),
        cancelled, addon::Capability_pair{"managed-configuration", 3});
    agy::handle_preparation(*invocation);
    return invocation;
}

void hook_transform_contract(const fs::path& package)
{
    // Oracle: the adopted automatic repair contract and RFC 8259 syntax.
    // Unrelated values retain their exact source bytes, including numbers the
    // SDK's int64/double data model cannot represent.
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    addon::Contract_failure failure;
    const auto transform = [&failure](bool present, const std::string& content) {
        const auto invocation = invoke_hook_transform(present, content);
        const auto result = addon::decode_managed_configuration_transform_result(invocation->result(), &failure);
        require(invocation->status() == addon::Result_status::SUCCESS && result && result->grant == std::string(32, 'd'),
            "successful pathless transform returns only the transformed content and exact opaque grant");
        return result->content;
    };
    const auto supported = agy::hook_definition(windows);
    const auto stanza = addon::serialize_json(supported);
    const auto missing = json(transform(false, ""));
    require(missing.as_object().size() == 1 && missing.find("logonomic-hook-relay") &&
        addon::serialize_json_canonical(*missing.find("logonomic-hook-relay")) ==
        addon::serialize_json_canonical(supported), "missing configuration becomes the matching packaged helper stanza");
    for (const auto* original : {"{}", " \r\n{ \t}\r\n", "{\"user-owned\":null}"}) {
        const auto repaired = json(transform(true, original));
        require(repaired.find("logonomic-hook-relay") && repaired.as_object().size() ==
            (std::string_view(original).find("user-owned") == std::string_view::npos ? 1u : 2u),
            "empty and valid nonempty objects accept the single owned member");
    }
    const std::string unrelated =
        "  \"user-owned\" : {\"budget\":9007199254740993,\"huge\":184467440737095516160,"
        "\"ratio\":1.2300e+4,\"unbounded\":1E+9999,\"tiny\":-0.00e-9999,"
        "\"nested\":[true,false,null,{\"escaped\":\"quote\\\" and slash\\\\\"}]}";
    const std::string prefix = "\r\n{\n" + unrelated + ",\n  \"logonomic\\u002dhook-relay\" : ";
    const std::string suffix = " ,\n  \"tail\": [1,2,3]\n}\t\n";
    const std::string original = prefix + "[null,{\"stale\":true}]" + suffix;
    const std::string repaired = prefix + stanza + suffix;
    require(transform(true, original) == repaired,
        "stale owned value is replaced as a unit while every other byte and escaped key remains exact");
    require(transform(true, repaired) == repaired,
        "exact supported member is a byte-preserving no-op after full document validation");
    require(invoke_hook_transform(true, "{\"logonomic-hook-relay\":" + stanza +
        ",\"user\":1,\"user\":2}")->status() == addon::Result_status::FAILED,
        "exact supported hooks do not bypass validation of unrelated duplicate members");
    const auto without_owned = "{\n" + unrelated + "\n}\n";
    require(transform(true, without_owned) == "{\n" + unrelated + "\n,\"logonomic-hook-relay\":" + stanza + "}\n",
        "absent owned member appends without changing unrelated numeric lexemes or formatting");

    for (const auto* content : {"", "[]", "null", "{", "{} trailing", "{\"user\":01}",
        "{\"user\":1.}", "{\"user\":1e+}", "{\"user\":--1}", "{\"user\":true,}",
        "{\"user\":[false,]}", "{\"a\":1,\"\\u0061\":2}", "{\"user\":{\"a\":1,\"a\":2}}",
        "{\"user\":\"\\uD800\"}", "{\"user\":\"\\x20\"}", "{\"user\":\"line\nbreak\"}"})
    {
        const auto invocation = invoke_hook_transform(true, content);
        require(invocation->status() == addon::Result_status::FAILED &&
            invocation->reason() == "invalid_hook_configuration",
            "malformed, nonobject, duplicate-name and invalid-string configurations return explicit failure");
    }
    for (const auto& content : {
            std::string("{\"user\":\"") + char(0xff) + "\"}",
            "{\"user\":" + std::string(addon::limits::k_max_json_depth, '[') + "0" +
                std::string(addon::limits::k_max_json_depth, ']') + "}"})
    {
        const auto invocation = invoke_hook_transform(true, content);
        require(invocation->status() == addon::Result_status::FAILED &&
            invocation->reason() == "invalid_hook_configuration", "invalid UTF-8 and over-depth JSON are refused");
    }
    constexpr std::size_t max_hooks_bytes = 256 * 1024;
    for (const auto& content : {std::string(max_hooks_bytes + 1, ' '),
        "{\"user\":\"" + std::string(max_hooks_bytes - 20, 'x') + "\"}"})
    {
        const auto invocation = invoke_hook_transform(true, content);
        require(invocation->status() == addon::Result_status::FAILED &&
            invocation->reason() == "hook_configuration_too_large", "both input and transformed output are bounded");
    }
    require(invoke_hook_transform(false, "", true)->status() == addon::Result_status::CANCELLED,
        "cancelled transforms return no replacement");
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(addon::sidecar_relative_path("managed-configuration", 3),
        addon::limits::k_max_sidecar_bytes, nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar
        ? addon::decode_managed_configuration_declaration(sidecar->payload, &failure, 3) : std::nullopt;
    require(declaration && declaration->relative_path == ".gemini/config/hooks.json",
        "the package declares the fixed frozen-home host-owned recovery-capable transform");
}

void status_line_transform_contract()
{
    // Oracle: Antigravity's statusLine command receives JSON stdin and owns
    // stdout. The managed observer must retain that command and its options.
    for (const bool windows : {false, true}) {
        const auto transform = [windows](bool present, const std::string& content) {
            std::string reason;
            const auto result = agy::transform_status_line(present, content, windows, reason);
            require(result.has_value() && reason.empty(), "valid status settings admit the observer transform");
            return *result;
        };
        const auto empty = transform(false, "");
        const auto missing = json(empty);
        const auto* status = missing.find("statusLine");
        require(status && status->find("command")->as_string() == agy::status_line_command(windows, "") &&
            status->find("stack_with_default")->as_boolean() && !status->find("padding"),
            "an observer with no user command retains the built-in status line");
        require(missing.find("logonomic-statusline-relay")->find("original")->is_null(),
            "an absent original is retained explicitly");
        require(transform(true, empty) == empty, "repeated installation never nests the observer");
        auto edited_default = missing;
        edited_default.find("statusLine")->set("padding", addon::Json_value(3));
        edited_default.find("statusLine")->set("enabled", addon::Json_value(false));
        const auto default_repaired = transform(true, addon::serialize_json(edited_default));
        const auto default_configuration = json(default_repaired);
        const auto default_original = json(
            default_configuration.find("logonomic-statusline-relay")->find("original")->as_string());
        require(default_original.find("padding")->as_integer() == 3 &&
            !default_original.find("enabled")->as_boolean() &&
            transform(true, default_repaired) == default_repaired,
            "display edits and native disable survive repair when no custom command existed");

        for (const std::string original : {
            "false", "null", "{\"enabled\":false,\"command\":\"exit 37\",\"padding\":4}"})
        {
            const auto result = transform(true, "{\"statusLine\":" + original + "}");
            const auto parsed = json(result);
            const bool disabled = original.find("enabled") != std::string::npos;
            require(parsed.find("logonomic-statusline-relay")->find("original")->as_string() == original &&
                parsed.find("statusLine")->find("command")->as_string() ==
                    agy::status_line_command(windows, disabled ? "exit 37" : ""),
                "disabled scripts retain a callable original for native re-enable");
            require(!disabled || (!parsed.find("statusLine")->find("enabled")->as_boolean() &&
                parsed.find("statusLine")->find("padding")->as_integer() == 4),
                "native disabled state and display settings remain unchanged");
            require(transform(true, result) == result, "inactive custom scripts remain inactive on repair");
        }

        const std::string original =
            "{ \"command\" : \"printf \\\"%s\\\\n\\\" 'user status'\", \"padding\":2, \"stack_with_default\":true }";
        const std::string unrelated =
            "\"user\" : {\"huge\":184467440737095516160,\"ratio\":1.2300e+4,\"tiny\":1E-9999}";
        const std::string source = "{\n  " + unrelated + ",\n  \"statusLine\":" + original + "\n}\n";
        const auto result = transform(true, source);
        require(result.find(unrelated) != std::string::npos && result.find("\"padding\":2") != std::string::npos,
            "unrelated settings and custom display options retain their exact bytes");
        require(transform(true, result) == result, "retained original command spelling is byte-stable");

        auto installed = json(transform(true, "{\"statusLine\":" + original + "}"));
        installed.find("statusLine")->set("padding", addon::Json_value(5));
        installed.find("statusLine")->set("enabled", addon::Json_value(false));
        const auto disabled = json(transform(true, addon::serialize_json(installed)));
        const auto retained = json(disabled.find("logonomic-statusline-relay")->find("original")->as_string());
        require(!retained.find("enabled")->as_boolean() && retained.find("padding")->as_integer() == 5 &&
            !disabled.find("statusLine")->find("enabled")->as_boolean() &&
            disabled.find("statusLine")->find("command")->as_string() ==
                agy::status_line_command(windows, json(original).find("command")->as_string()),
            "disabling an installed script preserves the native switch and callable command");
        auto reenabled = disabled;
        reenabled.find("statusLine")->set("enabled", addon::Json_value(true));
        reenabled.find("statusLine")->set("padding", addon::Json_value(6));
        reenabled.find("statusLine")->set("theme", addon::Json_value("user choice"));
        const auto repaired = json(transform(true, addon::serialize_json(reenabled)));
        const auto changed_original = json(repaired.find("logonomic-statusline-relay")->find("original")->as_string());
        require(changed_original.find("enabled")->as_boolean() && changed_original.find("padding")->as_integer() == 6 &&
            changed_original.find("theme")->as_string() == "user choice" &&
            repaired.find("statusLine")->find("enabled")->as_boolean(),
            "native re-enable and edits made while disabled survive subsequent repair");

        installed.find("statusLine")->set("command", addon::Json_value("echo replacement"));
        installed.find("statusLine")->erase("enabled");
        const auto replacement = json(transform(true, addon::serialize_json(installed)));
        require(replacement.find("statusLine")->find("command")->as_string() ==
            agy::status_line_command(windows, "echo replacement"),
            "a user's replacement command supersedes the retained predecessor");

        auto orphaned = json(empty);
        orphaned.erase("logonomic-statusline-relay");
        std::string reason;
        require(!agy::transform_status_line(true, addon::serialize_json(orphaned), windows, reason) &&
            reason == "invalid_status_line_relay", "a wrapper without its original is never wrapped recursively");
        for (const auto* invalid : {"[]", "{\"statusLine\":{\"command\":12}}", "{\"statusLine\":{\"command\":1E9999}}",
            "{\"statusLine\":{\"enabled\":\"false\"}}", "{\"statusLine\":{\"type\":\"other\"}}",
            "{\"statusLine\":{},\"statusLine\":{}}", "{\"statusLine\":{\"command\":\"a\\u0000b\"}}"})
        {
            require(!agy::transform_status_line(true, invalid, windows, reason) &&
                reason == "invalid_status_line_configuration",
                "uninterpretable status settings are preserved by refusal");
        }
        require(!agy::transform_status_line(true, std::string(256 * 1024 + 1, ' '), windows, reason) &&
            reason == "status_line_configuration_too_large", "settings transform has a bounded input and output");
        if (windows) {
            for (const auto length : {1500, 7000}) {
                const auto long_command = addon::serialize_json(addon::Json_value("echo " + std::string(length, 'x')));
                const auto long_original = "{\"command\":" + long_command + "}";
                const auto long_settings = json(transform(true, "{\"statusLine\":" + long_original + "}"));
                require(long_settings.find("statusLine")->find("command")->as_string().size() <= 8000 &&
                    long_settings.find("logonomic-statusline-relay")->find("original")->as_string() == long_original,
                    "runnable user commands stay intact without enlarging the installed Windows invocation");
            }
        }
    }
}

void hook_root_member_contract()
{
    // The advertised member bound applies to the complete replacement too.
    // Installation must not create a file that its next preparation refuses.
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    const auto root_document = [](std::size_t unrelated_members, const std::optional<std::string>& owned_value) {
        std::string content = "{";
        for (std::size_t index = 0; index < unrelated_members; ++index) {
            if (index != 0) {
                content += ',';
            }
            content += "\"user-" + std::to_string(index) + "\":0";
        }
        if (owned_value) {
            if (unrelated_members != 0) {
                content += ',';
            }
            content += "\"logonomic-hook-relay\":" + *owned_value;
        }
        return content + "}\n";
    };
    const auto stanza = addon::serialize_json(agy::hook_definition(windows));
    constexpr auto member_limit = addon::limits::k_max_json_members;
    const auto complete = root_document(member_limit - 1, stanza);
    const auto installed = invoke_hook_transform(true, root_document(member_limit - 1, std::nullopt));
    addon::Contract_failure failure;
    const auto installed_result = addon::decode_managed_configuration_transform_result(installed->result(), &failure);
    require(installed->status() == addon::Result_status::SUCCESS && installed_result &&
        installed_result->content == complete && json(installed_result->content).as_object().size() == member_limit,
        "1023 unrelated root members admit installation of the 1024th owned member without other changes");
    const auto repeated = invoke_hook_transform(true, installed_result->content);
    const auto repeated_result = addon::decode_managed_configuration_transform_result(repeated->result(), &failure);
    require(repeated->status() == addon::Result_status::SUCCESS && repeated_result &&
        repeated_result->content == complete,
        "an installed root at the 1024-member limit remains an exact byte-preserving no-op");
    const auto repaired = invoke_hook_transform(true, root_document(member_limit - 1, "{\"Stop\":[]}"));
    const auto repaired_result = addon::decode_managed_configuration_transform_result(repaired->result(), &failure);
    require(repaired->status() == addon::Result_status::SUCCESS && repaired_result && repaired_result->content == complete,
        "an existing owned root member at the 1024-member limit can be repaired without adding a member");
    const auto full = invoke_hook_transform(true, root_document(member_limit, std::nullopt));
    require(full->status() == addon::Result_status::FAILED && full->result().is_null(),
        "1024 unrelated root members must refuse insertion of the 1025th managed member");
}

void hook_home_provenance_contract()
{
    // A sourced command can change the home without naming it in the outer
    // launcher, so the frozen home cannot authorize that launcher's writer.
    Workspace workspace;
    const auto other_home = workspace.path() / "other-profile";
    fs::create_directories(other_home);
    addon::Prepare_request request;
    request.working_directory = path_text(workspace.path());
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.launch_environment->platform_id = "windows-x86_64";
    constexpr auto home_name = "USERPROFILE";
#else
    request.launch_environment->platform_id = addon::host_platform_id();
    constexpr auto home_name = "HOME";
#endif
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    const auto bash_environment = workspace.path() / "environment.sh";
    {
        std::ofstream stream(bash_environment, std::ios::binary);
        stream << "export " << home_name << "='" << path_text(other_home) << "'\n";
    }
    const auto bash_operand = "\"" + path_text(bash_environment) + "\"";
    struct Home_fixture
    {
        std::string label;
        std::string script;
        std::string runner;
        std::vector<std::string> argv;
    };
    std::vector<Home_fixture> fixtures;
    for (const auto* operation : {"source", "."}) {
        const auto script = std::string(operation) + " " + bash_operand + "; agy";
        fixtures.push_back({std::string("bash ") + operation, script, "bash", {"bash", "-lc", script}});
    }
#ifdef _WIN32
    const auto powershell_environment = workspace.path() / "environment.ps1";
    {
        std::ofstream stream(powershell_environment, std::ios::binary);
        stream << "$env:" << home_name << "='" << path_text(other_home) << "'\n";
    }
    const auto powershell_script = ". '" + path_text(powershell_environment) + "'; agy";
    fixtures.push_back({"PowerShell dot-source", powershell_script, "powershell",
        {"powershell.exe", "-NoLogo", "-Command", powershell_script}});
#endif
    addon::Contract_failure failure;
    for (const auto& fixture : fixtures) {
        const auto script_command = agy::recognize_launcher(fixture.script);
        const auto argv_command = agy::recognize_launcher(fixture.argv);
        const auto recognition_detail = fixture.label + ": both existing recognizers must retain opaque command evidence";
        require(script_command && argv_command && !script_command->sole_command && !argv_command->sole_command,
            recognition_detail.c_str());
        request.launcher = {fixture.script, fixture.runner, ""};
        request.launcher_argv = fixture.argv;
        Invocation invocation(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
            {"launch-preparation", 3});
        agy::handle_preparation(invocation);
        const auto patch = addon::decode_launch_patch(invocation.result(), &failure, 3);
        const auto authority_detail = fixture.label +
            ": opaque home provenance must withhold managed hook and delivery authority";
        require(invocation.status() == addon::Result_status::SUCCESS && patch && !patch->apply_managed_configuration &&
            patch->disposition && !patch->disposition->coordination_delivery && !patch->disposition->lifecycle_events &&
            std::none_of(patch->set.begin(), patch->set.end(), [](const auto& assignment) {
                return assignment.first == "AGY_LOGONOMIC_HOOKS_ENABLED";
            }), authority_detail.c_str());
    }
    std::vector<Home_fixture> controls = {
        {"direct AGY", "agy", "direct", {"agy"}},
        {"bash inline literal assignment", "COLOR=blue agy", "bash", {"bash", "-lc", "COLOR=blue agy"}},
        {"bash export literal assignment", "export COLOR=blue\nagy", "bash", {"bash", "-lc", "export COLOR=blue\nagy"}},
        {"bash tab export", "export\tCOLOR=blue\nagy", "bash", {"bash", "-lc", "export\tCOLOR=blue\nagy"}},
        {"bash multiple exports", "export COLOR=blue EXTRA='two words'\nagy", "bash",
            {"bash", "-lc", "export COLOR=blue EXTRA='two words'\nagy"}}
    };
#ifdef _WIN32
    controls.push_back({"PowerShell literal assignment", "$env:COLOR='blue'\nagy", "powershell",
        {"powershell.exe", "-NoLogo", "-Command", "$env:COLOR='blue'\nagy"}});
#endif
    for (const auto& fixture : controls) {
        const auto script_command = agy::recognize_launcher(fixture.script);
        const auto argv_command = agy::recognize_launcher(fixture.argv);
        const auto recognition_detail = fixture.label +
            ": both existing recognizers must retain sole-command evidence for supported literal assignments";
        require(script_command && argv_command && script_command->sole_command && argv_command->sole_command,
            recognition_detail.c_str());
        request.launcher = {fixture.script, fixture.runner, ""};
        request.launcher_argv = fixture.argv;
        Invocation invocation(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
            {"launch-preparation", 3});
        agy::handle_preparation(invocation);
        const auto patch = addon::decode_launch_patch(invocation.result(), &failure, 3);
        const auto authority_detail = fixture.label + ": supported launch must retain managed hook authority; status=" +
            std::to_string(static_cast<int>(invocation.status())) + ", reason=" + invocation.reason() +
            ", patch=" + std::to_string(patch.has_value()) + ", apply_managed_configuration=" +
            std::to_string(patch && patch->apply_managed_configuration) + ", disposition=" +
            std::to_string(patch && patch->disposition.has_value()) + ", coordination_delivery=" +
            std::to_string(patch && patch->disposition && patch->disposition->coordination_delivery);
        require(invocation.status() == addon::Result_status::SUCCESS && patch && patch->apply_managed_configuration &&
            patch->disposition && patch->disposition->coordination_delivery,
            authority_detail.c_str());
    }
    for (const auto* prefix : {"export\t", "export COLOR=blue "}) {
        const auto script = std::string(prefix) + home_name + "='" + path_text(other_home) + "'; agy";
        request.launcher = {script, "bash", ""};
        request.launcher_argv = {"bash", "-lc", script};
        Invocation invocation(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
            {"launch-preparation", 3});
        agy::handle_preparation(invocation);
        const auto patch = addon::decode_launch_patch(invocation.result(), &failure, 3);
        require(invocation.status() == addon::Result_status::SUCCESS && patch && !patch->apply_managed_configuration &&
            patch->disposition && !patch->disposition->coordination_delivery,
            "a home override in any export operand retains the existing provenance refusal");
    }
}

void hooks_installation_preparation_contract()
{
    // Oracle: launch preparation installs/repairs the package-owned member
    // through the host writer; the adapter never edits the user-global file.
    Workspace workspace;
    const auto path = workspace.path() / ".gemini" / "config" / "hooks.json";
    fs::create_directories(path.parent_path());
    addon::Prepare_request request;
    request.launcher = {"agy", "direct", ""};
    request.launcher_argv = {"agy"};
    request.working_directory = path_text(workspace.path());
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.launch_environment->platform_id = "windows-x86_64";
    constexpr auto home_name = "USERPROFILE";
#else
    request.launch_environment->platform_id = addon::host_platform_id();
    constexpr auto home_name = "HOME";
#endif
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    const std::string original =
        "{\n  \"user-owned\": {\"budget\":9007199254740993,\"ratio\":1.2300e+4},\n"
        "  \"logonomic-hook-relay\": {\"Stop\":[]}\n}\n";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << original;
    }
    Invocation prepare(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
        {"launch-preparation", 3});
    agy::handle_preparation(prepare);
    addon::Contract_failure failure;
    const auto patch = addon::decode_launch_patch(prepare.result(), &failure, 3);
    require(prepare.status() == addon::Result_status::SUCCESS && patch && patch->apply_managed_configuration,
        "recognized AGY preparation must request automatic installation/repair of its managed hooks");
    require(std::find(patch->set.begin(), patch->set.end(),
        std::pair<std::string, std::string>{"AGY_LOGONOMIC_HOOKS_ENABLED", "1"}) != patch->set.end(),
        "successful host-managed installation enables the prepared session's hooks");
    std::ifstream stream(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(stream), {}};
    require(after == original, "provider preparation leaves replacement and exact-preimage backup to the host");

    const auto refused_home = [&request, &failure](const char* case_name) {
        Invocation invocation(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
            {"launch-preparation", 3});
        agy::handle_preparation(invocation);
        const auto patch = addon::decode_launch_patch(invocation.result(), &failure, 3);
        const auto detail = std::string(case_name) +
            ": home guard must withhold the managed hook request and hook delivery (reason: " +
            invocation.reason() + ")";
        require(invocation.status() == addon::Result_status::SUCCESS && patch && !patch->apply_managed_configuration &&
            !patch->disposition->coordination_delivery && !patch->disposition->lifecycle_events &&
            std::none_of(patch->set.begin(), patch->set.end(), [](const auto& assignment) {
                return assignment.first == "AGY_LOGONOMIC_HOOKS_ENABLED";
            }), detail.c_str());
    };
    request.launcher.environment_text = std::string(home_name) + "=/different-home";
    refused_home("launcher environment home override");
    request.launcher.environment_text.clear();
    request.launcher.script_body = std::string(home_name) + "=/different-home agy";
    refused_home("launcher script home assignment");
    request.launcher.script_body = "agy --prompt " + std::string(home_name);
    request.launcher.runner_key = "bash";
    refused_home("shell script references the home variable");
    request.launcher = {"agy", "direct", ""};
    for (const auto& [home, case_name] : {
        std::pair{"",              "empty frozen home member"},
        std::pair{"relative-home", "relative frozen home member"}})
    {
        request.launch_environment->effective_environment.set(home_name, addon::Json_value(home));
        refused_home(case_name);
    }
    request.launch_environment->effective_environment.erase(home_name);
    refused_home("missing frozen home member");

    // prepare@3 requires the enclosing launch_environment record even when
    // its effective environment contains no usable home member.
    request.launch_environment.reset();
    Invocation missing_context(addon::k_prepare_operation, addon::encode_prepare_request(request, 3), false,
        {"launch-preparation", 3});
    agy::handle_preparation(missing_context);
    require(missing_context.status() == addon::Result_status::FAILED &&
        missing_context.reason() == addon::reason::k_frame_violation,
        "missing required launch_environment record fails prepare@3 as frame_violation");
}

// A delegated implementation request as the host sends it: two Task servers
// behind the host relay, credentials only in the host's original control.
struct Delegated_fixture
{
    Workspace                          workspace;
    std::string                        program;
    std::string                        bridge;
    std::string                        root;
    addon::Delegated_prepare_request   request;
    std::unique_ptr<Invocation>        invocation;
    std::optional<addon::Delegated_prepare_result> prepared;
};

void prepare_delegated(Delegated_fixture& fixture, bool cancelled = false)
{
    fixture.invocation = std::make_unique<Invocation>("prepare",
        addon::encode_delegated_prepare_request(fixture.request), cancelled,
        addon::Capability_pair{"delegated-session", 3});
    agy::handle_sessions(*fixture.invocation);
    addon::Contract_failure failure;
    fixture.prepared = fixture.invocation->status() == addon::Result_status::SUCCESS
        ? addon::decode_delegated_prepare_result(fixture.invocation->result(), &failure)
        : std::nullopt;
}

// The user's own agy settings in the fixture's home: `allow` is the JSON
// array of permissions.allow, or empty for no settings file.
void grant_task_tools(Delegated_fixture& fixture, const std::string& allow)
{
    const auto settings = fixture.workspace.path() / ".gemini" / "antigravity-cli" / "settings.json";
    fs::remove(settings);
    if (!allow.empty()) {
        fs::create_directories(settings.parent_path());
        std::ofstream(settings, std::ios::binary) << R"({"colorScheme":"dark","permissions":{"allow":)" << allow << "}}";
    }
}

std::unique_ptr<Delegated_fixture> delegated_fixture(const char* executable)
{
    auto fixture = std::make_unique<Delegated_fixture>();
#ifdef _WIN32
    const auto native_program = fixture->workspace.path() / "agy.exe";
    constexpr auto platform   = "windows-x86_64";
    constexpr auto home_name  = "USERPROFILE";
    fixture->bridge = "helpers/agy_launch_bridge.exe";
#else
    const auto native_program = fixture->workspace.path() / "agy";
    const auto platform       = addon::host_platform_id();
    constexpr auto home_name  = "HOME";
    fixture->bridge = "helpers/agy_launch_bridge";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    fixture->program = path_text(native_program);
    // The host passes its private root in '/'-separated form.
    fixture->root = path_text(fixture->workspace.path() / "structured_sessions" / "task-7f3a");
    std::replace(fixture->root.begin(), fixture->root.end(), '\\', '/');
    auto& request = fixture->request;
    request.purpose = addon::Structured_purpose::IMPLEMENTATION;
    request.context.launcher = {"COLOR=blue \"" + fixture->program + "\" --model gemini-3-pro --sandbox", "direct", ""};
    request.context.launcher_argv = {fixture->program, "--model", "gemini-3-pro", "--sandbox"};
    request.context.working_directory = path_text(fixture->workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = platform;
    request.context.launch_environment->effective_environment.set(
        home_name, addon::Json_value(path_text(fixture->workspace.path())));
    request.context.launch_environment->effective_environment.set(
        "AGY_TEST_VERSION", addon::Json_value("1.2.11"));
#ifdef _WIN32
    request.context.launch_environment->effective_environment.set(
        "AGY_TEST_REQUIRE_NO_AUTO_UPDATE", addon::Json_value("1"));
    request.context.launch_environment->effective_environment.set(
        "agy_cli_disable_auto_update", addon::Json_value("false"));
#endif
    request.prompt = "/usage stays literal.\nAdd a retry budget to the fetcher \xE2\x9C\x93";
    request.control.task_id = "task-7f3a";
    const auto server_program = [&fixture](const char* name) {
        return path_text(fixture->workspace.path() / "bin" / name);
    };
    request.control.servers = {
        {"coordination", server_program("logonomic_coordination_mcp"), {"status", "notice"},
            {{"LOGONOMIC_SESSION_CAPABILITY", "secret-session-capability"}}},
        {"delegation", server_program("logonomic_delegation_mcp"), {"targets", "submit"},
            {{"LOGONOMIC_SESSION_CAPABILITY", "secret-session-capability"}}},
    };
    request.control.private_artifact_root = fixture->root;
    request.control.connection = addon::Delegated_connection{
        server_program("logonomic_task_server_relay"), fixture->root + "/.logonomic/task-servers.json"};
    grant_task_tools(*fixture, R"json(["command(git status)","mcp(logonomic_coordination/*)","mcp(logonomic_delegation/*)"])json");
    return fixture;
}

// Oracle: delegated-session@3 (the SDK's docs and session_contracts.h), the
// host relay's argument grammar, the owner's rule that a delegated run keeps its
// launcher's permission posture (2026-09-28), and the headless behaviour of agy
// 1.2.11 and 1.2.12 recorded against a scripted gateway: MCP servers load
// additively from an added folder's `.agents/mcp_config.json`; headless mode
// denies an ungranted tool, grants one named in the user's permissions.allow
// (mcp(<server>/<tool>), `*` for any tool) or under the launcher's
// --dangerously-skip-permissions; and `--print=` with stream-json input takes
// the prompt from stdin.
void delegated_contract(const char* executable)
{
    auto fixture = delegated_fixture(executable);
    prepare_delegated(*fixture);
    require(fixture->invocation->status() == addon::Result_status::SUCCESS, "delegated implementation is prepared");
    require(fixture->prepared.has_value(), "delegated preparation obeys the delegated-session@3 result grammar");
    const auto& prepared = *fixture->prepared;
    addon::Contract_failure failure;
    require(addon::validate_structured_process_proposal(prepared.process, &failure),
        "host admits the delegated process proposal");
    require(addon::validate_delegated_acknowledgment(fixture->request.control, prepared.acknowledgment, &failure),
        "delegation acknowledges exactly the host's Task servers");
    require(prepared.acknowledgment.connection_version == addon::k_delegated_connection_version,
        "delegation acknowledges the configured relay connection");
    require(prepared.process.executable == addon::Process_executable(fixture->bridge),
        "the verified native program runs through the package's launch bridge");
    const auto folder = fixture->root + "/agy";
    require(prepared.process.argv == std::vector<std::string>{
        fixture->program, "--model", "gemini-3-pro", "--sandbox", "--input-format", "stream-json",
        "--output-format", "stream-json", "--disable-slash-commands", "--add-dir", folder, "--print="},
        "headless stream-json run keeps the launcher's options, adds the private Task folder and no permission");
    require(prepared.process.stdin_behavior == addon::Stdin_behavior::PAYLOAD &&
        prepared.process.stdin_payload.empty() && prepared.inspection.incremental &&
        prepared.inspection.keep_stdin_open,
        "the prompt waits on open stdin for agy's startup record");
    const auto& environment = prepared.process.environment;
    require(std::find(environment.begin(), environment.end(),
            std::pair<std::string, std::string>{"COLOR", "blue"}) != environment.end() &&
        std::find(environment.begin(), environment.end(),
            std::pair<std::string, std::string>{"AGY_TEST_VERSION", "1.2.11"}) != environment.end(),
        "the process receives the frozen launcher environment and the launcher's assignments");
#ifdef _WIN32
    require(std::find(environment.begin(), environment.end(),
        std::pair<std::string, std::string>{"AGY_CLI_DISABLE_AUTO_UPDATE", "true"}) != environment.end(),
        "delegated sessions disable the native updater without changing the launcher");
#endif

    require(prepared.private_artifacts.size() == 1 &&
        prepared.private_artifacts.front().relative_path == "agy/.agents/mcp_config.json",
        "one private configuration, independent of the server count");
    const auto configuration = json(prepared.private_artifacts.front().content);
    const auto* servers = configuration.find("mcpServers");
    require(servers && servers->is_object() && servers->as_object().size() == 2,
        "the configuration names exactly the Task servers");
    for (const auto* id : {"coordination", "delegation"}) {
        const auto* server = servers->find(std::string("logonomic_") + id);
        require(server && server->find("command") &&
            *server->find("command") == addon::Json_value(fixture->request.control.connection->relay_program) &&
            server->find("env") && *server->find("env") == addon::Json_value::object(),
            "each Task server is the host relay with no environment");
        const auto& arguments = server->find("args")->as_array();
        require(arguments.size() == 8 &&
            arguments[0] == addon::Json_value("--descriptor") &&
            arguments[1] == addon::Json_value(fixture->request.control.connection->descriptor_path) &&
            arguments[2] == addon::Json_value("--server") && arguments[3] == addon::Json_value(id) &&
            arguments[4] == addon::Json_value("--attestation-path") &&
            arguments[5] == addon::Json_value(folder + "/.agents/" + id + ".ready") &&
            arguments[6] == addon::Json_value("--attestation-nonce") &&
            arguments[7].as_string().size() == 32,
            "relay arguments name the descriptor, the server and its '/'-form catalog receipt");
    }
    const auto payload = addon::serialize_json(fixture->invocation->result());
    require(payload.find("secret-session-capability") == std::string::npos &&
        payload.find("logonomic_coordination_mcp") == std::string::npos,
        "no Task server credential or original server program reaches the process or its configuration");

    const auto refused = [&fixture](const char* surface, addon::Result_status status, const char* reason) {
        prepare_delegated(*fixture);
        require(fixture->invocation->status() == status && fixture->invocation->reason() == reason, surface);
    };
    const auto saved = fixture->request;
    fixture->request.control.connection.reset();
    refused("a delegation without the host connection is refused", addon::Result_status::UNAVAILABLE,
        "task_server_connection_unavailable");
    fixture->request = saved;
    fixture->request.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    refused("read-only review is not declared", addon::Result_status::UNAVAILABLE, "purpose_unavailable");
    fixture->request = saved;
    fixture->request.control.servers.front().server_id = "Coordination/../profile";
    refused("a Task server name agy cannot use as a plain directory name is refused",
        addon::Result_status::UNAVAILABLE, "task_server_unsupported");
    for (const auto* options : {"--mode plan", "--agent reviewer", "-p hello", "--output-format json",
        "--continue", "--conversation=ec33ebf9", "--conversation ec33ebf9", "--remote-control", "--remote-control=true", "models"})
    {
        fixture->request = saved;
        fixture->request.context.launcher.script_body = "\"" + fixture->program + "\" " + options;
        refused("an option the delegated protocol owns is refused", addon::Result_status::UNAVAILABLE,
            "launcher_program_unavailable");
    }
    fixture->request = saved;
    fixture->request.context.launcher.script_body = "source /opt/agy-env.sh; \"" + fixture->program + "\"";
    refused("a launcher running other commands cannot be replaced by a direct invocation",
        addon::Result_status::UNAVAILABLE, "launcher_program_unavailable");
    fixture->request = saved;
    fixture->request.context.launch_environment->effective_environment.set(
        "AGY_TEST_VERSION", addon::Json_value("1.2.10"));
    refused("a release older than the validated headless contract is refused", addon::Result_status::UNAVAILABLE,
        "launcher_program_unavailable");
    fixture->request = saved;
    prepare_delegated(*fixture, true);
    require(fixture->invocation->status() == addon::Result_status::CANCELLED,
        "a cancelled preparation launches nothing");

    const auto posture = [&fixture, &saved](const char* allow, const char* option) {
        fixture->request = saved;
        if (*option) {
            fixture->request.context.launcher.script_body += std::string(" ") + option;
            fixture->request.context.launcher_argv.push_back(option);
        }
        grant_task_tools(*fixture, allow);
        prepare_delegated(*fixture);
    };
    for (const auto* allow : {"", "[]", R"json(["mcp(logonomic_coordination/*)"])json",
        R"json(["mcp(logonomic_coordination/*)","mcp(logonomic_delegation/targets)"])json",
        R"json(["mcp(logonomic_coordination)","mcp(logonomic_delegation)"])json"})
    {
        posture(allow, "");
        require(fixture->invocation->status() == addon::Result_status::UNAVAILABLE &&
            fixture->invocation->reason() == "task_tools_not_permitted" &&
            fixture->invocation->detail().find("\"mcp(logonomic_coordination/*)\", \"mcp(logonomic_delegation/*)\"") !=
                std::string::npos &&
            fixture->invocation->detail().find("--dangerously-skip-permissions") != std::string::npos,
            "without a grant for every Task tool the run is refused, naming the allow entries that grant them");
    }
    posture("", "--dangerously-skip-permissions=false");
    require(fixture->invocation->reason() == "task_tools_not_permitted", "a switched-off bypass grants nothing");
    for (const auto* allow : {R"json(["mcp(*)"])json",
        R"json(["mcp(logonomic_coordination/status)","mcp(logonomic_coordination/notice)","mcp(logonomic_delegation/*)"])json"})
    {
        posture(allow, "");
        require(fixture->prepared.has_value(), "the user's own allow entries for every Task tool admit the run");
    }
    posture("", "--dangerously-skip-permissions");
    require(fixture->prepared.has_value() &&
        std::count(fixture->prepared->process.argv.begin(), fixture->prepared->process.argv.end(),
            "--dangerously-skip-permissions") == 1,
        "the launcher's own bypass admits the run and is passed once, as the launcher's");
    fixture->request = saved;
    grant_task_tools(*fixture, R"json(["mcp(logonomic_coordination/*)","mcp(logonomic_delegation/*)"])json");

    addon::Structured_prepare_request structured;
    structured.purpose = addon::Structured_purpose::IMPLEMENTATION;
    structured.context = saved.context;
    structured.prompt  = saved.prompt;
    Invocation bare("prepare", addon::encode_structured_prepare_request(structured), false,
        addon::Capability_pair{"structured-session", 3});
    agy::handle_sessions(bare);
    require(bare.status() == addon::Result_status::UNAVAILABLE && bare.reason() == "purpose_unavailable",
        "an implementation run without its Task servers is not offered");
}

std::string record(const addon::Json_value& value)
{
    return addon::serialize_json(value) + '\n';
}

std::string init_record(const std::string& cwd, const char* permission_mode)
{
    auto init = addon::Json_value::object();
    init.set("cwd", addon::Json_value(cwd));
    init.set("tools", json(R"(["call_mcp_tool","run_command","view_file","write_to_file"])"));
    init.set("permission_mode", addon::Json_value(permission_mode));
    auto value = addon::Json_value::object();
    value.set("event", addon::Json_value("init"));
    value.set("conversation_id", addon::Json_value("c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1"));
    value.set("init", std::move(init));
    return record(value);
}

// Records in the shape agy 1.2.11 writes them, adapted from scripted-gateway
// captures: server names, tool output and response text differ.
constexpr const char* k_first_step =
    R"({"event":"step_update","step_update":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","step_index":0,"state":"DONE","step_type":"user_input"}})" "\n";
constexpr const char* k_progress =
    R"({"event":"step_update","step_update":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","step_index":1,"state":"DONE","step_type":"agent_response","duration_seconds":0.0124069,"usage":{"input_tokens":11,"output_tokens":7,"thinking_tokens":0,"cache_read_tokens":0,"total_tokens":18}}})" "\n"
    R"({"event":"step_update","step_update":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","step_index":2,"state":"ACTIVE","step_type":"tool","tool_name":"call_mcp_tool","tool_info":{"name":"call_mcp_tool","parameters":{"Arguments":{},"ServerName":"logonomic_coordination","ToolName":"status"}}}})" "\n"
    R"({"event":"step_update","step_update":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","step_index":2,"state":"DONE","step_type":"tool","tool_name":"call_mcp_tool","duration_seconds":0.0054907,"tool_info":{"name":"call_mcp_tool","parameters":{"Arguments":{},"ServerName":"logonomic_coordination","ToolName":"status"},"output":"running"}}})" "\n";
constexpr const char* k_result =
    R"({"event":"result","result":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","status":"SUCCESS","response":"Added the retry budget.\n","duration_seconds":0.1900036,"num_turns":1,"usage":{"input_tokens":22,"output_tokens":14,"thinking_tokens":0,"cache_read_tokens":0,"total_tokens":36}}})" "\n";
constexpr const char* k_failed_result =
    R"({"event":"result","result":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","status":"ERROR","response":"","error":"not found","duration_seconds":0,"num_turns":1,"usage":{"input_tokens":0,"output_tokens":0,"thinking_tokens":0,"cache_read_tokens":0,"total_tokens":0}}})" "\n";
constexpr const char* k_startup_failure =
    R"json({"event":"result","result":{"conversation_id":"","status":"ERROR","response":"","error":"invalid model selection (--model \"gemini-3.6-flash\" --effort \"\"): --model gemini-3.6-flash requires --effort (available: low, medium, high)","duration_seconds":0,"num_turns":0,"usage":{"input_tokens":0,"output_tokens":0,"thinking_tokens":0,"cache_read_tokens":0,"total_tokens":0}}})json" "\n";
constexpr const char* k_denied_result =
    R"({"event":"result","result":{"conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1","status":"SUCCESS","response":"","duration_seconds":0.1802795,"num_turns":1,"usage":{"input_tokens":11,"output_tokens":7,"thinking_tokens":0,"cache_read_tokens":0,"total_tokens":18},"denied_actions":[{"action":"mcp","display_name":"CallMcpTool"}]}})" "\n";

addon::Structured_inspect_result inspect(
    const std::string& state, const std::string& output, bool final = false, int exit_code = 0, bool timed_out = false)
{
    addon::Structured_inspect_request request;
    request.purpose = addon::Structured_purpose::IMPLEMENTATION;
    request.inspection.protocol_state = state;
    request.inspection.stdout_bytes   = output;
    request.inspection.final          = final;
    request.inspection.exit_code      = exit_code;
    request.inspection.timed_out      = timed_out;
    Invocation invocation("inspect", addon::encode_structured_inspect_request(request), false,
        addon::Capability_pair{"structured-session", 3});
    agy::handle_sessions(invocation);
    require(invocation.status() == addon::Result_status::SUCCESS, "inspection answers the host");
    addon::Contract_failure failure;
    auto result = addon::decode_structured_inspect_result(invocation.result(), final, &failure);
    require(result.has_value(), "inspection obeys the structured-session@3 completion rules");
    return std::move(*result);
}

bool failed_with(const addon::Structured_inspect_result& result, const char* reason)
{
    return result.complete && result.failure_reason == std::string(reason) && !result.result_bytes;
}

// Oracle: the recorded headless protocol of agy 1.2.11, the host relay's
// catalog receipts, and structured-session@3 stdin closure.
void inspection_contract(const char* executable)
{
    auto fixture = delegated_fixture(executable);
    prepare_delegated(*fixture);
    require(fixture->prepared.has_value(), "inspection starts from a prepared delegation");
    const auto initial = fixture->prepared->inspection.protocol_state;
    const auto cwd = fixture->request.context.working_directory;

    auto released = inspect(initial, init_record(cwd, "request-review"));
    require(!released.complete && released.close_stdin && released.stdin_bytes,
        "the startup record releases the prompt and closes stdin after it");
    const auto message = json(std::string_view(*released.stdin_bytes).substr(0, released.stdin_bytes->size() - 1));
    require(released.stdin_bytes->back() == '\n' && message.as_object().size() == 2 &&
        message.find("event") && *message.find("event") == addon::Json_value("user") &&
        *message.find("message") == json(R"({"content":"/usage stays literal.\nAdd a retry budget to the fetcher )"
            "\xE2\x9C\x93" R"("})"),
        "the raw prompt is one stream-json user message");
    require(failed_with(inspect(initial, init_record(cwd + "-elsewhere", "request-review")),
        "session_provenance_mismatch"), "a process in another directory is not the prepared one");
    require(!inspect(initial, init_record(cwd, "always-proceed")).complete,
        "a launcher's own bypass is its posture, not a mismatch");
    require(failed_with(inspect(initial, k_first_step), "unexpected_startup_output"), "no step precedes startup");
    require(failed_with(inspect(initial, k_startup_failure), "startup_failed"),
        "a session agy cannot start is a startup failure");
    require(failed_with(inspect(initial, init_record(cwd, "request-review") + k_first_step),
        "unexpected_startup_output"), "agy writes nothing between startup and the prompt");

    const auto started = released.protocol_state;
    require(failed_with(inspect(started, k_first_step), "task_servers_not_ready"),
        "a Task server without its host catalog receipt fails the task");
    const auto configuration = json(fixture->prepared->private_artifacts.front().content);
    const auto receipt = [&configuration](const char* id, bool valid) {
        const auto& server    = *configuration.find("mcpServers")->find(std::string("logonomic_") + id);
        const auto& arguments = server.find("args")->as_array();
        const auto path = addon::path_from_utf8(arguments[5].as_string());
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << (valid ? arguments[7].as_string() : std::string(32, '0'));
    };
    receipt("coordination", true);
    receipt("delegation", false);
    require(failed_with(inspect(started, k_first_step), "task_servers_not_ready"),
        "a receipt must hold the relay's own nonce");
    receipt("delegation", true);

    const auto step_line = [](const char* members) {
        return std::string(R"({"event":"step_update","step_update":{)"
            R"("conversation_id":"c5ee32d1-6f8f-4598-8c5f-8c18e227b4b1",)") + members + "}}\n";
    };
    for (const auto* members : {
        R"("step_index":0,"state":"garbage","step_type":"user_input")",
        R"("state":"DONE","step_type":"user_input")",
        R"("step_index":"0","state":"DONE","step_type":"user_input")",
        R"("step_index":0,"state":"DONE","step_type":"User Input")"})
    {
        require(failed_with(inspect(started, step_line(members)), "invalid_structured_output"),
            "a malformed step record cannot start the turn");
    }
    require(failed_with(inspect(started, step_line(R"("step_index":3,"state":"DONE","step_type":"user_input")")),
        "session_provenance_mismatch"), "the turn starts with the prompt's own completed step");
    auto running = inspect(started, std::string(k_first_step) + k_progress);
    require(!running.complete && !running.stdin_bytes && !running.close_stdin &&
        running.presentation_state == addon::Presentation_state::AVAILABLE,
        "attested Task servers let the turn run; progress is presentation only");
    require(failed_with(
        inspect(running.protocol_state, step_line(R"("step_index":4,"state":"garbage","step_type":"tool")")),
        "invalid_structured_output"), "a malformed progress record is protocol drift");
    require(!inspect(running.protocol_state,
        step_line(R"("step_index":4,"state":"DONE","step_type":"system_message")")).complete,
        "a well-formed step of an unrecorded type is progress only");
    const auto split = std::string(k_result).find("\"status\"");
    auto partial = inspect(running.protocol_state, std::string(k_result).substr(0, split));
    require(!partial.complete, "a record split across inspections waits for its end");
    auto settled = inspect(partial.protocol_state, std::string(k_result).substr(split));
    require(!settled.complete, "the result completes only with the process");
    auto completed = inspect(settled.protocol_state, "", true);
    require(completed.complete && completed.result_bytes == std::string("Added the retry budget.\n"),
        "the root conversation's successful response is the task result");

    require(failed_with(inspect(settled.protocol_state, k_first_step), "output_after_result"),
        "nothing follows the terminal result");
    require(failed_with(inspect(settled.protocol_state, "", true, 3), "process_failed"),
        "a failing exit is not success");
    require(failed_with(inspect(settled.protocol_state, "", true, 0, true), "process_timed_out"),
        "a timed-out process is not success");
    require(failed_with(inspect(running.protocol_state, "", true), "unauthenticated_final_result"),
        "an exit without a result is not success");
    require(failed_with(inspect(running.protocol_state, k_failed_result), "provider_turn_failed"),
        "a failed turn fails the task");
    require(failed_with(inspect(running.protocol_state, k_denied_result), "tool_permission_denied"),
        "a turn agy ended at a denial of another tool fails as that denial");
    const auto denied_call = [&step_line](const char* server) {
        const auto parameters = std::string(R"("parameters":{"Arguments":{},"ServerName":")") + server +
            R"(","ToolName":"status"})";
        return step_line((std::string(R"("step_index":4,"state":"ACTIVE","step_type":"tool","tool_name":"call_mcp_tool","tool_info":{"name":"call_mcp_tool",)") +
                parameters + "}").c_str()) +
            step_line((std::string(R"("step_index":4,"state":"ERROR","step_type":"tool","tool_name":"call_mcp_tool","tool_info":{"name":"call_mcp_tool",)") +
                parameters + R"(,"error":{"type":"TOOL_ERROR","message":"permission check failed"}})").c_str());
    };
    require(failed_with(inspect(running.protocol_state, denied_call("logonomic_coordination") + k_denied_result),
        "task_tools_not_permitted"), "a denied Task tool call fails the task as not permitted");
    require(failed_with(inspect(running.protocol_state, denied_call("github") + k_denied_result),
        "tool_permission_denied"), "a denied call to the user's own server is the launcher's posture");
    auto answered_denial = std::string(k_denied_result);
    answered_denial.replace(answered_denial.find(R"("response":"")"), 13, R"("response":"Could not run it.")");
    auto answered = inspect(running.protocol_state, denied_call("github") + answered_denial);
    require(!answered.complete, "a response after another tool's denial is the task's result");
    require(failed_with(inspect(running.protocol_state, denied_call("logonomic_delegation") + answered_denial),
        "task_tools_not_permitted"), "a Task tool denial fails the task even when agy answers");
    auto foreign = std::string(k_result);
    foreign.replace(foreign.find("c5ee32d1"), 8, "9e54e2da");
    require(failed_with(inspect(running.protocol_state, foreign), "invalid_result_provenance"),
        "only the root conversation's result counts");
    require(failed_with(inspect(running.protocol_state, R"({"event":"warning","warning":{}})" "\n"),
        "unsupported_structured_event"), "an unknown record is protocol drift");
}

void startup_contract()
{
    const std::string id = "b22c8da5-1345-442f-906d-f63a1e1d552a";
    const auto stream = "I0929 07:51:02.417194     171 conversation_manager.go:887] Streaming conversation " + id + '\n';
    const auto redraw = "I0929 07:51:02.478085     362 manager.go:932] Full redraw completed (rerenderAll) for conversation " +
        id + " (epoch 0, items 4)\n";
    const auto selector = "I0929 07:51:02.417194       1 common.go:401] Resuming conversation " + id + '\n';
    require(agy::observe_startup(selector)->conversation_ids.empty(), "requested selector echo is not observed identity");
    require(agy::observe_startup(stream)->conversation_ids.empty(), "starting a stream is not loaded identity");
    require(agy::observe_startup(redraw)->conversation_ids.empty(), "an orphan redraw cannot qualify a startup");
    require(agy::observe_startup(redraw + stream)->conversation_ids.empty(), "redraw must follow native stream creation");
    const auto observed = agy::observe_startup(selector + stream + redraw + "private transcript text\n");
    require(observed && observed->conversation_ids == std::vector<std::string>{id} &&
        observed->native_records == stream + redraw, "AGY03 native loaded identity qualifies without transcript or selector echo");
    const auto current_redraw = "I1108 10:38:41.219802     288 manager.go:1147] Full redraw completed (RerenderAll) for conversation " +
        id + " (epoch 0, items 3) in 2ms\n";
    require(agy::observe_startup(stream + current_redraw)->conversation_ids == std::vector<std::string>{id},
        "native 1.5.3 capitalization and elapsed-time suffix preserve the exact startup readiness proof");
    require(agy::observe_startup(stream + redraw.substr(0, redraw.size() - 1))->conversation_ids.empty(),
        "an incomplete native record is not identity");
    require(!agy::observe_startup(std::string(1024 * 1024 + 1, 'x')) &&
        !agy::observe_startup(std::string("bad\0record", 10)), "bounded valid records are required");
    std::string conflict = stream + redraw;
    auto other = stream + redraw;
    for (auto position = other.find(id); position != std::string::npos; position = other.find(id, position + id.size())) {
        other.replace(position, id.size(), "ec33ebf9-0cba-4100-8142-c61503f6c587");
    }
    const auto conflicting = agy::observe_startup(conflict + other);
    require(conflicting && conflicting->conversation_ids.size() == 2,
        "all loaded identities survive classification so the host can reject a conflict");
    auto body = addon::Json_value::object();
    body.set("native_records", addon::Json_value(observed->native_records));
    Invocation classify(addon::k_classify_startup_operation,
        addon::encode_startup_observation_request({addon::serialize_json(body)}), false, {"interactive-resume", 2});
    agy::handle_preparation(classify);
    addon::Contract_failure failure;
    const auto result = addon::decode_startup_observation_result(classify.result(), &failure);
    require(classify.status() == addon::Result_status::SUCCESS && result && result->provider_session_ids ==
        std::vector<std::string>{id}, "actual owning adapter classifies native startup evidence");
}

} // namespace

int run_tests(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--status-line-command") {
        std::cout << agy::status_line_command(std::string_view(argv[2]) == "windows", argv[3]);
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--installed-status-line-settings") {
        auto adapter = permission_adapter(addon::path_from_utf8(argv[2]));
        const std::string settings{std::istreambuf_iterator<char>(std::cin), {}};
        const auto outcome = invoke_permission(*adapter, addon::k_managed_configuration_transform_operation,
            addon::encode_managed_configuration_transform_request({std::string(32, 'd'), true, settings}));
        const auto result = addon::decode_managed_configuration_transform_result(outcome.payload, nullptr);
        require(outcome.status == addon::Host_status::SUCCESS && result && result->grant == std::string(32, 'd'),
            "the packaged transform supplies the installed callback command");
        int exit_code = -1;
        require(adapter->shutdown(1000, &exit_code) && exit_code == 0, "settings adapter shuts down");
        std::cout << result->content;
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--package-call") {
        // Native qualification drivers use the real installed adapter and SDK
        // boundary. This test utility does not emulate host persistence or GUI
        // admission; its caller supplies the contract request explicitly.
        const auto call = json(std::string{std::istreambuf_iterator<char>(std::cin), {}});
        const auto* capability = call.find("capability_id");
        const auto* version = call.find("version");
        const auto* operation = call.find("operation");
        const auto* payload = call.find("payload");
        require(capability && capability->is_string() && version && version->is_integer() &&
            version->as_integer() > 0 && version->as_integer() <= 65535 && operation && operation->is_string() && payload,
            "package call names a capability, version, operation and payload");
        auto adapter = permission_adapter(addon::path_from_utf8(argv[2]));
        const auto id = adapter->invoke({capability->as_string(), static_cast<std::uint16_t>(version->as_integer())},
            operation->as_string(), *payload, 10000);
        const auto outcome = adapter->wait_outcome(id, 12000);
        require(outcome.has_value(), "the packaged native qualification call completes");
        auto result = addon::Json_value::object();
        result.set("status", addon::Json_value(addon::host_status_name(outcome->status)));
        result.set("reason_code", addon::Json_value(outcome->reason_code));
        result.set("payload", outcome->payload);
        bool contract_valid = false;
        if (outcome->status == addon::Host_status::SUCCESS) {
            if (capability->as_string() == "helper-ingress") {
                contract_valid = addon::decode_classify_event_result(outcome->payload, nullptr,
                    static_cast<std::uint16_t>(version->as_integer())).has_value();
            }
            else
            if (capability->as_string() == addon::k_permission_snapshot_capability_id &&
                operation->as_string() == addon::k_classify_event_operation)
            {
                contract_valid = addon::decode_permission_snapshot_classification_result(outcome->payload, nullptr, 3).has_value();
            }
            else
            if (operation->as_string() == addon::k_managed_configuration_transform_operation) {
                contract_valid = addon::decode_managed_configuration_transform_result(outcome->payload, nullptr).has_value();
            }
            else
            if (capability->as_string() == "interactive-resume" && operation->as_string() == addon::k_inspect_resume_operation) {
                const auto inspected = addon::decode_resume_inspect_request(*payload, nullptr);
                const auto answer = addon::decode_resume_inspect_result(outcome->payload, nullptr,
                    static_cast<std::uint16_t>(version->as_integer()));
                contract_valid = inspected && answer && addon::validate_resume_inspection(*inspected, *answer, nullptr);
            }
            else
            if (capability->as_string() == "interactive-resume" && operation->as_string() == addon::k_prepare_resume_operation) {
                const auto prepared = addon::decode_resume_prepare_request(*payload, nullptr, 2);
                const auto answer = addon::decode_resume_prepare_result(outcome->payload, nullptr, 2);
                contract_valid = prepared && answer &&
                    addon::validate_resume_acknowledgment(prepared->resume_record, answer->acknowledgment, nullptr);
            }
            else
            if (capability->as_string() == "launch-preparation") {
                const auto prepared = addon::decode_prepare_request(*payload, nullptr, 3);
                const auto answer = addon::decode_launch_patch(outcome->payload, nullptr, 3);
                contract_valid = prepared && answer && addon::validate_launch_patch(*answer,
                    prepared->declared_assets, prepared->declared_assets, nullptr);
            }
        }
        result.set("contract_valid", addon::Json_value(contract_valid));
        int exit_code = -1;
        require(adapter->shutdown(1000, &exit_code) && exit_code == 0, "qualification adapter shuts down");
        std::cout << addon::serialize_json(result);
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--permission-snapshot-events") {
        // The helper test feeds the exact sanitized event objects captured at
        // its transport boundary through the packaged adapter's real route.
        const auto events = json(std::string{std::istreambuf_iterator<char>(std::cin), {}});
        require(events.is_array(), "captured helper events form an array");
        auto adapter = permission_adapter(addon::path_from_utf8(argv[2]));
        auto results = addon::Json_value::array();
        for (const auto& event : events.as_array()) {
            require(event.is_object() && event.find("name") && event.find("name")->is_string() && event.find("body"),
                "captured helper events have a name and body");
            auto request = hook_event(event.find("name")->as_string());
            request.event.body = *event.find("body");
            request.ledger.current_epoch = 2;
            request.ledger.current_transition = addon::Ledger_transition{
                addon::Transition_kind::START, "Conversation-Parent", std::string(32, 'b')};
            const auto outcome = invoke_permission(*adapter, addon::k_classify_event_operation,
                addon::encode_classify_event_request(request));
            require(outcome.status == addon::Host_status::SUCCESS &&
                addon::decode_permission_snapshot_classification_result(outcome.payload, nullptr, 3).has_value(),
                "captured helper evidence traverses the adapter and strict SDK result decoder");
            results.push_back(outcome.payload);
        }
        int exit_code = -1;
        require(adapter->shutdown(1000, &exit_code) && exit_code == 0, "captured-event adapter shuts down");
        std::cout << addon::serialize_json(results);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        auto& output = std::getenv("AGY_TEST_HELP_STDERR") ? std::cerr : std::cout;
        output << "Usage: agy [options]\n--add-dir Add a directory to the workspace\n";
        if (!std::getenv("AGY_TEST_NO_INITIAL_PROMPT")) {
            output << "--prompt-interactive Run an initial prompt interactively and continue the session\n";
        }
        if (const char* private_output = std::getenv("AGY_TEST_HELP_PRIVATE")) {
            std::cerr << private_output << '\n';
        }
        return std::getenv("AGY_TEST_HELP_FAIL") ? 9 : 0;
    }
    if (argc > 1 && std::string_view(argv[argc - 1]).starts_with("--prompt-interactive=")) {
        auto arguments = addon::Json_value::array();
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(addon::Json_value(argv[index]));
        }
        std::cout << addon::serialize_json(arguments) << '\n';
        return 0;
    }
    // A copy of this executable stands in for the native program, whose
    // version probe the lifecycle and delegation gates run, in the version
    // the probed environment names.
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
#ifdef _WIN32
        if (std::getenv("AGY_TEST_REQUIRE_NO_AUTO_UPDATE")) {
            const char* disabled = std::getenv("AGY_CLI_DISABLE_AUTO_UPDATE");
            if (!disabled || std::string_view(disabled) != "true") {
                return 7;
            }
        }
#else
        if (const char* custody = std::getenv("AGY_TEST_CUSTODY")) {
            if (custody != std::to_string(getpgrp()) + ":" + std::to_string(getsid(0))) return 8;
        }
#endif
        const char* version = std::getenv("AGY_TEST_VERSION");
        std::cout << "Antigravity " << (version ? version : "1.2.8") << "\n";
        return 0;
    }
    require(argc == 2 || argc == 3,
        "usage: agy_provider_tests <package directory> "
        "[--initial-prompt|--resume|--permission-snapshot|--status-line|--hook-root-members|"
        "--hook-home-provenance|--transcript-tail]");
    if (argc == 3) {
        const std::string_view contract = argv[2];
        if (contract == "--initial-prompt") {
            launcher_contract();
            initial_prompt_contract(argv[0]);
        }
        else
        if (contract == "--resume") {
            resume_inspection_adapter_contract(addon::path_from_utf8(argv[1]));
            resume_contract(argv[0]);
            native_selection_preparation_contract(addon::path_from_utf8(argv[1]), argv[0]);
        }
        else
        if (contract == "--permission-snapshot") {
            permission_snapshot_adapter_contract(addon::path_from_utf8(argv[1]));
        }
        else
        if (contract == "--status-line") {
            status_line_contract();
            status_line_transform_contract();
        }
        else
        if (contract == "--hook-root-members") {
            hook_root_member_contract();
        }
        else
        if (contract == "--hook-home-provenance") {
            hook_home_provenance_contract();
        }
        else
        if (contract == "--transcript-tail") {
            transcript_contract();
        }
        else {
            require(false, "unknown provider contract filter");
        }
        std::cout << "PASS: " << contract << '\n';
        return 0;
    }
    launcher_approval_contract();
    launcher_contract();
    quota_contract(argv[0]);
    launch_environment_contract();
    initial_prompt_contract(argv[0]);
    resume_contract(argv[0]);
    resume_inspection_adapter_contract(addon::path_from_utf8(argv[1]));
    native_selection_contract();
    native_selection_preparation_contract(addon::path_from_utf8(argv[1]), argv[0]);
    startup_contract();
    lifecycle_contract();
    status_line_contract();
    permission_snapshot_adapter_contract(addon::path_from_utf8(argv[1]));
    declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
    turn_identity_contract();
    transcript_contract();
    hook_transform_contract(addon::path_from_utf8(argv[1]));
    status_line_transform_contract();
    hook_root_member_contract();
    hook_home_provenance_contract();
    hooks_installation_preparation_contract();
    delegated_contract(argv[0]);
    inspection_contract(argv[0]);
    std::cout << "PASS: Antigravity native parity contracts\n";
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index) {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            argv[index], -1, nullptr, 0, nullptr, nullptr);
        if (count <= 0) {
            return 2;
        }
        std::string argument(count, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            argv[index], -1, argument.data(), count, nullptr, nullptr);
        argument.pop_back();
        arguments.push_back(std::move(argument));
    }
    std::vector<char*> pointers;
    for (auto& argument : arguments) {
        pointers.push_back(argument.data());
    }
    return run_tests(argc, pointers.data());
}
#else
int main(int argc, char** argv)
{
    return run_tests(argc, argv);
}
#endif
