// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __APPLE__
#import <Metal/Metal.h>
#include "device.h"
namespace hikaru::graphics {
// Offscreen test entry point. The returned backend does not require or present
// through MetalContext; callers can render to texture ID 0 and read it back.
std::unique_ptr<Backend> makeMetalBackendForTesting(id<MTLDevice> device, id<MTLCommandQueue> queue);
}
#endif
