// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
inline bool isOpenGL(RenderType type)
{
	return type == RenderType::OpenGL;
}
inline bool isVulkan(RenderType type)
{
	return type == RenderType::Vulkan;
}
