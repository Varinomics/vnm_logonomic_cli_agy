#include "agy_events.h"
#include "agy_launcher.h"
#include "agy_launcher_approval.h"
#include "agy_preparation.h"
#include "agy_quota.h"
#include "agy_sessions.h"
#include "agy_usage_scan.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/permission_snapshot.h>
#include <logonomic_cli_addon/profile_storage.h>
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
        result.recognized = agy::recognize_launcher(request->launcher.script_body, request->launcher.runner_key).has_value();
        if (result.recognized) {
            result.display_name = "Antigravity";
        }
        context.succeed(addon::encode_recognition(result));
    }
    else {
        context.reject_unknown_operation();
    }
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
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (context.request().capability.capability_id == addon::k_permission_snapshot_capability_id) {
        context.succeed(addon::encode_permission_snapshot_classification_result(
            agy::classify_permission_snapshot(*request)));
        return;
    }
    const auto result = agy::classify_event(*request, &failure);
    if (!result) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
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
        if (capability == addon::k_launcher_approval_capability_id) {
            addon::handle_launcher_approval(context, agy::launcher_approval_rules());
        }
        else
        if (capability == "launcher-discovery") {
            discovery(context);
        }
        else
        if (capability == addon::k_profile_storage_capability_id) {
            if (context.request().operation == addon::k_profile_storage_acknowledge) {
                addon::acknowledge_profile_storage(context);
            }
            else {
                context.reject_unknown_operation();
            }
        }
        else
        if (capability == "launch-preparation" || capability == "interactive-resume" ||
            capability == "managed-configuration" ||
            (capability == addon::k_permission_snapshot_capability_id &&
                context.request().operation == addon::k_managed_configuration_transform_operation))
        {
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
        if (capability == "helper-ingress" || capability == addon::k_permission_snapshot_capability_id) {
            lifecycle(context);
        }
        else
        if (capability == "structured-session" || capability == "delegated-session") {
            agy::handle_sessions(context);
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
        {addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {addon::k_profile_storage_capability_id, addon::k_profile_storage_version},
        {"managed-configuration", 3},
        {addon::k_permission_snapshot_capability_id, addon::k_permission_snapshot_version},
        {"quota", 2}, {"lifecycle-events", 2}, {"helper-ingress", 3},
        {"coordination-delivery", 1}, {"model-context-delivery", 1},
        {"model-context-delivery", addon::k_model_context_delivery_activation_version},
        {"interactive-resume", 2}, {"interactive-resume", 3}, {"interactive-resume", 4},
        {"usage-scan", 3}, {"structured-session", 3}, {"delegated-session", 3},
        {addon::k_delegated_model_selection_capability_id, addon::k_delegated_model_selection_version},
        {addon::k_usage_temporal_capability_id, addon::k_usage_temporal_version},
        {addon::k_quota_failure_capability_id, addon::k_quota_failure_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
