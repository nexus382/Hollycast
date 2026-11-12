// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include "shaders.h"

#if defined(HAVE_D3D11) && !defined(LIBRETRO)
#include "rend/dx11/dx11context.h"
#include "rend/dx11/dx11_shaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <future>
#include <system_error>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace hikaru::graphics {
namespace {
void checkResult(HRESULT result,const char* operation) {
    if(FAILED(result)) {
        std::ostringstream message;message<<"Hikaru DX11 "<<operation<<" failed (0x"<<std::hex<<unsigned(result)<<")";
        throw FlycastException(message.str());
    }
}
D3D11_COMPARISON_FUNC comparison(GLenum value) {
    switch(value) {
    case GL_NEVER:return D3D11_COMPARISON_NEVER;case GL_LESS:return D3D11_COMPARISON_LESS;
    case GL_EQUAL:return D3D11_COMPARISON_EQUAL;case GL_LEQUAL:return D3D11_COMPARISON_LESS_EQUAL;
    case GL_GREATER:return D3D11_COMPARISON_GREATER;case GL_NOTEQUAL:return D3D11_COMPARISON_NOT_EQUAL;
    case GL_GEQUAL:return D3D11_COMPARISON_GREATER_EQUAL;default:return D3D11_COMPARISON_ALWAYS;
    }
}
D3D11_BLEND blendFactor(GLenum value,bool alpha=false) {
    switch(value) {
    case GL_ZERO:return D3D11_BLEND_ZERO;case GL_ONE:return D3D11_BLEND_ONE;
    case GL_SRC_COLOR:return alpha?D3D11_BLEND_SRC_ALPHA:D3D11_BLEND_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR:return alpha?D3D11_BLEND_INV_SRC_ALPHA:D3D11_BLEND_INV_SRC_COLOR;
    case GL_DST_COLOR:return alpha?D3D11_BLEND_DEST_ALPHA:D3D11_BLEND_DEST_COLOR;
    case GL_ONE_MINUS_DST_COLOR:return alpha?D3D11_BLEND_INV_DEST_ALPHA:D3D11_BLEND_INV_DEST_COLOR;
    case GL_SRC_ALPHA:return D3D11_BLEND_SRC_ALPHA;case GL_ONE_MINUS_SRC_ALPHA:return D3D11_BLEND_INV_SRC_ALPHA;
    case GL_DST_ALPHA:return D3D11_BLEND_DEST_ALPHA;case GL_ONE_MINUS_DST_ALPHA:return D3D11_BLEND_INV_DEST_ALPHA;
    default:throw FlycastException("Unsupported Hikaru DX11 blend factor");
    }
}
D3D11_BLEND_OP blendOperation(GLenum value) {
    switch(value) {
    case GL_FUNC_ADD:return D3D11_BLEND_OP_ADD;case GL_FUNC_SUBTRACT:return D3D11_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT:return D3D11_BLEND_OP_REV_SUBTRACT;case GL_MIN:return D3D11_BLEND_OP_MIN;
    case GL_MAX:return D3D11_BLEND_OP_MAX;default:throw FlycastException("Unsupported Hikaru DX11 blend operation");
    }
}
D3D11_TEXTURE_ADDRESS_MODE addressMode(GLenum value) {
    return value==GL_REPEAT?D3D11_TEXTURE_ADDRESS_WRAP:value==GL_MIRRORED_REPEAT?D3D11_TEXTURE_ADDRESS_MIRROR:D3D11_TEXTURE_ADDRESS_CLAMP;
}
bool isMipmapped(GLenum value) {return value!=GL_NEAREST&&value!=GL_LINEAR;}
unsigned mipCount(unsigned width,unsigned height) {
    unsigned count=1;while((width>>=1)|(height>>=1))++count;return count;
}
ComPtr<ID3DBlob> compileCode(pD3DCompile compiler,const std::string& source,const char* entry,const char* profile) {
    ComPtr<ID3DBlob> code,errors;
    const HRESULT result=compiler(source.data(),source.size(),"hikaru-dx11",nullptr,nullptr,entry,profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code.get(),&errors.get());
    if(FAILED(result)&&errors)
        throw FlycastException("Hikaru DX11 shader: "+std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()));
    checkResult(result,"shader compilation");return code;
}
class Dx11Backend final : public Backend, private CachedDX11Shaders {
    struct Image {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11DepthStencilView> depthView;
        D3D11_TEXTURE2D_DESC description{};
        std::vector<bool> initialized;
        bool depth=false,sampleable=true;
    };
    struct PixelKey {
        bool bitmap=false,family=false,palette=false,peelComposite=false;
        MeshShaderSpecialization constants{};
        unsigned passFlags=0;
        bool operator<(const PixelKey& other) const {return std::tie(bitmap,family,palette,peelComposite,constants,passFlags)<std::tie(other.bitmap,other.family,other.palette,other.peelComposite,other.constants,other.passFlags);}
    };
    struct PixelShader {ComPtr<ID3D11PixelShader> shader;uint64_t touched=0;bool deferred=false;};
    struct CompiledPixel {ComPtr<ID3DBlob> code;ComPtr<ID3D11PixelShader> shader;};
    struct PendingPixel {PixelKey key;u64 hash;std::future<CompiledPixel> result;};
    using BlendKey=std::tuple<bool,GLenum,GLenum,GLenum,GLenum,GLenum,GLenum,unsigned>;
    using DepthKey=std::tuple<bool,bool,GLenum>;
    using RasterKey=std::tuple<bool,GLenum,bool,bool,float,float>;
    using SamplerKey=std::tuple<GLenum,GLenum,GLenum,GLenum,unsigned>;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    pD3DCompile compiler=nullptr;
    DX11Context* owner=nullptr;
    bool specialize=true,bindingsDirty=true,backgroundCompilation=false;
    std::array<ComPtr<ID3D11PixelShader>,4> genericPixels;
    std::optional<PendingPixel> pendingPixel;
    std::unordered_map<unsigned,Image> images;
    std::map<PixelKey,PixelShader> pixelShaders;
    std::map<BlendKey,ComPtr<ID3D11BlendState>> blends;
    std::map<DepthKey,ComPtr<ID3D11DepthStencilState>> depths;
    std::map<RasterKey,ComPtr<ID3D11RasterizerState>> rasters;
    std::map<SamplerKey,ComPtr<ID3D11SamplerState>> samplers;
    std::array<ComPtr<ID3D11VertexShader>,6> vertexShaders;
    std::array<ComPtr<ID3D11InputLayout>,6> inputLayouts;
    ComPtr<ID3D11VertexShader> clearVertex;
    ComPtr<ID3D11PixelShader> clearPixel,clearIntegerPixel;
    ComPtr<ID3D11Buffer> vertexBuffer,vertexUniformBuffer,pixelUniformBuffer,paletteBuffer;
    size_t vertexCapacity=0,vertexOffset=0;
    static constexpr UINT ConstantStream=64*1024,ConstantWindow=UINT((sizeof(Uniforms)+255)/256*256);
    size_t vertexConstantsCursor=0,pixelConstantsCursor=0;
    UINT vertexConstantsOffset=0,pixelConstantsOffset=0;
    uint64_t shaderClock=0;
    std::array<ID3D11RenderTargetView*,2> boundTargets{};
    ID3D11DepthStencilView* boundDepth=nullptr;
    ID3D11VertexShader* boundVertex=nullptr;
    ID3D11PixelShader* boundPixel=nullptr;
    ID3D11BlendState* boundBlend=nullptr;
    ID3D11DepthStencilState* boundDepthState=nullptr;
    ID3D11RasterizerState* boundRaster=nullptr;
    ID3D11InputLayout* boundLayout=nullptr;
    ID3D11Buffer* boundVertexBuffer=nullptr;
    UINT boundVertexStride=0;
    D3D11_PRIMITIVE_TOPOLOGY boundTopology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D11_VIEWPORT boundViewport{};
    D3D11_RECT boundScissor{};
    std::array<ID3D11ShaderResourceView*,19> boundInputs{};
    std::array<ID3D11SamplerState*,16> boundSamplers{};
    Uniforms uploadedUniforms{};
    std::array<DrawRecord,DrawPaletteSize> uploadedPalette{};
    size_t uploadedPaletteSize=0;
    bool vertexUniformsValid=false,pixelUniformsValid=false;
    bool occlusionActive=false,queryOpen=false;
    // Separate from Dreamcast's shader cache. Bump this version if compilation
    // flags change; source, entry point and shader profile are already hashed.
    static constexpr const char* CacheFile="hikaru_dx11_shader_cache_v1.bin";
    std::vector<ComPtr<ID3D11Query>> queries;
    std::deque<std::vector<ComPtr<ID3D11Query>>> pendingOcclusions;
    static constexpr unsigned ColorFallback=0x7ffffffdu,MetadataFallback=0x7ffffffcu;

