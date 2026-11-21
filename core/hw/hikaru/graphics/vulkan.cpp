// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include "shaders.h"
#include "fragment_list_shared.h"

#ifdef USE_VULKAN
#include "rend/vulkan/buffer.h"
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/vulkan_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>
#include <limits>

namespace hikaru::graphics {
namespace {
bool diagnosticFlagEnabled(const char* name) {
    const char* value=std::getenv(name);
    return value&&value[0]&&std::strcmp(value,"0")!=0;
}
vk::Format colorFormat(GLenum format) {
    switch (format) {
    case GL_RGB10_A2: return vk::Format::eA2B10G10R10UnormPack32;
    case GL_RG32UI: return vk::Format::eR32G32Uint;
    default: return vk::Format::eR8G8B8A8Unorm;
    }
}
vk::Format depthFormat(GLenum format, bool sampled = false) {
    auto physical = VulkanContext::Instance()->GetPhysicalDevice();
    const vk::FormatFeatureFlags required = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
            (sampled ? vk::FormatFeatureFlagBits::eSampledImage : vk::FormatFeatureFlags{});
    auto supported = [&](vk::Format candidate) {
        return (physical.getFormatProperties(candidate).optimalTilingFeatures & required) == required;
    };
    vk::Format requested = vk::Format::eUndefined;
    switch (format) {
    case GL_DEPTH_COMPONENT16: requested = vk::Format::eD16Unorm; break;
    case GL_DEPTH_COMPONENT24: requested = vk::Format::eD24UnormS8Uint; break;
    case GL_DEPTH24_STENCIL8: requested = vk::Format::eD24UnormS8Uint; break;
    case GL_DEPTH_COMPONENT32F: case GL_DEPTH_COMPONENT: requested = vk::Format::eD32Sfloat; break;
    default: requested = VulkanContext::Instance()->GetDepthFormat(); break;
    }
    if (supported(requested)) return requested;
    // Hikaru explicitly compares canonical D24 codes during transparency
    // peeling. D16 cannot preserve them, even when it is otherwise renderable.
    for (vk::Format candidate : {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint, vk::Format::eD16Unorm})
        if ((format == GL_DEPTH_COMPONENT16 || candidate != vk::Format::eD16Unorm) && supported(candidate)) return candidate;
    throw std::runtime_error("No supported Hikaru Vulkan depth format");
}
vk::ImageAspectFlags imageAspects(vk::Format format, bool depth) {
    if (!depth)
        return vk::ImageAspectFlagBits::eColor;
    auto aspects = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth);
    if (format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD32SfloatS8Uint)
        aspects |= vk::ImageAspectFlagBits::eStencil;
    return aspects;
}
vk::Filter filter(GLenum f) { return f == GL_LINEAR ? vk::Filter::eLinear : vk::Filter::eNearest; }
vk::SamplerAddressMode address(GLenum w) {
    if (w == GL_REPEAT) return vk::SamplerAddressMode::eRepeat;
    if (w == GL_MIRRORED_REPEAT) return vk::SamplerAddressMode::eMirroredRepeat;
    return vk::SamplerAddressMode::eClampToEdge;
}
vk::CompareOp compare(GLenum f) {
    switch (f) {
    case GL_NEVER: return vk::CompareOp::eNever; case GL_LESS: return vk::CompareOp::eLess;
    case GL_EQUAL: return vk::CompareOp::eEqual; case GL_LEQUAL: return vk::CompareOp::eLessOrEqual;
    case GL_GREATER: return vk::CompareOp::eGreater; case GL_NOTEQUAL: return vk::CompareOp::eNotEqual;
    case GL_GEQUAL: return vk::CompareOp::eGreaterOrEqual; default: return vk::CompareOp::eAlways;
    }
}
vk::BlendFactor blendFactor(GLenum f) {
    switch (f) {
    case GL_ZERO: return vk::BlendFactor::eZero; case GL_ONE: return vk::BlendFactor::eOne;
    case GL_SRC_COLOR: return vk::BlendFactor::eSrcColor; case GL_ONE_MINUS_SRC_COLOR: return vk::BlendFactor::eOneMinusSrcColor;
    case GL_DST_COLOR: return vk::BlendFactor::eDstColor; case GL_ONE_MINUS_DST_COLOR: return vk::BlendFactor::eOneMinusDstColor;
    case GL_SRC_ALPHA: return vk::BlendFactor::eSrcAlpha; case GL_ONE_MINUS_SRC_ALPHA: return vk::BlendFactor::eOneMinusSrcAlpha;
    case GL_DST_ALPHA: return vk::BlendFactor::eDstAlpha; case GL_ONE_MINUS_DST_ALPHA: return vk::BlendFactor::eOneMinusDstAlpha;
    default: return vk::BlendFactor::eOne;
    }
}
vk::BlendOp blendOp(GLenum op) {
    if (op == GL_FUNC_SUBTRACT) return vk::BlendOp::eSubtract;
    if (op == GL_FUNC_REVERSE_SUBTRACT) return vk::BlendOp::eReverseSubtract;
    if (op == GL_MIN) return vk::BlendOp::eMin;
    if (op == GL_MAX) return vk::BlendOp::eMax;
    return vk::BlendOp::eAdd;
}

class VulkanBackend final : public Backend {
    struct Image {
        // VMA allocation must outlive every Vulkan object bound to it. Members
        // are destroyed in reverse declaration order, so keep it first.
        Allocation allocation;
        vk::UniqueImage image;
        vk::UniqueImageView view;
        vk::UniqueImageView attachmentView;
        vk::Format format = vk::Format::eUndefined;
        vk::Extent2D extent{};
        unsigned levels = 1;
        std::vector<bool> initializedLevels;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        bool depth = false, sampleable = true;
        uint64_t revision = 0;
    };
    static constexpr unsigned BatchDraws = 256, BatchSlots = 8;
    // Four-component UNORM fields include the neighboring alpha/selector bytes.
    // Avoid optional three-component byte/word formats on mobile devices.
    static constexpr std::array<vk::Format,8> PackedFormats={
        vk::Format::eR32G32B32Sfloat,vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR32G32B32Sfloat,vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR32G32Sfloat,vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR16G16B16A16Unorm,vk::Format::eR32Sfloat};
    static constexpr vk::DeviceSize RetainedStreamBytes = 32 * 1024 * 1024;
    static constexpr vk::DeviceSize LiveStreamBytes = 64 * 1024 * 1024;
    struct StreamBuffer {
        BufferData storage;
        uint8_t* mapping;
        std::unordered_map<std::shared_ptr<const Geometry>,vk::DeviceSize> geometryOffsets;
        vk::DeviceSize used=0;
        uint64_t geometryFrame=0;
        void beginGeometryFrame(uint64_t frame) {
            if(geometryFrame==frame)return;
            geometryOffsets.clear();used=0;geometryFrame=frame;
        }
        StreamBuffer(vk::DeviceSize bytes, vk::BufferUsageFlags usage)
            : storage(bytes,usage), mapping(static_cast<uint8_t*>(storage.allocation.MapMemory(false))) {}
        ~StreamBuffer() { storage.allocation.UnmapMemory(false); }
        void flush(vk::DeviceSize used) { if(used)storage.allocation.FlushMemory(0,used); }
    };
    struct PendingBatch {
        vk::UniqueCommandBuffer command;
        vk::UniqueFence fence;
        std::unique_ptr<StreamBuffer> vertexBuffer, uniformBuffer, paletteBuffer;
        std::vector<Image> retiredImages;
        std::vector<vk::UniqueFramebuffer> retiredFramebuffers;
        std::vector<vk::UniquePipeline> retiredPipelines;
        std::vector<std::unique_ptr<BufferData>> transfers;
        // Raw descriptor sets are owned by this pool, which is reset only
        // after the submission fence. Never use UniqueDescriptorSet here.
        vk::UniqueDescriptorPool descriptors;
        unsigned drawCount = 0;
        uint64_t serial = 0;
        vk::DeviceSize streamBytes() const {
            return (vertexBuffer?vertexBuffer->storage.bufferSize:0)+
                (uniformBuffer?uniformBuffer->storage.bufferSize:0)+
                (paletteBuffer?paletteBuffer->storage.bufferSize:0);
        }
    };
    struct PipelineKey {
        bool bitmap, familyResolve, peelComposite, palette, packed, depthTest, depthWrite, blend, cull, depthBias;
        GLenum depthFunc, cullFace, frontFace, srcRGB, dstRGB, srcAlpha, dstAlpha, equationRGB, equationAlpha, topology;
        std::array<bool,4> colorMask;
        vk::Format color, depth, metadata;
        std::array<int32_t,MeshShaderSpecializationCount> shader{};
        bool fragmentCollect=false;
        DepthPass depthPass=DepthPass::None;
        auto values() const { return std::tie(bitmap,familyResolve,peelComposite,palette,packed,depthTest,depthWrite,blend,cull,depthBias,depthFunc,cullFace,frontFace,srcRGB,dstRGB,srcAlpha,dstAlpha,equationRGB,equationAlpha,topology,colorMask,color,depth,metadata,shader,fragmentCollect,depthPass); }
        bool operator==(const PipelineKey& b) const { return values()==b.values(); }
        bool operator<(const PipelineKey& b) const { return values()<b.values(); }
    };
    struct DescriptorKey {
        bool texturePalette=false;
        unsigned slots=1;
        std::array<vk::DescriptorImageInfo,DrawPaletteSize+3> images{};
        vk::DescriptorBufferInfo palette;
        bool operator==(const DescriptorKey& other) const {
            return texturePalette==other.texturePalette&&slots==other.slots&&palette==other.palette&&
                std::equal(images.begin(),images.begin()+slots+3,other.images.begin());
        }
    };
    struct CachedDescriptors {
        DescriptorKey key;
        vk::DescriptorSet set;
    };
    VulkanContext* ctx = VulkanContext::Instance();
    vk::Device device = ctx->GetDevice();
    vk::UniqueCommandPool commandPool;
    vk::UniqueDescriptorSetLayout descriptorLayout, paletteDescriptorLayout;
    vk::UniquePipelineLayout pipelineLayout, palettePipelineLayout;

