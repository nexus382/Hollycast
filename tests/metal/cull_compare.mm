// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Compare the PVR -> GL and PVR -> Metal transforms with the same CullMode.
// clang++ -std=c++17 -fobjc-arc -Wno-deprecated-declarations -Icore -Icore/deps -Icore/deps/nowide/include -Icore/deps/glm tests/metal/cull_compare.mm core/rend/metal/metal_shaders.mm -framework Foundation -framework Metal -framework OpenGL -o /tmp/metal-cull-compare
#include "rend/metal/metal_geometry.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <simd/simd.h>
#include "rend/metal/metal_shaders.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
struct ProbeVertex { float xyz[3]; unsigned char color[4], offset[4]; float uv[2]; char rest[28]; };
struct alignas(16) Uniforms { simd_float4x4 matrix; simd_float4 clip,fogColor,fogVert,fogClampMin,fogClampMax,values; simd_uint4 flags,flags2,flags3,flags4; };
static GLuint glShader(GLenum type, const char *source) {
    GLuint s=glCreateShader(type); glShaderSource(s,1,&source,nullptr); glCompileShader(s); GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok) { char log[4096]; glGetShaderInfoLog(s,sizeof(log),nullptr,log); std::fprintf(stderr,"GL shader: %s\n",log); std::exit(2); } return s;
}
static bool glCull(unsigned isp, bool reverse) {
    const char *vs="#version 150\nin vec2 p; void main(){ gl_Position=vec4(-1.0+2.0*p.x/640.0,-1.0+2.0*p.y/480.0,0,1); }";
    const char *fs="#version 150\nout vec4 color; void main(){color=vec4(1,0,0,1);}";
    GLuint program=glCreateProgram(), v=glShader(GL_VERTEX_SHADER,vs), f=glShader(GL_FRAGMENT_SHADER,fs);
    glAttachShader(program,v); glAttachShader(program,f); glBindAttribLocation(program,0,"p"); glLinkProgram(program);
    GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked); if(!linked) { std::fprintf(stderr,"GL link failed\n"); std::exit(2); }
    float vertices[]={0,0,640,0,0,480};
    if (reverse) { std::swap(vertices[2],vertices[4]); std::swap(vertices[3],vertices[5]); }
    GLuint vao,buffer;
    glGenVertexArrays(1,&vao); glBindVertexArray(vao); glGenBuffers(1,&buffer); glBindBuffer(GL_ARRAY_BUFFER,buffer);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr); glEnableVertexAttribArray(0);
    glViewport(0,0,4,4); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glFrontFace(GL_CCW);
    // GLES setCull(ISP.CullMode ^ 1) with its OpenGL Y convention.
    glCullFace((isp ^ 1)==2 ? GL_FRONT : GL_BACK); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(program); glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char px[4]{}; glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    glDeleteBuffers(1,&buffer); glDeleteVertexArrays(1,&vao); glDeleteProgram(program); glDeleteShader(v); glDeleteShader(f); return px[0]>128;
}
static bool metalCull(id<MTLDevice> d, id<MTLRenderPipelineState> pipeline, unsigned isp, bool reverse) {
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget; id<MTLTexture> color=[d newTextureWithDescriptor:td];
    td.pixelFormat=MTLPixelFormatDepth32Float_Stencil8; id<MTLTexture> depth=[d newTextureWithDescriptor:td];
    td.pixelFormat=MTLPixelFormatRGBA8Unorm; td.width=td.height=1; td.usage=MTLTextureUsageShaderRead; id<MTLTexture> blank=[d newTextureWithDescriptor:td];
    unsigned pixel=0xffffffff; [blank replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&pixel bytesPerRow:4];
    td.pixelFormat=MTLPixelFormatR8Unorm; td.width=128; td.height=2; id<MTLTexture> fog=[d newTextureWithDescriptor:td]; unsigned char zeros[256]{};
    [fog replaceRegion:MTLRegionMake2D(0,0,128,2) mipmapLevel:0 withBytes:zeros bytesPerRow:128];
    const float positions[3][2]={{0,0},{640,0},{0,480}}; ProbeVertex vertices[3]{};
    for(int i=0;i<3;i++) { vertices[i].xyz[0]=positions[i][0]; vertices[i].xyz[1]=positions[i][1]; vertices[i].xyz[2]=1; vertices[i].color[0]=255; vertices[i].color[3]=255; }
    if (reverse) std::swap(vertices[1], vertices[2]);
    id<MTLBuffer> vb=[d newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
    id<MTLBuffer> out=[d newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    Uniforms u{}; u.matrix=matrix_identity_float4x4;
    // TransformMatrix(directx=true): x' = -1+2x/W, y' = 1-2y/H.
    u.matrix.columns[0].x=2.f/640; u.matrix.columns[1].y=-2.f/480; u.matrix.columns[3].x=-1; u.matrix.columns[3].y=1;
    u.values={0,1,1,0}; u.flags={0,1,0,0}; u.flags2={0,0,0,2}; u.flags4={1,0,0,0};
    id<MTLCommandQueue> q=[d newCommandQueue]; id<MTLCommandBuffer> cb=[q commandBuffer];
    MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor]; p.colorAttachments[0].texture=color; p.colorAttachments[0].loadAction=MTLLoadActionClear; p.colorAttachments[0].storeAction=MTLStoreActionStore;
    p.depthAttachment.texture=depth; p.depthAttachment.loadAction=MTLLoadActionClear; p.stencilAttachment.texture=depth; p.stencilAttachment.loadAction=MTLLoadActionClear;
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:p]; [e setRenderPipelineState:pipeline];
    [e setFrontFacingWinding:MTLWindingCounterClockwise]; [e setCullMode:metalCullMode(isp)];
    [e setVertexBuffer:vb offset:0 atIndex:0]; [e setVertexBytes:&u length:sizeof(u) atIndex:1]; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setFragmentTexture:blank atIndex:0]; [e setFragmentTexture:blank atIndex:1]; [e setFragmentTexture:fog atIndex:2]; [e setFragmentSamplerState:[d newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024]; [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted) { std::fprintf(stderr,"Metal command failed\n"); std::exit(2); } return static_cast<unsigned char*>(out.contents)[0]>128;
}
int main() { @autoreleasepool {
    CGLPixelFormatAttribute a[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format=nullptr; GLint count=0; CGLContextObj context=nullptr;
    if(CGLChoosePixelFormat(a,&format,&count)!=kCGLNoError || CGLCreateContext(format,nullptr,&context)!=kCGLNoError) return 2;
    CGLSetCurrentContext(context); GLuint fbo,texture; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 2;
    id<MTLDevice> d=MTLCreateSystemDefaultDevice(); if(!d) return 2; NSError *error=nil;
    id<MTLLibrary> lib=[d newLibraryWithSource:[NSString stringWithUTF8String:metalRegularShaderSource] options:nil error:&error];
    if(!lib) { std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"pvr_vertex"]; pd.fragmentFunction=[lib newFunctionWithName:@"pvr_fragment"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8; pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor]; vd.attributes[0].format=MTLVertexFormatFloat3; vd.attributes[1].format=MTLVertexFormatUChar4Normalized; vd.attributes[1].offset=12;
    vd.attributes[2].format=MTLVertexFormatUChar4Normalized; vd.attributes[2].offset=16; vd.attributes[3].format=MTLVertexFormatFloat2; vd.attributes[3].offset=20; vd.layouts[0].stride=sizeof(ProbeVertex); pd.vertexDescriptor=vd;
    id<MTLRenderPipelineState> pipeline=[d newRenderPipelineStateWithDescriptor:pd error:&error]; if(!pipeline) { std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    bool success=true;
    for (bool reverse : {false,true}) for(unsigned isp=2;isp<=3;isp++) {
        bool gl=glCull(isp,reverse), metal=metalCull(d,pipeline,isp,reverse);
        std::printf("%s winding, ISP CullMode %u: GL %s; Metal %s\n",reverse?"reversed":"original",isp,gl?"drawn":"culled",metal?"drawn":"culled");
        if(gl!=metal) success=false;
    }
    CGLSetCurrentContext(nullptr); CGLReleaseContext(context); CGLReleasePixelFormat(format); return success?0:1;
} }
