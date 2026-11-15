// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Production Process/Render integration with pre-parsed geometry and GPU readback.
// Run through renderer_entry_regression.py after building a TEST_AUTOMATION app.
// TA parsing, ImGui presentation, and timing observers are replaced; renderer logic is real.
#include "rend/metal/metal_renderer.mm"
#include <SDL.h>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <mutex>
namespace {
std::atomic<bool> observeTiming{false};
std::atomic<bool> timingSupported{false};
std::mutex timingMutex;
std::condition_variable timingReady;
uint64_t timingNanoseconds = 0;
uint32_t timingEpoch = 0;
constexpr uint32_t expectedEpoch = 47;
}
namespace performance_overlay {
bool isGpuTimingRequested() { return observeTiming.load(); }
uint32_t getGpuTimingEpoch() { return expectedEpoch; }
void setGpuTimingSupported(bool supported) { timingSupported.store(supported); }
void recordGpuTimingSample(uint64_t nanoseconds, uint32_t epoch) {
    { std::lock_guard<std::mutex> lock(timingMutex); timingNanoseconds = nanoseconds; timingEpoch = epoch; }
    timingReady.notify_one();
}
}
void ta_parse(TA_context*, bool) {}
void metalInitImGui() {}
void metalDrawImGui(id<MTLRenderCommandEncoder>, int, int) {}

