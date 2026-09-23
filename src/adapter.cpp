#include "agy_events.h"
#include "agy_launcher.h"
#include "agy_preparation.h"
#include "agy_quota.h"
#include "agy_usage_scan.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/legacy_state_import.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void discovery(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_discover_operation) {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        const auto candidate = agy::discover_launcher(*request);
        context.succeed(addon::encode_candidates(candidate ?
            std::vector<addon::Launcher_candidate>{*candidate} : std::vector<addon::Launcher_candidate>{}));
    }
    else
    if (context.request().operation == addon::k_recognize_operation) {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code);
            return;
        }
        addon::Launcher_recognition result;
        result.recognized = agy::recognize_launcher(request->launcher.script_body).has_value();
        if (result.recognized) {
            result.display_name = "Antigravity";
        }
        context.succeed(addon::encode_recognition(result));
    }
    else {
        context.reject_unknown_operation();
    }
}

void import_history(addon::Invocation_context& context)
{
    if (context.request().operation != "import_record") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_import_record_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (request->kind != addon::k_legacy_resume_record_kind) {
        context.fail(addon::Result_status::UNAVAILABLE, "unknown_record_kind");
        return;
    }
    const auto record = addon::decode_legacy_resume_record(request->record, &failure);
    if (!record) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    addon::Legacy_import_result result;
    if (record->provider_id == "agy") {
        auto converted = addon::Json_value::object();
        converted.set("provider_session_id", addon::Json_value(record->provider_session_id));
        result = *addon::Legacy_import_result::from_conversion(addon::encode_legacy_resume_conversion(converted));
    }
    context.succeed(addon::encode_import_record_result(result));
}

void lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_classify_event_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    const auto result = agy::classify_event(*request);
    if (!result) {
        context.fail(addon::Result_status::FAILED, "invalid_provider_event");
        return;
    }
    context.succeed(addon::encode_classify_event_result(*result));
}

void invoke(addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    try {
        const auto& capability = context.request().capability.capability_id;
        if (capability == "launcher-discovery") {
            discovery(context);
        }
        else
        if (capability == "launch-preparation" || capability == "interactive-resume") {
            agy::handle_preparation(context);
        }
        else
        if (capability == "quota") {
            agy::handle_quota(context);
        }
        else
        if (capability == "usage-scan") {
            agy::handle_usage_scan(context);
        }
        else
        if (capability == "helper-ingress") {
            lifecycle(context);
        }
        else
        if (capability == "legacy-state-import") {
            import_history(context);
        }
        else {
            context.reject_unknown_operation();
        }
    }
    catch (const std::exception&) {
        // User configuration and process streams must not enter diagnostics.
        context.fail(addon::Result_status::FAILED, "agy_operation_failed",
            "Antigravity integration could not preserve the supplied request.");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "agy-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {"quota", 2}, {"lifecycle-events", 1}, {"helper-ingress", 3},
        {"coordination-delivery", 1}, {"legacy-state-import", 1}, {"model-context-delivery", 1},
        {"interactive-resume", 2}, {"usage-scan", 3},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
