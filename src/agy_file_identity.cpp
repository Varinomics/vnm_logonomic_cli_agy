#include "agy_file_identity.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace agy {
namespace addon = logonomic::cli_addon;

std::optional<addon::Sha256_digest> native_file_change_identity(const std::filesystem::path& path)
{
    addon::Sha256 hash;
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO id{};
    const bool identified = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) && basic.ChangeTime.QuadPart != 0;
    CloseHandle(file);
    if (!identified) {
        return std::nullopt;
    }
    hash.update(&id.VolumeSerialNumber, sizeof(id.VolumeSerialNumber));
    hash.update(id.FileId.Identifier, sizeof(id.FileId.Identifier));
    hash.update(&basic.ChangeTime.QuadPart, sizeof(basic.ChangeTime.QuadPart));
#else
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
    }
    hash.update(&info.st_dev, sizeof(info.st_dev));
    hash.update(&info.st_ino, sizeof(info.st_ino));
#ifdef __APPLE__
    const auto changed = info.st_ctimespec;
#else
    const auto changed = info.st_ctim;
#endif
    hash.update(&changed.tv_sec, sizeof(changed.tv_sec));
    hash.update(&changed.tv_nsec, sizeof(changed.tv_nsec));
#endif
    return hash.finish();
}

} // namespace agy
