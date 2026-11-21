// Portions Copyright 2026 The Hollycast Authors
#include "ui/app_updater.h"
#include "ui/app_update_package.h"
#include "types.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

settings_t settings;
namespace {
std::promise<void> finishCheck;
std::shared_future<void> allowed = finishCheck.get_future().share();
std::atomic<bool> checked{false};
std::atomic<bool> completed{false}, sawCancellation{false};
bool startupEnabled = true;
bool installSeen = false, installDisabled = true;
int playInstalls = 0;
int snapshotReads = 0;
bool snapshotDirty = true;
bool snapshotFailure = false, malformedSnapshot = false, deferPlayCheck = false;
std::string playSnapshot = "{\"source\":\"play\"}";
ImVec2 installPosition;
constexpr const char* popup = "Hollycast Updates###app-updates";
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool frame(bool idle = true) {
    installSeen = false;
    ImGui::NewFrame();
    app_updates::draw(idle);
    const bool open = ImGui::IsPopupOpen(popup);
    ImGui::Render();
    return open;
}
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID, const ImRect&, const ImGuiLastItemData*) {}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* context, ImGuiID, const char* label, ImGuiItemStatusFlags) {
    if (label == nullptr) return;
    if (std::string(label) == "Update and restart" || std::string(label) == "Update through Google Play") {
        installSeen = true;
        installDisabled = (context->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
        installPosition = context->LastItemData.Rect.GetCenter();
    }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return ""; }

std::string androidAppUpdates(const std::string& action) {
    if (action.empty()) {
        ++snapshotReads;
        if (snapshotFailure) throw std::runtime_error("Transient bridge failure");
        if (malformedSnapshot) return "{";
    }
    if (action == "check" && !deferPlayCheck) {
        playSnapshot = "{\"source\":\"play\",\"availableCode\":11,\"installedCode\":10,\"revision\":1}";
        snapshotDirty = true;
    }
    if (action == "install") ++playInstalls;
    return playSnapshot;
}
bool androidAppUpdatesChanged() {
    const bool changed = snapshotDirty;
    snapshotDirty = false;
    return changed;
}
std::string get_writable_data_path(const std::string&) { return "unused-test-data"; }
namespace config {
std::string loadStr(const std::string&, const std::string&, const std::string& fallback) { return fallback; }
bool loadBool(const std::string&, const std::string&, bool) { return startupEnabled; }
void saveStr(const std::string&, const std::string&, const std::string&) {}
void saveBool(const std::string&, const std::string&, bool) {}
}
namespace i18n { const char* T(const char* text) { return text; } }
void SaveSettings() {}
void dc_exit() { throw std::runtime_error("Unexpected installation during startup UI test"); }
void scrollWindowFromTouch() {}
namespace app_updates::detail {
void cleanupDownloads(const std::string&, const std::string&) {}
Check check(const std::string&, const std::string&, bool, const std::atomic<bool>& cancelled, const Fetch&) {
    checked = true;
    while (allowed.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready && !cancelled.load()) {}
    sawCancellation = cancelled.load();
    Check result;
    result.available = true;
    result.package.version = "dev-test";
    result.package.date = "2026-11-11";
    result.changelog = "A published update";
    completed = true;
    return result;
}
Check playNotes(int, int, const std::atomic<bool>& cancelled) {
    return check("", "", false, cancelled, {});
}
std::string install(const Package&) { throw std::runtime_error("Unexpected install"); }
}
int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "startup";
        deferPlayCheck = mode == "play-queued";
        if (deferPlayCheck) playSnapshot = "{\"source\":\"play\",\"availableCode\":11}";
        startupEnabled = mode != "disabled" && mode != "manual-game" && mode != "play-count";
        const bool defer = mode == "defer";
        const bool manualGame = mode == "manual-game";
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        ImGui::GetCurrentContext()->TestEngineHookItems = true;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800, 640);
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (mode == "play-queued") {
            app_updates::open();
            frame();
            expect(installSeen && installDisabled, "Initial snapshot cleared a queued manual check");
            expect(!checked, "Initial availability started notes before the queued check finished");
            snapshotDirty = true;
            frame();
            expect(installDisabled && !checked, "Reattaching to the same snapshot cleared the queued check");
            playSnapshot = "{\"source\":\"play\",\"availableCode\":11,\"checking\":false}";
            snapshotDirty = true;
            frame();
            expect(!installDisabled, "Completed queued check did not enable installation");
            app_updates::stop();
            ImGui::DestroyContext();
            return 0;
        }
        if (mode == "play-count") {
            frame(false);
            expect(snapshotReads == 1, "Source selection did not read exactly one snapshot");
            for (int i = 0; i < 120; ++i) frame(false);
            expect(snapshotReads == 1, "Unchanged gameplay frames queried the Java snapshot");
            app_updates::open();
            frame(false);
            expect(snapshotReads == 2, "Manual check did not consume its changed snapshot");
            expect(installSeen && installDisabled, "Play installation was enabled during gameplay");
            frame(true);
            expect(installSeen && !installDisabled, "Optional notes blocked the Play install action");
            snapshotFailure = snapshotDirty = true;
            frame();
            const auto failedReads = snapshotReads;
            snapshotFailure = false;
            frame();
            expect(snapshotReads == failedReads + 1, "Transient bridge failure was not retried without publication");
            malformedSnapshot = snapshotDirty = true;
            frame();
            const auto malformedReads = snapshotReads;
            malformedSnapshot = false;
            frame();
            expect(snapshotReads == malformedReads + 1, "Transient parse failure was not retried without publication");
            playSnapshot = "{\"source\":\"play\",\"availableCode\":11,\"launching\":true}";
            snapshotDirty = true;
            frame();
            expect(installDisabled, "Published launching state did not disable installation");
            playSnapshot = "{\"source\":\"play\",\"availableCode\":11,\"launching\":false,\"message\":\"Update cancelled\"}";
            snapshotDirty = true;
            frame();
            expect(!installDisabled, "Cancellation did not permit a consented retry");
            playSnapshot = "{\"source\":\"play\",\"availableCode\":0,\"error\":\"offline\"}";
            snapshotDirty = true;
            frame();
            expect(installDisabled, "Failed check retained an unavailable install action");
            const auto reads = snapshotReads;
            for (int i = 0; i < 120; ++i) frame();
            expect(snapshotReads == reads, "Unchanged popup frames queried the Java snapshot");
            app_updates::stop();
            frame();
            expect(installDisabled, "Stale asynchronous notes restored unavailable installation");
            ImGui::DestroyContext();
            std::cout << "240 unchanged frames: 0 bridge reads; manual check, gameplay, launching, cancellation and failure passed\n";
            return 0;
        }
        // Exercise several real BeginPopupModal calls before a background result exists.
        for (int i = 0; i < 3; ++i) {
            const bool opened = frame(!defer && !manualGame);
            if (mode != "play") expect(!opened, "Popup appeared before update availability");
        }
        if (mode == "play") {
            expect(frame(), "Play availability did not open the updater");
            expect(installSeen && !installDisabled, "Optional notes blocked the Play install action");
            io.AddMousePosEvent(installPosition.x, installPosition.y);
            frame();
            io.AddMouseButtonEvent(0, true); frame();
            io.AddMouseButtonEvent(0, false); frame();
            expect(playInstalls == 1, "Play install did not start while notes were still loading");
        }
        if (!startupEnabled) {
            expect(!checked.load(), "Disabled startup check contacted the backend");
            app_updates::open();
            if (manualGame) expect(frame(false), "Manual update check stayed hidden during gameplay");
        }
        if (mode == "shutdown") {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!checked && std::chrono::steady_clock::now() < until) std::this_thread::yield();
            expect(checked, "Shutdown test did not start its delayed request");
            const auto start = std::chrono::steady_clock::now();
            app_updates::stop();
            expect(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200), "Shutdown waited for the network");
            finishCheck.set_value();
            while (!completed && std::chrono::steady_clock::now() < until) std::this_thread::yield();
            expect(completed && sawCancellation, "Background request outlived its cancellation state");
            ImGui::DestroyContext();
            std::cout << "shutdown: returned without waiting for the delayed request\n";
            return 0;
        }
        finishCheck.set_value();
        bool opened = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!opened && std::chrono::steady_clock::now() < deadline) {
            opened = frame(!defer && !manualGame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (manualGame) {
            // First prove that the result enabled installation while idle, then
            // that the same available result is blocked while a game is loaded.
            do {
                frame(true);
                std::this_thread::yield();
            } while (installDisabled && std::chrono::steady_clock::now() < deadline);
            expect(installSeen && !installDisabled, "Available result never enabled installation while idle");
            frame(false);
        }
        if (defer) {
            expect(!opened, "Update interrupted a busy/gameplay frame");
            opened = frame(true);
        }
        expect(opened, "Available startup update was silently dismissed before it opened");
        if (manualGame) expect(installSeen && installDisabled, "Updater allowed installation during a loaded game");
        app_updates::stop();
        ImGui::DestroyContext();
        std::cout << mode << ": popup appeared when eligible\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        // An assertion may fail while the fake network request is still blocked.
        try { finishCheck.set_value(); } catch (const std::future_error&) {}
        app_updates::stop();
        return 1;
    }
}