    ComPtr<ID3DBlob> compile(const std::string& source,const char* entry,const char* profile) {
        ComPtr<ID3DBlob> code;const u64 hash=hashShader(source.c_str(),entry,profile);
        if(lookupShader(hash,code))return code;
        code=compileCode(compiler,source,entry,profile);cacheShader(hash,code);return code;
    }
    const ComPtr<ID3D11PixelShader>& genericPixel(const PixelKey& key) const {
        // Transform palettes and indexed textures are independent shader modes.
        return genericPixels[(key.palette?2u:0u)+(key.constants[8]?1u:0u)];
    }
    ID3D11PixelShader* retainPixel(const PixelKey& key,ComPtr<ID3D11PixelShader> shader,bool deferred=false) {
        if(pixelShaders.size()>=512&&pixelShaders.find(key)==pixelShaders.end()) {
            auto oldest=std::min_element(pixelShaders.begin(),pixelShaders.end(),[](const auto& a,const auto& b){return a.second.touched<b.second.touched;});
            pixelShaders.erase(oldest);
        }
        return pixelShaders.insert_or_assign(key,PixelShader{std::move(shader),++shaderClock,deferred}).first->second.shader;
    }
    void completePixelCompilation(bool wait) {
        if(!pendingPixel||(!wait&&pendingPixel->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready))return;
        auto pending=std::move(*pendingPixel);pendingPixel.reset();
        ComPtr<ID3D11PixelShader> shader;
        try {
            auto compiled=pending.result.get();shader=std::move(compiled.shader);
            cacheShader(pending.hash,compiled.code);
        } catch(const std::exception& error) {
            // The generic shader remains exact if an optional specialization
            // fails. Cache that fallback under this key to avoid a retry storm.
            WARN_LOG(RENDERER,"Hikaru DX11 specialized shader: %s",error.what());
            shader=genericPixel(pending.key);
        }
        retainPixel(pending.key,std::move(shader));
    }
    ComPtr<ID3D11Buffer> buffer(size_t bytes,UINT bind) {
        if(!bytes||bytes>UINT_MAX)throw FlycastException("Invalid Hikaru DX11 buffer size");
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(bytes);desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=bind;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> result;checkResult(device->CreateBuffer(&desc,nullptr,&result.get()),"buffer allocation");return result;
    }
    void upload(ID3D11Buffer* target,const void* data,size_t bytes,D3D11_MAP mode=D3D11_MAP_WRITE_DISCARD,size_t offset=0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};checkResult(context->Map(target,0,mode,0,&mapped),"buffer map");
        std::memcpy(static_cast<uint8_t*>(mapped.pData)+offset,data,bytes);context->Unmap(target,0);
    }
    UINT constantOffset(size_t& cursor) {
        if(!context1)return 0;
        if(cursor+ConstantWindow>ConstantStream)cursor=0;
        const UINT offset=UINT(cursor);cursor+=ConstantWindow;return offset;
    }
    void unbindInputs() {
        if(bindingsDirty||std::any_of(boundInputs.begin(),boundInputs.end(),[](auto* view){return view!=nullptr;})) {
            boundInputs={};context->PSSetShaderResources(0,UINT(boundInputs.size()),boundInputs.data());
        }
    }
    void unbindTargets() {
        context->OMSetRenderTargets(0,nullptr,nullptr);boundTargets={};boundDepth=nullptr;bindingsDirty=true;
    }
    Image& image(unsigned id) {
        auto found=images.find(id);if(found==images.end())throw FlycastException("Unknown Hikaru DX11 image");return found->second;
    }
    Image makeImage(unsigned id,const Texture& info) {
        if(!info.width||!info.height||info.width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||info.height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw FlycastException("Invalid Hikaru DX11 image size");
        Image result;
        result.depth=(id&0x80000000u)||info.format==GL_DEPTH_COMPONENT||info.format==GL_DEPTH_COMPONENT16||
            info.format==GL_DEPTH_COMPONENT24||info.format==GL_DEPTH_COMPONENT32F||info.format==GL_DEPTH24_STENCIL8;
        result.sampleable=!result.depth||(id&0x80000000u)==0;
        auto& desc=result.description;desc.Width=info.width;desc.Height=info.height;desc.ArraySize=1;desc.SampleDesc.Count=1;
        desc.MipLevels=result.depth?1:1+std::min(info.maxLevel,mipCount(info.width,info.height)-1);
        desc.Usage=D3D11_USAGE_DEFAULT;
        DXGI_FORMAT sampleFormat=DXGI_FORMAT_UNKNOWN,depthFormat=DXGI_FORMAT_UNKNOWN;
        if(result.depth) {
            desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|(result.sampleable?D3D11_BIND_SHADER_RESOURCE:0);
            if(info.format==GL_DEPTH_COMPONENT16){desc.Format=DXGI_FORMAT_R16_TYPELESS;depthFormat=DXGI_FORMAT_D16_UNORM;sampleFormat=DXGI_FORMAT_R16_UNORM;}
            else if(info.format==GL_DEPTH_COMPONENT24||info.format==GL_DEPTH24_STENCIL8){desc.Format=DXGI_FORMAT_R24G8_TYPELESS;depthFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;sampleFormat=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;}
            else {desc.Format=DXGI_FORMAT_R32_TYPELESS;depthFormat=DXGI_FORMAT_D32_FLOAT;sampleFormat=DXGI_FORMAT_R32_FLOAT;}
        } else {
            desc.Format=info.format==GL_RGB10_A2?DXGI_FORMAT_R10G10B10A2_UNORM:info.format==GL_RG32UI?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R8G8B8A8_UNORM;
            sampleFormat=desc.Format;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        }
        checkResult(device->CreateTexture2D(&desc,nullptr,&result.texture.get()),"texture allocation");
        if(result.sampleable) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=sampleFormat;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=desc.MipLevels;
            checkResult(device->CreateShaderResourceView(result.texture,&view,&result.view.get()),"texture view");
        }
        if(result.depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};view.Format=depthFormat;view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            checkResult(device->CreateDepthStencilView(result.texture,&view,&result.depthView.get()),"depth view");
        } else checkResult(device->CreateRenderTargetView(result.texture,nullptr,&result.target.get()),"color target");
        result.initialized.resize(desc.MipLevels,false);return result;
    }
    ID3D11BlendState* blendState(const Draw& draw) {
        unsigned mask=0;for(unsigned i=0;i<4;++i)if(draw.colorMask[i])mask|=1u<<i;
        const BlendKey key{draw.blend,draw.srcRGB,draw.dstRGB,draw.srcAlpha,draw.dstAlpha,draw.equationRGB,draw.equationAlpha,mask};
        auto found=blends.find(key);if(found!=blends.end())return found->second;
        D3D11_BLEND_DESC desc{};desc.IndependentBlendEnable=true;
        for(auto& state:desc.RenderTarget) {
            state.SrcBlend=state.SrcBlendAlpha=D3D11_BLEND_ONE;state.DestBlend=state.DestBlendAlpha=D3D11_BLEND_ZERO;
            state.BlendOp=state.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        }
        auto& state=desc.RenderTarget[0];state.BlendEnable=draw.blend;
        state.SrcBlend=blendFactor(draw.srcRGB);state.DestBlend=blendFactor(draw.dstRGB);state.BlendOp=blendOperation(draw.equationRGB);
        state.SrcBlendAlpha=blendFactor(draw.srcAlpha,true);state.DestBlendAlpha=blendFactor(draw.dstAlpha,true);state.BlendOpAlpha=blendOperation(draw.equationAlpha);
        state.RenderTargetWriteMask=UINT8(mask);desc.RenderTarget[1].RenderTargetWriteMask=UINT8(mask);
        ComPtr<ID3D11BlendState> result;checkResult(device->CreateBlendState(&desc,&result.get()),"blend state");return blends.emplace(key,std::move(result)).first->second;
    }
    ID3D11DepthStencilState* depthState(const Draw& draw) {
        const bool enabled=draw.depth&&draw.depthTest;
        const DepthKey key{enabled,enabled&&draw.depthWrite,draw.depthFunc};
        auto found=depths.find(key);if(found!=depths.end())return found->second;
        D3D11_DEPTH_STENCIL_DESC desc{};desc.DepthEnable=enabled;desc.DepthWriteMask=enabled&&draw.depthWrite?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc=comparison(draw.depthFunc);desc.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;desc.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
        desc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};desc.BackFace=desc.FrontFace;
        ComPtr<ID3D11DepthStencilState> result;checkResult(device->CreateDepthStencilState(&desc,&result.get()),"depth state");return depths.emplace(key,std::move(result)).first->second;
    }
    ID3D11RasterizerState* rasterState(const Draw& draw) {
        // D3D and Metal use the same visual front-face convention. Our extra
        // offscreen clip-Y flip reverses that winding for internal targets.
        const bool ccw=(draw.frontFace==GL_CCW)!=(draw.uniforms.inverted[2]!=0.f);
        if(!std::isfinite(draw.biasFactor)||!std::isfinite(draw.biasUnits)||draw.biasUnits<INT_MIN||draw.biasUnits>=float(INT_MAX))
            throw FlycastException("Invalid Hikaru DX11 depth bias");
        const RasterKey key{draw.cull,draw.cullFace,draw.scissorEnabled,ccw,draw.biasFactor,draw.biasUnits};
        auto found=rasters.find(key);if(found!=rasters.end())return found->second;
        D3D11_RASTERIZER_DESC desc{};desc.FillMode=D3D11_FILL_SOLID;desc.CullMode=!draw.cull?D3D11_CULL_NONE:draw.cullFace==GL_FRONT?D3D11_CULL_FRONT:D3D11_CULL_BACK;
        desc.FrontCounterClockwise=ccw;desc.ScissorEnable=draw.scissorEnabled;desc.DepthClipEnable=true;
        desc.DepthBias=INT(std::lround(draw.biasUnits));desc.SlopeScaledDepthBias=draw.biasFactor;
        ComPtr<ID3D11RasterizerState> result;checkResult(device->CreateRasterizerState(&desc,&result.get()),"rasterizer state");return rasters.emplace(key,std::move(result)).first->second;
    }
    ID3D11SamplerState* sampler(const Texture& sampling,const Image& sampled) {
        const unsigned maxLod=isMipmapped(sampling.minFilter)?std::min(sampled.description.MipLevels-1,sampling.maxLevel):0;
        SamplerKey key{sampling.minFilter,sampling.magFilter,sampling.wrapS,sampling.wrapT,maxLod};
        auto found=samplers.find(key);if(found!=samplers.end())return found->second;
        const bool minLinear=sampling.minFilter==GL_LINEAR||sampling.minFilter==GL_LINEAR_MIPMAP_NEAREST||sampling.minFilter==GL_LINEAR_MIPMAP_LINEAR;
        const bool mipLinear=sampling.minFilter==GL_NEAREST_MIPMAP_LINEAR||sampling.minFilter==GL_LINEAR_MIPMAP_LINEAR;
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_ENCODE_BASIC_FILTER(minLinear?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,
            sampling.magFilter==GL_LINEAR?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,mipLinear?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,0);
        desc.AddressU=addressMode(sampling.wrapS);desc.AddressV=addressMode(sampling.wrapT);desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxAnisotropy=1;desc.ComparisonFunc=D3D11_COMPARISON_NEVER;desc.MaxLOD=float(maxLod);
        ComPtr<ID3D11SamplerState> result;checkResult(device->CreateSamplerState(&desc,&result.get()),"sampler state");return samplers.emplace(key,std::move(result)).first->second;
    }
    ID3D11PixelShader* pixelShader(const Draw& draw) {
        unsigned passFlags=0;
        if(specialize&&!draw.bitmap) {
            if(draw.uniforms.family[0]!=0)passFlags|=Dx11FamilyEnabled;
            if(draw.uniforms.family[0]>1)passFlags|=Dx11FamilyOther;
            if(draw.uniforms.family[1]!=0||draw.uniforms.family[0]>1)passFlags|=Dx11RawMetadata;
            if(draw.uniforms.peel[0]!=0)passFlags|=Dx11PeelEnabled;
            if(draw.uniforms.peel[1]==0)passFlags|=Dx11PeelAfterFirst;
            if(draw.uniforms.blend[0]!=0)passFlags|=Dx11WeighByAlpha;
            if(draw.uniforms.blend[1]!=0)passFlags|=Dx11ForceCoverage;
            auto sameDepth=[](float placement,float sorting){return !(sorting>0&&sorting!=placement);};
            const bool commonDepth=draw.palette.empty()?sameDepth(draw.uniforms.depth[1],draw.uniforms.material[3]):
                std::all_of(draw.palette.begin(),draw.palette.end(),[&](const DrawRecord& record){return sameDepth(record.modes[0],record.modes[1]);});
            if(commonDepth)passFlags|=Dx11CommonDepth;
            else {
                const bool separateDepth=draw.palette.empty()||std::all_of(draw.palette.begin(),draw.palette.end(),[&](const DrawRecord& record){return !sameDepth(record.modes[0],record.modes[1]);});
                if(separateDepth)passFlags|=Dx11SeparateDepth;
            }
        }
        const PixelKey key{draw.bitmap,draw.familyResolve,!draw.palette.empty(),draw.peelComposite,meshShaderSpecialization(draw,specialize),passFlags};
        auto found=pixelShaders.find(key);
        // Cache deferred keys too: a pending compiler must not make every
        // sorted triangle rebuild and hash the same HLSL source each frame.
        if(found!=pixelShaders.end()&&(!found->second.deferred||pendingPixel)) {
            found->second.touched=++shaderClock;return found->second.shader;
        }
        const auto source=dx11ShaderSource(key.bitmap,key.family,key.palette,key.constants,false,key.passFlags,key.peelComposite);
        ComPtr<ID3DBlob> code;const u64 hash=hashShader(source.c_str(),"pixelMain","ps_5_0");
        if(!lookupShader(hash,code)) {
            if(backgroundCompilation&&!draw.bitmap) {
                // One outstanding job bounds CPU/memory use. All immediate-
                // context work and bytecode-cache access stay on this thread.
                // The worker owns its source/device until compilation finishes.
                if(!pendingPixel) {
                    try {
                        auto result=std::async(std::launch::async,[dxDevice=device,dxCompiler=compiler,source] {
                            // MSVC may reuse an async pool thread. Restore its
                            // priority on success and on compiler/device failure.
                            struct PriorityRestore {
                                int previous=GetThreadPriority(GetCurrentThread());
                                ~PriorityRestore(){SetThreadPriority(GetCurrentThread(),previous);}
                            } priority;
                            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
                            CompiledPixel compiled;compiled.code=compileCode(dxCompiler,source,"pixelMain","ps_5_0");
                            checkResult(dxDevice->CreatePixelShader(compiled.code->GetBufferPointer(),compiled.code->GetBufferSize(),
                                nullptr,&compiled.shader.get()),"background pixel shader");
                            return compiled;
                        });
                        pendingPixel.emplace(PendingPixel{key,hash,std::move(result)});
                    } catch(const std::system_error& error) {
                        WARN_LOG(RENDERER,"Hikaru DX11 background compiler unavailable: %s",error.what());
                        backgroundCompilation=false; // Retain the synchronous path if no worker can start.
                    }
                }
                if(backgroundCompilation)return retainPixel(key,genericPixel(key),true);
            }
            code=compile(source,"pixelMain","ps_5_0");
        }
        ComPtr<ID3D11PixelShader> result;checkResult(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&result.get()),"pixel shader");
        return retainPixel(key,std::move(result));
    }
    void bindTargets(const Draw& draw) {
        auto& color=image(draw.color);auto* depth=draw.depth?&image(draw.depth):nullptr;
        auto* metadata=draw.metadata?&image(draw.metadata):nullptr;
        if((depth&&(depth->description.Width!=color.description.Width||depth->description.Height!=color.description.Height))||
            (metadata&&(metadata->description.Width!=color.description.Width||metadata->description.Height!=color.description.Height||metadata->description.Format!=DXGI_FORMAT_R32G32_UINT)))
            throw FlycastException("Hikaru DX11 attachment size/format mismatch");
        std::array<ID3D11RenderTargetView*,2> targets{color.target,metadata?static_cast<ID3D11RenderTargetView*>(metadata->target):nullptr};
        ID3D11DepthStencilView* depthView=depth?static_cast<ID3D11DepthStencilView*>(depth->depthView):nullptr;
        if(bindingsDirty||targets!=boundTargets||depthView!=boundDepth) {
            // Never leave a prior input bound when its resource becomes output.
            unbindInputs();context->OMSetRenderTargets(metadata?2:1,targets.data(),depthView);
            boundTargets=targets;boundDepth=depthView;
        }
    }
    void bindStates(const Draw& draw) {
        auto* blend=blendState(draw);auto* depth=depthState(draw);auto* raster=rasterState(draw);
        if(bindingsDirty||blend!=boundBlend){context->OMSetBlendState(blend,nullptr,UINT_MAX);boundBlend=blend;}
        if(bindingsDirty||depth!=boundDepthState){context->OMSetDepthStencilState(depth,0);boundDepthState=depth;}
        if(bindingsDirty||raster!=boundRaster){context->RSSetState(raster);boundRaster=raster;}
        auto& target=image(draw.color);const auto height=target.description.Height;
        const float y=draw.color==0?float(height)-draw.viewport[1]-draw.viewport[3]:float(draw.viewport[1]);
        const D3D11_VIEWPORT viewport{float(draw.viewport[0]),y,float(draw.viewport[2]),float(draw.viewport[3]),draw.nearDepth,draw.farDepth};
        if(bindingsDirty||std::memcmp(&viewport,&boundViewport,sizeof(viewport))!=0){context->RSSetViewports(1,&viewport);boundViewport=viewport;}
        if(draw.scissorEnabled) {
            const int top=draw.color==0?int(height)-draw.scissor[1]-draw.scissor[3]:draw.scissor[1];
            const D3D11_RECT rect{draw.scissor[0],top,draw.scissor[0]+std::max(0,draw.scissor[2]),top+std::max(0,draw.scissor[3])};
            if(bindingsDirty||std::memcmp(&rect,&boundScissor,sizeof(rect))!=0){context->RSSetScissorRects(1,&rect);boundScissor=rect;}
        }
    }
    Image& sampledImage(unsigned id,const Texture& sampling,const Draw& draw,bool integer) {
        auto found=images.find(id);Image* sampled=found!=images.end()?&found->second:nullptr;
        if(!id||id==draw.color||id==draw.depth||id==draw.metadata||!sampled||!sampled->sampleable||!sampled->initialized[0])sampled=nullptr;
        if(sampled&&isMipmapped(sampling.minFilter)) {
            const unsigned levels=1+std::min(sampling.maxLevel,mipCount(sampled->description.Width,sampled->description.Height)-1);
            if(levels>sampled->initialized.size())sampled=nullptr;
            else for(unsigned level=0;sampled&&level<levels;++level)if(!sampled->initialized[level])sampled=nullptr;
        }
        if(sampled&&(sampled->description.Format==DXGI_FORMAT_R32G32_UINT)!=integer)sampled=nullptr;
        return sampled?*sampled:image(integer?MetadataFallback:ColorFallback);
    }
    void endQuerySegment() {if(queryOpen){context->End(queries.back());queryOpen=false;}}
