#include "agy_sessions.h"
#include "agy_launcher.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

namespace fs = std::filesystem;
using Json = addon::Json_value;

// The agy release whose headless stream-json protocol, added-folder MCP
// loading and headless permission behaviour this contract was validated
// against. Newer releases attempt the same strict contract.
constexpr int k_minimum_major = 1;
constexpr int k_minimum_minor = 2;
constexpr int k_minimum_patch = 11;

// The folder added to the agy workspace. Its `.agents/mcp_config.json` is a
// customization root agy loads additively with the user's own servers; it
// also holds the host's Task catalog receipts.
constexpr std::string_view k_workspace_folder = "agy";

// Options the delegated protocol owns: conversation selection, the prompt
// mode, its formats and deadline, the agent persona and execution mode whose
// tool surface was validated, and a remote connection opened at startup.
// literal_options refuses positional words.
const std::vector<std::string_view>& delegated_refused_options()
{
    static const std::vector<std::string_view> refused{
        "c", "continue", "conversation", "new-project", "p", "print", "prompt", "i", "prompt-interactive",
        "h", "help", "version", "input-format", "output-format", "json-schema", "print-timeout", "agent", "mode",
        "remote-control"};
    return refused;
}

// agy caches each server's tool schemas in the user profile under a directory
// named after the server, so a Task server identity must be a plain name.
bool plain_server_id(std::string_view server_id)
{
    return !server_id.empty() && server_id.size() <= addon::limits::k_max_delegated_server_id_bytes &&
        server_id.front() >= 'a' && server_id.front() <= 'z' &&
        std::all_of(server_id.begin(), server_id.end(), [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                character == '_';
        });
}

std::string generic_path(const fs::path& path)
{
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string text(const Json& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_string() ? value->as_string() : std::string{};
}

Json object(std::string_view bytes)
{
    addon::Json_parse_error error;
    auto value = addon::parse_json(bytes, addon::Json_parse_limits{}, &error);
    if (!value || !value->is_object()) {
        throw std::runtime_error("invalid_json_object");
    }
    return std::move(*value);
}

bool same_directory(std::string_view left, std::string_view right)
{
    auto first  = addon::path_to_utf8(addon::path_from_utf8(left).lexically_normal());
    auto second = addon::path_to_utf8(addon::path_from_utf8(right).lexically_normal());
#ifdef _WIN32
    const auto fold = [](std::string& value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    };
    fold(first);
    fold(second);
#endif
    return !first.empty() && first == second;
}

// The host commits each receipt's nonce only after the server's own
// `tools/list` succeeded through the relay, and before it answers agy.
bool task_servers_attested(const Json& attestations)
{
    if (!attestations.is_array() || attestations.as_array().empty()) {
        return false;
    }
    for (const auto& attestation : attestations.as_array()) {
        const auto nonce = text(attestation, "nonce");
        const auto path  = addon::path_from_utf8(text(attestation, "path"));
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error || !fs::is_regular_file(status) || nonce.size() != 32) {
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        std::string bytes(nonce.size() + 1, '\0');
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(file.gcount()));
        if (bytes != nonce) {
            return false;
        }
    }
    return true;
}

// A step record as agy writes it. The recorded step states are ACTIVE, DONE
// and ERROR; step types were recorded only as far as runs exercised them, so
// a well-formed unrecorded type stays presentation. Only a result authenticates.
bool step_record(const Json& step)
{
    const auto* index = step.find("step_index");
    const auto type   = text(step, "step_type");
    const auto state  = text(step, "state");
    return !text(step, "conversation_id").empty() && index && index->is_integer() && index->as_integer() >= 0 &&
        (state == "ACTIVE" || state == "DONE" || state == "ERROR") &&
        !type.empty() && type.front() >= 'a' && type.front() <= 'z' &&
        std::all_of(type.begin(), type.end(), [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                character == '_';
        });
}

addon::Structured_inspect_result failed(std::string reason)
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = std::move(reason);
    return result;
}

bool task_server(const Json& state, const std::string& name)
{
    const auto* servers = state.find("task_servers");
    if (!servers || !servers->is_array()) {
        throw std::runtime_error("invalid_protocol_state");
    }
    return std::any_of(servers->as_array().begin(), servers->as_array().end(),
        [&name](const Json& server) { return server.is_string() && server.as_string() == name; });
}

