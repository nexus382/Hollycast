// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hw/pvr/ta.h"
#include "metal_renderer.h"
#include "metal_geometry.h"
#include "metal_context.h"
#include "metal_texture.h"
#include "metal_targets.h"
#include "metal_framebuffer.h"
#include "metal_shaders.h"
#include "metal_modvol.h"
#include "metal_oit.h"
#include "metal_oit_shaders.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/transform_matrix.h"
#include "rend/CustomTexture.h"
#include "rend/sample_filter.h"
#include "ui/performance_overlay.h"
#include "cfg/option.h"
#include <simd/simd.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <atomic>
#include <memory>

namespace {
struct alignas(16) Uniforms {
    simd_float4x4 matrix;
    simd_float4 clip, fogColor, fogVert, fogClampMin, fogClampMax, values;
    simd_uint4 flags, flags2, flags3, flags4;
};
static_assert(sizeof(Uniforms) == 224, "Metal uniform layout mismatch");

struct OITGpuTiming {
    std::atomic<u64> elapsed {0};
    std::atomic<unsigned> pending {1}; // Sentinels prevent early reporting during encoding.
    std::atomic<bool> failed {false};
    u32 epoch = 0;
    void finishOne() {
        if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1 && !failed.load(std::memory_order_relaxed)) {
            u64 value = elapsed.load(std::memory_order_relaxed);
            if (value) performance_overlay::recordGpuTimingSample(value, epoch);
        }
    }
};

class MetalRenderer final : public Renderer {
public:
    explicit MetalRenderer(bool perPixel = false) : perPixel(perPixel) {}
    bool Init() override;
    void Term() override;
    void Process(TA_context *ctx) override;
    bool Render() override;
    void RenderFramebuffer(const FramebufferInfo& info) override;
    bool Present() override;
    bool RenderLastFrame() override;
    bool supportsGpuTexturePreload() const override { return true; }
    void processCustomTexturePreloads() override {
        custom_texture.processGpuPreloads([this](u32 hash, const PreparedCustomTexture& prepared) {
            auto texture = MetalTexture::createGpuPreloadedTexture(prepared);
            if (!texture) return false;
            addGpuPreloadedTexture(hash, std::move(texture));
            return true;
        });
    }
    bool GetLastFrame(std::vector<u8>& data, int& width, int& height) override {
        return !clearLastFrame && MetalContext::Instance()->GetLastFrame(data, width, height);
    }
    BaseTextureCacheData *GetTexture(TSP tsp, TCW tcw, int area) override {
        return textureCache.GetTexture(tsp, tcw, area);
    }
protected:
    void clearTextureCache() override { textureCache.Clear(); textureCache.Cleanup(); }
private:
    id<MTLRenderPipelineState> pipeline(u32 src, u32 dst, bool depthOnly = false,
            bool mirror = false, bool mirrorWrite = false, bool collect = false, bool colorStage = false);
    void draw(id<MTLRenderCommandEncoder> enc, id<MTLBuffer> indices, const PolyParam& poly,
              u32 first, u32 count, MTLPrimitiveType primitive, bool punch, bool sorted, bool depthOnly = false,
              bool mirror = false, MetalOITBuffers *oit = nullptr, id<MTLTexture> opaqueDepth = nil, u32 sequence = 0,
              unsigned oitColor = 0);
    bool renderPerPixel();
    void writeFramebuffer(id<MTLCommandBuffer> command, id<MTLTexture> image);
    MetalTextureCache textureCache;
    MetalModifierVolumes modifiers;
    MetalOITBuffers oitBuffers;
    rend_context *renderContext = nullptr;
    TransformMatrix matrices { true };
    id<MTLLibrary> library = nil, oitLibrary = nil;
    const bool perPixel;
    MetalRenderTargets targets;
    id<MTLTexture> white = nil;
    id<MTLTexture> palette = nil, fog = nil;
    std::unordered_map<unsigned, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<unsigned, id<MTLDepthStencilState>> depthStates;
    std::unordered_map<unsigned, id<MTLSamplerState>> samplers;
    bool frameReady = false;
    bool layerWarningShown = false;
    MetalPvrTimingEvidence pvrTimingEvidence;
    float aspect = 4.f / 3.f;
};


bool MetalRenderer::Init() {
    @autoreleasepool {
    id<MTLDevice> device = MetalContext::Instance()->GetDevice();
    custom_texture.setCapabilities(MetalTexture::getCustomTextureCapabilities());
    NSError *error = nil;
    library = [device newLibraryWithSource:[NSString stringWithUTF8String:metalRegularShaderSource] options:nil error:&error];
    if (!library) {
        ERROR_LOG(RENDERER, "Metal shader compilation failed: %s", error.localizedDescription.UTF8String);
        return false;
    }
    if (perPixel) {
        std::string source = std::string(metalOITShaderSource) + metalRegularShaderSource + metalOITCollectShaderSource;
        oitLibrary = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
        if (!oitLibrary) throw RendererException(metalErrorMessage("Metal per-pixel shader compilation failed", error));
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = metalCpuTextureStorageMode(device);
    white = [device newTextureWithDescriptor:d];
    const u32 pixel = 0xffffffff;
    [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];
    modifiers.Init(device);
    performance_overlay::setGpuTimingSupported(false);
    if (!white || !pipeline(1, 0)) return false;
    NOTICE_LOG(RENDERER, "%s", perPixel ? "Native Metal per-pixel renderer initialized" : "Native Metal regular renderer initialized");
    return true;
    }
}
void MetalRenderer::Term() {
    MetalContext::Instance()->WaitIdle();
    modifiers.Term(); clearTextureCache(); clearGpuPreloadedTextures();
    performance_overlay::setGpuTimingSupported(false);
    pvrTimingEvidence.reset();
    pipelines.clear(); depthStates.clear(); samplers.clear(); targets.clear(); white = nil; palette = nil; fog = nil; library = nil; oitLibrary = nil;
}
void MetalRenderer::Process(TA_context *ctx) {
    @autoreleasepool {
    renderContext = &ctx->rend;
    if (resetTextureCache) { clearTextureCache(); resetTextureCache = false; }
    textureCache.Cleanup();
    if (!ctx->rend.isRTT && !config::EmulateFramebuffer) clearLastFrame = false;
    if (!ctx->rend.isRTT && ctx->rend.swapInterval > 0)
        MetalContext::Instance()->setSwapInterval(ctx->rend.swapInterval);
    // Use degenerate indices for strip joins; no primitive-restart state is required.
    ta_parse(ctx, false);
    // Allocate a fresh immutable table so queued frames never see later register updates.
    if (updatePalette || !palette) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1024 height:1 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = metalCpuTextureStorageMode(MetalContext::Instance()->GetDevice());
        palette = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:d];
        [palette replaceRegion:MTLRegionMake2D(0, 0, 1024, 1) mipmapLevel:0 withBytes:palette32_ram bytesPerRow:4096];
        updatePalette = false;
    }
    if (updateFogTable || !fog) {
        u8 fogData[256]; MakeFogTexture(fogData);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:128 height:2 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = metalCpuTextureStorageMode(MetalContext::Instance()->GetDevice());
        fog = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:d];
        [fog replaceRegion:MTLRegionMake2D(0, 0, 128, 2) mipmapLevel:0 withBytes:fogData bytesPerRow:128];
        updateFogTable = false;
    }
    }
}