// Feed exact RGBA8 bytes through the production Metal readback and compare the
// entire guest VRAM image, including untouched row padding. This reference
// neither calls the production dither/packing helpers nor reads their output.
static bool framebufferReadbackBytes(MetalRenderer& renderer, std::vector<u8>& guestVram) {
    auto device = MetalContext::Instance()->GetDevice();
    auto queue = MetalContext::Instance()->GetQueue();
    auto *savedContext = renderer.renderContext;
    const bool savedEmulation = config::EmulateFramebuffer;
    rend_context sample{};
    renderer.renderContext = &sample;
    config::EmulateFramebuffer = true;
    struct Restore {
        MetalRenderer& renderer;
        rend_context *context;
        bool emulate;
        ~Restore() { renderer.renderContext = context; config::EmulateFramebuffer = emulate; }
    } restore {renderer, savedContext, savedEmulation};

    struct Case { int width, height; bool dither, scale; bool rtt = false; };
    const Case cases[] = {
        {17, 5, false, false}, // padded Metal rows and odd guest width
        {64, 5, false, false}, // no Metal row padding
        {65, 3, false, false}, // overlapping source/destination during compaction
        {17, 5, true, false},
        {17, 5, false, true},  // scaled path reads directly from padded rows
        {17, 5, true, true},   // compact, dither, then scale
        {17, 5, false, false, true}, // RTT packing rounds to the nearest representable color
        {64, 5, true, true, true},   // RTT uses the linear guest VRAM address path
    };
    unsigned checks = 0;
    for (const auto& test : cases) for (unsigned mode = 0; mode <= 6; ++mode) {
        if ((test.dither || test.rtt) && mode > 3) continue;
        const int w = test.width, h = test.height;
        const int outW = test.scale ? 2 * w : w, outH = test.scale ? 2 * h : h;
        const unsigned bytesPerPixel = mode <= 3 ? 2 : mode == 4 ? 3 : 4;
        const unsigned lineStride = (outW * bytesPerPixel + 7) & ~7u;
        constexpr unsigned address = 0x1000, kval = 0xa7, alphaThreshold = 128;
        std::vector<u8> rgba((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            u8 *p = &rgba[((size_t)y * w + x) * 4];
            p[0] = u8(17 * x + 43 * y + checks * 11);
            p[1] = u8(53 * x + 29 * y + checks * 7);
            p[2] = u8(97 * x + 13 * y + checks * 3);
            p[3] = u8(37 * x + 17 * y + 127);
        }
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:w height:h mipmapped:NO];
        descriptor.storageMode = metalCpuTextureStorageMode(device);
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> image = [device newTextureWithDescriptor:descriptor];
        if (!image) return false;
        [image replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
                withBytes:rgba.data() bytesPerRow:w * 4];

        sample.isRTT = test.rtt;
        sample.globClip = {outW, outH};
        sample.framebufferWidth = outW; sample.framebufferHeight = outH;
        sample.fbClip = Rect({0, 0}, {outW, outH}); sample.tileClip = sample.fbClip;
        sample.scaler_ctl.vscalefactor = 1024;
        sample.fb_W_SOF1 = address; sample.fb_W_LINESTRIDE = lineStride / 8;
        sample.fb_W_CTRL.full = 0; sample.fb_W_CTRL.fb_packmode = mode;
        sample.fb_W_CTRL.fb_dither = test.dither;
        sample.fb_W_CTRL.fb_kval = kval; sample.fb_W_CTRL.fb_alpha_threshold = alphaThreshold;
        std::fill(guestVram.begin(), guestVram.end(), u8(0xa5));
        renderer.writeFramebuffer([queue commandBuffer], image);

        // PVR's threshold matrix, evaluated against the original tightly packed
        // bytes rather than any Metal staging storage.
        if (test.dither && !test.rtt) {
            const unsigned threshold[4][4] = {{5,13,7,15}, {9,1,11,3}, {6,14,4,12}, {10,2,8,0}};
            const unsigned divisor[3] = {mode == 2 ? 1u : 2u,
                    mode == 2 ? 1u : mode == 1 ? 4u : 2u, mode == 2 ? 1u : 2u};
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                u8 *p = &rgba[((size_t)y * w + x) * 4];
                for (int c = 0; c < 3; ++c)
                    p[c] = u8(std::min(255u, unsigned(p[c]) + threshold[y % 4][x % 4] / divisor[c]));
                p[3] = u8(std::min(255u, unsigned(p[3]) + 1));
            }
        }
        std::vector<u8> referenceVram(guestVram.size(), 0xa5);
        for (int y = 0; y < outH; ++y) for (int x = 0; x < outW; ++x) {
            unsigned p[4];
            for (int c = 0; c < 4; ++c) {
                if (!test.scale) p[c] = rgba[((size_t)y * w + x) * 4 + c];
                else {
                    // At exactly 2x, pixel centers land at quarter coordinates.
                    // Integer weights make an independent, exact bilinear oracle.
                    const int nx = 2 * x - 1, ny = 2 * y - 1;
                    const int x0 = nx < 0 ? -1 : nx / 4, y0 = ny < 0 ? -1 : ny / 4;
                    const unsigned fx = nx - 4 * x0, fy = ny - 4 * y0;
                    unsigned sum = 0;
                    for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                        const int sx = std::clamp(x0 + i, 0, w - 1), sy = std::clamp(y0 + j, 0, h - 1);
                        sum += rgba[((size_t)sy * w + sx) * 4 + c] * (i ? fx : 4 - fx) * (j ? fy : 4 - fy);
                    }
                    p[c] = (sum + 8) / 16;
                }
            }
            unsigned packed;
            if (test.rtt && !test.dither) {
                // Bias each quantized channel by half a step before truncation.
                for (int c = 0; c < (mode == 2 ? 4 : 3); ++c) {
                    const unsigned halfStep = mode == 2 ? 8 : mode == 1 && c == 1 ? 2 : 4;
                    p[c] = std::min(255u, p[c] + halfStep);
                }
            }
            switch (mode) {
            case 0: packed = ((kval & 128) << 8) | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3); break;
            case 1: packed = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3); break;
            case 2: packed = ((p[3] >> 4) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4); break;
            case 3: packed = (p[3] >= alphaThreshold ? 0x8000 : 0) | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3); break;
            default: packed = (p[0] << 16) | (p[1] << 8) | p[2]
                    | ((mode == 5 ? kval : mode == 6 ? p[3] : 0) << 24); break;
            }
            for (unsigned byte = 0; byte < bytesPerPixel; ++byte) {
                unsigned guest = address + y * lineStride + x * bytesPerPixel + byte;
                // Interleave the two 4 MiB banks in 32-bit words. Do not use
                // pvr_map32: the reference also checks the guest address layout.
                unsigned physical = ((guest & 0x3ffffc) << 1) | ((guest & 0x400000) >> 20) | (guest & 3);
                referenceVram[test.rtt ? guest : physical] = u8(packed >> (8 * byte));
            }
        }
        if (guestVram != referenceVram) {
            auto difference = std::mismatch(guestVram.begin(), guestVram.end(), referenceVram.begin());
            fprintf(stderr, "Metal readback %dx%d mode %u dither %d scale %d: VRAM byte %zu got %02x expected %02x\n",
                    w, h, mode, int(test.dither), int(test.scale), size_t(difference.first - guestVram.begin()),
                    unsigned(*difference.first), unsigned(*difference.second));
            return false;
        }
        ++checks;
    }
    fprintf(stderr, "Metal framebuffer readback: %u complete VRAM comparisons passed\n", checks);
    return true;
}