    using SamplerKey=std::tuple<GLenum,GLenum,GLenum,GLenum,uint32_t>;
    std::map<SamplerKey,vk::UniqueSampler> samplerCache;
    vk::UniqueRenderPass colorOnlyPass;
    std::map<std::tuple<vk::Format,vk::Format,vk::Format>,vk::UniqueRenderPass> renderPasses;
    vk::UniqueShaderModule meshVertex, meshFragment, paletteVertex, paletteFragment, fixedTexturePaletteFragment, bitmapVertex, bitmapFragment, familyFragment, peelFragment;
    vk::UniqueShaderModule captureDepthFragment, composeDepthFragment;
    struct FragmentListStorage {
        std::unique_ptr<BufferData> heads,status,readback;
        std::array<std::unique_ptr<BufferData>,4> nodes;
        Image background;
        vk::UniqueDescriptorPool descriptors;
        vk::DescriptorSet set;
        uint32_t capacity=0;
    };
    std::unique_ptr<FragmentListStorage> fragmentLists;
    vk::UniqueDescriptorSetLayout fragmentListLayout;
    vk::UniquePipelineLayout fragmentPipelineLayout;
    vk::UniqueShaderModule fragmentCollectShader,fragmentResolveVertex,fragmentResolveShader;
    std::map<vk::Format,vk::UniquePipeline> fragmentResolvePipelines;
    struct FragmentGpuTimer {
        vk::UniqueQueryPool pool;
        uint32_t pending=0;
        uint64_t mask=0;
        double period=0;
        bool enabled=false,recording=false,claimed=false;
    } fragmentGpu;
    struct FrameGpuTimer {
        vk::UniqueQueryPool pool;
        std::array<uint64_t,4> pending{};
        std::array<bool,4> discarded{};
        int active=-1,ending=-1;
        bool attempted=false,requested=diagnosticFlagEnabled("HIKARU_TRACE_GPU_TIMING");
    } frameGpu;
    uint64_t submissionSerial=0,retiredSerial=0;
    HikaruFragmentListParameters fragmentParameters{};
    bool fragmentListSupported=false,fragmentCollectActive=false,fragmentListFailed=false;
    bool fragmentOverflowWarned=false,fragmentSmallList=true;
    unsigned fragmentColor=0;
    HikaruFragmentPoolPolicy fragmentPoolPolicy{};
    vk::Extent2D fragmentPoolExtent{};
    vk::Format fragmentPoolFormat=vk::Format::eUndefined;
    uint64_t fragmentPoolAllocations=0;

#ifdef HIKARU_NATIVE_TESTING
    std::array<uint64_t,3> fragmentListCounts{}; // collections, resolves, overflows
    enum class FragmentListInitFaultForTesting : unsigned {
        None=0,OutOfDeviceMemory=1,OutOfHostMemory=2,BadAlloc=3,InvalidShader=4,DeviceLost=5
    };
    FragmentListInitFaultForTesting fragmentListInitFaultForTesting=FragmentListInitFaultForTesting::None;
#endif
    struct CachedPipeline { vk::UniquePipeline pipeline; uint64_t touched; };
    std::map<PipelineKey,CachedPipeline> pipelines;
    decltype(pipelines)::iterator lastPipeline=pipelines.end();
#ifdef HIKARU_NATIVE_TESTING
    size_t pipelineCacheLimit=512;
#else
    static constexpr size_t pipelineCacheLimit=512;
#endif
    uint64_t pipelineClock = 0;
    vk::UniquePipelineLayout clearPipelineLayout;
    vk::UniqueShaderModule clearVertex;
    std::array<vk::UniqueShaderModule,2> clearFragments;
    std::map<std::tuple<vk::Format,vk::Format,unsigned>,vk::UniquePipeline> clearPipelines;
    unsigned textureSlots = 1;
    bool specializeShaders = true, packedVerticesSupported = false;
    vk::UniqueShaderModule packedMeshVertex,packedPaletteVertex;
    std::unordered_map<unsigned, Image> images;
    std::unordered_map<unsigned, Texture> imageInfo;
    std::map<std::tuple<unsigned,unsigned,unsigned>,vk::UniqueFramebuffer> framebuffers;
    std::deque<PendingBatch> pendingBatches;
    std::vector<PendingBatch> freeBatches;
    std::optional<PendingBatch> failedSubmission;
    vk::UniqueCommandBuffer activeCommand;
    vk::UniqueFence activeFence;
    vk::UniqueDescriptorPool activeDescriptors;
    // Sets are immutable after recording and live in the active batch's pool.
    // Its uniform buffer stays fixed, so only the dynamic offset changes per draw.
    // A small ring retains nearby materials without allocating CPU map nodes
    // for each batch. Eviction drops only the lookup; the pool owns every set.
    std::array<CachedDescriptors,8> materialDescriptors;
    size_t nextDescriptors=0;
    const CachedDescriptors* lastDescriptors=nullptr;
    std::unique_ptr<StreamBuffer> activeVertexBuffer,activeUniformBuffer,activePaletteBuffer;
    std::vector<std::unique_ptr<BufferData>> activeTransfers;
    std::vector<Image> retiredImages;
    std::vector<vk::UniqueFramebuffer> retiredFramebuffers;
    std::vector<vk::UniquePipeline> retiredPipelines;
    // Each stream retains its own immutable ranges within a frame. It is
    // reacquired only after its previous fence, and never borrowed by another
    // slot. Preserve its high-water mark until safe recycling or frame end.
    uint64_t geometryFrame=1;
#ifdef HIKARU_NATIVE_TESTING
    uint64_t vertexUploadCount=0,geometryReuseCount=0;
#endif
    vk::DeviceSize activeVertexOffset=0,activeUniformOffset=0,activePaletteOffset=0;
    unsigned activeDrawCount=0,activeColor=0,activeDepth=0,activeMetadata=0;
    // Reuse only the last immutable range in this batch. CPU snapshots avoid
    // reading write-combined mapped GPU memory just to compare payloads.
    Uniforms uploadedUniforms{};
    bool uniformUploadValid=false,paletteUploadValid=false;
    vk::DeviceSize uploadedUniformOffset=0;
    std::array<DrawRecord,DrawPaletteSize> uploadedPalette{};
    size_t uploadedPaletteSize=0;
    vk::DescriptorBufferInfo uploadedPaletteInfo;
    vk::Pipeline boundPipeline;
    vk::Buffer boundVertexBuffer;
    vk::DeviceSize boundVertexOffset=0;
    std::optional<std::array<float,6>> boundViewport;
    std::optional<vk::Rect2D> boundScissor;
    std::optional<std::array<float,2>> boundDepthBias;
    // Opt-in diagnostics distinguish CPU pipeline work from queue backpressure.
    struct Timings {
        bool enabled=diagnosticFlagEnabled("HIKARU_TRACE_TIMING")||diagnosticFlagEnabled("HIKARU_TRACE_GPU_TIMING");
        enum class WaitReason { Readback, FragmentStartup, FragmentStorage, FragmentCounter, StreamBudget, BatchSlot, Count };
        std::array<double,size_t(WaitReason::Count)> waitReasonsMs{};
        std::array<uint64_t,size_t(WaitReason::Count)> waitCounts{};
        uint64_t fragmentReuses=0;
        unsigned frames=0,draws=0,palettes=0,pipelines=0,passes=0,batches=0,queries=0,uploads=0;
        unsigned descriptorSets=0,descriptorHits=0,packedDraws=0,vertexUploads=0,geometryHits=0;
        uint64_t vertexBytes=0,listFragments=0,textureUploads=0,textureBytes=0;
        uint64_t uniformUploads=0,uniformHits=0,uniformBytes=0,paletteUploads=0,paletteHits=0,paletteBytes=0;
        uint64_t pipelineLookups=0,pipelineFastHits=0,maskedClears=0,maskedPixels=0;
        unsigned listResolves=0,listOverflows=0,listCapacity=0,listPeakFragments=0;
        uint64_t listPoolBytes=0,poolAllocations=0;
        double waitMs=0,pipelineMs=0,submitMs=0,descriptorMs=0;
        bool gpuTiming=false;
        std::array<double,4> gpuMs{};
        std::array<uint64_t,4> gpuSamples{};
        double frameGpuMs=0;
        uint64_t frameGpuSamples=0,frameGpuDropped=0,frameGpuInvalid=0;
        struct Span {
            double& total; bool enabled;
            std::chrono::steady_clock::time_point start;
            Span(double& value,bool trace):total(value),enabled(trace),start(trace?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}){}
            void stop(){if(enabled){total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();enabled=false;}}
            ~Span(){stop();}
        };
        void present() {
            if(!enabled||++frames<60)return;
            std::fprintf(stderr,"Hikaru Vulkan timing: draws=%u palettes=%u pipelines=%u passes=%u batches=%u queries=%u uploads=%u wait_ms=%.3f pipeline_ms=%.3f submit_ms=%.3f descriptor_ms=%.3f descriptor_sets=%u descriptor_hits=%u packed_draws=%u vertex_uploads=%u geometry_hits=%u vertex_bytes=%llu\n",draws,palettes,pipelines,passes,batches,queries,uploads,waitMs/frames,pipelineMs/frames,submitMs/frames,descriptorMs/frames,descriptorSets,descriptorHits,packedDraws,vertexUploads,geometryHits,static_cast<unsigned long long>(vertexBytes));
            std::fprintf(stderr,"Hikaru Vulkan fragment lists: resolves=%u overflows=%u fragments=%llu capacity=%u peak_used=%u pool_bytes=%llu allocations=%llu\n",listResolves,listOverflows,static_cast<unsigned long long>(listFragments),listCapacity,listPeakFragments,static_cast<unsigned long long>(listPoolBytes),static_cast<unsigned long long>(poolAllocations));
            if(gpuTiming) {
                // Query slots can still be in flight across report windows.
                // Average only completed samples; missing data is not zero GPU time.
                constexpr const char* names[]={"clear","collect","copy","resolve"};
                std::fprintf(stderr,"Hikaru Vulkan fragment GPU timing:");
                for(unsigned phase=0;phase<gpuSamples.size();++phase) {
                    if(gpuSamples[phase])std::fprintf(stderr," %s_ms=%.3f",names[phase],gpuMs[phase]/gpuSamples[phase]);
                    else std::fprintf(stderr," %s_ms=unavailable",names[phase]);
                    std::fprintf(stderr," %s_samples=%llu",names[phase],static_cast<unsigned long long>(gpuSamples[phase]));
                }
                std::fputc('\n',stderr);
            }
            std::fprintf(stderr,"Hikaru Vulkan waits: readback_ms=%.3f startup_ms=%.3f storage_ms=%.3f counter_ms=%.3f stream_ms=%.3f slots_ms=%.3f readback_waits=%llu startup_waits=%llu storage_waits=%llu counter_waits=%llu stream_waits=%llu slots_waits=%llu reused_collections=%llu\n",
                waitReasonsMs[0]/frames,waitReasonsMs[1]/frames,waitReasonsMs[2]/frames,waitReasonsMs[3]/frames,waitReasonsMs[4]/frames,waitReasonsMs[5]/frames,
                static_cast<unsigned long long>(waitCounts[0]),static_cast<unsigned long long>(waitCounts[1]),static_cast<unsigned long long>(waitCounts[2]),
                static_cast<unsigned long long>(waitCounts[3]),static_cast<unsigned long long>(waitCounts[4]),static_cast<unsigned long long>(waitCounts[5]),
                static_cast<unsigned long long>(fragmentReuses));
            std::fprintf(stderr,"Hikaru Vulkan reuse: uniform_uploads=%llu uniform_hits=%llu uniform_bytes=%llu palette_uploads=%llu palette_hits=%llu palette_bytes=%llu pipeline_lookups=%llu pipeline_fast_hits=%llu masked_clears=%llu masked_pixels=%llu\n",
                static_cast<unsigned long long>(uniformUploads),static_cast<unsigned long long>(uniformHits),static_cast<unsigned long long>(uniformBytes),
                static_cast<unsigned long long>(paletteUploads),static_cast<unsigned long long>(paletteHits),static_cast<unsigned long long>(paletteBytes),
                static_cast<unsigned long long>(pipelineLookups),static_cast<unsigned long long>(pipelineFastHits),
                static_cast<unsigned long long>(maskedClears),static_cast<unsigned long long>(maskedPixels));
            std::fprintf(stderr,"Hikaru Vulkan costs: texture_uploads=%llu texture_bytes=%llu\n",
                static_cast<unsigned long long>(textureUploads),static_cast<unsigned long long>(textureBytes));
            uniformUploads=uniformHits=uniformBytes=paletteUploads=paletteHits=paletteBytes=0;
            pipelineLookups=pipelineFastHits=maskedClears=maskedPixels=0;
            if(gpuTiming) {
                std::fprintf(stderr,"Hikaru Vulkan GPU timing: frame_ms=");
                if(frameGpuSamples)std::fprintf(stderr,"%.3f",frameGpuMs/frameGpuSamples);
                else std::fputs("unavailable",stderr);
                std::fprintf(stderr," frame_samples=%llu dropped=%llu invalid=%llu\n",
                    static_cast<unsigned long long>(frameGpuSamples),static_cast<unsigned long long>(frameGpuDropped),
                    static_cast<unsigned long long>(frameGpuInvalid));
            }
            frameGpuMs=0;frameGpuSamples=frameGpuDropped=frameGpuInvalid=0;
            gpuMs={};gpuSamples={};waitReasonsMs={};waitCounts={};fragmentReuses=0;
            listResolves=listOverflows=listPeakFragments=0;listFragments=0;
            frames=draws=palettes=pipelines=passes=batches=queries=uploads=descriptorSets=descriptorHits=packedDraws=vertexUploads=geometryHits=0;
            vertexBytes=textureUploads=textureBytes=0;waitMs=pipelineMs=submitMs=descriptorMs=0;
        }
    } timing;
    bool renderPassOpen=false;
    bool occlusionActive=false; bool occlusionQueryOpen=false; vk::QueryPool occlusionQuery;
    std::vector<vk::UniqueQueryPool> occlusionQueries;
    std::deque<std::vector<vk::UniqueQueryPool>> pendingOcclusions;

    vk::DeviceSize liveStreamBytes() const {
        vk::DeviceSize bytes=(activeVertexBuffer?activeVertexBuffer->storage.bufferSize:0)+
            (activeUniformBuffer?activeUniformBuffer->storage.bufferSize:0)+
            (activePaletteBuffer?activePaletteBuffer->storage.bufferSize:0);
        for(const auto& batch:pendingBatches)bytes+=batch.streamBytes();
        for(const auto& batch:freeBatches)bytes+=batch.streamBytes();
        return bytes;
    }
    void allocateStream(std::unique_ptr<StreamBuffer>& target,vk::DeviceSize bytes,vk::BufferUsageFlags usage) {
        if(target&&target->storage.bufferSize>=bytes)return;
        if(&target==&activeUniformBuffer)uniformUploadValid=false;
        if(&target==&activePaletteBuffer)paletteUploadValid=false;
        target.reset(); // only unused storage from a fence-retired slot can grow
        while(liveStreamBytes()+bytes>LiveStreamBytes) {
            for(auto& batch:freeBatches){batch.vertexBuffer.reset();batch.uniformBuffer.reset();batch.paletteBuffer.reset();}
            if(liveStreamBytes()+bytes<=LiveStreamBytes||pendingBatches.empty())break;
            waitForBatch(pendingBatches.front(),Timings::WaitReason::StreamBudget);
            reapCompletedDraws();
        }
        // A single oversized guest draw is still rendered, after other batches
        // drain. It must not be dropped or split into a different topology.
        target=std::make_unique<StreamBuffer>(bytes,usage);
    }
    void reapCompletedDraws() {
        while(!pendingBatches.empty()) {
            const auto status=device.getFenceStatus(*pendingBatches.front().fence);
            if(status==vk::Result::eNotReady)break;
            if(status!=vk::Result::eSuccess)throw std::runtime_error("Failed querying Hikaru Vulkan draw fence");
            auto batch=std::move(pendingBatches.front());pendingBatches.pop_front();
            retiredSerial=std::max(retiredSerial,batch.serial);
            // The fence covers all commands that reference these resources.
            if(batch.descriptors)device.resetDescriptorPool(*batch.descriptors);
            batch.retiredFramebuffers.clear();batch.retiredPipelines.clear();batch.retiredImages.clear();batch.transfers.clear();
            batch.drawCount=0;
            vk::DeviceSize retained=batch.streamBytes();
            for(const auto& free:freeBatches)retained+=free.streamBytes();
            if(retained>RetainedStreamBytes){batch.vertexBuffer.reset();batch.uniformBuffer.reset();batch.paletteBuffer.reset();}
            freeBatches.push_back(std::move(batch));
        }
    }
    void waitForBatch(const PendingBatch& batch,Timings::WaitReason reason) {
        // Only fence latency belongs here; submission and resource recycling
        // have their own costs and must not be attributed to a GPU wait.
        Timings::Span measured(timing.waitMs,timing.enabled);
        const auto index=size_t(reason);
        Timings::Span classified(timing.waitReasonsMs[index],timing.enabled);
        if(timing.enabled)++timing.waitCounts[index];
        if(device.waitForFences(*batch.fence,true,UINT64_MAX)!=vk::Result::eSuccess)
            throw std::runtime_error("Failed waiting for Hikaru Vulkan submission");
    }
    void waitForDraws(Timings::WaitReason reason=Timings::WaitReason::Readback) {
        flushBatch();
        if(pendingBatches.empty())return;
        waitForBatch(pendingBatches.back(),reason);
        reapCompletedDraws();
    }
    vk::CommandBuffer startBatch() {
        if(failedSubmission)throw std::runtime_error("Hikaru Vulkan submission has failed");
        if(activeCommand)return *activeCommand;
        reapCompletedDraws();
        if(freeBatches.empty()&&pendingBatches.size()>=BatchSlots) {
            waitForBatch(pendingBatches.front(),Timings::WaitReason::BatchSlot);
            reapCompletedDraws();
        }
        if(!freeBatches.empty()) {
            auto batch=std::move(freeBatches.back());freeBatches.pop_back();
            activeCommand=std::move(batch.command);activeFence=std::move(batch.fence);
            activeVertexBuffer=std::move(batch.vertexBuffer);activeUniformBuffer=std::move(batch.uniformBuffer);
            activePaletteBuffer=std::move(batch.paletteBuffer);activeDescriptors=std::move(batch.descriptors);
            activeCommand->reset();
        } else {
            auto commands=device.allocateCommandBuffersUnique({*commandPool,vk::CommandBufferLevel::ePrimary,1});
            activeCommand=std::move(commands.front());activeFence=device.createFenceUnique({});
        }
        if(activeVertexBuffer) {
            activeVertexBuffer->beginGeometryFrame(geometryFrame);
            activeVertexOffset=activeVertexBuffer->used;
        }
        for(auto& entry:materialDescriptors)entry.set=nullptr;
        nextDescriptors=0;lastDescriptors=nullptr;
        uniformUploadValid=paletteUploadValid=false;
        boundPipeline=nullptr;boundVertexBuffer=nullptr;boundViewport.reset();boundScissor.reset();boundDepthBias.reset();
        auto cmd=*activeCommand;
        cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        return cmd;
    }
    void endRenderPass(vk::CommandBuffer cmd) {
        if(!renderPassOpen)return;
        if(occlusionQueryOpen){occlusionQueryOpen=false;cmd.endQuery(occlusionQuery,0);}
        cmd.endRenderPass();
        renderPassOpen=false;
        activeColor=activeDepth=activeMetadata=0;
    }
    void flushBatch() {
        if(!activeCommand)return;
        auto cmd=*activeCommand;
        endRenderPass(cmd);
        cmd.end();
        PendingBatch batch;
        batch.command=std::move(activeCommand);
        batch.vertexBuffer=std::move(activeVertexBuffer);batch.uniformBuffer=std::move(activeUniformBuffer);
        batch.paletteBuffer=std::move(activePaletteBuffer);batch.descriptors=std::move(activeDescriptors);
        if(batch.vertexBuffer)batch.vertexBuffer->flush(activeVertexOffset);
        if(batch.uniformBuffer)batch.uniformBuffer->flush(activeUniformOffset);
        if(batch.paletteBuffer)batch.paletteBuffer->flush(activePaletteOffset);
        batch.drawCount=activeDrawCount;batch.transfers=std::move(activeTransfers);
        batch.retiredImages=std::move(retiredImages);batch.retiredFramebuffers=std::move(retiredFramebuffers);
        batch.retiredPipelines=std::move(retiredPipelines);batch.fence=std::move(activeFence);
        activeDrawCount=0;activeVertexOffset=activeUniformOffset=activePaletteOffset=0;
        device.resetFences(*batch.fence);
        pendingBatches.push_back(std::move(batch));
        try {
            auto& submitted=pendingBatches.back();
            Timings::Span measured(timing.submitMs,timing.enabled);
            ctx->SubmitCommandBuffers({*submitted.command},*submitted.fence);
            submitted.serial=++submissionSerial;
            if(frameGpu.ending>=0) {
                frameGpu.pending[size_t(frameGpu.ending)]=submitted.serial;
                frameGpu.active=frameGpu.ending=-1;
            }
            if(timing.enabled)++timing.batches;
        } catch (...) {
            // Its retirement lists may still protect earlier queue work. Keep
            // them alive until shutdown drains that work; this fence was never
            // submitted and must not enter the normal wait/recycle queue.
            failedSubmission.emplace(std::move(pendingBatches.back()));
            pendingBatches.pop_back();
            throw;
        }
    }
    vk::Framebuffer framebuffer(unsigned colorId,unsigned depthId,Image& color,Image* depth,vk::RenderPass pass,unsigned metadataId=0,Image* metadata=nullptr) {
        auto key=std::make_tuple(colorId,depthId,metadataId);
        auto it=framebuffers.find(key);
        if(it!=framebuffers.end())return *it->second;
        std::array<vk::ImageView,3> views{};
        unsigned count=0;views[count++]=*color.attachmentView;
        if(metadata)views[count++]=*metadata->attachmentView;
        if(depth)views[count++]=*depth->attachmentView;
        vk::FramebufferCreateInfo fi({},pass,count,views.data(),color.extent.width,color.extent.height,1);
        return *framebuffers.emplace(key,device.createFramebufferUnique(fi)).first->second;
    }
    vk::Sampler sampler(const Texture& sampling,const Image& image,bool mipmapped) {
        const uint32_t maxLod=mipmapped?std::min(image.levels-1,sampling.maxLevel):0;
        SamplerKey key{sampling.minFilter,sampling.magFilter,sampling.wrapS,sampling.wrapT,maxLod};
        auto it=samplerCache.find(key);
        if(it!=samplerCache.end())return *it->second;
        const GLenum mf=sampling.minFilter;
        const bool nearest=mf==GL_NEAREST||mf==GL_NEAREST_MIPMAP_NEAREST||mf==GL_NEAREST_MIPMAP_LINEAR;
        vk::SamplerCreateInfo si{};
        si.magFilter=filter(sampling.magFilter);
        si.minFilter=nearest?vk::Filter::eNearest:vk::Filter::eLinear;
        si.mipmapMode=(mf==GL_LINEAR_MIPMAP_LINEAR||mf==GL_NEAREST_MIPMAP_LINEAR)?vk::SamplerMipmapMode::eLinear:vk::SamplerMipmapMode::eNearest;
        si.addressModeU=address(sampling.wrapS);si.addressModeV=address(sampling.wrapT);si.addressModeW=vk::SamplerAddressMode::eClampToEdge;
        si.mipLodBias=0.f;si.maxLod=float(maxLod);
        return *samplerCache.emplace(key,device.createSamplerUnique(si)).first->second;
    }

