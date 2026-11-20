// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test probe implementations are linked only by native graphics test builds.
#include <cstdint>

namespace hikaru::graphics {
class Backend;

struct Dx11GpuTimingPhase {
    uint64_t samples=0;
    double milliseconds=0;
};
struct Dx11DiagnosticsSnapshot {
    bool enabled=false,gpuRequested=false,gpuSupported=false;
    uint64_t draws=0,vertexUploads=0,vertexBytes=0;
    uint64_t uniformUploads=0,uniformHits=0,uniformBytes=0;
    uint64_t paletteUploads=0,paletteHits=0,paletteBytes=0;
    uint64_t shaderCacheLookups=0,shaderCacheHits=0,shaderCompiles=0,shaderCompileNanoseconds=0;
    uint64_t textureUploads=0,textureBytes=0,queriesCreated=0,gpuQueryObjects=0,queryWaitNanoseconds=0;
    uint64_t fragmentCounterWaitNanoseconds=0,maskedClears=0,maskedPixels=0;
    Dx11GpuTimingPhase frame,clear,collect,copy,resolve;
    uint64_t gpuDropped=0,gpuInvalid=0;
};

Dx11DiagnosticsSnapshot dx11DiagnosticsForTesting(const Backend& backend);
void finishDx11DiagnosticFrameForTesting(Backend& backend);
void pollDx11DiagnosticFramesForTesting(Backend& backend);
}
