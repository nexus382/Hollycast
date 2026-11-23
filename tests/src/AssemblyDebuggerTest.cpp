// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "debug/assembly_debugger.h"
#include "emulator.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/modules/mmu.h"
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#ifndef LIBRETRO
#include "../../shell/android-studio/flycast/src/main/jni/src/frontend_input_queue.h"
#include "rewind.h"
#include "ui/gui.h"
#include "ui/gui_debugger.h"
#include "ui/gui_util.h"
#include "ui/imgui_driver.h"
#include "ui/mainui.h"
#endif

class AssemblyDebuggerTest : public ::testing::Test
{
protected:
    static constexpr u32 pc = 0x8c010000;

    void SetUp() override
    {
        ASSERT_TRUE(addrspace::reserve());
        // A synthetic RAM program exercises execution control without a game
        // image or BIOS. Bypass the HLE boot entry after loading its session.
        path = std::filesystem::temp_directory_path() / ("hollycast-debugger-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".elf");
        std::ofstream(path, std::ios::binary).put('\0');
        emu.loadGame(path.string().c_str());
        config::GDB = false;
        config::GGPOEnable = false;
        config::DynarecEnabled = true;
        config::AutoSaveState = false;
        config::ThreadedRendering = true;
        settings.raHardcoreMode = false;
        settings.network.online = false;
        settings.naomi.multiboard = false;
        Sh4cntx.pc = pc;
        Sh4cntx.r[0] = 0;
        ASSERT_TRUE(assembly_debugger::open());
    }

    void TearDown() override
    {
        assembly_debugger::close();
        emu.unloadGame(false);
        std::filesystem::remove(path);
        // Direct shared-thread runs can stop before the renderer resets the
        // guest rounding mode. Do not leak that state into unrelated tests.
        setDefaultRoundingMode();
    }

    void program(std::initializer_list<u16> instructions)
    {
        u32 address = pc;
        for (u16 opcode : instructions)
        {
            addrspace::write16(address, opcode);
            address += 2;
        }
    }

    bool waitForBreak()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!assembly_debugger::breakPending() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        emu.stop();
        const bool hit = assembly_debugger::breakPending();
        assembly_debugger::acknowledgeBreak();
        return hit;
    }

    std::filesystem::path path;
};

TEST(AssemblyAddressTest, AcceptsOnlyCompleteAlignedHexadecimalAddresses)
{
    u32 address = 123;
    for (const char* text : {"8C010000", "0x8c010000", "0X8C010000"})
    {
        ASSERT_TRUE(assembly_debugger::parseAddress(text, address));
        EXPECT_EQ(address, 0x8c010000u);
    }
    for (const char* text : {"", "0x", "8c010001", "100000000", "-2", " 8c010000", "8c010000x"})
    {
        address = 123;
        EXPECT_FALSE(assembly_debugger::parseAddress(text, address)) << text;
        EXPECT_EQ(address, 123u);
    }
    ASSERT_TRUE(assembly_debugger::parseAddress("FFFFFFFE", address));
    EXPECT_EQ(address, 0xfffffffeu);
}

TEST_F(AssemblyDebuggerTest, StepsSynchronouslyInBothRenderingModesWithoutPatchingMemory)
{
    program({0x7001, 0x7001, 0x0009}); // add #1,r0; add #1,r0; nop
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc, true));
    for (bool threaded : {false, true})
    {
        config::ThreadedRendering = threaded;
        const u32 before = Sh4cntx.pc;
        assembly_debugger::step();
        EXPECT_EQ(Sh4cntx.pc, before + 2);
        EXPECT_FALSE(emu.running());
    }
    EXPECT_EQ(Sh4cntx.r[0], 2u);
    EXPECT_EQ(addrspace::read16(pc), 0x7001);
    EXPECT_EQ(assembly_debugger::breakpoints().count(pc), 1u);
    EXPECT_TRUE(static_cast<bool>(config::DynarecEnabled));
}

TEST_F(AssemblyDebuggerTest, ResumeHitsBreakpointBeforeInstructionAndCanHitItAgain)
{
    program({0x7001, 0xaffd, 0x0009}); // add #1,r0; bra pc; nop
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc, true));
    assembly_debugger::resume();
    ASSERT_TRUE(waitForBreak());
    EXPECT_EQ(Sh4cntx.pc, pc);
    EXPECT_EQ(Sh4cntx.r[0], 1u);
    assembly_debugger::resume();
    ASSERT_TRUE(waitForBreak());
    EXPECT_EQ(Sh4cntx.pc, pc);
    EXPECT_EQ(Sh4cntx.r[0], 2u);
    EXPECT_EQ(addrspace::read16(pc), 0x7001);
}