// The `permissions.allow` entries of the user's own agy settings in the home
// the process runs with; none when the file is missing or unreadable.
std::vector<std::string> allowed_permissions(
    const std::vector<std::pair<std::string, std::string>>& environment, bool windows)
{
    const auto home = std::find_if(environment.begin(), environment.end(), [windows](const auto& entry) {
        return windows ? addon::lower(entry.first) == "userprofile" : entry.first == "HOME";
    });
    if (home == environment.end() || !addon::path_from_utf8(home->second).is_absolute()) {
        return {};
    }
    const auto path = addon::path_from_utf8(home->second) / ".gemini" / "antigravity-cli" / "settings.json";
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > 1024 * 1024) {
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    addon::Json_parse_error parse_error;
    const auto settings = addon::parse_json(bytes, addon::Json_parse_limits{}, &parse_error);
    const auto* permissions = settings && settings->is_object() ? settings->find("permissions") : nullptr;
    const auto* allow = permissions && permissions->is_object() ? permissions->find("allow") : nullptr;
    std::vector<std::string> result;
    if (allow && allow->is_array()) {
        for (const auto& entry : allow->as_array()) {
            if (entry.is_string()) {
                result.push_back(entry.as_string());
            }
        }
    }
    return result;
}

// agy grants a tool call in a headless run only through the launcher's
// --dangerously-skip-permissions, which approves every tool, or the user's own
// allow entries, which name MCP tools as mcp(<server>/<tool>) with `*` for any
// tool. The package adds neither; it only checks that one covers every Task tool.
bool task_tools_allowed(const std::vector<std::string>& allowed, const std::vector<addon::Delegated_task_server>& servers)
{
    const auto granted = [&allowed](const std::string& entry) {
        return std::find(allowed.begin(), allowed.end(), entry) != allowed.end();
    };
    return granted("mcp(*)") || std::all_of(servers.begin(), servers.end(), [&granted](const auto& server) {
        const auto name = "logonomic_" + server.server_id;
        return granted("mcp(" + name + "/*)") ||
            std::all_of(server.actions.begin(), server.actions.end(), [&granted, &name](const std::string& action) {
                return granted("mcp(" + name + "/" + action + ")");
            });
    });
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!request->control.connection) {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_purpose_unavailable);
        return;
    }
    if (request->prompt.empty() || !addon::is_valid_utf8(request->prompt) ||
        !addon::path_from_utf8(request->context.working_directory).is_absolute())
    {
        context.fail(addon::Result_status::FAILED, "invalid_implementation_request");
        return;
    }
    if (!std::all_of(request->control.servers.begin(), request->control.servers.end(),
        [](const auto& server) { return plain_server_id(server.server_id); }))
    {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_unsupported");
        return;
    }
    // The bridge replaces the launcher's shell with a direct native
    // invocation, which reproduces only a literal launcher.
    const auto& environment = *request->context.launch_environment;
    const bool windows = environment.platform_id == "windows-x86_64";
    const auto command = recognize_launcher(request->context.launcher.script_body);
    if (!command || !command->sole_command ||
        !std::all_of(command->environment.begin(), command->environment.end(),
            [windows](const auto& assignment) { return literal_assignment(assignment.second, windows); }) ||
        !literal_options(command->arguments, windows, delegated_refused_options()))
    {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_launcher_program_unavailable);
        return;
    }
    // The run keeps the launcher's permission posture; without a grant for the
    // Task tools, headless agy denies them and the task cannot use its servers.
    const auto process_environment = direct_environment(environment, *command);
    if (!literal_switch(command->arguments, "dangerously-skip-permissions") &&
        !task_tools_allowed(allowed_permissions(process_environment, windows), request->control.servers))
    {
        std::string entries;
        for (const auto& server : request->control.servers) {
            entries += (entries.empty() ? "\"mcp(logonomic_" : ", \"mcp(logonomic_") + server.server_id + "/*)\"";
        }
        context.fail(addon::Result_status::UNAVAILABLE, "task_tools_not_permitted",
            "Antigravity denies Logonomic's Task tools in a headless run unless they are granted. Add " + entries +
            " to permissions.allow in ~/.gemini/antigravity-cli/settings.json, or let the launcher pass "
            "--dangerously-skip-permissions, which approves every tool.");
        return;
    }
    const auto launch = verified_launch(
        request->context.launcher, environment.effective_environment, request->context.working_directory, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (!launch || !version_is_at_least(launch->version, k_minimum_major, k_minimum_minor, k_minimum_patch)) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_launcher_program_unavailable);
        return;
    }

    // Each Task server is the host's relay; its receipt proves the real
    // server's catalog. Stable names keep agy's per-server schema cache in the
    // user profile bounded; the servers themselves are private to this folder.
    const auto workspace   = addon::path_from_utf8(request->control.private_artifact_root) / k_workspace_folder;
    const auto& connection = *request->control.connection;
    addon::Delegated_prepare_result result;
    auto servers      = Json::object();
    auto attestations = Json::array();
    auto task_servers = Json::array();
    for (const auto& server : request->control.servers) {
        task_servers.push_back(Json("logonomic_" + server.server_id));
        const auto nonce = addon::random_nonce_hex();
        // The host accepts a receipt path only in its clean '/'-separated form.
        const auto receipt = generic_path(workspace / ".agents" / (server.server_id + ".ready"));
        auto arguments = Json::array();
        for (const auto& argument : {
            std::string("--descriptor"), connection.descriptor_path, std::string("--server"), server.server_id,
            std::string("--attestation-path"), receipt, std::string("--attestation-nonce"), nonce})
        {
            arguments.push_back(Json(argument));
        }
        auto definition = Json::object();
        definition.set("command", Json(connection.relay_program));
        definition.set("args",    std::move(arguments));
        definition.set("env",     Json::object());
        servers.set("logonomic_" + server.server_id, std::move(definition));
        auto attestation = Json::object();
        attestation.set("path",  Json(receipt));
        attestation.set("nonce", Json(nonce));
        attestations.push_back(std::move(attestation));
        result.acknowledgment.server_ids.push_back(server.server_id);
    }
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    auto configuration = Json::object();
    configuration.set("mcpServers", std::move(servers));
    result.private_artifacts.push_back({std::string(k_workspace_folder) + "/.agents/mcp_config.json",
        addon::serialize_json(configuration)});

