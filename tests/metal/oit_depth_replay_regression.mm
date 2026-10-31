// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Verify ordered opaque color/depth replay and copying the modifier stencil
// between the two depth attachments used by native Metal per-pixel rendering.
#include "hw/pvr/Renderer_if.h"
#include "rend/metal/metal_oit.h"
#include <simd/simd.h>
#include <cstdio>
#include <cstdlib>
struct Fragment { simd_float4 color; float depth; };
static void require(bool b,const char *s){if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);}}
int main(){@autoreleasepool{
 id<MTLDevice>d=MTLCreateSystemDefaultDevice();require(d!=nil,"device");id<MTLCommandQueue>q=[d newCommandQueue];NSError*error=nil;
 NSString*src=@R"MSL(
#include <metal_stdlib>
using namespace metal;
struct F {float4 color;float depth;}; struct O {float4 color [[color(0)]];float depth [[depth(any)]];};
vertex float4 vertexMain(uint i [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}
fragment O fragmentMain(constant F& f [[buffer(0)]]){return {f.color,f.depth};}
)MSL";
 id<MTLLibrary>lib=[d newLibraryWithSource:src options:nil error:&error];if(!lib){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 MTLRenderPipelineDescriptor*pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vertexMain"];pd.fragmentFunction=[lib newFunctionWithName:@"fragmentMain"];
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 pd.colorAttachments[0].blendingEnabled=YES;pd.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;pd.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
 pd.colorAttachments[0].sourceAlphaBlendFactor=MTLBlendFactorOne;pd.colorAttachments[0].destinationAlphaBlendFactor=MTLBlendFactorZero;
 id<MTLRenderPipelineState>pipeline=[d newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline!=nil,"pipeline");
 MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:1 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;id<MTLTexture>color=[d newTextureWithDescriptor:td];
 td.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;id<MTLTexture>pre=[d newTextureWithDescriptor:td],replay=[d newTextureWithDescriptor:td],nextDepth=[d newTextureWithDescriptor:td];
 auto pass=[&](id<MTLCommandBuffer> cb,id<MTLTexture> depth,bool clearColor,bool clearDepth,bool clearStencil){MTLRenderPassDescriptor*r=[MTLRenderPassDescriptor renderPassDescriptor];
  r.colorAttachments[0].texture=color;r.colorAttachments[0].loadAction=clearColor?MTLLoadActionClear:MTLLoadActionLoad;r.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);r.colorAttachments[0].storeAction=MTLStoreActionStore;
  r.depthAttachment.texture=depth;r.depthAttachment.loadAction=clearDepth?MTLLoadActionClear:MTLLoadActionLoad;r.depthAttachment.clearDepth=0;r.depthAttachment.storeAction=MTLStoreActionStore;
  r.stencilAttachment.texture=depth;r.stencilAttachment.loadAction=clearStencil?MTLLoadActionClear:MTLLoadActionLoad;r.stencilAttachment.clearStencil=0;r.stencilAttachment.storeAction=MTLStoreActionStore;
  id<MTLRenderCommandEncoder>e=[cb renderCommandEncoderWithDescriptor:r];[e setRenderPipelineState:pipeline];return e;};
 auto state=[&](bool writeDepth,bool setStencil,bool noStencilTest=false){MTLDepthStencilDescriptor*dsc=[MTLDepthStencilDescriptor new];dsc.depthCompareFunction=MTLCompareFunctionGreater;dsc.depthWriteEnabled=writeDepth;
  MTLStencilDescriptor*st=[MTLStencilDescriptor new];st.stencilCompareFunction=(setStencil || noStencilTest)?MTLCompareFunctionAlways:MTLCompareFunctionEqual;st.readMask=0x81;st.writeMask=setStencil?0xff:0;st.stencilFailureOperation=MTLStencilOperationKeep;st.depthFailureOperation=MTLStencilOperationKeep;st.depthStencilPassOperation=setStencil?MTLStencilOperationReplace:MTLStencilOperationKeep;dsc.frontFaceStencil=st;dsc.backFaceStencil=st;return [d newDepthStencilStateWithDescriptor:dsc];};
 Fragment red{{1,0,0,1},.3f},blue{{0,0,1,.5f},.6f},green{{0,1,0,1},.8f};
 auto draw=[&](id<MTLRenderCommandEncoder>e,const Fragment&f,MTLScissorRect clip,id<MTLDepthStencilState>s){[e setScissorRect:clip];[e setDepthStencilState:s];[e setStencilReferenceValue:0x81];[e setFragmentBytes:&f length:sizeof(f) atIndex:0];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];};
 id<MTLCommandBuffer>cb=[q commandBuffer];id<MTLRenderCommandEncoder>e=pass(cb,replay,true,true,true);[e endEncoding];
 e=pass(cb,pre,true,true,true);draw(e,red,{0,0,2,1},state(true,true));draw(e,blue,{0,0,1,1},state(true,true));draw(e,green,{1,0,1,1},state(false,true));[e endEncoding];
 metalOITCopyDepthStencilAspect(d,cb,pre,replay,2,1,true);
 e=pass(cb,replay,true,false,false);draw(e,red,{0,0,2,1},state(true,false,true));draw(e,blue,{0,0,1,1},state(true,false));draw(e,green,{1,0,1,1},state(false,false));[e endEncoding];
 id<MTLBuffer> beforeNext=[d newBufferWithLength:256 options:MTLResourceStorageModeShared];
 id<MTLBlitCommandEncoder> beforeCopy=[cb blitCommandEncoder];[beforeCopy copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2,1,1) toBuffer:beforeNext destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];[beforeCopy endEncoding];
 // Later render passes need the prepass depth plane without its stencil bits.
 metalOITCopyDepthStencilAspect(d,cb,pre,nextDepth,2,1,false);
 e=pass(cb,nextDepth,false,false,true); MTLDepthStencilDescriptor *nextState=[MTLDepthStencilDescriptor new];nextState.depthCompareFunction=MTLCompareFunctionGreater;nextState.depthWriteEnabled=NO;
 Fragment yellow{{1,1,0,1},.5f}; draw(e,yellow,{0,0,2,1},[d newDepthStencilStateWithDescriptor:nextState]);[e endEncoding];
 id<MTLBuffer>output=[d newBufferWithLength:256 options:MTLResourceStorageModeShared];id<MTLBlitCommandEncoder>b=[cb blitCommandEncoder];[b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2,1,1) toBuffer:output destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];[b endEncoding];[cb commit];[cb waitUntilCompleted];
 if(cb.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"GPU: %s\n",cb.error.localizedDescription.UTF8String);return 2;}
 auto*before=static_cast<const uint8_t*>(beforeNext.contents);require(before[4]==0 && before[5]==255 && before[6]==0,"visible ZWriteDis polygon keeps color");
 auto*px=static_cast<const uint8_t*>(output.contents);require(abs((int)px[0]-128)<=1 && px[1]==0 && abs((int)px[2]-128)<=1,"non-shadow opaque replay preserves earlier red beneath shadowed blended blue");
 require(px[4]==255 && px[5]==255 && px[6]==0,"depth-plane copy preserves .3 depth, allowing a later .5 fragment");
 std::puts("Metal OIT stencil copy and ordered depth/color replay passed");return 0;
}}
