// Portions Copyright 2026 The Hollycast Authors
#include "app_update_package.h"
#include "oslib/oslib.h"
#include "oslib/resources.h"
#include "stdclass.h"
#include "json.hpp"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <stdexcept>
#include <vector>
#if defined(_WIN32) && !defined(TARGET_UWP)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) && !defined(TARGET_IPHONE)
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/wait.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#include <sys/wait.h>
#endif
#if defined(__ANDROID__)
extern std::string getFilesPath();
extern void installAndroidUpdate(const std::string&, const std::string&, const std::string&, uint64_t);
#endif

namespace app_updates::detail {
namespace {
namespace fs = std::filesystem;
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__ANDROID__) && !defined(TARGET_IPHONE)
void writeResource(const std::string& key, const fs::path& path) {
    size_t size = 0;
    auto bytes = resource::load(key, size);
    if (!bytes || size == 0) throw std::runtime_error("The update helper is missing from this build.");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.get()), size);
    out.close();
    if (!out) throw std::runtime_error("Cannot write the update helper.");
}
#endif
#if defined(__APPLE__) && !defined(TARGET_IPHONE)
bool inside(const fs::path& child, const fs::path& parent) {
    const auto relative = fs::weakly_canonical(child).lexically_relative(fs::canonical(parent));
    return !relative.empty() && *relative.begin() != "..";
}
#endif
}

std::string install(const Package& package) {
    const std::string prefix = "https://github.com/nexus382/Hollycast/releases/download/";
    if (package.url.rfind(prefix, 0) != 0 || package.url.find_first_of("\r\n\t ") != std::string::npos
            || !std::regex_match(package.sha256, std::regex("[a-f0-9]{64}"))
            || package.size == 0 || package.size > 512ULL * 1024 * 1024)
        throw std::runtime_error("The selected update has invalid package metadata.");
    fs::path target;
    std::string kind;
#if defined(_WIN32) && !defined(TARGET_UWP)
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) throw std::runtime_error("Cannot locate the running application.");
    executable.resize(length);
    target = fs::path(executable);
    kind = "windows";
#elif defined(__ANDROID__)
    kind = "android";
#elif defined(__APPLE__) && !defined(TARGET_IPHONE)
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::string executable(length, '\0');
    if (_NSGetExecutablePath(executable.data(), &length) != 0) throw std::runtime_error("Cannot locate the running application.");
    target = fs::canonical(fs::u8path(executable.c_str()));
    while (!target.empty() && target.extension() != ".app") {
        const auto parent = target.parent_path();
        if (parent == target) break;
        target = parent;
    }
    if (target.extension() != ".app") throw std::runtime_error("Automatic updates require a packaged Hollycast application.");
    // User profiles must stay outside the signed application that is replaced as a whole.
    if (inside(fs::u8path(get_writable_data_path("")), target)
            || inside(fs::u8path(get_writable_config_path("")), target))
        throw std::runtime_error("Move your Hollycast profile outside the application bundle before updating.");
    kind = "macos";
#elif defined(__linux__)
    const char* image = std::getenv("APPIMAGE");
    if (!image || !*image) throw std::runtime_error("Automatic Linux installation is available for AppImage builds. Use your package manager for other installations.");
    target = fs::canonical(fs::u8path(image));
    kind = "linux";
#else
    throw std::runtime_error("This platform requires updates through its system or homebrew installer.");
#endif
    fs::path base;
#if defined(__ANDROID__)
    base = fs::u8path(getFilesPath()) / "app-updates";
#else
    base = fs::u8path(get_writable_data_path("app-updates"));
#endif
    fs::create_directories(base);
    std::random_device random;
    fs::path work;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto candidate = base / ("update-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(candidate)) { work = candidate; break; }
    }
    if (work.empty()) throw std::runtime_error("Cannot create a private update folder.");
#ifndef _WIN32
    fs::permissions(work, fs::perms::owner_all, fs::perm_options::replace);
#endif
#if defined(__ANDROID__)
    installAndroidUpdate(package.url, package.sha256, work.u8string(), package.size);
#elif defined(_WIN32) && !defined(TARGET_UWP)
    const auto bundled = target.parent_path() / L"HollycastUpdater.exe";
    if (!fs::is_regular_file(bundled) || fs::is_symlink(fs::symlink_status(bundled)))
        throw std::runtime_error("HollycastUpdater.exe is missing. Re-extract the full Hollycast download before updating.");
    fs::copy_file(bundled, work / L"HollycastUpdater.exe");
    nlohmann::json manifest{{"url", package.url}, {"sha256", package.sha256}, {"size", package.size},
        {"target", target.u8string()}, {"pid", GetCurrentProcessId()}, {"cwd", fs::current_path().u8string()}};
    std::ofstream out(work / "manifest.json", std::ios::binary);
    out << manifest.dump(); out.close();
    if (!out) throw std::runtime_error("Cannot save the update request.");
    const auto arguments = L"--manifest \"" + (work / "manifest.json").wstring() + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (work / L"HollycastUpdater.exe").c_str(),
            arguments.c_str(), target.parent_path().c_str(), SW_HIDE)) <= 32)
        throw std::runtime_error("Windows could not start the update helper.");
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(TARGET_IPHONE)
    writeResource("update/install-desktop.sh", work / "install.sh");
    std::vector<std::string> args{"/bin/sh", (work / "install.sh").string(), kind, package.url,
        package.sha256, std::to_string(package.size), target.string(), std::to_string(getpid()),
        work.string(), fs::current_path().string()};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("Cannot start the update helper.");
    if (child == 0) {
        // Detach without leaving a zombie in the long-lived emulator. Only async-signal-safe
        // process calls run after fork; all arguments were allocated in the parent.
        const pid_t detached = fork();
        if (detached < 0) _exit(127);
        if (detached > 0) _exit(0);
        setsid();
        execv("/bin/sh", argv.data());
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Cannot detach the update helper.");
#endif
    return work.u8string();
}
}
