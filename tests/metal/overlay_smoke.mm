// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Render the production VMU/crosshair overlay into an offscreen Metal texture.
#include "rend/metal/metal_overlay.h"
#include "rend/osd.h"
#include "cfg/option.h"
#include <cstdio>
#include <cstdlib>

settings_t settings{};
u32 vmu_lcd_data[8][48*32]{};
bool vmu_lcd_status[8]{};
u64 vmuLastChanged[8]{};
namespace config {
void deleteEntry(const std::string&, const std::string&) {}
bool isTransient(const std::string&, const std::string&) { return false; }
bool loadBool(const std::string&, const std::string&, bool fallback) { return fallback; }
void saveBool(const std::string&, const std::string&, bool) {}
int loadInt(const std::string&, const std::string&, int fallback) { return fallback; }
void saveInt(const std::string&, const std::string&, int) {}
std::array<Option<int>,4> CrosshairColor{Option<int>("c0",0),Option<int>("c1",0),Option<int>("c2",0),Option<int>("c3",0)};
Option<int> CrosshairSize("crosshairSize",16);
std::array<Option<MapleDeviceType>,4> MapleMainDevices{Option<MapleDeviceType>("m0",MDT_LightGun),Option<MapleDeviceType>("m1",MDT_None),Option<MapleDeviceType>("m2",MDT_None),Option<MapleDeviceType>("m3",MDT_None)};
}
static bool ownsFirst=false;
namespace vmu_window { bool ownsScreen(int index) { return index==0 && ownsFirst; } }
int getScaledTopInset() { return 5; }
const u32 *getCrosshairTextureData() { static u32 white[16*16]; std::fill_n(white,16*16,0xffffffff); return white; }
std::pair<float,float> getCrosshairPosition(int) { return {140.f,50.f}; }
static void require(bool ok,const char *message) { if(!ok) { fprintf(stderr,"FAIL: %s\n",message); std::exit(1); } }
int main() { @autoreleasepool {
 id<MTLDevice> device=MTLCreateSystemDefaultDevice(); require(device!=nil,"Metal device");
 id<MTLCommandQueue> queue=[device newCommandQueue]; MetalOverlay overlay(device);
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:200 height:160 mipmapped:NO];
 d.storageMode=MTLStorageModePrivate; d.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> target=[device newTextureWithDescriptor:d]; NSUInteger stride=1024;
 id<MTLBuffer> result=[device newBufferWithLength:stride*160 options:MTLResourceStorageModeShared];
 auto draw=[&](bool vmus,bool crosshair) {
  id<MTLCommandBuffer> cmd=[queue commandBuffer]; MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture=target; pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
  id<MTLRenderCommandEncoder> enc=[cmd renderCommandEncoderWithDescriptor:pass];overlay.draw(enc,200,160,1.f,vmus,crosshair);[enc endEncoding];
  id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder]; [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(200,160,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride*160];[blit endEncoding];[cmd commit];[cmd waitUntilCompleted];require(cmd.status==MTLCommandBufferStatusCompleted,"overlay GPU command");
 };
 auto rgb=[&](int x,int y,int c) -> int { const u8 *p=(u8 *)result.contents+y*stride+x*4;return p[2-c]; };
 settings.platform.system=DC_PLATFORM_DREAMCAST; vmu_lcd_status[0]=true;vmuLastChanged[0]=1;std::fill_n(vmu_lcd_data[0],48*32,0xffffffff);
 config::CrosshairColor[0]=0xff0000ff; draw(true,true);
 require(rgb(20,20,0)>135 && rgb(20,20,0)<150,"white VMU tint and alpha");
 require(rgb(140,50,0)>240 && rgb(140,50,1)<10,"red crosshair position and color");
 ownsFirst=true;draw(true,true); require(rgb(20,20,0)==0,"detached VMU suppressed");
 ownsFirst=false;std::fill_n(vmu_lcd_data[0],48*32,0xff000000);vmuLastChanged[0]=2;draw(true,false);require(rgb(20,20,0)==0,"VMU changes refreshed");
 std::fill_n(vmu_lcd_data[0],48*32,0xffffffff);vmuLastChanged[0]=3;settings.platform.system=DC_PLATFORM_NAOMI;draw(true,false);require(rgb(20,20,0)==0,"arcade VMU suppressed");
 settings.platform.system=DC_PLATFORM_DREAMCAST; draw(false,false);require(rgb(140,50,0)==0 && rgb(20,20,0)==0,"disabled overlay suppressed");
 puts("Metal VMU tint, refresh, ownership, platform and crosshair checks passed");
} }
