#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/quota.h>

#include <optional>
#include <string_view>

namespace agy {

std::optional<logonomic::cli_addon::Quota_observation> parse_quota(
    std::string_view output,
    const logonomic::cli_addon::Observation_scope& scope);
void handle_quota(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
