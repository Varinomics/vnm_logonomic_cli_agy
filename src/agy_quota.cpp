#include "agy_quota.h"
#include "agy_launcher.h"

#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace agy {
namespace addon = logonomic::cli_addon;
namespace {

std::string text(const addon::Json_value& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_string() ? value->as_string() : std::string{};
}

std::optional<std::uint64_t> reset_time(std::string_view value)
{
    // ISO 8601 UTC/offset dates become UTC milliseconds; invalid optional
    // metadata is omitted, as the native provider omitted invalid reset text.
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':')
    {
        return std::nullopt;
    }
    const auto number = [&value](std::size_t position, std::size_t count) -> std::optional<int> {
        int parsed = 0;
        const auto first = value.data() + position;
        const auto last  = first + count;
        if (!std::all_of(first, last, [](char digit) { return digit >= '0' && digit <= '9'; })) {
            return std::nullopt;
        }
        const auto result = std::from_chars(first, last, parsed);
        return result.ec == std::errc{} && result.ptr == last ? std::optional(parsed) : std::nullopt;
    };
    const auto year   = number(0, 4);
    const auto month  = number(5, 2);
    const auto day    = number(8, 2);
    const auto hour   = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        *hour > 23 || *minute > 59 || *second > 59)
    {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{
        std::chrono::year(*year), std::chrono::month(*month), std::chrono::day(*day)};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::size_t offset = 19;
    int millis = 0;
    if (offset < value.size() && value[offset] == '.') {
        const auto start = ++offset;
        int multiplier = 100;
        while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
            millis += (value[offset++] - '0') * multiplier;
            multiplier /= 10;
        }
        if (offset == start) {
            return std::nullopt;
        }
    }
    int offset_minutes = 0;
    if (offset + 1 == value.size() && (value[offset] == 'Z' || value[offset] == 'z')) {
        ++offset;
    }
    else
    if (offset + 6 == value.size() && (value[offset] == '+' || value[offset] == '-') &&
        value[offset + 3] == ':')
    {
        const auto hours   = number(offset + 1, 2);
        const auto minutes = number(offset + 4, 2);
        if (!hours || !minutes || *hours > 23 || *minutes > 59) {
            return std::nullopt;
        }
        offset_minutes = (*hours * 60 + *minutes) * (value[offset] == '+' ? 1 : -1);
        offset += 6;
    }
    if (offset != value.size()) {
        return std::nullopt;
    }
    const auto instant = std::chrono::sys_days(date) + std::chrono::hours(*hour) +
        std::chrono::minutes(*minute - offset_minutes) + std::chrono::seconds(*second) +
        std::chrono::milliseconds(millis);
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
    return count >= 0 ? std::optional(static_cast<std::uint64_t>(count)) : std::nullopt;
}

std::optional<addon::Quota_window> group_window(const addon::Json_value& group)
{
    auto name = text(group, "name");
    const auto start = name.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    name = name.substr(start, name.find_last_not_of(" \t\r\n") - start + 1);
    const auto* buckets = group.find("buckets");
    if (!buckets || !buckets->is_array() || buckets->as_array().size() != 1) {
        return std::nullopt;
    }
    const auto& bucket = buckets->as_array().front();
    const auto* remaining = bucket.find("remaining_fraction");
    if (!remaining || !remaining->is_number() || !std::isfinite(remaining->as_number()) ||
        remaining->as_number() < 0 || remaining->as_number() > 1)
    {
        return std::nullopt;
    }
    addon::Quota_window result;
    result.used_percent = (1.0 - remaining->as_number()) * 100.0;
    result.label        = std::move(name);
    result.resets_at_ms  = reset_time(text(bucket, "reset_time"));
    if (text(bucket, "window") == "weekly") {
        result.window_minutes = 7 * 24 * 60;
    }
    return result;
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_probe_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto command = recognize_launcher(request->context.launcher.script_body);
    if (!command) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_unrecognized");
        return;
    }
    const auto program = resolve_native_launcher(*command,
        request->context.launch_environment->effective_environment, request->context.working_directory);
    if (!program) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_program_unavailable");
        return;
    }
    addon::Structured_prepare_result result;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
#ifdef _WIN32
    result.process.executable = "helpers/agy_launch_bridge.exe";
#else
    result.process.executable = "helpers/agy_launch_bridge";
#endif
    result.process.argv = {*program, "-p", "/usage", "--output-format", "json"};
    // The frozen launcher environment is copied whole, so no name is also
    // listed for pass-through: the host refuses a name supplied both ways.
    result.process.environment = direct_environment(*request->context.launch_environment, *command);
    result.inspection.protocol_state = "await_exit";
    context.succeed(addon::encode_structured_prepare_result(result));
}

void inspect(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_probe_inspect_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    addon::Quota_inspect_result result;
    result.complete       = request->inspection.final || request->inspection.timed_out;
    result.protocol_state = request->inspection.protocol_state;
    if (!result.complete) {
        context.succeed(addon::encode_quota_inspect_result(result, 2));
        return;
    }
    addon::Observation_scope scope;
    if (request->account_context) {
        scope.source_account_key = request->account_context->adapter_account_key;
    }
    scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (!request->inspection.timed_out && request->inspection.exit_code == 0) {
        result.observation = parse_quota(request->inspection.stdout_bytes, scope);
    }
    if (!result.observation) {
        auto reason = addon::Quota_failure::INVALID_RESPONSE;
        std::optional<addon::Quota_diagnostic> diagnostic;
        if (request->inspection.timed_out) {
            reason = addon::Quota_failure::TIMED_OUT;
        }
        else
        if (request->inspection.exit_code != 0) {
            reason = addon::Quota_failure::PROCESS_FAILED;
            diagnostic = addon::Quota_diagnostic{
                addon::Quota_failure_stage::PROCESS_EXIT, request->inspection.exit_code};
        }
        result = addon::make_quota_failure(reason, scope, diagnostic);
    }
    context.succeed(addon::encode_quota_inspect_result(result, 2, request->report_quota_failure));
}

} // namespace

std::optional<addon::Quota_observation> parse_quota(
    std::string_view output, const addon::Observation_scope& scope)
{
    addon::Json_parse_error failure;
    const auto value = addon::parse_json(output, {}, &failure);
    if (!value || !value->is_object() || text(*value, "status") != "SUCCESS") {
        return std::nullopt;
    }
    const auto* command = value->find("command");
    if (!command || text(*command, "name") != "usage") {
        return std::nullopt;
    }
    const auto* data   = command->find("data");
    const auto* groups = data ? data->find("groups") : nullptr;
    if (!groups || !groups->is_array() || groups->as_array().size() != 2) {
        return std::nullopt;
    }
    addon::Quota_observation result;
    result.scope     = scope;
    result.state     = addon::Quota_status::READY;
    result.primary   = group_window(groups->as_array()[0]);
    result.secondary = group_window(groups->as_array()[1]);
    if (!result.primary || !result.secondary) {
        return std::nullopt;
    }
    // Validate normalized labels and metadata before publishing a ready result.
    addon::Quota_inspect_result inspection;
    inspection.complete    = true;
    inspection.observation = result;
    addon::Contract_failure contract_failure;
    if (!addon::decode_quota_inspect_result(addon::encode_quota_inspect_result(inspection), true, &contract_failure)) {
        return std::nullopt;
    }
    return result;
}

void handle_quota(addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
    }
    else
    if (context.request().operation == addon::k_prepare_operation) {
        prepare(context);
    }
    else
    if (context.request().operation == addon::k_inspect_operation) {
        inspect(context);
    }
    else {
        context.reject_unknown_operation();
    }
}

} // namespace agy
