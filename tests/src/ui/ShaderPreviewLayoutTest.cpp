/*
 * Portions Copyright 2026 The Hollycast Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ui/shader_preview_layout.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <tuple>

class ShaderPreviewLayoutTest : public ::testing::TestWithParam<std::tuple<int, int, int>> {};

TEST_P(ShaderPreviewLayoutTest, KeepsImageAndControlsInsideSeparatePanes)
{
	const auto [width, height, inset] = GetParam();
	const int screenWidth = std::max(0, width);
	const int screenHeight = std::max(0, height);
	const int topInset = std::clamp(inset, 0, screenHeight);
	const auto layout = SettingsNew::shaderPreviewLayout(width, height, inset);
	EXPECT_GE(layout.gameWidth, 0);
	EXPECT_GE(layout.gameHeight, 0);
	EXPECT_LE(layout.gameWidth, screenWidth);
	EXPECT_LE(topInset + layout.gameHeight, screenHeight);
	EXPECT_GE(layout.controlsX, 0);
	EXPECT_GE(layout.controlsY, topInset);
	EXPECT_GE(layout.controlsWidth, 0);
	EXPECT_GE(layout.controlsHeight, 0);
	EXPECT_EQ(screenWidth, layout.controlsX + layout.controlsWidth);
	EXPECT_EQ(screenHeight, layout.controlsY + layout.controlsHeight);
	EXPECT_TRUE(layout.gameWidth <= layout.controlsX || topInset + layout.gameHeight <= layout.controlsY);
	EXPECT_EQ(static_cast<int64_t>(screenWidth) * (screenHeight - topInset),
			static_cast<int64_t>(layout.gameWidth) * layout.gameHeight
					+ static_cast<int64_t>(layout.controlsWidth) * layout.controlsHeight);
}

INSTANTIATE_TEST_SUITE_P(WindowSizes, ShaderPreviewLayoutTest, ::testing::Values(
		std::make_tuple(1280, 720, 32), std::make_tuple(800, 600, 32),
		std::make_tuple(480, 800, 28), std::make_tuple(240, 320, 20),
		std::make_tuple(3840, 2160, 64), std::make_tuple(2560, 1080, 30),
		std::make_tuple(800, 1280, 48), std::make_tuple(320, 240, 20),
		std::make_tuple(1, 1, 0), std::make_tuple(0, 0, 0),
		std::make_tuple(-1, 10, 20), std::make_tuple(640, 480, 900),
		std::make_tuple(640, 480, -50)));

TEST(ShaderPreviewLayout, UsesSideBySideLandscapeAndStackedPortrait)
{
	const auto landscape = SettingsNew::shaderPreviewLayout(1280, 720, 32);
	EXPECT_EQ(landscape.gameWidth, landscape.controlsX);
	EXPECT_EQ(32, landscape.controlsY);
	const auto portrait = SettingsNew::shaderPreviewLayout(480, 800, 28);
	EXPECT_EQ(0, portrait.controlsX);
	EXPECT_EQ(480, portrait.gameWidth);
	EXPECT_EQ(28 + portrait.gameHeight, portrait.controlsY);
}

TEST(ShaderPreviewLayout, KeepsUiAndFramebufferPartitionsAlignedAtHighDpi)
{
	const auto ui = SettingsNew::shaderPreviewLayout(800, 600, 32);
	const auto framebuffer = SettingsNew::shaderPreviewLayout(1600, 1200, 64);
	EXPECT_NEAR(ui.gameWidth * 2, framebuffer.gameWidth, 1);
	EXPECT_NEAR(ui.gameHeight * 2, framebuffer.gameHeight, 1);
	EXPECT_NEAR(ui.controlsX * 2, framebuffer.controlsX, 1);
	EXPECT_NEAR(ui.controlsY * 2, framebuffer.controlsY, 1);
}
