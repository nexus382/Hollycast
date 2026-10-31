// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "metal_common.h"

// Screen render targets survive temporary RTT and CPU framebuffer outputs.
class MetalRenderTargets
{
public:
    id<MTLTexture> output = nil;
    id<MTLTexture> depth = nil;
    id<MTLTexture> frameOutput = nil;

    bool begin(id<MTLDevice> device, int width, int height, bool rtt)
    {
        bool fresh = rtt || !screenOutput || !screenDepth || screenOutput.width != (NSUInteger)width
                || screenOutput.height != (NSUInteger)height;
        if (fresh) {
            output = makeTexture(device, width, height, MTLPixelFormatRGBA8Unorm,
                    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            depth = makeTexture(device, width, height, MTLPixelFormatDepth32Float_Stencil8,
                    MTLTextureUsageRenderTarget);
            if (!rtt) { screenOutput = output; screenDepth = depth; }
        } else {
            output = screenOutput; depth = screenDepth;
        }
        return fresh;
    }
    void end(bool rtt) { if (rtt) { output = nil; depth = nil; } }
    void setFramebuffer(id<MTLTexture> texture) { frameOutput = texture; }
    void clear() { output = nil; depth = nil; screenOutput = nil; screenDepth = nil; frameOutput = nil; }
private:
    id<MTLTexture> screenOutput = nil, screenDepth = nil;
    static id<MTLTexture> makeTexture(id<MTLDevice> device, int width, int height,
            MTLPixelFormat format, MTLTextureUsage usage)
    {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                width:width height:height mipmapped:NO];
        desc.usage = usage;
        desc.storageMode = MTLStorageModePrivate;
        return [device newTextureWithDescriptor:desc];
    }
};
