// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "metal_common.h"
#include <memory>

class MetalOverlay
{
  public:
	explicit MetalOverlay(id<MTLDevice> device);
	~MetalOverlay();
	void draw(id<MTLRenderCommandEncoder> encoder, int width, int height, float scaling, bool showVmus,
			  bool showCrosshairs);

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
