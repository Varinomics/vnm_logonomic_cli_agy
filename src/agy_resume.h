#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace agy {

struct Resume_identity
{
    std::string provider_session_id;
    std::optional<std::string> native_home;
};

std::optional<Resume_identity> decode_resume_identity(const logonomic::cli_addon::Json_value& record);
logonomic::cli_addon::Json_value encode_resume_identity(const Resume_identity& identity);
std::optional<std::string> native_home(std::string_view value);
bool resume_scope_matches(
    const Resume_identity& identity,
    const logonomic::cli_addon::Session_launch_context& context);

// Reads only the exact native conversation in the request's resolved scope.
// Preparation uses the same predicate afresh before proposing a process.
logonomic::cli_addon::Resume_inspect_result inspect_resume(
    const Resume_identity& identity,
    const logonomic::cli_addon::Session_launch_context& context,
    const std::function<bool()>& cancelled);

} // namespace agy
