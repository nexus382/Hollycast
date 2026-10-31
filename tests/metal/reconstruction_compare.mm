// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Compare Metal reconstruction against the actual project-local GLSL kernels.
// clang++ -std=c++17 -fobjc-arc -Icore tests/metal/reconstruction_compare.mm -framework Metal -framework Foundation -framework OpenGL -Wno-deprecated-declarations -o /tmp/metal-reconstruction-compare
#include "rend/metal/metal_reconstruction.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static void require(bool value, const char *message) { if (!value) { std::fprintf(stderr,"FAIL: %s\n", message); std::exit(2); } }
static GLuint shader(GLenum type, const std::string& text) {
    GLuint s=glCreateShader(type); const char *p=text.c_str(); glShaderSource(s,1,&p,nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok); if(!ok) { char log[8192]; glGetShaderInfoLog(s,sizeof(log),nullptr,log); std::fprintf(stderr,"GL shader: %s\n",log); std::exit(2); } return s;
}
int main(int argc, char **argv) { @autoreleasepool {
    const char *path=argc>1?argv[1]:"core/rend/gles/gles.cpp";
    std::ifstream input(path); require(bool(input),"open project GLSL source");
    std::string glsource((std::istreambuf_iterator<char>(input)),{});
    size_t a=glsource.find("highp float jincDistance("), b=glsource.find("\n)\" R\"(",a);
    size_t c=glsource.find("lowp vec4 clampToSourceNeighborhood(",b);
    size_t d=glsource.find("#if TARGET_GL == GLES3 && pp_SampleTextureFilter >= 5",c);
    require(a!=std::string::npos && b!=std::string::npos && c!=std::string::npos && d!=std::string::npos,"locate original kernels");
    std::string kernels=glsource.substr(a,b-a)+glsource.substr(c,d-c);
    CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj format=nullptr; CGLContextObj context=nullptr; GLint count=0;
    require(CGLChoosePixelFormat(attrs,&format,&count)==kCGLNoError && CGLCreateContext(format,nullptr,&context)==kCGLNoError,"OpenGL context"); CGLSetCurrentContext(context);
    GLuint fbo,out,vao,glTex,glPal; glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glGenTextures(1,&out);glBindTexture(GL_TEXTURE_2D,out);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,out,0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"OpenGL framebuffer");
    glGenVertexArrays(1,&vao); glBindVertexArray(vao); glViewport(0,0,8,8); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glGenTextures(1,&glTex); glGenTextures(1,&glPal);
    unsigned char inputPixels[4*4*4]; for(int i=0;i<16;i++) { inputPixels[i*4]=(i%4)*65; inputPixels[i*4+1]=(i/4)*75; inputPixels[i*4+2]=((i%5)*61)%256; inputPixels[i*4+3]=(i%4)*85; }
    unsigned char palette[1024*4]; for(int i=0;i<1024;i++) { palette[i*4]=(i*7)%256; palette[i*4+1]=(i*17)%256; palette[i*4+2]=(i*31)%256; palette[i*4+3]=(i*13)%256; }
    auto glUpload=[](GLuint texture, int w, int h, const unsigned char *data) { glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data); };
    glUpload(glTex,4,4,inputPixels); glUpload(glPal,1024,1,palette);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"Metal device");
    NSError *error=nil; const char *metal=R"MSL(#include <metal_stdlib>
