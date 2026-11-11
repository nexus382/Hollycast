// Portions Copyright 2026 The Hollycast Authors
#include "ui/app_updater.h"
#include "ui/app_update_package.h"
#include "types.h"
#include "imgui.h"
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
bool startupEnabled = true;
constexpr const char* popup = "Hollycast Updates###app-updates";
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool frame(bool idle = true) {
    ImGui::NewFrame();
    app_updates::draw(idle);
    const bool open = ImGui::IsPopupOpen(popup);
    ImGui::Render();
    return open;
}
}
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
Check check(const std::string&, const std::string&, bool, const std::atomic<bool>&, const Fetch&) {
    checked = true;
    allowed.wait();
    Check result;
    result.available = true;
    result.package.version = "dev-test";
    result.package.date = "2026-11-11";
    result.changelog = "A published update";
    return result;
}
std::string install(const Package&) { throw std::runtime_error("Unexpected install"); }
}
int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "startup";
        startupEnabled = mode != "disabled";
        const bool defer = mode == "defer";
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800, 640);
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        // Exercise several real BeginPopupModal calls before a background result exists.
        for (int i = 0; i < 3; ++i) expect(!frame(!defer), "Popup appeared before update availability");
        if (!startupEnabled) {
            expect(!checked.load(), "Disabled startup check contacted the backend");
            app_updates::open();
        }
        finishCheck.set_value();
        bool opened = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!opened && std::chrono::steady_clock::now() < deadline) {
            opened = frame(!defer);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (defer) {
            expect(!opened, "Update interrupted a busy/gameplay frame");
            opened = frame(true);
        }
        expect(opened, "Available startup update was silently dismissed before it opened");
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
