// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "valkyrie/engine.h"
#include <array>
#include <cstdint>

namespace hikaru {
struct GameDescriptor;

// Snapshot of the existing controller mapping, before cabinet-specific wiring.
// Buttons are active-low, matching kcode; axes retain the full signed range.
struct ControllerInputs {
    uint32_t buttons = UINT32_MAX;
    int16_t stickX = 0, stickY = 0;
    uint16_t leftTrigger = 0, rightTrigger = 0;
    int16_t rightX = 0, rightY = 0;
};

// Only the loaded title consumes its compatibility option. Coin pulse shaping
// and Brave's pointer overlay remain frontend responsibilities.
HikaruEngineInputs bindCabinetInputs(const GameDescriptor* game,
    const std::array<ControllerInputs, 2>& controllers,
    bool airTrixDpadBoard, bool planetDpadStick, bool virtualOn4TwinStick = true);
} // namespace hikaru