TEST_F(AssemblyDebuggerTest, DelaySlotBreakpointStopsBeforeBranchWithNoPartialSideEffects)
{
    program({0x0009, 0xb001, 0x7001, 0x0009, 0xafff, 0x0009});
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc + 4, true));
    const u32 savedPr = Sh4cntx.pr;
    assembly_debugger::resume();
    ASSERT_TRUE(waitForBreak());
    EXPECT_EQ(Sh4cntx.pc, pc + 2);
    EXPECT_EQ(Sh4cntx.pr, savedPr);
    EXPECT_EQ(Sh4cntx.r[0], 0u);
    assembly_debugger::step();
    EXPECT_EQ(Sh4cntx.pc, pc + 8);
    EXPECT_EQ(Sh4cntx.pr, pc + 6);
    EXPECT_EQ(Sh4cntx.r[0], 1u);
}

TEST_F(AssemblyDebuggerTest, SharedThreadExecutionPausesAtBreakpoint)
{
    program({0x7001, 0x7001, 0x0009});
    config::ThreadedRendering = false;
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc + 2, true));
    assembly_debugger::resume();
    ASSERT_TRUE(emu.render());
    emu.stop();
    EXPECT_TRUE(assembly_debugger::breakPending());
    EXPECT_EQ(Sh4cntx.pc, pc + 2);
    EXPECT_EQ(Sh4cntx.r[0], 1u);
}

TEST_F(AssemblyDebuggerTest, ConditionalDelaySlotsOnlyStopWhenExecuted)
{
    program({0x0009, 0x0009});
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc + 2, true));
    EXPECT_FALSE(assembly_debugger::checkBreakpoint(pc, 0x8d00, false));
    EXPECT_TRUE(assembly_debugger::checkBreakpoint(pc, 0x8d00, true));
    assembly_debugger::acknowledgeBreak();
    EXPECT_FALSE(assembly_debugger::checkBreakpoint(pc, 0x8f00, true));
    EXPECT_TRUE(assembly_debugger::checkBreakpoint(pc, 0x8f00, false));
}

TEST_F(AssemblyDebuggerTest, UntakenConditionalBranchStepsToFollowingInstruction)
{
    program({0x8d00, 0x7001, 0x0009}); // bt/s; add #1,r0; nop
    Sh4cntx.sr.T = 0;
    assembly_debugger::step();
    EXPECT_EQ(Sh4cntx.pc, pc + 2);
    EXPECT_EQ(Sh4cntx.r[0], 0u);
    assembly_debugger::step();
    EXPECT_EQ(Sh4cntx.pc, pc + 4);
    EXPECT_EQ(Sh4cntx.r[0], 1u);
}

TEST_F(AssemblyDebuggerTest, SafeMemoryReadsDoNotMutateTlbOrTouchDevices)
{
    program({0x7001});
    u16 opcode = 0;
    ASSERT_TRUE(assembly_debugger::readInstruction(pc, opcode));
    EXPECT_EQ(opcode, 0x7001);
    EXPECT_FALSE(assembly_debugger::readInstruction(pc + 1, opcode));
    EXPECT_FALSE(assembly_debugger::readInstruction(0xff000000, opcode));
    EXPECT_FALSE(assembly_debugger::setBreakpoint(0xff000000, true));
    CCN_MMUCR.AT = 1;
    UTLB[0].Address.VPN = 0x02000000 >> 10;
    UTLB[0].Data.V = 1;
    UTLB[0].Data.PR = 3;
    UTLB[0].Data.SZ0 = 1;
    UTLB[0].Data.PPN = 0x0c010000 >> 10;
    UTLB_Sync(0);
    mmu_set_state();
    ASSERT_TRUE(mmu_enabled());
    const u32 mmucr = CCN_MMUCR.reg_data;
    const u32 pteh = CCN_PTEH.reg_data;
    ASSERT_TRUE(assembly_debugger::readInstruction(0x02000000, opcode));
    EXPECT_EQ(opcode, 0x7001);
    EXPECT_FALSE(assembly_debugger::readInstruction(0x03000000, opcode));
    EXPECT_EQ(CCN_MMUCR.reg_data, mmucr);
    EXPECT_EQ(CCN_PTEH.reg_data, pteh);
    EXPECT_EQ(ITLB[0].Data.V, 0u);
}

