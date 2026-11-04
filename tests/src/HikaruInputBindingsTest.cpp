// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "hw/hikaru/input_bindings.h"
#include "hw/hikaru/roms.h"
#include "input/gamepad.h"
#include <utility>

namespace {
using namespace hikaru;
class HikaruInputBindingsTest : public ::testing::Test {
protected:
    std::array<ControllerInputs, 2> controllers{};
    HikaruEngineInputs bind(const GameDescriptor* game, bool airDpad = true, bool planetDpad = true) {
        return bindCabinetInputs(game, controllers, airDpad, planetDpad);
    }
};

TEST_F(HikaruInputBindingsTest, NeutralAndUnloadedCabinetsHaveCenteredAxes)
{
    for (const auto* game : {static_cast<const GameDescriptor*>(nullptr), &AirTrixGame, &BraveFirefightersGame, &PlanetHarriersGame}) {
        const auto input = bind(game);
        for (const auto player : input.players) EXPECT_EQ(player, 0);
        for (const auto coin : input.coin) EXPECT_EQ(coin, 0);
        for (const auto axis : input.analog) EXPECT_EQ(axis, 0x8000);
        EXPECT_EQ(input.test, 0);
        EXPECT_EQ(input.service, 0);
    }
    controllers[0].buttons = 0;
    controllers[0].stickX = -32768;
    const auto unloaded = bind(nullptr);
    EXPECT_EQ(unloaded.players[0], 0);
    EXPECT_EQ(unloaded.coin[0], 0);
    EXPECT_EQ(unloaded.test, 0);
    EXPECT_EQ(unloaded.analog[0], 0x8000);
}

TEST_F(HikaruInputBindingsTest, AirTrixKicksAreIndependentOfBoardMotionAndCoins)
{
    for (const auto binding : {std::pair<uint32_t, uint16_t>{DC_BTN_A, 0x1000}, {DC_BTN_B, 0x2000}}) {
        controllers[0].buttons = ~binding.first;
        auto input = bind(&AirTrixGame);
        EXPECT_EQ(input.players[0], binding.second);
        EXPECT_EQ(input.coin[0], 0);
        EXPECT_EQ(input.analog[0], 0x8000);
        EXPECT_EQ(input.analog[1], 0x8000);
        controllers[0].buttons &= ~DC_DPAD_LEFT;
        input = bind(&AirTrixGame);
        EXPECT_EQ(input.players[0], binding.second | 0x0200);
        EXPECT_EQ(input.analog[0], 0);
    }
}

TEST_F(HikaruInputBindingsTest, AirTrixCabinetButtonsDoNotInsertCredits)
{
    for (const auto binding : {std::pair<uint32_t, uint16_t>{DC_BTN_C, 0x0200}, {DC_BTN_X, 0x0100}}) {
        controllers[0].buttons = ~binding.first;
        const auto input = bind(&AirTrixGame);
        EXPECT_EQ(input.players[0], binding.second);
        EXPECT_EQ(input.coin[0], 0);
    }
    for (const auto coin : {DC_BTN_D, DC_BTN_Y}) {
        controllers[0].buttons = ~uint32_t(coin);
        const auto input = bind(&AirTrixGame);
        EXPECT_EQ(input.coin[0], 1);
        EXPECT_EQ(input.players[0], 0);
    }
}

TEST_F(HikaruInputBindingsTest, AirTrixDpadCanMoveBothAxesWithoutKicking)
{
    controllers[0].buttons = ~(DC_DPAD_LEFT | DC_DPAD_UP);
    auto input = bind(&AirTrixGame);
    EXPECT_EQ(input.analog[0], 0);
    EXPECT_EQ(input.analog[1], 0);
    EXPECT_EQ(input.players[0], 0x0200);
    controllers[0].buttons = ~(DC_DPAD_RIGHT | DC_DPAD_DOWN);
    input = bind(&AirTrixGame);
    EXPECT_EQ(input.analog[0], 0xffff);
    EXPECT_EQ(input.analog[1], 0xffff);
    EXPECT_EQ(input.players[0], 0x0100);
    controllers[0].buttons = ~(DC_DPAD_LEFT | DC_DPAD_RIGHT | DC_DPAD_UP | DC_DPAD_DOWN);
    input = bind(&AirTrixGame);
    EXPECT_EQ(input.analog[0], 0x8000);
    EXPECT_EQ(input.analog[1], 0x8000);
}

TEST_F(HikaruInputBindingsTest, DisablingAirDpadBoardPreservesOriginalKickSwitches)
{
    controllers[0].stickX = 123;
    controllers[0].stickY = -456;
    for (const auto binding : {std::pair<uint32_t, uint16_t>{DC_DPAD_UP, 0x2000}, {DC_DPAD_DOWN, 0x1000}}) {
        controllers[0].buttons = ~binding.first;
        const auto input = bind(&AirTrixGame, false);
        EXPECT_EQ(input.players[0], binding.second);
        EXPECT_EQ(input.analog[0], 32768 + 123);
        EXPECT_EQ(input.analog[1], 32768 - 456);
    }
}

TEST_F(HikaruInputBindingsTest, TriggerAliasesHaveAnExplicitHalfRangeThreshold)
{
    controllers[0].leftTrigger = 0x7fff;
    controllers[0].rightTrigger = 0x7fff;
    EXPECT_EQ(bind(&AirTrixGame).players[0], 0);
    EXPECT_EQ(bind(&PlanetHarriersGame).players[0], 0);
    controllers[0].leftTrigger = 0x8000;
    EXPECT_EQ(bind(&AirTrixGame).players[0], 0x0200);
    EXPECT_EQ(bind(&PlanetHarriersGame).players[0], 0x0080);
    controllers[0].rightTrigger = 0x8000;
    EXPECT_EQ(bind(&AirTrixGame).players[0], 0x0300);
    EXPECT_EQ(bind(&BraveFirefightersGame).players[0], 0);
}

TEST_F(HikaruInputBindingsTest, BraveRetainsBothPlayersAimAndUnusedButtonCredits)
{
    controllers[0].stickX = -32768;
    controllers[0].stickY = 32767;
    controllers[1].stickX = 32767;
    controllers[1].stickY = -32768;
    controllers[1].buttons = ~(DC_BTN_START | DC_BTN_A | DC_BTN_B | DC_BTN_C | DC_BTN_X);
    const auto input = bind(&BraveFirefightersGame);
    EXPECT_EQ(input.players[0], 0);
    EXPECT_EQ(input.coin[0], 0);
    EXPECT_EQ(input.players[1], 0x8300);
    EXPECT_EQ(input.coin[1], 1);
    EXPECT_EQ(input.analog[0], 0);
    EXPECT_EQ(input.analog[1], 0xffff);
    EXPECT_EQ(input.analog[2], 0xffff);
    EXPECT_EQ(input.analog[3], 0);
}

TEST_F(HikaruInputBindingsTest, PlanetRetainsAllFiveActionsAndReversedAxisWiring)
{
    controllers[0].buttons = ~(DC_BTN_A | DC_BTN_B | DC_BTN_C | DC_BTN_X | DC_BTN_Y);
    controllers[0].stickX = -32768;
    controllers[0].stickY = 32767;
    const auto input = bind(&PlanetHarriersGame);
    EXPECT_EQ(input.players[0], 0x03e0);
    EXPECT_EQ(input.coin[0], 0);
    EXPECT_EQ(input.analog[0], 0xffff);
    EXPECT_EQ(input.analog[1], 0);
    EXPECT_EQ(input.analog[2], 0x8000);
}

TEST_F(HikaruInputBindingsTest, PlanetDpadOptionIsLocalToItsCabinet)
{
    controllers[0].buttons = ~(DC_DPAD_LEFT | DC_DPAD_UP);
    auto input = bind(&PlanetHarriersGame, false, true);
    EXPECT_EQ(input.analog[0], 0xffff);
    EXPECT_EQ(input.analog[1], 0);
    EXPECT_EQ(input.players[0], 0x2800);
    input = bind(&PlanetHarriersGame, true, false);
    EXPECT_EQ(input.analog[0], 0x8000);
    EXPECT_EQ(input.analog[1], 0x8000);
    EXPECT_EQ(input.players[0], 0x2800);
    input = bind(&BraveFirefightersGame, true, true);
    EXPECT_EQ(input.analog[0], 0x8000);
    EXPECT_EQ(input.analog[1], 0x8000);
    EXPECT_EQ(input.players[0], 0x2800);
}

TEST_F(HikaruInputBindingsTest, StartOperatorAndCoinInputsRemainAvailableForEveryTitle)
{
    controllers[0].buttons = ~(DC_BTN_START | DC_DPAD2_UP | DC_DPAD2_DOWN | DC_BTN_D);
    controllers[1].buttons = ~(DC_BTN_START | DC_BTN_D);
    for (const auto* game : {&AirTrixGame, &BraveFirefightersGame, &PlanetHarriersGame}) {
        const auto input = bind(game);
        EXPECT_EQ(input.players[0], 0x8000);
        EXPECT_EQ(input.players[1], 0x8000);
        EXPECT_EQ(input.test, 1);
        EXPECT_EQ(input.service, 1);
        EXPECT_EQ(input.coin[0], 1);
        EXPECT_EQ(input.coin[1], 1);
    }
}
} // namespace
