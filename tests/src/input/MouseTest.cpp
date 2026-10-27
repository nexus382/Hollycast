#include "gtest/gtest.h"

#include "cfg/option.h"
#include "input/mouse.h"

TEST(MouseTest, RelativeMotionRebasesNextAbsoluteEvent)
{
	const int sensitivity = config::MouseSensitivity;
	const int stretching = config::ScreenStretching;
	const bool rotate = config::Rotate90;
	config::MouseSensitivity = 100;
	config::ScreenStretching = 100;
	config::Rotate90 = false;
	for (unsigned port = 0; port < 4; ++port)
	{
		mo_x_delta[port] = mo_y_delta[port] = mo_wheel_delta[port] = 0;
		mo_x_prev[port] = mo_y_prev[port] = -1;
		mo_x_abs[port] = mo_y_abs[port] = 0;
	}

	SetMousePosition(100, 100, 640, 480);
	SetRelativeMousePosition(100, 40);
	mo_x_delta[0] = mo_y_delta[0] = 0; // The guest consumed the captured movement.
	SetMousePosition(200, 140, 640, 480);
	EXPECT_FLOAT_EQ(mo_x_delta[0], 0);
	EXPECT_FLOAT_EQ(mo_y_delta[0], 0);
	SetMousePosition(201, 138, 640, 480);
	EXPECT_FLOAT_EQ(mo_x_delta[0], 1);
	EXPECT_FLOAT_EQ(mo_y_delta[0], -2);

	for (unsigned port = 0; port < 4; ++port)
	{
		mo_x_delta[port] = mo_y_delta[port] = mo_wheel_delta[port] = 0;
		mo_x_prev[port] = mo_y_prev[port] = -1;
		mo_x_abs[port] = mo_y_abs[port] = 0;
	}
	config::MouseSensitivity = sensitivity;
	config::ScreenStretching = stretching;
	config::Rotate90 = rotate;
}