TEST_F(AssemblyDebuggerTest, CloseRestoresCpuEngineAndClearsBreakpoints)
{
    program({0x0009});
    auto* interpreter = emu.getSh4Executor();
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc, true));
    assembly_debugger::close();
    EXPECT_FALSE(assembly_debugger::active());
    EXPECT_TRUE(assembly_debugger::breakpoints().empty());
    EXPECT_FALSE(assembly_debugger::breakPending());
    EXPECT_TRUE(static_cast<bool>(config::DynarecEnabled));
#if FEAT_SHREC != DYNAREC_NONE
    EXPECT_NE(emu.getSh4Executor(), interpreter);
#endif
}

TEST_F(AssemblyDebuggerTest, SteppingPreservesHostFloatingPointEnvironment)
{
    program({0x0009});
    const int previous = std::fegetround();
    ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    assembly_debugger::step();
    EXPECT_EQ(std::fegetround(), FE_DOWNWARD);
    std::fesetround(previous);
}

TEST_F(AssemblyDebuggerTest, ClosePreservesDisabledDynarecPreference)
{
    auto* interpreter = emu.getSh4Executor();
    config::DynarecEnabled = false;
    assembly_debugger::close();
    EXPECT_FALSE(static_cast<bool>(config::DynarecEnabled));
    EXPECT_EQ(emu.getSh4Executor(), interpreter);
}

TEST_F(AssemblyDebuggerTest, RejectsHikaruAndReopensForDreamcast)
{
    assembly_debugger::close();
    const auto previousSystem = settings.platform.system;
    settings.platform.system = DC_PLATFORM_HIKARU;
    EXPECT_FALSE(assembly_debugger::available());
    EXPECT_FALSE(assembly_debugger::open());
    EXPECT_FALSE(assembly_debugger::active());
    settings.platform.system = previousSystem;
    EXPECT_TRUE(assembly_debugger::open());
}

TEST_F(AssemblyDebuggerTest, BlocksConflictingDebuggerAndOnlineModes)
{
    assembly_debugger::close();
    const auto verifyRestricted = [](auto& restricted) {
        restricted = true;
        EXPECT_FALSE(assembly_debugger::open());
        restricted = false;
    };
    verifyRestricted(settings.raHardcoreMode);
    verifyRestricted(settings.network.online);
    verifyRestricted(settings.naomi.multiboard);
#ifdef GDB_SERVER
    config::GDB = true;
    EXPECT_FALSE(assembly_debugger::open());
    config::GDB = false;
#endif
    config::GGPOEnable = true;
    EXPECT_FALSE(assembly_debugger::open());
    config::GGPOEnable = false;
}

#ifndef LIBRETRO
TEST_F(AssemblyDebuggerTest, FrontendResetReturnsToBootVector)
{
    program({ 0xAFFE, 0x0009 }); // bra to itself; nop in the delay slot
    for (bool threaded : { false, true })
    {
        config::ThreadedRendering = threaded;
        Sh4cntx.pc = pc;
        Sh4cntx.r[0] = 42;
        emu.start();

        ASSERT_TRUE(emu.resetGame());
        EXPECT_FALSE(emu.running());
        EXPECT_EQ(0xA0000000u, Sh4cntx.pc);
        EXPECT_EQ(0u, Sh4cntx.r[0]);
        EXPECT_EQ(path.string(), settings.content.path);
    }
}