#ifdef _WIN32
    result.process.executable = "helpers/agy_launch_bridge.exe";
#else
    result.process.executable = "helpers/agy_launch_bridge";
#endif
    // The prompt follows the startup record on stdin, so an empty `--print`
    // selects print mode; slash expansion would rewrite the raw prompt. The
    // launcher's own options decide permissions: headless agy denies every tool
    // they and the user's settings do not grant.
    result.process.argv = {launch->program};
    auto arguments = command->arguments;
    if (request->model_selection) {
        const auto& selection = *request->model_selection;
        if (selection.model) {
            replace_literal_option(arguments, "model", *selection.model);
        }
        if (selection.reasoning_effort) {
            replace_literal_option(arguments, "effort", *selection.reasoning_effort);
        }
        result.model_selection = selection;
    }
    result.process.argv.insert(result.process.argv.end(), arguments.begin(), arguments.end());
    result.process.argv.insert(result.process.argv.end(), {
        "--input-format", "stream-json", "--output-format", "stream-json", "--disable-slash-commands",
        "--add-dir", generic_path(workspace), "--print="});
    // The frozen launcher environment is copied whole, so no name is also
    // listed for pass-through: the host refuses a name supplied both ways.
    result.process.environment    = process_environment;
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;

    auto state = Json::object();
    state.set("stage",             Json("await_init"));
    state.set("prompt_base64",     Json(addon::encode_base64(request->prompt)));
    state.set("working_directory", Json(request->context.working_directory));
    state.set("attestations",      std::move(attestations));
    state.set("task_servers",      std::move(task_servers));
    state.set("last_mcp_server",   Json(""));
    state.set("last_mcp_answered", Json(false));
    state.set("conversation_id",   Json(""));
    state.set("pending_base64",    Json(""));
    state.set("result_base64",     Json(""));
    result.inspection.incremental     = true;
    result.inspection.keep_stdin_open = true;
    result.inspection.protocol_state  = addon::serialize_json(state);
    auto payload = addon::encode_delegated_prepare_result(result);
    if (!addon::validate_session_exchange_payload(payload, &failure)) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(std::move(payload));
}

} // namespace

