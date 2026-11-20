/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "remote_gamepad.h"

#include <cmath>

RemoteGamepad::RemoteGamepad(int maple_port)
	: GamepadDevice(maple_port, "remoteplay", false)
{
	refreshName();
	_unique_id = "remote_gamepad_uid" + std::to_string(maple_port);
	input_mapper = std::make_shared<IdentityInputMapping>();
	input_mapper->addTrigger(DC_AXIS_LT, false);
	input_mapper->addTrigger(DC_AXIS_RT, false);
}

void RemoteGamepad::buttonInput(u32 key, bool pressed)
{
	if (key == 0)
		return;
	if (key <= DC_BTN_BITMAPPED_LAST)
	{
		if (pressed)
			buttonState |= key;
		else
			buttonState &= ~key;
	}
	gamepad_btn_input(key, pressed);
}

void RemoteGamepad::triggerInput(u32 key, float value)
{
	// Map 0..1 onto the SDL-style axis range used by GamepadDevice (-32768..32767).
	int v = (int)std::round(std::max(0.f, std::min(1.f, value)) * 65535.f) - 32768;
	gamepad_axis_input(key, v);
}

void RemoteGamepad::joystickInput(float x, float y)
{
	// Keep the stick inside the Dreamcast stick's circular gate.
	float r2 = x * x + y * y;
	if (r2 > 1.f)
	{
		float scale = 1.f / std::sqrt(r2);
		x *= scale;
		y *= scale;
	}
	int joyx = (int)std::round(x * 32767.f);
	int joyy = (int)std::round(y * 32767.f);
	if (joyx >= 0)
		gamepad_axis_input(DC_AXIS_RIGHT, joyx);
	else
		gamepad_axis_input(DC_AXIS_LEFT, -joyx);
	if (joyy >= 0)
		gamepad_axis_input(DC_AXIS_DOWN, joyy);
	else
		gamepad_axis_input(DC_AXIS_UP, -joyy);
}

void RemoteGamepad::releaseAll()
{
	for (int i = 0; i <= 17; i++)
		if (buttonState & (1u << i))
			gamepad_btn_input(1u << i, false);
	buttonState = 0;
	joystickInput(0, 0);
	gamepad_axis_input(DC_AXIS_LT, -32768);
	gamepad_axis_input(DC_AXIS_RT, -32768);
}

void RemoteGamepad::applyEvent(const remoteplay::RemoteInputEvent& event)
{
	using Type = remoteplay::RemoteInputEvent::Type;
	switch (event.type)
	{
	case Type::Button: buttonInput(event.key, event.pressed); break;
	case Type::Trigger: triggerInput(event.key, event.value); break;
	case Type::Stick: joystickInput(event.x, event.y); break;
	case Type::ReleaseAll: releaseAll(); break;
	case Type::Ping: break;
	}
}
