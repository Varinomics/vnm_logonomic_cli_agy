#pragma once

#include <logonomic_cli_addon/sha256.h>

#include <filesystem>
#include <optional>

namespace agy {

// Device, file identity and change time distinguish restored or rewritten
// files even when size and last-write time are preserved.
std::optional<logonomic::cli_addon::Sha256_digest> native_file_change_identity(const std::filesystem::path& path);

} // namespace agy
