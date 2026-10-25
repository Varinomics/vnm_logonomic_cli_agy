#include "agy_launcher_approval.h"

namespace agy {

const logonomic::cli_addon::Launcher_approval_rules& launcher_approval_rules()
{
    static const logonomic::cli_addon::Launcher_approval_rules rules{
        .program_names = {"agy", "agy.exe", "agy.cmd", "agy.bat", "agy.ps1"},
        .canonical_argument = "--dangerously-skip-permissions",
        .bypass_arguments = {"--dangerously-skip-permissions"},
        .approval_modes = {},
        .conflicting_arguments = {},
        .value_arguments = {
            "--mode", "--model", "--effort", "--agent", "--add-dir", "--conversation",
            "-i", "--prompt-interactive", "-p", "--print", "--prompt", "--output-format",
            "--input-format", "--json-schema", "--print-timeout", "--project", "--log-file",
        },
        .optional_value_arguments = {},
        .flag_arguments = {
            "-c", "--continue", "--disable-slash-commands", "--sandbox", "--remote-control",
            "--new-project",
        },
        .session_commands = {},
        .unsupported_commands = {
            "auth", "login", "logout", "models", "agents", "config", "settings", "update",
            "upgrade", "version", "help", "completion", "mcp",
        },
    };
    return rules;
}

} // namespace agy
