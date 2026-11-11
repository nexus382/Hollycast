// Portions Copyright 2026 The Hollycast Authors
#include "ui/app_update_package.h"
#include "oslib/http_client.h"
#include "json.hpp"
#include <iostream>
#include <stdexcept>
using Json = nlohmann::json;
using namespace app_updates::detail;
namespace {
std::string httpBody;
int httpStatus = 200;
int count = 0;
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++count;
}
const std::string oldSha(40, 'a'), newSha(40, 'b');
#if defined(_WIN32) && defined(_MSC_VER) && defined(_WIN64)
const std::string assetName = "Hollycast-x86_64-pc-windows-msvc.zip";
#elif defined(_WIN32) && defined(_MSC_VER)
const std::string assetName = "Hollycast-i686-pc-windows-msvc.zip";
#elif defined(_WIN32)
const std::string assetName = "Hollycast-x86_64-w64-mingw32.zip";
#else
const std::string assetName = "Hollycast-portable.zip";
#endif
const std::string url = "https://github.com/nexus382/Hollycast/releases/download/ci-dev-123/";
Json catalogue, release;
std::string relation;
int total;
bool incomplete;
void reset() {
    relation = "ahead"; total = 1; incomplete = false;
    Json files = Json::array();
    for (const auto* platform : {"Windows", "Android", "Linux", "macOS", "Switch", "UWP"})
        files.push_back({{"platform", platform}, {"url", url + assetName}});
    catalogue = {{"repository", "nexus382/Hollycast"}, {"builds", Json::array({{
        {"channel", "ci"}, {"branch", "dev"}, {"commit", newSha}, {"date", "2026-11-11T00:00:00Z"},
        {"notes", "Track notes"}, {"files", files}}})}};
    release = {{"draft", false}, {"assets", Json::array({{{"name", assetName},
        {"state", "uploaded"}, {"browser_download_url", url + assetName},
        {"digest", "sha256:" + std::string(64, 'c')}, {"size", 42}}})}};
}
std::string fetch(const std::string& address) {
    if (address.find("builds.json") != std::string::npos) return catalogue.dump();
    if (address.find("releases/tags") != std::string::npos) return release.dump();
    const int page = address.find("page=2") == std::string::npos ? 1 : 2;
    Json commits = Json::array();
    if (!incomplete) for (int i = (page - 1) * 100; i < total && i < page * 100; ++i)
        commits.push_back({{"sha", std::to_string(i)}, {"commit", {{"message", "change-" + std::to_string(i)},
            {"committer", {{"date", "2026-11-11"}}}}}});
    return Json{{"status", relation}, {"total_commits", total}, {"commits", commits}}.dump();
}
void fails(const std::function<void()>& action, const char* message) {
    try { action(); } catch (const std::exception&) { ++count; return; }
    throw std::runtime_error(message);
}
}
namespace http {
void init() {}
int get(const std::string&, std::vector<u8>& bytes, const Headers*, Headers*, int) {
    bytes.assign(httpBody.begin(), httpBody.end()); return httpStatus;
}
}
int main() {
    try {
        std::atomic<bool> cancelled{false};
        auto run = [&](bool switched = false) { return check("dev", oldSha, switched, cancelled, fetch); };
        Check result;
        // Other standalone hosts have no published CI asset. They still exercise Play
        // metadata below; positive package selection applies to supported CI platforms.
#if (defined(_WIN32) && !defined(_M_ARM64) && !defined(__aarch64__)) \
    || (defined(__linux__) && defined(__x86_64__)) || defined(__ANDROID__) || defined(__APPLE__) || defined(__SWITCH__)
        reset(); result = run(); expect(result.available && result.package.size == 42, "New matching build missing");
        expect(!check("dev", newSha, false, cancelled, fetch).available, "Current build offered again");
        total = 101; result = run();
        expect(result.changelog.find("change-100") < result.changelog.find("change-99"), "History pagination order wrong");
        expect(result.changelog.find("change-0\n") != std::string::npos, "Oldest missed change missing");
        reset(); relation = "behind"; total = 0;
        expect(!run().available, "Automatic downgrade offered"); expect(run(true).available, "Explicit track switch blocked");
        reset(); catalogue["builds"][0]["files"].erase(5); fails([&] { run(); }, "Incomplete platform set accepted");
        reset(); release["assets"][0]["digest"] = nullptr; fails([&] { run(); }, "Missing checksum accepted");
        reset(); release["assets"][0]["browser_download_url"] = "https://example.com/file.zip"; fails([&] { run(); }, "Foreign asset accepted");
        reset(); release["assets"][0]["size"] = 1024ULL*1024*1024; fails([&] { run(); }, "Oversize package accepted");
        reset(); incomplete = true; fails([&] { run(); }, "Incomplete changelog accepted");
        reset(); cancelled = true; fails([&] { run(); }, "Cancelled check continued"); cancelled = false;
        reset(); catalogue["repository"] = "someone/else"; fails([&] { run(); }, "Foreign catalogue accepted");
#endif
        httpBody = Json{{"releases", Json::array({
            {{"versionCode", 11}, {"versionName", "stable-11"}, {"date", "2026-11-01"}, {"channel", "master"}, {"notes", "stable change"}},
            {{"versionCode", 12}, {"versionName", "dev-12"}, {"date", "2026-11-02"}, {"channel", "dev"}, {"notes", "testing-only"}},
            {{"versionCode", 13}, {"versionName", "stable-13"}, {"date", "2026-11-03"}, {"channel", "master"}, {"notes", "latest stable"}},
            {{"versionCode", 14}, {"versionName", "dev-14"}, {"date", "2026-11-04"}, {"channel", "dev"}, {"notes", "future-dev"}}
        })}}.dump();
        result = playNotes(10, 13, cancelled);
        expect(result.changelog.find("stable change") != std::string::npos, "Combined Play notes missing");
        expect(result.changelog.find("testing-only") == std::string::npos && result.changelog.find("future-dev") == std::string::npos,
            "Play showed ineligible CI/testing changes");
        result = playNotes(10, 14, cancelled); expect(result.changelog.find("testing-only") != std::string::npos, "Eligible testing notes missing");
        result = playNotes(10, 15, cancelled); expect(result.package.version == "Google Play (15)", "Unpublished Play metadata substituted");
        httpStatus = 503; expect(playNotes(10, 13, cancelled).available, "Offline notes blocked Play installation");
        std::cout << count << " updater catalogue assertions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
