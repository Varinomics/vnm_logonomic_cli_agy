#include "agy_events.h"
#include "agy_launcher.h"
#include "agy_preparation.h"
#include "agy_quota.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
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
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_finished = true;
        m_status = status;
        m_reason = std::move(reason);
    }
    addon::Result_status status() const { return m_status; }
    const std::string& reason() const { return m_reason; }
    const addon::Json_value& result() const { return m_result; }

private:
    addon::Invoke_request m_request;
    addon::Json_value m_result;
    std::string m_reason;
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

    // The host freezes the launcher's environment in the spelling it found,
    // which on Windows is `Path` rather than `PATH`, and refuses a proposal
    // that names any variable twice under ASCII case folding.
    request.context.launch_environment->effective_environment = json(R"({
        "Path": "C:\\Windows\\system32;C:\\Users\\Jane Doe\\AppData\\Local\\agy\\bin",
        "PATHEXT": ".COM;.EXE;.BAT;.CMD",
        "USERPROFILE": "C:\\Users\\Jane Doe",
        "LOCALAPPDATA": "C:\\Users\\Jane Doe\\AppData\\Local",
        "APPDATA": "C:\\Users\\Jane Doe\\AppData\\Roaming",
        "SystemRoot": "C:\\Windows"
    })");
    Invocation populated("prepare", addon::encode_probe_prepare_request(request, 2));
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

    addon::Probe_inspect_request inspection;
    inspection.inspection.protocol_state = "await_exit";
    Invocation waiting("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(waiting);
    const auto incomplete = addon::decode_quota_inspect_result(waiting.result(), false, &failure, 2);
    require(incomplete && !incomplete->complete && !incomplete->observation, "quota waits for CLI exit");
    inspection.inspection.final = true;
    inspection.inspection.timed_out = true;
    inspection.inspection.stdout_bytes = addon::serialize_json(quota_payload());
    Invocation timeout("inspect", addon::encode_probe_inspect_request(inspection, 2));
    agy::handle_quota(timeout);
    const auto timed_out = addon::decode_quota_inspect_result(timeout.result(), true, &failure, 2);
    require(timed_out && timed_out->observation->state == addon::Quota_status::FAILED,
        "timed-out process output never becomes ready quota");
    Invocation cancelled("prepare", addon::encode_probe_prepare_request(request, 2), true);
    agy::handle_quota(cancelled);
    require(cancelled.status() == addon::Result_status::CANCELLED, "cancelled quota preparation launches nothing");
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
    constexpr bool windows    = true;
    const std::string bridge  = "helpers/agy_launch_bridge.exe";
#else
    const auto native_program = workspace.path() / "agy";
    constexpr auto platform   = "linux-x86_64";
    constexpr auto home_name  = "HOME";
    constexpr bool windows    = false;
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
    require(resumed->patch.set == std::vector<std::pair<std::string, std::string>>{{"COLOR", "blue"}},
        "the launcher's literal assignments reach the resumed process; absent hooks stay disabled");

    const auto path = workspace.path() / ".gemini" / "config" / "hooks.json";
    fs::create_directories(path.parent_path());
    {
        auto hooks = addon::Json_value::object();
        hooks.set("logonomic-hook-relay", agy::hook_definition(windows));
        std::ofstream stream(path, std::ios::binary);
        stream << addon::serialize_json(hooks);
    }
    invocation = prepare();
    resumed = addon::decode_resume_prepare_result(invocation->result(), &failure, 2);
    require(resumed && resumed->patch.set == std::vector<std::pair<std::string, std::string>>{
        {"COLOR", "blue"}, {"AGY_LOGONOMIC_HOOKS_ENABLED", "1"}, {"AGY_LOGONOMIC_LIFECYCLE_ENABLED", "1"}},
        "installed hooks report into the resumed conversation, as into a launch");
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
        "-- --sandbox", "--help", "--version"})
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

void hooks_are_read_only()
{
    Workspace workspace;
    const auto path = workspace.path() / ".gemini" / "config" / "hooks.json";
    fs::create_directories(path.parent_path());
    addon::Prepare_request request;
    request.launcher = {"agy", "direct", ""};
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    constexpr bool windows = true;
    request.launch_environment->platform_id = "windows-x86_64";
    constexpr auto home_name = "USERPROFILE";
#else
    constexpr bool windows = false;
    request.launch_environment->platform_id = "linux-x86_64";
    constexpr auto home_name = "HOME";
#endif
    request.launch_environment->effective_environment.set(home_name, addon::Json_value(path_text(workspace.path())));
    require(!agy::hooks_preinstalled(request.launcher, request.launch_environment) && !fs::exists(path),
        "absent global configuration is never created");
    auto hooks = addon::Json_value::object();
    hooks.set("user-owned", json(R"({"enabled":true})"));
    hooks.set("logonomic-hook-relay", agy::hook_definition(windows));
    const auto bytes = addon::serialize_json(hooks);
    {
        std::ofstream stream(path, std::ios::binary);
        stream << bytes;
    }
    require(agy::hooks_preinstalled(request.launcher, request.launch_environment), "only exact supported stanza opts in");
    request.launcher.environment_text = std::string(home_name) + "=/different-home";
    require(!agy::hooks_preinstalled(request.launcher, request.launch_environment),
        "launcher home override cannot claim global hook config");
    std::ifstream stream(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(stream), {}};
    require(after == bytes, "launch preparation preserves exact user-owned file bytes");
}

} // namespace

int main(int argc, char** argv)
{
    // A copy of this executable stands in for the native program, whose
    // version probe the lifecycle gate runs.
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "Antigravity 1.2.8\n";
        return 0;
    }
    require(argc == 2, "usage: agy_provider_tests <package directory>");
    launcher_contract();
    quota_contract(argv[0]);
    resume_contract(argv[0]);
    lifecycle_contract();
    declared_profile_admits_every_shape(addon::path_from_utf8(argv[1]));
    turn_identity_contract();
    transcript_contract();
    hooks_are_read_only();
    std::cout << "PASS: Antigravity native parity contracts\n";
    return 0;
}