public:
    Dx11Backend(ID3D11Device* dxDevice,ID3D11DeviceContext* dxContext,pD3DCompile dxCompiler,DX11Context* dxOwner,bool optimized,bool background=false)
        :compiler(dxCompiler),owner(dxOwner),specialize(optimized),backgroundCompilation(background&&optimized) {
        if(!dxDevice||!dxContext||!compiler||dxDevice->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)
            throw FlycastException("Hikaru DX11 requires Direct3D feature level 11.0");
        device.reset(dxDevice);dxDevice->AddRef();context.reset(dxContext);dxContext->AddRef();
        // Reuse the existing bytecode cache so renderer switches and later
        // runs do not synchronously recompile each material on first draw.
        // Offscreen test devices never read or write the user's cache.
        enableCache(owner!=nullptr);if(owner)loadCache(CacheFile);
        // Offset binding and NO_OVERWRITE are independent optional features.
        // Append into unused 256-byte-aligned windows; DISCARD only on wrap.
        // Older devices retain the original per-update buffer-renaming path.
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        if(specialize&&SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&
            options.ConstantBufferOffsetting&&options.MapNoOverwriteOnDynamicConstantBuffer)
            context.as(context1);
        const size_t constantBytes=context1?ConstantStream:sizeof(Uniforms);
        vertexUniformBuffer=buffer(constantBytes,D3D11_BIND_CONSTANT_BUFFER);
        pixelUniformBuffer=buffer(constantBytes,D3D11_BIND_CONSTANT_BUFFER);paletteBuffer=buffer(sizeof(DrawRecord)*DrawPaletteSize,D3D11_BIND_CONSTANT_BUFFER);
        if(owner)INFO_LOG(RENDERER,"Hikaru DX11 constant streaming: %s",context1?"enabled":"fallback");
        const char* names[]={"POSITION","NORMAL","COLOR","COLOR","COLOR","COLOR","TEXCOORD","TEXCOORD","TEXCOORD","TEXCOORD"};
        const UINT indices[]={0,0,0,1,2,3,0,1,2,3};
        const DXGI_FORMAT formats[]={DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R32G32B32_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R32_FLOAT};
        const UINT offsets[]={offsetof(PackedVertex,position),offsetof(PackedVertex,normal),offsetof(PackedVertex,diffuse),
            offsetof(PackedVertex,ambient),offsetof(PackedVertex,specular),offsetof(PackedVertex,unknown),offsetof(PackedVertex,uv),
            offsetof(PackedVertex,alpha),offsetof(PackedVertex,matrixSelect),offsetof(PackedVertex,drawIndex)};
        for(unsigned kind=0;kind<6;++kind) {
            const bool packed=kind>=3;const unsigned variant=kind%3;
            D3D11_INPUT_ELEMENT_DESC layout[10]{};
            for(UINT i=0;i<10;++i)layout[i]={names[i],indices[i],packed?formats[i]:i<7?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R32_FLOAT,
                0,packed?offsets[i]:i<7?i*16:112+(i-7)*4,D3D11_INPUT_PER_VERTEX_DATA,0};
            auto code=compile(dx11ShaderSource(variant==2,false,variant==1,{},packed),"vertexMain","vs_5_0");
            checkResult(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertexShaders[kind].get()),"vertex shader");
            // Match each compiled signature, including declared attributes
            // the bitmap shader does not read. FXC retains those semantics.
            checkResult(device->CreateInputLayout(layout,10,code->GetBufferPointer(),
                code->GetBufferSize(),&inputLayouts[kind].get()),"vertex layout");
        }
        const std::string clearSource=R"(