    void immediate(const std::function<void(vk::CommandBuffer)>& record) {
        auto cmd=startBatch();
        endRenderPass(cmd);
        record(cmd);
        waitForDraws(); // CPU readbacks and initialization only; never device-wide.
    }
    void retireImage(unsigned id) {
        auto it=images.find(id);
        if(it==images.end())return;
        flushBatch();
        // Presentation submits on this same queue. A following marker fences
        // its sampling too, rather than just the last off-screen draw.
        if(id==0)ctx->PresentFrame({}, {}, {}, 1.f);
        startBatch();
        for(auto fb=framebuffers.begin();fb!=framebuffers.end();) {
            if(std::get<0>(fb->first)==id||std::get<1>(fb->first)==id||std::get<2>(fb->first)==id) {
                retiredFramebuffers.push_back(std::move(fb->second));
                fb=framebuffers.erase(fb);
            } else ++fb;
        }
        retiredImages.push_back(std::move(it->second));
        images.erase(it);
        flushBatch();
    }

    void barrier(vk::CommandBuffer cmd, Image& image, vk::ImageLayout next) {
        if (image.layout == next) return;
        vk::AccessFlags src; vk::PipelineStageFlags srcStage;
        switch (image.layout) {
        case vk::ImageLayout::eUndefined: src = {}; srcStage = vk::PipelineStageFlagBits::eTopOfPipe; break;
        case vk::ImageLayout::eTransferDstOptimal: src = vk::AccessFlagBits::eTransferWrite; srcStage = vk::PipelineStageFlagBits::eTransfer; break;
        case vk::ImageLayout::eTransferSrcOptimal: src = vk::AccessFlagBits::eTransferRead; srcStage = vk::PipelineStageFlagBits::eTransfer; break;
        case vk::ImageLayout::eColorAttachmentOptimal: src = vk::AccessFlagBits::eColorAttachmentWrite; srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput; break;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal: src = vk::AccessFlagBits::eDepthStencilAttachmentRead|vk::AccessFlagBits::eDepthStencilAttachmentWrite; srcStage = vk::PipelineStageFlagBits::eEarlyFragmentTests|vk::PipelineStageFlagBits::eLateFragmentTests; break;
        default: src = vk::AccessFlagBits::eShaderRead; srcStage = vk::PipelineStageFlagBits::eFragmentShader; break;
        }
        vk::AccessFlags dst; vk::PipelineStageFlags dstStage;
        switch (next) {
        case vk::ImageLayout::eTransferDstOptimal: dst=vk::AccessFlagBits::eTransferWrite; dstStage=vk::PipelineStageFlagBits::eTransfer; break;
        case vk::ImageLayout::eTransferSrcOptimal: dst=vk::AccessFlagBits::eTransferRead; dstStage=vk::PipelineStageFlagBits::eTransfer; break;
        case vk::ImageLayout::eColorAttachmentOptimal: dst=vk::AccessFlagBits::eColorAttachmentRead|vk::AccessFlagBits::eColorAttachmentWrite; dstStage=vk::PipelineStageFlagBits::eColorAttachmentOutput; break;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal: dst=vk::AccessFlagBits::eDepthStencilAttachmentRead|vk::AccessFlagBits::eDepthStencilAttachmentWrite; dstStage=vk::PipelineStageFlagBits::eEarlyFragmentTests|vk::PipelineStageFlagBits::eLateFragmentTests; break;
        default: dst=vk::AccessFlagBits::eShaderRead; dstStage=vk::PipelineStageFlagBits::eFragmentShader; break;
        }
        if (next == vk::ImageLayout::eDepthStencilReadOnlyOptimal) {
            dst = vk::AccessFlagBits::eShaderRead;
            dstStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        if (image.layout == vk::ImageLayout::eDepthStencilReadOnlyOptimal) {
            src = vk::AccessFlagBits::eShaderRead;
            srcStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        const auto aspect = imageAspects(image.format, image.depth);
        vk::ImageMemoryBarrier b(src,dst,image.layout,next,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,*image.image,{aspect,0,image.levels,0,1});
        cmd.pipelineBarrier(srcStage,dstStage,{},nullptr,nullptr,b);
        image.layout=next;
    }
    Image makeImage(unsigned w,unsigned h,vk::Format fmt,unsigned levels,bool depth,bool target,bool sampledDepth=false) {
        if (!w || !h) throw std::runtime_error("Invalid Hikaru Vulkan image size");
        Image out; out.extent=vk::Extent2D(w,h); out.format=fmt; out.levels=std::max(1u,levels); out.depth=depth; out.sampleable=!depth||sampledDepth;
        out.initializedLevels.resize(out.levels,false);
        vk::ImageUsageFlags usage = depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment|vk::ImageUsageFlagBits::eTransferDst : vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc;
        if (target && !depth) usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (depth && sampledDepth) usage |= vk::ImageUsageFlagBits::eSampled;
        vk::ImageCreateInfo ci({},vk::ImageType::e2D,fmt,{w,h,1},out.levels,1,vk::SampleCountFlagBits::e1,vk::ImageTiling::eOptimal,usage,vk::SharingMode::eExclusive);
        out.image=device.createImageUnique(ci);
        VmaAllocationCreateInfo alloc{}; alloc.requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        out.allocation=ctx->GetAllocator().AllocateForImage(*out.image,alloc);
        auto aspect=depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor;
        out.view=device.createImageViewUnique({{},*out.image,vk::ImageViewType::e2D,fmt,{}, {aspect,0,out.levels,0,1}});
        out.attachmentView=device.createImageViewUnique({{},*out.image,vk::ImageViewType::e2D,fmt,{}, {aspect,0,1,0,1}});
        return out;
    }
    Image& image(unsigned id) {
        auto it=images.find(id); if(it==images.end()) throw std::runtime_error("Unknown Hikaru Vulkan image"); return it->second;
    }
    vk::UniqueRenderPass createPass(vk::Format color,vk::Format depth,bool withDepth,vk::Format metadata=vk::Format::eUndefined) {
        std::array<vk::AttachmentDescription,3> attachments{};
        std::array<vk::AttachmentReference,2> colors{};
        unsigned count=0,colorCount=0;
        for(auto format:{color,metadata}) {
            if(format==vk::Format::eUndefined)continue;
            auto& attachment=attachments[count];
            attachment.format=format;attachment.samples=vk::SampleCountFlagBits::e1;
            attachment.loadOp=vk::AttachmentLoadOp::eLoad;attachment.storeOp=vk::AttachmentStoreOp::eStore;
            attachment.stencilLoadOp=vk::AttachmentLoadOp::eDontCare;attachment.stencilStoreOp=vk::AttachmentStoreOp::eDontCare;
            attachment.initialLayout=attachment.finalLayout=vk::ImageLayout::eColorAttachmentOptimal;
            colors[colorCount++]=vk::AttachmentReference(count++,vk::ImageLayout::eColorAttachmentOptimal);
        }
        vk::AttachmentReference depthReference(count,vk::ImageLayout::eDepthStencilAttachmentOptimal);
        if(withDepth) {
            auto& attachment=attachments[count++];attachment.format=depth;attachment.samples=vk::SampleCountFlagBits::e1;
            attachment.loadOp=vk::AttachmentLoadOp::eLoad;attachment.storeOp=vk::AttachmentStoreOp::eStore;
            attachment.stencilLoadOp=vk::AttachmentLoadOp::eDontCare;attachment.stencilStoreOp=vk::AttachmentStoreOp::eDontCare;
            attachment.initialLayout=attachment.finalLayout=vk::ImageLayout::eDepthStencilAttachmentOptimal;
        }
        vk::SubpassDescription sub({},vk::PipelineBindPoint::eGraphics,0,nullptr,colorCount,colors.data(),nullptr,withDepth?&depthReference:nullptr);
        // Consecutive family and resolve passes share images on this queue.
        auto stages=vk::PipelineStageFlagBits::eColorAttachmentOutput|vk::PipelineStageFlagBits::eEarlyFragmentTests|vk::PipelineStageFlagBits::eLateFragmentTests;
        auto access=vk::AccessFlagBits::eColorAttachmentRead|vk::AccessFlagBits::eColorAttachmentWrite|vk::AccessFlagBits::eDepthStencilAttachmentRead|vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        vk::SubpassDependency dependency(VK_SUBPASS_EXTERNAL,0,stages,stages,access,access,vk::DependencyFlagBits::eByRegion);
        vk::RenderPassCreateInfo info({},count,attachments.data(),1,&sub,1,&dependency);
        return device.createRenderPassUnique(info);
    }
    vk::RenderPass renderPass(vk::Format color,vk::Format depth,vk::Format metadata=vk::Format::eUndefined) {
        auto key=std::make_tuple(color,depth,metadata);
        auto it=renderPasses.find(key);
        if(it==renderPasses.end())it=renderPasses.emplace(key,createPass(color,depth,depth!=vk::Format::eUndefined,metadata)).first;
        return *it->second;
    }
    vk::Pipeline pipeline(const PipelineKey& k) {
        if(lastPipeline!=pipelines.end()&&lastPipeline->first==k) {
            lastPipeline->second.touched=++pipelineClock;
            if(timing.enabled)++timing.pipelineFastHits;
            return *lastPipeline->second.pipeline;
        }
        if(timing.enabled)++timing.pipelineLookups;
        auto found=pipelines.find(k);
        if(found!=pipelines.end()){lastPipeline=found;found->second.touched=++pipelineClock;return *found->second.pipeline;}
        auto pass=renderPass(k.color,k.depth,k.metadata);
        vk::ShaderModule depthFragment;
        if (k.depthPass != DepthPass::None) {
            const bool capture = k.depthPass == DepthPass::Capture;
            auto& cached = capture ? captureDepthFragment : composeDepthFragment;
            if (!cached)
                cached = ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,
                    vulkanFragmentSource(true, capture, false, 1, false, false, k.depthPass));
            depthFragment = *cached;
        }
        vk::PipelineShaderStageCreateInfo stages[2]={{ {},vk::ShaderStageFlagBits::eVertex,k.bitmap?*bitmapVertex:k.packed?(k.palette?*packedPaletteVertex:*packedMeshVertex):k.palette?*paletteVertex:*meshVertex,"main" },{{},vk::ShaderStageFlagBits::eFragment,depthFragment?depthFragment:k.fragmentCollect?*fragmentCollectShader:k.familyResolve?*familyFragment:k.peelComposite?*peelFragment:k.bitmap?*bitmapFragment:k.palette?(k.shader[8]?*paletteFragment:*fixedTexturePaletteFragment):*meshFragment,"main"}};
        std::array<vk::SpecializationMapEntry,MeshShaderSpecializationCount> entries{};
        for(unsigned i=0;i<entries.size();++i)entries[i]=vk::SpecializationMapEntry(i,uint32_t(i*sizeof(int32_t)),sizeof(int32_t));
        vk::SpecializationInfo specialization(entries.size(),entries.data(),sizeof(k.shader),k.shader.data());
        if(!k.bitmap)stages[1].pSpecializationInfo=&specialization;
        vk::VertexInputBindingDescription binding(0,k.packed?sizeof(PackedVertex):sizeof(Vertex),vk::VertexInputRate::eVertex);
        std::array<vk::VertexInputAttributeDescription,8> attrs;
        if(k.packed) {
            constexpr uint32_t offsets[]={offsetof(PackedVertex,position),offsetof(PackedVertex,diffuse),
                offsetof(PackedVertex,normal),offsetof(PackedVertex,ambient),offsetof(PackedVertex,uv),
                offsetof(PackedVertex,specular),offsetof(PackedVertex,unknown),offsetof(PackedVertex,drawIndex)};
            for(uint32_t i=0;i<attrs.size();++i)attrs[i]={i,0,PackedFormats[i],offsets[i]};
        } else for(uint32_t i=0;i<attrs.size();++i)attrs[i]={i,0,vk::Format::eR32G32B32A32Sfloat,i*16};
        vk::PipelineVertexInputStateCreateInfo vi({},1,&binding,attrs.size(),attrs.data());
        vk::PrimitiveTopology topology=k.topology==GL_TRIANGLE_STRIP?vk::PrimitiveTopology::eTriangleStrip:k.topology==GL_LINES?vk::PrimitiveTopology::eLineList:k.topology==GL_POINTS?vk::PrimitiveTopology::ePointList:vk::PrimitiveTopology::eTriangleList;
        vk::PipelineInputAssemblyStateCreateInfo ia({},topology,false);
        vk::PipelineViewportStateCreateInfo vp({},1,nullptr,1,nullptr);
        const auto cullMode=!k.cull?vk::CullModeFlags{}:(k.cullFace==GL_FRONT?vk::CullModeFlags(vk::CullModeFlagBits::eFront):k.cullFace==GL_FRONT_AND_BACK?vk::CullModeFlags(vk::CullModeFlagBits::eFrontAndBack):vk::CullModeFlags(vk::CullModeFlagBits::eBack));
        vk::PipelineRasterizationStateCreateInfo rs({},false,false,vk::PolygonMode::eFill,cullMode,k.frontFace==GL_CW?vk::FrontFace::eClockwise:vk::FrontFace::eCounterClockwise,k.depthBias,0,0,0,1);
        vk::PipelineMultisampleStateCreateInfo ms({},vk::SampleCountFlagBits::e1);
        vk::PipelineDepthStencilStateCreateInfo ds({},k.depthTest,k.depthWrite,k.depthTest?compare(k.depthFunc):vk::CompareOp::eAlways,false,false,{}, {},0.f,1.f);
        vk::PipelineColorBlendAttachmentState ba{}; ba.blendEnable=k.blend; ba.srcColorBlendFactor=blendFactor(k.srcRGB);ba.dstColorBlendFactor=blendFactor(k.dstRGB);ba.colorBlendOp=blendOp(k.equationRGB);ba.srcAlphaBlendFactor=blendFactor(k.srcAlpha);ba.dstAlphaBlendFactor=blendFactor(k.dstAlpha);ba.alphaBlendOp=blendOp(k.equationAlpha);ba.colorWriteMask={};for(int i=0;i<4;i++)if(k.colorMask[i])ba.colorWriteMask|=static_cast<vk::ColorComponentFlagBits>(1u<<i);
        std::array<vk::PipelineColorBlendAttachmentState,2> blends{ba,ba};
        // The metadata attachment is used only by unblended depth writers.
        vk::PipelineColorBlendStateCreateInfo cb({},false,vk::LogicOp::eCopy,k.metadata==vk::Format::eUndefined?1:2,blends.data());
        vk::DynamicState dyns[]={vk::DynamicState::eViewport,vk::DynamicState::eScissor,vk::DynamicState::eDepthBias}; vk::PipelineDynamicStateCreateInfo dyn({},3,dyns);
        vk::GraphicsPipelineCreateInfo ci({},2,stages,&vi,&ia,nullptr,&vp,&rs,&ms,&ds,&cb,&dyn,k.fragmentCollect?*fragmentPipelineLayout:k.shader[8]?*palettePipelineLayout:*pipelineLayout,pass);
        Timings::Span measured(timing.pipelineMs,timing.enabled);
        if(timing.enabled)++timing.pipelines;
        auto result=device.createGraphicsPipelineUnique(ctx->GetPipelineCache(),ci);
        // Evicted pipelines can still be referenced by recorded or submitted
        // draws. Retire them on this queue before reclaiming their handles.
        if(pipelines.size()>=pipelineCacheLimit) {
            auto oldest=std::min_element(pipelines.begin(),pipelines.end(),[](const auto& a,const auto& b){return a.second.touched<b.second.touched;});
            if(lastPipeline==oldest)lastPipeline=pipelines.end();
            retiredPipelines.push_back(std::move(oldest->second.pipeline));pipelines.erase(oldest);
        }
        lastPipeline=pipelines.emplace(k,CachedPipeline{std::move(result.value),++pipelineClock}).first;
        return *lastPipeline->second.pipeline;
    }
    vk::Pipeline maskedClearPipeline(vk::Format format,vk::Format depthFormat,unsigned channels) {
        const auto key=std::make_tuple(format,depthFormat,channels);
        auto found=clearPipelines.find(key);
        if(found!=clearPipelines.end())return *found->second;
        if(!clearPipelineLayout) {
            const vk::PushConstantRange color(vk::ShaderStageFlagBits::eFragment,0,sizeof(Vec4));
            clearPipelineLayout=device.createPipelineLayoutUnique({{},0,nullptr,1,&color});
            clearVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanFullscreenVertexSource());
        }
        const unsigned integer=format==vk::Format::eR32G32Uint?1:0;
        if(!clearFragments[integer])
            clearFragments[integer]=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanClearFragmentSource(integer!=0));
        const std::array<vk::PipelineShaderStageCreateInfo,2> stages={{
            {{},vk::ShaderStageFlagBits::eVertex,*clearVertex,"main"},
            {{},vk::ShaderStageFlagBits::eFragment,*clearFragments[integer],"main"}}};
        const vk::PipelineVertexInputStateCreateInfo vertices;
        const vk::PipelineInputAssemblyStateCreateInfo assembly({},vk::PrimitiveTopology::eTriangleList,false);
        const vk::PipelineViewportStateCreateInfo viewport({},1,nullptr,1,nullptr);
        const vk::PipelineRasterizationStateCreateInfo raster({},false,false,vk::PolygonMode::eFill,
            vk::CullModeFlagBits::eNone,vk::FrontFace::eCounterClockwise,false,0,0,0,1);
        const vk::PipelineMultisampleStateCreateInfo samples({},vk::SampleCountFlagBits::e1);
        const vk::PipelineDepthStencilStateCreateInfo depth;
        vk::PipelineColorBlendAttachmentState attachment;
        attachment.colorWriteMask=vk::ColorComponentFlags(channels);
        const vk::PipelineColorBlendStateCreateInfo blending({},false,vk::LogicOp::eCopy,1,&attachment);
        const std::array<vk::DynamicState,2> states={vk::DynamicState::eViewport,vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic({},states.size(),states.data());
        const vk::GraphicsPipelineCreateInfo info({},stages.size(),stages.data(),&vertices,&assembly,nullptr,
            &viewport,&raster,&samples,&depth,&blending,&dynamic,*clearPipelineLayout,
            renderPass(format,depthFormat));
        auto result=device.createGraphicsPipelineUnique(ctx->GetPipelineCache(),info);
        return *clearPipelines.emplace(key,std::move(result.value)).first->second;
    }
    vk::UniqueDescriptorSetLayout makeDescriptorLayout(unsigned slots) {
        std::array<vk::DescriptorSetLayoutBinding,6> bindings;
        bindings[0]={0,vk::DescriptorType::eUniformBufferDynamic,1,vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment};
        for(uint32_t i=1;i<5;i++)bindings[i]={i,vk::DescriptorType::eCombinedImageSampler,i==1?slots:1,vk::ShaderStageFlagBits::eFragment};
        bindings[5]={5,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eVertex};
        return device.createDescriptorSetLayoutUnique({{},bindings.size(),bindings.data()});
    }
    vk::DescriptorPool descriptorPool() {
        if(!activeDescriptors) {
            std::array<vk::DescriptorPoolSize,3> sizes={vk::DescriptorPoolSize(vk::DescriptorType::eUniformBufferDynamic,BatchDraws),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,BatchDraws),
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,(textureSlots+3)*BatchDraws)};
            activeDescriptors=device.createDescriptorPoolUnique({{},BatchDraws,sizes.size(),sizes.data()});
        }
        return *activeDescriptors;
    }

    void pollFrameGpuTiming() {
        if(!frameGpu.pool)return;
        struct Result { uint64_t value=0,available=0; };
        for(unsigned slot=0;slot<frameGpu.pending.size();++slot) {
            const uint64_t serial=frameGpu.pending[slot];
            if(!serial||serial>retiredSerial)continue;
            if(frameGpu.discarded[slot]) {
                ++timing.frameGpuInvalid;frameGpu.pending[slot]=0;frameGpu.discarded[slot]=false;
                continue;
            }
            // Availability alone can still describe the previous generation
            // until a recorded reset executes. The existing batch fence proves
            // that this generation's reset and end timestamp have both retired.
            std::array<Result,2> results{};
            const auto status=device.getQueryPoolResults(*frameGpu.pool,slot*2,2,sizeof(results),
                results.data(),sizeof(Result),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWithAvailability);
            if(status==vk::Result::eNotReady)continue;
            if(status!=vk::Result::eSuccess)throw std::runtime_error("Failed reading Hikaru frame GPU timestamps");
            if(!results[0].available||!results[1].available)continue;
            if(fragmentGpu.mask==~uint64_t{0}&&results[1].value<results[0].value) {
                ++timing.frameGpuInvalid;frameGpu.pending[slot]=0;continue;
            }
            const uint64_t ticks=(results[1].value-results[0].value)&fragmentGpu.mask;
            const double ms=double(ticks)*fragmentGpu.period/1000000.0;
            if(std::isfinite(ms)&&ms>=0) { timing.frameGpuMs+=ms;++timing.frameGpuSamples; }
            else ++timing.frameGpuInvalid;
            frameGpu.pending[slot]=0;
        }
    }
    void beginFrameGpuTiming(vk::CommandBuffer cmd) {
        if(!frameGpu.requested||frameGpu.attempted)return;
        frameGpu.attempted=true;
        // An abandoned frame can still own timestamps in the active batch.
        // Let the next ordinary submission retire it; do not submit just for diagnostics.
        if(frameGpu.ending>=0){++timing.frameGpuDropped;return;}
        if(!enableFragmentGpuTiming()){frameGpu.requested=false;return;}
        pollFrameGpuTiming();
        if(!frameGpu.pool) {
            try { frameGpu.pool=device.createQueryPoolUnique({{},vk::QueryType::eTimestamp,8}); }
            catch(const vk::OutOfDeviceMemoryError&) {
                frameGpu.requested=false;
                std::fprintf(stderr,"Hikaru Vulkan frame GPU timestamp allocation failed; rendering continues\n");
                return;
            }
        }
        for(unsigned slot=0;slot<frameGpu.pending.size();++slot)if(!frameGpu.pending[slot]) {
            endRenderPass(cmd);
            cmd.resetQueryPool(*frameGpu.pool,slot*2,2);
            cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,*frameGpu.pool,slot*2);
            frameGpu.active=int(slot);return;
        }
        ++timing.frameGpuDropped; // Keep all in-flight slots immutable; never wait for diagnostics.
    }
    void endFrameGpuTiming(vk::CommandBuffer cmd) {
        if(frameGpu.active<0)return;
        cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,*frameGpu.pool,unsigned(frameGpu.active)*2+1);
        frameGpu.ending=frameGpu.active; // flushBatch assigns the actual submission serial on success.
    }
    void discardFrameGpuTiming() {
        if(frameGpu.active>=0) {
            const unsigned slot=unsigned(frameGpu.active);
            frameGpu.discarded[slot]=true;
            // Only the start endpoint exists. Its ordinary submission fence is
            // enough to retire the slot; never read an unissued end endpoint.
            if(activeCommand)frameGpu.ending=int(slot);
            else frameGpu.pending[slot]=submissionSerial;
            frameGpu.active=-1;
        }
        frameGpu.attempted=false;fragmentGpu.claimed=false;
    }
    bool enableFragmentGpuTiming() {
        if(fragmentGpu.enabled)return true;
        const auto physical=ctx->GetPhysicalDevice();
        const auto queues=physical.getQueueFamilyProperties();
        const unsigned family=ctx->GetGraphicsQueueFamilyIndex();
        if(family>=queues.size()||!queues[family].timestampValidBits)return false;
        const double period=physical.getProperties().limits.timestampPeriod;
        if(!(period>0)||!std::isfinite(period))return false;
        const unsigned bits=queues[family].timestampValidBits;
        fragmentGpu.mask=bits>=64?~uint64_t{0}:(uint64_t{1}<<bits)-1;
        fragmentGpu.period=period;fragmentGpu.enabled=true;
        timing.gpuTiming=true;
        return true;
    }
    void pollFragmentGpuTiming() {
        if(!fragmentGpu.pool||!fragmentGpu.pending)return;
        struct Result { uint64_t value=0,available=0; };
        for(unsigned phase=0;phase<4;++phase) {
            const uint32_t pair=3u<<(phase*2);
            if((fragmentGpu.pending&pair)!=pair)continue;
            std::array<Result,2> results{};
            const auto status=device.getQueryPoolResults(*fragmentGpu.pool,phase*2,2,sizeof(results),
                results.data(),sizeof(Result),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWithAvailability);
            if(status==vk::Result::eNotReady)continue;
            if(status!=vk::Result::eSuccess)throw std::runtime_error("Failed reading Hikaru GPU timestamps");
            if(!results[0].available||!results[1].available)continue;
            const uint64_t ticks=(results[1].value-results[0].value)&fragmentGpu.mask;
            timing.gpuMs[phase]+=double(ticks)*fragmentGpu.period/1000000.0;
            ++timing.gpuSamples[phase];fragmentGpu.pending&=~pair;
        }
    }
    void beginFragmentGpuTiming(vk::CommandBuffer cmd) {
        fragmentGpu.recording=false;
        // Use the same first-transaction sampling rule as GL/DX when frame
        // profiling is active. Ownerless transaction tests can still operate
        // without a presentation boundary.
        if(frameGpu.attempted&&fragmentGpu.claimed)return;
        fragmentGpu.claimed=true;
        if(!fragmentGpu.enabled||fragmentGpu.pending)return;
        if(!fragmentGpu.pool) {
            try { fragmentGpu.pool=device.createQueryPoolUnique({{},vk::QueryType::eTimestamp,8}); }
            catch(const vk::OutOfDeviceMemoryError&) {
                fragmentGpu.enabled=false;timing.gpuTiming=false;
                std::fprintf(stderr,"Hikaru Vulkan fragment GPU timestamp allocation failed; rendering continues\n");
                return; // Optional diagnostics must not disable rendering.
            }
        }
        cmd.resetQueryPool(*fragmentGpu.pool,0,8);
        fragmentGpu.recording=true;
    }
    void fragmentTimestamp(vk::CommandBuffer cmd,unsigned index,bool end) {
        if(!fragmentGpu.recording)return;
        cmd.writeTimestamp(end?vk::PipelineStageFlagBits::eBottomOfPipe:vk::PipelineStageFlagBits::eTopOfPipe,
            *fragmentGpu.pool,index);
        fragmentGpu.pending|=1u<<index;
    }
    vk::UniquePipeline createFragmentResolvePipeline(vk::Format format,vk::PipelineLayout layout,
        vk::ShaderModule vertexShader,vk::ShaderModule fragmentShader) {
        const std::array<vk::PipelineShaderStageCreateInfo,2> stages={{
            {{},vk::ShaderStageFlagBits::eVertex,vertexShader,"main"},
            {{},vk::ShaderStageFlagBits::eFragment,fragmentShader,"main"}}};
        const vk::PipelineVertexInputStateCreateInfo vertices;
        const vk::PipelineInputAssemblyStateCreateInfo assembly({},vk::PrimitiveTopology::eTriangleList,false);
        const vk::PipelineViewportStateCreateInfo viewport({},1,nullptr,1,nullptr);
        const vk::PipelineRasterizationStateCreateInfo raster({},false,false,vk::PolygonMode::eFill,vk::CullModeFlagBits::eNone,vk::FrontFace::eCounterClockwise,false,0,0,0,1);
        const vk::PipelineMultisampleStateCreateInfo samples({},vk::SampleCountFlagBits::e1);
        const vk::PipelineDepthStencilStateCreateInfo depth;
        vk::PipelineColorBlendAttachmentState attachment;
        attachment.colorWriteMask=vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA;
        const vk::PipelineColorBlendStateCreateInfo blending({},false,vk::LogicOp::eCopy,1,&attachment);
        const std::array<vk::DynamicState,2> states={vk::DynamicState::eViewport,vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic({},states.size(),states.data());
        const vk::GraphicsPipelineCreateInfo info({},stages.size(),stages.data(),&vertices,&assembly,nullptr,
            &viewport,&raster,&samples,&depth,&blending,&dynamic,layout,renderPass(format,vk::Format::eUndefined));
        auto pipeline=device.createGraphicsPipelineUnique(ctx->GetPipelineCache(),info);
        return std::move(pipeline.value);
    }
    vk::Pipeline fragmentResolvePipeline(vk::Format format) {
        auto found=fragmentResolvePipelines.find(format);
        if(found!=fragmentResolvePipelines.end())return *found->second;
        auto pipeline=createFragmentResolvePipeline(format,*fragmentPipelineLayout,*fragmentResolveVertex,*fragmentResolveShader);
        return *fragmentResolvePipelines.emplace(format,std::move(pipeline)).first->second;
    }
    bool initializeFragmentLists(vk::Format format) {
        if(fragmentListLayout) {
            fragmentResolvePipeline(format);
            return true;
        }
        std::array<vk::DescriptorSetLayoutBinding,4> bindings{};
        for(uint32_t i=0;i<3;++i)bindings[i]={i,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eFragment};
        bindings[1].descriptorCount=4;
        bindings[3]={3,vk::DescriptorType::eCombinedImageSampler,1,vk::ShaderStageFlagBits::eFragment};
        auto listLayout=device.createDescriptorSetLayoutUnique({{},bindings.size(),bindings.data()});
#ifdef HIKARU_NATIVE_TESTING
        const auto fault=fragmentListInitFaultForTesting;
        fragmentListInitFaultForTesting=FragmentListInitFaultForTesting::None;
        switch(fault) {
        case FragmentListInitFaultForTesting::OutOfDeviceMemory: throw vk::OutOfDeviceMemoryError("Injected fragment-list initialization failure");
        case FragmentListInitFaultForTesting::OutOfHostMemory: throw vk::OutOfHostMemoryError("Injected fragment-list initialization failure");
        case FragmentListInitFaultForTesting::BadAlloc: throw std::bad_alloc();
        case FragmentListInitFaultForTesting::DeviceLost: throw vk::DeviceLostError("Injected fragment-list initialization failure");
        default: break;
        }
        const bool invalidShader=fault==FragmentListInitFaultForTesting::InvalidShader;
#endif
        const std::array<vk::DescriptorSetLayout,2> layouts={*descriptorLayout,*listLayout};
        const vk::PushConstantRange parameters(vk::ShaderStageFlagBits::eFragment,0,sizeof(HikaruFragmentListParameters));
        auto pipelineLayout=device.createPipelineLayoutUnique({{},layouts.size(),layouts.data(),1,&parameters});
        auto collectSource=vulkanFragmentSource(false,false,false,1,false,true);
#ifdef HIKARU_NATIVE_TESTING
        if(invalidShader)collectSource="invalid GLSL";
#endif
        auto collectShader=ShaderCompiler::CompileOptional(vk::ShaderStageFlagBits::eFragment,collectSource);
        if(!collectShader)return false;
        auto resolveVertex=ShaderCompiler::CompileOptional(vk::ShaderStageFlagBits::eVertex,vulkanFullscreenVertexSource());
        if(!resolveVertex)return false;
        auto resolveShader=ShaderCompiler::CompileOptional(vk::ShaderStageFlagBits::eFragment,
            vulkanFragmentListResolveSource(fragmentSmallList));
        if(!resolveShader)return false;
        // Create the target-format pipeline before publishing first-use handles.
        auto resolvePipeline=createFragmentResolvePipeline(format,*pipelineLayout,*resolveVertex,*resolveShader);
        fragmentResolvePipelines.emplace(format,std::move(resolvePipeline));
        fragmentListLayout=std::move(listLayout);
        fragmentPipelineLayout=std::move(pipelineLayout);
        fragmentCollectShader=std::move(collectShader);
        fragmentResolveVertex=std::move(resolveVertex);
        fragmentResolveShader=std::move(resolveShader);
        return true;
    }
    std::unique_ptr<BufferData> allocateFragmentBuffer(vk::DeviceSize bytes,vk::BufferUsageFlags usage) {
        // UMA devices can expose small local heaps plus a much larger coherent
        // system heap. Prefer local storage, then allow the GPU-visible shared
        // heap instead of disabling per-pixel collection at high resolutions.
        try { return std::make_unique<BufferData>(bytes,usage,vk::MemoryPropertyFlagBits::eDeviceLocal); }
        catch(const vk::OutOfDeviceMemoryError&) {
            if(timing.enabled)std::fprintf(stderr,"Hikaru Vulkan fragment storage: %llu bytes on shared memory\n",static_cast<unsigned long long>(bytes));
            return std::make_unique<BufferData>(bytes,usage,
                vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent);
        }
    }
    void resizeFragmentNodes(FragmentListStorage& storage,uint32_t capacity) {
        // The caller has retired all users of this descriptor set and its
        // buffers. Keep the fixed-size heads/background while resizing nodes.
        for(auto& block:storage.nodes)block.reset();
        for(auto& block:storage.nodes)block=allocateFragmentBuffer(vk::DeviceSize(capacity/4)*16,
            vk::BufferUsageFlagBits::eStorageBuffer);
        std::array<vk::DescriptorBufferInfo,4> nodes;
        for(size_t i=0;i<nodes.size();++i)nodes[i]=vk::DescriptorBufferInfo(*storage.nodes[i]->buffer,0,storage.nodes[i]->bufferSize);
        const vk::WriteDescriptorSet write(storage.set,1,0,4,vk::DescriptorType::eStorageBuffer,nullptr,nodes.data());
        device.updateDescriptorSets(1,&write,0,nullptr);
        storage.capacity=capacity;
        ++fragmentPoolAllocations;
        fragmentPoolPolicy.hint=capacity;
        hikaru_fragment_pool_reset_window(&fragmentPoolPolicy);
    }
    std::unique_ptr<FragmentListStorage> allocateFragmentLists(const Image& target,uint32_t capacity) {
        auto result=std::make_unique<FragmentListStorage>();
        const auto storage=vk::BufferUsageFlagBits::eStorageBuffer;
        result->background=makeImage(target.extent.width,target.extent.height,target.format,1,false,false);
        result->heads=allocateFragmentBuffer(vk::DeviceSize(target.extent.width)*target.extent.height*4,
            storage|vk::BufferUsageFlagBits::eTransferDst);
        result->status=allocateFragmentBuffer(16,storage|vk::BufferUsageFlagBits::eTransferSrc|vk::BufferUsageFlagBits::eTransferDst);
        result->readback=std::make_unique<BufferData>(16,vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCached);
        const std::array<vk::DescriptorPoolSize,2> sizes={{{vk::DescriptorType::eStorageBuffer,6},
            {vk::DescriptorType::eCombinedImageSampler,1}}};
        result->descriptors=device.createDescriptorPoolUnique({{},1,sizes.size(),sizes.data()});
        const auto layout=*fragmentListLayout;
        result->set=device.allocateDescriptorSets({*result->descriptors,1,&layout}).front();
        const vk::DescriptorBufferInfo heads(*result->heads->buffer,0,result->heads->bufferSize);
        const vk::DescriptorBufferInfo status(*result->status->buffer,0,result->status->bufferSize);
        const vk::DescriptorImageInfo background(sampler(Texture{},result->background,false),
            *result->background.view,vk::ImageLayout::eShaderReadOnlyOptimal);
        std::array<vk::WriteDescriptorSet,3> writes{};
        writes[0]={result->set,0,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&heads};
        writes[1]={result->set,2,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&status};
        writes[2]={result->set,3,0,1,vk::DescriptorType::eCombinedImageSampler,&background};
        device.updateDescriptorSets(writes,nullptr);
        resizeFragmentNodes(*result,capacity);
        return result;
    }

public:
    bool beginFragmentList(unsigned color,uint64_t budget) override {
        if(fragmentCollectActive)throw std::runtime_error("Nested Hikaru fragment-list collection");
        if(!fragmentListSupported||occlusionActive||!pendingOcclusions.empty())return false;
        auto& target=image(color);
        if(target.depth||
            (target.format!=vk::Format::eR8G8B8A8Unorm&&target.format!=vk::Format::eA2B10G10R10UnormPack32))return false;
        const auto limits=ctx->GetPhysicalDevice().getProperties().limits;
        const uint64_t pixels=uint64_t(target.extent.width)*target.extent.height;
        // Heads, the preserved opaque color, status and readback all count
        // against the user's budget, alongside the 16-byte fragment nodes.
        const uint64_t fixedBytes=pixels*8+32;
        const uint64_t blockLimit=std::min<uint64_t>(limits.maxStorageBufferRange,ctx->GetMaxMemoryAllocationSize());
        const uint64_t maximum=budget>fixedBytes?std::min<uint64_t>((budget-fixedBytes)/16,(blockLimit/16)*4):0;
        if(pixels>UINT32_MAX||pixels*4>blockLimit||maximum<4) {
            if(fragmentLists){waitForDraws(Timings::WaitReason::FragmentStorage);fragmentLists.reset();}
            fragmentPoolPolicy={};return false;
        }
        if(fragmentPoolExtent!=target.extent||fragmentPoolFormat!=target.format) {
            fragmentPoolPolicy={};fragmentPoolExtent=target.extent;fragmentPoolFormat=target.format;
        }
        // Start with one node per pixel; observed peaks then size the shared
        // pool across viewports without reallocating for each individual view.
        const uint64_t desired=fragmentPoolPolicy.hint?std::max(4096u,fragmentPoolPolicy.hint):std::max<uint64_t>(4096,pixels);
        const uint32_t capacity=uint32_t(std::min(maximum,desired)/4*4);
        const bool storageChanges=!fragmentLists||fragmentLists->background.extent!=target.extent||
            fragmentLists->background.format!=target.format||fragmentLists->capacity!=capacity;
        // Replacing buffers or updating their descriptors needs host retirement.
        // Unchanged storage remains immutable on the CPU; the barrier below
        // orders the previous resolve before this collection on the same queue.
        if(storageChanges)waitForDraws(Timings::WaitReason::FragmentStorage);
        else if(timing.enabled)++timing.fragmentReuses;
        pollFragmentGpuTiming();
        const bool initializedBefore=bool(fragmentListLayout);
        auto failFragmentListSetup=[&](const char* reason) {
            fragmentLists.reset();
            fragmentPoolPolicy={};
            timing.listCapacity=0;timing.listPoolBytes=0;
            if(!initializedBefore) {
                // First-use setup has no recorded users; retain caches after any later failure.
                fragmentResolvePipelines.clear();
                fragmentCollectShader.reset();fragmentResolveVertex.reset();fragmentResolveShader.reset();
                fragmentPipelineLayout.reset();fragmentListLayout.reset();
            }
            fragmentListSupported=false;
            std::fprintf(stderr,"Hikaru: fragment-list %s failed; using depth peeling\n",reason);
            return false;
        };
        try {
            if(!initializeFragmentLists(target.format))return failFragmentListSetup("optional shader compilation");
            if(!fragmentLists||fragmentLists->background.extent!=target.extent||fragmentLists->background.format!=target.format) {
                fragmentLists.reset();fragmentLists=allocateFragmentLists(target,capacity);
            } else if(fragmentLists->capacity!=capacity)resizeFragmentNodes(*fragmentLists,capacity);
        } catch(const vk::OutOfDeviceMemoryError&) {
            return failFragmentListSetup("optional allocation");
        } catch(const vk::OutOfHostMemoryError&) {
            return failFragmentListSetup("optional allocation");
        } catch(const std::bad_alloc&) {
            return failFragmentListSetup("optional allocation");
        }
        timing.listCapacity=fragmentLists->capacity;
        timing.listPoolBytes=fixedBytes+uint64_t(fragmentLists->capacity)*16;
        timing.poolAllocations=fragmentPoolAllocations;
        fragmentParameters={target.extent.width,target.extent.height,fragmentLists->capacity,0,
            target.format==vk::Format::eA2B10G10R10UnormPack32?1023.f:255.f,
            target.format==vk::Format::eA2B10G10R10UnormPack32?3.f:255.f,fragmentLists->capacity/4};
        fragmentColor=color;fragmentListFailed=false;
        auto cmd=startBatch();endRenderPass(cmd);
        beginFragmentGpuTiming(cmd);
        fragmentTimestamp(cmd,0,false);
        // Include transfer accesses for empty collections too: their heads and
        // counter were cleared/read even when no fragment shader ran.
        const auto reuseStages=vk::PipelineStageFlagBits::eTransfer|vk::PipelineStageFlagBits::eFragmentShader;
        const vk::MemoryBarrier retired(vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite|
            vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eTransferWrite|vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite);
        cmd.pipelineBarrier(reuseStages,reuseStages,{},retired,nullptr,nullptr);
        cmd.fillBuffer(*fragmentLists->heads->buffer,0,fragmentLists->heads->bufferSize,0);
        cmd.fillBuffer(*fragmentLists->status->buffer,0,16,0);
        fragmentTimestamp(cmd,1,true);
        const vk::MemoryBarrier initialized(vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eFragmentShader,
            {},initialized,nullptr,nullptr);
        fragmentTimestamp(cmd,2,false);
        fragmentCollectActive=true;
#ifdef HIKARU_NATIVE_TESTING
        ++fragmentListCounts[0];
#endif
        return true;
    }
    bool endFragmentList() override {
        if(!fragmentCollectActive)throw std::runtime_error("No active Hikaru fragment-list collection");
        fragmentCollectActive=false;
        auto cmd=startBatch();endRenderPass(cmd);
        fragmentTimestamp(cmd,3,true);
        const vk::MemoryBarrier countReady(vk::AccessFlagBits::eShaderWrite,vk::AccessFlagBits::eTransferRead);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,vk::PipelineStageFlagBits::eTransfer,
            {},countReady,nullptr,nullptr);
        cmd.copyBuffer(*fragmentLists->status->buffer,*fragmentLists->readback->buffer,vk::BufferCopy(0,0,16));
        const vk::MemoryBarrier hostReady(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,
            {},hostReady,nullptr,nullptr);
        waitForDraws(Timings::WaitReason::FragmentCounter);
        pollFragmentGpuTiming();
        std::array<uint32_t,4> status{};
        const void* mapped=fragmentLists->readback->allocation.MapMemory(false);
        try {
            fragmentLists->readback->allocation.InvalidateMemory(0,sizeof(status));
            std::memcpy(status.data(),mapped,sizeof(status));
        } catch (...) {
            fragmentLists->readback->allocation.UnmapMemory(false);
            throw;
        }
        fragmentLists->readback->allocation.UnmapMemory(false);
        if(status[1]||fragmentListFailed) {
#ifdef HIKARU_NATIVE_TESTING
            ++fragmentListCounts[2];
#endif
            if(fragmentListFailed)fragmentListSupported=false;
            else hikaru_fragment_pool_grow(&fragmentPoolPolicy,status[0],fragmentLists->capacity);
            if(!fragmentOverflowWarned) {
                std::fprintf(stderr,"Hikaru: fragment-list capacity (%u nodes) exceeded or unsupported draw; using depth peeling for this viewport\n",fragmentLists->capacity);
                fragmentOverflowWarned=true;
            }
            if(timing.enabled)++timing.listOverflows;
            fragmentLists.reset(); // Collection has retired; fallback owns the budget now.
            return false;
        }
        hikaru_fragment_pool_observe(&fragmentPoolPolicy,status[0],fragmentLists->capacity);
        if(timing.enabled){++timing.listResolves;timing.listFragments+=status[0];
            timing.listPeakFragments=std::max(timing.listPeakFragments,status[0]);}
#ifdef HIKARU_NATIVE_TESTING
        ++fragmentListCounts[1];
#endif
        if(!status[0])return true;
        auto& target=image(fragmentColor);auto& background=fragmentLists->background;
        cmd=startBatch();
        // Every node belongs to one pixel; the resolver may rewrite its links.
        const vk::MemoryBarrier collected(vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,vk::PipelineStageFlagBits::eFragmentShader,
            {},collected,nullptr,nullptr);
        fragmentTimestamp(cmd,4,false);
        barrier(cmd,target,vk::ImageLayout::eTransferSrcOptimal);
        barrier(cmd,background,vk::ImageLayout::eTransferDstOptimal);
        const vk::ImageSubresourceLayers color(vk::ImageAspectFlagBits::eColor,0,0,1);
        cmd.copyImage(*target.image,target.layout,*background.image,background.layout,
            vk::ImageCopy(color,{0,0,0},color,{0,0,0},{target.extent.width,target.extent.height,1}));
        fragmentTimestamp(cmd,5,true);
        barrier(cmd,background,vk::ImageLayout::eShaderReadOnlyOptimal);
        barrier(cmd,target,vk::ImageLayout::eColorAttachmentOptimal);
        const auto pass=renderPass(target.format,vk::Format::eUndefined);
        const auto fb=framebuffer(fragmentColor,0,target,nullptr,pass);
        cmd.beginRenderPass({pass,fb,vk::Rect2D({0,0},target.extent),0,nullptr},vk::SubpassContents::eInline);
        fragmentTimestamp(cmd,6,false);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,fragmentResolvePipeline(target.format));
        cmd.setViewport(0,vk::Viewport(0,0,float(target.extent.width),float(target.extent.height),0,1));
        cmd.setScissor(0,vk::Rect2D({0,0},target.extent));
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,*fragmentPipelineLayout,1,fragmentLists->set,nullptr);
        cmd.pushConstants(*fragmentPipelineLayout,vk::ShaderStageFlagBits::eFragment,0,sizeof(fragmentParameters),&fragmentParameters);
        cmd.draw(3,1,0,0);
        cmd.endRenderPass();
        fragmentTimestamp(cmd,7,true);
        boundPipeline=nullptr;boundVertexBuffer=nullptr;boundViewport.reset();boundScissor.reset();boundDepthBias.reset();
        lastDescriptors=nullptr;
        flushBatch();
        return true;
    }

    bool supportsDrawPalette() const override { return true; }
    bool supportsPackedVertices() const override { return packedVerticesSupported; }
    bool supportsGeometryReuse() const override { return true; }
