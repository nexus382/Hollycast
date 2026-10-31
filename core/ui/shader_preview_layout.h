/*
 * Portions Copyright 2026 The Hollycast Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once
#include <algorithm>

namespace SettingsNew {

struct ShaderPreviewLayout
{
	int gameWidth;
	int gameHeight;
	int controlsX;
	int controlsY;
	int controlsWidth;
	int controlsHeight;
};

// Both the UI and presenters use this partition. The game keeps its own aspect
// ratio inside its pane, so editing never hides part of the image behind controls.
inline ShaderPreviewLayout shaderPreviewLayout(int width, int height, int topInset)
{
	width = std::max(0, width);
	height = std::max(0, height);
	topInset = std::clamp(topInset, 0, height);
	const int availableHeight = height - topInset;
	if (static_cast<double>(width) >= availableHeight * 1.2)
	{
		const int gameWidth = static_cast<int>(width * .58);
		return { gameWidth, availableHeight, gameWidth, topInset, width - gameWidth, availableHeight };
	}
	const int gameHeight = static_cast<int>(availableHeight * .45);
	return { width, gameHeight, 0, topInset + gameHeight, width, availableHeight - gameHeight };
}

} // namespace SettingsNew