TEST_F(AssemblyDebuggerTest, AndroidRewindPauseWaitsForRestoreAndQueuedSave)
{
    const bool previousRewind = config::RewindEnable;
    config::RewindEnable = true;
    emu.start();
    rewind_state::poll();
    rewind_state::afterFrame();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rewind_state::snapshotCount() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        rewind_state::poll();
    }
    EXPECT_EQ(1u, rewind_state::snapshotCount());

    struct Frontend
    {
        FrontendInputQueue actions;
        std::future<bool> paused;
        void requestPause()
        {
            paused = actions.postPause([] {
                const bool running = emu.running();
                emu.stop();
                emu.finishPendingPause();
                return running;
            });
        }
    } frontend;
    frontend.actions.setActive(true);
    const auto pauseOnRestore = +[](Event, void* param) {
        EXPECT_TRUE(rewind_state::restoring());
        std::thread(&Frontend::requestPause, static_cast<Frontend*>(param)).join();
    };
    EventManager::listen(Event::LoadState, pauseOnRestore, &frontend);
    // Advance the guest clock so the lone snapshot is earlier than the live state.
    EXPECT_FALSE(rewind_state::canStep());
    --Sh4cntx.sh4_sched_next;
    EXPECT_TRUE(rewind_state::canStep());
    Sh4cntx.r[0] = 42;
    int source = 0;
    rewind_state::setButtonInputAllowed(true);
    rewind_state::setRequested(&source, true);
    EXPECT_EQ(rewind_state::FrameAction::Restore, rewind_state::beforeFrame());
    EXPECT_FALSE(rewind_state::restoring());
    rewind_state::releaseSource(&source);
    rewind_state::setButtonInputAllowed(!gui_is_open());
    EventManager::unlisten(Event::LoadState, pauseOnRestore, &frontend);
    EXPECT_EQ(0u, Sh4cntx.r[0]);
    EXPECT_EQ(pc, Sh4cntx.pc);
    EXPECT_TRUE(emu.running());
    ASSERT_TRUE(frontend.paused.valid());
    EXPECT_EQ(std::future_status::timeout, frontend.paused.wait_for(std::chrono::seconds(0)));
    frontend.actions.drain();
    EXPECT_TRUE(frontend.paused.get());
    EXPECT_FALSE(emu.running());

    // The UI can also request pause while an earlier save hotkey has temporarily stopped execution.
    emu.start();
    frontend.actions.post([&] {
        emu.stop();
        EXPECT_TRUE(emu.usesSharedRewind());
        std::thread(&Frontend::requestPause, &frontend).join();
        emu.start();
    });
    frontend.actions.drain();
    EXPECT_TRUE(emu.running());
    ASSERT_TRUE(frontend.paused.valid());
    EXPECT_EQ(std::future_status::timeout, frontend.paused.wait_for(std::chrono::seconds(0)));
    frontend.actions.drain();
    EXPECT_TRUE(frontend.paused.get());
    EXPECT_FALSE(emu.running());
    frontend.requestPause();
    frontend.actions.drain();
    EXPECT_FALSE(frontend.paused.get()); // A game paused in Commands must stay paused on resume.

    rewind_state::releaseRequests();
    config::RewindEnable = previousRewind;
}

TEST(RewindFrontendQueueTest, RendererExitReleasesPendingPause)
{
    FrontendInputQueue actions;
    EXPECT_FALSE(actions.postPause([] { return true; }).valid());
    actions.setActive(true);
    auto paused = actions.postPause([] { return true; });
    ASSERT_TRUE(paused.valid());
    actions.setActive(false);
    ASSERT_EQ(std::future_status::ready, paused.wait_for(std::chrono::seconds(0)));
    EXPECT_THROW(paused.get(), std::future_error);
}