#ifdef HIKARU_NATIVE_TESTING
    std::array<uint64_t,2> geometryCountsForTesting() const { return {vertexUploadCount,geometryReuseCount}; }
    std::array<uint64_t,3> fragmentCountsForTesting() const { return fragmentListCounts; }
    void disableFragmentListsForTesting() { fragmentListSupported=false; }
    void setFragmentListInitFaultForTesting(unsigned fault) {
        fragmentListInitFaultForTesting=static_cast<FragmentListInitFaultForTesting>(fault);
    }
    std::array<uint64_t,4> fragmentPoolForTesting() const {
        return {fragmentLists?fragmentLists->capacity:0,fragmentPoolAllocations,
            fragmentPoolPolicy.hint,fragmentPoolPolicy.samples};
    }
    void forceGeneralFragmentResolveForTesting() {
        if(fragmentListLayout)throw std::runtime_error("Fragment resolver already initialized");
        fragmentSmallList=false;
    }
    void enableWaitProfilingForTesting() { timing.enabled=true; }
    void limitPipelineCacheForTesting(size_t limit) {
        if(!limit||!pipelines.empty())throw std::runtime_error("Pipeline test limit requires an empty cache");
        pipelineCacheLimit=limit;
    }
    std::array<uint64_t,8> reuseCountsForTesting() const {
        return {timing.uniformUploads,timing.uniformHits,timing.paletteUploads,timing.paletteHits,
            timing.pipelineLookups,timing.pipelineFastHits,timing.maskedClears,
            timing.waitCounts[size_t(Timings::WaitReason::Readback)]};
    }
    std::array<uint64_t,4> fragmentWaitCountsForTesting() const {
        return {timing.waitCounts[size_t(Timings::WaitReason::FragmentStartup)],
            timing.waitCounts[size_t(Timings::WaitReason::FragmentStorage)],
            timing.waitCounts[size_t(Timings::WaitReason::FragmentCounter)],timing.fragmentReuses};
    }
    bool enableFrameGpuTimingForTesting() {
        frameGpu.requested=enableFragmentGpuTiming();return frameGpu.requested;
    }
    void finishFrameGpuTimingForTesting() {
        auto cmd=startBatch();endRenderPass(cmd);endFrameGpuTiming(cmd);flushBatch();
        frameGpu.attempted=false;fragmentGpu.claimed=false;
    }
    std::array<uint64_t,3> frameGpuSamplesForTesting() {
        reapCompletedDraws();pollFrameGpuTiming();
        return {timing.frameGpuSamples,timing.frameGpuDropped,timing.frameGpuInvalid};
    }
    double frameGpuMsForTesting() { return timing.frameGpuMs; }
    bool enableFragmentGpuTimingForTesting() { return enableFragmentGpuTiming(); }
    std::array<uint64_t,4> fragmentGpuSamplesForTesting() {
        pollFragmentGpuTiming();return timing.gpuSamples;
    }
    std::array<double,4> fragmentGpuTimesForTesting() {
        pollFragmentGpuTiming();return timing.gpuMs;
    }

