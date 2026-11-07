// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
// Native Metal backend for the Hikaru graphics dispatch. The shader contract
// and vertex/uniform layouts are defined in shaders.cpp and device.h.
#import <Metal/Metal.h>
#if !__has_feature(objc_arc)
#error "Hikaru Metal requires automatic reference counting"
#endif
#import <dispatch/dispatch.h>
#include "device.h"
#include "shaders.h"
#include "metal_test.h"
#include "rend/metal/metal_context.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace hikaru::graphics {
namespace {
static_assert(sizeof(Vertex) == sizeof(float) * 32, "Metal vertex ABI mismatch");

MTLPixelFormat pixelFormat(GLenum format) {
    switch (format) {
    case GL_RGB10_A2: return MTLPixelFormatRGB10A2Unorm;
    case GL_RG32UI: return MTLPixelFormatRG32Uint;
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32:
    case GL_DEPTH_COMPONENT32F: return MTLPixelFormatDepth32Float;
    default: return MTLPixelFormatRGBA8Unorm;
    }
}
MTLColorWriteMask metalColorWriteMask(uint8_t glMask) {
    MTLColorWriteMask mask=MTLColorWriteMaskNone;
    if(glMask&0x1)mask|=MTLColorWriteMaskRed;
    if(glMask&0x2)mask|=MTLColorWriteMaskGreen;
    if(glMask&0x4)mask|=MTLColorWriteMaskBlue;
    if(glMask&0x8)mask|=MTLColorWriteMaskAlpha;
    return mask;
}
bool isDepth(GLenum format) {
    return format == GL_DEPTH_COMPONENT || format == GL_DEPTH_COMPONENT16 ||
           format == GL_DEPTH_COMPONENT24 || format == GL_DEPTH_COMPONENT32 ||
           format == GL_DEPTH_COMPONENT32F;
}
MTLBlendFactor blendFactor(GLenum value) {
    switch (value) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation blendOperation(GLenum value) {
    switch (value) {
    case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract;
    case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case GL_MIN: return MTLBlendOperationMin;
    case GL_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
MTLCompareFunction compareFunction(GLenum value) {
    switch (value) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
MTLPrimitiveType primitiveType(GLenum value) {
    switch (value) {
    case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    case GL_TRIANGLE_FAN: return MTLPrimitiveTypeTriangle;
    case GL_LINES: return MTLPrimitiveTypeLine;
    case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case GL_POINTS: return MTLPrimitiveTypePoint;
    default: return MTLPrimitiveTypeTriangle;
    }
}
MTLSamplerAddressMode addressMode(GLenum value) {
    switch (value) {
    case GL_REPEAT: return MTLSamplerAddressModeRepeat;
    case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

struct TextureEntry {
    id<MTLTexture> texture = nil;
    Texture info{};
    uint64_t revision = 0;
    unsigned levels = 0;
    bool renderbuffer = false;
    std::vector<bool> initializedLevels;
};
struct PipelineKey {
    bool bitmap=false, familyResolve=false, blend=false, cull=false, depth=false;
    uint32_t format=0, metadataFormat=0, srcRGB=0, dstRGB=0, srcAlpha=0, dstAlpha=0;
    uint32_t eqRGB=0, eqAlpha=0, cullFace=0, frontFace=0;
    uint8_t colorMask=0;
    bool operator<(const PipelineKey& b) const {
        return std::tie(bitmap,familyResolve,blend,cull,depth,format,metadataFormat,srcRGB,dstRGB,srcAlpha,dstAlpha,eqRGB,eqAlpha,cullFace,frontFace,colorMask) <
               std::tie(b.bitmap,b.familyResolve,b.blend,b.cull,b.depth,b.format,b.metadataFormat,b.srcRGB,b.dstRGB,b.srcAlpha,b.dstAlpha,b.eqRGB,b.eqAlpha,b.cullFace,b.frontFace,b.colorMask);
    }
};
struct SamplerKey {
    uint32_t minFilter=0,magFilter=0,wrapS=0,wrapT=0,maxLevel=0;
    bool operator<(const SamplerKey& b) const {
        return std::tie(minFilter,magFilter,wrapS,wrapT,maxLevel) <
               std::tie(b.minFilter,b.magFilter,b.wrapS,b.wrapT,b.maxLevel);
    }
};

class MetalBackend final : public Backend {
public:
    MetalBackend(id<MTLDevice> requestedDevice, id<MTLCommandQueue> requestedQueue,
                 std::function<void(id<MTLTexture>,float)> presentFrame = {})
        : presenter(std::move(presentFrame)) {
        @autoreleasepool {
            if (!requestedDevice || !requestedQueue) throw std::runtime_error("Hikaru Metal backend requires a Metal device and queue");
            initialize(requestedDevice, requestedQueue);
        }
    }
    ~MetalBackend() override { @autoreleasepool { flushDraws(); } }
    void initialize(id<MTLDevice> requestedDevice, id<MTLCommandQueue> requestedQueue) {
        device = requestedDevice; queue = requestedQueue;
        inFlight = dispatch_semaphore_create(3);
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:metalShaderSource().c_str()];
        library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) throw std::runtime_error(std::string("Hikaru Metal shader compile failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
        MTLTextureDescriptor *fallbackDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        fallbackDesc.storageMode = MTLStorageModePrivate; fallbackDesc.usage = MTLTextureUsageShaderRead;
        fallback = [device newTextureWithDescriptor:fallbackDesc];
        const uint8_t transparent[4]={0,0,0,0};
        uploadColor(fallback,0,1,1,transparent);
        MTLTextureDescriptor *depthDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1 height:1 mipmapped:NO];
        depthDesc.storageMode=MTLStorageModePrivate;depthDesc.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        fallbackDepth=[device newTextureWithDescriptor:depthDesc];
        clearDepthTexture(fallbackDepth);
        MTLTextureDescriptor *metadataDesc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint width:1 height:1 mipmapped:NO];
        metadataDesc.storageMode=MTLStorageModePrivate;metadataDesc.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
        fallbackMetadata=[device newTextureWithDescriptor:metadataDesc];
        MTLRenderPassDescriptor *metadataClear=[MTLRenderPassDescriptor renderPassDescriptor];
        metadataClear.colorAttachments[0].texture=fallbackMetadata;
        metadataClear.colorAttachments[0].loadAction=MTLLoadActionClear;
        metadataClear.colorAttachments[0].storeAction=MTLStoreActionStore;
        metadataClear.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
        id<MTLCommandBuffer> clearCommand=[queue commandBuffer];
        [[clearCommand renderCommandEncoderWithDescriptor:metadataClear] endEncoding];commitCommandBuffer(clearCommand);
        textures[0] = {};
    }

    void texture(unsigned id, const Texture& info, unsigned level, TextureUpload upload, const void* pixels, size_t bytes) override {
      @autoreleasepool {
        if (!info.width || !info.height) {
            if(upload==TextureUpload::FullUpdate)throw std::runtime_error("Hikaru Metal full texture update has no compatible allocation");
            flushDraws();return;
        }
        auto existing=textures.find(id);
        if(upload==TextureUpload::FullUpdate&&existing==textures.end())throw std::runtime_error("Hikaru Metal full texture update has no compatible allocation");
        auto& entry = upload==TextureUpload::FullUpdate?existing->second:textures[id];
        const bool rb=(id & 0x80000000u)!=0;
        const auto format=pixelFormat(info.format);
        const unsigned geometric=1u+unsigned(std::floor(std::log2(std::max(info.width,info.height))));
        const unsigned levels=info.maxLevel==1000?geometric:std::max(1u,std::min(geometric,info.maxLevel+1));
        const bool compatible=entry.texture&&entry.info.width==info.width&&entry.info.height==info.height&&entry.info.format==info.format&&entry.levels==levels;
        if(upload==TextureUpload::FullUpdate&&!compatible)throw std::runtime_error("Hikaru Metal full texture update has no compatible allocation");
        if(upload==TextureUpload::FullUpdate) {
            if(!pixels||bytes==0||level!=0||isDepth(info.format))throw std::runtime_error("Hikaru Metal full texture update requires level-zero color pixel data");
            if(size_t(info.width)>SIZE_MAX/size_t(info.height)/4)throw std::runtime_error("Hikaru Metal full texture update is too large");
            if(bytes!=size_t(info.width)*info.height*4)throw std::runtime_error("Hikaru Metal texture upload has an invalid byte count");
        }
        flushDraws();
        if (!compatible) {
            MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:info.width height:info.height mipmapped:levels>1];
            desc.mipmapLevelCount=levels;
            // Private storage supports renderable and sampleable depth textures
            // on discrete and Apple silicon GPUs alike.
            desc.storageMode=MTLStorageModePrivate;
            desc.usage=isDepth(info.format) ? MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead : MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
            entry.texture=[device newTextureWithDescriptor:desc];
            if (!entry.texture) throw std::runtime_error("Unable to allocate Hikaru Metal texture");
            entry.info=info; entry.revision=info.revision; entry.levels=levels; entry.renderbuffer=rb;entry.initializedLevels.assign(levels,false);
            if (isDepth(info.format)) clearDepthTexture(entry.texture);
        } else {
            entry.info=info; entry.revision=info.revision;
        }
        if (pixels && bytes) {
            const unsigned w=std::max(1u,info.width>>level), h=std::max(1u,info.height>>level);
            if (level>=entry.levels) throw std::runtime_error("Hikaru Metal texture upload exceeds allocated mip levels");
            if (bytes < size_t(w)*h*4) throw std::runtime_error("Hikaru Metal texture upload has insufficient bytes");
            // The dispatch's unpack step expands legacy packed 16-bit/LA formats,
            // but intentionally preserves GL_UNSIGNED_INT_2_10_10_10_REV. Its
            // component bit order matches MTLPixelFormatRGB10A2Unorm exactly.
            if (isDepth(info.format)) throw std::runtime_error("CPU upload to a Hikaru Metal depth texture is unsupported");
            // Private storage is updated through a blit on the same serial queue, keeping prior draws before writes and later draws after them.
            uploadColor(entry.texture,level,w,h,pixels);
        }
        if(level>=entry.levels)throw std::runtime_error("Hikaru Metal texture upload exceeds allocated mip levels");
        entry.initializedLevels[level]=true;
      }
    }

    void beginOcclusion() override {
        if(occlusionActive)throw std::runtime_error("Nested Hikaru visibility query");
        flushDraws();
        visibilityBuffers.clear();
        occlusionActive=true;
    }
    bool endOcclusion() override {
        if(!occlusionActive)throw std::runtime_error("No active Hikaru visibility query");
        flushDraws();
        // A marker on the same queue waits only for the preceding query work.
        id<MTLCommandBuffer> marker=[queue commandBuffer];
        commitCommandBuffer(marker);
        [marker waitUntilCompleted];
        occlusionActive=false;
        if(marker.status==MTLCommandBufferStatusError)throw std::runtime_error("Hikaru visibility query failed");
        bool visible=false;
        for(id<MTLBuffer> buffer:visibilityBuffers) {
            const auto* results=static_cast<const uint64_t*>(buffer.contents);
            for(unsigned i=0;i<256;i++)visible|=results[i]!=0;
        }
        visibilityBuffers.clear();
        return visible;
    }
    void eraseTexture(unsigned id) override { @autoreleasepool { flushDraws();if (id) textures.erase(id); } }

    void clear(const Draw& state, GLbitfield mask, const Vec4& color, float depth) override {
      @autoreleasepool {
        flushDraws();
        auto colorTex=target(state.color,false), depthTex=target(state.depth,true);
        if (!colorTex && !depthTex) return;
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        const bool fullColorMask=std::all_of(state.colorMask.begin(),state.colorMask.end(),[](bool v){return v;});
        const bool nativeClear=!state.scissorEnabled && (!(mask&GL_COLOR_BUFFER_BIT)||fullColorMask) &&
                               (!(mask&GL_DEPTH_BUFFER_BIT)||state.depthWrite);
        if (colorTex) {
            auto a=pass.colorAttachments[0];a.texture=colorTex;a.storeAction=MTLStoreActionStore;
            a.loadAction=nativeClear&&(mask&GL_COLOR_BUFFER_BIT)?MTLLoadActionClear:MTLLoadActionLoad;
            if(a.loadAction==MTLLoadActionClear)a.clearColor=MTLClearColorMake(color[0],color[1],color[2],color[3]);
        }
        if (depthTex) {
            auto a=pass.depthAttachment;a.texture=depthTex;a.storeAction=MTLStoreActionStore;
            a.loadAction=nativeClear&&(mask&GL_DEPTH_BUFFER_BIT)?MTLLoadActionClear:MTLLoadActionLoad;
            if(a.loadAction==MTLLoadActionClear)a.clearDepth=depth;
        }
        if(nativeClear){id<MTLCommandBuffer> nativeCmd=[queue commandBuffer];id<MTLRenderCommandEncoder> nativeEnc=[nativeCmd renderCommandEncoderWithDescriptor:pass];[nativeEnc endEncoding];commitCommandBuffer(nativeCmd);return;}
        id<MTLCommandBuffer> cmd=[queue commandBuffer]; id<MTLRenderCommandEncoder> enc=[cmd renderCommandEncoderWithDescriptor:pass];
        if (!enc) return;
        const unsigned w=colorTex ? (unsigned)colorTex.width : (unsigned)depthTex.width;
        const unsigned h=colorTex ? (unsigned)colorTex.height : (unsigned)depthTex.height;
        MTLViewport vp={0,0,(double)w,(double)h,0,1};[enc setViewport:vp];
        GLint x=0,y=0,sw=(GLint)w,sh=(GLint)h;
        if (state.scissorEnabled) { x=state.scissor[0];y=state.color==0?(GLint)h-state.scissor[1]-state.scissor[3]:state.scissor[1];sw=state.scissor[2];sh=state.scissor[3]; }
        x=std::max<GLint>(0,x);y=std::max<GLint>(0,y);sw=std::max<GLint>(0,std::min<GLint>(sw,(GLint)w-x));sh=std::max<GLint>(0,std::min<GLint>(sh,(GLint)h-y));
        [enc setScissorRect:MTLScissorRect{(NSUInteger)x,(NSUInteger)y,(NSUInteger)sw,(NSUInteger)sh}];
        PipelineKey key{}; key.format=colorTex ? (uint32_t)pixelFormat(textures[state.color].info.format) : MTLPixelFormatInvalid;
        key.colorMask=0;if(mask&GL_COLOR_BUFFER_BIT)for(unsigned i=0;i<4;i++)if(state.colorMask[i])key.colorMask|=1u<<i;
        auto p=clearPipeline(key,depthTex!=nil); if (!p) { [enc endEncoding];commitCommandBuffer(cmd);return; }
        [enc setRenderPipelineState:p];
        if (depthTex) [enc setDepthStencilState:depthState(GL_ALWAYS,(mask&GL_DEPTH_BUFFER_BIT)!=0 && state.depthWrite)];
        struct alignas(16) ClearUniform { float color[4]; float depth; float padding[3]{}; } u{{color[0],color[1],color[2],color[3]},depth,{0,0,0}};
        [enc setVertexBytes:&u length:sizeof(u) atIndex:0];[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        if ((mask&GL_COLOR_BUFFER_BIT) || (mask&GL_DEPTH_BUFFER_BIT)) [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];commitCommandBuffer(cmd);
      }
    }

    void draw(const Draw& d) override {
      @autoreleasepool {
        auto color=target(d.color,false), depth=target(d.depth,true), metadata=d.metadata?target(d.metadata,false):nil;
        if (!color || d.vertices.empty() || (d.cull && d.cullFace==GL_FRONT_AND_BACK)) return;
        if(!beginDrawPass(d.color,d.depth,color,depth,d.metadata,metadata))return;
        id<MTLRenderCommandEncoder> enc=activeEncoder;
        const unsigned w=(unsigned)color.width,h=(unsigned)color.height;
        GLint vx=d.viewport[2]||d.viewport[3]?d.viewport[0]:0,vy=d.viewport[2]||d.viewport[3]?d.viewport[1]:0;
        GLint vw=d.viewport[2]||d.viewport[3]?d.viewport[2]:(GLint)w,vh=d.viewport[2]||d.viewport[3]?d.viewport[3]:(GLint)h;
        MTLViewport vp={(double)vx,(double)(d.color==0?(GLint)h-vy-vh:vy),(double)vw,(double)vh,d.nearDepth,d.farDepth};
        [enc setViewport:vp];
        GLint sx=0,sy=0,sw=(GLint)w,sh=(GLint)h;
        if(d.scissorEnabled){sx=d.scissor[0];sy=d.color==0?(GLint)h-d.scissor[1]-d.scissor[3]:d.scissor[1];sw=d.scissor[2];sh=d.scissor[3];}
        sx=std::max<GLint>(0,sx);sy=std::max<GLint>(0,sy);sw=std::max<GLint>(0,std::min<GLint>(sw,(GLint)w-sx));sh=std::max<GLint>(0,std::min<GLint>(sh,(GLint)h-sy));
        [enc setScissorRect:MTLScissorRect{(NSUInteger)sx,(NSUInteger)sy,(NSUInteger)sw,(NSUInteger)sh}];
        PipelineKey key{};key.bitmap=d.bitmap;key.familyResolve=d.familyResolve;key.metadataFormat=metadata?(uint32_t)MTLPixelFormatRG32Uint:0;key.blend=d.blend;key.cull=d.cull;key.depth=depth!=nil;key.format=(uint32_t)pixelFormat(textures[d.color].info.format);
        key.srcRGB=d.srcRGB;key.dstRGB=d.dstRGB;key.srcAlpha=d.srcAlpha;key.dstAlpha=d.dstAlpha;key.eqRGB=d.equationRGB;key.eqAlpha=d.equationAlpha;key.cullFace=d.cullFace;key.frontFace=d.frontFace;
        for(unsigned i=0;i<4;i++)if(d.colorMask[i])key.colorMask|=1u<<i;
        id<MTLRenderPipelineState> pipeline=pipelineFor(key);if(!pipeline)return;
        [enc setRenderPipelineState:pipeline];
        if(d.cull){MTLCullMode cm=d.cullFace==GL_FRONT?MTLCullModeFront:MTLCullModeBack;[enc setCullMode:cm];}
        else [enc setCullMode:MTLCullModeNone];
        // Metal's framebuffer Y direction reverses GL winding for internal
        // render targets. The screen shader flips Y itself, cancelling that
        // reversal for texture ID zero.
        bool ccw=d.frontFace==GL_CCW;
        if(d.uniforms.inverted[2]!=0.0f)ccw=!ccw;
        [enc setFrontFacingWinding:ccw?MTLWindingCounterClockwise:MTLWindingClockwise];
        if(depth)[enc setDepthStencilState:depthState(d.depthTest?d.depthFunc:GL_ALWAYS,d.depthTest&&d.depthWrite)];
        // Encoder state survives between guest draws in a batched pass.
        // Reset zero values explicitly so a prior biased draw cannot affect
        // following geometry.
        [enc setDepthBias:d.biasUnits slopeScale:d.biasFactor clamp:0];
        setVertexData(enc,d.vertices.data(),d.vertices.size());[enc setVertexBytes:&d.uniforms length:sizeof(d.uniforms) atIndex:1];[enc setFragmentBytes:&d.uniforms length:sizeof(d.uniforms) atIndex:1];
        for(unsigned i=0;i<4;i++){
            id<MTLTexture> t=nil;auto it=textures.find(d.textures[i]);
            const bool meshDepthSlot=!d.bitmap&&(i==1||i==2);
            const bool metadataSlot=(!d.bitmap&&i==3)||(d.familyResolve&&i>=2);
            if(metadataSlot){
                if(d.textures[i]&&d.textures[i]!=d.metadata&&it!=textures.end()&&it->second.info.format==GL_RG32UI)t=it->second.texture;
                [enc setFragmentTexture:t?:fallbackMetadata atIndex:i];
            } else if(meshDepthSlot){
                if(d.textures[i] && it!=textures.end() && isDepth(it->second.info.format)) t=it->second.texture;
                [enc setFragmentTexture:t?:fallbackDepth atIndex:i];
            } else {
                if(d.textures[i] && d.textures[i]!=d.color && d.textures[i]!=d.depth && d.textures[i]!=d.metadata && it!=textures.end() && !isDepth(it->second.info.format)) {
                    const auto& sampled=it->second;
                    const GLenum minFilter=d.sampling[i].minFilter;
                    const bool mipmapped=minFilter==GL_NEAREST_MIPMAP_NEAREST||minFilter==GL_NEAREST_MIPMAP_LINEAR||minFilter==GL_LINEAR_MIPMAP_NEAREST||minFilter==GL_LINEAR_MIPMAP_LINEAR;
                    const unsigned geometric=1u+unsigned(std::floor(std::log2(std::max(sampled.info.width,sampled.info.height))));
                    const unsigned expected=mipmapped?(d.sampling[i].maxLevel==1000?geometric:std::min(geometric,d.sampling[i].maxLevel+1)):1u;
                    bool complete=expected<=sampled.levels;
                    for(unsigned level=0;complete&&level<expected;level++)complete=sampled.initializedLevels[level];
                    if(complete)t=sampled.texture;
                }
                [enc setFragmentTexture:t?:fallback atIndex:i];
            }
            [enc setFragmentSamplerState:samplerFor(d.sampling[i]) atIndex:i];
        }
        if(occlusionActive)[enc setVisibilityResultMode:MTLVisibilityResultModeBoolean offset:activeDraws*sizeof(uint64_t)];
        MTLPrimitiveType pt=primitiveType(d.topology);
        if(d.topology==GL_TRIANGLE_FAN){ // Expand fan to a triangle list because Metal has no triangle-fan primitive.
            std::vector<Vertex> expanded;expanded.reserve((d.vertices.size()-2)*3);for(size_t i=1;i+1<d.vertices.size();i++){expanded.push_back(d.vertices[0]);expanded.push_back(d.vertices[i]);expanded.push_back(d.vertices[i+1]);}
            setVertexData(enc,expanded.data(),expanded.size());[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:expanded.size()];
        } else [enc drawPrimitives:pt vertexStart:0 vertexCount:d.vertices.size()];
        if(++activeDraws>=256)flushDraws();
      }
    }

    void readPixels(unsigned textureId,int x,int y,int width,int height,uint8_t* rgba) override {
      @autoreleasepool {
        flushDraws();
        auto t=target(textureId,false);if(!t||!rgba||width<=0||height<=0)return;
        const NSUInteger bpr=(NSUInteger(t.width)*4+255u)&~NSUInteger(255u);
        id<MTLBuffer> staging=[device newBufferWithLength:bpr*t.height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cmd=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
        [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bpr*t.height];
        if(t.storageMode==MTLStorageModeManaged)[blit synchronizeResource:t];
        [blit endEncoding];commitCommandBuffer(cmd);[cmd waitUntilCompleted];
        if(cmd.status==MTLCommandBufferStatusError)throw std::runtime_error(std::string("Hikaru Metal readback failed: ")+(cmd.error.localizedDescription.UTF8String?:"unknown error"));
        const uint8_t* source=(const uint8_t*)staging.contents;
        for(int iy=0;iy<height;iy++){
            int gy=y+iy, rowTop=textureId==0?(int)t.height-1-gy:gy;
            if(gy<0||gy>=(int)t.height){std::memset(rgba+size_t(iy)*width*4,0,size_t(width)*4);continue;}
            const uint8_t* row=source+size_t(rowTop)*bpr;
            for(int ix=0;ix<width;ix++){int gx=x+ix;auto* dst=rgba+(size_t(iy)*width+ix)*4;if(gx<0||gx>=(int)t.width)std::memset(dst,0,4);else if(textures[textureId].info.format==GL_RGB10_A2){uint32_t v;std::memcpy(&v,row+size_t(gx)*4,4);for(unsigned c=0;c<3;c++){unsigned b=(v>>(10*c))&1023u;dst[c]=uint8_t((b*255u+511u)/1023u);}dst[3]=uint8_t((((v>>30)&3u)*255u+1u)/3u);}else std::memcpy(dst,row+size_t(gx)*4,4);}
        }
      }
    }
    void present(unsigned width,unsigned height) override {
      @autoreleasepool {
        flushDraws();
        auto t=target(0,false);if(!presenter||!t)return;
        presenter(t,height?float(width)/float(height):0.f);
      }
    }

private:
    std::function<void(id<MTLTexture>,float)> presenter;id<MTLDevice> device=nil;id<MTLCommandQueue> queue=nil;id<MTLLibrary> library=nil;id<MTLTexture> fallback=nil,fallbackDepth=nil,fallbackMetadata=nil;
    dispatch_semaphore_t inFlight=nullptr;
    id<MTLCommandBuffer> activeCommand=nil;
    id<MTLRenderCommandEncoder> activeEncoder=nil;
    unsigned activeColor=~0u,activeDepth=~0u,activeMetadata=~0u,activeDraws=0;
    bool occlusionActive=false;
    std::vector<id<MTLBuffer>> visibilityBuffers;
    std::map<unsigned,TextureEntry> textures;std::map<PipelineKey,id<MTLRenderPipelineState>> pipelines;std::map<std::tuple<uint32_t,uint8_t,bool>,id<MTLRenderPipelineState>> clearPipelines;std::map<SamplerKey,id<MTLSamplerState>> samplers;std::map<std::pair<GLenum,bool>,id<MTLDepthStencilState>> depths;
    void commitCommandBuffer(id<MTLCommandBuffer> cmd) {
        if(!cmd)return;
        dispatch_semaphore_wait(inFlight,DISPATCH_TIME_FOREVER);
        dispatch_semaphore_t semaphore=inFlight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(semaphore);}];
        [cmd commit];
    }
    void flushDraws() {
        if(!activeCommand)return;
        [activeEncoder endEncoding];
        id<MTLCommandBuffer> command=activeCommand;
        activeEncoder=nil;activeCommand=nil;activeColor=activeDepth=activeMetadata=~0u;activeDraws=0;
        commitCommandBuffer(command);
    }
    bool beginDrawPass(unsigned colorID,unsigned depthID,id<MTLTexture> color,id<MTLTexture> depth,unsigned metadataID,id<MTLTexture> metadata) {
        if(activeEncoder&&activeColor==colorID&&activeDepth==depthID&&activeMetadata==metadataID&&activeDraws<256)return true;
        flushDraws();
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        if(metadata){pass.colorAttachments[1].texture=metadata;pass.colorAttachments[1].loadAction=MTLLoadActionLoad;pass.colorAttachments[1].storeAction=MTLStoreActionStore;}
        if(depth){pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;}
        if(occlusionActive) {
            id<MTLBuffer> results=[device newBufferWithLength:256*sizeof(uint64_t) options:MTLResourceStorageModeShared];
            if(!results)throw std::runtime_error("Cannot allocate Hikaru visibility results");
            std::memset(results.contents,0,256*sizeof(uint64_t));
            visibilityBuffers.push_back(results);
            pass.visibilityResultBuffer=results;
        }
        activeCommand=[queue commandBuffer];activeEncoder=[activeCommand renderCommandEncoderWithDescriptor:pass];
        if(!activeEncoder){activeCommand=nil;return false;}
        activeColor=colorID;activeDepth=depthID;activeMetadata=metadataID;activeDraws=0;return true;
    }
    void setVertexData(id<MTLRenderCommandEncoder> encoder,const Vertex* vertices,size_t count) {
        const size_t byteCount=count*sizeof(Vertex);
        // Metal's setVertexBytes uses a small, command-buffer-owned transient
        // allocation. This avoids one Objective-C buffer plus a command per
        // guest draw (including the common single-triangle HUD batches).
        if(byteCount<=4096){[encoder setVertexBytes:vertices length:byteCount atIndex:0];return;}
        id<MTLBuffer> buffer=[device newBufferWithBytes:vertices length:byteCount options:MTLResourceStorageModeShared];
        [encoder setVertexBuffer:buffer offset:0 atIndex:0];
    }
    id<MTLTexture> target(unsigned id,bool depth){auto it=textures.find(id);if(it==textures.end()||!it->second.texture)return nil;return isDepth(it->second.info.format)==depth?it->second.texture:nil;}
    void uploadColor(id<MTLTexture> texture,unsigned level,unsigned width,unsigned height,const void* pixels){
        const NSUInteger bpr=(NSUInteger(width)*4+255u)&~NSUInteger(255u);std::vector<uint8_t> bytes(bpr*height);
        const auto* source=static_cast<const uint8_t*>(pixels);for(unsigned y=0;y<height;y++)std::memcpy(bytes.data()+size_t(y)*bpr,source+size_t(y)*width*4,size_t(width)*4);
        id<MTLBuffer> staging=[device newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cmd=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cmd blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:bpr sourceBytesPerImage:bpr*height sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:0 destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding];commitCommandBuffer(cmd);
    }
    void clearDepthTexture(id<MTLTexture> texture){
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.depthAttachment.texture=texture;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=1.0;
        id<MTLCommandBuffer> cmd=[queue commandBuffer];id<MTLRenderCommandEncoder> enc=[cmd renderCommandEncoderWithDescriptor:pass];[enc endEncoding];commitCommandBuffer(cmd);
    }
    id<MTLSamplerState> samplerFor(const Texture& s){const bool mipmapped=s.minFilter==GL_NEAREST_MIPMAP_NEAREST||s.minFilter==GL_NEAREST_MIPMAP_LINEAR||s.minFilter==GL_LINEAR_MIPMAP_NEAREST||s.minFilter==GL_LINEAR_MIPMAP_LINEAR;SamplerKey key{s.minFilter,s.magFilter,s.wrapS,s.wrapT,mipmapped?s.maxLevel:0};auto i=samplers.find(key);if(i!=samplers.end())return i->second;MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];d.minFilter=(s.minFilter==GL_NEAREST||s.minFilter==GL_NEAREST_MIPMAP_NEAREST||s.minFilter==GL_NEAREST_MIPMAP_LINEAR)?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;d.magFilter=s.magFilter==GL_NEAREST?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;d.mipFilter=!mipmapped?MTLSamplerMipFilterNotMipmapped:(s.minFilter==GL_NEAREST_MIPMAP_LINEAR||s.minFilter==GL_LINEAR_MIPMAP_LINEAR)?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;d.lodMaxClamp=mipmapped?(float)s.maxLevel:0.f;d.sAddressMode=addressMode(s.wrapS);d.tAddressMode=addressMode(s.wrapT);auto state=[device newSamplerStateWithDescriptor:d];samplers[key]=state;return state;}
    id<MTLDepthStencilState> depthState(GLenum func,bool write){auto key=std::make_pair(func,write);auto i=depths.find(key);if(i!=depths.end())return i->second;MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];d.depthCompareFunction=compareFunction(func);d.depthWriteEnabled=write;auto s=[device newDepthStencilStateWithDescriptor:d];depths[key]=s;return s;}
    id<MTLRenderPipelineState> pipelineFor(const PipelineKey& k){
        auto found=pipelines.find(k);if(found!=pipelines.end())return found->second;
        NSString *vertex=k.bitmap?@"bitmapVertex":@"meshVertex";
        NSString *fragment=k.familyResolve?@"familyResolveFragment":k.bitmap?@"bitmapFragment":k.metadataFormat?@"familyMeshFragment":@"meshFragment";
        MTLRenderPipelineDescriptor *descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:vertex];descriptor.fragmentFunction=[library newFunctionWithName:fragment];
        descriptor.colorAttachments[0].pixelFormat=(MTLPixelFormat)k.format;
        if(k.metadataFormat){descriptor.colorAttachments[1].pixelFormat=(MTLPixelFormat)k.metadataFormat;descriptor.colorAttachments[1].writeMask=MTLColorWriteMaskAll;}
        auto color=descriptor.colorAttachments[0];color.writeMask=metalColorWriteMask(k.colorMask);color.blendingEnabled=k.blend;
        if(k.blend){color.sourceRGBBlendFactor=blendFactor(k.srcRGB);color.destinationRGBBlendFactor=blendFactor(k.dstRGB);color.sourceAlphaBlendFactor=blendFactor(k.srcAlpha);color.destinationAlphaBlendFactor=blendFactor(k.dstAlpha);color.rgbBlendOperation=blendOperation(k.eqRGB);color.alphaBlendOperation=blendOperation(k.eqAlpha);}
        if(k.depth)descriptor.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        NSError *error=nil;auto pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if(!pipeline)throw std::runtime_error(std::string("Hikaru Metal pipeline creation failed: ")+(error.localizedDescription.UTF8String?:"unknown error"));
        pipelines[k]=pipeline;return pipeline;
    }
    id<MTLRenderPipelineState> clearPipeline(const PipelineKey& key,bool withDepth){ // partial clears need channel-mask and scissor support
        auto cacheKey=std::make_tuple(key.format,key.colorMask,withDepth);auto found=clearPipelines.find(cacheKey);if(found!=clearPipelines.end())return found->second;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;struct U{float4 color;float depth;};vertex float4 cv(uint i [[vertex_id]],constant U& u [[buffer(0)]]){float2 p[3]={{-1,-1},{3,-1},{-1,3}};return float4(p[i],u.depth,1);}fragment float4 cf(constant U& u [[buffer(0)]]){return u.color;}";NSError *e=nil;id<MTLLibrary> l=[device newLibraryWithSource:source options:nil error:&e];if(!l)throw std::runtime_error(std::string("Hikaru Metal clear shader compile failed: ")+(e.localizedDescription.UTF8String?:"unknown error"));MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[l newFunctionWithName:@"cv"];d.fragmentFunction=[l newFunctionWithName:@"cf"];d.colorAttachments[0].pixelFormat=(MTLPixelFormat)key.format;d.colorAttachments[0].writeMask=metalColorWriteMask(key.colorMask);if(withDepth)d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;id<MTLRenderPipelineState> p=[device newRenderPipelineStateWithDescriptor:d error:&e];if(!p)throw std::runtime_error(std::string("Hikaru Metal clear pipeline failed: ")+(e.localizedDescription.UTF8String?:"unknown error"));clearPipelines[cacheKey]=p;return p;}
};
}
std::unique_ptr<Backend> makeMetalBackend() {
    auto* context=dynamic_cast<MetalContext*>(GraphicsContext::Instance());
    if(!context||!context->GetDevice()||!context->GetQueue())return {};
    return std::make_unique<MetalBackend>(context->GetDevice(),context->GetQueue(),[context](id<MTLTexture> texture,float aspect){context->PresentFrame(texture,aspect);});
}
std::unique_ptr<Backend> makeMetalBackendForTesting(id<MTLDevice> device,id<MTLCommandQueue> queue) { return std::make_unique<MetalBackend>(device,queue); }
} // namespace hikaru::graphics
