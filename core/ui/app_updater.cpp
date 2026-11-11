// Portions Copyright 2026 The Hollycast Authors
#include "app_updater.h"
#include "app_update_package.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "gui_util.h"
#include "imgui.h"
#include "oslib/i18n.h"
#include "version.h"
#if defined(__ANDROID__)
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
    bool started = false, requested = false, installing = false;
    bool quitRequested = false, helperStarted = false, dismissed = false;
    bool play = false, playChecking = false, playLaunching = false;
    int playNotesCode = 0, playAvailableCode = 0, playInstalledCode = 0;
    std::string playSnapshot, playInstalledVersion;
    std::string channel, error, status, staging;
    std::chrono::steady_clock::time_point installStarted;
    void check(bool switched = false) {
        if (work.valid() || installing) return;
        error.clear(); status.clear(); result = {}; dismissed = false;
#if defined(__ANDROID__)
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
        cancelled = false;
        work = std::async(std::launch::async, [this, selected, switchingTrack] {
            return detail::check(selected, GIT_HASH, switchingTrack, cancelled);
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
    if (state.started) state.check();
}

void stop() {
    state.cancelled = true;
    if (state.work.valid()) state.work.wait();
}

void draw(bool idle) {
    if (!state.started) {
        state.started = true;
#if defined(__ANDROID__)
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
            const auto status = helperStatus(pending);
            if (status.rfind("Error\n", 0) == 0) {
                state.error = status.substr(6);
                state.requested = true;
                clearPending();
            } else if (status.rfind("Done", 0) == 0 || status.rfind("Installer", 0) == 0) clearPending();
        }
        if (state.requested && state.error.empty()) state.check();
#if !defined(TEST_AUTOMATION) && !defined(HOLLYCAST_DISABLE_STARTUP_UPDATES)
        else if (state.error.empty() && config::loadBool("updates", "startup", true)) state.check();
#endif
    }
#if defined(__ANDROID__)
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
            state.cancelled = false;
            state.work = std::async(std::launch::async, [installed, code] {
                return detail::playNotes(installed, code, state.cancelled);
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
    if (state.installing) {
        const auto status = helperStatus(state.staging);
        if (!status.empty()) state.helperStarted = true;
        if (status.rfind("Error\n", 0) == 0) {
            state.error = status.substr(6); state.installing = false;
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
            clearPending();
        } else if ((!state.helperStarted && std::chrono::steady_clock::now() - state.installStarted > std::chrono::seconds(20))
                || std::chrono::steady_clock::now() - state.installStarted > std::chrono::minutes(8)) {
            cancelInstall();
            state.error = T("The update helper did not finish. Please retry.");
        }
    }
    if (state.requested && idle && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::OpenPopup("Hollycast Updates###app-updates");
        state.requested = false;
    }
    const auto display = ImGui::GetIO().DisplaySize;
    const ImVec2 size(std::min(uiScaled(680), std::max(1.0f, display.x - uiScaled(20))),
        std::min(uiScaled(600), std::max(1.0f, display.y - uiScaled(20))));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    bool shown = true;
    if (!ImGui::BeginPopupModal("Hollycast Updates###app-updates", state.installing ? nullptr : &shown, ImGuiWindowFlags_NoSavedSettings)) {
        if (!shown) { state.dismissed = true; state.requested = false; }
        return;
    }
    if (state.play && !state.playInstalledVersion.empty())
        ImGui::TextWrapped("%s: %s (%d)", T("Current version"), state.playInstalledVersion.c_str(), state.playInstalledCode);
    else
        ImGui::TextWrapped("%s: %s (%s)", T("Current version"), GIT_VERSION, GIT_HASH);
    ImGui::TextWrapped("%s: %s", T("Build date"), BUILD_DATE);
#if defined(__ANDROID__)
    if (state.play) {
        ImGui::TextWrapped("%s", T("Updates are managed by Google Play. Your Play testing enrollment controls the update track."));
        if (ImGui::Button(T("Manage Play testing track"))) androidAppUpdates("testing");
    }
#endif
    const char* tracks[]{"Master", "Dev"};
    int selected = state.channel == "master" ? 0 : 1;
    ImGui::BeginDisabled(state.work.valid() || state.installing || state.playChecking || state.playLaunching);
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
    if (state.work.valid() || state.playChecking) ImGui::TextUnformatted(T("Checking for updates..."));
    if (!state.error.empty()) ImGui::TextWrapped("%s", state.error.c_str());
    if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    const float footer = uiScaled(85);
    ImGui::BeginChild("update-notes", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    scrollWindowFromTouch();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(state.result.changelog.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::BeginDisabled(!state.result.available || state.work.valid() || state.installing || state.playChecking || state.playLaunching);
#if defined(__ANDROID__)
    const char* installLabel = state.play ? T("Update through Google Play") : T("Install update");
#else
    const char* installLabel = T("Update and restart");
#endif
    if (ImGui::Button(installLabel)) {
        try {
            state.error.clear();
#if defined(__ANDROID__)
            if (state.play) {
                androidAppUpdates("install");
            } else
#endif
            {
                state.staging = detail::install(state.result.package);
                config::saveStr("updates", "pending_directory", state.staging);
                SaveSettings();
                state.installing = true;
                state.quitRequested = false;
                state.helperStarted = false;
                state.installStarted = std::chrono::steady_clock::now();
#if defined(__ANDROID__)
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
        state.dismissed = true; state.requested = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
}