TEST_F(AssemblyDebuggerTest, PresentsDebuggerControlsWhenProgramProducesNoVideoFrames)
{
    // Model the DX/OpenGL presentation contract without a GPU. The driver is
    // eligible to present after a full game-background redraw or a GUI frame.
    class NoVideoDriver final : public ImGuiDriver
    {
    public:
        NoVideoDriver() : ImGuiDriver(false) {}
        void newFrame() override {}
        void renderDrawData(ImDrawData*, bool fullRedraw) override
        {
            ++draws;
            lastFullRedraw = fullRedraw;
            frameReady |= fullRedraw;
        }
        void setFrameRendered() override { frameReady = true; }
        void present() override
        {
            ++presentCalls;
            if (frameReady)
                ++presents;
            frameReady = false;
        }
        ImTextureID getTexture(const std::string&) override { return {}; }
        ImTextureID updateTexture(const std::string&, const u8*, int, int, bool) override { return {}; }
        void deleteTexture(const std::string&) override {}
        int draws = 0;
        int presentCalls = 0;
        int presents = 0;
        bool frameReady = false;
        bool lastFullRedraw = false;
    };

    // Restore process-wide UI state even when a fatal assertion exits early.
    class ScopedUiState
    {
    public:
        ScopedUiState()
            : previousContext(ImGui::GetCurrentContext()), previousDisplay(settings.display), previousState(gui_state),
              previousThreadedRendering(config::ThreadedRendering), previousDupeFrames(config::DupeFrames),
              previousDynarecEnabled(config::DynarecEnabled),
              previousDriver(std::move(imguiDriver)) {}

        ~ScopedUiState()
        {
            emu.stop();
            imguiDriver = std::move(previousDriver);
            settings.display = previousDisplay;
            gui_state = previousState;
            if (testContext != nullptr)
                ImGui::DestroyContext(testContext);
            ImGui::SetCurrentContext(previousContext);
            config::ThreadedRendering = previousThreadedRendering;
            config::DupeFrames = previousDupeFrames;
            config::DynarecEnabled = previousDynarecEnabled;
        }

        ImGuiContext* testContext = nullptr;

    private:
        ImGuiContext* previousContext;
        decltype(settings.display) previousDisplay;
        GuiState previousState;
        bool previousThreadedRendering;
        bool previousDupeFrames;
        bool previousDynarecEnabled;
        std::unique_ptr<ImGuiDriver> previousDriver;
    } scopedUiState;

    scopedUiState.testContext = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    settings.display.width = 1280;
    settings.display.height = 800;
    settings.display.uiScale = 1.f;
    imguiDriver = std::make_unique<NoVideoDriver>();
    auto* driver = static_cast<NoVideoDriver*>(imguiDriver.get());
    program({0xaffe, 0x0009}); // bra pc; nop: no framebuffer or PVR commands.
    config::DupeFrames = false;

    for (bool threaded : {true, false})
    {
        SCOPED_TRACE(threaded);
        config::ThreadedRendering = threaded;
        assembly_debugger::resume();
        gui_state = GuiState::Closed;
        const int previousDraws = driver->draws;
        const int previousPresentCalls = driver->presentCalls;
        const int previousPresents = driver->presents;
        EXPECT_TRUE(mainui_rend_frame());
        // On x86 Linux fegetround() only reports x87 state; nearbyint also
        // catches a guest MXCSR rounding mode leaking into the host UI.
        volatile double roundingProbe = 1.75;
        EXPECT_EQ(std::nearbyint(roundingProbe), 2.0);
        EXPECT_EQ(driver->draws, previousDraws + 1);
        EXPECT_TRUE(driver->lastFullRedraw);
        EXPECT_TRUE(driver->frameReady);
        driver->present();
        EXPECT_EQ(driver->presentCalls, previousPresentCalls + 1);
        EXPECT_EQ(driver->presents, previousPresents + 1);
        emu.stop();
    }

    // A running game that times out waiting for a PVR frame must not present
    // an OSD-only redraw over the stale backbuffer when the debugger is closed.
    assembly_debugger::close();
    // Keep this synthetic presentation fixture on its interpreter instead of
    // also switching CPU engines when the debugger is closed.
    config::DynarecEnabled = false;
    gui_state = GuiState::Closed;
    for (bool threaded : {true, false})
    {
        SCOPED_TRACE(threaded);
        config::ThreadedRendering = threaded;
        const int previousDraws = driver->draws;
        const int previousPresentCalls = driver->presentCalls;
        const int previousPresents = driver->presents;
        emu.start();
        ASSERT_TRUE(emu.running());
        const bool frameRendered = mainui_rend_frame();
        if (frameRendered)
            driver->present();
        EXPECT_EQ(frameRendered, !threaded);
        EXPECT_TRUE(emu.running());
        EXPECT_EQ(driver->draws, previousDraws);
        EXPECT_FALSE(driver->frameReady);
        EXPECT_EQ(driver->presentCalls, previousPresentCalls + static_cast<int>(frameRendered));
        EXPECT_EQ(driver->presents, previousPresents);
        emu.stop();
    }

    // The open-menu path must continue requesting a full redraw and presentation.
    gui_state = GuiState::Commands;
    const int menuDraws = driver->draws;
    const int menuPresentCalls = driver->presentCalls;
    const int menuPresents = driver->presents;
    const bool menuFrameRendered = mainui_rend_frame();
    EXPECT_TRUE(menuFrameRendered);
    EXPECT_EQ(driver->draws, menuDraws + 1);
    EXPECT_TRUE(driver->lastFullRedraw);
    EXPECT_TRUE(driver->frameReady);
    if (menuFrameRendered)
        driver->present();
    EXPECT_EQ(driver->presentCalls, menuPresentCalls + 1);
    EXPECT_EQ(driver->presents, menuPresents + 1);
}

