// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "input_bindings.h"
#include "roms.h"
#include "input/gamepad.h"

namespace hikaru {
HikaruEngineInputs bindCabinetInputs(const GameDescriptor* game,
    const std::array<ControllerInputs, 2>& controllers,
    bool airTrixDpadBoard, bool planetDpadStick, bool virtualOn4TwinStick,
    bool nascarGamepad, uint16_t previousNascarGear, bool starWarsGamepad)
{
    HikaruEngineInputs input{};
    for (auto& axis : input.analog)
        axis = 0x8000;
    if (game == &StarWarsRacerGame) {
        const auto& pad = controllers[0];
        const auto pressed = [&](uint32_t mask) { return (pad.buttons & mask) == 0; };
        // The cabinet has two independent levers: analog 0/1, low when pushed
        // forward. Preserve the signed stick endpoints and their neutral center.
        input.analog[0] = uint16_t(int(pad.stickY) + 32768);
        input.analog[1] = uint16_t(int(pad.rightY) + 32768);
        const bool boost = pressed(DC_BTN_A) || (starWarsGamepad && pad.leftTrigger >= 0x8000);
        const bool brake = pressed(DC_BTN_B) || (starWarsGamepad && pad.rightTrigger >= 0x8000);
        const bool left = pressed(DC_BTN_C) || (starWarsGamepad && (pressed(DC_BTN_Z) || pressed(DC_DPAD_LEFT)));
        const bool right = pressed(DC_BTN_X) || (starWarsGamepad && (pressed(DC_BTN_Y) || pressed(DC_DPAD_RIGHT)));
        // The left button is wired to P1 Right and the right button to P1 Left.
        input.players[0] = (pressed(DC_BTN_START) ? 0x8000 : 0)
            | (boost ? 0x0200 : 0) | (brake ? 0x0100 : 0)
            | (left ? 0x0400 : 0) | (right ? 0x0800 : 0);
        input.coin[0] = pressed(DC_BTN_D);
        input.test = pressed(DC_DPAD2_DOWN);
        input.service = pressed(DC_DPAD2_UP);
        return input;
    }
    if (game == &SegaNascarGame) {
        const auto& pad = controllers[0];
        const auto pressed = [&](uint32_t mask) { return (pad.buttons & mask) == 0; };
        // Standard cabinet wiring: wheel/gas/brake occupy analog 0/1/2.
        // View is P1 Down; the three shifter switches occupy P2 Left/Up/Down.
        // Deluxe has different View/presence wiring and reversed steering.
        input.players[0] = (pressed(DC_BTN_START) ? 0x8000 : 0) | (pressed(DC_BTN_Y) ? 0x1000 : 0);
        input.coin[0] = pressed(DC_BTN_D);
        input.test = pressed(DC_DPAD2_DOWN);
        input.service = pressed(DC_DPAD2_UP);
        input.analog[0] = uint16_t(int(pad.stickX) + 32768);
        input.analog[1] = pad.rightTrigger;
        input.analog[2] = pad.leftTrigger;
        if (nascarGamepad) {
            const bool left = pressed(DC_DPAD_LEFT), right = pressed(DC_DPAD_RIGHT);
            if (left != right) input.analog[0] = left ? 0 : 0xffff;
            // The physical shifter stays in its notch after the hand releases.
            // Reuse the serialized MIE switch snapshot so load/reset retains it.
            switch (previousNascarGear) {
            case 0x2800: case 0x1800: case 0x2000: case 0x1000:
                input.players[1] = previousNascarGear;
                break;
            default: break;
            }
        }
        const unsigned gearButtons = (pressed(DC_BTN_A) ? 1u : 0u) | (pressed(DC_BTN_B) ? 2u : 0u)
            | (pressed(DC_BTN_C) ? 4u : 0u) | (pressed(DC_BTN_X) ? 8u : 0u);
        if (pressed(DC_BTN_Z)) input.players[1] = 0;
        else switch (gearButtons) {
        case 1: input.players[1] = 0x2800; break; // First: left + up.
        case 2: input.players[1] = 0x1800; break; // Second: left + down.
        case 4: input.players[1] = 0x2000; break; // Third: up.
        case 8: input.players[1] = 0x1000; break; // Fourth: down.
        default: break; // Simultaneous selections must not invent a gear.
        }
        return input;
    }
    if (game == &VirtualOn4Game) {
        // One cabinet's left/right digital levers occupy JVS player slots 1/2.
        // They are not two players. Convert the mapped axes after the host's
        // dead-zone handling; neutral/opposing D-pad inputs do not force motion.
        for (unsigned lever = 0; lever < 2; ++lever) {
            const auto& pad = controllers[virtualOn4TwinStick ? 0 : lever];
            const bool right = virtualOn4TwinStick && lever == 1;
            const int x = right ? pad.rightX : pad.stickX, y = right ? pad.rightY : pad.stickY;
            const auto pressed = [&](uint32_t mask) { return (pad.buttons & mask) == 0; };
            const bool up = !right && pressed(DC_DPAD_UP), down = !right && pressed(DC_DPAD_DOWN);
            const bool left = !right && pressed(DC_DPAD_LEFT), rightDpad = !right && pressed(DC_DPAD_RIGHT);
            input.players[lever] = ((up != down ? up : y <= -16384) ? 0x2000 : 0)
                | ((up != down ? down : y >= 16384) ? 0x1000 : 0)
                | ((left != rightDpad ? left : x <= -16384) ? 0x0800 : 0)
                | ((left != rightDpad ? rightDpad : x >= 16384) ? 0x0400 : 0);
            const bool fire = right ? pressed(DC_BTN_B) || pad.rightTrigger >= 0x8000
                : pressed(DC_BTN_A) || pad.leftTrigger >= 0x8000;
            const bool turbo = right ? pressed(DC_BTN_X) : pressed(DC_BTN_C) || pressed(DC_BTN_Z);
            input.players[lever] |= (fire ? 0x0200 : 0) | (turbo ? 0x0100 : 0);
        }
        const auto buttons = controllers[0].buttons;
        input.players[0] |= (!(buttons & DC_BTN_START) ? 0x8000 : 0) | (!(buttons & DC_BTN_Y) ? 0x0080 : 0);
        input.coin[0] = !(buttons & DC_BTN_D) || (!virtualOn4TwinStick && !(controllers[1].buttons & DC_BTN_D));
        input.test = !(buttons & DC_DPAD2_DOWN);
        input.service = !(buttons & DC_DPAD2_UP);
        return input;
    }
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
                if (game == &BraveFirefightersGame && pressed(DC_BTN_B))
                    input.players[i] |= 0x0080; // Fog uses both push-2 positions, as the mouse path does
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
