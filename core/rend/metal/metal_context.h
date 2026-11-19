// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "wsi/context.h"
#include <functional>
#include <vector>
#include <memory>
#include <array>
#include "metal_common.h"

class MetalContext final : public GraphicsContext
{
  public:
	static MetalContext *Instance() { return static_cast<MetalContext *>(GraphicsContext::Instance()); }
	static void Create(void *window, void *display);
	~MetalContext() override;
	id<MTLDevice> GetDevice() const { return device; }
	id<MTLCommandQueue> GetQueue() const { return queue; }
	void PresentFrame(id<MTLTexture> texture, float aspectRatio);
	// The Hikaru backend owns the filter. Its retained frame can outlive it.
	using HikaruFrameShader = std::function<bool(id<MTLTexture>, int, int, id<MTLTexture> __strong&, bool&)>;
	void PresentHikaruFrame(id<MTLTexture> texture, float aspectRatio,
			const std::shared_ptr<const HikaruFrameShader>& shader);
	// Updated by the renderer before presentation, including paused shader edits.
	void SetShaderOutput(id<MTLTexture> texture, bool transformed, bool fullPresentation,
			const std::array<int, 4>& gameArea = {});
	void PresentLastFrame();
	bool GetLastFrame(std::vector<u8> &data, int &width, int &height);
	void WaitIdle();
	void resize() override;
	void setSwapInterval(int interval) override;
	void setVideoMode(bool pal);
	std::string getDriverName() override;
	std::string getDriverVersion() override;
	bool isAMD() override { return false; }
	bool hasPerPixel() override;

  private:
	MetalContext(void *window, void *display);
	void presentFrame(id<MTLTexture> texture, float aspectRatio,
			const std::shared_ptr<const HikaruFrameShader>& shader);
	bool readFrame(std::vector<u8> &data, int &width, int &height, bool normalize);
	struct Impl;
	std::unique_ptr<Impl> impl;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
};