cbuffer ClearConstants:register(b0){float4 color;float4 depth;};
float4 clearVertex(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}
struct ClearResult{float4 color:SV_Target0;float depth:SV_Depth;};
ClearResult clearPixel(){ClearResult result;result.color=color;result.depth=depth.x;return result;}
struct IntegerResult{uint2 color:SV_Target0;float depth:SV_Depth;};
IntegerResult clearInteger(){IntegerResult result;result.color=uint2(color.xy);result.depth=depth.x;return result;}
)";
        auto code=compile(clearSource,"clearVertex","vs_5_0");checkResult(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&clearVertex.get()),"clear vertex shader");
        code=compile(clearSource,"clearPixel","ps_5_0");checkResult(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&clearPixel.get()),"clear pixel shader");
        code=compile(clearSource,"clearInteger","ps_5_0");checkResult(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&clearIntegerPixel.get()),"integer clear shader");
        Texture fallback;fallback.width=fallback.height=1;fallback.maxLevel=0;
        const uint8_t transparent[4]={};texture(ColorFallback,fallback,0,TextureUpload::Definition,transparent,sizeof(transparent));
        fallback.format=GL_RG32UI;const uint32_t zero[2]={};texture(MetadataFallback,fallback,0,TextureUpload::Definition,zero,sizeof(zero));
        if(backgroundCompilation) {
            // Keep scalar/palette transforms and single/indexed sampling exact
            // while material/light/pass decisions use the original dynamic equations.
            for(unsigned variant=0;variant<genericPixels.size();++variant) {
                MeshShaderSpecialization dynamic{};dynamic[8]=variant&1;
                auto generic=compile(dx11ShaderSource(false,false,variant>=2,dynamic),"pixelMain","ps_5_0");
                checkResult(device->CreatePixelShader(generic->GetBufferPointer(),generic->GetBufferSize(),nullptr,
                    &genericPixels[variant].get()),"generic pixel shader");
            }
            // Fixed composition programs are cheap and need no late compiler
            // call while the single specialization worker is active.
            compile(dx11ShaderSource(true,false,false,{}),"pixelMain","ps_5_0");
            compile(dx11ShaderSource(true,true,false,{}),"pixelMain","ps_5_0");
        }
    }
    ~Dx11Backend() override {
        // Finish before the owner unloads its compiler/device. The worker never
        // borrows this backend or its context, so there is no shutdown lock cycle.
        try {completePixelCompilation(true);}catch(const std::exception& error){WARN_LOG(RENDERER,"Hikaru DX11 shader shutdown: %s",error.what());}
        endQuerySegment();
        if(owner)saveCache(CacheFile);
        if(owner&&GraphicsContext::Instance()==owner)owner->presentFrame({});
        // The immediate context owns submitted resource references. Releasing
        // CPU cache entries never requires a device-wide GPU idle wait.
        unbindInputs();unbindTargets();
    }
