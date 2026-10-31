// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production OIT append/clear/resolve implementation on Metal.
#include "hw/pvr/Renderer_if.h"
#include "rend/metal/metal_oit.h"
#include "rend/metal/metal_oit_shaders.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
static void require(bool condition, const char *message) { if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
struct Parameters { float depth, red, green, blue, alpha; uint32_t sequence, tsp; };
int main() { @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice(); require(device != nil, "Metal device");
    id<MTLCommandQueue> queue=[device newCommandQueue];
    std::string source=metalOITShaderSource;
    source += R"MSL(
struct T { float depth, red, green, blue, alpha; uint sequence, tsp; };
vertex float4 testVertex(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }
fragment void testAppend(float4 position [[position]], uint primitive [[primitive_id]], constant T& t [[buffer(0)]],
                         device OITNode *nodes [[buffer(1)]], device atomic_uint *heads [[buffer(2)]], device OITControl *control [[buffer(3)]],
                         texture2d<float,access::read> mirror [[texture(0)]]) {
    oitAppendVisible(nodes,heads,control,mirror,uint2(position.xy),t.depth,float4(t.red,t.green,t.blue,t.alpha),float4(0),t.sequence,primitive,t.tsp,0,0);
}
)MSL";
    NSError *error=nil; id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
    if(!library) { std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new]; p.vertexFunction=[library newFunctionWithName:@"testVertex"]; p.fragmentFunction=[library newFunctionWithName:@"testAppend"];
    p.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm; p.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:p error:&error]; require(pipeline != nil, "collection pipeline");
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite; id<MTLTexture> input=[device newTextureWithDescriptor:td], output=[device newTextureWithDescriptor:td];
    id<MTLTexture> secondaryInput=[device newTextureWithDescriptor:td], secondaryOutput=[device newTextureWithDescriptor:td];
    uint32_t transparent=0; [secondaryInput replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&transparent bytesPerRow:4];
    uint32_t white=0xffffffff; [input replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
    td.pixelFormat=MTLPixelFormatR32Float; td.usage=MTLTextureUsageShaderRead; id<MTLTexture> mirror=[device newTextureWithDescriptor:td]; float front=0.5f;
    [mirror replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&front bytesPerRow:4];
    MetalOITBuffers buffers; buffers.init(device,1,1,8192*sizeof(MetalOITNode),8,0.5f);
    auto capture = [&](const Parameters *params, unsigned n, unsigned instances) {
        id<MTLCommandBuffer> command=[queue commandBuffer]; buffers.clear(command);
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor]; pass.colorAttachments[0].texture=input; pass.colorAttachments[0].loadAction=MTLLoadActionLoad; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass]; [encoder setRenderPipelineState:pipeline];
        [encoder setFragmentBuffer:buffers.nodes() offset:0 atIndex:1]; [encoder setFragmentBuffer:buffers.heads() offset:0 atIndex:2]; [encoder setFragmentBuffer:buffers.control() offset:0 atIndex:3]; [encoder setFragmentTexture:mirror atIndex:0];
        for(unsigned i=0;i<n;i++) { [encoder setFragmentBytes:&params[i] length:sizeof(Parameters) atIndex:0]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:instances]; }
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted]; require(command.status==MTLCommandBufferStatusCompleted,"collection GPU command");
    };
    const uint32_t blend=(4u<<29)|(5u<<26); // source alpha, inverse source alpha
    Parameters hidden={0.2f,0,0,0,1,0,blend}; capture(&hidden,1,1);
    auto *state=static_cast<MetalOITControl*>(buffers.control().contents); require(state->count==0,"opaque depth rejects hidden fragment before allocation");
    Parameters fragments[]={{0.6f,1,0,0,.5f,1,blend},{0.8f,0,0,1,.5f,2,blend}};
    capture(fragments,2,1); require(state->count==2 && !buffers.overflowed(),"collect visible fragments");
    id<MTLCommandBuffer> command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted]; require(command.status==MTLCommandBufferStatusCompleted,"resolve GPU command");
    uint32_t pixel=0; [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    const auto *c=reinterpret_cast<const unsigned char*>(&pixel);
    // White background -> red at 50% -> blue at 50%.
    require(std::abs(int(c[0])-128)<=2 && std::abs(int(c[1])-64)<=2 && std::abs(int(c[2])-191)<=2,"back-to-front alpha blend");
    // Equal-depth ordering must follow TA submission, independently of append order.
    Parameters ties[]={{0.7f,0,0,1,.5f,2,blend},{0.7f,1,0,0,.5f,1,blend}};
    capture(ties,2,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted];
    [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(std::abs(int(c[0])-128)<=2 && std::abs(int(c[1])-64)<=2 && std::abs(int(c[2])-191)<=2,"equal depth follows submission order");
    state->maxLayers=1;
    capture(fragments,2,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted];
    [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(buffers.layersExceeded() && std::abs(int(c[0])-128)<=2 && std::abs(int(c[1])-128)<=2 && c[2]>=253,"layer cap keeps nearest and reports truncation");
    state->maxLayers=8;
    // Write red to the secondary buffer, then select it as the next source.
    Parameters secondary[]={{0.6f,1,0,0,1,0,(1u<<29)|(1u<<24)}, {0.8f,0,0,0,0,1,(1u<<29)|(1u<<25)}};
    capture(secondary,2,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted];
    [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(c[0]==255 && c[1]==0 && c[2]==0 && c[3]==255,"secondary accumulation is used in sorted order");
    // Carry a secondary attachment into and out of sorted resolution.
    uint32_t redSeed=0xff0000ff; [secondaryInput replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&redSeed bytesPerRow:4];
    Parameters readSeed={0.7f,0,0,0,0,1,(1u<<29)|(1u<<25)};
    capture(&readSeed,1,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted];
    [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(pixel==redSeed,"sorted source reads previous secondary attachment");
    Parameters writeSecondary={0.7f,0,0,1,1,1,(1u<<29)|(1u<<24)};
    capture(&writeSecondary,1,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryInput,secondaryOutput); [command commit]; [command waitUntilCompleted];
    uint32_t secondaryPixel=0;[secondaryOutput getBytes:&secondaryPixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(secondaryPixel==0xffff0000,"sorted destination writes subsequent secondary attachment");
    capture(&readSeed,1,1); command=[queue commandBuffer]; buffers.resolve(command,input,output,secondaryOutput,secondaryInput); [command commit]; [command waitUntilCompleted];
    [output getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
    require(pixel==0xffff0000,"later sorted source observes previous sorted destination");
    capture(&fragments[0],1,4097); require(buffers.overflowed(),"bounded allocation reports overflow");
    require(buffers.grow(),"buffer grows within configured budget"); capture(&fragments[0],1,4097); require(!buffers.overflowed() && state->count==4097,"retry after growth keeps all fragments");
    id<MTLBuffer> retained= buffers.nodes();
    buffers.init(device,1,1,8192*sizeof(MetalOITNode),3,0.75f);
    require(buffers.nodes()==retained && state->capacity==8192 && state->maxLayers==3 && state->shadowScale==0.75f,"same-size buffers and compiled pipelines are reused");
    buffers.init(device,2,3,8192*sizeof(MetalOITNode),8,0.5f);
    auto *resized=static_cast<MetalOITControl*>(buffers.control().contents);
    require(resized->width==2 && resized->height==3 && buffers.heads().length==24,"resolution transition reallocates correct head dimensions");
    std::puts("Metal OIT depth, ordering, layer cap, secondary blend, overflow and retry passed"); return 0;
} }