#endif
    bool supportsTexturePalette() const override { return textureSlots==DrawPaletteSize; }
    void beginOcclusion() override {
        if(occlusionActive)throw std::runtime_error("Nested Hikaru visibility query");
        // A query needs a pass boundary, while its results are collected only
        // after the existing final submission wait. Keep layers in one stream.
        endRenderPass(startBatch());
        occlusionQueries.clear();
        occlusionActive=true;
    }
    void finishOcclusion() override {
        if(!occlusionActive)throw std::runtime_error("No active Hikaru visibility query");
        occlusionActive=false;
        pendingOcclusions.push_back(std::move(occlusionQueries));occlusionQueries.clear();
    }
    bool occlusionResult() override {
        if(pendingOcclusions.empty())throw std::runtime_error("No pending Hikaru visibility query");
        auto pending=std::move(pendingOcclusions.front());pendingOcclusions.pop_front();
        waitForDraws();
        bool visible=false;
        for(const auto& query:pending) {
            uint64_t samples=0;
            auto result=device.getQueryPoolResults(*query,0,1,sizeof(samples),&samples,sizeof(samples),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWait);
            if(result!=vk::Result::eSuccess)throw std::runtime_error("Failed reading Hikaru visibility query");
            visible|=samples!=0;
        }
        return visible;
    }
    bool endOcclusion() override { finishOcclusion(); return occlusionResult(); }
    explicit VulkanBackend(bool specialize=true) : specializeShaders(specialize) {
        if(!ctx) throw std::runtime_error("Hikaru Vulkan backend requires an active Vulkan context");
        commandPool=device.createCommandPoolUnique({vk::CommandPoolCreateFlagBits::eTransient|vk::CommandPoolCreateFlagBits::eResetCommandBuffer,ctx->GetGraphicsQueueFamilyIndex()});
        const auto physical=ctx->GetPhysicalDevice();
        const auto limits=physical.getProperties().limits;
        if(diagnosticFlagEnabled("HIKARU_TRACE_GPU_TIMING")&&!enableFragmentGpuTiming())
            std::fprintf(stderr,"Hikaru Vulkan fragment GPU timing is unavailable on this queue\n");
        packedVerticesSupported=std::all_of(PackedFormats.begin(),PackedFormats.end(),[&](vk::Format format) {
            return bool(physical.getFormatProperties(format).bufferFeatures&vk::FormatFeatureFlagBits::eVertexBuffer);
        });
        fragmentListSupported=ctx->hasPerPixel()&&physical.getFeatures().geometryShader&&
            limits.maxPerStageDescriptorStorageBuffers>=6&&limits.maxDescriptorSetStorageBuffers>=7&&
            limits.maxBoundDescriptorSets>=2&&limits.maxPushConstantsSize>=sizeof(HikaruFragmentListParameters);
        // The base texture aliases palette slot zero; the other three inputs
        // remain depth/metadata or bitmap layers. No descriptor-indexing feature
        // is needed, and small mobile sampler limits keep the scalar texture path.
        if(limits.maxPerStageDescriptorSamplers>=DrawPaletteSize+3&&limits.maxPerStageDescriptorSampledImages>=DrawPaletteSize+3&&
            limits.maxDescriptorSetSamplers>=DrawPaletteSize+3&&limits.maxDescriptorSetSampledImages>=DrawPaletteSize+3)
            textureSlots=DrawPaletteSize;
        descriptorLayout=makeDescriptorLayout(1);pipelineLayout=device.createPipelineLayoutUnique({{},1,&*descriptorLayout});
        paletteDescriptorLayout=makeDescriptorLayout(textureSlots);palettePipelineLayout=device.createPipelineLayoutUnique({{},1,&*paletteDescriptorLayout});
        colorOnlyPass=createPass(vk::Format::eR8G8B8A8Unorm,vk::Format::eUndefined,false);
        meshVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(false));
        meshFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(false,false,false,1));
        paletteVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(false,true));
        if(packedVerticesSupported) {
            packedMeshVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(false,false,true));
            packedPaletteVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(false,true,true));
        }
        paletteFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(false,false,true,textureSlots));
        fixedTexturePaletteFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(false,false,true,1));
        bitmapVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(true));
        bitmapFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(true,false,false,1));
        familyFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(true,true,false,1));
        peelFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(true,false,false,1,true));
        constexpr unsigned metadataFallback=0xfffffffcu;
        images.emplace(metadataFallback,makeImage(1,1,vk::Format::eR32G32Uint,1,false,false));
        immediate([&](vk::CommandBuffer cmd){auto& f=image(metadataFallback);barrier(cmd,f,vk::ImageLayout::eTransferDstOptimal);cmd.clearColorImage(*f.image,vk::ImageLayout::eTransferDstOptimal,vk::ClearColorValue(std::array<uint32_t,4>{}),vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1));barrier(cmd,f,vk::ImageLayout::eShaderReadOnlyOptimal);});
        image(metadataFallback).initializedLevels[0]=true;
        constexpr unsigned fallback=0xfffffffdu;
        images.emplace(fallback,makeImage(1,1,vk::Format::eR8G8B8A8Unorm,1,false,false));
        uint8_t zero[4]={};BufferData staging(4,vk::BufferUsageFlagBits::eTransferSrc);
        void* mapped=staging.allocation.MapMemory(false);
        try {
            std::memcpy(mapped,zero,sizeof(zero));
            staging.allocation.FlushMemory(0,sizeof(zero));
        } catch (...) {
            staging.allocation.UnmapMemory(false);
            throw;
        }
        staging.allocation.UnmapMemory(false);
        immediate([&](vk::CommandBuffer cmd){auto& f=image(fallback);barrier(cmd,f,vk::ImageLayout::eTransferDstOptimal);cmd.copyBufferToImage(*staging.buffer,*f.image,vk::ImageLayout::eTransferDstOptimal,vk::BufferImageCopy(0,0,0,{vk::ImageAspectFlagBits::eColor,0,0,1},{0,0,0},{1,1,1}));barrier(cmd,f,vk::ImageLayout::eShaderReadOnlyOptimal);});
    }
    ~VulkanBackend() override {
        // The last unfinished frame can be discarded. Submitting it or waiting on
        // its fence can throw during device loss. Drain already submitted draws
        // and presentation on their shared queue before releasing the OSD view.
        ctx->WaitIdle();
        ctx->PresentFrame({}, {}, {}, 1.f);
    }
    void texture(unsigned id,const Texture& info,unsigned level,TextureUpload upload,const void* pixels,size_t bytes) override {
        if(timing.enabled)++timing.uploads;
        const bool fullUpdate=upload==TextureUpload::FullUpdate;
        const bool depth=(id&0x80000000u)!=0||info.format==GL_DEPTH_COMPONENT16||info.format==GL_DEPTH_COMPONENT24||info.format==GL_DEPTH_COMPONENT32F||info.format==GL_DEPTH24_STENCIL8||info.format==GL_DEPTH_COMPONENT;
        if(fullUpdate&&(!info.width||!info.height||level!=0||!pixels||bytes==0||depth))throw std::runtime_error("Hikaru Vulkan full texture update requires level-zero color pixel data");
        unsigned levels=info.maxLevel==1000?1u+unsigned(std::floor(std::log2(std::max(info.width,info.height)))):std::max(1u,info.maxLevel+1);
        const unsigned uploadWidth=std::max(1u,info.width>>level),uploadHeight=std::max(1u,info.height>>level);
        if(fullUpdate) {
            if(size_t(uploadWidth)>SIZE_MAX/size_t(uploadHeight)/4)throw std::runtime_error("Hikaru Vulkan full texture update is too large");
            if(bytes!=size_t(uploadWidth)*uploadHeight*4)throw std::runtime_error("Hikaru Vulkan full texture update has an invalid byte count");
        }
        auto i=images.find(id);
        auto fmt=depth?depthFormat(info.format,(id&0x80000000u)==0):colorFormat(info.format);
        const bool compatible=i!=images.end()&&i->second.extent.width==info.width&&i->second.extent.height==info.height&&i->second.format==fmt&&i->second.levels==(depth?1:levels);
        if(fullUpdate&&!compatible)throw std::runtime_error("Hikaru Vulkan full texture update has no compatible allocation");
        reapCompletedDraws();
        if(!compatible) {
            retireImage(id); auto fresh=makeImage(info.width,info.height,fmt,depth?1:levels,depth,!depth,depth&&(id&0x80000000u)==0);fresh.revision=info.revision;images.emplace(id,std::move(fresh));
        }
        imageInfo[id]=info;
        Image& im=image(id); if(level>=im.levels)throw std::runtime_error("Hikaru Vulkan mip level exceeds allocated image");
        if(!pixels||bytes==0){im.initializedLevels[level]=true;return;}
        if(depth)throw std::runtime_error("CPU uploads to Hikaru Vulkan depth textures are unsupported");
        auto staging=std::make_unique<BufferData>(bytes,vk::BufferUsageFlagBits::eTransferSrc);
        void* mapped=staging->allocation.MapMemory(false);
        try {
            std::memcpy(mapped,pixels,bytes);
            staging->allocation.FlushMemory(0,bytes);
        } catch (...) {
            staging->allocation.UnmapMemory(false);
            throw;
        }
        staging->allocation.UnmapMemory(false);
        if(timing.enabled){++timing.textureUploads;timing.textureBytes+=bytes;}
        auto cmd=startBatch();
        // End prior sampling before the transfer write; the shared queue and image barriers order later draws after the replacement.
        endRenderPass(cmd);
        barrier(cmd,im,vk::ImageLayout::eTransferDstOptimal);
        vk::BufferImageCopy region(0,0,0,{vk::ImageAspectFlagBits::eColor,level,0,1},{0,0,0},{uploadWidth,uploadHeight,1});
        cmd.copyBufferToImage(*staging->buffer,*im.image,vk::ImageLayout::eTransferDstOptimal,region);
        barrier(cmd,im,vk::ImageLayout::eShaderReadOnlyOptimal);
        activeTransfers.push_back(std::move(staging));
        if(activeTransfers.size()>=64)waitForDraws();
        im.initializedLevels[level]=true;
    }
    void eraseTexture(unsigned id) override { reapCompletedDraws();retireImage(id);imageInfo.erase(id); }
    void clear(const Draw& d,GLbitfield mask,const Vec4& color,float depth) override {
        Image& c=image(d.color);Image* z=d.depth?&image(d.depth):nullptr;
        if(z&&z->extent!=c.extent)throw std::runtime_error("Hikaru Vulkan color/depth attachment size mismatch");
        unsigned channels=0;
        for(unsigned i=0;i<d.colorMask.size();++i)if(d.colorMask[i])channels|=1u<<i;
        const bool clearColor=(mask&GL_COLOR_BUFFER_BIT)&&channels;
        const bool clearDepth=(mask&GL_DEPTH_BUFFER_BIT)&&z&&d.depthWrite;
        if(!clearColor&&!clearDepth)return;
        vk::Rect2D rect({0,0},c.extent);
        if(d.scissorEnabled) {
            const int64_t left=d.scissor[0],right=left+d.scissor[2];
            const int64_t bottom=d.scissor[1],top=bottom+d.scissor[3];
            const auto clip=[](int64_t value,uint32_t size){return int32_t(std::clamp<int64_t>(value,0,size));};
            const int32_t x0=clip(left,c.extent.width),x1=clip(right,c.extent.width);
            const int32_t y0=clip(d.color==0?int64_t(c.extent.height)-top:bottom,c.extent.height);
            const int32_t y1=clip(d.color==0?int64_t(c.extent.height)-bottom:top,c.extent.height);
            rect.offset=vk::Offset2D(x0,y0);rect.extent=vk::Extent2D(std::max(0,x1-x0),std::max(0,y1-y0));
        }
        if(!rect.extent.width||!rect.extent.height)return;
        auto cmd=startBatch();
        beginFrameGpuTiming(cmd);
        // Clears must not contribute to an open visibility query.
        endRenderPass(cmd);
        barrier(cmd,c,vk::ImageLayout::eColorAttachmentOptimal);
        if(z)barrier(cmd,*z,vk::ImageLayout::eDepthStencilAttachmentOptimal);
        const auto depthFormat=z?z->format:vk::Format::eUndefined;
        const auto rp=renderPass(c.format,depthFormat);
        const auto fb=framebuffer(d.color,d.depth,c,z,rp);
        cmd.beginRenderPass({rp,fb,vk::Rect2D({0,0},c.extent),0,nullptr},vk::SubpassContents::eInline);
        std::array<vk::ClearAttachment,2> attachments{};uint32_t count=0;
        if(clearColor&&channels==15) {
            attachments[count].aspectMask=vk::ImageAspectFlagBits::eColor;attachments[count].colorAttachment=0;
            attachments[count].clearValue.color=vk::ClearColorValue(std::array<float,4>{color[0],color[1],color[2],color[3]});++count;
        }
        if(clearDepth) {
            attachments[count].aspectMask=vk::ImageAspectFlagBits::eDepth;
            attachments[count].clearValue.depthStencil=vk::ClearDepthStencilValue(depth,0);++count;
        }
        const vk::ClearRect area(rect,0,1);
        if(count)cmd.clearAttachments(count,attachments.data(),1,&area);
        if(clearColor&&channels!=15) {
            // Attachment clears ignore colorWriteMask. A tiny GPU draw preserves
            // the other channels without reading the image back to the host.
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,maskedClearPipeline(c.format,depthFormat,channels));
            cmd.setViewport(0,vk::Viewport(0,0,float(c.extent.width),float(c.extent.height),0,1));
            cmd.setScissor(0,rect);
            cmd.pushConstants(*clearPipelineLayout,vk::ShaderStageFlagBits::eFragment,0,sizeof(Vec4),&color);
            cmd.draw(3,1,0,0);
            boundPipeline=nullptr;boundVertexBuffer=nullptr;boundViewport.reset();boundScissor.reset();boundDepthBias.reset();
            lastDescriptors=nullptr;
            if(timing.enabled){++timing.maskedClears;timing.maskedPixels+=uint64_t(rect.extent.width)*rect.extent.height;}
        }
        cmd.endRenderPass();
    }
    void draw(const Draw& d) override {
        if(d.geometry&&(!d.vertices.empty()||!d.packedVertices.empty()))
            throw std::runtime_error("Ambiguous Hikaru Vulkan geometry");
        const auto& vertices=d.vertexData();const auto& packedVertices=d.packedVertexData();
        const bool packed=!packedVertices.empty();
        const size_t count=packed?packedVertices.size():vertices.size();
        const size_t stride=packed?sizeof(PackedVertex):sizeof(Vertex);
        if(!count)return;
        if(packed&&(!packedVerticesSupported||!vertices.empty()||d.bitmap))
            throw std::runtime_error("Invalid Hikaru Vulkan packed vertices");
        const bool collecting=fragmentCollectActive;
        uint32_t primitives=0;
        if(collecting) {
            if(fragmentListFailed)return;
            const size_t primitiveCount=d.topology==GL_TRIANGLES?count/3:
                d.topology==GL_TRIANGLE_STRIP&&count>=3?count-2:0;
            if(d.color!=fragmentColor||d.bitmap||!d.palette.empty()||!primitiveCount||
                primitiveCount>UINT32_MAX-fragmentParameters.orderBase) {
                fragmentListFailed=true;return;
            }
            primitives=uint32_t(primitiveCount);
        }
        if(timing.enabled){++timing.draws;timing.palettes+=!d.palette.empty();timing.packedDraws+=packed;}
        // Slot acquisition and pressure waits reap completed work. Polling a
        // fence for every triangle draw adds driver calls without freeing any
        // resource the active slot can use.
        if(activeDrawCount>=BatchDraws)flushBatch();
        const unsigned depthId=collecting?0:d.depth,metadataId=collecting?0:d.metadata;
        Image& target=image(d.color);Image* z=depthId?&image(depthId):nullptr;
        Image* metadata=metadataId?&image(metadataId):nullptr;
        if(metadata&&(metadata->extent!=target.extent||metadata->format!=vk::Format::eR32G32Uint))throw std::runtime_error("Invalid Hikaru family metadata target");
        if(z&&z->extent!=target.extent) throw std::runtime_error("Hikaru Vulkan color/depth attachment size mismatch");
        // Attachment transitions are ordered by render-pass dependencies and
        // image barriers on this queue. Submit at stream pressure or readback.
        if(renderPassOpen&&(d.color!=activeColor||depthId!=activeDepth||metadataId!=activeMetadata))endRenderPass(*activeCommand);
        if(count>UINT32_MAX/stride)throw std::runtime_error("Hikaru Vulkan draw exceeds buffer upload limit");
        const vk::DeviceSize vertexBytes=count*stride;
        if(d.palette.size()>DrawPaletteSize||(d.texturePalette&&(!supportsTexturePalette()||d.paletteTextures.size()!=d.palette.size()||d.paletteSampling.size()!=d.palette.size())))
            throw std::runtime_error("Invalid Hikaru Vulkan draw palette");
        auto cmd=startBatch();
        beginFrameGpuTiming(cmd);
        const auto retained=[&] {
            return d.geometry&&activeVertexBuffer&&
                activeVertexBuffer->geometryOffsets.find(d.geometry)!=activeVertexBuffer->geometryOffsets.end();
        };
        if(activeVertexBuffer&&activeVertexOffset+vertexBytes>activeVertexBuffer->storage.bufferSize&&!retained()) {
            if(activeDrawCount) {flushBatch();cmd=startBatch();}
            if(activeVertexBuffer&&activeVertexOffset+vertexBytes>activeVertexBuffer->storage.bufferSize&&!retained()) {
                // This slot was fence-retired and has recorded no vertex draw.
                // Rewinding it here cannot overwrite a pending GPU read.
                activeVertexBuffer->geometryOffsets.clear();
                activeVertexBuffer->used=activeVertexOffset=0;
            }
        }
        allocateStream(activeVertexBuffer,std::max<vk::DeviceSize>(1024*1024,vertexBytes),vk::BufferUsageFlagBits::eVertexBuffer);
        activeVertexBuffer->beginGeometryFrame(geometryFrame);
        activeVertexOffset=activeVertexBuffer->used;
        const vk::DeviceSize alignment=std::max<vk::DeviceSize>(1,ctx->GetUniformBufferAlignment());
        const vk::DeviceSize uniformStride=(sizeof(Uniforms)+alignment-1)/alignment*alignment;
        allocateStream(activeUniformBuffer,uniformStride*BatchDraws,vk::BufferUsageFlagBits::eUniformBuffer);
        // Owner rank is emitted only to integer metadata. Ignore that field
        // for comparison when unused; keep the actual immutable payload in the
        // snapshot so a later metadata draw still compares its owner exactly.
        // Compare the source directly: copying the whole block before a hit
        // would replace avoided GPU writes with unnecessary CPU memory traffic.
        constexpr size_t ownerOffset=offsetof(Uniforms,family)+2*sizeof(float);
        const auto* uniformData=reinterpret_cast<const uint8_t*>(&d.uniforms);
        const auto* previousUniformData=reinterpret_cast<const uint8_t*>(&uploadedUniforms);
        const bool uniformHit=uniformUploadValid&&(metadata?
            std::memcmp(previousUniformData,uniformData,sizeof(Uniforms))==0:
            std::memcmp(previousUniformData,uniformData,ownerOffset)==0&&
            std::memcmp(previousUniformData+ownerOffset+sizeof(float),uniformData+ownerOffset+sizeof(float),
                sizeof(Uniforms)-ownerOffset-sizeof(float))==0);
        const vk::DeviceSize uniformOffset=uniformHit?uploadedUniformOffset:
            (activeUniformOffset+alignment-1)/alignment*alignment;
        if(uniformOffset+sizeof(Uniforms)>activeUniformBuffer->storage.bufferSize)throw std::runtime_error("Hikaru Vulkan uniform batch capacity exceeded");
        auto& geometryOffsets=activeVertexBuffer->geometryOffsets;
        const auto cached=d.geometry?geometryOffsets.find(d.geometry):geometryOffsets.end();
        const bool uploaded=cached==geometryOffsets.end();
        const vk::DeviceSize vertexOffset=uploaded?activeVertexOffset:cached->second;
        if(uploaded) {
            const void* vertexData=packed?static_cast<const void*>(packedVertices.data()):vertices.data();
            std::memcpy(activeVertexBuffer->mapping+vertexOffset,vertexData,size_t(vertexBytes));
            activeVertexOffset+=vertexBytes;activeVertexBuffer->used=activeVertexOffset;
            if(timing.enabled){++timing.vertexUploads;timing.vertexBytes+=vertexBytes;}
            if(d.geometry)geometryOffsets.emplace(d.geometry,vertexOffset);
#ifdef HIKARU_NATIVE_TESTING
            ++vertexUploadCount;
#endif
        } else {
            if(timing.enabled)++timing.geometryHits;
#ifdef HIKARU_NATIVE_TESTING
            ++geometryReuseCount;
#endif
        }
        if(uniformHit) {
            if(timing.enabled)++timing.uniformHits;
        } else {
            std::memcpy(activeUniformBuffer->mapping+uniformOffset,&d.uniforms,sizeof(Uniforms));
            uploadedUniforms=d.uniforms;uploadedUniformOffset=uniformOffset;uniformUploadValid=true;
            activeUniformOffset=uniformOffset+sizeof(Uniforms);
            if(timing.enabled){++timing.uniformUploads;timing.uniformBytes+=sizeof(Uniforms);}
        }
        vk::DescriptorBufferInfo paletteInfo;
        if(!d.palette.empty()) {
            const vk::DeviceSize storageAlignment=std::max<vk::DeviceSize>(1,ctx->GetStorageBufferAlignment());
            const vk::DeviceSize maxRecordBytes=sizeof(DrawRecord)*DrawPaletteSize;
            const vk::DeviceSize stride=(maxRecordBytes+storageAlignment-1)/storageAlignment*storageAlignment;
            allocateStream(activePaletteBuffer,stride*BatchDraws,vk::BufferUsageFlagBits::eStorageBuffer);
            const vk::DeviceSize offset=(activePaletteOffset+storageAlignment-1)/storageAlignment*storageAlignment;
            const size_t bytes=d.palette.size()*sizeof(DrawRecord);
            if(offset+bytes>activePaletteBuffer->storage.bufferSize)throw std::runtime_error("Hikaru Vulkan palette capacity exceeded");
            for(size_t i=0;i<count;++i) {
                const float index=packed?packedVertices[i].drawIndex:vertices[i].extra[2];
                if(!std::isfinite(index)||index<0||index>=d.palette.size()||std::floor(index)!=index)
                    throw std::runtime_error("Invalid Hikaru Vulkan palette index");
            }
            if(paletteUploadValid&&uploadedPaletteSize==d.palette.size()&&
                std::memcmp(uploadedPalette.data(),d.palette.data(),bytes)==0) {
                paletteInfo=uploadedPaletteInfo;
                if(timing.enabled)++timing.paletteHits;
            } else {
                std::memcpy(activePaletteBuffer->mapping+offset,d.palette.data(),bytes);
                activePaletteOffset=offset+bytes;
                paletteInfo=vk::DescriptorBufferInfo(*activePaletteBuffer->storage.buffer,offset,bytes);
                std::copy(d.palette.begin(),d.palette.end(),uploadedPalette.begin());
                uploadedPaletteSize=d.palette.size();uploadedPaletteInfo=paletteInfo;paletteUploadValid=true;
                if(timing.enabled){++timing.paletteUploads;timing.paletteBytes+=bytes;}
            }
        }
        const unsigned slots=d.texturePalette?textureSlots:1;
        const auto layout=d.texturePalette?*paletteDescriptorLayout:*descriptorLayout;
        Timings::Span descriptorWork(timing.descriptorMs,timing.enabled);
        DescriptorKey descriptorKey;descriptorKey.texturePalette=d.texturePalette;descriptorKey.slots=slots;descriptorKey.palette=paletteInfo;
        auto& imgs=descriptorKey.images;
        constexpr unsigned transparentFallback=0xfffffffdu;
        std::array<unsigned,DrawPaletteSize+3> resolvedTextures{};
        for(unsigned n=0;n<slots+3;n++) {
            const bool base=n<slots;
            const unsigned unit=base?0:n-slots+1;
            const bool paletteTexture=base&&d.texturePalette&&n<d.palette.size();
            const Texture sampling=paletteTexture?d.paletteSampling[n]:base&&n!=0?Texture{}:d.sampling[unit];
            const GLenum mf=sampling.minFilter;
            const bool mipmapped=mf==GL_NEAREST_MIPMAP_NEAREST||mf==GL_NEAREST_MIPMAP_LINEAR||mf==GL_LINEAR_MIPMAP_NEAREST||mf==GL_LINEAR_MIPMAP_LINEAR;
            const unsigned tid=paletteTexture?d.paletteTextures[n]:base&&n!=0?0:d.textures[unit];Image* sampled=nullptr;
            if(tid&&tid!=d.color&&tid!=depthId&&tid!=metadataId){auto it=images.find(tid);if(it!=images.end())sampled=&it->second;}
            if(sampled&&sampled->sampleable&&sampled->initializedLevels[0]) {
                const unsigned geometric=1u+unsigned(std::floor(std::log2(std::max(sampled->extent.width,sampled->extent.height))));
                const unsigned expected=sampling.maxLevel==1000?geometric:std::min(geometric,sampling.maxLevel+1);
                const unsigned checked=mipmapped?expected:1;
                if(checked>sampled->levels)sampled=nullptr;
                else for(unsigned level=0;sampled&&level<checked;level++)if(!sampled->initializedLevels[level])sampled=nullptr;
            } else sampled=nullptr;
            if(!sampled) {
                const unsigned fallback=(!base&&!d.bitmap&&unit==3)||(!base&&d.familyResolve&&unit>=2)?0xfffffffcu:transparentFallback;
                sampled=&image(fallback);resolvedTextures[n]=fallback;
            } else resolvedTextures[n]=tid;
            imgs[n]={sampler(sampling,*sampled,mipmapped),*sampled->view,sampled->depth?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal};
        }
        if(!lastDescriptors||!(lastDescriptors->key==descriptorKey)) {
            auto found=std::find_if(materialDescriptors.begin(),materialDescriptors.end(),
                [&](const CachedDescriptors& entry){return entry.set&&entry.key==descriptorKey;});
            if(found==materialDescriptors.end()) {
                const auto set=device.allocateDescriptorSets({descriptorPool(),1,&layout}).front();
                vk::DescriptorBufferInfo bi(*activeUniformBuffer->storage.buffer,0,sizeof(Uniforms));
                std::array<vk::WriteDescriptorSet,6> writes{};writes[0]={set,0,0,1,vk::DescriptorType::eUniformBufferDynamic,nullptr,&bi};
                writes[1]={set,1,0,slots,vk::DescriptorType::eCombinedImageSampler,imgs.data()};
                for(unsigned unit=1;unit<4;++unit)writes[unit+1]={set,unit+1,0,1,vk::DescriptorType::eCombinedImageSampler,&imgs[slots+unit-1]};
                if(!d.palette.empty())writes[5]={set,5,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&paletteInfo};
                device.updateDescriptorSets(d.palette.empty()?5:6,writes.data(),0,nullptr);
                found=materialDescriptors.begin()+nextDescriptors;
                *found={std::move(descriptorKey),set};
                nextDescriptors=(nextDescriptors+1)%materialDescriptors.size();
                if(timing.enabled)++timing.descriptorSets;
            } else if(timing.enabled)++timing.descriptorHits;
            lastDescriptors=&*found;
        } else if(timing.enabled)++timing.descriptorHits;
        const auto set=lastDescriptors->set;
        descriptorWork.stop();
        for(unsigned n=0;n<slots+3;n++)if(resolvedTextures[n]!=transparentFallback){
            auto& sampled=image(resolvedTextures[n]);
            const auto layout=sampled.depth?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal;
            if(sampled.layout!=layout) {
                // End the pass at the first transition. Repeated bindings of
                // the same image then observe its updated layout.
                endRenderPass(cmd);
                barrier(cmd,sampled,layout);
            }
        }
        if(!renderPassOpen){
            if(timing.enabled)++timing.passes;
            barrier(cmd,target,vk::ImageLayout::eColorAttachmentOptimal);
            if(z)barrier(cmd,*z,vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if(metadata)barrier(cmd,*metadata,vk::ImageLayout::eColorAttachmentOptimal);
            auto rp=renderPass(target.format,z?z->format:vk::Format::eUndefined,metadata?metadata->format:vk::Format::eUndefined);
            auto fb=framebuffer(d.color,depthId,target,z,rp,metadataId,metadata);
            if(occlusionActive) {
                occlusionQueries.push_back(device.createQueryPoolUnique({{},vk::QueryType::eOcclusion,1}));
                if(timing.enabled)++timing.queries;
                cmd.resetQueryPool(*occlusionQueries.back(),0,1);
            }
            vk::Rect2D area({0,0},target.extent);cmd.beginRenderPass({rp,fb,area,0,nullptr},vk::SubpassContents::eInline);
            if(occlusionActive){occlusionQueryOpen=true;occlusionQuery=*occlusionQueries.back();cmd.beginQuery(occlusionQuery,0,{});}
            renderPassOpen=true;activeColor=d.color;activeDepth=depthId;activeMetadata=metadataId;
        }
        // Vulkan's positive-height viewport has the opposite framebuffer Y
        // orientation from GL. Internal targets keep the shader's GL clip Y
        // orientation, so their front-face sense reverses; the presentation
        // target also flips clip Y in the shader and keeps the requested sense.
        const GLenum frontFace=(d.uniforms.inverted[2]!=0.f)?(d.frontFace==GL_CCW?GL_CW:GL_CCW):d.frontFace;
        PipelineKey key{d.bitmap,d.familyResolve,d.peelComposite,!d.palette.empty(),packed,bool(z&&d.depthTest),bool(z&&d.depthWrite),collecting?false:d.blend,d.cull,d.biasFactor!=0||d.biasUnits!=0,d.depthFunc,d.cullFace,frontFace,d.srcRGB,d.dstRGB,d.srcAlpha,d.dstAlpha,d.equationRGB,d.equationAlpha,d.topology,collecting?std::array<bool,4>{false,false,false,false}:d.colorMask,target.format,z?z->format:vk::Format::eUndefined,metadata?metadata->format:vk::Format::eUndefined,meshShaderSpecialization(d,specializeShaders)};
        key.fragmentCollect=collecting;
        key.depthPass=d.depthPass;
        auto p=pipeline(key);
        if(boundPipeline!=p){cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,p);boundPipeline=p;}
        const float viewportY=d.color==0?float(target.extent.height)-d.viewport[1]-d.viewport[3]:float(d.viewport[1]);
        const std::array<float,6> viewport{float(d.viewport[0]),viewportY,float(d.viewport[2]),float(d.viewport[3]),d.nearDepth,d.farDepth};
        if(!boundViewport||*boundViewport!=viewport){cmd.setViewport(0,vk::Viewport(viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5]));boundViewport=viewport;}
        vk::Rect2D sc({0,0},target.extent);
        if(d.scissorEnabled){const int32_t y=d.color==0?int32_t(target.extent.height)-d.scissor[1]-d.scissor[3]:d.scissor[1];sc.offset=vk::Offset2D(d.scissor[0],y);sc.extent=vk::Extent2D(uint32_t(std::max(0,d.scissor[2])),uint32_t(std::max(0,d.scissor[3])));}
        if(!boundScissor||*boundScissor!=sc){cmd.setScissor(0,sc);boundScissor=sc;}
        const std::array<float,2> bias{d.biasUnits,d.biasFactor};
        if(!boundDepthBias||*boundDepthBias!=bias){cmd.setDepthBias(bias[0],0,bias[1]);boundDepthBias=bias;}
        const uint32_t dynamicOffset=uint32_t(uniformOffset);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,collecting?*fragmentPipelineLayout:d.texturePalette?*palettePipelineLayout:*pipelineLayout,0,set,dynamicOffset);
        if(collecting) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,*fragmentPipelineLayout,1,fragmentLists->set,nullptr);
            cmd.pushConstants(*fragmentPipelineLayout,vk::ShaderStageFlagBits::eFragment,0,sizeof(fragmentParameters),&fragmentParameters);
            fragmentParameters.orderBase+=primitives;
        }
        const auto vertexBuffer=*activeVertexBuffer->storage.buffer;
        if(boundVertexBuffer!=vertexBuffer||boundVertexOffset!=vertexOffset) {
            cmd.bindVertexBuffers(0,vertexBuffer,vertexOffset);boundVertexBuffer=vertexBuffer;boundVertexOffset=vertexOffset;
        }
        cmd.draw(uint32_t(count),1,0,0);
        ++activeDrawCount;
    }
    void readPixels(unsigned id,int x,int y,int w,int h,uint8_t* rgba) override {
        Image& im=image(id);if(w<=0||h<=0)return; BufferData dst(size_t(w)*h*4,vk::BufferUsageFlagBits::eTransferDst);
        const bool flipReadback=id==0;
        const int imageY=flipReadback?int(im.extent.height)-y-h:y;
        immediate([&](vk::CommandBuffer cmd){barrier(cmd,im,vk::ImageLayout::eTransferSrcOptimal);vk::BufferImageCopy region(0,w,h,{vk::ImageAspectFlagBits::eColor,0,0,1},{x,imageY,0},{uint32_t(w),uint32_t(h),1});cmd.copyImageToBuffer(*im.image,vk::ImageLayout::eTransferSrcOptimal,*dst.buffer,region);barrier(cmd,im,vk::ImageLayout::eShaderReadOnlyOptimal);});
        std::vector<uint8_t> raw(size_t(w)*h*4);
        const void* mapped=dst.allocation.MapMemory(false);
        try {
            dst.allocation.InvalidateMemory(0,raw.size());
            std::memcpy(raw.data(),mapped,raw.size());
        } catch (...) {
            dst.allocation.UnmapMemory(false);
            throw;
        }
        dst.allocation.UnmapMemory(false);
        for(int row=0;row<h;row++)for(int col=0;col<w;col++){
            const unsigned sourceRow=flipReadback?unsigned(h-1-row):unsigned(row);
            const auto* src=raw.data()+(size_t(sourceRow)*w+col)*4;auto* dstPixel=rgba+(size_t(row)*w+col)*4;
            if(im.format==vk::Format::eA2B10G10R10UnormPack32){uint32_t packed;std::memcpy(&packed,src,4);dstPixel[0]=((packed&1023)*255+511)/1023;dstPixel[1]=(((packed>>10)&1023)*255+511)/1023;dstPixel[2]=(((packed>>20)&1023)*255+511)/1023;dstPixel[3]=(((packed>>30)&3)*255+1)/3;}
            else std::memcpy(dstPixel,src,4);
        }
    }
    bool needsSceneDepth() const override {
        return ctx->HikaruShaderNeedsSceneDepth();
    }
    void present(unsigned w,unsigned h) override {
        Image& out=image(0);
        if(out.extent.width!=w||out.extent.height!=h){discardFrameGpuTiming();return;}
        auto cmd=startBatch();
        endRenderPass(cmd);
        barrier(cmd,out,vk::ImageLayout::eShaderReadOnlyOptimal);
        endFrameGpuTiming(cmd);
        flushBatch();
        frameGpu.attempted=false;fragmentGpu.claimed=false;
        const Image* sceneDepth = nullptr;
        if (presentedDepthTexture) {
            const auto depth = images.find(presentedDepthTexture);
            if (depth != images.end() && depth->second.depth && depth->second.sampleable
                    && depth->second.extent == out.extent
                    && depth->second.layout == vk::ImageLayout::eDepthStencilAttachmentOptimal)
                sceneDepth = &depth->second;
        }
        ctx->PresentFrame(*out.image, *out.view, out.extent, h ? float(w) / float(h) : 1.f,
            sceneDepth ? *sceneDepth->image : vk::Image {}, sceneDepth ? *sceneDepth->view : vk::ImageView {}, true,
            sceneDepth ? imageAspects(sceneDepth->format, true) : vk::ImageAspectFlags {});
        ++geometryFrame;
        if(frameGpu.requested){reapCompletedDraws();pollFrameGpuTiming();}
        pollFragmentGpuTiming();
        timing.present();
    }
};
}
std::unique_ptr<Backend> makeVulkanBackend() { return std::make_unique<VulkanBackend>(); }
#ifdef HIKARU_NATIVE_TESTING
std::unique_ptr<Backend> makeVulkanBackendForTesting(bool specialize) { return std::make_unique<VulkanBackend>(specialize); }
std::array<uint64_t,3> vulkanFragmentCountsForTesting(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).fragmentCountsForTesting();
}
std::array<uint64_t,4> vulkanFragmentPoolForTesting(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).fragmentPoolForTesting();
}
void forceVulkanGeneralFragmentResolveForTesting(Backend& backend) {
    static_cast<VulkanBackend&>(backend).forceGeneralFragmentResolveForTesting();
}
void limitVulkanPipelineCacheForTesting(Backend& backend,size_t limit) {
    static_cast<VulkanBackend&>(backend).limitPipelineCacheForTesting(limit);
}
std::array<uint64_t,8> vulkanReuseCountsForTesting(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).reuseCountsForTesting();
}
void enableVulkanWaitProfilingForTesting(Backend& backend) {
    static_cast<VulkanBackend&>(backend).enableWaitProfilingForTesting();
}
std::array<uint64_t,4> vulkanFragmentWaitCountsForTesting(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).fragmentWaitCountsForTesting();
}
bool enableVulkanFrameGpuTimingForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).enableFrameGpuTimingForTesting();
}
void finishVulkanDiagnosticFrameForTesting(Backend& backend) {
    static_cast<VulkanBackend&>(backend).finishFrameGpuTimingForTesting();
}
std::array<uint64_t,3> vulkanFrameGpuSamplesForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).frameGpuSamplesForTesting();
}
double vulkanFrameGpuMsForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).frameGpuMsForTesting();
}
bool enableVulkanFragmentGpuTimingForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).enableFragmentGpuTimingForTesting();
}
std::array<uint64_t,4> vulkanFragmentGpuSamplesForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).fragmentGpuSamplesForTesting();
}
std::array<double,4> vulkanFragmentGpuTimesForTesting(Backend& backend) {
    return static_cast<VulkanBackend&>(backend).fragmentGpuTimesForTesting();
}
void disableVulkanFragmentListsForTesting(Backend& backend) {
    static_cast<VulkanBackend&>(backend).disableFragmentListsForTesting();
}
void injectVulkanFragmentListInitFaultForTesting(Backend& backend,unsigned fault) {
    static_cast<VulkanBackend&>(backend).setFragmentListInitFaultForTesting(fault);
}
std::array<uint64_t,2> vulkanGeometryCountsForTesting(const Backend& backend) {
    return static_cast<const VulkanBackend&>(backend).geometryCountsForTesting();
}
#endif
} // namespace hikaru::graphics
#else
namespace hikaru::graphics { std::unique_ptr<Backend> makeVulkanBackend() { throw std::runtime_error("Vulkan support is not enabled"); } }
#endif
