// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <cstdint>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
void testWarning(const char *, ...);
#define WARN_LOG(category, ...) testWarning(__VA_ARGS__)
enum class RenderType
{
	OpenGL,
	Vulkan,
	DirectX9,
	DirectX11
};
struct TestSettings
{
	struct
	{
		float uiScale = 1.f;
		int dpi = 96;
		int width = 1280;
		int height = 720;
	} display;
	struct
	{
		bool console = true;
		bool isConsole() const
		{
			return console;
		}
	} platform;
};
extern TestSettings settings;