#ifdef HIKARU_NATIVE_TESTING
    void completeCompilationForTesting() {completePixelCompilation(true);}
#endif
    bool supportsDrawPalette() const override {return true;}
    bool supportsTexturePalette() const override {return true;}
    bool supportsPackedVertices() const override {return true;}
    void texture(unsigned id,const Texture& info,unsigned level,TextureUpload uploadType,const void* pixels,size_t bytes) override {
        const bool update=uploadType==TextureUpload::FullUpdate;
        if(!info.width||!info.height||level>=mipCount(info.width,info.height))throw FlycastException("Invalid Hikaru DX11 mip definition");
        auto found=images.find(id);
        const unsigned levels=1+std::min(info.maxLevel,mipCount(info.width,info.height)-1);
        const bool depth=(id&0x80000000u)||info.format==GL_DEPTH_COMPONENT||info.format==GL_DEPTH_COMPONENT16||info.format==GL_DEPTH_COMPONENT24||info.format==GL_DEPTH_COMPONENT32F||info.format==GL_DEPTH24_STENCIL8;
        const DXGI_FORMAT expected=depth?(info.format==GL_DEPTH_COMPONENT16?DXGI_FORMAT_R16_TYPELESS:info.format==GL_DEPTH_COMPONENT24||info.format==GL_DEPTH24_STENCIL8?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R32_TYPELESS):
            info.format==GL_RGB10_A2?DXGI_FORMAT_R10G10B10A2_UNORM:info.format==GL_RG32UI?DXGI_FORMAT_R32G32_UINT:DXGI_FORMAT_R8G8B8A8_UNORM;
        const bool compatible=found!=images.end()&&found->second.description.Width==info.width&&found->second.description.Height==info.height&&found->second.description.Format==expected&&found->second.description.MipLevels==(depth?1:levels);
        const size_t pixelBytes=info.format==GL_RG32UI?8:4;
        const unsigned width=std::max(1u,info.width>>level),height=std::max(1u,info.height>>level);
        if(pixels&&bytes!=size_t(width)*height*pixelBytes)throw FlycastException("Invalid Hikaru DX11 texture byte count");
        if(update&&(!compatible||level||!pixels||depth))throw FlycastException("Hikaru DX11 full update requires compatible level-zero color storage");
        unbindInputs();unbindTargets();
        if(!compatible){images.erase(id);images.emplace(id,makeImage(id,info));}
        auto& target=image(id);if(level>=target.description.MipLevels)throw FlycastException("Hikaru DX11 mip exceeds texture storage");
        if(pixels){if(depth)throw FlycastException("Hikaru DX11 CPU depth uploads are unsupported");context->UpdateSubresource(target.texture,level,nullptr,pixels,width*UINT(pixelBytes),0);}
        target.initialized[level]=true;
    }
    void eraseTexture(unsigned id) override {
        unbindInputs();unbindTargets();if(id==0&&owner&&GraphicsContext::Instance()==owner)owner->presentFrame({});images.erase(id);
    }
    void clear(const Draw& draw,GLbitfield mask,const Vec4& color,float depth) override {
        endQuerySegment();
        auto& target=image(draw.color);const bool colors=(mask&GL_COLOR_BUFFER_BIT)!=0;
        const bool z=(mask&GL_DEPTH_BUFFER_BIT)&&draw.depth&&draw.depthWrite;
        if(!colors&&!z)return;
        unbindInputs();unbindTargets();
        const bool allChannels=std::all_of(draw.colorMask.begin(),draw.colorMask.end(),[](bool enabled){return enabled;});
        if(!draw.scissorEnabled&&(!colors||allChannels)) {
            if(colors)context->ClearRenderTargetView(target.target,color.data());
            if(z)context->ClearDepthStencilView(image(draw.depth).depthView,D3D11_CLEAR_DEPTH,depth,0);
            return;
        }
        Draw state=draw;state.metadata=0;state.blend=false;state.cull=false;state.biasFactor=state.biasUnits=0;
        state.depthTest=z;state.depthWrite=z;state.depthFunc=GL_ALWAYS;state.nearDepth=0;state.farDepth=1;
        state.viewport={0,0,GLint(target.description.Width),GLint(target.description.Height)};
        if(!colors)state.colorMask={false,false,false,false};
        const std::array<Vec4,2> constants{color,Vec4{depth,0,0,0}};upload(pixelUniformBuffer,constants.data(),sizeof(constants));
        pixelUniformsValid=false;pixelConstantsCursor=0; // Clear DISCARD invalidates the pixel stream only.
        bindTargets(state);bindStates(state);
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(clearVertex,nullptr,0);context->PSSetShader(target.description.Format==DXGI_FORMAT_R32G32_UINT?clearIntegerPixel:clearPixel,nullptr,0);
        context->PSSetConstantBuffers(0,1,&pixelUniformBuffer.get());context->Draw(3,0);bindingsDirty=true;
    }
    void draw(const Draw& draw) override {
        const bool packed=!draw.packedVertices.empty();
        const size_t count=packed?draw.packedVertices.size():draw.vertices.size();
        const UINT stride=packed?sizeof(PackedVertex):sizeof(Vertex);
        if(!count||(draw.cull&&draw.cullFace==GL_FRONT_AND_BACK))return;
        if((packed&&(!draw.vertices.empty()||draw.bitmap))||count>UINT_MAX/stride||draw.palette.size()>DrawPaletteSize||
            (draw.texturePalette&&(draw.paletteTextures.size()!=draw.palette.size()||draw.paletteSampling.size()!=draw.palette.size())))
            throw FlycastException("Invalid Hikaru DX11 draw size");
        const size_t bytes=count*stride;
        if(vertexCapacity<bytes){vertexCapacity=std::max(size_t(1024*1024),bytes);vertexBuffer=buffer(vertexCapacity,D3D11_BIND_VERTEX_BUFFER);vertexOffset=0;}
        // Scalar, packed and bitmap draws share a stream. Align to the
        // current stride so Draw's first-vertex offset never truncates bytes.
        vertexOffset=(vertexOffset+stride-1)/stride*stride;
        if(vertexOffset+bytes>vertexCapacity)vertexOffset=0;
        const void* vertices=packed?static_cast<const void*>(draw.packedVertices.data()):draw.vertices.data();
        upload(vertexBuffer,vertices,bytes,vertexOffset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,vertexOffset);
        // The VS reads the five matrices and clip-Y inversion; the PS reads
        // the material/light tail. Preserve the HLSL layout in both buffers,
        // but do not rename/re-upload lighting for each new model transform.
        constexpr size_t materialOffset=offsetof(Uniforms,ambient),inversionOffset=offsetof(Uniforms,inverted);
        Uniforms uniforms=draw.uniforms;
        // Owner order affects only the integer MRT output. Without that
        // attachment its varying value must not invalidate identical lighting.
        if(!draw.metadata)uniforms.family[2]=0;
        const auto* data=reinterpret_cast<const uint8_t*>(&uniforms);
        const auto* previous=reinterpret_cast<const uint8_t*>(&uploadedUniforms);
        const bool vertexChanged=!vertexUniformsValid||std::memcmp(data,previous,materialOffset)!=0||uniforms.inverted!=uploadedUniforms.inverted;
        const bool pixelChanged=!pixelUniformsValid||std::memcmp(data+materialOffset,previous+materialOffset,sizeof(Uniforms)-materialOffset)!=0;
        if(vertexChanged) {
            vertexConstantsOffset=constantOffset(vertexConstantsCursor);
            const auto mode=vertexConstantsOffset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD;
            D3D11_MAPPED_SUBRESOURCE mapped{};checkResult(context->Map(vertexUniformBuffer,0,mode,0,&mapped),"vertex constants map");
            auto* target=static_cast<uint8_t*>(mapped.pData)+vertexConstantsOffset;
            std::memcpy(target,data,materialOffset);
            std::memcpy(target+inversionOffset,&uniforms.inverted,sizeof(uniforms.inverted));
            context->Unmap(vertexUniformBuffer,0);vertexUniformsValid=true;
        }
        if(pixelChanged) {
            pixelConstantsOffset=constantOffset(pixelConstantsCursor);
            const auto mode=pixelConstantsOffset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD;
            upload(pixelUniformBuffer,data+materialOffset,sizeof(Uniforms)-materialOffset,mode,pixelConstantsOffset+materialOffset);
            pixelUniformsValid=true;
        }
        uploadedUniforms=uniforms;
        if(!draw.palette.empty()) {
            for(size_t i=0;i<count;++i) {
                const float index=packed?draw.packedVertices[i].drawIndex:draw.vertices[i].extra[2];
                if(!std::isfinite(index)||index<0||index>=draw.palette.size()||std::floor(index)!=index)
                    throw FlycastException("Invalid Hikaru DX11 draw palette index");
            }
            if(uploadedPaletteSize!=draw.palette.size()||std::memcmp(uploadedPalette.data(),draw.palette.data(),draw.palette.size()*sizeof(DrawRecord))!=0) {
                upload(paletteBuffer,draw.palette.data(),draw.palette.size()*sizeof(DrawRecord));
                std::copy(draw.palette.begin(),draw.palette.end(),uploadedPalette.begin());uploadedPaletteSize=draw.palette.size();
            }
        }
        bindTargets(draw);bindStates(draw);
        const unsigned vertexKind=(packed?3:0)+(draw.bitmap?2:draw.palette.empty()?0:1);
        auto* vertex=static_cast<ID3D11VertexShader*>(vertexShaders[vertexKind]);auto* pixel=pixelShader(draw);
        if(bindingsDirty||vertex!=boundVertex){context->VSSetShader(vertex,nullptr,0);boundVertex=vertex;}
        if(bindingsDirty||pixel!=boundPixel){context->PSSetShader(pixel,nullptr,0);boundPixel=pixel;}
        if(bindingsDirty){context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);context->CSSetShader(nullptr,nullptr,0);}
        if(context1) {
            const UINT count=ConstantWindow/16,vertexFirst=vertexConstantsOffset/16,pixelFirst=pixelConstantsOffset/16;
            if(bindingsDirty||vertexChanged)context1->VSSetConstantBuffers1(0,1,&vertexUniformBuffer.get(),&vertexFirst,&count);
            if(bindingsDirty||pixelChanged)context1->PSSetConstantBuffers1(0,1,&pixelUniformBuffer.get(),&pixelFirst,&count);
            if(bindingsDirty)context->VSSetConstantBuffers(1,1,&paletteBuffer.get());
        } else if(bindingsDirty) {
            ID3D11Buffer* constants[]={vertexUniformBuffer,paletteBuffer};context->VSSetConstantBuffers(0,2,constants);context->PSSetConstantBuffers(0,1,&pixelUniformBuffer.get());
        }
        auto* layout=static_cast<ID3D11InputLayout*>(inputLayouts[vertexKind]);
        if(bindingsDirty||layout!=boundLayout){context->IASetInputLayout(layout);boundLayout=layout;}
        if(bindingsDirty||vertexBuffer!=boundVertexBuffer||stride!=boundVertexStride) {
            const UINT offset=0;context->IASetVertexBuffers(0,1,&vertexBuffer.get(),&stride,&offset);boundVertexBuffer=vertexBuffer;boundVertexStride=stride;
        }
        D3D11_PRIMITIVE_TOPOLOGY topology=draw.topology==GL_TRIANGLES?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:draw.topology==GL_TRIANGLE_STRIP?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            draw.topology==GL_LINES?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        if(bindingsDirty||topology!=boundTopology){context->IASetPrimitiveTopology(topology);boundTopology=topology;}
        std::array<ID3D11ShaderResourceView*,19> views{};std::array<ID3D11SamplerState*,16> sampling{};
        const unsigned baseSlots=draw.texturePalette?DrawPaletteSize:1;
        for(unsigned i=0;i<baseSlots;++i) {
            const bool palette=draw.texturePalette&&i<draw.palette.size();
            const auto& state=palette?draw.paletteSampling[i]:draw.sampling[0];
            const unsigned id=palette?draw.paletteTextures[i]:i?0:draw.textures[0];
            auto& input=sampledImage(id,state,draw,false);views[i]=input.view;sampling[i]=sampler(state,input);
        }
        for(unsigned i=1;i<4;++i) {
            const bool integer=draw.familyResolve?i>=2:draw.peelComposite?i==2:!draw.bitmap&&i==3;
            auto& input=sampledImage(draw.textures[i],draw.sampling[i],draw,integer);views[15+i]=input.view;
            if(draw.bitmap&&!draw.familyResolve)sampling[i]=sampler(draw.sampling[i],input);
        }
        if(bindingsDirty||views!=boundInputs){context->PSSetShaderResources(0,UINT(views.size()),views.data());boundInputs=views;}
        if(bindingsDirty||sampling!=boundSamplers){context->PSSetSamplers(0,UINT(sampling.size()),sampling.data());boundSamplers=sampling;}
        if(occlusionActive&&!queryOpen) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};ComPtr<ID3D11Query> query;
            checkResult(device->CreateQuery(&desc,&query.get()),"occlusion query");queries.push_back(std::move(query));context->Begin(queries.back());queryOpen=true;
        }
        context->Draw(UINT(count),UINT(vertexOffset/stride));vertexOffset+=bytes;bindingsDirty=false;
    }
    void readPixels(unsigned id,int x,int y,int width,int height,uint8_t* rgba) override {
        if(width<=0||height<=0)return;
        auto& source=image(id);
        if(source.depth||source.description.Format==DXGI_FORMAT_R32G32_UINT||x<0||y<0||uint64_t(x)+width>source.description.Width||uint64_t(y)+height>source.description.Height)
            throw FlycastException("Invalid Hikaru DX11 color readback");
        D3D11_TEXTURE2D_DESC desc=source.description;desc.Width=UINT(width);desc.Height=UINT(height);desc.MipLevels=1;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;checkResult(device->CreateTexture2D(&desc,nullptr,&staging.get()),"readback allocation");
        const UINT top=id==0?source.description.Height-y-height:y;
        const D3D11_BOX box{UINT(x),top,0,UINT(x+width),top+UINT(height),1};context->CopySubresourceRegion(staging,0,0,0,0,source.texture,0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};checkResult(context->Map(staging,0,D3D11_MAP_READ,0,&mapped),"readback map");
        for(int row=0;row<height;++row) {
            const auto* input=static_cast<const uint8_t*>(mapped.pData)+size_t(id==0?height-1-row:row)*mapped.RowPitch;
            auto* output=rgba+size_t(row)*width*4;
            if(desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM)for(int col=0;col<width;++col) {
                uint32_t value;std::memcpy(&value,input+col*4,4);
                output[col*4]=uint8_t(((value&1023)*255+511)/1023);output[col*4+1]=uint8_t((((value>>10)&1023)*255+511)/1023);
                output[col*4+2]=uint8_t((((value>>20)&1023)*255+511)/1023);output[col*4+3]=uint8_t(((value>>30)*255+1)/3);
            } else std::memcpy(output,input,size_t(width)*4);
        }
        context->Unmap(staging,0);
    }
    void beginOcclusion() override {
        if(occlusionActive)throw FlycastException("Nested Hikaru DX11 visibility query");
        queries.clear();occlusionActive=true;
    }
    void finishOcclusion() override {
        if(!occlusionActive)throw FlycastException("No active Hikaru DX11 visibility query");
        endQuerySegment();occlusionActive=false;
        pendingOcclusions.push_back(std::move(queries));queries.clear();
    }
    bool occlusionResult() override {
        if(pendingOcclusions.empty())throw FlycastException("No pending Hikaru DX11 visibility query");
        auto pending=std::move(pendingOcclusions.front());pendingOcclusions.pop_front();
        bool visible=false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(auto& query:pending) {
            UINT64 samples=0;HRESULT result;
            while((result=context->GetData(query,&samples,sizeof(samples),0))==S_FALSE) {
                if(std::chrono::steady_clock::now()>deadline)throw FlycastException("Hikaru DX11 visibility query timed out");
                std::this_thread::yield();
            }
            checkResult(result,"visibility query result");visible|=samples!=0;
        }
        return visible;
    }
    bool endOcclusion() override { finishOcclusion(); return occlusionResult(); }
    void present(unsigned width,unsigned height) override {
        completePixelCompilation(false);
        auto& output=image(0);if(output.description.Width!=width||output.description.Height!=height)return;
        if(!owner)throw FlycastException("Hikaru DX11 presenter is unavailable");
        unbindInputs();unbindTargets();owner->presentFrame(output.view);bindingsDirty=true;
    }
};
}
std::unique_ptr<Backend> makeDx11Backend() {
    auto* owner=dynamic_cast<DX11Context*>(GraphicsContext::Instance());
    if(!owner)throw FlycastException("Hikaru DX11 requires an active DX11 context");
    return std::make_unique<Dx11Backend>(owner->getDevice(),owner->getDeviceContext(),owner->getCompiler(),owner,true,true);
}
#ifdef HIKARU_NATIVE_TESTING
std::unique_ptr<Backend> makeDx11BackendForTesting(ID3D11Device* device,ID3D11DeviceContext* context,pD3DCompile compiler,bool specialize) {
    return std::make_unique<Dx11Backend>(device,context,compiler,nullptr,specialize);
}
std::unique_ptr<Backend> makeBackgroundDx11BackendForTesting(ID3D11Device* device,ID3D11DeviceContext* context,pD3DCompile compiler) {
    return std::make_unique<Dx11Backend>(device,context,compiler,nullptr,true,true);
}
void completeDx11CompilationForTesting(Backend& backend) {static_cast<Dx11Backend&>(backend).completeCompilationForTesting();}
#endif
} // namespace hikaru::graphics
#endif
