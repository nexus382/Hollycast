// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include "shaders.h"
#include "fragment_list_shared.h"

#if defined(HAVE_D3D11) && !defined(LIBRETRO)
#include "rend/dx11/dx11context.h"
#include "rend/dx11/dx11_shaders.h"
#include "dx11_diagnostics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
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
bool traceFlagEnabled(const char* name) {
    const char* value=std::getenv(name);
    return value&&value[0]!='\0'&&std::strcmp(value,"0")!=0;
}
using DiagnosticClock=std::chrono::steady_clock;
uint64_t elapsedNanoseconds(DiagnosticClock::time_point start) {
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(DiagnosticClock::now()-start).count();
    return elapsed>0?uint64_t(elapsed):0;
}
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
        bool fragmentList=false;
        DepthPass depthPass=DepthPass::None;
        bool operator<(const PixelKey& other) const {return std::tie(bitmap,family,palette,peelComposite,constants,passFlags,fragmentList,depthPass)<std::tie(other.bitmap,other.family,other.palette,other.peelComposite,other.constants,other.passFlags,other.fragmentList,other.depthPass);}
    };
    struct FragmentBuffer {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11UnorderedAccessView> view;
    };
    struct FragmentStorage {
        FragmentBuffer heads,status;
        std::array<FragmentBuffer,4> nodes;
        ComPtr<ID3D11Buffer> readback;
        Image background;
        uint32_t capacity=0;
    };
    // HLSL cbuffers round the shared 28-byte parameter ABI to 16-byte rows.
    struct FragmentConstants {
        HikaruFragmentListParameters parameters;
        uint32_t padding=0;
    };
    static_assert(sizeof(FragmentConstants)==32,"Hikaru DX11 list constant layout changed");
    struct PixelShader {ComPtr<ID3D11PixelShader> shader;uint64_t touched=0;bool deferred=false;};
    struct CompiledPixel {
        ComPtr<ID3DBlob> code;
        ComPtr<ID3D11PixelShader> shader;
        std::exception_ptr error;
        uint64_t compileNanoseconds=0;
        bool compileAttempted=false;
    };
    struct PendingPixel {PixelKey key;u64 hash;std::future<CompiledPixel> result;};
    struct CostCounters {
        uint64_t draws=0,vertexUploads=0,vertexBytes=0;
        uint64_t uniformUploads=0,uniformHits=0,uniformBytes=0;
        uint64_t paletteUploads=0,paletteHits=0,paletteBytes=0;
        uint64_t shaderCacheLookups=0,shaderCacheHits=0,shaderCompiles=0,shaderCompileNanoseconds=0;
        uint64_t textureUploads=0,textureBytes=0,queriesCreated=0,gpuQueryObjects=0,queryWaitNanoseconds=0;
        uint64_t fragmentCounterWaitNanoseconds=0,maskedClears=0,maskedPixels=0;
    };
    // A slot cannot be reused until its disjoint query and all recorded timestamps complete.
    struct GpuFrame {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>,10> timestamps;
        unsigned recordedTimestamps=0,phaseMask=0;
        bool open=false,inFlight=false,valid=true,listClaimed=false,listRecording=false;
    };
    struct Diagnostics {
        Diagnostics(bool isEnabled,bool requestsGpu):enabled(isEnabled),gpuRequested(requestsGpu) {}
        const bool enabled,gpuRequested;
        bool gpuSupported=false,gpuFailureReported=false;
        CostCounters costs;
        std::array<GpuFrame,4> frames{};
        std::array<double,5> gpuMilliseconds{};
        std::array<uint64_t,5> gpuSamples{};
        unsigned activeFrame=4,nextFrame=0;
        bool frameAttempted=false;
        uint64_t gpuDropped=0,gpuInvalid=0,presentedFrames=0;
    };
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
    std::unique_ptr<FragmentStorage> fragmentLists;
    HikaruFragmentListParameters fragmentParameters{};
    HikaruFragmentPoolPolicy fragmentPoolPolicy{};
    unsigned fragmentPoolWidth=0,fragmentPoolHeight=0,fragmentColor=0;
    DXGI_FORMAT fragmentPoolFormat=DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Buffer> fragmentConstants;
    std::array<ComPtr<ID3D11PixelShader>,2> fragmentGenericPixels,fragmentResolvePixels;
    size_t fragmentConstantsCursor=0;
    bool fragmentListSupported=true,fragmentCollectActive=false,fragmentListFailed=false,fragmentSmallList=true;
    bool fragmentOverflowWarned=false;
    const bool traceTiming=traceFlagEnabled("HIKARU_TRACE_TIMING");
    const bool traceGpuTiming=traceFlagEnabled("HIKARU_TRACE_GPU_TIMING");
    const bool fragmentTrace=traceTiming;
    Diagnostics diagnostics{traceTiming||traceGpuTiming,traceGpuTiming};
    std::array<uint64_t,3> fragmentCounts{};
    uint64_t fragmentAllocations=0,fragmentResolves=0;
    std::unordered_map<unsigned,Image> images;
    std::map<PixelKey,PixelShader> pixelShaders;
    std::map<BlendKey,ComPtr<ID3D11BlendState>> blends;
    std::map<DepthKey,ComPtr<ID3D11DepthStencilState>> depths;
    std::map<RasterKey,ComPtr<ID3D11RasterizerState>> rasters;
    std::map<SamplerKey,ComPtr<ID3D11SamplerState>> samplers;
    // These maps own immutable states until backend destruction. Insertions
    // preserve iterators, including across clears and context rebindings.
    decltype(blends)::iterator lastBlend=blends.end();
    decltype(depths)::iterator lastDepth=depths.end();
    decltype(rasters)::iterator lastRaster=rasters.end();
    decltype(samplers)::iterator lastSampler=samplers.end();
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

    void recordShaderCompile(DiagnosticClock::time_point started) {
        if(diagnostics.enabled) {
            ++diagnostics.costs.shaderCompiles;
            diagnostics.costs.shaderCompileNanoseconds+=elapsedNanoseconds(started);
        }
    }
    ComPtr<ID3DBlob> compile(const std::string& source,const char* entry,const char* profile) {
        ComPtr<ID3DBlob> code;const u64 hash=hashShader(source.c_str(),entry,profile);
        const bool cached=lookupShader(hash,code);
        if(diagnostics.enabled) {
            ++diagnostics.costs.shaderCacheLookups;
            diagnostics.costs.shaderCacheHits+=cached;
        }
        if(cached)return code;
        if(!diagnostics.enabled)code=compileCode(compiler,source,entry,profile);
        else {
            const auto started=DiagnosticClock::now();
            try {code=compileCode(compiler,source,entry,profile);}
            catch(...) {recordShaderCompile(started);throw;}
            recordShaderCompile(started);
        }
        cacheShader(hash,code);return code;
    }
    const ComPtr<ID3D11PixelShader>& genericPixel(const PixelKey& key) const {
        if(key.fragmentList)return fragmentGenericPixels[key.constants[8]?1:0];
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
            auto compiled=pending.result.get();
            if(diagnostics.enabled&&compiled.compileAttempted) {
                ++diagnostics.costs.shaderCompiles;
                diagnostics.costs.shaderCompileNanoseconds+=compiled.compileNanoseconds;
            }
            if(compiled.error)std::rethrow_exception(compiled.error);
            shader=std::move(compiled.shader);cacheShader(pending.hash,compiled.code);
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
    FragmentBuffer fragmentBuffer(uint64_t elements,UINT stride) {
        if(!elements||elements>(1ull<<27)||elements*stride>uint64_t(D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM)*1024*1024)
            throw FlycastException("Invalid Hikaru DX11 fragment buffer size");
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(elements*stride);desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=stride;
        FragmentBuffer result;checkResult(device->CreateBuffer(&desc,nullptr,&result.buffer.get()),"fragment buffer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=UINT(elements);
        checkResult(device->CreateUnorderedAccessView(result.buffer,&view,&result.view.get()),"fragment buffer view");return result;
    }
    void resizeFragmentNodes(FragmentStorage& storage,uint32_t capacity) {
        // Construct all four blocks before replacing live storage. The D3D
        // command stream retains references used by the preceding resolve.
        std::array<FragmentBuffer,4> nodes;
        for(auto& node:nodes)node=fragmentBuffer(capacity/4,16);
        storage.nodes=std::move(nodes);storage.capacity=capacity;
        ++fragmentAllocations;hikaru_fragment_pool_reset_window(&fragmentPoolPolicy);
    }
    void initializeFragmentPrograms() {
        if(!fragmentConstants)fragmentConstants=buffer(context1?ConstantStream:sizeof(FragmentConstants),D3D11_BIND_CONSTANT_BUFFER);
        // Indexed textures are an independent mode even without a transform
        // palette. Exact generic collectors remain available while the one
        // optional material-specialization job is compiling.
        for(unsigned indexed=0;indexed<fragmentGenericPixels.size();++indexed)if(!fragmentGenericPixels[indexed]) {
            MeshShaderSpecialization dynamic{};dynamic[8]=indexed;
            auto code=compile(dx11ShaderSource(false,false,false,dynamic,false,0,false,true),"pixelMain","ps_5_0");
            checkResult(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&fragmentGenericPixels[indexed].get()),"fragment collector shader");
        }
        auto& resolve=fragmentResolvePixels[fragmentSmallList?0:1];
        if(!resolve) {
            auto code=compile(dx11FragmentListResolveSource(fragmentSmallList),"pixelMain","ps_5_0");
            checkResult(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&resolve.get()),"fragment resolve shader");
        }
    }
    void bindFragmentConstants() {
        UINT offset=0;
        if(context1) {
            if(fragmentConstantsCursor+256>ConstantStream)fragmentConstantsCursor=0;
            offset=UINT(fragmentConstantsCursor);fragmentConstantsCursor+=256;
        }
        const FragmentConstants constants{fragmentParameters,0};
        upload(fragmentConstants,&constants,sizeof(constants),offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,offset);
        if(context1) {
            const UINT first=offset/16,count=16;
            context1->PSSetConstantBuffers1(2,1,&fragmentConstants.get(),&first,&count);
        } else context->PSSetConstantBuffers(2,1,&fragmentConstants.get());
    }
    void bindFragmentViews(ID3D11RenderTargetView* color) {
        std::array<ID3D11UnorderedAccessView*,6> views{fragmentLists->heads.view,
            fragmentLists->nodes[0].view,fragmentLists->nodes[1].view,fragmentLists->nodes[2].view,fragmentLists->nodes[3].view,fragmentLists->status.view};
        // UAV slots share the RTV namespace. Reserve slot zero even in the
        // collector, which deliberately has no color or depth attachment.
        context->OMSetRenderTargetsAndUnorderedAccessViews(1,&color,nullptr,1,UINT(views.size()),views.data(),nullptr);
        boundTargets={color,nullptr};boundDepth=nullptr;bindingsDirty=true;
    }
    void finishFragmentBindings() {
        unbindInputs();unbindFragmentViews();
        ID3D11Buffer* empty=nullptr;context->PSSetConstantBuffers(2,1,&empty);
    }
    void traceFragmentEnd(uint32_t used,bool failed) const {
        if(fragmentTrace&&(fragmentCounts[0]==1||fragmentCounts[0]%60==0||failed))
            std::fprintf(stderr,"Hikaru DX11 fragment lists: %ux%u used=%u capacity=%u fallback=%u resolves=%llu allocations=%llu poolBytes=%llu\n",
                fragmentParameters.width,fragmentParameters.height,used,fragmentParameters.capacity,unsigned(failed),
                static_cast<unsigned long long>(fragmentResolves),static_cast<unsigned long long>(fragmentAllocations),
                static_cast<unsigned long long>(fragmentLists?uint64_t(fragmentParameters.width)*fragmentParameters.height*8+32+uint64_t(fragmentLists->capacity)*16:0));
    }
    void unbindInputs() {
        if(bindingsDirty||std::any_of(boundInputs.begin(),boundInputs.end(),[](auto* view){return view!=nullptr;})) {
            boundInputs={};context->PSSetShaderResources(0,UINT(boundInputs.size()),boundInputs.data());
        }
    }
    void unbindFragmentViews() {
        std::array<ID3D11UnorderedAccessView*,6> empty{};
        context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,UINT(empty.size()),empty.data(),nullptr);
        boundTargets={};boundDepth=nullptr;bindingsDirty=true;
    }
    void unbindTargets() {
        context->OMSetRenderTargets(0,nullptr,nullptr);boundTargets={};boundDepth=nullptr;bindingsDirty=true;
    }
    Image& image(unsigned id) {
        auto found=images.find(id);if(found==images.end())throw FlycastException("Unknown Hikaru DX11 image");return found->second;
    }
    struct ImageDescription {
        D3D11_TEXTURE2D_DESC texture{};
        DXGI_FORMAT sampledView = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT depthView = DXGI_FORMAT_UNKNOWN;
    };
    static ImageDescription describeImage(const Texture& info) {
        if(!info.width||!info.height||info.width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION||info.height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw FlycastException("Invalid Hikaru DX11 image size");
        ImageDescription image;
        auto& desc = image.texture;
        desc.Width = info.width;
        desc.Height = info.height;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (info.isDepth()) {
            desc.MipLevels = 1;
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (info.format == GL_DEPTH_COMPONENT16) {
                desc.Format = DXGI_FORMAT_R16_TYPELESS;
                image.depthView = DXGI_FORMAT_D16_UNORM;
                image.sampledView = DXGI_FORMAT_R16_UNORM;
            } else if (info.format == GL_DEPTH_COMPONENT24 || info.format == GL_DEPTH24_STENCIL8) {
                desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
                image.depthView = DXGI_FORMAT_D24_UNORM_S8_UINT;
                image.sampledView = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            } else {
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
                image.depthView = DXGI_FORMAT_D32_FLOAT;
                image.sampledView = DXGI_FORMAT_R32_FLOAT;
            }
            if (info.renderbuffer)
                image.sampledView = DXGI_FORMAT_UNKNOWN;
        } else {
            desc.MipLevels = 1 + std::min(info.maxLevel, mipCount(info.width, info.height) - 1);
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.Format = info.format == GL_RGB10_A2 ? DXGI_FORMAT_R10G10B10A2_UNORM
                : info.format == GL_RG32UI ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
            image.sampledView = desc.Format;
        }
        if (image.sampledView != DXGI_FORMAT_UNKNOWN)
            desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        return image;
    }
    Image makeImage(const ImageDescription& storage) {
        Image result;
        result.description = storage.texture;
        result.depth = storage.depthView != DXGI_FORMAT_UNKNOWN;
        result.sampleable = storage.sampledView != DXGI_FORMAT_UNKNOWN;
        const auto& desc = result.description;
        checkResult(device->CreateTexture2D(&desc,nullptr,&result.texture.get()),"texture allocation");
        if(result.sampleable) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=storage.sampledView;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=desc.MipLevels;
            checkResult(device->CreateShaderResourceView(result.texture,&view,&result.view.get()),"texture view");
        }
        if(result.depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC view{};view.Format=storage.depthView;view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
            checkResult(device->CreateDepthStencilView(result.texture,&view,&result.depthView.get()),"depth view");
        } else checkResult(device->CreateRenderTargetView(result.texture,nullptr,&result.target.get()),"color target");
        result.initialized.resize(desc.MipLevels,false);return result;
    }
    ID3D11BlendState* blendState(const Draw& draw) {
        unsigned mask=0;for(unsigned i=0;i<4;++i)if(draw.colorMask[i])mask|=1u<<i;
        const BlendKey key{draw.blend,draw.srcRGB,draw.dstRGB,draw.srcAlpha,draw.dstAlpha,draw.equationRGB,draw.equationAlpha,mask};
        if(lastBlend!=blends.end()&&lastBlend->first==key)return lastBlend->second;
        auto found=blends.find(key);
        if(found!=blends.end()){lastBlend=found;return found->second;}
        D3D11_BLEND_DESC desc{};desc.IndependentBlendEnable=true;
        for(auto& state:desc.RenderTarget) {
            state.SrcBlend=state.SrcBlendAlpha=D3D11_BLEND_ONE;state.DestBlend=state.DestBlendAlpha=D3D11_BLEND_ZERO;
            state.BlendOp=state.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        }
        auto& state=desc.RenderTarget[0];state.BlendEnable=draw.blend;
        state.SrcBlend=blendFactor(draw.srcRGB);state.DestBlend=blendFactor(draw.dstRGB);state.BlendOp=blendOperation(draw.equationRGB);
        state.SrcBlendAlpha=blendFactor(draw.srcAlpha,true);state.DestBlendAlpha=blendFactor(draw.dstAlpha,true);state.BlendOpAlpha=blendOperation(draw.equationAlpha);
        state.RenderTargetWriteMask=UINT8(mask);desc.RenderTarget[1].RenderTargetWriteMask=UINT8(mask);
        ComPtr<ID3D11BlendState> result;checkResult(device->CreateBlendState(&desc,&result.get()),"blend state");return (lastBlend=blends.emplace(key,std::move(result)).first)->second;
    }
    ID3D11DepthStencilState* depthState(const Draw& draw) {
        const bool enabled=!fragmentCollectActive&&draw.depth&&draw.depthTest;
        const DepthKey key{enabled,enabled&&draw.depthWrite,draw.depthFunc};
        if(lastDepth!=depths.end()&&lastDepth->first==key)return lastDepth->second;
        auto found=depths.find(key);
        if(found!=depths.end()){lastDepth=found;return found->second;}
        D3D11_DEPTH_STENCIL_DESC desc{};desc.DepthEnable=enabled;desc.DepthWriteMask=enabled&&draw.depthWrite?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc=comparison(draw.depthFunc);desc.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;desc.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
        desc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};desc.BackFace=desc.FrontFace;
        ComPtr<ID3D11DepthStencilState> result;checkResult(device->CreateDepthStencilState(&desc,&result.get()),"depth state");return (lastDepth=depths.emplace(key,std::move(result)).first)->second;
    }
    ID3D11RasterizerState* rasterState(const Draw& draw) {
        // D3D and Metal use the same visual front-face convention. Our extra
        // offscreen clip-Y flip reverses that winding for internal targets.
        const bool ccw=(draw.frontFace==GL_CCW)!=(draw.uniforms.inverted[2]!=0.f);
        if(!std::isfinite(draw.biasFactor)||!std::isfinite(draw.biasUnits)||draw.biasUnits<INT_MIN||draw.biasUnits>=float(INT_MAX))
            throw FlycastException("Invalid Hikaru DX11 depth bias");
        const RasterKey key{draw.cull,draw.cullFace,draw.scissorEnabled,ccw,draw.biasFactor,draw.biasUnits};
        if(lastRaster!=rasters.end()&&lastRaster->first==key)return lastRaster->second;
        auto found=rasters.find(key);
        if(found!=rasters.end()){lastRaster=found;return found->second;}
        D3D11_RASTERIZER_DESC desc{};desc.FillMode=D3D11_FILL_SOLID;desc.CullMode=!draw.cull?D3D11_CULL_NONE:draw.cullFace==GL_FRONT?D3D11_CULL_FRONT:D3D11_CULL_BACK;
        desc.FrontCounterClockwise=ccw;desc.ScissorEnable=draw.scissorEnabled;desc.DepthClipEnable=true;
        desc.DepthBias=INT(std::lround(draw.biasUnits));desc.SlopeScaledDepthBias=draw.biasFactor;
        ComPtr<ID3D11RasterizerState> result;checkResult(device->CreateRasterizerState(&desc,&result.get()),"rasterizer state");return (lastRaster=rasters.emplace(key,std::move(result)).first)->second;
    }
    ID3D11SamplerState* sampler(const Texture& sampling,const Image& sampled) {
        const unsigned maxLod=isMipmapped(sampling.minFilter)?std::min(sampled.description.MipLevels-1,sampling.maxLevel):0;
        SamplerKey key{sampling.minFilter,sampling.magFilter,sampling.wrapS,sampling.wrapT,maxLod};
        if(lastSampler!=samplers.end()&&lastSampler->first==key)return lastSampler->second;
        auto found=samplers.find(key);
        if(found!=samplers.end()){lastSampler=found;return found->second;}
        const bool minLinear=sampling.minFilter==GL_LINEAR||sampling.minFilter==GL_LINEAR_MIPMAP_NEAREST||sampling.minFilter==GL_LINEAR_MIPMAP_LINEAR;
        const bool mipLinear=sampling.minFilter==GL_NEAREST_MIPMAP_LINEAR||sampling.minFilter==GL_LINEAR_MIPMAP_LINEAR;
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_ENCODE_BASIC_FILTER(minLinear?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,
            sampling.magFilter==GL_LINEAR?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,mipLinear?D3D11_FILTER_TYPE_LINEAR:D3D11_FILTER_TYPE_POINT,0);
        desc.AddressU=addressMode(sampling.wrapS);desc.AddressV=addressMode(sampling.wrapT);desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxAnisotropy=1;desc.ComparisonFunc=D3D11_COMPARISON_NEVER;desc.MaxLOD=float(maxLod);
        ComPtr<ID3D11SamplerState> result;checkResult(device->CreateSamplerState(&desc,&result.get()),"sampler state");return (lastSampler=samplers.emplace(key,std::move(result)).first)->second;
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
        const PixelKey key{draw.bitmap,draw.familyResolve,!draw.palette.empty(),draw.peelComposite,meshShaderSpecialization(draw,specialize),passFlags,fragmentCollectActive,draw.depthPass};
        if(fragmentCollectActive&&!specialize)return genericPixel(key);
        auto found=pixelShaders.find(key);
        // Cache deferred keys too: a pending compiler must not make every
        // sorted triangle rebuild and hash the same HLSL source each frame.
        if(found!=pixelShaders.end()&&(!found->second.deferred||pendingPixel)) {
            found->second.touched=++shaderClock;return found->second.shader;
        }
        const auto source=dx11ShaderSource(key.bitmap,key.family,key.palette,key.constants,false,key.passFlags,key.peelComposite,key.fragmentList,key.depthPass);
        ComPtr<ID3DBlob> code;const u64 hash=hashShader(source.c_str(),"pixelMain","ps_5_0");
        const bool cached=lookupShader(hash,code);
        if(diagnostics.enabled) {
            ++diagnostics.costs.shaderCacheLookups;
            diagnostics.costs.shaderCacheHits+=cached;
        }
        if(!cached) {
            if(backgroundCompilation&&!draw.bitmap) {
                // One outstanding job bounds CPU/memory use. All immediate-
                // context work and bytecode-cache access stay on this thread.
                // The worker owns its source/device until compilation finishes.
                if(!pendingPixel) {
                    try {
                        auto result=std::async(std::launch::async,[dxDevice=device,dxCompiler=compiler,source,measureCompile=diagnostics.enabled] {
                            // MSVC may reuse an async pool thread. Restore its
                            // priority on success and on compiler/device failure.
                            struct PriorityRestore {
                                int previous=GetThreadPriority(GetCurrentThread());
                                ~PriorityRestore(){SetThreadPriority(GetCurrentThread(),previous);}
                            } priority;
                            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
                            CompiledPixel compiled;compiled.compileAttempted=true;
                            const auto started=measureCompile?DiagnosticClock::now():DiagnosticClock::time_point{};
                            try {compiled.code=compileCode(dxCompiler,source,"pixelMain","ps_5_0");}
                            catch(...) {compiled.error=std::current_exception();}
                            if(measureCompile)compiled.compileNanoseconds=elapsedNanoseconds(started);
                            if(!compiled.error) {
                                try {
                                    checkResult(dxDevice->CreatePixelShader(compiled.code->GetBufferPointer(),compiled.code->GetBufferSize(),
                                        nullptr,&compiled.shader.get()),"background pixel shader");
                                } catch(...) {compiled.error=std::current_exception();}
                            }
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
        if(fragmentCollectActive)return; // UAV-only collection must not bind or modify the opaque attachments.
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
        // Collection has no DSV/MRT attachments. Its material shader must
        // sample the opaque depth and family metadata instead of replacing
        // them with the zero fallback reserved for real attachment feedback.
        const bool attachment=id==draw.color||(!fragmentCollectActive&&(id==draw.depth||id==draw.metadata));
        if(!id||attachment||!sampled||!sampled->sampleable||!sampled->initialized[0])sampled=nullptr;
        if(sampled&&isMipmapped(sampling.minFilter)) {
            const unsigned levels=1+std::min(sampling.maxLevel,mipCount(sampled->description.Width,sampled->description.Height)-1);
            if(levels>sampled->initialized.size())sampled=nullptr;
            else for(unsigned level=0;sampled&&level<levels;++level)if(!sampled->initialized[level])sampled=nullptr;
        }
        if(sampled&&(sampled->description.Format==DXGI_FORMAT_R32G32_UINT)!=integer)sampled=nullptr;
        return sampled?*sampled:image(integer?MetadataFallback:ColorFallback);
    }
    static constexpr unsigned GpuPhaseCount=5,GpuFrameRingSize=4,NoGpuFrame=GpuFrameRingSize;
    static constexpr unsigned GpuFramePhase=0,GpuClearPhase=1,GpuCollectPhase=2,GpuCopyPhase=3,GpuResolvePhase=4;
    static constexpr unsigned GpuTimestampCount=GpuPhaseCount*2;
    void disableGpuTiming(HRESULT result) {
        if(diagnostics.activeFrame<NoGpuFrame) {
            auto& frame=diagnostics.frames[diagnostics.activeFrame];
            if(frame.open) {
                const unsigned endIndex=GpuFramePhase*2+1;
                context->End(frame.timestamps[endIndex]);
                frame.recordedTimestamps|=1u<<endIndex;
                frame.phaseMask|=1u<<GpuFramePhase;
                context->End(frame.disjoint);frame.open=false;
            }
        }
        diagnostics.gpuSupported=false;
        diagnostics.activeFrame=NoGpuFrame;
        diagnostics.frameAttempted=false;
        for(auto& frame:diagnostics.frames)frame=GpuFrame{};
        if(!diagnostics.gpuFailureReported) {
            std::fprintf(stderr,"Hikaru DX11 GPU timing unavailable (0x%08X); rendering continues\n",unsigned(result));
            diagnostics.gpuFailureReported=true;
        }
    }
    bool createDiagnosticQuery(D3D11_QUERY type,ComPtr<ID3D11Query>& query) {
        const D3D11_QUERY_DESC description{type,0};
        const HRESULT result=device->CreateQuery(&description,&query.get());
        if(FAILED(result)) {
            checkResult(device->GetDeviceRemovedReason(),"diagnostic query device");
            disableGpuTiming(result);return false;
        }
        if(diagnostics.enabled)++diagnostics.costs.gpuQueryObjects;
        return true;
    }
    bool initializeGpuQueries(GpuFrame& frame) {
        if(!frame.disjoint&&!createDiagnosticQuery(D3D11_QUERY_TIMESTAMP_DISJOINT,frame.disjoint))return false;
        for(auto& timestamp:frame.timestamps)
            if(!timestamp&&!createDiagnosticQuery(D3D11_QUERY_TIMESTAMP,timestamp))return false;
        return true;
    }
    // Sample the first list attempt only, including overflow. Later viewports must not
    // replace its timestamps or make a failed pass look like a successful resolve.
    void markGpuTimestamp(unsigned phase,bool end) {
        if(diagnostics.activeFrame>=GpuFrameRingSize||(phase&&!diagnostics.frames[diagnostics.activeFrame].listRecording))return;
        auto& frame=diagnostics.frames[diagnostics.activeFrame];
        const unsigned index=phase*2+unsigned(end);
        context->End(frame.timestamps[index]);
        frame.recordedTimestamps|=1u<<index;
        if(end&&(frame.recordedTimestamps&(3u<<(phase*2)))==(3u<<(phase*2)))
            frame.phaseMask|=1u<<phase;
    }
    bool readDiagnosticQuery(ID3D11Query* query,void* data,UINT bytes) {
        const HRESULT result=context->GetData(query,data,bytes,D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(result==S_FALSE)return false;
        if(FAILED(result)) {
            checkResult(device->GetDeviceRemovedReason(),"diagnostic query device");
            disableGpuTiming(result);return false;
        }
        return true;
    }
    void pollGpuFrames() {
        if(!diagnostics.gpuRequested||!diagnostics.gpuSupported)return;
        // Poll each outstanding result once; S_FALSE keeps the entire slot in flight.
        for(auto& frame:diagnostics.frames) {
            if(!frame.inFlight)continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            bool ready=readDiagnosticQuery(frame.disjoint,&disjoint,sizeof(disjoint));
            std::array<UINT64,GpuTimestampCount> timestamps{};
            for(unsigned i=0;i<GpuTimestampCount&&diagnostics.gpuSupported;++i) {
                if(frame.recordedTimestamps&(1u<<i))
                    ready=readDiagnosticQuery(frame.timestamps[i],&timestamps[i],sizeof(timestamps[i]))&&ready;
            }
            if(!diagnostics.gpuSupported)return;
            if(!ready)continue;
            bool valid=frame.valid&&!disjoint.Disjoint&&disjoint.Frequency!=0;
            std::array<double,GpuPhaseCount> milliseconds{};
            for(unsigned phase=0;valid&&phase<GpuPhaseCount;++phase) {
                if(!(frame.phaseMask&(1u<<phase)))continue;
                const UINT64 begin=timestamps[phase*2],end=timestamps[phase*2+1];
                if(end<begin) {valid=false;break;}
                milliseconds[phase]=double(end-begin)*1000.0/double(disjoint.Frequency);
                valid=std::isfinite(milliseconds[phase])&&milliseconds[phase]>=0;
            }
            // Reject the whole frame before publishing any phase of a bad sample.
            if(!valid)++diagnostics.gpuInvalid;
            else for(unsigned phase=0;phase<GpuPhaseCount;++phase) {
                if(!(frame.phaseMask&(1u<<phase)))continue;
                diagnostics.gpuMilliseconds[phase]+=milliseconds[phase];++diagnostics.gpuSamples[phase];
            }
            frame.inFlight=false;frame.recordedTimestamps=0;frame.phaseMask=0;frame.listClaimed=false;frame.listRecording=false;
        }
    }
    void ensureGpuFrame() {
        if(!diagnostics.gpuRequested||diagnostics.gpuFailureReported||diagnostics.frameAttempted)return;
        diagnostics.frameAttempted=true;pollGpuFrames();
        if(diagnostics.gpuFailureReported)return;
        // A saturated ring skips this frame instead of blocking or reusing pending queries.
        for(unsigned offset=0;offset<GpuFrameRingSize;++offset) {
            const unsigned index=(diagnostics.nextFrame+offset)%GpuFrameRingSize;
            auto& frame=diagnostics.frames[index];
            if(frame.open||frame.inFlight)continue;
            if(!initializeGpuQueries(frame)) {++diagnostics.gpuDropped;return;}
            diagnostics.gpuSupported=true;
            frame.recordedTimestamps=0;frame.phaseMask=0;frame.listClaimed=false;frame.listRecording=false;
            frame.open=true;frame.valid=true;diagnostics.activeFrame=index;diagnostics.nextFrame=(index+1)%GpuFrameRingSize;
            context->Begin(frame.disjoint);markGpuTimestamp(GpuFramePhase,false);return;
        }
        ++diagnostics.gpuDropped;
    }
    void finishGpuFrame(bool valid=true) {
        if(diagnostics.activeFrame<NoGpuFrame) {
            auto& frame=diagnostics.frames[diagnostics.activeFrame];
            frame.valid=valid;
            markGpuTimestamp(GpuFramePhase,true);context->End(frame.disjoint);
            frame.open=false;frame.inFlight=true;diagnostics.activeFrame=NoGpuFrame;
        }
        diagnostics.frameAttempted=false;
    }
    void presentDiagnostics() {
        if(!diagnostics.enabled||++diagnostics.presentedFrames<60)return;
        const auto& costs=diagnostics.costs;
        std::fprintf(stderr,
            "Hikaru DX11 costs: draws=%llu vertex_uploads=%llu vertex_bytes=%llu uniform_uploads=%llu uniform_hits=%llu uniform_bytes=%llu palette_uploads=%llu palette_hits=%llu palette_bytes=%llu shadercache_lookups=%llu shadercache_hits=%llu shadercache_compiles=%llu shadercache_compile_ns=%llu texture_uploads=%llu texture_bytes=%llu queries_created=%llu gpu_query_objects=%llu query_wait_ns=%llu fragment_counter_wait_ns=%llu masked_clears=%llu masked_pixels=%llu\n",
            (unsigned long long)costs.draws,(unsigned long long)costs.vertexUploads,(unsigned long long)costs.vertexBytes,
            (unsigned long long)costs.uniformUploads,(unsigned long long)costs.uniformHits,(unsigned long long)costs.uniformBytes,
            (unsigned long long)costs.paletteUploads,(unsigned long long)costs.paletteHits,(unsigned long long)costs.paletteBytes,
            (unsigned long long)costs.shaderCacheLookups,(unsigned long long)costs.shaderCacheHits,
            (unsigned long long)costs.shaderCompiles,(unsigned long long)costs.shaderCompileNanoseconds,
            (unsigned long long)costs.textureUploads,(unsigned long long)costs.textureBytes,
            (unsigned long long)costs.queriesCreated,(unsigned long long)costs.gpuQueryObjects,
            (unsigned long long)costs.queryWaitNanoseconds,
            (unsigned long long)costs.fragmentCounterWaitNanoseconds,(unsigned long long)costs.maskedClears,
            (unsigned long long)costs.maskedPixels);
        if(traceGpuTiming) {
            char milliseconds[GpuPhaseCount][32]{};
            for(unsigned phase=0;phase<GpuPhaseCount;++phase) {
                if(diagnostics.gpuSamples[phase])
                    std::snprintf(milliseconds[phase],sizeof(milliseconds[phase]),"%.3f",
                        diagnostics.gpuMilliseconds[phase]/double(diagnostics.gpuSamples[phase]));
                else std::snprintf(milliseconds[phase],sizeof(milliseconds[phase]),"unavailable");
            }
            std::fprintf(stderr,
                "Hikaru DX11 GPU timing: frame_ms=%s clear_ms=%s collect_ms=%s copy_ms=%s resolve_ms=%s frame_samples=%llu clear_samples=%llu collect_samples=%llu copy_samples=%llu resolve_samples=%llu dropped=%llu invalid=%llu supported=%u\n",
                milliseconds[0],milliseconds[1],milliseconds[2],milliseconds[3],milliseconds[4],
                (unsigned long long)diagnostics.gpuSamples[0],(unsigned long long)diagnostics.gpuSamples[1],
                (unsigned long long)diagnostics.gpuSamples[2],(unsigned long long)diagnostics.gpuSamples[3],
                (unsigned long long)diagnostics.gpuSamples[4],(unsigned long long)diagnostics.gpuDropped,
                (unsigned long long)diagnostics.gpuInvalid,unsigned(diagnostics.gpuSupported));
        }
        diagnostics.costs={};diagnostics.gpuMilliseconds={};diagnostics.gpuSamples={};
        diagnostics.gpuDropped=diagnostics.gpuInvalid=diagnostics.presentedFrames=0;
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
        endQuerySegment();finishGpuFrame();
        if(owner)saveCache(CacheFile);
        if(owner&&GraphicsContext::Instance()==owner)owner->resetHikaruPresentation();
        // The immediate context owns submitted resource references. Releasing
        // CPU cache entries never requires a device-wide GPU idle wait.
        finishFragmentBindings();unbindTargets();
    }
#ifdef HIKARU_NATIVE_TESTING
    void completeCompilationForTesting() {completePixelCompilation(true);}
    Dx11DiagnosticsSnapshot diagnosticsForTesting() const {
        Dx11DiagnosticsSnapshot result;
        result.enabled=diagnostics.enabled;result.gpuRequested=diagnostics.gpuRequested;result.gpuSupported=diagnostics.gpuSupported;
        const auto& costs=diagnostics.costs;
        result.draws=costs.draws;result.vertexUploads=costs.vertexUploads;result.vertexBytes=costs.vertexBytes;
        result.uniformUploads=costs.uniformUploads;result.uniformHits=costs.uniformHits;result.uniformBytes=costs.uniformBytes;
        result.paletteUploads=costs.paletteUploads;result.paletteHits=costs.paletteHits;result.paletteBytes=costs.paletteBytes;
        result.shaderCacheLookups=costs.shaderCacheLookups;result.shaderCacheHits=costs.shaderCacheHits;
        result.shaderCompiles=costs.shaderCompiles;result.shaderCompileNanoseconds=costs.shaderCompileNanoseconds;
        result.textureUploads=costs.textureUploads;result.textureBytes=costs.textureBytes;
        result.queriesCreated=costs.queriesCreated;result.gpuQueryObjects=costs.gpuQueryObjects;
        result.queryWaitNanoseconds=costs.queryWaitNanoseconds;
        result.fragmentCounterWaitNanoseconds=costs.fragmentCounterWaitNanoseconds;
        result.maskedClears=costs.maskedClears;result.maskedPixels=costs.maskedPixels;
        const auto phase=[&](unsigned index) {
            return Dx11GpuTimingPhase{diagnostics.gpuSamples[index],
                diagnostics.gpuSamples[index]?diagnostics.gpuMilliseconds[index]/double(diagnostics.gpuSamples[index]):0.0};
        };
        result.frame=phase(0);result.clear=phase(1);result.collect=phase(2);result.copy=phase(3);result.resolve=phase(4);
        result.gpuDropped=diagnostics.gpuDropped;result.gpuInvalid=diagnostics.gpuInvalid;return result;
    }
    void finishDiagnosticFrameForTesting() {finishGpuFrame();}
    void pollDiagnosticFramesForTesting() {pollGpuFrames();}
    std::array<uint64_t,3> fragmentCountsForTesting() const {return fragmentCounts;}
    std::array<uint64_t,4> fragmentPoolForTesting() const {
        return {fragmentLists?fragmentLists->capacity:0,fragmentAllocations,
            fragmentLists?uint64_t(fragmentLists->background.description.Width)*fragmentLists->background.description.Height*8+32+uint64_t(fragmentLists->capacity)*16:0,
            fragmentPoolPolicy.hint};
    }
    void forceGeneralFragmentResolveForTesting() {fragmentSmallList=false;}
    void disableFragmentListsForTesting() {fragmentListSupported=false;}
#endif
    bool beginFragmentList(unsigned color,uint64_t budget) override {
        if(fragmentCollectActive)throw FlycastException("Nested Hikaru DX11 fragment-list collection");
        if(!fragmentListSupported||occlusionActive||!pendingOcclusions.empty())return false;
        auto& target=image(color);const auto& desc=target.description;
        if(target.depth||desc.SampleDesc.Count!=1||
            (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)) {
            finishFragmentBindings();fragmentLists.reset();return false;
        }
        const uint64_t pixels=uint64_t(desc.Width)*desc.Height,fixedBytes=pixels*8+32;
        const uint64_t blockLimit=uint64_t(D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM)*1024*1024;
        const uint64_t maximum=budget>fixedBytes?std::min((budget-fixedBytes)/16,(blockLimit/16)*4):0;
        if(pixels>(1ull<<27)||pixels*4>blockLimit||maximum<4) {
            // Release storage before peeling takes ownership of the same budget.
            finishFragmentBindings();fragmentLists.reset();fragmentPoolPolicy={};return false;
        }
        if(fragmentPoolWidth!=desc.Width||fragmentPoolHeight!=desc.Height||fragmentPoolFormat!=desc.Format) {
            fragmentPoolPolicy={};fragmentPoolWidth=desc.Width;fragmentPoolHeight=desc.Height;fragmentPoolFormat=desc.Format;
        }
        const uint64_t desired=fragmentPoolPolicy.hint?std::max(4096u,fragmentPoolPolicy.hint):std::max<uint64_t>(4096,pixels);
        const uint32_t capacity=uint32_t(std::min(maximum,desired)/4*4);
        unbindInputs();unbindFragmentViews();
        try {
            initializeFragmentPrograms();
            if(!fragmentLists||fragmentLists->background.description.Width!=desc.Width||
                fragmentLists->background.description.Height!=desc.Height||fragmentLists->background.description.Format!=desc.Format) {
                fragmentLists.reset();
                auto storage=std::make_unique<FragmentStorage>();
                storage->heads=fragmentBuffer(pixels,4);storage->status=fragmentBuffer(4,4);
                D3D11_BUFFER_DESC readback{};readback.ByteWidth=16;readback.Usage=D3D11_USAGE_STAGING;readback.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                checkResult(device->CreateBuffer(&readback,nullptr,&storage->readback.get()),"fragment readback");
                Texture info;info.width=desc.Width;info.height=desc.Height;info.maxLevel=0;
                info.format=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM?GL_RGB10_A2:GL_RGBA;
                storage->background=makeImage(describeImage(info));resizeFragmentNodes(*storage,capacity);fragmentLists=std::move(storage);
            } else if(fragmentLists->capacity!=capacity)resizeFragmentNodes(*fragmentLists,capacity);
        } catch(const std::exception& error) {
            finishFragmentBindings();fragmentLists.reset();
            // A device removal is fatal, unlike an optional program/allocation
            // failure. Do not disguise it as a recoverable sorting fallback.
            checkResult(device->GetDeviceRemovedReason(),"fragment-list device");
            fragmentListSupported=false;
            WARN_LOG(RENDERER,"Hikaru DX11 fragment lists unavailable; using depth peeling: %s",error.what());
            return false;
        }
        fragmentParameters={desc.Width,desc.Height,capacity,0,
            desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM?1023.f:255.f,
            desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM?3.f:255.f,capacity/4};
        ensureGpuFrame();
        if(diagnostics.activeFrame<GpuFrameRingSize) {
            auto& frame=diagnostics.frames[diagnostics.activeFrame];
            frame.listRecording=!frame.listClaimed;frame.listClaimed=true;
        }
        markGpuTimestamp(GpuClearPhase,false);
        const UINT zero[4]={};context->ClearUnorderedAccessViewUint(fragmentLists->heads.view,zero);
        context->ClearUnorderedAccessViewUint(fragmentLists->status.view,zero);
        markGpuTimestamp(GpuClearPhase,true);markGpuTimestamp(GpuCollectPhase,false);
        fragmentColor=color;fragmentListFailed=false;fragmentCollectActive=true;++fragmentCounts[0];
        bindFragmentViews(nullptr);return true;
    }
    bool endFragmentList() override {
        if(!fragmentCollectActive)throw FlycastException("No active Hikaru DX11 fragment-list collection");
        fragmentCollectActive=false;finishFragmentBindings();markGpuTimestamp(GpuCollectPhase,true);
        // Map(READ) waits for this tiny copy and the preceding UAV writes. The
        // opaque image is still untouched, including on overflow or bad draws.
        context->CopyResource(fragmentLists->readback,fragmentLists->status.buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT mapResult;
        const auto mapStarted=diagnostics.enabled?DiagnosticClock::now():DiagnosticClock::time_point{};
        mapResult=context->Map(fragmentLists->readback,0,D3D11_MAP_READ,0,&mapped);
        if(diagnostics.enabled)diagnostics.costs.fragmentCounterWaitNanoseconds+=elapsedNanoseconds(mapStarted);
        checkResult(mapResult,"fragment counter map");
        std::array<uint32_t,4> status{};std::memcpy(status.data(),mapped.pData,sizeof(status));
        context->Unmap(fragmentLists->readback,0);
        const bool failed=fragmentListFailed||status[1]||status[0]>fragmentLists->capacity;
        if(failed) {
            ++fragmentCounts[2];
            if(fragmentListFailed)fragmentListSupported=false;
            else hikaru_fragment_pool_grow(&fragmentPoolPolicy,status[0],fragmentLists->capacity);
            if(!fragmentOverflowWarned) {
                WARN_LOG(RENDERER,"Hikaru DX11 fragment-list pool exceeded or unsupported draw; using depth peeling for this viewport");
                fragmentOverflowWarned=true;
            }
            traceFragmentEnd(status[0],true);
            if(diagnostics.activeFrame<GpuFrameRingSize)diagnostics.frames[diagnostics.activeFrame].listRecording=false;
            fragmentLists.reset();return false;
        }
        hikaru_fragment_pool_observe(&fragmentPoolPolicy,status[0],fragmentLists->capacity);
        ++fragmentCounts[1];
        if(status[0]) {
            auto& target=image(fragmentColor);
            // Copy only mip zero: a color target can own a mip chain, whereas
            // the preserved background intentionally has a single level.
            markGpuTimestamp(GpuCopyPhase,false);
            context->CopySubresourceRegion(fragmentLists->background.texture,0,0,0,0,target.texture,0,nullptr);
            markGpuTimestamp(GpuCopyPhase,true);markGpuTimestamp(GpuResolvePhase,false);
            bindFragmentViews(target.target);
            Draw state;state.color=fragmentColor;state.depthTest=state.depthWrite=false;state.blend=false;state.cull=false;
            state.viewport={0,0,GLint(fragmentParameters.width),GLint(fragmentParameters.height)};
            bindStates(state);
            context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(clearVertex,nullptr,0);
            context->PSSetShader(fragmentResolvePixels[fragmentSmallList?0:1],nullptr,0);
            context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
            bindFragmentConstants();
            context->PSSetShaderResources(0,1,&fragmentLists->background.view.get());boundInputs[0]=fragmentLists->background.view;
            context->Draw(3,0);markGpuTimestamp(GpuResolvePhase,true);++fragmentResolves;
            finishFragmentBindings();
        }
        if(diagnostics.activeFrame<GpuFrameRingSize)diagnostics.frames[diagnostics.activeFrame].listRecording=false;
        traceFragmentEnd(status[0],false);return true;
    }
    bool supportsDrawPalette() const override {return true;}
    bool supportsTexturePalette() const override {return true;}
    bool supportsPackedVertices() const override {return true;}
    void texture(unsigned id,const Texture& info,unsigned level,TextureUpload uploadType,const void* pixels,size_t bytes) override {
        if(fragmentCollectActive&&id==fragmentColor)throw FlycastException("Hikaru DX11 scene replacement during fragment collection");
        const bool update=uploadType==TextureUpload::FullUpdate;
        if(!info.width||!info.height||level>=mipCount(info.width,info.height))throw FlycastException("Invalid Hikaru DX11 mip definition");
        auto found=images.find(id);
        const auto storage = describeImage(info);
        const bool depth = storage.depthView != DXGI_FORMAT_UNKNOWN;
        const bool compatible = found != images.end()
            && found->second.description.Width == info.width && found->second.description.Height == info.height
            && found->second.description.Format == storage.texture.Format
            && found->second.description.BindFlags == storage.texture.BindFlags
            && found->second.description.MipLevels == storage.texture.MipLevels;
        const size_t pixelBytes=info.format==GL_RG32UI?8:4;
        const unsigned width=std::max(1u,info.width>>level),height=std::max(1u,info.height>>level);
        if(pixels&&bytes!=size_t(width)*height*pixelBytes)throw FlycastException("Invalid Hikaru DX11 texture byte count");
        if(update&&(!compatible||level||!pixels||depth))throw FlycastException("Hikaru DX11 full update requires compatible level-zero color storage");
        unbindInputs();
        // Texture cache misses and release of old peel targets can occur
        // between BeginFragmentList and the first mesh. Keep the collector's
        // UAV binding intact while unrelated texture storage changes.
        if(!fragmentCollectActive)unbindTargets();
        else bindingsDirty=true;
        if(!compatible){images.erase(id);images.emplace(id,makeImage(storage));}
        auto& target=image(id);if(level>=target.description.MipLevels)throw FlycastException("Hikaru DX11 mip exceeds texture storage");
        if(pixels) {
            if(depth)throw FlycastException("Hikaru DX11 CPU depth uploads are unsupported");
            context->UpdateSubresource(target.texture,level,nullptr,pixels,width*UINT(pixelBytes),0);
            if(diagnostics.enabled) {++diagnostics.costs.textureUploads;diagnostics.costs.textureBytes+=bytes;}
        }
        target.initialized[level]=true;
    }
    void eraseTexture(unsigned id) override {
        if(fragmentCollectActive&&id==fragmentColor)throw FlycastException("Hikaru DX11 scene deletion during fragment collection");
        unbindInputs();
        if(!fragmentCollectActive)unbindTargets();
        else bindingsDirty=true;
        if(id==0&&owner&&GraphicsContext::Instance()==owner)owner->presentHikaruFrame({});
        images.erase(id);
    }
    void clear(const Draw& draw,GLbitfield mask,const Vec4& color,float depth) override {
        if(fragmentCollectActive)throw FlycastException("Hikaru DX11 clear during fragment collection");
        ensureGpuFrame();endQuerySegment();
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
        if(diagnostics.enabled&&colors&&!allChannels) {
            ++diagnostics.costs.maskedClears;
            uint64_t width=target.description.Width,height=target.description.Height;
            if(draw.scissorEnabled) {
                const auto clippedExtent=[](int origin,int extent,unsigned limit) {
                    const int64_t first=std::clamp<int64_t>(origin,0,limit);
                    const int64_t last=std::clamp<int64_t>(int64_t(origin)+std::max(0,extent),0,limit);
                    return uint64_t(std::max<int64_t>(0,last-first));
                };
                width=clippedExtent(draw.scissor[0],draw.scissor[2],target.description.Width);
                height=clippedExtent(draw.scissor[1],draw.scissor[3],target.description.Height);
            }
            diagnostics.costs.maskedPixels+=width*height;
        }
    }
    void draw(const Draw& draw) override {
        const bool packed=!draw.packedVertices.empty();
        const size_t count=packed?draw.packedVertices.size():draw.vertices.size();
        const UINT stride=packed?sizeof(PackedVertex):sizeof(Vertex);
        if(!count||(draw.cull&&draw.cullFace==GL_FRONT_AND_BACK))return;
        if((packed&&(!draw.vertices.empty()||draw.bitmap))||count>UINT_MAX/stride||draw.palette.size()>DrawPaletteSize||
            (draw.texturePalette&&(draw.paletteTextures.size()!=draw.palette.size()||draw.paletteSampling.size()!=draw.palette.size())))
            throw FlycastException("Invalid Hikaru DX11 draw size");
        uint32_t primitives=0;
        if(fragmentCollectActive) {
            const uint64_t primitiveCount=draw.topology==GL_TRIANGLES?count/3:count>=3?count-2:0;
            if(fragmentListFailed)return;
            if(draw.color!=fragmentColor||draw.bitmap||draw.familyResolve||draw.peelComposite||!draw.palette.empty()||
                (draw.topology!=GL_TRIANGLES&&draw.topology!=GL_TRIANGLE_STRIP)||
                primitiveCount>UINT32_MAX-fragmentParameters.orderBase) {
                fragmentListFailed=true;return;
            }
            primitives=uint32_t(primitiveCount);
        }
        ensureGpuFrame();
        const size_t bytes=count*stride;
        if(vertexCapacity<bytes){vertexCapacity=std::max(size_t(1024*1024),bytes);vertexBuffer=buffer(vertexCapacity,D3D11_BIND_VERTEX_BUFFER);vertexOffset=0;}
        // Scalar, packed and bitmap draws share a stream. Align to the
        // current stride so Draw's first-vertex offset never truncates bytes.
        vertexOffset=(vertexOffset+stride-1)/stride*stride;
        if(vertexOffset+bytes>vertexCapacity)vertexOffset=0;
        const void* vertices=packed?static_cast<const void*>(draw.packedVertices.data()):draw.vertices.data();
        upload(vertexBuffer,vertices,bytes,vertexOffset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,vertexOffset);
        if(diagnostics.enabled) {++diagnostics.costs.vertexUploads;diagnostics.costs.vertexBytes+=bytes;}
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
        if(diagnostics.enabled) {
            if(vertexChanged||pixelChanged) {
                ++diagnostics.costs.uniformUploads;
                if(vertexChanged)diagnostics.costs.uniformBytes+=materialOffset+sizeof(uniforms.inverted);
                if(pixelChanged)diagnostics.costs.uniformBytes+=sizeof(Uniforms)-materialOffset;
            } else ++diagnostics.costs.uniformHits;
        }
        uploadedUniforms=uniforms;
        if(!draw.palette.empty()) {
            for(size_t i=0;i<count;++i) {
                const float index=packed?draw.packedVertices[i].drawIndex:draw.vertices[i].extra[2];
                if(!std::isfinite(index)||index<0||index>=draw.palette.size()||std::floor(index)!=index)
                    throw FlycastException("Invalid Hikaru DX11 draw palette index");
            }
            const size_t paletteBytes=draw.palette.size()*sizeof(DrawRecord);
            if(uploadedPaletteSize!=draw.palette.size()||std::memcmp(uploadedPalette.data(),draw.palette.data(),paletteBytes)!=0) {
                upload(paletteBuffer,draw.palette.data(),paletteBytes);
                std::copy(draw.palette.begin(),draw.palette.end(),uploadedPalette.begin());uploadedPaletteSize=draw.palette.size();
                if(diagnostics.enabled) {++diagnostics.costs.paletteUploads;diagnostics.costs.paletteBytes+=paletteBytes;}
            } else if(diagnostics.enabled)++diagnostics.costs.paletteHits;
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
        const bool texturePalette=draw.texturePalette&&!draw.palette.empty();
        const unsigned baseSlots=texturePalette?static_cast<unsigned>(draw.palette.size()):1;
        for(unsigned i=0;i<baseSlots;++i) {
            const auto& state=texturePalette?draw.paletteSampling[i]:draw.sampling[0];
            const unsigned id=texturePalette?draw.paletteTextures[i]:draw.textures[0];
            auto& input=sampledImage(id,state,draw,false);views[i]=input.view;sampling[i]=sampler(state,input);
        }
        if(draw.texturePalette&&baseSlots<DrawPaletteSize) {
            // The shader declares all palette slots. Unused entries share the
            // same transparent image and sampler, regardless of the last draw.
            auto& fallback=sampledImage(0,draw.sampling[0],draw,false);
            std::fill(views.begin()+baseSlots,views.begin()+DrawPaletteSize,fallback.view.get());
            std::fill(sampling.begin()+baseSlots,sampling.end(),sampler(draw.sampling[0],fallback));
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
            checkResult(device->CreateQuery(&desc,&query.get()),"occlusion query");
            if(diagnostics.enabled)++diagnostics.costs.queriesCreated;
            queries.push_back(std::move(query));context->Begin(queries.back());queryOpen=true;
        }
        if(fragmentCollectActive)bindFragmentConstants();
        context->Draw(UINT(count),UINT(vertexOffset/stride));vertexOffset+=bytes;bindingsDirty=false;
        if(diagnostics.enabled)++diagnostics.costs.draws;
        if(fragmentCollectActive)fragmentParameters.orderBase+=primitives;
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
        if(fragmentCollectActive)throw FlycastException("Hikaru DX11 visibility query during fragment collection");
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
        const auto waitStarted=diagnostics.enabled?DiagnosticClock::now():DiagnosticClock::time_point{};
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
        if(diagnostics.enabled)diagnostics.costs.queryWaitNanoseconds+=elapsedNanoseconds(waitStarted);
        return visible;
    }
    bool endOcclusion() override { finishOcclusion(); return occlusionResult(); }
    bool needsSceneDepth() const override { return owner&&owner->hikaruNeedsSceneDepth(); }
    void present(unsigned width,unsigned height) override {
        if(fragmentCollectActive)throw FlycastException("Hikaru DX11 presentation during fragment collection");
        completePixelCompilation(false);
        auto& output=image(0);
        if(output.description.Width!=width||output.description.Height!=height){finishGpuFrame(false);return;}
        if(!owner)throw FlycastException("Hikaru DX11 presenter is unavailable");
        ComPtr<ID3D11ShaderResourceView> depth;
        if(presentedDepthTexture) {
            auto& sceneDepth=image(presentedDepthTexture);
            if(sceneDepth.depth&&sceneDepth.description.Width==width&&sceneDepth.description.Height==height)
                depth=sceneDepth.view;
        }
        unbindInputs();unbindTargets();
        // Timestamp deltas are elapsed command intervals; close before Present to exclude display time.
        finishGpuFrame();owner->presentHikaruFrame(output.view,width,height,depth);bindingsDirty=true;
        pollGpuFrames();presentDiagnostics();
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
Dx11DiagnosticsSnapshot dx11DiagnosticsForTesting(const Backend& backend) {return static_cast<const Dx11Backend&>(backend).diagnosticsForTesting();}
void finishDx11DiagnosticFrameForTesting(Backend& backend) {static_cast<Dx11Backend&>(backend).finishDiagnosticFrameForTesting();}
void pollDx11DiagnosticFramesForTesting(Backend& backend) {static_cast<Dx11Backend&>(backend).pollDiagnosticFramesForTesting();}
std::array<uint64_t,3> dx11FragmentCountsForTesting(const Backend& backend) {return static_cast<const Dx11Backend&>(backend).fragmentCountsForTesting();}
std::array<uint64_t,4> dx11FragmentPoolForTesting(const Backend& backend) {return static_cast<const Dx11Backend&>(backend).fragmentPoolForTesting();}
void forceDx11GeneralFragmentResolveForTesting(Backend& backend) {static_cast<Dx11Backend&>(backend).forceGeneralFragmentResolveForTesting();}
void disableDx11FragmentListsForTesting(Backend& backend) {static_cast<Dx11Backend&>(backend).disableFragmentListsForTesting();}
#endif
} // namespace hikaru::graphics
#endif