id<MTLRenderPipelineState> MetalRenderer::pipeline(u32 src, u32 dst, bool depthOnly, bool mirror, bool mirrorWrite, bool collect, bool colorStage) {
    unsigned key = src * 8 + dst + (depthOnly ? 64 : 0) + (mirror ? 128 : 0) + (mirrorWrite ? 256 : 0) + (collect ? 512 : 0) + (colorStage ? 1024 : 0);
    auto found = pipelines.find(key);
    if (found != pipelines.end()) return found->second;
    static const MTLBlendFactor source[] = { MTLBlendFactorZero, MTLBlendFactorOne,
        MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha };
    static const MTLBlendFactor dest[] = { MTLBlendFactorZero, MTLBlendFactorOne,
        MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha,
        MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha };
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> activeLibrary = (collect || colorStage) ? oitLibrary : library;
    d.vertexFunction = [activeLibrary newFunctionWithName:(collect || colorStage) ? @"pvr_oit_vertex" : @"pvr_vertex"];
    d.fragmentFunction = [activeLibrary newFunctionWithName:collect ? @"pvr_collect" : colorStage ? (mirror ? @"pvr_oit_color_mirror" : @"pvr_oit_color") : mirror ? @"pvr_mirror" : @"pvr_fragment"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.colorAttachments[0].blendingEnabled = !depthOnly && !collect;
    if (depthOnly || collect) d.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    if (mirror) {
        d.colorAttachments[1].pixelFormat = MTLPixelFormatR32Float;
        d.colorAttachments[1].writeMask = mirrorWrite ? MTLColorWriteMaskRed : MTLColorWriteMaskNone;
    }
    d.colorAttachments[0].sourceRGBBlendFactor = source[src];
    d.colorAttachments[0].sourceAlphaBlendFactor = source[src];
    d.colorAttachments[0].destinationRGBBlendFactor = dest[dst];
    d.colorAttachments[0].destinationAlphaBlendFactor = dest[dst];
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    v.attributes[0].format = MTLVertexFormatFloat3; v.attributes[0].offset = offsetof(Vertex, x);
    v.attributes[1].format = MTLVertexFormatUChar4Normalized; v.attributes[1].offset = offsetof(Vertex, col);
    v.attributes[2].format = MTLVertexFormatUChar4Normalized; v.attributes[2].offset = offsetof(Vertex, spc);
    v.attributes[3].format = MTLVertexFormatFloat2; v.attributes[3].offset = offsetof(Vertex, u);
    if (collect || colorStage) {
        v.attributes[4].format = MTLVertexFormatUChar4Normalized; v.attributes[4].offset = offsetof(Vertex, col1);
        v.attributes[5].format = MTLVertexFormatUChar4Normalized; v.attributes[5].offset = offsetof(Vertex, spc1);
        v.attributes[6].format = MTLVertexFormatFloat2; v.attributes[6].offset = offsetof(Vertex, u1);
    }
    for (int i = 0; i < ((collect || colorStage) ? 7 : 4); ++i) v.attributes[i].bufferIndex = 0;
    v.layouts[0].stride = sizeof(Vertex);
    d.vertexDescriptor = v;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [MetalContext::Instance()->GetDevice() newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state) throw RendererException(std::string("Metal pipeline: ") + error.localizedDescription.UTF8String);
    pipelines.emplace(key, state);
    return state;
}

void MetalRenderer::draw(id<MTLRenderCommandEncoder> enc, id<MTLBuffer> indices,
        const PolyParam& p, u32 first, u32 count, MTLPrimitiveType primitive, bool punch, bool sorted, bool depthOnly,
        bool mirror, MetalOITBuffers *oit, id<MTLTexture> opaqueDepth, u32 sequence, unsigned oitColor) {
    if (count < 3) return;
    if (p.isNaomi2()) throw RendererException("Metal regular renderer does not yet support Naomi 2 geometry");
    if (!perPixel && !oit && (p.tsp.SrcSelect || p.tsp.DstSelect))
        throw RendererException("Metal regular renderer does not yet support the PVR secondary accumulation buffer");
    bool stencilStage = oitColor > 0 && oitColor < 4;
    bool colorStage = stencilStage || oitColor == 5;
    bool writeDepth = oitColor == 4 ? (punch || !p.isp.ZWriteDis)
            : stencilStage ? (punch || !p.isp.ZWriteDis)
            : depthOnly || punch || (!sorted && !p.isp.ZWriteDis);
    [enc setRenderPipelineState:pipeline(p.tsp.SrcInstr, p.tsp.DstInstr, depthOnly, mirror, mirror && writeDepth, oit != nullptr, colorStage)];
    static const MTLCompareFunction compare[] = { MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
        MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
    unsigned colorDepthMode = p.isp.DepthMode;
    unsigned depthKey = (oit ? 7 : stencilStage ? (punch ? 6 : colorDepthMode) : oitColor == 4 ? (punch ? 6 : p.isp.DepthMode)
            : punch || sorted || depthOnly ? 6 : p.isp.DepthMode)
            | ((!oit && writeDepth) ? 8 : 0)
            | (((depthOnly && oitColor != 4) || oit || colorStage) ? 16 : 0)
            | ((stencilStage ? (oitColor == 1 ? 1u : 2u) : 0u) << 5);
    auto depthIt = depthStates.find(depthKey);
    if (depthIt == depthStates.end()) {
        MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = compare[depthKey & 7];
        ds.depthWriteEnabled = (depthKey & 8) != 0;
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = (depthKey >> 5) == 1 ? MTLCompareFunctionNotEqual
            : (depthKey >> 5) == 2 ? MTLCompareFunctionEqual : MTLCompareFunctionAlways;
        stencil.stencilFailureOperation = MTLStencilOperationKeep;
        stencil.depthFailureOperation = MTLStencilOperationKeep;
        stencil.depthStencilPassOperation = MTLStencilOperationReplace;
        stencil.readMask = stencilStage ? 0x81 : 0xff; stencil.writeMask = (depthKey & 16) ? 0 : 0x80;
        ds.frontFaceStencil = stencil; ds.backFaceStencil = stencil;
        id<MTLDepthStencilState> state = [MetalContext::Instance()->GetDevice() newDepthStencilStateWithDescriptor:ds];
        if (!state) throw RendererException("Metal depth/stencil state allocation failed");
        depthIt = depthStates.emplace(depthKey, state).first;
    }
    [enc setDepthStencilState:depthIt->second];
    [enc setStencilReferenceValue:stencilStage ? 0x81 : p.pcw.Shadow ? 0x80 : 0];
    [enc setCullMode:metalCullMode(p.isp.CullMode)];
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
    Rect clip { glm::ivec2(0), glm::ivec2(0) };
    TileClipping clipMode = matrices.getTileClip(p.tileclip, clip);
    Rect scissor = matrices.getBaseScissor();
    if (clipMode == TileClipping::Outside) scissor = intersect(scissor, clip);
    int left = std::clamp(scissor.origin.x, 0, (int)targets.output.width);
    int top = std::clamp(scissor.origin.y, 0, (int)targets.output.height);
    int right = std::clamp(scissor.origin.x + scissor.size.x, left, (int)targets.output.width);
    int bottom = std::clamp(scissor.origin.y + scissor.size.y, top, (int)targets.output.height);
    if (right == left || bottom == top) return;
    [enc setScissorRect:MTLScissorRect{(NSUInteger)left, (NSUInteger)top, (NSUInteger)(right-left), (NSUInteger)(bottom-top)}];

    Uniforms u {};
    memcpy(&u.matrix, &matrices.GetNormalMatrix()[0][0], sizeof(u.matrix));
    u.clip = simd_make_float4(clip.origin.x, clip.origin.y, clip.origin.x + clip.size.x, clip.origin.y + clip.size.y);
    u.fogColor = simd_make_float4(FOG_COL_RAM.red(), FOG_COL_RAM.green(), FOG_COL_RAM.blue(), 1.f);
    u.fogVert = simd_make_float4(FOG_COL_VERT.red(), FOG_COL_VERT.green(), FOG_COL_VERT.blue(), 1.f);
    auto& lo = renderContext->fog_clamp_min; auto& hi = renderContext->fog_clamp_max;
    u.fogClampMin = simd_make_float4(lo.red(), lo.green(), lo.blue(), lo.alpha());
    u.fogClampMax = simd_make_float4(hi.red(), hi.green(), hi.blue(), hi.alpha());
    float trilinear = 1.f;
    if (p.pcw.Texture && p.tsp.FilterMode > 1 && !punch && p.tcw.MipMapped) {
        trilinear = 0.25f * (p.tsp.MipMapD & 3);
        if (p.tsp.FilterMode == 2) trilinear = 1.f - trilinear;
    }
    MetalTexture *texture = static_cast<MetalTexture *>(p.texture);
    u.values = simd_make_float4((PT_ALPHA_REF & 255) / 255.f, trilinear, FOG_DENSITY.get() * config::ExtraDepthScale, texture && texture->IsMipmapped() ? D_Adjust_LoD_Bias[p.tsp.MipMapD] : 0.f);
    u.flags = simd_make_uint4(p.pcw.Texture, p.tsp.UseAlpha, p.tsp.IgnoreTexA, p.tsp.ShadInstr);
    u.flags2 = simd_make_uint4(p.pcw.Offset, punch, clipMode == TileClipping::Inside,
            config::Fog ? p.tsp.FogCtrl : 2);
    bool paletted = texture && texture->gpuPalette;
    bool linear = config::TextureFiltering == 2 || (config::TextureFiltering == 0 && p.tsp.FilterMode != 0);
    sample_filter::Settings reconstruction(config::SampleTextureFilter.get(), config::MaxFilteredTextureSize.get());
    unsigned mode = texture && p.tcw.PixelFmt != PixelBumpMap && reconstruction.appliesTo(texture->texture().width) ? reconstruction.mode : 0;
    u.flags3 = simd_make_uint4(p.tsp.ColorClamp && (lo.full != 0 || hi.full != 0xffffffff), p.tcw.PixelFmt == PixelBumpMap, paletted ? (linear ? 2 : 1) : 0,
            p.tcw.PixelFmt == PixelPal4 ? p.tcw.PalSelect << 4 : (p.tcw.PalSelect >> 4) << 8);
    u.flags4 = simd_make_uint4(p.pcw.Gouraud, mode, config::NativeDepthInterpolation,
            (oitColor == 2 ? 1u : oitColor == 3 ? 2u : 0u) | (colorStage && p.tsp.SrcSelect ? 4u : 0u));
    u.fogVert.w = FPU_SHAD_SCALE.scale_factor / 256.f;
    [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [enc setFragmentTexture:texture && texture->texture() ? texture->texture() : white atIndex:0];
    [enc setFragmentTexture:palette atIndex:1];
    [enc setFragmentTexture:fog atIndex:2];
    if (colorStage) [enc setFragmentTexture:opaqueDepth ? opaqueDepth : white atIndex:3];
    if (paletted || mode) linear = false;
    bool mip = linear && !punch && texture && texture->IsMipmapped();
    unsigned anisotropy = linear ? std::clamp((int)config::AnisotropicFiltering, 1, 16) : 1;
    unsigned samplerKey = unsigned(linear) | (unsigned(mip) << 1)
            | (p.tsp.ClampU << 2) | (p.tsp.FlipU << 3) | (p.tsp.ClampV << 4) | (p.tsp.FlipV << 5)
            | (anisotropy << 6);
    auto samplerIt = samplers.find(samplerKey);
    if (samplerIt == samplers.end()) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = mip ? (anisotropy > 1 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest) : MTLSamplerMipFilterNotMipmapped;
        sd.maxAnisotropy = anisotropy;
        sd.sAddressMode = p.tsp.ClampU ? MTLSamplerAddressModeClampToEdge : p.tsp.FlipU ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
        sd.tAddressMode = p.tsp.ClampV ? MTLSamplerAddressModeClampToEdge : p.tsp.FlipV ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> state = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:sd];
        if (!state) throw RendererException("Metal sampler allocation failed");
        samplerIt = samplers.emplace(samplerKey, state).first;
    }
    [enc setFragmentSamplerState:samplerIt->second atIndex:0];
    if (oit) {
        const bool twoVolumes = p.tsp1.full != (u32)-1;
        MetalTexture *texture1 = twoVolumes ? static_cast<MetalTexture *>(p.texture1) : nullptr;
        Uniforms u1 = u;
        id<MTLSamplerState> sampler1 = samplerIt->second;
        if (twoVolumes) {
            const TSP& t = p.tsp1; const TCW& tcw = p.tcw1;
            bool pal1 = texture1 && texture1->gpuPalette;
            bool linear1 = config::TextureFiltering == 2 || (config::TextureFiltering == 0 && t.FilterMode != 0);
            unsigned mode1 = texture1 && tcw.PixelFmt != PixelBumpMap && reconstruction.appliesTo(texture1->texture().width) ? reconstruction.mode : 0;
            u1.flags = simd_make_uint4(p.pcw.Texture,t.UseAlpha,t.IgnoreTexA,t.ShadInstr);
            u1.flags2.w = config::Fog ? t.FogCtrl : 2;
            u1.flags3 = simd_make_uint4(t.ColorClamp && (lo.full != 0 || hi.full != 0xffffffff), tcw.PixelFmt == PixelBumpMap,
                    pal1 ? (linear1 ? 2 : 1) : 0, tcw.PixelFmt == PixelPal4 ? tcw.PalSelect << 4 : (tcw.PalSelect >> 4) << 8);
            u1.flags4.y = mode1;
            float weight1 = 1.f;
            if (p.pcw.Texture && t.FilterMode > 1 && tcw.MipMapped) {
                weight1 = 0.25f * (t.MipMapD & 3); if (t.FilterMode == 2) weight1 = 1.f - weight1;
            }
            u1.values.y = weight1;
            u1.values.w = texture1 && texture1->IsMipmapped() ? D_Adjust_LoD_Bias[t.MipMapD] : 0.f;
            if (pal1 || mode1) linear1 = false;
            bool mip1 = linear1 && texture1 && texture1->IsMipmapped();
            unsigned aniso1 = linear1 ? std::clamp((int)config::AnisotropicFiltering, 1, 16) : 1;
            unsigned key1 = unsigned(linear1) | (unsigned(mip1) << 1) | (t.ClampU << 2) | (t.FlipU << 3) | (t.ClampV << 4) | (t.FlipV << 5) | (aniso1 << 6);
            auto found = samplers.find(key1);
            if (found == samplers.end()) {
                MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
                sd.minFilter = sd.magFilter = linear1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                sd.mipFilter = mip1 ? (aniso1 > 1 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest) : MTLSamplerMipFilterNotMipmapped;
                sd.maxAnisotropy = aniso1;
                sd.sAddressMode = t.ClampU ? MTLSamplerAddressModeClampToEdge : t.FlipU ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
                sd.tAddressMode = t.ClampV ? MTLSamplerAddressModeClampToEdge : t.FlipV ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
                sampler1 = [MetalContext::Instance()->GetDevice() newSamplerStateWithDescriptor:sd];
                if (!sampler1) throw RendererException("Metal area-1 sampler allocation failed");
                samplers.emplace(key1, sampler1);
            } else sampler1 = found->second;
        }
        struct { u32 sequence, tsp0, tsp1, flags; } params {sequence, p.tsp.full, p.tsp1.full, (p.pcw.Shadow ? 4u : 0u) | (twoVolumes ? 8u : 0u)};
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:2];
        [enc setFragmentBuffer:oit->nodes() offset:0 atIndex:3];
        [enc setFragmentBuffer:oit->heads() offset:0 atIndex:4];
        [enc setFragmentBuffer:oit->control() offset:0 atIndex:5];
        [enc setFragmentTexture:opaqueDepth atIndex:3];
        [enc setFragmentBytes:&u1 length:sizeof(u1) atIndex:6];
        [enc setFragmentTexture:texture1 && texture1->texture() ? texture1->texture() : white atIndex:4];
        [enc setFragmentSamplerState:sampler1 atIndex:1];
    }
    [enc drawIndexedPrimitives:primitive indexCount:count indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:first * sizeof(u32)];
}

