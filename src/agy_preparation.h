#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

namespace agy {

logonomic::cli_addon::Json_value hook_definition(bool windows);
bool hooks_preinstalled(const logonomic::cli_addon::Prepare_request& request);
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
