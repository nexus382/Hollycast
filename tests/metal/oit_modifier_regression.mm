// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Tests actual per-pixel modifier shader with overlapping geometry and
// separate GPU encoder boundaries between dependent operations.
#include "hw/pvr/Renderer_if.h"
#include "rend/metal/metal_oit.h"
#include "rend/metal/metal_oit_shaders.h"
#include "rend/metal/metal_modvol_shaders.h"
#include <simd/simd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
static void require(bool b, const char *m) { if(!b){std::fprintf(stderr,"FAIL: %s\n",m);std::exit(1);} }
struct alignas(16) ModUniforms { simd_float4x4 matrix; simd_float4 clip,values; simd_uint4 flags; };
int main(){ @autoreleasepool {
 id<MTLDevice> d=MTLCreateSystemDefaultDevice(); require(d!=nil,"Metal device"); id<MTLCommandQueue> q=[d newCommandQueue]; NSError *error=nil;
 std::string source=std::string(metalOITShaderSource)+metalModifierShaderSource+metalOITModifierShaderSource;
 id<MTLLibrary> l=[d newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
 if(!l){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new]; p.vertexFunction=[l newFunctionWithName:@"modifierVertex"];p.fragmentFunction=[l newFunctionWithName:@"oitModifier"];
 p.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;p.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
 id<MTLRenderPipelineState> pipeline=[d newRenderPipelineStateWithDescriptor:p error:&error]; require(pipeline!=nil,"modifier pipeline");
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> color=[d newTextureWithDescriptor:td]; id<MTLBuffer> heads=[d newBufferWithLength:4 options:MTLResourceStorageModeShared];
 id<MTLBuffer> nodes=[d newBufferWithLength:sizeof(MetalOITNode) options:MTLResourceStorageModeShared];
 id<MTLBuffer> ctl=[d newBufferWithLength:sizeof(MetalOITControl) options:MTLResourceStorageModeShared];
 *static_cast<uint32_t*>(heads.contents)=0; auto *node=static_cast<MetalOITNode*>(nodes.contents); *node={0,0,0.1f,0xffffffffu,0,0,0,0,4};
 *static_cast<MetalOITControl*>(ctl.contents)={1,0,0,1,1,1,8,0.5f};
 const float vertices[]={-1,-1,1, 3,-1,1, -1,3,1, -1,-1,1, 3,-1,1, -1,3,1};
 id<MTLBuffer> vb=[d newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
 ModUniforms u{};u.matrix=matrix_identity_float4x4;
 auto run=[&](unsigned mode,int vertexCount,id<MTLBuffer> geometry=nil){
  id<MTLCommandBuffer> cb=[q commandBuffer];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionDontCare;pass.colorAttachments[0].storeAction=MTLStoreActionDontCare;
  id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:pass];[e setRenderPipelineState:pipeline];[e setVertexBuffer:geometry ? geometry : vb offset:0 atIndex:0];
  [e setVertexBytes:&u length:sizeof(u) atIndex:1];[e setFragmentBytes:&u length:sizeof(u) atIndex:1];[e setFragmentBytes:&mode length:sizeof(mode) atIndex:2];
  [e setFragmentBuffer:nodes offset:0 atIndex:3];[e setFragmentBuffer:heads offset:0 atIndex:4];[e setFragmentBuffer:ctl offset:0 atIndex:5];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertexCount];[e endEncoding];[cb commit];[cb waitUntilCompleted];
  require(cb.status==MTLCommandBufferStatusCompleted,"modifier command");
 };
 for(int repeat=0;repeat<20;repeat++){
  node->flags=4;run(0,6);require(node->flags==4,"two overlapping XOR volumes cancel");
  run(1,6);require(node->flags==5,"overlapping OR sets stencil");
  run(2,3);require(node->flags==6,"inclusion promotes stencil to accumulated shadow");
  run(3,3);require(node->flags==6,"exclusion preserves accumulated shadow outside current volume");
  const uint32_t exclusionExpected[4]={4,4,6,4};
  for (uint32_t bits=0;bits<4;bits++) {node->flags=4|bits;run(3,3);require(node->flags==exclusionExpected[bits],"exclusion truth table");}
  const uint32_t inclusionExpected[4]={4,6,6,6};
  for (uint32_t bits=0;bits<4;bits++) {node->flags=4|bits;run(2,3);require(node->flags==inclusionExpected[bits],"inclusion truth table");}
 }
 // Both depth interpolation modes must make identical near/far decisions.
 float farVertices[9]={-1,-1,.25f, 3,-1,.25f, -1,3,.25f};
 float nearVertices[9]={-1,-1,4.f, 3,-1,4.f, -1,3,4.f};
 id<MTLBuffer> farGeometry=[d newBufferWithBytes:farVertices length:sizeof(farVertices) options:MTLResourceStorageModeShared];
 id<MTLBuffer> nearGeometry=[d newBufferWithBytes:nearVertices length:sizeof(nearVertices) options:MTLResourceStorageModeShared];
 for(unsigned native=0;native<2;native++) {
  u.flags.y=native; node->depth=.45f; node->flags=4; run(1,3,farGeometry); require(node->flags==4,"far modifier leaves fragment unshadowed");
  node->flags=4; run(1,3,nearGeometry); require(node->flags==5,"near modifier marks fragment shadowed");
 }
 std::puts("Metal OIT overlapping modifiers, inclusion/exclusion and native depth passed");return 0;
} }
