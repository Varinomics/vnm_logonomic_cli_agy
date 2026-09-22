#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

#include <optional>

namespace agy {

logonomic::cli_addon::Json_value hook_definition(bool windows);
bool hooks_preinstalled(
    const logonomic::cli_addon::Launcher_request&                  launcher,
    const std::optional<logonomic::cli_addon::Launch_environment>& launch_environment);
// launch-preparation@3 `prepare` and interactive-resume@2 `prepare_resume`.
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
