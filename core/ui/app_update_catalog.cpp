// Portions Copyright 2026 The Hollycast Authors
#include "app_update_package.h"
#include "oslib/http_client.h"
#include "json.hpp"
#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>

namespace app_updates::detail {
namespace {
using Json = nlohmann::json;
const std::string api = "https://api.github.com/repos/nexus382/Hollycast/";
const std::string downloadPrefix = "https://github.com/nexus382/Hollycast/releases/download/";
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string platform() {
#if defined(TARGET_UWP)
    return "UWP";
#elif defined(__ANDROID__)
    return "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__) && !defined(TARGET_IPHONE)
    return "macOS";
#elif defined(__SWITCH__)
    return "Switch";
#elif defined(__linux__)
    return "Linux";
#else
    return {};
#endif
}
bool compatible(const std::string& name) {
    if (name.find("libretro") != std::string::npos) return false;
#if defined(_WIN32) && !defined(TARGET_UWP)
#if defined(_M_ARM64) || defined(__aarch64__)
    return false; // There are no native Windows ARM64 CI packages.
#elif defined(_MSC_VER)
#if defined(_WIN64)
    return name.find("x86_64-pc-windows-msvc") != std::string::npos;
#else
    return name.find("i686-pc-windows-msvc") != std::string::npos;
#endif
#else
    return name.find("x86_64-w64-mingw32") != std::string::npos;
#endif
#else
    return true;
#endif
}
}

Check check(const std::string& channel, const std::string& current, bool switchTrack,
        const std::atomic<bool>& cancelled, const Fetch& transport) {
    require(channel == "dev" || channel == "master", "Unknown update channel.");
    require(std::regex_match(current, std::regex("[a-f0-9]{7,40}")), "This build has no usable source version.");
    const auto fetch = [&](const std::string& url) {
        require(!cancelled.load(), "Update check cancelled.");
        std::string text;
        if (transport) text = transport(url);
        else {
            http::init();
            std::vector<u8> bytes;
            const http::Headers headers{{"Accept", "application/vnd.github+json"},
                    {"User-Agent", "Hollycast-Updater"}};
            const int status = http::get(url, bytes, &headers, nullptr, 15000, &cancelled);
            if (!http::success(status))
                throw std::runtime_error("Update check failed (HTTP " + std::to_string(status) + "). Please retry later.");
            text.assign(bytes.begin(), bytes.end());
        }
        require(!cancelled.load(), "Update check cancelled.");
        require(text.size() <= 32 * 1024 * 1024, "Update response exceeds the metadata limit.");
        return Json::parse(text);
    };
    const Json catalog = fetch("https://nexus382.github.io/Hollycast/builds.json");
    require(catalog.value("repository", "") == "nexus382/Hollycast", "Unexpected update catalogue.");
    const auto& builds = catalog.at("builds");
    require(builds.is_array() && builds.size() <= 1000, "Invalid update catalogue.");
    Json latest;
    for (const auto& build : builds) {
        if (build.value("channel", "") != "ci" || build.value("branch", "") != channel) continue;
        require(std::regex_match(build.value("commit", ""), std::regex("[a-f0-9]{40}")), "Invalid release version.");
        // The publisher already groups releases by branch+commit and requires all six platforms.
        std::set<std::string> platforms;
        for (const auto& file : build.at("files")) if (!file.value("support", false)) platforms.insert(file.value("platform", ""));
        if (platforms.count("Windows") == 0 || platforms.count("Android") == 0
                || platforms.count("Linux") == 0 || platforms.count("macOS") == 0
                || platforms.count("Switch") == 0 || platforms.count("UWP") == 0) continue;
        if (latest.is_null() || build.at("date").get<std::string>() > latest.at("date").get<std::string>()) latest = build;
    }
    require(!latest.is_null(), "No complete build is available on this channel.");
    Check result;
    result.package.commit = latest.at("commit").get<std::string>();
    result.package.version = versionLabel(channel, result.package.commit);
    result.package.date = latest.at("date").get<std::string>();
    if (result.package.commit.compare(0, current.size(), current) == 0) return result;

    const std::string compare = api + "compare/" + current + "..." + result.package.commit;
    std::set<std::string> seen;
    size_t expected = 0;
    std::vector<std::string> notes;
    for (size_t page = 1; ; ++page) {
        const auto history = fetch(compare + "?per_page=100&page=" + std::to_string(page));
        if (page == 1) {
            const std::string relation = history.at("status").get<std::string>();
            if (relation == "identical") return result;
            result.olderTrack = relation == "behind" || relation == "diverged";
            if (relation == "behind" && !switchTrack) return result;
            expected = history.at("total_commits").get<size_t>();
            require(expected <= 10000, "The missed history is too large for an in-app update check.");
        }
        const auto& commits = history.at("commits");
        require(commits.is_array(), "Invalid update history.");
        for (const auto& commit : commits) {
            if (!seen.insert(commit.at("sha").get<std::string>()).second) continue;
            const auto& data = commit.at("commit");
            const auto message = data.at("message").get<std::string>();
            notes.push_back(data.at("committer").value("date", "") + "\n" + message + "\n\n");
        }
        if (seen.size() >= expected) break;
        require(!commits.empty() && page < 100, "GitHub returned incomplete update history. Please retry later.");
    }
    // GitHub paginates oldest first; reverse only after collecting every page.
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) result.changelog += *it;
    if (result.changelog.empty()) result.changelog = latest.value("notes", "No additional release notes.");
    result.available = true;
#if defined(TARGET_IPHONE) || defined(TARGET_UWP) || defined(__SWITCH__)
    // Release information is still useful on hosts whose system owns installation.
    result.installMessage = "Install this update through your system or homebrew installer.";
    return result;
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(__x86_64__)
    result.installMessage = "No matching Linux CI package is published for this architecture. Use your package manager.";
    return result;
#endif
    std::string fileUrl;
    for (const auto& file : latest.at("files"))
        if (file.value("platform", "") == platform() && !file.value("support", false)) fileUrl = file.value("url", "");
    require(fileUrl.rfind(downloadPrefix, 0) == 0, "No official update package is available for this platform.");
    const size_t slash = fileUrl.find('/', downloadPrefix.size());
    require(slash != std::string::npos, "Invalid release download URL.");
    const std::string tag = fileUrl.substr(downloadPrefix.size(), slash - downloadPrefix.size());
    require(std::regex_match(tag, std::regex("[A-Za-z0-9._-]+")), "Invalid release tag.");
    const auto release = fetch(api + "releases/tags/" + tag);
    require(!release.value("draft", true), "The selected update is not published.");
    const std::string selectedName = fileUrl.substr(slash + 1);
    for (const auto& asset : release.at("assets")) {
        const auto name = asset.value("name", "");
        const bool selected = platform() == "Windows"
            ? compatible(name) && name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0
            : name == selectedName;
        if (!selected || asset.value("state", "") != "uploaded") continue;
        const auto digest = asset.contains("digest") && asset.at("digest").is_string()
            ? asset.at("digest").get<std::string>() : std::string();
        require(std::regex_match(digest, std::regex("sha256:[a-f0-9]{64}")), "This release has no verified package checksum.");
        result.package.url = asset.at("browser_download_url").get<std::string>();
        require(result.package.url.rfind(downloadPrefix + tag + "/", 0) == 0, "Unexpected package download location.");
        result.package.sha256 = digest.substr(7);
        result.package.size = asset.at("size").get<uint64_t>();
        require(result.package.size > 0 && result.package.size <= 512ULL * 1024 * 1024, "Update package exceeds the size limit.");
        break;
    }
    require(!result.package.url.empty(), "No matching package is published for this build's architecture.");
    return result;
}
Check playNotes(int installedCode, int availableCode, const std::atomic<bool>& cancelled) {
    Check result;
    result.available = true;
    result.package.version = "Google Play (" + std::to_string(availableCode) + ")";
    result.package.date = "Not provided by Google Play";
    result.changelog = "Release notes for this Play version are not published yet. You can view What's new on its Google Play page.";
    try {
        require(!cancelled.load(), "Update check cancelled.");
        http::init();
        std::vector<u8> bytes;
        const int status = http::get("https://raw.githubusercontent.com/nexus382/Hollycast/master/resources/update/play-releases.json",
            bytes, nullptr, nullptr, 15000, &cancelled);
        require(http::success(status) && bytes.size() <= 1024 * 1024, "Play release notes are unavailable.");
        require(!cancelled.load(), "Update check cancelled.");
        const auto releases = Json::parse(std::string(bytes.begin(), bytes.end())).at("releases");
        require(releases.is_array() && releases.size() <= 1000, "Invalid Play release notes.");
        std::map<int, Json> missed;
        for (const auto& release : releases) {
            const int code = release.at("versionCode").get<int>();
            if (code > installedCode && code <= availableCode) missed.emplace(code, release);
        }
        // Never substitute the latest CI build when Play hasn't published its metadata.
        if (missed.count(availableCode) == 0) return result;
        const auto& target = missed.at(availableCode);
        result.package.version = target.at("versionName").get<std::string>() + " (" + std::to_string(availableCode) + ")";
        result.package.date = target.at("date").get<std::string>();
        result.changelog.clear();
        // Stable and testing releases share version codes. Include only the eligible target's
        // track plus stable releases so a production update never advertises testing features.
        for (auto it = missed.rbegin(); it != missed.rend(); ++it) {
            const auto& release = it->second;
            const auto channel = release.at("channel").get<std::string>();
            if (channel != "master" && channel != target.at("channel").get<std::string>()) continue;
            result.changelog += release.at("versionName").get<std::string>() + " - "
                + release.at("date").get<std::string>() + "\n" + release.at("notes").get<std::string>() + "\n\n";
        }
    } catch (const std::exception&) {
        // Play installation remains available when our optional release-note feed is offline.
        result.changelog = "Release notes could not be loaded. You can still update through Google Play.";
    }
    return result;
}

}
