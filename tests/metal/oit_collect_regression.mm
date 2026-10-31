// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real PVR collection shader, depth mirror, two-volume area, and resolve.
#include "hw/pvr/Renderer_if.h"
#include "rend/metal/metal_oit.h"
#include "rend/metal/metal_oit_shaders.h"
#include "rend/metal/metal_shaders.h"
#include <simd/simd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
static void require(bool b,const char *s) { if(!b){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);} }
struct alignas(16) Uniforms { simd_float4x4 matrix; simd_float4 clip,fogColor,fogVert,fogClampMin,fogClampMax,values; simd_uint4 flags,flags2,flags3,flags4; };
struct Draw { uint32_t sequence,tsp0,tsp1,flags; };
int main(){@autoreleasepool {
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();require(d!=nil,"Metal device");id<MTLCommandQueue> q=[d newCommandQueue];NSError *error=nil;
 std::string source=std::string(metalOITShaderSource)+metalRegularShaderSource+metalOITCollectShaderSource;
 id<MTLLibrary> lib=[d newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error]; if(!lib){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new];p.vertexFunction=[lib newFunctionWithName:@"pvr_oit_vertex"];p.fragmentFunction=[lib newFunctionWithName:@"pvr_collect"];
 p.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;p.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
 MTLVertexDescriptor *v=[MTLVertexDescriptor vertexDescriptor];
 v.attributes[0].format=MTLVertexFormatFloat3;v.attributes[0].offset=offsetof(Vertex,x);
 v.attributes[1].format=MTLVertexFormatUChar4Normalized;v.attributes[1].offset=offsetof(Vertex,col);
 v.attributes[2].format=MTLVertexFormatUChar4Normalized;v.attributes[2].offset=offsetof(Vertex,spc);
 v.attributes[3].format=MTLVertexFormatFloat2;v.attributes[3].offset=offsetof(Vertex,u);
 v.attributes[4].format=MTLVertexFormatUChar4Normalized;v.attributes[4].offset=offsetof(Vertex,col1);
 v.attributes[5].format=MTLVertexFormatUChar4Normalized;v.attributes[5].offset=offsetof(Vertex,spc1);
 v.attributes[6].format=MTLVertexFormatFloat2;v.attributes[6].offset=offsetof(Vertex,u1);v.layouts[0].stride=sizeof(Vertex);p.vertexDescriptor=v;
 id<MTLRenderPipelineState> pipeline=[d newRenderPipelineStateWithDescriptor:p error:&error];if(!pipeline){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 auto tex=[&](MTLPixelFormat format, MTLTextureUsage usage,int width=1,int height=1) { MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];desc.usage=usage;return [d newTextureWithDescriptor:desc]; };
 id<MTLTexture> target=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead), output=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite);
 id<MTLTexture> area0=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead),area1=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead),palette=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead),fog=tex(MTLPixelFormatR8Unorm,MTLTextureUsageShaderRead,128,2),mirror=tex(MTLPixelFormatR32Float,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
 id<MTLTexture> depth=tex(MTLPixelFormatDepth32Float_Stencil8,MTLTextureUsageRenderTarget);
 id<MTLTexture> secondaryInput=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite), secondaryOutput=tex(MTLPixelFormatRGBA8Unorm,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite);
 uint32_t transparent=0;[secondaryInput replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&transparent bytesPerRow:4];
 const uint32_t red=0xff0000ff,blue=0xffff0000,white=0xffffffff; [area0 replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&red bytesPerRow:4];[area1 replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&blue bytesPerRow:4];[palette replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
 uint8_t fogBytes[256]{};[fog replaceRegion:MTLRegionMake2D(0,0,128,2) mipmapLevel:0 withBytes:fogBytes bytesPerRow:128];
 Vertex vertices[3]{};float xy[3][2]={{-1,-1},{3,-1},{-1,3}};for(int i=0;i<3;i++){vertices[i].x=xy[i][0];vertices[i].y=xy[i][1];vertices[i].z=1;vertices[i].col[3]=vertices[i].col1[3]=255;}
 id<MTLBuffer> vb=[d newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
 Uniforms u{};u.matrix=matrix_identity_float4x4;u.values={0,1,1,0};u.flags={1,1,0,0};u.flags2={0,0,0,2};u.flags4={1,12,1,0};
 Draw draw{1,1u<<29,1u<<29,8u|2u}; MetalOITBuffers oit;oit.init(d,1,1,1024*sizeof(MetalOITNode),8,.5f);
 auto run=[&](float front, bool uploadMirror=true){
  if (uploadMirror) [mirror replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&front bytesPerRow:4];id<MTLCommandBuffer> cb=[q commandBuffer];oit.clear(cb);
  MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=target;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(1,1,1,1);
  id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:pipeline];[e setVertexBuffer:vb offset:0 atIndex:0];[e setVertexBytes:&u length:sizeof(u) atIndex:1];[e setFragmentBytes:&u length:sizeof(u) atIndex:1];[e setFragmentBytes:&draw length:sizeof(draw) atIndex:2];
  [e setFragmentBuffer:oit.nodes() offset:0 atIndex:3];[e setFragmentBuffer:oit.heads() offset:0 atIndex:4];[e setFragmentBuffer:oit.control() offset:0 atIndex:5];[e setFragmentBytes:&u length:sizeof(u) atIndex:6];
  [e setFragmentTexture:area0 atIndex:0];[e setFragmentTexture:palette atIndex:1];[e setFragmentTexture:fog atIndex:2];[e setFragmentTexture:mirror atIndex:3];[e setFragmentTexture:area1 atIndex:4];id<MTLSamplerState> samp=[d newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];[e setFragmentSamplerState:samp atIndex:0];[e setFragmentSamplerState:samp atIndex:1];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,"collection command");
 };
 // Render a closer opaque polygon through the production mirror fragment.
 MTLRenderPipelineDescriptor *opaque=[MTLRenderPipelineDescriptor new];
 opaque.vertexFunction=[lib newFunctionWithName:@"pvr_vertex"];opaque.fragmentFunction=[lib newFunctionWithName:@"pvr_mirror"];opaque.vertexDescriptor=v;
 opaque.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;opaque.colorAttachments[1].pixelFormat=MTLPixelFormatR32Float;
 opaque.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;opaque.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 id<MTLRenderPipelineState> mirrorPipeline=[d newRenderPipelineStateWithDescriptor:opaque error:&error];if(!mirrorPipeline){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 Vertex frontVertices[3];std::memcpy(frontVertices,vertices,sizeof(vertices));for(auto& vtx:frontVertices)vtx.z=4;
 id<MTLBuffer> frontBuffer=[d newBufferWithBytes:frontVertices length:sizeof(frontVertices) options:MTLResourceStorageModeShared];
 MTLDepthStencilDescriptor *depthDesc=[MTLDepthStencilDescriptor new];depthDesc.depthCompareFunction=MTLCompareFunctionGreater;depthDesc.depthWriteEnabled=YES;
 id<MTLCommandBuffer> opaqueCommand=[q commandBuffer];MTLRenderPassDescriptor *opaquePass=[MTLRenderPassDescriptor renderPassDescriptor];
 opaquePass.colorAttachments[0].texture=target;opaquePass.colorAttachments[0].loadAction=MTLLoadActionClear;opaquePass.colorAttachments[0].storeAction=MTLStoreActionStore;
 opaquePass.colorAttachments[1].texture=mirror;opaquePass.colorAttachments[1].loadAction=MTLLoadActionClear;opaquePass.colorAttachments[1].storeAction=MTLStoreActionStore;
 opaquePass.depthAttachment.texture=depth;opaquePass.depthAttachment.loadAction=MTLLoadActionClear;opaquePass.depthAttachment.clearDepth=0;opaquePass.depthAttachment.storeAction=MTLStoreActionStore;
 opaquePass.stencilAttachment.texture=depth;opaquePass.stencilAttachment.loadAction=MTLLoadActionClear;opaquePass.stencilAttachment.storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> oe=[opaqueCommand renderCommandEncoderWithDescriptor:opaquePass];[oe setRenderPipelineState:mirrorPipeline];[oe setDepthStencilState:[d newDepthStencilStateWithDescriptor:depthDesc]];[oe setVertexBuffer:frontBuffer offset:0 atIndex:0];
 [oe setVertexBytes:&u length:sizeof(u) atIndex:1];[oe setFragmentBytes:&u length:sizeof(u) atIndex:1];[oe setFragmentTexture:area0 atIndex:0];[oe setFragmentTexture:palette atIndex:1];[oe setFragmentTexture:fog atIndex:2];[oe setFragmentSamplerState:[d newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
 [oe drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[oe endEncoding];[opaqueCommand commit];[opaqueCommand waitUntilCompleted];require(opaqueCommand.status==MTLCommandBufferStatusCompleted,"opaque depth mirror command");
 run(0,false);auto *ctl=static_cast<MetalOITControl*>(oit.control().contents);require(ctl->count==0,"fully opaque-occluded polygon adds zero fragments");
 run(.1f);require(ctl->count==1,"visible polygon collected");id<MTLCommandBuffer> cb=[q commandBuffer];oit.resolve(cb,target,output,secondaryInput,secondaryOutput);[cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,"resolve command");uint32_t pixel=0;[output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];require(pixel==blue,"shadowed two-volume fragment uses area-1 texture");
 // Real opaque color replay must select area 1 only on shadowed stencil.
 MTLRenderPipelineDescriptor *colorDesc=[MTLRenderPipelineDescriptor new];colorDesc.vertexFunction=[lib newFunctionWithName:@"pvr_oit_vertex"];colorDesc.fragmentFunction=[lib newFunctionWithName:@"pvr_oit_color"];colorDesc.vertexDescriptor=v;
 colorDesc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;colorDesc.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;colorDesc.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 id<MTLRenderPipelineState> colorPipeline=[d newRenderPipelineStateWithDescriptor:colorDesc error:&error];if(!colorPipeline){std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
 auto colorPass=[&](bool initialize, bool secondarySelect=false, bool singleShadow=false) {
  id<MTLCommandBuffer> command=[q commandBuffer];MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(1,1,1,1);
  pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;
  pass.stencilAttachment.texture=depth;pass.stencilAttachment.loadAction=initialize ? MTLLoadActionClear : MTLLoadActionLoad;pass.stencilAttachment.clearStencil=0;pass.stencilAttachment.storeAction=MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:colorPipeline];
  MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;
  MTLStencilDescriptor *st=[MTLStencilDescriptor new];st.stencilCompareFunction=initialize ? MTLCompareFunctionAlways : MTLCompareFunctionEqual;st.stencilFailureOperation=MTLStencilOperationKeep;st.depthFailureOperation=MTLStencilOperationKeep;st.depthStencilPassOperation=initialize ? MTLStencilOperationReplace : MTLStencilOperationKeep;st.readMask=0x81;st.writeMask=initialize ? 0xff : 0;ds.frontFaceStencil=st;ds.backFaceStencil=st;
  [encoder setDepthStencilState:[d newDepthStencilStateWithDescriptor:ds]];[encoder setStencilReferenceValue:0x81];[encoder setVertexBuffer:vb offset:0 atIndex:0];
  Uniforms areaUniform=u;areaUniform.flags4.w=secondarySelect ? 4 : singleShadow ? 1 : 2;areaUniform.fogVert.w=.5f;[encoder setVertexBytes:&areaUniform length:sizeof(areaUniform) atIndex:1];[encoder setFragmentBytes:&areaUniform length:sizeof(areaUniform) atIndex:1];
  [encoder setFragmentTexture:singleShadow ? area0 : area1 atIndex:0];[encoder setFragmentTexture:palette atIndex:1];[encoder setFragmentTexture:fog atIndex:2];[encoder setFragmentTexture:secondarySelect ? area0 : palette atIndex:3];[encoder setFragmentSamplerState:[d newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];[command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,"opaque color replay command");
 };
 colorPass(true);colorPass(false);[target getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];require(pixel==blue,"opaque shadowed stencil selects area-1 texture");
 colorPass(false,false,true);[target getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];require((pixel & 255)==128 && ((pixel >> 8) & 65535)==0,"unsorted translucent single-volume shadow scales RGB");
 colorPass(false,true);[target getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];require(pixel==red,"ordered secondary-source draw samples the secondary target");
 colorDesc.fragmentFunction=[lib newFunctionWithName:@"pvr_oit_color_mirror"];colorDesc.colorAttachments[1].pixelFormat=MTLPixelFormatR32Float;
 require([d newRenderPipelineStateWithDescriptor:colorDesc error:&error] != nil,"secondary-source color plus depth-mirror pipeline");
 std::puts("Metal OIT depth mirror, hidden collector, opaque/translucent two-volume, native depth and reconstruction passed");return 0;
}}
