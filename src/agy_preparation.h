#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

#include <optional>
#include <string>
#include <string_view>

namespace agy {

logonomic::cli_addon::Json_value hook_definition(bool windows);
std::string status_line_command(bool windows, std::string_view original_command);
std::optional<std::string> transform_status_line(
    bool present, std::string_view content, bool windows, std::string& reason);
// launch-preparation@3, interactive-resume@2, managed-configuration@3 and
// the permission-snapshot@1 settings transform.
void handle_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
