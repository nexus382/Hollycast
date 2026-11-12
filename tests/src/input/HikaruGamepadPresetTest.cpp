// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "input/gamepad_device.h"
#include <limits>

namespace {
class FeedbackGamepad final : public GamepadDevice {
public:
    explicit FeedbackGamepad(int port) : GamepadDevice(port, "FeedbackTest", false) { rumbleEnabled = true; }
    void rumble(float power, float, u32 duration) override {
        lastPower = power;
        lastDuration = duration;
        ++calls;
    }
    float lastPower = 0.f;
    u32 lastDuration = 0;
    unsigned calls = 0;
};
class GamepadFeedbackTest : public testing::Test {
protected:
    std::shared_ptr<FeedbackGamepad> pad;
    void SetUp() override {
        pad = std::make_shared<FeedbackGamepad>(2);
        GamepadDevice::Register(pad);
    }
    void TearDown() override { GamepadDevice::Unregister(pad); }
};
}
TEST_F(GamepadFeedbackTest, RoutesSignedForceAndStopToTheAssignedPort) {
    GamepadDevice::ApplyForceFeedback(1, 0.5f);
    EXPECT_EQ(pad->calls, 0u);
    GamepadDevice::ApplyForceFeedback(2, -0.5f);
    EXPECT_FLOAT_EQ(pad->lastPower, 0.5f);
    EXPECT_EQ(pad->lastDuration, 50u);
    GamepadDevice::ApplyForceFeedback(2, 3.f);
    EXPECT_FLOAT_EQ(pad->lastPower, 1.f);
    GamepadDevice::ApplyForceFeedback(2, std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(pad->lastPower, 0.f);
    EXPECT_EQ(pad->lastDuration, 0u);
}
TEST_F(GamepadFeedbackTest, PortReassignmentReleasesThePreviousForce) {
    GamepadDevice::ApplyForceFeedback(2, 0.75f);
    ASSERT_EQ(pad->lastPower, 0.75f);
    pad->set_maple_port(1);
    EXPECT_EQ(pad->lastPower, 0.f);
    EXPECT_EQ(pad->lastDuration, 0u);
    const unsigned stopped = pad->calls;
    GamepadDevice::ApplyForceFeedback(2, 1.f);
    EXPECT_EQ(pad->calls, stopped);
    GamepadDevice::ApplyForceFeedback(1, -0.25f);
    EXPECT_FLOAT_EQ(pad->lastPower, 0.25f);
    const unsigned running = pad->calls;
    pad->set_maple_port(1);
    EXPECT_EQ(pad->calls, running);
}
TEST_F(GamepadFeedbackTest, UnregisterReleasesForceAndStopsFurtherRouting) {
    GamepadDevice::ApplyForceFeedback(2, -1.f);
    ASSERT_EQ(pad->lastPower, 1.f);
    GamepadDevice::Unregister(pad);
    EXPECT_EQ(pad->lastPower, 0.f);
    EXPECT_EQ(pad->lastDuration, 0u);
    const unsigned stopped = pad->calls;
    GamepadDevice::ApplyForceFeedback(2, 1.f);
    EXPECT_EQ(pad->calls, stopped);
    GamepadDevice::Unregister(pad);
    EXPECT_EQ(pad->calls, stopped);
}
TEST_F(GamepadFeedbackTest, SupportsAllPortsAndRejectsInvalidPorts) {
    pad->set_maple_port(4);
    GamepadDevice::ApplyForceFeedback(0, 0.75f);
    EXPECT_FLOAT_EQ(pad->lastPower, 0.75f);
    const unsigned calls = pad->calls;
    GamepadDevice::ApplyForceFeedback(4, 1.f);
    EXPECT_EQ(pad->calls, calls);
    GamepadDevice::ApplyForceFeedback(3, 0.f);
    EXPECT_EQ(pad->lastPower, 0.f);
}
#ifdef USE_HIKARU
#include "hw/hikaru/frontend.h"
#include "hw/hikaru/input_bindings.h"
#include "hw/hikaru/roms.h"

TEST_F(GamepadFeedbackTest, PauseAndUnloadReleaseTheCabinetMotor) {
    pad->set_maple_port(0);
    GamepadDevice::ApplyForceFeedback(0, 1.f);
    ASSERT_EQ(pad->lastPower, 1.f);
    hikaru::frontend::pauseAudio();
    EXPECT_EQ(pad->lastPower, 0.f);
    EXPECT_EQ(pad->lastDuration, 0u);
    GamepadDevice::ApplyForceFeedback(0, -1.f);
    ASSERT_EQ(pad->lastPower, 1.f);
    hikaru::frontend::unload();
    EXPECT_EQ(pad->lastPower, 0.f);
    EXPECT_EQ(pad->lastDuration, 0u);
    EXPECT_FALSE(hikaru::frontend::gamepadPresetEnabled());
}
#ifdef USE_SDL
#include "sdl/sdl_gamepad.h"
#if SDL_VERSION_ATLEAST(2, 0, 14)
namespace {
class PresetSDLGamepad final : public SDLGamepad {
public:
    using SDLGamepad::SDLGamepad;
    using GamepadDevice::setGamepadPreset;
};
class HikaruGamepadPresetTest : public testing::Test {
protected:
    int joystickIndex = -1;
    bool initialized = false;
    std::shared_ptr<PresetSDLGamepad> pad;
    u32 savedButtons = kcode[0];
    s16 savedX = joyx[0], savedY = joyy[0], savedRightX = joyrx[0], savedRightY = joyry[0];
    u16 savedLeft = lt[0], savedRight = rt[0];

    void SetUp() override {
        ASSERT_EQ(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER), 0) << SDL_GetError();
        initialized = true;
        joystickIndex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
        ASSERT_GE(joystickIndex, 0) << SDL_GetError();
        char guid[33]{};
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(joystickIndex), guid, sizeof(guid));
        const std::string layout = std::string(guid) +
            ",Hikaru Preset Regression,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
            "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
            "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
        ASSERT_GE(SDL_GameControllerAddMapping(layout.c_str()), 0) << SDL_GetError();
        auto joystick = SDL_JoystickOpen(joystickIndex);
        ASSERT_NE(joystick, nullptr) << SDL_GetError();
        pad = std::make_shared<PresetSDLGamepad>(0, joystickIndex, joystick);
        SDLGamepad::AddSDLGamepad(pad);
        auto saved = pad->get_input_mapping();
        // Reproduce the ES-DE map with pedal axes assigned to buttons.
        saved->set_button(DC_BTN_C, 0);
        saved->set_axis(DC_BTN_A, 4, true);
        saved->set_axis(DC_BTN_B, 5, true);
        saved->dead_zone = 0.f;
        saved->saturation = 1.f;
        kcode[0] = ~0u;
        joyx[0] = joyy[0] = joyrx[0] = joyry[0] = 0;
        lt[0] = rt[0] = 0;
    }
    void TearDown() override {
        if (pad) pad->close();
        if (joystickIndex >= 0) SDL_JoystickDetachVirtual(joystickIndex);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        kcode[0] = savedButtons;
        joyx[0] = savedX; joyy[0] = savedY;
        joyrx[0] = savedRightX; joyry[0] = savedRightY;
        lt[0] = savedLeft; rt[0] = savedRight;
    }
    std::array<hikaru::ControllerInputs, 2> controllers() const {
        return {{{kcode[0], joyx[0], joyy[0], lt[0], rt[0], joyrx[0], joyry[0]}, {}}};
    }
};
}
TEST_F(HikaruGamepadPresetTest, RestoresAnalogPedalsAndAllNascarGearsThroughSdlInput) {
    const auto saved = pad->get_input_mapping();
    pad->gamepad_axis_input(5, 32767);
    ASSERT_EQ(kcode[0] & DC_BTN_B, 0u);
    pad->setGamepadPreset(true);
    pad->gamepad_axis_input(4, -32768);
    EXPECT_NE(kcode[0] & DC_BTN_B, 0u);
    pad->gamepad_axis_input(5, 32767);
    auto input = hikaru::bindCabinetInputs(&hikaru::SegaNascarGame, controllers(), false, false);
    EXPECT_EQ(input.analog[1], 0xffff);
    EXPECT_EQ(input.analog[2], 0);
    pad->gamepad_axis_input(4, 32767);
    pad->gamepad_axis_input(5, -32768);
    input = hikaru::bindCabinetInputs(&hikaru::SegaNascarGame, controllers(), false, false);
    EXPECT_EQ(input.analog[1], 0);
    EXPECT_EQ(input.analog[2], 0xffff);
    const uint16_t gears[] = {0x2800, 0x1800, 0x2000, 0x1000};
    for (u32 button = 0; button < 4; ++button) {
        pad->gamepad_btn_input(button, true);
        input = hikaru::bindCabinetInputs(&hikaru::SegaNascarGame, controllers(), false, false);
        EXPECT_EQ(input.players[1], gears[button]);
        pad->gamepad_btn_input(button, false);
    }
    EXPECT_EQ(pad->get_input_mapping(), saved);
    EXPECT_EQ(saved->get_axis_id(0, 5, true), DC_BTN_B);
}
TEST_F(HikaruGamepadPresetTest, DisablingReleasesHeldInputsAndRestoresTheSavedMapping) {
    pad->setGamepadPreset(true);
    pad->gamepad_btn_input(0, true);
    pad->gamepad_axis_input(5, 32767);
    ASSERT_EQ(kcode[0] & DC_BTN_A, 0u);
    ASSERT_EQ(rt[0], 0xffff);
    pad->setGamepadPreset(false);
    pad->gamepad_btn_input(0, false);
    EXPECT_NE(kcode[0] & DC_BTN_A, 0u);
    EXPECT_EQ(rt[0], 0);
    pad->gamepad_btn_input(0, true);
    EXPECT_EQ(kcode[0] & DC_BTN_C, 0u);
    EXPECT_NE(kcode[0] & DC_BTN_A, 0u);
    pad->gamepad_btn_input(0, false);
}
TEST_F(HikaruGamepadPresetTest, RoutesCoinStartAndTestWithoutAProfileFile) {
    pad->setGamepadPreset(true);
    pad->gamepad_btn_input(7, true);
    pad->gamepad_btn_input(6, true);
    pad->gamepad_btn_input(8, true);
    auto input = hikaru::bindCabinetInputs(&hikaru::SegaNascarGame, controllers(), false, false);
    EXPECT_TRUE(input.coin[0]);
    EXPECT_TRUE(input.test);
    EXPECT_EQ(input.players[0] & 0x8000, 0x8000);
    pad->gamepad_btn_input(7, false);
    pad->gamepad_btn_input(6, false);
    pad->gamepad_btn_input(8, false);
}
TEST_F(HikaruGamepadPresetTest, SupportsTheExistingAllPortsAssignment) {
    pad->set_maple_port(4);
    pad->setGamepadPreset(true);
    pad->gamepad_axis_input(5, 32767);
    EXPECT_EQ(rt[0], 0xffff);
    pad->gamepad_btn_input(7, true);
    EXPECT_EQ(kcode[0] & DC_BTN_D, 0u);
    pad->setGamepadPreset(false);
    pad->gamepad_btn_input(7, false);
    EXPECT_EQ(rt[0], 0);
    EXPECT_NE(kcode[0] & DC_BTN_D, 0u);
}
#endif
#endif
#endif
