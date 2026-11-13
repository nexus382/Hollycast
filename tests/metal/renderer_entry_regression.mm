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
 std::vector<u8> localVram(8*1024*1024); vram.setRegion(localVram.data(), localVram.size());
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
