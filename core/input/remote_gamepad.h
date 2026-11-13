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
#pragma once
#include "gamepad_device.h"
#include "ui/remoteplay/remote_play_protocol.h"

// A phone connected through Remote Play, acting as a Dreamcast controller
// on one maple port (0-3). Unlike VirtualGamepad this is not mobile-only:
// the phone is the screen, this desktop process is the host.
class RemoteGamepad : public GamepadDevice
{
public:
	RemoteGamepad(int maple_port);

	void refreshName() override { _name = "Remote Play Phone"; }
	bool is_virtual_gamepad() override { return true; }

	void applyEvent(const remoteplay::RemoteInputEvent& event);
	void buttonInput(u32 key, bool pressed);
	void triggerInput(u32 key, float value);	// value 0..1
	void joystickInput(float x, float y);		// -1..1
	void releaseAll();

private:
	u32 buttonState = 0;
};