// Queue two complete scenes before either can read its uploaded geometry.
static bool queuedSceneColors(MetalRenderer& renderer, TA_context& ctx) {
    auto device = MetalContext::Instance()->GetDevice();
    auto queue = MetalContext::Instance()->GetQueue();
    id<MTLSharedEvent> gate = [device newSharedEvent];
    if (!gate) return false;
    id<MTLCommandBuffer> block = [queue commandBuffer];
    [block encodeWaitForEvent:gate value:1];
    [block commit];
    // A future synchronous Render() must fail the test without wedging the queue.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        if (gate.signaledValue == 0) gate.signaledValue = 1;
    });

    id<MTLBuffer> samples[2];
    id<MTLCommandBuffer> copies[2];
    bool rendered = true;
    for (unsigned frame = 0; frame < 2; ++frame) {
        for (auto& vertex : ctx.rend.verts) {
            vertex.col[0] = frame == 0 ? 255 : 0;
            vertex.col[1] = frame == 1 ? 255 : 0;
            vertex.col[2] = 0;
            vertex.col[3] = 255;
        }
        renderer.Process(&ctx);
        if (!renderer.Render()) { rendered = false; break; }
        id<MTLTexture> output = renderer.targets.frameOutput;
        samples[frame] = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        copies[frame] = [queue commandBuffer];
        id<MTLBlitCommandEncoder> copy = [copies[frame] blitCommandEncoder];
        [copy copyFromTexture:output sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(output.width / 2, output.height / 2, 0) sourceSize:MTLSizeMake(1, 1, 1)
                toBuffer:samples[frame] destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
        [copy endEncoding];
        [copies[frame] commit];
    }
    const bool queuedBeforeExecution = gate.signaledValue == 0;
    gate.signaledValue = 1;
    if (!rendered || !queuedBeforeExecution) return false;
    [copies[1] waitUntilCompleted];
    for (unsigned frame = 0; frame < 2; ++frame) {
        const auto *pixel = static_cast<const u8 *>(samples[frame].contents);
        if (copies[frame].status != MTLCommandBufferStatusCompleted
                || pixel[0] != (frame == 0 ? 255 : 0) || pixel[1] != (frame == 1 ? 255 : 0) || pixel[2] != 0)
            return false;
    }
    return true;
}
int main() { @autoreleasepool {
 setenv("SDL_MAC_BACKGROUND_APP", "1", 1); setenv("HOLLYCAST_TEST_BACKGROUND", "1", 1);
 if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
 MetalContext::Create(nullptr,nullptr);
 void *window=nullptr, *display=nullptr; MetalContext::Instance()->getWindow(&window,&display);
 if (!(SDL_GetWindowFlags(static_cast<SDL_Window *>(window)) & SDL_WINDOW_HIDDEN)) { fprintf(stderr,"fixture window is not hidden\n"); return 1; }
 config::RendererType=RenderType::Metal_OIT;
 MetalRenderer r(true);
 if(!r.Init()) return 2;
 TA_context ctx{};
 auto &scene=ctx.rend;
 scene.scaler_ctl.vscalefactor=1024; scene.globClip={64,64}; scene.fbClip=Rect({0,0},{64,64}); scene.tileClip=scene.fbClip;
 scene.framebufferWidth=64;scene.framebufferHeight=64; scene.fZ_max=1; scene.clearFramebuffer=true;
 Vertex a{}; a.x=0;a.y=0;a.z=1;a.col[0]=255;a.col[3]=255;
 Vertex b=a;b.x=64; Vertex c=a;c.y=64; Vertex d=b;d.y=64;
 scene.verts={a,b,c,d};scene.idx={0,1,2,3};
 PolyParam p{};p.init();p.first=0;p.count=4;p.isp.DepthMode=7;p.tsp.SrcInstr=1;p.tsp.DstInstr=0;p.pcw.Gouraud=1;
 scene.global_param_op.push_back(p); RenderPass pass{};pass.op_count=1;scene.render_passes.push_back(pass);
 // Shadow-enabled blended foreground over a shadow-disabled opaque background.
 for (auto v : std::vector<Vertex>{a,b,c,d}) { v.z=2; v.col[0]=0; v.col[2]=255; v.col[3]=128; scene.verts.push_back(v); }
 scene.idx.insert(scene.idx.end(),{4,5,6,7});
 PolyParam front=p; front.first=4; front.isp.DepthMode=4; front.pcw.Shadow=1;front.tsp.UseAlpha=1;front.tsp.SrcInstr=4;front.tsp.DstInstr=5;
 scene.global_param_op[0].isp.DepthMode=4; scene.global_param_op.push_back(front); scene.render_passes[0].op_count=2;
 config::ModifierVolumes=true; FPU_SHAD_SCALE.scale_factor=128;
 scene.modtrig={{0,0,4,64,0,4,0,64,4},{64,0,4,64,64,4,0,64,4}};
 ModifierVolumeParam mv{};mv.init();mv.first=0;mv.count=2;mv.isp.DepthMode=1;
 scene.global_param_mvo.push_back(mv);scene.render_passes[0].mvo_count=1;
 auto runRenderer=[&](MetalRenderer& renderer, const char *label, int er, int eg, int eb) {
   renderer.Process(&ctx); if(!renderer.Render()) { fprintf(stderr,"%s render failed\n",label); return false; }
   id<MTLTexture> t=renderer.targets.frameOutput;
   id<MTLDevice> dev=MetalContext::Instance()->GetDevice();
   id<MTLBuffer> buf=[dev newBufferWithLength:t.width*t.height*4 options:MTLResourceStorageModeShared];
   id<MTLCommandBuffer> cmd=[MetalContext::Instance()->GetQueue() commandBuffer]; id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
   [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:t.width*4 destinationBytesPerImage:t.width*t.height*4]; [blit endEncoding];[cmd commit];[cmd waitUntilCompleted];
   auto px=(unsigned char*)buf.contents + ((t.height/2)*t.width+t.width/2)*4;
   fprintf(stderr,"%s: %u,%u,%u,%u\n",label,px[0],px[1],px[2],px[3]);
   return cmd.status==MTLCommandBufferStatusCompleted && abs((int)px[0]-er)<4 && abs((int)px[1]-eg)<4 && abs((int)px[2]-eb)<4;
 };
 auto run=[&](const char *label, int er, int eg, int eb) { return runRenderer(r,label,er,eg,eb); };
 bool ok=run("opaque blended shadow",127,0,64);
 scene.global_param_op.pop_back(); scene.global_param_tr.push_back(front); scene.render_passes[0].op_count=1;scene.render_passes[0].tr_count=1;
 scene.global_param_op[0].pcw.Shadow=1;
 ok=run("unsorted translucent shadow",64,0,64) && ok;
 // Two-volume translucent polygons select the second vertex color in the modifier.
 for (size_t i=4;i<8;i++) { scene.verts[i].col1[1]=255;scene.verts[i].col1[3]=128; }
 scene.global_param_tr[0].tsp1=front.tsp;
 ok=run("unsorted translucent second volume",64,128,0) && ok;
 // The translucent list runs the OR and inclusion encoders in order before
 // resolving the shadowed second volume into the primary color.
 scene.render_passes[0].autosort=true;
 scene.render_passes[0].mv_op_tr_shared=true;
 ok=run("sorted translucent second volume with ordered modifiers",64,128,0) && ok;
 scene.render_passes[0].autosort=false;
 scene.render_passes[0].mv_op_tr_shared=false;
 scene.global_param_op[0].pcw.Shadow=0;
 scene.global_param_tr.clear();scene.render_passes[0].tr_count=0;
 for (auto v : std::vector<Vertex>{a,b,c,d}) { v.z=3;v.col[0]=0;v.col[1]=255; scene.verts.push_back(v); }
 scene.idx.insert(scene.idx.end(),{8,9,10,11});
 PolyParam green=p;green.first=8;green.isp.DepthMode=4;green.isp.ZWriteDis=1;
 scene.global_param_op.push_back(green);scene.global_param_op.push_back(front);scene.render_passes[0].op_count=3;
 ok=run("no-depth-write opaque",0,127,64) && ok;
 // A closer sorted translucent layer must write depth before the next pass
 // when the translucent depth-mask option is enabled.
 for (size_t i=4;i<8;i++) scene.verts[i].z=3;
 for (size_t i=8;i<12;i++) scene.verts[i].z=2;
 front.pcw.Shadow=0; green.isp.ZWriteDis=0;
 scene.global_param_op={p,green}; scene.global_param_op[0].isp.DepthMode=4;
 scene.global_param_tr={front}; scene.global_param_mvo.clear();
 RenderPass first{};first.op_count=1;first.tr_count=1;first.autosort=true;
 RenderPass second=first;second.op_count=2;second.autosort=false;
 scene.render_passes={first,second}; config::ModifierVolumes=false;
 config::TranslucentPolygonDepthMask=true;
 ok=run("sorted translucent depth mask",127,0,128) && ok;
 config::TranslucentPolygonDepthMask=false;
 ok=run("disabled translucent depth mask",0,255,0) && ok;
 // Observe production OIT command completion, including the framebuffer path.
 observeTiming.store(true);
 { std::lock_guard<std::mutex> lock(timingMutex); timingNanoseconds=0; timingEpoch=0; }
 r.Process(&ctx);
 ok=r.Render() && ok;
 MetalContext::Instance()->WaitIdle();
 {
   std::unique_lock<std::mutex> lock(timingMutex);
   const bool sample=timingReady.wait_for(lock,std::chrono::seconds(5),[]{return timingNanoseconds != 0;});
   fprintf(stderr,"OIT GPU timing: %llu ns, epoch %u, supported %d\n",(unsigned long long)timingNanoseconds,timingEpoch,(int)timingSupported.load());
   ok=(sample && timingEpoch==expectedEpoch && timingSupported.load()) && ok;
 }
 // Full framebuffer emulation writes its PVR result into an isolated VRAM
 // buffer; the following blank upload consumes the same-frame timing marker.
 std::vector<u8> localVram(8*1024*1024);
 settings.platform.vram_size = localVram.size();
 settings.platform.vram_mask = settings.platform.vram_size - 1;
 vram.setRegion(localVram.data(), localVram.size());
 scene.fb_W_LINESTRIDE=16; config::EmulateFramebuffer=true;
 { std::lock_guard<std::mutex> lock(timingMutex); timingNanoseconds=0; timingEpoch=0; }
 r.Process(&ctx); ok=r.Render() && ok;
 MetalContext::Instance()->WaitIdle();
 {
   std::unique_lock<std::mutex> lock(timingMutex);
   const bool sample=timingReady.wait_for(lock,std::chrono::seconds(5),[]{return timingNanoseconds != 0;});
   ok=(sample && timingEpoch==expectedEpoch && timingSupported.load()) && ok;
 }
 FramebufferInfo blank{};
 r.RenderFramebuffer(blank);
 bool preserved=timingSupported.load();
 r.RenderFramebuffer(blank);
 bool unsupported=!timingSupported.load();
 fprintf(stderr,"framebuffer timing preserved %d; CPU-only unsupported %d\n",(int)preserved,(int)unsupported);
 ok=(preserved && unsupported) && ok;
 ok=framebufferReadbackBytes(r,localVram) && ok;
 // Render a red RTT, then sample the cache-owned texture in a screen draw.
 observeTiming.store(false); config::EmulateFramebuffer=false; config::RenderToTextureBuffer=false;
 scene.isRTT=true; scene.global_param_op={p}; scene.global_param_op[0].isp.DepthMode=4;
 scene.global_param_tr.clear(); RenderPass rttPass{};rttPass.op_count=1; scene.render_passes={rttPass};
 r.Process(&ctx); bool rttResult=r.Render();
 MetalTexture *rtt=r.textureCache.getRTTexture(scene.fb_W_SOF1 & VRAM_MASK,scene.fb_W_CTRL.fb_packmode,64,64);
 if (rttResult || !rtt || !rtt->texture() || rtt->dirty) { fprintf(stderr,"RTT cache target missing\n"); ok=false; }
 else {
   scene.isRTT=false;
   scene.verts[0].u=0;scene.verts[0].v=0; scene.verts[1].u=1;scene.verts[1].v=0;
   scene.verts[2].u=0;scene.verts[2].v=1; scene.verts[3].u=1;scene.verts[3].v=1;
   scene.global_param_op[0].pcw.Texture=1; scene.global_param_op[0].texture=rtt;
   ok=run("RTT cache sampled by screen",255,0,0) && ok;
 }
 scene.isRTT=false;scene.global_param_op={p};
 bool queued=queuedSceneColors(r,ctx);
 fprintf(stderr,"queued OIT scene geometry preserves both colors: %d\n",int(queued));
 ok=queued && ok;
 r.Term();
 config::RendererType=RenderType::Metal;
 MetalRenderer regular;
 if(!regular.Init()) return 2;
 scene.verts={a,b,c,d}; scene.idx={0,1,2,3};
 for(auto v:std::vector<Vertex>{a,b,c,d}) { v.z=3;v.col[0]=0;v.col[1]=128;scene.verts.push_back(v); }
 for(auto v:std::vector<Vertex>{a,b,c,d}) { v.z=2;v.col[0]=0;v.col[2]=255;scene.verts.push_back(v); }
 scene.idx.insert(scene.idx.end(),{4,5,6,7,8,9,10,11,4,5,6,5,7,6});
 scene.sortedTriangles.clear(); scene.global_param_tr.clear(); scene.global_param_pt.clear(); scene.global_param_mvo.clear();
 config::ModifierVolumes=false; config::TranslucentPolygonDepthMask=false; config::PerStripSorting=false;
 PolyParam secondary=p; secondary.first=4;secondary.tsp.DstSelect=1;
 PolyParam readSecondary=p;readSecondary.first=8;readSecondary.tsp.SrcSelect=1;
 PolyParam blue=p;blue.first=8;
 RenderPass regularPass{};regularPass.op_count=2;
 scene.global_param_op={p,secondary};scene.render_passes={regularPass};
 ok=runRenderer(regular,"regular secondary preserves primary",255,0,0) && ok;
 scene.global_param_op.push_back(readSecondary);scene.render_passes[0].op_count=3;
 ok=runRenderer(regular,"regular secondary source replaces vertex color",0,128,0) && ok;
 PolyParam accumulate=readSecondary;accumulate.tsp.DstSelect=1;accumulate.tsp.DstInstr=1;
 scene.global_param_op={p,secondary,accumulate,readSecondary};scene.render_passes[0].op_count=4;
 ok=runRenderer(regular,"regular secondary read modify write",0,255,0) && ok;
 scene.global_param_op={p,readSecondary};scene.render_passes[0].op_count=2;
 ok=runRenderer(regular,"regular secondary cleared each frame",0,0,0) && ok;
 PolyParam secondaryBlue=blue;secondaryBlue.tsp.DstSelect=1;
 PolyParam addSecondary=readSecondary;addSecondary.tsp.DstInstr=1;
 scene.global_param_op={p,secondary,readSecondary,secondaryBlue};scene.global_param_tr={addSecondary};
 RenderPass regularFirst{};regularFirst.op_count=3;
 RenderPass regularSecond=regularFirst;regularSecond.op_count=4;regularSecond.tr_count=1;
 scene.render_passes={regularFirst,regularSecond};
 ok=runRenderer(regular,"regular secondary submission order across passes",0,128,255) && ok;
 PolyParam behind=blue;behind.isp.DepthMode=4;
 scene.global_param_op={p,secondary,behind};scene.global_param_tr.clear();
 scene.render_passes={regularPass};scene.render_passes[0].op_count=3;
 ok=runRenderer(regular,"regular secondary shares depth",255,0,0) && ok;
 scene.global_param_op={p,secondary};scene.global_param_tr={accumulate};scene.global_param_op.push_back(behind);
 scene.sortedTriangles={{0,12,6}};
 regularFirst.op_count=2;regularFirst.tr_count=1;regularFirst.sorted_tr_count=1;regularFirst.autosort=true;
 regularSecond=regularFirst;regularSecond.op_count=3;regularSecond.autosort=false;
 scene.render_passes={regularFirst,regularSecond};
 scene.global_param_op[1].isp.ZWriteDis=1;config::TranslucentPolygonDepthMask=true;
 ok=runRenderer(regular,"regular secondary depth-only replay preserves primary",255,0,0) && ok;
 config::TranslucentPolygonDepthMask=false;
 ok=runRenderer(regular,"regular secondary disabled depth-only replay",0,0,255) && ok;
 scene.global_param_op={p,secondary};scene.global_param_tr.clear();scene.sortedTriangles.clear();
 scene.render_passes={regularPass};scene.render_passes[0].mvo_count=1;
 scene.global_param_op[0].pcw.Shadow=1;scene.global_param_op[1].pcw.Shadow=1;
 scene.global_param_mvo={mv};config::ModifierVolumes=true;
 ok=runRenderer(regular,"regular secondary modifier targets primary",128,0,0) && ok;
 regular.Term(); delete MetalContext::Instance(); SDL_Quit();return ok ? 0 : 5;
}}
