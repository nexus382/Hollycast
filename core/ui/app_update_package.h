// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace app_updates::detail {
// Match the public catalogue's branch/commit label without changing package versions.
inline std::string versionLabel(const std::string& channel, const std::string& commit) {
    return std::string(channel == "master" ? "Master" : "Dev") + " \xC2\xB7 " + commit.substr(0, 9);
}
struct Package {
    std::string version, commit, date, url, sha256;
    uint64_t size = 0;
};
struct Check {
    Package package;
    std::string changelog;
    bool available = false;
    bool olderTrack = false;
    std::string installMessage;
};
using Fetch = std::function<std::string(const std::string&)>;
// Fixed official catalogue/releases; the transport seam also exercises paginated history offline.
Check check(const std::string& channel, const std::string& current, bool switchTrack,
        const std::atomic<bool>& cancelled, const Fetch& fetch = {});
// Play supplies the eligible version code; published notes are bounded to that exact code.
Check playNotes(int installedCode, int availableCode, const std::atomic<bool>& cancelled);
// Starts a verified-package helper. Only the UI may request application shutdown after Ready.
std::string install(const Package& package);
// Remove only terminal updater-owned work folders; active helpers retain their files.
void cleanupDownloads(const std::string& root, const std::string& pending = {});
}
