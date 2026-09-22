#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agy {

struct Launcher_command
{
    std::string                                      program;
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::string>                         arguments;   // unquoted words after the program
    bool                                             sole_command = false;   // nothing else in the script runs
};

struct Verified_launch
{
    std::string program;
    std::string version;
};

std::optional<Launcher_command> recognize_launcher(std::string_view script);
std::optional<logonomic::cli_addon::Launcher_candidate> discover_launcher(
    const logonomic::cli_addon::Discover_request& request);
bool version_is_at_least(std::string_view version, int major, int minor, int patch);
std::string parse_version_output(std::string_view output, std::string_view errors);
std::optional<std::string> resolve_native_launcher(
    const Launcher_command& command,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory);

// Evidence belongs to the resolved native image and effective environment.
// Cancelled/failed probes never enter the bounded installation cache.
std::optional<Verified_launch> verified_launch(
    const logonomic::cli_addon::Launcher_request& launcher,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory,
    logonomic::cli_addon::Invocation_context& context);

} // namespace agy