TEST_F(AssemblyDebuggerTest, BrowserKeepsScrollPositionAndCanReturnToCurrentInstruction)
{
    program({0x7001, 0x0009});
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const float previousScale = settings.display.uiScale;
    settings.display.uiScale = 1.f;
    gui_open_debugger();
    auto frame = [] {
        ImGui::NewFrame();
        gui_display_debugger();
        ImGui::Render();
    };
    frame();
    frame();
    ImGuiWindow* browser = ImGui::FindWindowByName("###AssemblyDebugger");
    ImGuiWindow* rows = nullptr;
    if (browser)
        for (ImGuiWindow* window : GImGui->Windows)
            if (window->ParentWindow == browser && std::strstr(window->Name, "/assemblyRows_") != nullptr)
                rows = window;
    EXPECT_NE(browser, nullptr);
    EXPECT_NE(rows, nullptr);
    if (browser && rows)
    {
        ImGui::SetScrollY(rows, 1500.f);
        frame();
        frame();
        const float scroll = rows->Scroll.y;
        Sh4cntx.pc += 2;
        frame();
        EXPECT_FLOAT_EQ(rows->Scroll.y, scroll);
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Current Instruction")));
        frame();
        frame();
        EXPECT_LT(rows->Scroll.y, scroll);
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Step")));
        frame();
        EXPECT_EQ(Sh4cntx.pc, pc + 4);

        ImGui::ActivateItemByID(browser->GetID("##assemblyAddress"));
        frame();
        io.AddInputCharactersUTF8("8C010000");
        frame();
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Go to Address")));
        frame();
        frame();
        EXPECT_EQ(Sh4cntx.pc, pc + 4); // Navigation must never change execution.
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Add Breakpoint")));
        frame();
        EXPECT_EQ(assembly_debugger::breakpoints().count(pc), 1u);
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Remove Breakpoint")));
        frame();
        EXPECT_TRUE(assembly_debugger::breakpoints().empty());

        program({0x7001, 0xaffd, 0x0009});
        Sh4cntx.pc = pc;
        ImGui::ActivateItemByID(browser->GetID(i18n::T("Resume")));
        frame();
        EXPECT_TRUE(emu.running());
        frame();
        ImGuiWindow* running = ImGui::FindWindowByName("###AssemblyDebuggerRunning");
        EXPECT_NE(running, nullptr);
        if (running)
        {
            ImGui::ActivateItemByID(running->GetID(i18n::T("Pause")));
            frame();
            EXPECT_FALSE(emu.running());
            EXPECT_EQ(gui_state, GuiState::Debugger);
            frame();
            ImGui::ActivateItemByID(browser->GetID(i18n::T("Close Debugger")));
            frame();
            EXPECT_FALSE(assembly_debugger::active());
            EXPECT_EQ(gui_state, GuiState::Commands);
        }
    }
    settings.display.uiScale = previousScale;
    gui_state = GuiState::Main;
    ImGui::DestroyContext();
    ImGui::SetCurrentContext(previous);
}

TEST_F(AssemblyDebuggerTest, MouseClickRemovesOnlyTheChosenBreakpoint)
{
    program({0x0009, 0x0009});
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc, true));
    ASSERT_TRUE(assembly_debugger::setBreakpoint(pc + 2, true));

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const float previousScale = settings.display.uiScale;
    settings.display.uiScale = 1.f;
    gui_open_debugger();
    auto frame = [] {
        ImGui::NewFrame();
        gui_display_debugger();
        ImGui::Render();
    };
    frame();
    frame();

    ImGuiWindow* browser = ImGui::FindWindowByName("###AssemblyDebugger");
    ImGuiWindow* registers = nullptr;
    for (ImGuiWindow* window : GImGui->Windows)
        if (window->ParentWindow == browser && std::strstr(window->Name, "/assemblyRegisters_") != nullptr)
            registers = window;
    EXPECT_NE(registers, nullptr);
    if (registers)
    {
        // The pane's last item is the final breakpoint's X. Click its actual
        // laid-out position: activating its ID would bypass overlapping hits.
        const float buttonWidth = ImGui::CalcTextSize("X").x + ImGui::GetStyle().FramePadding.x * 2;
        const ImVec2 removePosition(registers->DC.CursorPosPrevLine.x - buttonWidth * 0.5f,
                registers->DC.CursorPosPrevLine.y + ImGui::GetTextLineHeight() * 0.5f);
        io.AddMousePosEvent(removePosition.x, removePosition.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        EXPECT_EQ(assembly_debugger::breakpoints().count(pc), 1u);
        EXPECT_EQ(assembly_debugger::breakpoints().count(pc + 2), 0u);
        EXPECT_EQ(Sh4cntx.pc, pc);
    }

    settings.display.uiScale = previousScale;
    gui_state = GuiState::Main;
    ImGui::DestroyContext();
    ImGui::SetCurrentContext(previous);
}
#endif
