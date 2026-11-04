// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "input_bindings.h"
#include "roms.h"
#include "input/gamepad.h"

namespace hikaru {
HikaruEngineInputs bindCabinetInputs(const GameDescriptor* game,
    const std::array<ControllerInputs, 2>& controllers,
    bool airTrixDpadBoard, bool planetDpadStick)
{
    HikaruEngineInputs input{};
    for (auto& axis : input.analog)
        axis = 0x8000;
    if (game != &AirTrixGame && game != &BraveFirefightersGame && game != &PlanetHarriersGame)
        return input;

    for (unsigned i = 0; i < controllers.size(); ++i) {
        const auto& controller = controllers[i];
        auto pressed = [&](uint32_t mask) { return (controller.buttons & mask) == 0; };
        input.players[i] = pressed(DC_BTN_START) ? 0x8000 : 0;
        input.coin[i] = pressed(DC_BTN_D);
        if (game == &AirTrixGame) {
            // Sega's UP/DOWN switches are nose/tail kicks, independent of the
            // analog board yaw/roll. Standard action buttons must reach them.
            input.players[i] |= (pressed(DC_BTN_A) ? 0x1000 : 0)
                | (pressed(DC_BTN_B) ? 0x2000 : 0)
                | ((pressed(DC_BTN_C) || controller.leftTrigger >= 0x8000 || pressed(DC_DPAD_LEFT)) ? 0x0200 : 0)
                | ((pressed(DC_BTN_X) || controller.rightTrigger >= 0x8000 || pressed(DC_DPAD_RIGHT)) ? 0x0100 : 0);
            if (!airTrixDpadBoard)
                input.players[i] |= (pressed(DC_DPAD_UP) ? 0x2000 : 0) | (pressed(DC_DPAD_DOWN) ? 0x1000 : 0);
            // C/X now have cabinet actions. Keep a separate credit alias on
            // button 5 (right shoulder in the default SDL arcade pad layout).
            input.coin[i] = input.coin[i] || pressed(DC_BTN_Y);
        } else {
            input.players[i] |= (pressed(DC_BTN_A) ? 0x0200 : 0)
                | (pressed(DC_BTN_B) ? 0x0100 : 0) | (pressed(DC_DPAD_UP) ? 0x2000 : 0)
                | (pressed(DC_DPAD_DOWN) ? 0x1000 : 0) | (pressed(DC_DPAD_LEFT) ? 0x0800 : 0)
                | (pressed(DC_DPAD_RIGHT) ? 0x0400 : 0);
            if (game == &PlanetHarriersGame) {
                // Match Naomi's button order; LT retains the keyboard F alias.
                input.players[i] |= ((pressed(DC_BTN_C) || controller.leftTrigger >= 0x8000) ? 0x0080 : 0)
                    | (pressed(DC_BTN_X) ? 0x0040 : 0) | (pressed(DC_BTN_Y) ? 0x0020 : 0);
            } else {
                input.coin[i] = input.coin[i] || pressed(DC_BTN_C) || pressed(DC_BTN_X);
            }
        }
    }
    const auto& controller = controllers[0];
    input.test = (controller.buttons & DC_DPAD2_DOWN) == 0;
    input.service = (controller.buttons & DC_DPAD2_UP) == 0;
    input.analog[0] = uint16_t(int(controller.stickX) + 32768);
    input.analog[1] = uint16_t(int(controller.stickY) + 32768);
    if (game == &BraveFirefightersGame) {
        input.analog[2] = uint16_t(int(controllers[1].stickX) + 32768);
        input.analog[3] = uint16_t(int(controllers[1].stickY) + 32768);
    } else if (game == &PlanetHarriersGame) {
        // One stick per cabinet. Planet wires vertical first, with low = down.
        input.analog[0] = uint16_t(int(controller.stickY) + 32768);
        input.analog[1] = uint16_t(int(controller.stickX) + 32768);
        if (planetDpadStick) {
            if (!(controller.buttons & DC_DPAD_LEFT)) input.analog[1] = 0;
            else if (!(controller.buttons & DC_DPAD_RIGHT)) input.analog[1] = 0xffff;
            if (!(controller.buttons & DC_DPAD_UP)) input.analog[0] = 0xffff;
            else if (!(controller.buttons & DC_DPAD_DOWN)) input.analog[0] = 0;
        }
    } else if (airTrixDpadBoard) {
        // Both board axes must be available independently of the kick buttons.
        // Opposing D-pad directions cancel, leaving the mapped analog axis.
        const bool left = !(controller.buttons & DC_DPAD_LEFT), right = !(controller.buttons & DC_DPAD_RIGHT);
        const bool up = !(controller.buttons & DC_DPAD_UP), down = !(controller.buttons & DC_DPAD_DOWN);
        if (left != right) input.analog[0] = left ? 0 : 0xffff;
        if (up != down) input.analog[1] = up ? 0 : 0xffff;
    }
    return input;
}
} // namespace hikaru