// The recorded headless protocol of agy: exactly one `init` record before
// any input, the root conversation's `user_input` step once agy has loaded
// the Task servers, progress `step_update`s, and one terminal `result` per
// turn. Stdin EOF after the only message ends the process after that turn.
addon::Structured_inspect_result inspect_session(const addon::Structured_inspection& inspection)
{
    try {
        auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
        if (!parsed || !parsed->is_object()) return failed("invalid_structured_output");
        Json state = std::move(*parsed);
        const auto pending_value = addon::decode_base64(text(state, "pending_base64"));
        const auto result_value  = addon::decode_base64(text(state, "result_base64"));
        if (!pending_value || !result_value) {
            return failed("invalid_protocol_state");
        }
        auto stage        = text(state, "stage");
        auto conversation = text(state, "conversation_id");
        auto answer       = *result_value;
        auto pending      = *pending_value + inspection.stdout_bytes;
        addon::Structured_inspect_result result;
        const auto record = [&result](addon::Presentation_event_kind kind) {
            result.presentation_state = addon::Presentation_state::AVAILABLE;
            if (!result.presentation.empty() && result.presentation.back().kind == kind) {
                ++result.presentation.back().count;
            }
            else {
                result.presentation.push_back({kind, 1});
            }
        };
        std::size_t consumed = 0;
        for (std::size_t end; (end = pending.find('\n', consumed)) != std::string::npos;) {
            const std::string_view line(pending.data() + consumed, end - consumed);
            consumed = end + 1;
            if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
                continue;
            }
            // agy writes nothing between its startup record and the prompt.
            if (result.stdin_bytes) {
                return failed("unexpected_startup_output");
            }
            if (stage == "settled") {
                return failed("output_after_result");
            }
            const auto value = object(line);
            const auto event = text(value, "event");
            if (stage == "await_init") {
                // agy answers a session it cannot start, such as one with an
                // invalid model selection, with a failed result instead.
                if (event == "result") {
                    return failed("startup_failed");
                }
                const auto* init = value.find("init");
                if (event != "init" || !init || !init->is_object()) {
                    return failed("unexpected_startup_output");
                }
                conversation = text(value, "conversation_id");
                if (conversation.empty() || !same_directory(text(*init, "cwd"), text(state, "working_directory"))) {
                    return failed("session_provenance_mismatch");
                }
                const auto prompt = addon::decode_base64(text(state, "prompt_base64"));
                if (!prompt || prompt->empty()) {
                    return failed("invalid_protocol_state");
                }
                auto message = Json::object();
                message.set("content", Json(*prompt));
                auto frame = Json::object();
                frame.set("event",   Json("user"));
                frame.set("message", std::move(message));
                result.stdin_bytes = addon::serialize_json(frame) + '\n';
                result.close_stdin = true;
                state.set("prompt_base64", Json(""));
                stage = "await_first_step";
                record(addon::Presentation_event_kind::REASONING);
                continue;
            }
            if (event == "step_update") {
                const auto* step = value.find("step_update");
                if (!step || !step->is_object() || !step_record(*step)) {
                    return failed("invalid_structured_output");
                }
                const auto type = text(*step, "step_type");
                if (stage == "await_first_step") {
                    if (text(*step, "conversation_id") != conversation || type != "user_input" ||
                        text(*step, "state") != "DONE" || step->find("step_index")->as_integer() != 0)
                    {
                        return failed("session_provenance_mismatch");
                    }
                    // agy loads the added folder's servers before this step
                    // and silently omits one that failed. The host kills the
                    // process on this failure; agy may progress until then.
                    const auto* attestations = state.find("attestations");
                    if (!attestations || !task_servers_attested(*attestations)) {
                        return failed("task_servers_not_ready");
                    }
                    state.set("attestations", Json::array());
                    stage = "running";
                }
                // agy ends a turn at a headless denial, so a denial refers to
                // the root conversation's last MCP call.
                const auto* tool = step->find("tool_info");
                if (type == "tool" && text(*step, "tool_name") == "call_mcp_tool" &&
                    text(*step, "conversation_id") == conversation && tool && tool->is_object())
                {
                    const auto* parameters = tool->find("parameters");
                    state.set("last_mcp_server",
                        Json(parameters && parameters->is_object() ? text(*parameters, "ServerName") : std::string{}));
                    state.set("last_mcp_answered", Json(tool->find("output") != nullptr));
                }
                record(
                    type == "agent_response" ? addon::Presentation_event_kind::ANALYSIS_UPDATE :
                    type == "tool"           ? addon::Presentation_event_kind::TOOL_RESPONSE :
                                               addon::Presentation_event_kind::REASONING);
                continue;
            }
            if (event == "result") {
                const auto* turn = value.find("result");
                if (stage != "running" || !turn || !turn->is_object() ||
                    text(*turn, "conversation_id") != conversation)
                {
                    return failed("invalid_result_provenance");
                }
                const auto status = text(*turn, "status");
                if (status == "ERROR") {
                    return failed("provider_turn_failed");
                }
                const auto* response = turn->find("response");
                const auto* denied   = turn->find("denied_actions");
                if (status != "SUCCESS" || !response || !response->is_string() || (denied && !denied->is_array())) {
                    return failed("invalid_final_result");
                }
                // A tool the launcher's posture does not grant is denied, and
                // agy ends the turn there without a response.
                if (denied && !denied->as_array().empty()) {
                    const auto& actions = denied->as_array();
                    const auto* answered = state.find("last_mcp_answered");
                    if (std::any_of(actions.begin(), actions.end(),
                            [](const Json& action) { return text(action, "action") == "mcp"; }) &&
                        task_server(state, text(state, "last_mcp_server")) &&
                        answered && answered->is_boolean() && !answered->as_boolean())
                    {
                        return failed("task_tools_not_permitted");
                    }
                    if (response->as_string().empty()) {
                        return failed("tool_permission_denied");
                    }
                }
                // A denied or failed turn can still report success with no
                // response; it is not an authenticated result.
                if (response->as_string().empty()) {
                    return failed("empty_final_result");
                }
                if (response->as_string().size() > addon::limits::k_max_structured_stream_bytes) {
                    return failed("final_result_too_large");
                }
                answer = response->as_string();
                stage  = "settled";
                record(addon::Presentation_event_kind::RESULT_AUTHENTICATED);
                continue;
            }
            return failed("unsupported_structured_event");
        }
        pending.erase(0, consumed);
        if (inspection.final) {
            if (inspection.timed_out) {
                return failed("process_timed_out");
            }
            if (inspection.exit_code != 0) {
                return failed("process_failed");
            }
            if (stage != "settled" || pending.find_first_not_of(" \t\r\n") != std::string::npos) {
                return failed("unauthenticated_final_result");
            }
            result.complete     = true;
            result.result_bytes = std::move(answer);
            return result;
        }
        state.set("stage",           Json(stage));
        state.set("conversation_id", Json(conversation));
        state.set("pending_base64",  Json(addon::encode_base64(pending)));
        state.set("result_base64",   Json(addon::encode_base64(answer)));
        result.protocol_state = addon::serialize_json(state);
        if (!addon::exchange_protocol_state_valid(result.protocol_state)) {
            return failed("protocol_state_limit");
        }
        return result;
    }
    catch (const std::exception&) {
        return failed("invalid_structured_output");
    }
}

void handle_sessions(addon::Invocation_context& context)
{
    const auto& request = context.request();
    addon::Contract_failure failure;
    if (request.capability.capability_id == "structured-session" && request.operation == "inspect") {
        const auto inspection = addon::decode_structured_inspect_request(request.payload, &failure);
        if (!inspection) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_structured_inspect_result(inspect_session(inspection->inspection)));
    }
    else
    if (request.operation != "prepare") {
        context.reject_unknown_operation();
    }
    else
    if (request.capability.capability_id == "delegated-session") {
        prepare_delegated(context);
    }
    else
    if (request.capability.capability_id == "structured-session") {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_purpose_unavailable,
            "Antigravity implementation requires delegated task control.");
    }
    else {
        context.reject_unknown_operation();
    }
}

} // namespace agy
