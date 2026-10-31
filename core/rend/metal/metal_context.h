// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "wsi/context.h"
#include <vector>
#include <memory>
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
	void PresentLastFrame();
	bool GetLastFrame(std::vector<u8> &data, int &width, int &height);
	void WaitIdle();
	void resize() override;
	void setSwapInterval(int interval) override;
	std::string getDriverName() override;
	std::string getDriverVersion() override;
	bool isAMD() override { return false; }
	bool hasPerPixel() override;

  private:
	MetalContext(void *window, void *display);
	bool readFrame(std::vector<u8> &data, int &width, int &height, bool normalize);
	struct Impl;
	std::unique_ptr<Impl> impl;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
};
