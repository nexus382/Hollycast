// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "metal_common.h"
#include "rend/postprocess/shader_chain.h"
#include <memory>

class MetalShaderChain
{
public:
	MetalShaderChain(id<MTLDevice> device, id<MTLCommandQueue> queue);
	~MetalShaderChain();
	bool render(id<MTLTexture> source, id<MTLTexture> depth,
			const postprocess::ShaderFrameInfo& frame, id<MTLTexture>& output,
			bool& transformed, bool& fullPresentation);
private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
