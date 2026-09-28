#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

namespace agy {

// delegated-session@3 `prepare`, and structured-session@3 `prepare` (refused:
// an Antigravity implementation run needs its Task servers) and `inspect`.
void handle_sessions(logonomic::cli_addon::Invocation_context& context);

// The stream-json inspection of a prepared delegated process.
logonomic::cli_addon::Structured_inspect_result inspect_session(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace agy
