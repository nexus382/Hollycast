// Portions Copyright 2026 The Hollycast Authors
#include "app_updater.h"
#include "app_update_package.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "gui_util.h"
#include "imgui.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include "version.h"
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
#include "json.hpp"
extern std::string androidAppUpdates(const std::string& action);
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>

namespace app_updates {
namespace {
using i18n::T;
struct State {
    std::atomic<bool> cancelled{false};
    std::future<detail::Check> work;
    detail::Check result;
    bool started = false, requested = false, installing = false, awaitingInstaller = false;
    bool quitRequested = false, helperStarted = false, dismissed = false, manual = false, recovering = false;
    bool play = false, playChecking = false, playLaunching = false;
    int playNotesCode = 0, playAvailableCode = 0, playInstalledCode = 0;
    std::string playSnapshot, playInstalledVersion;
    std::string channel, error, status, staging;
    std::chrono::steady_clock::time_point installStarted;
    std::chrono::steady_clock::time_point recoveryStarted, nextRecoveryPoll;
    void start(std::function<detail::Check(const std::atomic<bool>&)> operation) {
        cancelled = false;
        // The transport observes cancellation while waiting for network I/O.
        // Keep the worker joined so neither HTTP nor UI state can outlive teardown.
        work = std::async(std::launch::async, [this, operation = std::move(operation)] {
            return operation(cancelled);
        });
    }
    void check(bool switched = false) {
        if (work.valid() || installing || awaitingInstaller) return;
        error.clear(); status.clear(); result = {}; dismissed = false;
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
        if (play) {
            playChecking = true;
            playNotesCode = 0;
            try { androidAppUpdates("check"); }
            catch (const std::exception& e) { playChecking = false; error = e.what(); }
            return;
        }
#endif
        const auto selected = channel;
        // A saved track different from this binary's track remains an explicit switch
        // across retries/restarts; only same-track checks suppress older builds.
        const bool switchingTrack = switched || selected != HOLLYCAST_UPDATE_CHANNEL;
        start([selected, switchingTrack](const std::atomic<bool>& token) {
            return detail::check(selected, GIT_HASH, switchingTrack, token);
        });
    }
};
State state;
std::string helperStatus(const std::string& directory) {
    std::ifstream in(std::filesystem::u8path(directory) / "status", std::ios::binary);
    std::array<char, 4096> buffer{};
    in.read(buffer.data(), buffer.size());
    return std::string(buffer.data(), static_cast<size_t>(in.gcount()));
}
void clearPending() {
    config::saveStr("updates", "pending_directory", "");
    SaveSettings();
#if !defined(__ANDROID__)
    detail::cleanupDownloads(get_writable_data_path("app-updates"));
#endif
}
void cancelInstall() {
    std::ofstream cancel(std::filesystem::u8path(state.staging) / "cancel");
    cancel.close();
    if (!cancel) {
        state.quitRequested = true; // Never close while a cancellation request could not be saved.
        state.error = T("Cannot cancel the update. Keep Hollycast open until the helper stops.");
        return;
    }
    state.installing = false;
    state.status = T("Update cancelled.");
    clearPending();
}
void preferences() {
    config::saveStr("updates", "channel", state.channel);
    SaveSettings();
}
}

void open() {
    state.dismissed = false;
    state.requested = true;
    state.manual = true;
    if (state.started) state.check();
}

void stop() {
    state.cancelled = true;
    if (state.work.valid()) state.work.wait();
}

void draw(bool idle) {
    if (!state.started) {
        state.started = true;
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
        state.play = true; // Fail closed: a broken Android bridge must never select GitHub for a store build.
        try {
            state.playSnapshot = androidAppUpdates("");
            const auto source = nlohmann::json::parse(state.playSnapshot).value("source", "");
            if (source != "play" && source != "github") throw std::runtime_error("Unknown Android update source.");
            state.play = source == "play";
        } catch (const std::exception& e) { state.error = e.what(); state.requested = true; }
#endif
        state.channel = config::loadStr("updates", "channel", HOLLYCAST_UPDATE_CHANNEL);
        if (state.channel != "master" && state.channel != "dev") state.channel = HOLLYCAST_UPDATE_CHANNEL;
        // Recover a helper failure that happened after the previous process exited.
        const auto pending = config::loadStr("updates", "pending_directory", "");
        if (!pending.empty()) {
            state.staging = pending;
            const auto status = helperStatus(pending);
            if (status.rfind("Error\n", 0) == 0) {
                state.error = status.substr(6);
                state.requested = true;
                clearPending();
            } else if (status.rfind("Done", 0) == 0 || status.rfind("Cancelled", 0) == 0
                    || status.rfind("Installer", 0) == 0) clearPending();
            else {
                state.recovering = true;
                state.recoveryStarted = std::chrono::steady_clock::now();
            }
        }
#if !defined(__ANDROID__)
        detail::cleanupDownloads(get_writable_data_path("app-updates"), pending);
#endif
        if (state.requested && state.error.empty()) state.check();
#if !defined(TEST_AUTOMATION) && !defined(HOLLYCAST_DISABLE_STARTUP_UPDATES)
        else if (state.error.empty() && config::loadBool("updates", "startup", true)) state.check();
#endif
    }
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
    if (state.play) {
        try {
            const auto snapshot = androidAppUpdates("");
            if (snapshot != state.playSnapshot) {
                state.playSnapshot = snapshot;
                const auto info = nlohmann::json::parse(snapshot);
                state.playChecking = info.value("checking", false);
                state.playLaunching = info.value("launching", false);
                state.error = info.value("error", "");
                state.status = info.value("message", "");
                state.playAvailableCode = info.value("availableCode", 0);
                state.playInstalledCode = info.value("installedCode", 0);
                state.playInstalledVersion = info.value("installedName", "");
                if (!state.playChecking && state.playAvailableCode == 0) { state.result = {}; state.playNotesCode = 0; }
            }
        } catch (const std::exception& e) { state.playChecking = false; state.error = e.what(); }
        if (!state.playChecking && state.playAvailableCode > 0
                && state.playAvailableCode != state.playNotesCode && !state.work.valid()) {
            state.playNotesCode = state.playAvailableCode;
            if (!state.dismissed && !ImGui::IsPopupOpen("Hollycast Updates###app-updates")) state.requested = true;
            const int code = state.playAvailableCode, installed = state.playInstalledCode;
            state.result = {};
            state.result.available = true;
            state.result.package.version = "Google Play (" + std::to_string(code) + ")";
            state.result.package.date = T("Not provided by Google Play");
            state.result.changelog = T("Loading release notes. You can update through Google Play now.");
            state.start([installed, code](const std::atomic<bool>& token) {
                return detail::playNotes(installed, code, token);
            });
        }
    }
#endif
    if (state.work.valid() && state.work.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = state.work.get();
            if (!state.play || (state.playNotesCode > 0 && state.playNotesCode == state.playAvailableCode))
                state.result = std::move(result);
            if (state.result.available && !state.dismissed
                    && !ImGui::IsPopupOpen("Hollycast Updates###app-updates")) state.requested = true;
            else if (!state.result.available && !state.play) state.status = T("You already have this build or a newer build on the selected track.");
        } catch (const std::exception& e) {
            // Offline startup is silent. Manual checks expose errors and retain a retry action.
            state.error = e.what();
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (state.recovering && now >= state.nextRecoveryPoll) {
        state.nextRecoveryPoll = now + std::chrono::milliseconds(250);
        const auto status = helperStatus(state.staging);
        const bool expired = now - state.recoveryStarted > std::chrono::minutes(8)
            || (status.empty() && now - state.recoveryStarted > std::chrono::seconds(20));
        if (status.rfind("Error\n", 0) == 0) {
            state.error = status.substr(6);
            state.requested = true;
        }
        if (expired) {
            state.error = T("The previous update did not finish. Check the installed version and retry.");
            state.requested = true;
        }
        if (expired || status.rfind("Error\n", 0) == 0 || status.rfind("Done", 0) == 0 || status.rfind("Cancelled", 0) == 0) {
            state.recovering = false;
            clearPending();
        }
    }
    if (state.installing || state.awaitingInstaller) {
        const auto status = helperStatus(state.staging);
        if (!status.empty()) state.helperStarted = true;
        if (status.rfind("Error\n", 0) == 0) {
            state.error = status.substr(6); state.installing = false; state.awaitingInstaller = false;
            state.status.clear();
            clearPending();
        } else if (status.rfind("Ready", 0) == 0 && idle && !state.quitRequested) {
            // The helper has verified and staged the package before the application exits.
            state.quitRequested = true;
            SaveSettings();
            dc_exit();
        } else if (status.rfind("Installer", 0) == 0) {
            state.status = T("Confirm the update in the Android package installer.");
            state.installing = false;
            state.awaitingInstaller = true;
        } else if (status.rfind("Done", 0) == 0 || status.rfind("Cancelled", 0) == 0) {
            state.installing = false;
            state.awaitingInstaller = false;
            state.status = status.rfind("Done", 0) == 0 ? T("Update installed.") : T("Update cancelled.");
            clearPending();
        } else if (!state.awaitingInstaller && ((!state.helperStarted && std::chrono::steady_clock::now() - state.installStarted > std::chrono::seconds(20))
                || std::chrono::steady_clock::now() - state.installStarted > std::chrono::minutes(8))) {
            cancelInstall();
            state.error = T("The update helper did not finish. Please retry.");
        }
    }
    if (state.requested && (idle || state.manual) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::OpenPopup("Hollycast Updates###app-updates");
        state.requested = false;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    const ImVec2 size(std::min(uiScaled(680), std::max(1.0f, display.x - uiScaled(20))),
        std::min(uiScaled(600), std::max(1.0f, display.y - uiScaled(20))));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    // ImGui also clears p_open for a popup that has never opened. Only an
    // already-open modal becoming closed represents a user's dismissal.
    const bool wasOpen = ImGui::IsPopupOpen("Hollycast Updates###app-updates");
    bool shown = true;
    if (!ImGui::BeginPopupModal("Hollycast Updates###app-updates", state.installing ? nullptr : &shown, ImGuiWindowFlags_NoSavedSettings)) {
        if (wasOpen && !shown) { state.dismissed = true; state.requested = false; state.manual = false; }
        return;
    }
    if (state.play && !state.playInstalledVersion.empty())
        ImGui::TextWrapped("%s: %s (%d)", T("Current version"), state.playInstalledVersion.c_str(), state.playInstalledCode);
    else
        ImGui::TextWrapped("%s: %s", T("Current version"), detail::versionLabel(HOLLYCAST_UPDATE_CHANNEL, GIT_HASH).c_str());
    ImGui::TextWrapped("%s: %s", T("Build date"), BUILD_DATE);
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
    if (state.play) {
        ImGui::TextWrapped("%s", T("Updates are managed by Google Play. Your Play testing enrollment controls the update track."));
        if (ImGui::Button(T("Manage Play testing track"))) androidAppUpdates("testing");
    }
#endif
    const char* tracks[]{"Master", "Dev"};
    int selected = state.channel == "master" ? 0 : 1;
    ImGui::BeginDisabled(state.work.valid() || state.installing || state.awaitingInstaller || state.playChecking || state.playLaunching);
    if (!state.play && ImGui::Combo(T("Update track"), &selected, tracks, 2)) {
        state.channel = selected == 0 ? "master" : "dev";
        preferences();
        state.check(true);
    }
    bool startup = config::loadBool("updates", "startup", true);
    if (ImGui::Checkbox(T("Check for updates on startup"), &startup)) {
        config::saveBool("updates", "startup", startup); SaveSettings();
    }
    if (ImGui::Button(T("Check now"))) state.check();
    ImGui::EndDisabled();
    if (!state.result.package.version.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s: %s", T("Newest version"), state.result.package.version.c_str());
        ImGui::TextWrapped("%s: %s", T("Build date"), state.result.package.date.c_str());
    }
    if (state.result.olderTrack) ImGui::TextWrapped("%s", T("This track has a different history and may contain fewer features than your current build."));
    if ((!state.play && state.work.valid()) || state.playChecking) ImGui::TextUnformatted(T("Checking for updates..."));
    if (!state.error.empty()) ImGui::TextWrapped("%s", state.error.c_str());
    if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    if (!state.result.installMessage.empty()) ImGui::TextWrapped("%s", state.result.installMessage.c_str());
    if (!idle && state.result.available) ImGui::TextWrapped("%s", T("Close the current game before installing an update."));
    const float footer = uiScaled(85);
    ImGui::BeginChild("update-notes", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    scrollWindowFromTouch();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(state.result.changelog.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::BeginDisabled(!idle || !state.result.available || !state.result.installMessage.empty()
        || (!state.play && state.work.valid()) || state.installing || state.awaitingInstaller || state.playChecking || state.playLaunching);
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
    const char* installLabel = state.play ? T("Update through Google Play") : T("Install update");
#else
    const char* installLabel = T("Update and restart");
#endif
    if (ImGui::Button(installLabel)) {
        try {
            state.error.clear();
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
            if (state.play) {
                androidAppUpdates("install");
            } else
#endif
            {
#if !defined(__ANDROID__)
                detail::cleanupDownloads(get_writable_data_path("app-updates"),
                    config::loadStr("updates", "pending_directory", ""));
#endif
                state.staging = detail::install(state.result.package);
                state.recovering = false;
                config::saveStr("updates", "pending_directory", state.staging);
                SaveSettings();
                state.installing = true;
                state.awaitingInstaller = false;
                state.quitRequested = false;
                state.helperStarted = false;
                state.installStarted = std::chrono::steady_clock::now();
#if defined(__ANDROID__) || defined(HOLLYCAST_TEST_PLAY_UPDATES)
                state.status = T("Downloading and verifying the update before opening the Android installer.");
#else
                state.status = T("Downloading and verifying the update. Hollycast will restart when it is ready.");
#endif
            }
        } catch (const std::exception& e) { state.error = e.what(); }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (state.installing) {
        if (ImGui::Button(T("Cancel update"))) cancelInstall();
    } else if (ImGui::Button(T("Later"))) {
        state.dismissed = true; state.requested = false; state.manual = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
}
