// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#define Rect MacOSRect
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#undef Rect
#include <string>

inline MTLStorageMode metalCpuTextureStorageMode(id<MTLDevice> device)
{
#if TARGET_OS_OSX
	if (@available(macOS 10.15, *))
		return device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
	return MTLStorageModeManaged;
#else
	return MTLStorageModeShared;
#endif
}

inline std::string metalErrorMessage(const char *operation, NSError *error)
{
	const char *detail = error.localizedDescription.UTF8String;
	return std::string(operation) + (detail ? ": " + std::string(detail) : "");
}

struct ImDrawData;
void metalInitImGui();
void metalDrawImGui(id<MTLRenderCommandEncoder> encoder, int width, int height);