bool MetalRenderer::Render() {
    if (perPixel) return renderPerPixel();
    @autoreleasepool {
    if (!renderContext) return false;
    int w, h; getScaledFramebufferSize(*renderContext, w, h);
    if (w <= 0 || h <= 0) return false;
    bool newTarget = targets.begin(MetalContext::Instance()->GetDevice(), w, h, renderContext->isRTT);
    if (!targets.output || !targets.depth) throw RendererException("Metal framebuffer allocation failed");
    matrices.CalcMatrices(renderContext, w, h);
    id<MTLDevice> device = MetalContext::Instance()->GetDevice();
    if (renderContext->verts.empty() || renderContext->idx.empty()) return false;
    MetalGeometryBuffers geometry = metalUploadGeometry(device, *renderContext);
    id<MTLBuffer> vertices = geometry.vertices;
    id<MTLBuffer> indices = geometry.indices;
    id<MTLCommandBuffer> command = [MetalContext::Instance()->GetQueue() commandBuffer];
    if (!command) throw RendererException("Metal command buffer allocation failed");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = targets.output;
    pass.colorAttachments[0].loadAction = newTarget || renderContext->clearFramebuffer ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = renderContext->isRTT ? MTLClearColorMake(0, 0, 0, 0)
        : MTLClearColorMake(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1);
    pass.depthAttachment.texture = targets.depth; pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore; pass.depthAttachment.clearDepth = 0;
    pass.stencilAttachment.texture = targets.depth; pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore; pass.stencilAttachment.clearStencil = 0;
    id<MTLRenderCommandEncoder> enc = [command renderCommandEncoderWithDescriptor:pass];
    [enc setVertexBuffer:vertices offset:0 atIndex:0];
    RenderPass prev {};
    for (const RenderPass& cur : renderContext->render_passes) {
        for (u32 i = prev.op_count; i < cur.op_count; ++i) {
            auto& p = renderContext->global_param_op[i]; draw(enc, indices, p, p.first, p.count, MTLPrimitiveTypeTriangleStrip, false, false);
        }
        for (u32 i = prev.pt_count; i < cur.pt_count; ++i) {
            auto& p = renderContext->global_param_pt[i]; draw(enc, indices, p, p.first, p.count, MTLPrimitiveTypeTriangleStrip, true, false);
        }
        if (config::ModifierVolumes && cur.mvo_count > prev.mvo_count) {
            modifiers.Draw(enc, *renderContext, matrices, prev.mvo_count, cur.mvo_count - prev.mvo_count, w, h);
            [enc setVertexBuffer:vertices offset:0 atIndex:0];
        }
        if (cur.autosort && !config::PerStripSorting) {
            for (u32 i = prev.sorted_tr_count; i < cur.sorted_tr_count; ++i) {
                auto& t = renderContext->sortedTriangles[i]; auto& p = renderContext->global_param_tr[t.polyIndex];
                draw(enc, indices, p, t.first, t.count, MTLPrimitiveTypeTriangle, false, true);
            }
            if (&cur != &renderContext->render_passes.back() && config::TranslucentPolygonDepthMask) {
                for (u32 i = prev.sorted_tr_count; i < cur.sorted_tr_count; ++i) {
                    auto& t = renderContext->sortedTriangles[i]; auto& p = renderContext->global_param_tr[t.polyIndex];
                    if (!p.isp.ZWriteDis)
                        draw(enc, indices, p, t.first, t.count, MTLPrimitiveTypeTriangle, false, false, true);
                }
            }
        } else {
            for (u32 i = prev.tr_count; i < cur.tr_count; ++i) {
                auto& p = renderContext->global_param_tr[i]; draw(enc, indices, p, p.first, p.count, MTLPrimitiveTypeTriangleStrip, false, cur.autosort);
            }
        }
        prev = cur;
    }
    [enc endEncoding];
    if (!renderContext->isRTT) {
        int rawWidth, rawHeight; getPvrFramebufferSize(*renderContext, rawWidth, rawHeight);
        performance_overlay::recordRenderedResolution(rawWidth, rawHeight, w, h);
    }
    const bool timing = !renderContext->isRTT && performance_overlay::isGpuTimingRequested();
    const u32 epoch = timing ? performance_overlay::getGpuTimingEpoch() : 0;
    if (timing) performance_overlay::setGpuTimingSupported(true);
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError)
            ERROR_LOG(RENDERER, "Metal command buffer failed: %s", completed.error.localizedDescription.UTF8String ?: "unknown error");
        else if (timing && completed.GPUEndTime > completed.GPUStartTime)
            performance_overlay::recordGpuTimingSample((u64)((completed.GPUEndTime - completed.GPUStartTime) * 1e9), epoch);
    }];
    if (renderContext->isRTT && !config::RenderToTextureBuffer) {
        [command commit];
        int rw, rh; getPvrFramebufferSize(*renderContext, rw, rh);
        if (rw > 1024 || rh > 1024) throw RendererException("Metal render-to-texture exceeds PVR texture size");
        MetalTexture *target = textureCache.getRTTexture(renderContext->fb_W_SOF1 & VRAM_MASK,
                renderContext->fb_W_CTRL.fb_packmode, rw, rh);
        target->setTexture(targets.output); target->dirty = 0; target->unprotectVRam();
    } else if (renderContext->isRTT || config::EmulateFramebuffer) writeFramebuffer(command, targets.output);
    else [command commit];
    if (renderContext->isRTT) { targets.end(true); return false; }
    if (config::EmulateFramebuffer) {
        if (timing) pvrTimingEvidence.submitted(epoch);
        return true;
    }
    aspect = getOutputFramebufferAspectRatio(); targets.frameOutput = targets.output; frameReady = true; clearLastFrame = false;
    return true;
    }
}


