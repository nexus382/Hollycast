// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtest/gtest.h"
#include "HikaruNativeGraphicsCases.h"
#if defined(HAVE_D3D11) && defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#include "windows/comptr.h"
#endif
#if defined(USE_VULKAN) && defined(USE_SDL) && !defined(LIBRETRO)
#include "hw/hikaru/graphics/device.h"
#include "rend/vulkan/vulkan_context.h"
#include "sdl/sdl.h"
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "stdclass.h"
#include "imgui.h"
#include "log/LogManager.h"
#include "ui/gui.h"
#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace hikaru::graphics {
// Defined only when the native test executable compiles the Vulkan backend.
std::unique_ptr<Backend> makeVulkanBackendForTesting(bool specialize);
#if defined(HAVE_D3D11) && defined(_WIN32)
std::unique_ptr<Backend> makeDx11BackendForTesting(ID3D11Device*,ID3D11DeviceContext*,pD3DCompile,bool);
#endif
}
using namespace hikaru::graphics;
using namespace hikaru::graphics::test;
namespace {
class NativeTestLogListener final : public LogListener {
    void Log(LogTypes::LOG_LEVELS,const char* message) override { std::fprintf(stderr,"%s",message); }
};
class HikaruVulkanTest : public testing::Test {
protected:
    ImGuiContext* previousImGui=nullptr;
    ImGuiContext* testImGui=nullptr;
    RenderType previousRenderer=config::RendererType;
    decltype(settings.display) previousDisplay=settings.display;
    std::string configPath,dataPath;
    bool opened=false, configured=false, videoInitialized=false, createdLogger=false;
    void SetUp() override {
        if(!std::getenv("HOLLYCAST_NATIVE_VULKAN_TESTS"))GTEST_SKIP()<<"requires an explicitly requested physical Vulkan run";
        ASSERT_EQ(GraphicsContext::Instance(),nullptr);
        configPath=get_writable_config_path("");dataPath=get_writable_data_path("");
        const auto folder=std::filesystem::temp_directory_path()/
            ("hollycast-vulkan-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);set_user_config_dir(folder.string());set_user_data_dir(folder.string());
        configured=true;config::setAutoSave(false);config::RendererType=RenderType::Vulkan;
        if(!LogManager::GetInstance()){
            LogManager::Init();createdLogger=true;
            LogManager::GetInstance()->RegisterListener(LogListener::CONSOLE_LISTENER,new NativeTestLogListener());
            LogManager::GetInstance()->SetEnable(LogTypes::RENDERER,true);
            LogManager::GetInstance()->SetEnable(LogTypes::COMMON,true);
            LogManager::GetInstance()->SetLogLevel(LogTypes::LINFO);
        }
        previousImGui=ImGui::GetCurrentContext();gui_init();testImGui=ImGui::GetCurrentContext();
        // gtest_main replaces SDL2main in test builds. Explicitly perform its
        // readiness step, and report an unavailable video driver as a test failure.
        SDL_SetMainReady();ASSERT_EQ(SDL_InitSubSystem(SDL_INIT_VIDEO),0)<<SDL_GetError();videoInitialized=true;
        sdl_window_create();opened=true;
        ASSERT_TRUE(isVulkan(config::RendererType))<<"Vulkan initialization fell back to another API";
        void* window=nullptr;void* display=nullptr;GraphicsContext::Instance()->getWindow(&window,&display);
        if(window)SDL_HideWindow(static_cast<SDL_Window*>(window));
    }
    void TearDown() override {
        if(opened)sdl_window_destroy();else if(videoInitialized)SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if(createdLogger)LogManager::Shutdown();
        if(testImGui){gui_term();ImGui::SetCurrentContext(previousImGui);}
        if(configured){config::RendererType=previousRenderer;settings.display=previousDisplay;
            set_user_config_dir(configPath);set_user_data_dir(dataPath);config::setAutoSave(true);}
    }
};
TEST_F(HikaruVulkanTest, SpecializedPixelsPalettesAndRetiredBatches) {
    auto generic=makeVulkanBackendForTesting(false),optimized=makeVulkanBackendForTesting(true);
    exerciseNativeGraphics(*generic,*optimized);
#if defined(HAVE_D3D11) && defined(_WIN32)
    // Compare the new HLSL backend against the independent GLSL renderer on
    // actual devices, including all light modes and palette matrix selectors.
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&level,1,
        D3D11_SDK_VERSION,&device.get(),nullptr,&context.get())));
    auto dx11=makeDx11BackendForTesting(device,context,D3DCompile,true);
    exerciseNativeGraphics(*generic,*dx11);
#endif
}
}
#endif
