#include "agy_sessions.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string running_state()
{
    return R"({"stage":"running","conversation_id":"session","pending_base64":"","result_base64":""})";
}

addon::Structured_inspect_result inspect(std::string state, std::string bytes, bool final = false,
    int exit_code = 0)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes   = std::move(bytes);
    request.final          = final;
    request.exit_code      = exit_code;
    return agy::inspect_session(request);
}

void continuation_preserves_large_result()
{
    const std::string answer(800 * 1024, 'x');
    const auto bytes = R"({"event":"result","result":{"conversation_id":"session","status":"SUCCESS","response":)" +
        addon::serialize_json(addon::Json_value(answer)) + "}}\n";
    const auto whole = inspect(running_state(), bytes, true);
    require(whole.complete && !whole.failure_reason && whole.result_bytes == answer,
        "whole native record must return the control answer");
    for (const auto split : std::array<std::size_t, 6>{1, 768 * 1024 - 1, 768 * 1024,
            768 * 1024 + 1, 800 * 1024, bytes.size()})
    {
        const auto first = inspect(running_state(), bytes.substr(0, split));
        require(!first.complete && !first.failure_reason &&
            addon::exchange_protocol_state_valid(first.protocol_state), "prefix must produce a valid continuation");
        const auto idle = inspect(first.protocol_state, "");
        require(!idle.complete && !idle.failure_reason, "empty inspection must preserve the accepted prefix or answer");
        const auto retained = inspect(idle.protocol_state, bytes.substr(split));
        require(!retained.complete && !retained.failure_reason, "the complete answer must remain pending until exit");
        const auto last = inspect(retained.protocol_state, "", true);
        require(last.complete && !last.failure_reason && last.result_bytes == whole.result_bytes,
            "chunk boundaries must not truncate or discard the answer");
        require(inspect(retained.protocol_state, "", true, 7).failure_reason == "process_failed",
            "a large retained answer must not override an unsuccessful exit");
    }
}

} // namespace

int main()
{
    try {
        continuation_preserves_large_result();
        std::cout << "agy_continuation_tests splits=6 failures=0\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "agy_continuation_tests: " << error.what() << '\n';
        return 1;
    }
}
