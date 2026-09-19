#pragma once

#include <logonomic_cli_addon/helper_ingress.h>

#include <optional>
#include <string_view>

namespace agy {

// Transcript material is read only inside the helper. Only stable identities
// and bounded observation evidence leave that process.
logonomic::cli_addon::Json_value sanitize_hook(
    std::string_view name, const logonomic::cli_addon::Json_value& payload);

std::optional<logonomic::cli_addon::Classify_event_result> classify_event(
    const logonomic::cli_addon::Classify_event_request& request);

} // namespace agy