using namespace metal;
)MSL" METAL_RECONSTRUCTION_SOURCE R"MSL(
struct V {float4 pos [[position]];};
vertex V v(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[i],0,1)}; }
fragment float4 f(V in [[stage_in]], texture2d<float> tex [[texture(0)]], texture2d<float> pal [[texture(1)]], sampler s [[sampler(0)]], constant uint4 &u [[buffer(0)]]) {
    float2 uv=float2(in.pos.x,8.0-in.pos.y)*0.17+float2(-0.14,0.08);
    return metalSampleReconstruction(tex,s,pal,u.x,u.y!=0,u.z,u.w,uv);
})MSL";
    id<MTLLibrary> lib=[device newLibraryWithSource:[NSString stringWithUTF8String:metal] options:nil error:&error]; if(!lib) std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);require(lib!=nil,"Metal shader");
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"v"];pd.fragmentFunction=[lib newFunctionWithName:@"f"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline!=nil,"Metal pipeline");
    auto makeTexture=[&](int w,int h,MTLTextureUsage usage,const unsigned char *data) { MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=usage;id<MTLTexture> t=[device newTextureWithDescriptor:td];if(data)[t replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:data bytesPerRow:w*4];return t; };
    id<MTLTexture> source=makeTexture(4,4,MTLTextureUsageShaderRead,inputPixels), pal=makeTexture(1024,1,MTLTextureUsageShaderRead,palette), output=makeTexture(8,8,MTLTextureUsageRenderTarget,nullptr);
    MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterNearest;sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeRepeat;id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:sd];id<MTLCommandQueue> queue=[device newCommandQueue];
    const char *vert="#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}";
    bool pass=true;int total=0;
    for(int pattern=0;pattern<3;pattern++) {
      if(pattern != 0) {
        for(int y=0;y<4;y++) for(int x=0;x<4;x++) {
            int i=y*4+x;
            bool on = pattern == 1 ? (x >= y) : ((x+y)%3 == 0 || (x==2 && y==1));
            inputPixels[i*4]=on ? 220 : 40;
            inputPixels[i*4+1]=on ? 30 : 180;
            inputPixels[i*4+2]=on ? 90 : 240;
            inputPixels[i*4+3]=on ? 255 : 0;
        }
        glUpload(glTex,4,4,inputPixels);
        [source replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:inputPixels bytesPerRow:16];
      }
      for(int mode=1;mode<=19;mode++) { if(mode==14) continue;
        std::string pre="#version 150\n#define highp\n#define lowp\n#define mediump\n#define PI 3.14159265358979323846\n#define pp_SampleTextureFilter "+std::to_string(mode)+R"(
uniform sampler2D tex; uniform sampler2D palette; uniform int palMode; uniform int ignoreAlpha;
struct TextureSampleState {bool palette; bool ignoreAlpha;};
vec4 sampleSourceTexel(TextureSampleState s, vec2 uv) {vec4 c=textureLod(tex,uv,0.0); if(s.palette) c=texelFetch(palette,ivec2(int(floor(c.r*255.0+0.5))+7,0),0); if(s.ignoreAlpha)c.a=1.0; return c;}
)";
        std::string dispatch=R"(
out vec4 outColor;
void main(){vec2 uv=gl_FragCoord.xy*0.17+vec2(-0.14,0.08);TextureSampleState s;s.palette=palMode!=0;s.ignoreAlpha=ignoreAlpha!=0;
#if pp_SampleTextureFilter == 19
outColor=sampleSourceTexel(s,uv);
#elif pp_SampleTextureFilter == 1 || pp_SampleTextureFilter == 2
outColor=alphaWeightedBilinear(s,uv,pp_SampleTextureFilter==2);
#elif pp_SampleTextureFilter == 3 || pp_SampleTextureFilter == 4
outColor=jinc2Sample(s,uv,pp_SampleTextureFilter==4);
#elif pp_SampleTextureFilter >= 5 && pp_SampleTextureFilter <= 7
outColor=mmpxSample(s,uv);
#elif pp_SampleTextureFilter >= 8 && pp_SampleTextureFilter <= 11
outColor=jinc2AntiRingSample(s,uv,pp_SampleTextureFilter==8?0.0:pp_SampleTextureFilter==9?0.18:pp_SampleTextureFilter==10?0.08:0.22);
#elif pp_SampleTextureFilter == 12 || pp_SampleTextureFilter == 13
outColor=separableReconstructionSample(s,uv,pp_SampleTextureFilter-12,true);
#else
outColor=separableReconstructionSample(s,uv,pp_SampleTextureFilter-13,false);
#endif
})";
        GLuint vs=shader(GL_VERTEX_SHADER,vert),fs=shader(GL_FRAGMENT_SHADER,pre+kernels+dispatch),prog=glCreateProgram();glAttachShader(prog,vs);glAttachShader(prog,fs);glLinkProgram(prog);GLint ok=0;glGetProgramiv(prog,GL_LINK_STATUS,&ok);require(ok,"GL link");glUseProgram(prog);glUniform1i(glGetUniformLocation(prog,"tex"),0);glUniform1i(glGetUniformLocation(prog,"palette"),1);
        for(int palMode=0;palMode<=1;palMode++) for(int ignore=0;ignore<=1;ignore++) {
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,glTex);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,glPal);glUniform1i(glGetUniformLocation(prog,"palMode"),palMode);glUniform1i(glGetUniformLocation(prog,"ignoreAlpha"),ignore);glDrawArrays(GL_TRIANGLES,0,3);unsigned char glPixels[8*8*4];glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,glPixels);
            id<MTLBuffer> result=[device newBufferWithLength:2048 options:MTLResourceStorageModeShared];id<MTLCommandBuffer> command=[queue commandBuffer];MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;rp.colorAttachments[0].storeAction=MTLStoreActionStore;id<MTLRenderCommandEncoder> enc=[command renderCommandEncoderWithDescriptor:rp];[enc setRenderPipelineState:pipeline];[enc setFragmentTexture:source atIndex:0];[enc setFragmentTexture:pal atIndex:1];[enc setFragmentSamplerState:sampler atIndex:0];unsigned params[]={unsigned(mode),unsigned(ignore),unsigned(palMode),7};[enc setFragmentBytes:params length:sizeof(params) atIndex:0];[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];[blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:2048];[blit endEncoding];[command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,"Metal command");const auto *pixels=(const unsigned char *)result.contents;
            int maxDiff=0;for(int y=0;y<8;y++)for(int x=0;x<8;x++)for(int k=0;k<4;k++)maxDiff=std::max(maxDiff,std::abs(int(glPixels[(y*8+x)*4+k])-int(pixels[(7-y)*256+x*4+k])));
            if(maxDiff>3){std::fprintf(stderr,"FAIL pattern %d mode %d palette=%d ignoreAlpha=%d max difference=%d\n",pattern,mode,palMode,ignore,maxDiff);pass=false;} total++;
        }
        glDeleteProgram(prog);glDeleteShader(vs);glDeleteShader(fs);
    }
    }
    CGLSetCurrentContext(nullptr);CGLReleaseContext(context);CGLReleasePixelFormat(format);
    if(!pass)return 1;std::printf("Metal reconstruction matches OpenGL across %d cases\n",total);return 0;
} }