bool MetalRenderer::renderPerPixel() {
    @autoreleasepool {
    if (!renderContext) return false;
    int w, h; getScaledFramebufferSize(*renderContext, w, h);
    if (w <= 0 || h <= 0 || renderContext->verts.empty() || renderContext->idx.empty()) return false;
    id<MTLDevice> device = MetalContext::Instance()->GetDevice();
    id<MTLCommandQueue> queue = MetalContext::Instance()->GetQueue();
    std::shared_ptr<OITGpuTiming> timing;
    if (!renderContext->isRTT && performance_overlay::isGpuTimingRequested()) {
        timing = std::make_shared<OITGpuTiming>(); timing->epoch = performance_overlay::getGpuTimingEpoch();
        performance_overlay::setGpuTimingSupported(true);
    }
    struct TimingGuard { std::shared_ptr<OITGpuTiming> state; ~TimingGuard() { if (state) state->finishOne(); } } timingGuard {timing};
    auto track = [timing](id<MTLCommandBuffer> cmd) {
        if (!cmd) throw RendererException("Metal per-pixel command buffer allocation failed");
        if (timing) timing->pending.fetch_add(1, std::memory_order_relaxed);
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            if (completed.status == MTLCommandBufferStatusError) {
                ERROR_LOG(RENDERER, "Metal per-pixel GPU command failed: %s", completed.error.localizedDescription.UTF8String ?: "unknown error");
                if (timing) timing->failed.store(true, std::memory_order_relaxed);
            } else if (timing && completed.GPUEndTime > completed.GPUStartTime) {
                timing->elapsed.fetch_add((u64)((completed.GPUEndTime-completed.GPUStartTime)*1e9), std::memory_order_relaxed);
            }
            if (timing) timing->finishOne();
        }];
    };
    auto newCommand = [&] { id<MTLCommandBuffer> cmd = [queue commandBuffer]; track(cmd); return cmd; };
    bool fresh = targets.begin(device, w, h, renderContext->isRTT);
    if (!targets.output || !targets.depth) throw RendererException("Metal per-pixel framebuffer allocation failed");
    matrices.CalcMatrices(renderContext, w, h);
    MetalGeometryBuffers geometry = metalUploadGeometry(device, *renderContext);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:w height:h mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> opaqueDepth = [device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> resolved = [device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> replayDepth = [device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> secondary = [device newTextureWithDescriptor:descriptor];
    id<MTLTexture> secondaryCopy = [device newTextureWithDescriptor:descriptor];
    if (!opaqueDepth || !resolved || !secondary || !secondaryCopy || !replayDepth) throw RendererException("Metal per-pixel render target allocation failed");
    id<MTLCommandBuffer> clearSecondary = newCommand();
    MTLRenderPassDescriptor *secondaryPass = [MTLRenderPassDescriptor renderPassDescriptor];
    secondaryPass.colorAttachments[0].texture = secondary; secondaryPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    secondaryPass.colorAttachments[0].storeAction = MTLStoreActionStore; secondaryPass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
    [[clearSecondary renderCommandEncoderWithDescriptor:secondaryPass] endEncoding]; [clearSecondary commit];
    auto copySecondary = [&] {
        id<MTLCommandBuffer> command = newCommand(); id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
        [copy copyFromTexture:secondary sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                  sourceSize:MTLSizeMake(w,h,1) toTexture:secondaryCopy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [copy endEncoding]; [command commit];
    };
    // Color replay has its own depth history; the modifier stencil plane is
    // copied into it after the depth prepass for each TA render pass.
    id<MTLCommandBuffer> clearReplay = newCommand();
    MTLRenderPassDescriptor *replayClear = [MTLRenderPassDescriptor renderPassDescriptor];
    replayClear.depthAttachment.texture = replayDepth; replayClear.depthAttachment.loadAction = MTLLoadActionClear;
    replayClear.depthAttachment.clearDepth = 0; replayClear.depthAttachment.storeAction = MTLStoreActionStore;
    replayClear.stencilAttachment.texture = replayDepth; replayClear.stencilAttachment.loadAction = MTLLoadActionClear;
    replayClear.stencilAttachment.clearStencil = 0; replayClear.stencilAttachment.storeAction = MTLStoreActionStore;
    [[clearReplay renderCommandEncoderWithDescriptor:replayClear] endEncoding]; [clearReplay commit];
    auto makePass = [&](bool first, bool withMirror, id<MTLTexture> colorTarget = nil, id<MTLTexture> depthTarget = nil) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = colorTarget ? colorTarget : targets.output;
        p.colorAttachments[0].loadAction = first && (fresh || renderContext->clearFramebuffer) ? MTLLoadActionClear : MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        p.colorAttachments[0].clearColor = renderContext->isRTT ? MTLClearColorMake(0,0,0,0) : MTLClearColorMake(VO_BORDER_COL.red(),VO_BORDER_COL.green(),VO_BORDER_COL.blue(),1);
        if (withMirror) {
            p.colorAttachments[1].texture = opaqueDepth;
            p.colorAttachments[1].loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
            p.colorAttachments[1].storeAction = MTLStoreActionStore;
            p.colorAttachments[1].clearColor = MTLClearColorMake(0,0,0,0);
        }
        p.depthAttachment.texture = depthTarget ? depthTarget : targets.depth; p.depthAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.storeAction = MTLStoreActionStore; p.depthAttachment.clearDepth = 0;
        p.stencilAttachment.texture = depthTarget ? depthTarget : targets.depth; p.stencilAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
        p.stencilAttachment.storeAction = MTLStoreActionStore; p.stencilAttachment.clearStencil = 0;
        return p;
    };
    auto finish = [](id<MTLCommandBuffer> cmd) {
        [cmd commit]; [cmd waitUntilCompleted];
        if (cmd.status != MTLCommandBufferStatusCompleted)
            throw RendererException(metalErrorMessage("Metal per-pixel GPU command failed", cmd.error));
    };
    RenderPass prev {};
    bool first = true;
    for (const RenderPass& cur : renderContext->render_passes) {
        id<MTLCommandBuffer> command = newCommand();
        id<MTLRenderCommandEncoder> enc = [command renderCommandEncoderWithDescriptor:makePass(first, true)];
        [enc setVertexBuffer:geometry.vertices offset:0 atIndex:0];
        for (u32 i=prev.op_count;i<cur.op_count;i++) { const auto& p=renderContext->global_param_op[i]; draw(enc,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,false,false,true,true,nullptr,nil,0,4); }
        for (u32 i=prev.pt_count;i<cur.pt_count;i++) { const auto& p=renderContext->global_param_pt[i]; draw(enc,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,true,false,true,true,nullptr,nil,0,4); }
        [enc endEncoding]; [command commit];
        first = false;
        if (config::ModifierVolumes && cur.mvo_count > prev.mvo_count) {
            command = newCommand(); enc = [command renderCommandEncoderWithDescriptor:makePass(false, false)];
            modifiers.Draw(enc,*renderContext,matrices,prev.mvo_count,cur.mvo_count-prev.mvo_count,w,h,false);
            [enc endEncoding]; [command commit];
        }
        command = newCommand();
        metalOITCopyDepthStencilAspect(device, command, targets.depth, replayDepth, w, h, true);
        [command commit];
        // Replay in submission order; secondary-source draws sample a snapshot
        // so Metal never reads and writes the same attachment concurrently.
        id<MTLCommandBuffer> colorCommand = nil;
        id<MTLRenderCommandEncoder> colorEncoder = nil;
        auto flushColor = [&] {
            if (colorEncoder) { [colorEncoder endEncoding]; [colorCommand commit]; colorEncoder = nil; colorCommand = nil; }
        };
        auto opaqueVariant = [&](const PolyParam& p, bool punch, unsigned variant) {
            bool secondaryOperation = p.tsp.SrcSelect || p.tsp.DstSelect;
            if (secondaryOperation) { flushColor(); if (p.tsp.SrcSelect) copySecondary(); }
            if (!colorEncoder) {
                colorCommand = newCommand();
                colorEncoder = [colorCommand renderCommandEncoderWithDescriptor:
                        makePass(false, false, p.tsp.DstSelect ? secondary : targets.output, replayDepth)];
                [colorEncoder setVertexBuffer:geometry.vertices offset:0 atIndex:0];
            }
            draw(colorEncoder,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,punch,false,false,false,nullptr,
                    p.tsp.SrcSelect ? secondaryCopy : nil,0,variant);
            if (secondaryOperation) flushColor();
        };
        auto opaqueColor = [&](const PolyParam& p, bool punch) {
            opaqueVariant(p,punch,p.pcw.Shadow ? 1 : 5);
            if (p.pcw.Shadow) {
                if (p.tsp1.full != (u32)-1) {
                    PolyParam area1=p; area1.tsp=p.tsp1; area1.tcw=p.tcw1; area1.texture=p.texture1;
                    opaqueVariant(area1,punch,3);
                } else opaqueVariant(p,punch,2);
            }
        };
        for (u32 i=prev.op_count;i<cur.op_count;i++) opaqueColor(renderContext->global_param_op[i],false);
        for (u32 i=prev.pt_count;i<cur.pt_count;i++) opaqueColor(renderContext->global_param_pt[i],true);
        flushColor();
        if (config::ModifierVolumes && cur.mvo_count > prev.mvo_count) {
            command = newCommand(); enc = [command renderCommandEncoderWithDescriptor:makePass(false, false)];
            modifiers.ClearStencil(enc,w,h); [enc endEncoding]; [command commit];
        }
        if (!cur.autosort) {
            // Keep the opaque modifier stencil snapshot while translucent depth
            // writes advance independently of the prepass attachment.
            command = newCommand();
            metalOITCopyDepthStencilAspect(device, command, targets.depth, replayDepth, w, h, false);
            [command commit];
            id<MTLCommandBuffer> trCommand = nil; id<MTLRenderCommandEncoder> trEncoder = nil;
            auto flushTranslucent = [&] {
                if (trEncoder) { [trEncoder endEncoding]; [trCommand commit]; trEncoder = nil; trCommand = nil; }
            };
            auto translucentVariant = [&](const PolyParam& p, unsigned variant) {
                bool secondaryOperation = p.tsp.SrcSelect || p.tsp.DstSelect;
                if (secondaryOperation) { flushTranslucent(); if (p.tsp.SrcSelect) copySecondary(); }
                if (!trEncoder) {
                    trCommand = newCommand(); trEncoder = [trCommand renderCommandEncoderWithDescriptor:
                            makePass(false, true, p.tsp.DstSelect ? secondary : targets.output, replayDepth)];
                    [trEncoder setVertexBuffer:geometry.vertices offset:0 atIndex:0];
                }
                draw(trEncoder,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,false,false,false,true,nullptr,
                        p.tsp.SrcSelect ? secondaryCopy : nil,0,variant);
                if (secondaryOperation) flushTranslucent();
            };
            for (u32 i=prev.tr_count;i<cur.tr_count;i++) {
                const auto& p=renderContext->global_param_tr[i];
                translucentVariant(p, p.pcw.Shadow ? 1 : 5);
                if (p.pcw.Shadow) {
                    if (p.tsp1.full != (u32)-1) {
                        PolyParam area1=p; area1.tsp=p.tsp1; area1.tcw=p.tcw1; area1.texture=p.texture1;
                        translucentVariant(area1,3);
                    } else translucentVariant(p,2);
                }
            }
            flushTranslucent();
            command = newCommand();
            metalOITCopyDepthStencilAspect(device, command, replayDepth, targets.depth, w, h, false);
            [command commit];
        } else if (cur.tr_count > prev.tr_count) {
            MetalOITBuffers& oit = oitBuffers;
            const int64_t budget = config::PixelBufferSize;
            if (budget <= 0) throw RendererException("Metal per-pixel fragment buffer budget is zero");
            oit.init(device,w,h,(size_t)budget,config::PerPixelLayers, FPU_SHAD_SCALE.scale_factor / 256.f);
            for (;;) {
                command = newCommand(); oit.clear(command);
                enc = [command renderCommandEncoderWithDescriptor:makePass(false, false)];
                [enc setVertexBuffer:geometry.vertices offset:0 atIndex:0];
                for (u32 i=prev.tr_count;i<cur.tr_count;i++) { const auto& p=renderContext->global_param_tr[i]; draw(enc,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,false,true,false,false,&oit,opaqueDepth,i); }
                [enc endEncoding]; finish(command);
                if (!oit.overflowed()) break;
                if (!oit.grow()) throw RendererException("Metal per-pixel fragment buffer exhausted; increase Pixel Buffer Size");
            }
            if (config::ModifierVolumes) {
                if (cur.mv_op_tr_shared)
                    oit.modifiers(queue, targets.output, targets.depth, *renderContext, matrices,
                            renderContext->global_param_mvo.data(), prev.mvo_count, cur.mvo_count - prev.mvo_count, track);
                else
                    oit.modifiers(queue, targets.output, targets.depth, *renderContext, matrices,
                            renderContext->global_param_mvo_tr.data(), prev.mvo_tr_count, cur.mvo_tr_count - prev.mvo_tr_count, track);
            }
            command = newCommand(); oit.resolve(command,targets.output,resolved,secondary,secondaryCopy);
            id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
            [copy copyFromTexture:resolved sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                      sourceSize:MTLSizeMake(w,h,1) toTexture:targets.output destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [copy copyFromTexture:secondaryCopy sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                      sourceSize:MTLSizeMake(w,h,1) toTexture:secondary destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [copy endEncoding]; finish(command);
            if (oit.layersExceeded() && !layerWarningShown) {
                layerWarningShown = true;
                WARN_LOG(RENDERER,"Metal per-pixel layer limit exceeded; nearest layers retained");
                os_notify("Per-pixel transparency layer limit reached", 6000, "Increase Maximum Layers to retain more transparent surfaces.");
            }
            if (&cur != &renderContext->render_passes.back() && config::TranslucentPolygonDepthMask) {
                command = newCommand(); enc = [command renderCommandEncoderWithDescriptor:makePass(false, true)];
                [enc setVertexBuffer:geometry.vertices offset:0 atIndex:0];
                for (u32 i=prev.tr_count;i<cur.tr_count;i++) { const auto& p=renderContext->global_param_tr[i];
                    if (!p.isp.ZWriteDis) draw(enc,geometry.indices,p,p.first,p.count,MTLPrimitiveTypeTriangleStrip,false,true,true,true);
                }
                [enc endEncoding]; [command commit];
            }
        }
        if (&cur != &renderContext->render_passes.back()) {
            command = newCommand();
            metalOITCopyDepthStencilAspect(device, command, targets.depth, replayDepth, w, h, false);
            [command commit];
        }
        prev = cur;
    }
    if (renderContext->isRTT && !config::RenderToTextureBuffer) {
        int rw,rh; getPvrFramebufferSize(*renderContext,rw,rh);
        if (rw>1024 || rh>1024) throw RendererException("Metal per-pixel render-to-texture exceeds PVR size");
        MetalTexture *target=textureCache.getRTTexture(renderContext->fb_W_SOF1 & VRAM_MASK,renderContext->fb_W_CTRL.fb_packmode,rw,rh);
        target->setTexture(targets.output); target->dirty=0; target->unprotectVRam();
    } else if (renderContext->isRTT || config::EmulateFramebuffer) {
        writeFramebuffer(newCommand(),targets.output);
    }
    if (renderContext->isRTT) { targets.end(true); return false; }
    int rawW,rawH; getPvrFramebufferSize(*renderContext,rawW,rawH); performance_overlay::recordRenderedResolution(rawW,rawH,w,h);
    if (!config::EmulateFramebuffer) { aspect=getOutputFramebufferAspectRatio(); targets.frameOutput=targets.output; frameReady=true; clearLastFrame=false; }
    else if (timing) pvrTimingEvidence.submitted(timing->epoch);
    return true;
    }
}

void MetalRenderer::writeFramebuffer(id<MTLCommandBuffer> command, id<MTLTexture> image) {
    const int w = (int)image.width, h = (int)image.height;
    const size_t stride = ((size_t)w * 4 + 255) & ~size_t(255);
    id<MTLBuffer> buffer = [MetalContext::Instance()->GetDevice() newBufferWithLength:stride * h options:MTLResourceStorageModeShared];
    if (!buffer) throw RendererException("Metal framebuffer readback allocation failed");
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buffer destinationOffset:0
       destinationBytesPerRow:stride destinationBytesPerImage:stride * h];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status == MTLCommandBufferStatusError)
        throw RendererException("Metal framebuffer readback failed");
    int targetW, targetH;
    Rect clip;
    if (renderContext->isRTT) {
        targetW = renderContext->framebufferWidth; targetH = renderContext->framebufferHeight;
        clip = renderContext->fbClip;
    } else {
        glm::ivec2 size; getWriteFBToVramParams(*renderContext, size, clip);
        targetW = size.x; targetH = size.y;
    }
    if (targetW <= 0 || targetH <= 0) return;
    std::vector<u8> pixels((size_t)targetW * targetH * 4);
    const u8 *source = static_cast<const u8 *>(buffer.contents);
    size_t sourceStride = stride;
    std::vector<u8> dithered;
    if (!renderContext->isRTT && config::EmulateFramebuffer && renderContext->fb_W_CTRL.fb_dither
            && renderContext->fb_W_CTRL.fb_packmode <= 3) {
        dithered.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            memcpy(dithered.data() + (size_t)y * w * 4, source + (size_t)y * stride, (size_t)w * 4);
        metalDitherFramebuffer(dithered.data(), w, h, renderContext->fb_W_CTRL.fb_packmode, true);
        source = dithered.data(); sourceStride = (size_t)w * 4;
    }
    if (targetW == w && targetH == h) {
        for (int y = 0; y < h; ++y)
            memcpy(pixels.data() + (size_t)y * w * 4, source + (size_t)y * sourceStride, (size_t)w * 4);
    } else for (int y = 0; y < targetH; ++y) for (int x = 0; x < targetW; ++x) {
        float sx = (x + 0.5f) * w / targetW - 0.5f, sy = (y + 0.5f) * h / targetH - 0.5f;
        int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
        float fx = sx - x0, fy = sy - y0;
        int xa = std::clamp(x0, 0, w - 1), xb = std::clamp(x0 + 1, 0, w - 1);
        int ya = std::clamp(y0, 0, h - 1), yb = std::clamp(y0 + 1, 0, h - 1);
        u8 *out = &pixels[((size_t)y * targetW + x) * 4];
        for (int c = 0; c < 4; ++c) {
            float a = source[(size_t)ya * sourceStride + xa * 4 + c] * (1 - fx) + source[(size_t)ya * sourceStride + xb * 4 + c] * fx;
            float b = source[(size_t)yb * sourceStride + xa * 4 + c] * (1 - fx) + source[(size_t)yb * sourceStride + xb * 4 + c] * fx;
            out[c] = (u8)std::clamp((int)std::lround(a * (1 - fy) + b * fy), 0, 255);
        }
    }
    u32 address = renderContext->fb_W_SOF1 & VRAM_MASK;
    u32 lineStride = renderContext->fb_W_LINESTRIDE * 8;
    if (renderContext->isRTT) {
        if (!lineStride) lineStride = targetW * 2;
        WriteTextureToVRam(targetW, targetH, pixels.data(), (u16 *)&vram[address], renderContext->fb_W_CTRL, lineStride, clip);
    } else {
        WriteFramebuffer(targetW, targetH, pixels.data(), address, renderContext->fb_W_CTRL, lineStride, clip);
    }
}

void MetalRenderer::RenderFramebuffer(const FramebufferInfo& info) {
    // Preserve a PVR sample submitted for this emulated frame; CPU-only video
    // must not advertise a GPU sample on its own.
    if (!pvrTimingEvidence.consume(performance_overlay::isGpuTimingRequested(), performance_overlay::getGpuTimingEpoch()))
        performance_overlay::setGpuTimingSupported(false);
    @autoreleasepool {
    PixelBuffer<u32> pixels;
    int w, h;
    u32 border = RGBAPacker::pack(info.vo_border_col._red, info.vo_border_col._green, info.vo_border_col._blue, 255);
    bool blank = !info.fb_r_ctrl.fb_enable || info.vo_control.blank_video;
    if (blank) { w = h = 1; } else ReadFramebuffer(info, pixels, w, h);
    if (w <= 0 || h <= 0) return;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = metalCpuTextureStorageMode(MetalContext::Instance()->GetDevice());
    id<MTLTexture> uploaded = [MetalContext::Instance()->GetDevice() newTextureWithDescriptor:d];
    [uploaded replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:blank ? &border : pixels.data() bytesPerRow:w * 4];
    targets.setFramebuffer(uploaded); aspect = getDCFramebufferAspectRatio(); frameReady = true; clearLastFrame = false;
    performance_overlay::recordRenderedResolution(w, h, w, h);
    }
}

bool MetalRenderer::Present() {
    if (!frameReady || clearLastFrame || !targets.frameOutput) return false;
    MetalContext::Instance()->PresentFrame(targets.frameOutput, aspect);
    frameReady = false;
    return true;
}
bool MetalRenderer::RenderLastFrame() {
    return !clearLastFrame;
}
} // namespace

Renderer *rend_Metal() { return new MetalRenderer; }
Renderer *rend_Metal_OIT() { return new MetalRenderer(true); }
