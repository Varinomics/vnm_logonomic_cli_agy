#include "agy_events.h"
#include "agy_launcher.h"
#include "agy_preparation.h"
#include "agy_quota.h"
#include "agy_sessions.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sidecar.h>

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
        m_path(fs::temp_directory_path() / ("agy-contract-" + addon::random_nonce_hex()))
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

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
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
    require(agy::parse_version_output("Antigravity 1.1.11\n", "") == "1.1.11", "labelled native version");
    require(agy::parse_version_output("1.1.11\n", "agy 1.1.12\n").empty(), "conflicting version streams");
    require(!agy::version_is_at_least("1.1.10", 1, 1, 11), "lower than observed hook contract");
    require(!agy::version_is_at_least("1.1.11-rc.1", 1, 1, 11), "prerelease is below stable minimum");
    require(agy::version_is_at_least("3.2.0", 1, 1, 11), "future native version has no upper allowlist");
}

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
    request.context.launch_environment->platform_id = "linux-x86_64";
#endif
    Invocation prepare("prepare", addon::encode_probe_prepare_request(request, 2));
    agy::handle_quota(prepare);
    addon::Contract_failure failure;
    const auto process = addon::decode_structured_prepare_result(prepare.result(), &failure, 2);
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
    const auto admitted = addon::decode_structured_prepare_result(populated.result(), &failure, 2);
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
    inspection.inspection.timed_out = true;
    Invocation timeout("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(timeout);
    const auto timed_out = addon::decode_quota_inspect_result(timeout.result(), true, &failure, 2);
    require(timed_out && timed_out->observation->state == addon::Quota_status::FAILED,
        "timed-out process output never becomes ready quota");
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
    request.launch_environment->platform_id = "linux-x86_64";
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

// Oracle: interactive-resume@2 (the SDK's docs/interactive_resume_1.md and
// session_contracts.h) and agy's `--conversation <ID>` selector.
void resume_contract(const char* executable)
{
    Workspace workspace;
#ifdef _WIN32
    const auto native_program = workspace.path() / "agy.exe";
    constexpr auto platform   = "windows-x86_64";
    constexpr auto home_name  = "USERPROFILE";
    const std::string bridge  = "helpers/agy_launch_bridge.exe";
#else
    const auto native_program = workspace.path() / "agy";
    constexpr auto platform   = "linux-x86_64";
    constexpr auto home_name  = "HOME";
    const std::string bridge  = "helpers/agy_launch_bridge";
#endif
    fs::copy_file(fs::absolute(executable), native_program);
    const std::string conversation = "ec33ebf9-0cba-4100-8142-c61503f6c587";
    const auto program = path_text(native_program);

    addon::Resume_prepare_request request;
    request.context.launcher = {"COLOR=blue \"" + program + "\" --model gemini-3-pro --sandbox", "direct", ""};
    request.context.launcher_argv = {program, "--model", "gemini-3-pro", "--sandbox"};
    request.context.working_directory = path_text(workspace.path());
    request.context.launch_environment = addon::Launch_environment{};
    request.context.launch_environment->platform_id = platform;
    request.context.launch_environment->effective_environment.set(
        home_name, addon::Json_value(path_text(workspace.path())));
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
    require(resumed->patch.set == expected_environment && !resumed->patch.apply_managed_configuration,
        "resume enables the hooks the host installed before resume preparation and keeps literal assignments");

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
    for (const auto* options : {"--continue", "-c", "--conversation=other", "-p hello", "--print", "-i hello",
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
        "export COLOR=~/theme\n\"" + program + "\""})
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
}

addon::Classify_event_request hook_event(std::string name)
{
    addon::Classify_event_request request;
    request.event.name = std::move(name);
    request.event.body = json(R"({"conversationId":"Conversation-Parent"})");
    request.transition_nonce = std::string(32, 'a');
    return request;
}

// Oracle: the host advances a root turn only for a working fact naming a new
// provider_turn_id (lifecycle-events@1), and agy restarts invocationNum at
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
    request.launch_environment->platform_id = "linux-x86_64";
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
        addon::encode_delegated_prepare_request(fixture.request, 3), cancelled,
        addon::Capability_pair{"delegated-session", 3});
    agy::handle_sessions(*fixture.invocation);
    addon::Contract_failure failure;
    fixture.prepared = fixture.invocation->status() == addon::Result_status::SUCCESS
        ? addon::decode_delegated_prepare_result(fixture.invocation->result(), &failure, 3)
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
    constexpr auto platform   = "linux-x86_64";
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

// Oracle: delegated-session@1-3 (the SDK's docs and session_contracts.h), the
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
        "--continue", "--conversation=ec33ebf9", "--remote-control", "--remote-control=true", "models"})
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
    Invocation bare("prepare", addon::encode_structured_prepare_request(structured, 3), false,
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
    Invocation invocation("inspect", addon::encode_structured_inspect_request(request, 3), false,
        addon::Capability_pair{"structured-session", 3});
    agy::handle_sessions(invocation);
    require(invocation.status() == addon::Result_status::SUCCESS, "inspection answers the host");
    addon::Contract_failure failure;
    auto result = addon::decode_structured_inspect_result(invocation.result(), final, &failure, 3);
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

int main(int argc, char** argv)
{
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
#endif
        const char* version = std::getenv("AGY_TEST_VERSION");
        std::cout << "Antigravity " << (version ? version : "1.2.8") << "\n";
        return 0;
    }
    require(argc == 2, "usage: agy_provider_tests <package directory>");
    launcher_contract();
    quota_contract(argv[0]);
    launch_environment_contract();
    resume_contract(argv[0]);
    startup_contract();
    lifecycle_contract();
    declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
    turn_identity_contract();
    transcript_contract();
    hook_transform_contract(addon::path_from_utf8(argv[1]));
    hooks_installation_preparation_contract();
    delegated_contract(argv[0]);
    inspection_contract(argv[0]);
    std::cout << "PASS: Antigravity native parity contracts\n";
    return 0;
}
