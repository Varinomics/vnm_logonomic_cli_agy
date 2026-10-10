#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <string_view>

namespace agy {

enum class Native_usage_decode_status
{
    NO_USAGE,
    DECODED,
    INVALID,
};

struct Native_usage_decode_result
{
    Native_usage_decode_status status = Native_usage_decode_status::NO_USAGE;
    std::optional<logonomic::cli_addon::Usage_record> record;
    std::optional<std::string> instant;
};

Native_usage_decode_result decode_native_usage_step(
    std::string_view metadata,
    std::string_view cascade_id,
    std::uint64_t step_index);

std::optional<std::vector<logonomic::cli_addon::Usage_record>> scan_native_usage_directory(
    const std::filesystem::path& conversations,
    const std::function<bool()>& is_cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& report_progress,
    std::string* out_error);

void handle_usage_scan(logonomic::cli_addon::Invocation_context& context);

} // namespace agy
