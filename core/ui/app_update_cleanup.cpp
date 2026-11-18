// Portions Copyright 2026 The Hollycast Authors
#include "app_update_package.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>

namespace app_updates::detail {
void cleanupDownloads(const std::string& root, const std::string& pending) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto base = fs::u8path(root);
    if (fs::symlink_status(base, error).type() != fs::file_type::directory) return;
    const std::regex name("update-[0-9]+-[0-9]+");
    fs::directory_iterator folders(base, error), end;
    while (!error && folders != end) {
        const auto folder = *folders;
        folders.increment(error);
        std::error_code ignored;
        if (!std::regex_match(folder.path().filename().u8string(), name)
                || folder.symlink_status(ignored).type() != fs::file_type::directory) continue;
        if (!pending.empty() && (folder.path().lexically_normal() == fs::u8path(pending).lexically_normal()
                || fs::equivalent(folder.path(), fs::u8path(pending), ignored))) continue;
        if (fs::symlink_status(folder.path() / "status", ignored).type() != fs::file_type::regular) continue;
        std::ifstream status(folder.path() / "status", std::ios::binary);
        std::array<char, 10> text{};
        status.read(text.data(), text.size());
        const std::string value(text.data(), static_cast<size_t>(status.gcount()));
        status.close();
        if (value.rfind("Done", 0) != 0 && value.rfind("Error\n", 0) != 0 && value.rfind("Cancelled", 0) != 0) continue;
        bool removed = true;
        fs::directory_iterator files(folder.path(), ignored);
        while (!ignored && files != end) {
            const auto path = files->path();
            files.increment(ignored);
            if (path.filename() == "status") continue;
            // Work folders are flat. Unknown files, directories and reparse points
            // are retained, even if someone gives them an updater-shaped name.
            const std::string filename = path.filename().u8string();
            const bool managed = filename == "package.zip" || filename == "HollycastUpdater.exe"
                || filename == "fresh-updater.exe" || filename == "manifest.json" || filename == "install.sh"
                || filename == "cancel" || filename == "status.tmp"
                || std::regex_match(filename, std::regex("(inner-[0-2]\\.zip|entries-[0-2])"));
            if (!managed || fs::symlink_status(path, ignored).type() != fs::file_type::regular) {
                removed = false;
                continue;
            }
            std::error_code failed;
            fs::remove(path, failed);
            removed = removed && !failed;
        }
        // A Windows helper can still have its private executable open. Keep its
        // terminal marker so a later launch can reclaim the remaining files.
        if (removed && !ignored) {
            fs::remove(folder.path() / "status", ignored);
            fs::remove(folder.path(), ignored);
        }
    }
}
}
