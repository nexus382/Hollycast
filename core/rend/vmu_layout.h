// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace vmu_window {
struct Position { float x = 0.f, y = 0.f; };
struct Bounds { float x = 0.f, y = 0.f, width = 0.f, height = 0.f; };

// In-app layout uses fractions of the available travel, so a saved position is
// still reachable after rotation, a cutout change or resizing in split-screen.
// The Android backend serializes access from the render and Java UI threads.
class TouchLayout
{
public:
	void setMovable(bool enabled)
	{
		movable = enabled;
		if (!movable)
			end(); // Locking mid-drag commits the last position without hiding the LCD.
	}
	void configure(float width, float height, Bounds safeArea, float lcdWidth)
	{
		const float size = std::max(0.f, std::min({lcdWidth, safeArea.width, safeArea.height * 1.5f}));
		if (width != viewWidth || height != viewHeight || safeArea.x != area.x || safeArea.y != area.y
				|| safeArea.width != area.width || safeArea.height != area.height || size != screenWidth)
			end(); // A rotation must not reinterpret an old finger offset in the new layout.
		viewWidth = width;
		viewHeight = height;
		area = safeArea;
		screenWidth = size;
	}

	Bounds bounds(int index) const
	{
		const Position& pos = positions[index];
		return {area.x + pos.x * std::max(0.f, area.width - screenWidth),
				area.y + pos.y * std::max(0.f, area.height - screenWidth / 1.5f),
				screenWidth, screenWidth / 1.5f};
	}

	void setVisible(unsigned mask)
	{
		visible = mask;
		if (dragged >= 0 && (visible & (1u << dragged)) == 0)
			end();
	}

	bool begin(float x, float y)
	{
		if (!movable || !std::isfinite(x) || !std::isfinite(y) || viewWidth <= 0 || viewHeight <= 0 || screenWidth <= 0)
			return false;
		x *= viewWidth;
		y *= viewHeight;
		// Last drawn LCD wins when screens overlap.
		for (int i = 7; i >= 0; --i)
		{
			const Bounds rect = bounds(i);
			if ((visible & (1u << i)) != 0 && x >= rect.x && x < rect.x + rect.width
					&& y >= rect.y && y < rect.y + rect.height)
			{
				end();
				dragged = i;
				offset = {x - rect.x, y - rect.y};
				return true;
			}
		}
		return false;
	}

	void move(float x, float y)
	{
		if (dragged < 0 || !std::isfinite(x) || !std::isfinite(y))
			return;
		Position& pos = positions[dragged];
		const float travelX = area.width - screenWidth;
		const float travelY = area.height - screenWidth / 1.5f;
		// A temporary viewport with no travel must not erase the saved preference.
		if (travelX > 0)
			pos.x = std::clamp((x * viewWidth - offset.x - area.x) / travelX, 0.f, 1.f);
		if (travelY > 0)
			pos.y = std::clamp((y * viewHeight - offset.y - area.y) / travelY, 0.f, 1.f);
		moved = true;
	}

	void end()
	{
		if (dragged >= 0 && moved)
			dirty |= 1u << dragged;
		dragged = -1;
		moved = false;
	}

	unsigned takeDirty() { unsigned result = dirty; dirty = 0; return result; }
	void setPosition(int index, Position pos)
	{
		positions[index] = {std::clamp(pos.x, 0.f, 1.f), std::clamp(pos.y, 0.f, 1.f)};
	}
	Position position(int index) const { return positions[index]; }
	void reset()
	{
		end();
		for (int i = 0; i < 8; ++i)
			positions[i] = {i < 4 ? 0.f : 1.f, (i % 4) / 3.f};
		dirty = 0xff;
	}

private:
	std::array<Position, 8> positions {};
	Bounds area;
	Position offset;
	float viewWidth = 0.f, viewHeight = 0.f, screenWidth = 0.f;
	unsigned visible = 0, dirty = 0;
	int dragged = -1;
	bool moved = false;
	bool movable = true;
};
}
