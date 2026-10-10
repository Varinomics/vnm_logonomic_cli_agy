#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

namespace agy {

logonomic::cli_addon::Json_value hook_definition(bool windows);
// launch-preparation@3, interactive-resume@2, and managed-configuration@3.
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
