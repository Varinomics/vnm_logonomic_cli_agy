#include "agy_events.h"
#include "agy_resume.h"
#include "agy_startup.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/provider_event_diagnostic.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

namespace fs = std::filesystem;
constexpr std::size_t k_max_children      = 32;
constexpr std::size_t k_max_tail_bytes    = 1024 * 1024;
constexpr std::size_t k_max_record_bytes  = 256 * 1024;
constexpr std::size_t k_max_tail_records  = 512;
constexpr std::size_t k_max_identity_units = 256;
constexpr char k_observation[] = "logonomicAgySubagentObservation";

std::string text(const addon::Json_value& value, std::string_view member)
{
    const auto* found = value.find(member);
    return found && found->is_string() ? found->as_string() : std::string{};
}

std::string trimmed(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == 0 && last == value.size() - 1) {
        return value;
    }
    return value.substr(first, last - first + 1);
}

bool bounded_identity(std::string_view value)
{
    if (value.empty() || !addon::is_valid_utf8(value) || value.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t units = 0;
    for (unsigned char character : value) {
        if ((character & 0xc0) != 0x80) {
            units += character >= 0xf0 ? 2 : 1;
        }
    }
    return units <= k_max_identity_units;
}

// URI authority syntax matters: a malformed file URI is not evidence of a
// child's log, even when its text happens to contain the child's identity.
bool valid_file_uri(std::string_view uri)
{
    if (uri.size() < 5 || (uri[0] != 'f' && uri[0] != 'F') ||
        (uri[1] != 'i' && uri[1] != 'I') || (uri[2] != 'l' && uri[2] != 'L') ||
        (uri[3] != 'e' && uri[3] != 'E') || uri[4] != ':' ||
        uri.find_first_of("\r\n") != std::string_view::npos ||
        uri.find('\0') != std::string_view::npos)
    {
        return false;
    }
    uri.remove_prefix(5);
    if (!uri.starts_with("//")) {
        return true;
    }
    uri.remove_prefix(2);
    auto authority = uri.substr(0, uri.find_first_of("/?#"));
    const auto user = authority.rfind('@');
    if (user != std::string_view::npos) {
        authority.remove_prefix(user + 1);
    }
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        auto address = authority.substr(1, close - 1);
        // A scoped IPv6 address may carry an encoded zone identifier.
        if (const auto zone = address.find("%25"); zone != std::string_view::npos) {
            if (zone + 3 == address.size()) {
                return false;
            }
            address = address.substr(0, zone);
        }
        if ((address.starts_with(':') && !address.starts_with("::")) ||
            (address.ends_with(':') && !address.ends_with("::")))
        {
            return false;
        }
        const auto compression = address.find("::");
        if (compression != std::string_view::npos &&
            (address.find("::", compression + 2) != std::string_view::npos ||
                address.find(":::") != std::string_view::npos))
        {
            return false;
        }
        unsigned groups = 0;
        std::size_t begin = 0;
        while (begin < address.size()) {
            const auto end = address.find(':', begin);
            const auto part = address.substr(begin,
                end == std::string_view::npos ? address.size() - begin : end - begin);
            if (!part.empty()) {
                if (part.find('.') != std::string_view::npos) {
                    if (end != std::string_view::npos) {
                        return false;
                    }
                    unsigned octets = 0;
                    std::size_t offset = 0;
                    while (offset < part.size()) {
                        const auto dot = part.find('.', offset);
                        const auto octet = part.substr(offset,
                            dot == std::string_view::npos ? part.size() - offset : dot - offset);
                        unsigned value = 0;
                        if (octet.empty() || octet.size() > 3) {
                            return false;
                        }
                        for (const char character : octet) {
                            if (character < '0' || character > '9') {
                                return false;
                            }
                            value = value * 10 + static_cast<unsigned>(character - '0');
                        }
                        if (value > 255) {
                            return false;
                        }
                        ++octets;
                        if (dot == std::string_view::npos) {
                            break;
                        }
                        offset = dot + 1;
                        if (offset == part.size()) {
                            return false;
                        }
                    }
                    if (octets != 4) {
                        return false;
                    }
                    groups += 2;
                }
                else {
                    if (part.size() > 4 || part.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) {
                        return false;
                    }
                    ++groups;
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 1;
        }
        if ((compression == std::string_view::npos &&
                (groups != 8 || address.starts_with(':') || address.ends_with(':'))) ||
            (compression != std::string_view::npos && groups >= 8))
        {
            return false;
        }
        authority.remove_prefix(close + 1);
    }
    else {
        if (authority.find_first_of("[] \\") != std::string_view::npos) {
            return false;
        }
        const auto colon = authority.find(':');
        if (colon == std::string_view::npos) {
            return true;
        }
        authority.remove_prefix(colon);
    }
    if (authority.empty()) {
        return true;
    }
    if (!authority.starts_with(':')) {
        return false;
    }
    authority.remove_prefix(1);
    unsigned port = 0;
    for (const char character : authority) {
        if (character < '0' || character > '9' || port > 6553) {
            return false;
        }
        port = port * 10 + static_cast<unsigned>(character - '0');
    }
    return port <= 65535;
}

std::optional<std::int64_t> integer(const addon::Json_value* value)
{
    if (!value || !value->is_number()) {
        return std::nullopt;
    }
    const double number = value->as_number();
    if (number < 0 || number > 9007199254740991.0 || std::floor(number) != number) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
}

bool invokes_subagent(const addon::Json_value& payload)
{
    const auto* call = payload.find("toolCall");
    return call && text(*call, "name") == "invoke_subagent";
}

std::optional<std::size_t> expected_children(const addon::Json_value& payload)
{
    const auto* call = payload.find("toolCall");
    const auto* args = call ? call->find("args") : nullptr;
    const auto* children = args ? args->find("Subagents") : nullptr;
    std::optional<addon::Json_value> parsed;
    if (children && children->is_string()) {
        addon::Json_parse_limits limits;
        limits.max_bytes = k_max_record_bytes;
        addon::Json_parse_error failure;
        parsed = addon::parse_json(children->as_string(), limits, &failure);
        children = parsed ? &*parsed : nullptr;
    }
    if (!children || !children->is_array() || children->as_array().empty() ||
        children->as_array().size() > k_max_children)
    {
        return std::nullopt;
    }
    for (const auto& child : children->as_array()) {
        if (!child.is_object()) {
            return std::nullopt;
        }
    }
    return children->as_array().size();
}

std::vector<addon::Json_value> embedded_objects(std::string_view content)
{
    std::vector<addon::Json_value> result;
    std::size_t start = 0;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0; index < content.size(); ++index) {
        const char character = content[index];
        if (quoted) {
            if (escaped) {
                escaped = false;
            }
            else
            if (character == '\\') {
                escaped = true;
            }
            else
            if (character == '"') {
                quoted = false;
            }
            continue;
        }
        if (character == '"') {
            quoted = true;
        }
        else
        if (character == '{') {
            if (depth++ == 0) {
                start = index;
            }
        }
        else
        if (character == '}' && depth > 0 && --depth == 0) {
            addon::Json_parse_error failure;
            auto object = addon::parse_json(content.substr(start, index - start + 1), {}, &failure);
            if (object && object->is_object()) {
                result.push_back(std::move(*object));
                if (result.size() > k_max_children) {
                    return {};
                }
            }
        }
    }
    return depth == 0 && !quoted ? result : std::vector<addon::Json_value>{};
}

bool regular_transcript(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || !fs::is_regular_file(status)) {
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

std::optional<addon::Json_value> subagent_results(
    const addon::Json_value& payload, std::int64_t completed_step, std::size_t count)
{
    const auto parent = trimmed(text(payload, "conversationId"));
    const auto path   = addon::path_from_utf8(text(payload, "transcriptPath"));
    if (!bounded_identity(parent) || !path.is_absolute() || path.filename() != "transcript.jsonl" ||
        !regular_transcript(path))
    {
        return std::nullopt;
    }
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    if (error || std::find(canonical.begin(), canonical.end(), addon::path_from_utf8(parent)) == canonical.end()) {
        return std::nullopt;
    }
    std::ifstream stream(canonical, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    const auto start = std::max<std::streamoff>(0, size - static_cast<std::streamoff>(k_max_tail_bytes));
    stream.seekg(start);
    std::string bytes(static_cast<std::size_t>(size - start) + 1, '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    // A growing transcript may still fit in the tail window observed above.
    while (stream && bytes.size() <= k_max_tail_bytes) {
        const auto offset = bytes.size();
        const auto size = std::min(k_max_tail_bytes + 1, std::max<std::size_t>(4096, offset * 2));
        // Reserve from empty so string growth does not double an existing capacity past the limit.
        std::string grown;
        grown.reserve(size);
        grown.append(bytes);
        bytes = std::move(grown);
        bytes.resize(size);
        stream.read(bytes.data() + offset, static_cast<std::streamsize>(bytes.size() - offset));
        bytes.resize(offset + static_cast<std::size_t>(stream.gcount()));
    }
    if (bytes.size() > k_max_tail_bytes) {
        return std::nullopt;
    }
    if (start > 0) {
        const auto newline = bytes.find('\n');
        if (newline == std::string::npos) {
            return std::nullopt;
        }
        bytes.erase(0, newline + 1);
    }
    std::vector<addon::Json_value> records;
    std::string_view remaining(bytes);
    std::size_t line_count = 0;
    for (;;) {
        if (++line_count > k_max_tail_records + 1) {
            return std::nullopt;
        }
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        if (!line.empty()) {
            if (line.size() > k_max_record_bytes) {
                return std::nullopt;
            }
            addon::Json_parse_error failure;
            auto record = addon::parse_json(line, {}, &failure);
            if (!record || !record->is_object()) {
                return std::nullopt;
            }
            records.push_back(std::move(*record));
        }
        if (end == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(end + 1);
    }
    std::optional<std::size_t> index;
    for (std::size_t current = 0; current < records.size(); ++current) {
        const auto& record = records[current];
        if (integer(record.find("step_index")) != completed_step) {
            continue;
        }
        const auto* calls = record.find("tool_calls");
        if (index || text(record, "type") != "PLANNER_RESPONSE" || !calls || !calls->is_array() ||
            calls->as_array().size() != 1 || text(calls->as_array().front(), "name") != "invoke_subagent")
        {
            return std::nullopt;
        }
        index = current;
    }
    if (!index || *index + 1 >= records.size()) {
        return std::nullopt;
    }
    const auto& record = records[*index + 1];
    if (integer(record.find("step_index")) != completed_step + 1 || text(record, "type") != "GENERIC") {
        return std::nullopt;
    }
    const auto content = text(record, "content");
    if (content.empty() || content.size() > k_max_record_bytes) {
        return std::nullopt;
    }
    const auto objects = embedded_objects(content);
    if (objects.size() != count) {
        return std::nullopt;
    }
    auto children = addon::Json_value::array();
    std::set<std::string> identities;
    for (const auto& object : objects) {
        const auto child = trimmed(text(object, "conversationId"));
        const auto uri   = trimmed(text(object, "logAbsoluteUri"));
        if (!bounded_identity(child) || !identities.insert(child).second || uri.size() > 2048 ||
            !valid_file_uri(uri) || uri.find(child) == std::string::npos)
        {
            return std::nullopt;
        }
        auto identity = addon::Json_value::object();
        identity.set("conversationId", addon::Json_value(child));
        children.push_back(std::move(identity));
    }
    return children;
}

} // namespace

std::optional<addon::Model_update> capture_model_observation(
    std::string_view name, const addon::Json_value& payload, std::string_view source_nonce)
{
    const auto invocation = integer(payload.find("invocationNum"));
    const auto steps = integer(payload.find("initialNumSteps"));
    constexpr auto counter_bound = std::numeric_limits<std::uint32_t>::max();
    if (name != "PreInvocation" || !invocation || !steps ||
        *invocation > counter_bound || *steps > counter_bound)
    {
        return std::nullopt;
    }
    addon::Model_update update;
    update.provider_session_id = trimmed(text(payload, "conversationId"));
    update.transition_nonce    = source_nonce;
    // Native counters order invocations within the trajectory; neither host
    // receipt order nor a nonce read after callback delivery grants authority.
    update.sequence = (static_cast<std::uint64_t>(*steps) << 32) |
        static_cast<std::uint64_t>(*invocation);
    update.status = addon::Model_status::CONFIRMED;
    update.model  = text(payload, "modelName");
    update.source = "agy.PreInvocation";
    return addon::is_valid_model_update(update) ? std::optional(std::move(update)) : std::nullopt;
}

addon::Json_value sanitize_hook(std::string_view name, const addon::Json_value& payload)
{
    auto result = addon::Json_value::object();
    if (name == "StatusLine") {
        const auto session = trimmed(text(payload, "conversation_id"));
        const auto state   = text(payload, "agent_state");
        const auto* pending = payload.find("tool_confirmation_pending");
        // The CLI omits the flag when no confirmation is showing. A partial
        // or malformed callback cannot establish that negative snapshot.
        if (!bounded_identity(session) || !bounded_identity(state) || (pending && !pending->is_boolean())) {
            return result;
        }
        result.set("conversationId", addon::Json_value(session));
        result.set("toolConfirmationPending", addon::Json_value(pending && pending->as_boolean()));
        return result;
    }
    result.set("conversationId", addon::Json_value(trimmed(text(payload, "conversationId"))));
    if (name == "Stop") {
        for (const auto* member : {"fullyIdle", "executionNum"}) {
            if (const auto* value = payload.find(member)) {
                result.set(member, *value);
            }
        }
    }
    if (name == "PreInvocation") {
        for (const auto* member : {"invocationNum", "initialNumSteps"}) {
            if (const auto* value = payload.find(member)) {
                result.set(member, *value);
            }
        }
    }
    if (name != "PostToolUse" || !invokes_subagent(payload)) {
        return result;
    }
    auto observation = addon::Json_value::object();
    observation.set("present", addon::Json_value(false));
    observation.set("version", addon::Json_value(1));
    const auto step  = integer(payload.find("stepIdx"));
    const auto count = expected_children(payload);
    if (step && count) {
        const auto children = subagent_results(payload, *step, *count);
        if (children) {
            observation.set("present", addon::Json_value(true));
            observation.set("stepIdx", addon::Json_value(*step));
            observation.set("children", *children);
        }
    }
    result.set(k_observation, std::move(observation));
    return result;
}

namespace {

bool transition_nonce(std::string_view nonce)
{
    return nonce.size() == 32 && nonce.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::optional<addon::Classify_event_request> active_hook(const addon::Classify_event_request& request)
{
    const auto& body = request.event.body;
    const auto* event = body.find("event");
    const auto* payload = event ? event->find("body") : nullptr;
    const auto name = event ? text(*event, "name") : std::string{};
    const auto nonce = text(body, "activation_nonce");
    if (!body.is_object() || body.as_object().size() != 2 || !event || !event->is_object() ||
        event->as_object().size() != 2 || !payload || !payload->is_object() ||
        (name != "PreInvocation" && name != "PostToolUse" && name != "Stop" && name != "StatusLine") ||
        !transition_nonce(nonce) || request.ledger.current_epoch == 0 || !request.ledger.current_transition ||
        request.ledger.current_transition->nonce != nonce ||
        request.ledger.current_transition->provider_session_id != text(*payload, "conversationId"))
    {
        return std::nullopt;
    }
    auto bound = request;
    bound.event.name = name;
    bound.event.body = *payload;
    return bound;
}

std::optional<addon::Classify_event_result> classify_selection(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    const auto& body = request.event.body;
    const auto selected = selected_conversation(text(body, "native_record"));
    const auto home = native_home(text(body, "native_home"));
    const auto identity = text(body, "conversationId");
    const auto previous = text(body, "previous_activation_nonce");
    const auto* previous_value = body.find("previous_activation_nonce");
    if (!body.is_object()) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "event.body", addon::Provider_event_problem::WRONG_TYPE, &body, {}, "selection_members"));
    }
    for (const auto* field : {"native_record", "native_home", "conversationId", "previous_activation_nonce"}) {
        const auto* value = body.find(field);
        if (!value || !value->is_string()) {
            return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
                request.event.name, field, !value ? addon::Provider_event_problem::MISSING
                    : addon::Provider_event_problem::WRONG_TYPE,
                value, {}, "selection_member"));
        }
    }
    if (body.as_object().size() != 4) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "event.body", addon::Provider_event_problem::INVALID_SHAPE, &body, 4,
            "selection_members"));
    }
    if (!selected || *selected != identity) {
        const auto* record = body.find("native_record");
        const auto* actual = !selected ? record : body.find("conversationId");
        const auto problem = !actual ? addon::Provider_event_problem::MISSING
            : !actual->is_string() ? addon::Provider_event_problem::WRONG_TYPE
            : !selected ? addon::Provider_event_problem::INVALID_VALUE : addon::Provider_event_problem::MISMATCH;
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, !selected ? "native_record" : "conversationId", problem, actual, {},
            "selected_conversation"));
    }
    if (!home) {
        const auto* actual = body.find("native_home");
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "native_home", !actual ? addon::Provider_event_problem::MISSING
                : !actual->is_string() ? addon::Provider_event_problem::WRONG_TYPE
                                      : addon::Provider_event_problem::INVALID_VALUE,
            actual, {}, "native_home"));
    }
    if (!previous_value || !previous_value->is_string() || (!previous.empty() && !transition_nonce(previous))) {
        return addon::reject_provider_event(failure, addon::make_provider_event_diagnostic(
            request.event.name, "previous_activation_nonce", !previous_value ? addon::Provider_event_problem::MISSING
                : !previous_value->is_string() ? addon::Provider_event_problem::WRONG_TYPE
                                              : addon::Provider_event_problem::INVALID_VALUE,
            previous_value, {}, "transition_nonce"));
    }
    addon::Classify_event_result result;
    const auto& current = request.ledger.current_transition;
    if ((current && (current->provider_session_id == identity || current->nonce != previous)) ||
        (!current && !previous.empty()))
    {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = request.event.name;
    observation.provider_session_id = identity;
    observation.session_transition = addon::Session_transition::STARTED;
    observation.resume_record = encode_resume_identity({identity, *home});
    result.classification = addon::Classification::TRANSITION;
    // This is an explicit native selection. Re-entering A after B is RESUME,
    // even if A was already loaded or its backing store later became unreadable.
    result.transition = current ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace

addon::Permission_snapshot_classification_result classify_permission_snapshot(
    const addon::Classify_event_request& request)
{
    addon::Permission_snapshot_classification_result result;
    const auto& name = request.event.name;
    if (name == "NativeHook" || (name == "StatusLine" && request.event.body.find("event"))) {
        const auto bound = active_hook(request);
        return bound ? classify_permission_snapshot(*bound) : result;
    }
    if (name == "StatusLine") {
        const auto session = trimmed(text(request.event.body, "conversationId"));
        const auto* pending = request.event.body.find("toolConfirmationPending");
        if (!request.event.body.is_object() || request.event.body.as_object().size() != 2 ||
            !bounded_identity(session) || !pending || !pending->is_boolean() ||
            request.ledger.current_epoch == 0 || !request.ledger.current_transition ||
            request.ledger.current_transition->provider_session_id != session)
        {
            return result;
        }
        // The callback can describe only the current native conversation. It
        // cannot create an epoch or claim a root turn from a display snapshot.
        addon::Lifecycle_observation observation;
        observation.provider_event_name = name;
        observation.provider_session_id = session;
        result.base.classification = addon::Classification::EVENT;
        result.base.observations.push_back(std::move(observation));
        result.permission_snapshot = addon::Permission_snapshot_update{1, 0, pending->as_boolean()};
    }
    return result;
}

std::optional<addon::Classify_event_result> classify_event(
    const addon::Classify_event_request& request, addon::Contract_failure* failure)
{
    addon::Classify_event_result result;
    const auto& name = request.event.name;
    if (name == "NativeConversation") {
        return classify_selection(request, failure);
    }
    if (name == "NativeHook") {
        const auto bound = active_hook(request);
        return bound ? classify_event(*bound, failure) : std::optional(result);
    }
    if (name != "PreInvocation" && name != "PostToolUse" && name != "Stop") {
        return result;
    }
    if (request.model_failure) {
        result.model_failure = request.model_failure;
    }
    else
    if (request.model_state_version == addon::k_model_state_version) {
        addon::decode_model_updates(request.event.body, result.model_updates, result.model_failure);
    }
    const auto session = trimmed(text(request.event.body, "conversationId"));
    if (!bounded_identity(session)) {
        const auto* value = request.event.body.find("conversationId");
        auto problem = addon::Provider_event_problem::MISSING;
        std::optional<std::uint64_t> units;
        if (value) {
            problem = addon::Provider_event_problem::WRONG_TYPE;
            if (value->is_string()) {
                problem = session.empty() ? addon::Provider_event_problem::EMPTY
                    : !addon::is_valid_utf8(session) ? addon::Provider_event_problem::INVALID_VALUE
                    : session.find('\0') != std::string::npos ? addon::Provider_event_problem::CONTROL_CHARACTER
                                                             : addon::Provider_event_problem::TOO_LARGE;
                if (problem == addon::Provider_event_problem::TOO_LARGE) {
                    units = 0;
                    for (unsigned char character : session) {
                        if ((character & 0xc0) != 0x80) {
                            *units += character >= 0xf0 ? 2 : 1;
                        }
                    }
                }
            }
        }
        auto diagnostic = addon::make_provider_event_diagnostic(
            name, "conversationId", problem, value, k_max_identity_units, "utf16_identity");
        if (units) {
            diagnostic.actual_size = units;
        }
        return addon::reject_provider_event(failure, std::move(diagnostic));
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = session;
    result.classification = addon::Classification::EVENT;
    if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != session) {
        result.classification = addon::Classification::TRANSITION;
        result.transition = addon::Transition_kind::START;
        observation.session_transition = addon::Session_transition::STARTED;
    }
    if (name == "PreInvocation") {
        observation.phase_transition = addon::Phase_transition::WORKING;
        observation.phase_evidence   = addon::Evidence::INFERRED;
        // agy names no turn, and invocationNum restarts at zero for every
        // execution of the conversation. The first invocation of an execution
        // starts the root turn, which the trajectory's step count at that start
        // identifies: steps normally accumulate, so it differs for later turns,
        // but a conversation rewound to an earlier step count repeats a retired
        // identity, which the host refuses as stale. Later invocations carry no
        // turn identity rather than one that could advance the turn again.
        const auto steps = integer(request.event.body.find("initialNumSteps"));
        if (integer(request.event.body.find("invocationNum")) == 0 && steps) {
            addon::Sha256 hash;
            hash.update("logonomic.agy.turn.v1\n" + std::to_string(session.size()) + '\n' +
                session + '\n' + std::to_string(*steps));
            observation.provider_turn_id = "agy-turn-" + addon::to_hex(hash.finish());
        }
    }
    const auto lose_subagents = [&result, &session](std::string reason) {
        addon::Subagent_evidence evidence;
        evidence.kind = addon::Subagent_evidence_kind::LOSS;
        evidence.provider_session_id = session;
        evidence.reason_code = std::move(reason);
        result.subagent_evidence = std::move(evidence);
    };
    if (name == "PostToolUse") {
        if (const auto* evidence = request.event.body.find(k_observation)) {
            const auto* present = evidence->find("present");
            const auto version = integer(evidence->find("version"));
            const bool tagged = evidence->is_object() && present && present->is_boolean() && version == 1;
            if (tagged && !present->as_boolean() && evidence->as_object().size() == 2) {
                lose_subagents("missing_subagent_observation");
            }
            else {
                const auto step = integer(evidence->find("stepIdx"));
                const auto* children = evidence->find("children");
                bool valid = tagged && present->as_boolean() && evidence->as_object().size() == 4 &&
                    step && children && children->is_array() && !children->as_array().empty() &&
                    children->as_array().size() <= k_max_children;
                std::vector<std::string> identities;
                std::set<std::string> distinct;
                if (valid) {
                    for (const auto& child : children->as_array()) {
                        const auto identity = trimmed(text(child, "conversationId"));
                        if (!child.is_object() || child.as_object().size() != 1 ||
                            !bounded_identity(identity) || identity == session || !distinct.insert(identity).second)
                        {
                            valid = false;
                            break;
                        }
                        identities.push_back(identity);
                    }
                }
                if (!valid) {
                    lose_subagents("invalid_subagent_observation");
                }
                else {
                    addon::Subagent_evidence batch;
                    batch.kind = addon::Subagent_evidence_kind::BATCH;
                    batch.provider_session_id = session;
                    batch.batch_id = "agy-step-" + std::to_string(*step);
                    batch.child_ids = identities;
                    result.subagent_evidence = std::move(batch);
                    // The first child can carry the root identity transition;
                    // the full 32-child hook stays one atomic classification.
                    for (const auto& identity : identities) {
                        auto child = observation;
                        child.agent_id = identity;
                        child.subagent_transition = addon::Subagent_transition::STARTED;
                        if (!result.observations.empty()) {
                            child.session_transition = addon::Session_transition::NONE;
                        }
                        result.observations.push_back(std::move(child));
                    }
                    return result;
                }
            }
        }
    }
    if (name == "Stop") {
        const auto* idle = request.event.body.find("fullyIdle");
        if (!idle || !idle->is_boolean()) {
            lose_subagents("invalid_fully_idle");
        }
        else
        if (idle->as_boolean()) {
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence   = addon::Evidence::INFERRED;
            const auto execution = integer(request.event.body.find("executionNum"));
            if (!execution) {
                lose_subagents("invalid_stop_execution");
            }
            else {
                addon::Sha256 hash;
                hash.update("logonomic.agy.Stop.v1\n" + std::to_string(session.size()) + '\n' +
                    session + '\n' + std::to_string(*execution));
                observation.provider_event_id = "agy-stop-" + addon::to_hex(hash.finish());
                addon::Subagent_evidence evidence;
                evidence.kind = addon::Subagent_evidence_kind::RECONCILE;
                evidence.provider_session_id = session;
                evidence.provider_event_id = observation.provider_event_id;
                result.subagent_evidence = std::move(evidence);
            }
        }
    }
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace agy
