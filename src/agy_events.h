#pragma once

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/permission_snapshot.h>

#include <optional>
#include <string_view>

namespace agy {

// Transcript material is read only inside the helper. Only stable identities
// and bounded observation evidence leave that process.
logonomic::cli_addon::Json_value sanitize_hook(
    std::string_view name, const logonomic::cli_addon::Json_value& payload);

std::optional<logonomic::cli_addon::Classify_event_result> classify_event(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Contract_failure* failure = nullptr);

logonomic::cli_addon::Permission_snapshot_classification_result classify_permission_snapshot(
    const logonomic::cli_addon::Classify_event_request& request);

} // namespace agy
