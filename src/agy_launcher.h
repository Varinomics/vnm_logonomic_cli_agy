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
std::optional<Launcher_command> recognize_launcher(const std::vector<std::string>& argv);
std::optional<logonomic::cli_addon::Launcher_candidate> discover_launcher(
    const logonomic::cli_addon::Discover_request& request);
bool version_is_at_least(std::string_view version, int major, int minor, int patch);
std::string parse_version_output(std::string_view output, std::string_view errors);
std::optional<std::string> resolve_native_launcher(
    const Launcher_command& command,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory);

// A direct native invocation replaces the launcher's shell, so it keeps the
// launcher's assignments and options only when they are literal: nothing any
// supported runner expands, splits, globs or quotes.
bool literal_assignment(std::string_view value, bool windows);
// Options accept one or two leading dashes. True when every word is a literal
// option, each value-taking option carries a literal value, and no option is
// one of `refused`; a positional word such as a subcommand is refused too.
bool literal_options(
    const std::vector<std::string>&      arguments,
    bool                                 windows,
    const std::vector<std::string_view>& refused);
// True when literal options that passed literal_options set the switch `name`:
// the word alone, or name=value with a value agy's flag parser reads as true.
bool literal_switch(const std::vector<std::string>& arguments, std::string_view name);
// Windows updater descendants can open consoles outside the host terminal.
// Apply the native opt-out only to the environment of this managed invocation.
void disable_auto_updates(std::vector<std::pair<std::string, std::string>>& environment);
// The environment a direct native invocation of `command` receives: the frozen
// launcher environment whole, then the launcher's own assignments, each
// replacing a variable of the same name (ASCII case-insensitively on Windows),
// with native auto-updates disabled on Windows.
std::vector<std::pair<std::string, std::string>> direct_environment(
    const logonomic::cli_addon::Launch_environment& frozen,
    const Launcher_command&                         command);

// Evidence belongs to the resolved native image and effective environment.
// Cancelled/failed probes never enter the bounded installation cache.
std::optional<Verified_launch> verified_launch(
    const logonomic::cli_addon::Launcher_request& launcher,
    const logonomic::cli_addon::Json_value& effective_environment,
    std::string_view working_directory,
    logonomic::cli_addon::Invocation_context& context);

} // namespace agy
