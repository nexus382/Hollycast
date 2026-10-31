// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise production MetalDriver with two queued ImGui frames and texture replacement.
#define METAL_CONTEXT_REAL_IMGUI
#define main context_basic_smoke_main
#include "context_smoke.mm"
#undef main

namespace config {
int loadInt(const std::string &, const std::string &, int fallback) { return fallback; }
void saveInt(const std::string &, const std::string &, int) {}
Option<bool, false> VideoRouting("routing", true);
Option<bool, false> VideoRoutingScale("routingScale", false);
Option<int, false> VideoRoutingVRes("routingHeight", 60);
}
static id<MTLBuffer> routed;
static NSUInteger routedWidth, routedHeight, stride;
void os_VideoRoutingTermMetal() {}
void os_VideoRoutingPublishMetalTexture(id<MTLDevice> device, id<MTLTexture> texture, id<MTLCommandBuffer> cmd)
{
 routedWidth=texture.width; routedHeight=texture.height; stride=(routedWidth*4+255)&~(NSUInteger)255;
 routed=[device newBufferWithLength:stride*routedHeight options:MTLResourceStorageModeShared];
 id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
 [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(routedWidth,routedHeight,1) toBuffer:routed destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride*routedHeight]; [blit endEncoding];
}
static bool isColor(int x,int y,int r,int g,int b)
{
 const u8 *p=(const u8 *)routed.contents+y*stride+x*4;
 return std::abs((int)p[2]-r)<5 && std::abs((int)p[1]-g)<5 && std::abs((int)p[0]-b)<5;
}
int main()
{
 @autoreleasepool {
  require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
  ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr; MetalContext::Create(nullptr,nullptr);
  ImGuiIO &io=ImGui::GetIO(); io.DisplaySize=ImVec2(settings.display.width,settings.display.height); io.DeltaTime=1.f/60.f;
  const u8 red[]={255,0,0,255}; const u8 green[]={0,255,0,255};
  ImTextureID first=imguiDriver->updateTexture("changing",red,1,1,true);
  imguiDriver->newFrame(); ImGui::NewFrame();
  ImGui::GetBackgroundDrawList()->AddImage(first,ImVec2(5,5),ImVec2(45,45));
  ImGui::GetBackgroundDrawList()->AddText(ImVec2(10,55),0xffffffff,"A");
  ImGui::Render(); imguiDriver->renderDrawData(ImGui::GetDrawData(),false);
  ImTextureID second=imguiDriver->updateTexture("changing",green,1,1,true);
  imguiDriver->newFrame(); ImGui::NewFrame();
  ImGui::GetBackgroundDrawList()->AddImage(second,ImVec2(55,5),ImVec2(95,45));
  ImGui::GetBackgroundDrawList()->AddText(ImVec2(60,55),0xffffffff,"B");
  ImGui::Render(); imguiDriver->renderDrawData(ImGui::GetDrawData(),true);
  imguiDriver->deleteTexture("changing");
  config::VSync=false; imguiDriver->present(); MetalContext::Instance()->WaitIdle();
  require(routed && isColor(25,25,255,0,0),"first frame and retired texture retained");
  require(isColor(75,25,0,255,0),"second frame and replacement texture retained");
  auto hasText=[](int x0,int y0) { for(int y=y0;y<y0+20;y++) for(int x=x0;x<x0+20;x++) { const u8 *p=(const u8*)routed.contents+y*stride+x*4; if(p[0]>40 && p[1]>40 && p[2]>40) return true; } return false; };
  require(hasText(10,55) && hasText(60,55),"dynamic font atlas survives both snapshots");
  GraphicsContext::Term(); ImGui::DestroyContext(); SDL_DestroyWindow(testWindow);SDL_Quit();
  puts("Metal ImGui multi-frame queue, texture retirement and font atlas passed");
 }
}
