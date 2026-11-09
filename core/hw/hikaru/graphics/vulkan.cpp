// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include "shaders.h"

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
#include <stdexcept>
#include <tuple>
#include <limits>

namespace hikaru::graphics {
namespace {
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
    static constexpr vk::DeviceSize RetainedStreamBytes = 32 * 1024 * 1024;
    static constexpr vk::DeviceSize LiveStreamBytes = 64 * 1024 * 1024;
    struct StreamBuffer {
        BufferData storage;
        uint8_t* mapping;
        StreamBuffer(vk::DeviceSize bytes, vk::BufferUsageFlags usage)
            : storage(bytes,usage), mapping(static_cast<uint8_t*>(storage.MapMemory())) {}
        ~StreamBuffer() { storage.UnmapMemory(); }
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
        vk::DeviceSize streamBytes() const {
            return (vertexBuffer?vertexBuffer->storage.bufferSize:0)+
                (uniformBuffer?uniformBuffer->storage.bufferSize:0)+
                (paletteBuffer?paletteBuffer->storage.bufferSize:0);
        }
    };
    struct PipelineKey {
        bool bitmap, familyResolve, palette, depthTest, depthWrite, blend, cull, depthBias;
        GLenum depthFunc, cullFace, frontFace, srcRGB, dstRGB, srcAlpha, dstAlpha, equationRGB, equationAlpha, topology;
        std::array<bool,4> colorMask;
        vk::Format color, depth, metadata;
        std::array<int32_t,MeshShaderSpecializationCount> shader{};
        bool operator<(const PipelineKey& b) const { return std::tie(bitmap,familyResolve,palette,depthTest,depthWrite,blend,cull,depthBias,depthFunc,cullFace,frontFace,srcRGB,dstRGB,srcAlpha,dstAlpha,equationRGB,equationAlpha,topology,colorMask,color,depth,metadata,shader) < std::tie(b.bitmap,b.familyResolve,b.palette,b.depthTest,b.depthWrite,b.blend,b.cull,b.depthBias,b.depthFunc,b.cullFace,b.frontFace,b.srcRGB,b.dstRGB,b.srcAlpha,b.dstAlpha,b.equationRGB,b.equationAlpha,b.topology,b.colorMask,b.color,b.depth,b.metadata,b.shader); }
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
    vk::UniqueShaderModule meshVertex, meshFragment, paletteVertex, paletteFragment, fixedTexturePaletteFragment, bitmapVertex, bitmapFragment, familyFragment;
    struct CachedPipeline { vk::UniquePipeline pipeline; uint64_t touched; };
    std::map<PipelineKey,CachedPipeline> pipelines;
    uint64_t pipelineClock = 0;
    unsigned textureSlots = 1;
    bool specializeShaders = true;
    std::unordered_map<unsigned, Image> images;
    std::unordered_map<unsigned, Texture> imageInfo;
    std::map<std::tuple<unsigned,unsigned,unsigned>,vk::UniqueFramebuffer> framebuffers;
    std::deque<PendingBatch> pendingBatches;
    std::vector<PendingBatch> freeBatches;
    std::optional<PendingBatch> failedSubmission;
    vk::UniqueCommandBuffer activeCommand;
    vk::UniqueFence activeFence;
    vk::UniqueDescriptorPool activeDescriptors;
    std::unique_ptr<StreamBuffer> activeVertexBuffer,activeUniformBuffer,activePaletteBuffer;
    std::vector<std::unique_ptr<BufferData>> activeTransfers;
    std::vector<Image> retiredImages;
    std::vector<vk::UniqueFramebuffer> retiredFramebuffers;
    std::vector<vk::UniquePipeline> retiredPipelines;
    vk::DeviceSize activeVertexOffset=0,activeUniformOffset=0,activePaletteOffset=0;
    unsigned activeDrawCount=0,activeColor=0,activeDepth=0,activeMetadata=0;
    vk::Pipeline boundPipeline;
    vk::Buffer boundVertexBuffer;
    std::optional<std::array<float,6>> boundViewport;
    std::optional<vk::Rect2D> boundScissor;
    std::optional<std::array<float,2>> boundDepthBias;
    // Opt-in diagnostics distinguish CPU pipeline work from queue backpressure.
    struct Timings {
        const bool enabled=std::getenv("HIKARU_TRACE_TIMING")!=nullptr;
        unsigned frames=0,draws=0,palettes=0,pipelines=0,passes=0,batches=0,queries=0,uploads=0;
        double waitMs=0,pipelineMs=0,submitMs=0,descriptorMs=0;
        struct Span {
            double& total; bool enabled;
            std::chrono::steady_clock::time_point start;
            Span(double& value,bool trace):total(value),enabled(trace),start(trace?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}){}
            void stop(){if(enabled){total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();enabled=false;}}
            ~Span(){stop();}
        };
        void present() {
            if(!enabled||++frames<60)return;
            std::fprintf(stderr,"Hikaru Vulkan timing: draws=%u palettes=%u pipelines=%u passes=%u batches=%u queries=%u uploads=%u wait_ms=%.3f pipeline_ms=%.3f submit_ms=%.3f descriptor_ms=%.3f\n",draws,palettes,pipelines,passes,batches,queries,uploads,waitMs/frames,pipelineMs/frames,submitMs/frames,descriptorMs/frames);
            frames=draws=palettes=pipelines=passes=batches=queries=uploads=0;waitMs=pipelineMs=submitMs=descriptorMs=0;
        }
    } timing;
    bool renderPassOpen=false;
    bool occlusionActive=false;
    std::vector<vk::UniqueQueryPool> occlusionQueries;

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
        target.reset(); // only unused storage from a fence-retired slot can grow
        while(liveStreamBytes()+bytes>LiveStreamBytes) {
            for(auto& batch:freeBatches){batch.vertexBuffer.reset();batch.uniformBuffer.reset();batch.paletteBuffer.reset();}
            if(liveStreamBytes()+bytes<=LiveStreamBytes||pendingBatches.empty())break;
            Timings::Span measured(timing.waitMs,timing.enabled);
            if(device.waitForFences(*pendingBatches.front().fence,true,UINT64_MAX)!=vk::Result::eSuccess)
                throw std::runtime_error("Failed waiting for Hikaru Vulkan stream budget");
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
    void waitForDraws() {
        Timings::Span measured(timing.waitMs,timing.enabled);
        flushBatch();
        if(pendingBatches.empty())return;
        if(device.waitForFences(*pendingBatches.back().fence,true,UINT64_MAX)!=vk::Result::eSuccess)
            throw std::runtime_error("Failed waiting for Hikaru Vulkan submission");
        reapCompletedDraws();
    }
    vk::CommandBuffer startBatch() {
        if(failedSubmission)throw std::runtime_error("Hikaru Vulkan submission has failed");
        if(activeCommand)return *activeCommand;
        reapCompletedDraws();
        if(freeBatches.empty()&&pendingBatches.size()>=BatchSlots) {
            Timings::Span measured(timing.waitMs,timing.enabled);
            if(device.waitForFences(*pendingBatches.front().fence,true,UINT64_MAX)!=vk::Result::eSuccess)
                throw std::runtime_error("Failed waiting for Hikaru Vulkan batch slot");
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
        boundPipeline=nullptr;boundVertexBuffer=nullptr;boundViewport.reset();boundScissor.reset();boundDepthBias.reset();
        auto cmd=*activeCommand;
        cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        return cmd;
    }
    void endRenderPass(vk::CommandBuffer cmd) {
        if(!renderPassOpen)return;
        if(occlusionActive)cmd.endQuery(*occlusionQueries.back(),0);
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
        vk::ImageAspectFlags aspect = image.depth ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth) : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
        if (image.format == vk::Format::eD24UnormS8Uint || image.format == vk::Format::eD32SfloatS8Uint)
            aspect |= vk::ImageAspectFlagBits::eStencil;
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
        auto found=pipelines.find(k);
        if(found!=pipelines.end()){found->second.touched=++pipelineClock;return *found->second.pipeline;}
        auto pass=renderPass(k.color,k.depth,k.metadata);
        vk::PipelineShaderStageCreateInfo stages[2]={{ {},vk::ShaderStageFlagBits::eVertex,k.bitmap?*bitmapVertex:k.palette?*paletteVertex:*meshVertex,"main" },{{},vk::ShaderStageFlagBits::eFragment,k.familyResolve?*familyFragment:k.bitmap?*bitmapFragment:k.palette?(k.shader[8]?*paletteFragment:*fixedTexturePaletteFragment):*meshFragment,"main"}};
        std::array<vk::SpecializationMapEntry,MeshShaderSpecializationCount> entries{};
        for(unsigned i=0;i<entries.size();++i)entries[i]=vk::SpecializationMapEntry(i,uint32_t(i*sizeof(int32_t)),sizeof(int32_t));
        vk::SpecializationInfo specialization(entries.size(),entries.data(),sizeof(k.shader),k.shader.data());
        if(!k.bitmap)stages[1].pSpecializationInfo=&specialization;
        vk::VertexInputBindingDescription binding(0,sizeof(Vertex),vk::VertexInputRate::eVertex);
        std::array<vk::VertexInputAttributeDescription,8> attrs; for(uint32_t i=0;i<8;i++)attrs[i]={i,0,vk::Format::eR32G32B32A32Sfloat,i*16};
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
        vk::GraphicsPipelineCreateInfo ci({},2,stages,&vi,&ia,nullptr,&vp,&rs,&ms,&ds,&cb,&dyn,k.shader[8]?*palettePipelineLayout:*pipelineLayout,pass);
        Timings::Span measured(timing.pipelineMs,timing.enabled);
        if(timing.enabled)++timing.pipelines;
        auto result=device.createGraphicsPipelineUnique(ctx->GetPipelineCache(),ci);
        // Evicted pipelines can still be referenced by recorded or submitted
        // draws. Retire them on this queue before reclaiming their handles.
        if(pipelines.size()>=512) {
            auto oldest=std::min_element(pipelines.begin(),pipelines.end(),[](const auto& a,const auto& b){return a.second.touched<b.second.touched;});
            retiredPipelines.push_back(std::move(oldest->second.pipeline));pipelines.erase(oldest);
        }
        return *pipelines.emplace(k,CachedPipeline{std::move(result.value),++pipelineClock}).first->second.pipeline;
    }
    vk::UniqueDescriptorSetLayout makeDescriptorLayout(unsigned slots) {
        std::array<vk::DescriptorSetLayoutBinding,6> bindings;
        bindings[0]={0,vk::DescriptorType::eUniformBuffer,1,vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment};
        for(uint32_t i=1;i<5;i++)bindings[i]={i,vk::DescriptorType::eCombinedImageSampler,i==1?slots:1,vk::ShaderStageFlagBits::eFragment};
        bindings[5]={5,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eVertex};
        return device.createDescriptorSetLayoutUnique({{},bindings.size(),bindings.data()});
    }
    vk::DescriptorPool descriptorPool() {
        if(!activeDescriptors) {
            std::array<vk::DescriptorPoolSize,3> sizes={vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer,BatchDraws),
                vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer,BatchDraws),
                vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,(textureSlots+3)*BatchDraws)};
            activeDescriptors=device.createDescriptorPoolUnique({{},BatchDraws,sizes.size(),sizes.data()});
        }
        return *activeDescriptors;
    }

public:
    bool supportsDrawPalette() const override { return true; }
    bool supportsTexturePalette() const override { return textureSlots==DrawPaletteSize; }
    void beginOcclusion() override {
        if(timing.enabled)++timing.queries;
        if(occlusionActive)throw std::runtime_error("Nested Hikaru visibility query");
        flushBatch();
        occlusionQueries.clear();
        occlusionActive=true;
    }
    bool endOcclusion() override {
        if(!occlusionActive)throw std::runtime_error("No active Hikaru visibility query");
        waitForDraws();
        occlusionActive=false;
        bool visible=false;
        for(const auto& query:occlusionQueries) {
            uint64_t samples=0;
            auto result=device.getQueryPoolResults(*query,0,1,sizeof(samples),&samples,sizeof(samples),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWait);
            if(result!=vk::Result::eSuccess)throw std::runtime_error("Failed reading Hikaru visibility query");
            visible|=samples!=0;
        }
        occlusionQueries.clear();
        return visible;
    }
    explicit VulkanBackend(bool specialize=true) : specializeShaders(specialize) {
        if(!ctx) throw std::runtime_error("Hikaru Vulkan backend requires an active Vulkan context");
        commandPool=device.createCommandPoolUnique({vk::CommandPoolCreateFlagBits::eTransient|vk::CommandPoolCreateFlagBits::eResetCommandBuffer,ctx->GetGraphicsQueueFamilyIndex()});
        const auto limits=ctx->GetPhysicalDevice().getProperties().limits;
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
        paletteFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(false,false,true,textureSlots));
        fixedTexturePaletteFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(false,false,true,1));
        bitmapVertex=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,vulkanVertexSource(true));
        bitmapFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(true,false,false,1));
        familyFragment=ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,vulkanFragmentSource(true,true,false,1));
        constexpr unsigned metadataFallback=0xfffffffcu;
        images.emplace(metadataFallback,makeImage(1,1,vk::Format::eR32G32Uint,1,false,false));
        immediate([&](vk::CommandBuffer cmd){auto& f=image(metadataFallback);barrier(cmd,f,vk::ImageLayout::eTransferDstOptimal);cmd.clearColorImage(*f.image,vk::ImageLayout::eTransferDstOptimal,vk::ClearColorValue(std::array<uint32_t,4>{}),vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1));barrier(cmd,f,vk::ImageLayout::eShaderReadOnlyOptimal);});
        image(metadataFallback).initializedLevels[0]=true;
        constexpr unsigned fallback=0xfffffffdu;
        images.emplace(fallback,makeImage(1,1,vk::Format::eR8G8B8A8Unorm,1,false,false));
        uint8_t zero[4]={};BufferData staging(4,vk::BufferUsageFlagBits::eTransferSrc);staging.upload(4,zero);
        immediate([&](vk::CommandBuffer cmd){auto& f=image(fallback);barrier(cmd,f,vk::ImageLayout::eTransferDstOptimal);cmd.copyBufferToImage(*staging.buffer,*f.image,vk::ImageLayout::eTransferDstOptimal,vk::BufferImageCopy(0,0,0,{vk::ImageAspectFlagBits::eColor,0,0,1},{0,0,0},{1,1,1}));barrier(cmd,f,vk::ImageLayout::eShaderReadOnlyOptimal);});
    }
    ~VulkanBackend() override {
        waitForDraws();
        // PresentFrame retains the sampled view for subsequent OSD frames.
        // Drain the device and clear that reference before destroying images.
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
        staging->upload(bytes,pixels);
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
        Image& c=image(d.color); Image* z=d.depth?&image(d.depth):nullptr;
        if(z&&z->extent!=c.extent) throw std::runtime_error("Hikaru Vulkan color/depth attachment size mismatch");
        const bool clearColor=(mask&GL_COLOR_BUFFER_BIT)!=0;
        const bool colorMaskFull=std::all_of(d.colorMask.begin(),d.colorMask.end(),[](bool enabled){return enabled;});
        if(clearColor&&!colorMaskFull) {
            const size_t bytes=size_t(c.extent.width)*c.extent.height*4;
            BufferData pixels(bytes,vk::BufferUsageFlagBits::eTransferSrc|vk::BufferUsageFlagBits::eTransferDst);
            immediate([&](vk::CommandBuffer cmd){barrier(cmd,c,vk::ImageLayout::eTransferSrcOptimal);vk::BufferImageCopy copy(0,c.extent.width,c.extent.height,{vk::ImageAspectFlagBits::eColor,0,0,1},{0,0,0},{c.extent.width,c.extent.height,1});cmd.copyImageToBuffer(*c.image,vk::ImageLayout::eTransferSrcOptimal,*pixels.buffer,copy);});
            std::vector<uint8_t> raw(bytes);pixels.download(bytes,raw.data());
            int x0=0,y0=0,x1=c.extent.width,y1=c.extent.height;
            if(d.scissorEnabled){x0=std::clamp(d.scissor[0],0,int(c.extent.width));x1=std::clamp(d.scissor[0]+d.scissor[2],0,int(c.extent.width));y0=std::clamp(d.color==0?int(c.extent.height)-d.scissor[1]-d.scissor[3]:d.scissor[1],0,int(c.extent.height));y1=std::clamp(d.color==0?int(c.extent.height)-d.scissor[1]:d.scissor[1]+d.scissor[3],0,int(c.extent.height));}
            if(c.format==vk::Format::eA2B10G10R10UnormPack32){
                auto quant=[](float v,uint32_t max){return uint32_t(std::lround(std::clamp(v,0.f,1.f)*max));};
                for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){auto* ptr=raw.data()+(size_t(y)*c.extent.width+x)*4;uint32_t packed;std::memcpy(&packed,ptr,4);for(unsigned channel=0;channel<4;channel++)if(d.colorMask[channel]){unsigned bits=channel==3?2:10,shift=channel*10;uint32_t m=(1u<<bits)-1;packed=(packed&~(m<<shift))|(quant(color[channel],m)<<shift);}std::memcpy(ptr,&packed,4);}
            } else {
                for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){auto* pixel=raw.data()+(size_t(y)*c.extent.width+x)*4;for(unsigned channel=0;channel<4;channel++)if(d.colorMask[channel])pixel[channel]=uint8_t(std::lround(std::clamp(color[channel],0.f,1.f)*255.f));}
            }
            pixels.upload(bytes,raw.data());
            immediate([&](vk::CommandBuffer cmd){barrier(cmd,c,vk::ImageLayout::eTransferDstOptimal);vk::BufferImageCopy copy(0,c.extent.width,c.extent.height,{vk::ImageAspectFlagBits::eColor,0,0,1},{0,0,0},{c.extent.width,c.extent.height,1});cmd.copyBufferToImage(*pixels.buffer,*c.image,vk::ImageLayout::eTransferDstOptimal,copy);barrier(cmd,c,vk::ImageLayout::eShaderReadOnlyOptimal);});
        }
        const bool gpuColor=clearColor&&colorMaskFull;
        const bool gpuDepth=(mask&GL_DEPTH_BUFFER_BIT)&&z&&d.depthWrite;
        if(!gpuColor&&!gpuDepth)return;
        auto cmd=startBatch();
        endRenderPass(cmd);
        {
            barrier(cmd,c,vk::ImageLayout::eColorAttachmentOptimal);
            if(z)barrier(cmd,*z,vk::ImageLayout::eDepthStencilAttachmentOptimal);
            auto rp=renderPass(c.format,z?z->format:vk::Format::eUndefined);
            auto fb=framebuffer(d.color,d.depth,c,z,rp);
            vk::Rect2D area({0,0},c.extent);cmd.beginRenderPass({rp,fb,area,0,nullptr},vk::SubpassContents::eInline);
            std::array<vk::ClearAttachment,2> attachments{};uint32_t count=0;
            if(gpuColor){attachments[count].aspectMask=vk::ImageAspectFlagBits::eColor;attachments[count].colorAttachment=0;attachments[count].clearValue.color=vk::ClearColorValue(std::array<float,4>{color[0],color[1],color[2],color[3]});++count;}
            if(gpuDepth){attachments[count].aspectMask=vk::ImageAspectFlagBits::eDepth;attachments[count].clearValue.depthStencil=vk::ClearDepthStencilValue(depth,0);++count;}
            vk::Rect2D rect({0,0},c.extent);
            if(d.scissorEnabled){int x0=std::clamp(d.scissor[0],0,int(c.extent.width)),x1=std::clamp(d.scissor[0]+d.scissor[2],0,int(c.extent.width));int y0=std::clamp(d.color==0?int(c.extent.height)-d.scissor[1]-d.scissor[3]:d.scissor[1],0,int(c.extent.height)),y1=std::clamp(d.color==0?int(c.extent.height)-d.scissor[1]:d.scissor[1]+d.scissor[3],0,int(c.extent.height));rect.offset=vk::Offset2D(x0,y0);rect.extent=vk::Extent2D(std::max(0,x1-x0),std::max(0,y1-y0));}
            if(count&&rect.extent.width&&rect.extent.height){std::vector<vk::ClearAttachment> selected(attachments.begin(),attachments.begin()+count);std::array<vk::ClearRect,1> rects{vk::ClearRect(rect,0,1)};cmd.clearAttachments(selected,rects);}
            cmd.endRenderPass();
        }
    }
    void draw(const Draw& d) override {
        if(d.vertices.empty())return;
        if(timing.enabled){++timing.draws;timing.palettes+=!d.palette.empty();}
        // Slot acquisition and pressure waits reap completed work. Polling a
        // fence for every triangle draw adds driver calls without freeing any
        // resource the active slot can use.
        if(activeDrawCount>=BatchDraws)flushBatch();
        Image& target=image(d.color);Image* z=d.depth?&image(d.depth):nullptr;
        Image* metadata=d.metadata?&image(d.metadata):nullptr;
        if(metadata&&(metadata->extent!=target.extent||metadata->format!=vk::Format::eR32G32Uint))throw std::runtime_error("Invalid Hikaru family metadata target");
        if(z&&z->extent!=target.extent) throw std::runtime_error("Hikaru Vulkan color/depth attachment size mismatch");
        if(renderPassOpen&&(d.color!=activeColor||d.depth!=activeDepth||d.metadata!=activeMetadata))flushBatch();
        if(d.vertices.size()>UINT32_MAX/sizeof(Vertex))throw std::runtime_error("Hikaru Vulkan draw exceeds buffer upload limit");
        const vk::DeviceSize vertexBytes=d.vertices.size()*sizeof(Vertex);
        if(d.palette.size()>DrawPaletteSize||(d.texturePalette&&(!supportsTexturePalette()||d.paletteTextures.size()!=d.palette.size()||d.paletteSampling.size()!=d.palette.size())))
            throw std::runtime_error("Invalid Hikaru Vulkan draw palette");
        if(activeVertexBuffer&&activeVertexOffset+vertexBytes>activeVertexBuffer->storage.bufferSize)flushBatch();
        auto cmd=startBatch();
        allocateStream(activeVertexBuffer,std::max<vk::DeviceSize>(1024*1024,vertexBytes),vk::BufferUsageFlagBits::eVertexBuffer);
        const vk::DeviceSize alignment=std::max<vk::DeviceSize>(1,ctx->GetUniformBufferAlignment());
        const vk::DeviceSize uniformStride=(sizeof(Uniforms)+alignment-1)/alignment*alignment;
        allocateStream(activeUniformBuffer,uniformStride*BatchDraws,vk::BufferUsageFlagBits::eUniformBuffer);
        const vk::DeviceSize uniformOffset=(activeUniformOffset+alignment-1)/alignment*alignment;
        if(uniformOffset+sizeof(Uniforms)>activeUniformBuffer->storage.bufferSize)throw std::runtime_error("Hikaru Vulkan uniform batch capacity exceeded");
        const uint32_t vertexOffset=uint32_t(activeVertexOffset);
        std::memcpy(activeVertexBuffer->mapping+vertexOffset,d.vertices.data(),size_t(vertexBytes));
        std::memcpy(activeUniformBuffer->mapping+uniformOffset,&d.uniforms,sizeof(Uniforms));
        vk::DescriptorBufferInfo paletteInfo;
        if(!d.palette.empty()) {
            const vk::DeviceSize storageAlignment=std::max<vk::DeviceSize>(1,ctx->GetStorageBufferAlignment());
            const vk::DeviceSize maxRecordBytes=sizeof(DrawRecord)*DrawPaletteSize;
            const vk::DeviceSize stride=(maxRecordBytes+storageAlignment-1)/storageAlignment*storageAlignment;
            allocateStream(activePaletteBuffer,stride*BatchDraws,vk::BufferUsageFlagBits::eStorageBuffer);
            const vk::DeviceSize offset=(activePaletteOffset+storageAlignment-1)/storageAlignment*storageAlignment;
            const size_t bytes=d.palette.size()*sizeof(DrawRecord);
            if(offset+bytes>activePaletteBuffer->storage.bufferSize)throw std::runtime_error("Hikaru Vulkan palette capacity exceeded");
            for(const auto& vertex:d.vertices)if(!std::isfinite(vertex.extra[2])||vertex.extra[2]<0||vertex.extra[2]>=d.palette.size()||std::floor(vertex.extra[2])!=vertex.extra[2])
                throw std::runtime_error("Invalid Hikaru Vulkan palette index");
            std::memcpy(activePaletteBuffer->mapping+offset,d.palette.data(),bytes);
            activePaletteOffset=offset+bytes;paletteInfo=vk::DescriptorBufferInfo(*activePaletteBuffer->storage.buffer,offset,bytes);
        }
        const unsigned slots=d.texturePalette?textureSlots:1;
        const auto layout=d.texturePalette?*paletteDescriptorLayout:*descriptorLayout;
        Timings::Span descriptorWork(timing.descriptorMs,timing.enabled);
        auto sets=device.allocateDescriptorSets({descriptorPool(),1,&layout});const auto set=sets.front();
        vk::DescriptorBufferInfo bi(*activeUniformBuffer->storage.buffer,uniformOffset,sizeof(Uniforms));
        std::array<vk::WriteDescriptorSet,6> writes{};writes[0]={set,0,0,1,vk::DescriptorType::eUniformBuffer,nullptr,&bi};
        std::array<vk::DescriptorImageInfo,DrawPaletteSize+3> imgs{};
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
            if(tid&&tid!=d.color&&tid!=d.depth&&tid!=d.metadata){auto it=images.find(tid);if(it!=images.end())sampled=&it->second;}
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
        writes[1]={set,1,0,slots,vk::DescriptorType::eCombinedImageSampler,imgs.data()};
        for(unsigned unit=1;unit<4;++unit)writes[unit+1]={set,unit+1,0,1,vk::DescriptorType::eCombinedImageSampler,&imgs[slots+unit-1]};
        if(!d.palette.empty())writes[5]={set,5,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&paletteInfo};
        device.updateDescriptorSets(d.palette.empty()?5:6,writes.data(),0,nullptr);
        descriptorWork.stop();
        bool sampleTransition=false;
        for(unsigned n=0;n<slots+3;n++)if(resolvedTextures[n]!=transparentFallback){
            const auto& sampled=image(resolvedTextures[n]);
            const auto layout=sampled.depth?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal;
            sampleTransition|=sampled.layout!=layout;
        }
        if(renderPassOpen&&sampleTransition)endRenderPass(cmd);
        for(unsigned n=0;n<slots+3;n++)if(resolvedTextures[n]!=transparentFallback){
            auto& sampled=image(resolvedTextures[n]);
            barrier(cmd,sampled,sampled.depth?vk::ImageLayout::eDepthStencilReadOnlyOptimal:vk::ImageLayout::eShaderReadOnlyOptimal);
        }
        if(!renderPassOpen){
            if(timing.enabled)++timing.passes;
            barrier(cmd,target,vk::ImageLayout::eColorAttachmentOptimal);
            if(z)barrier(cmd,*z,vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if(metadata)barrier(cmd,*metadata,vk::ImageLayout::eColorAttachmentOptimal);
            auto rp=renderPass(target.format,z?z->format:vk::Format::eUndefined,metadata?metadata->format:vk::Format::eUndefined);
            auto fb=framebuffer(d.color,d.depth,target,z,rp,d.metadata,metadata);
            if(occlusionActive) {
                occlusionQueries.push_back(device.createQueryPoolUnique({{},vk::QueryType::eOcclusion,1}));
                cmd.resetQueryPool(*occlusionQueries.back(),0,1);
            }
            vk::Rect2D area({0,0},target.extent);cmd.beginRenderPass({rp,fb,area,0,nullptr},vk::SubpassContents::eInline);
            if(occlusionActive)cmd.beginQuery(*occlusionQueries.back(),0,{});
            renderPassOpen=true;activeColor=d.color;activeDepth=d.depth;activeMetadata=d.metadata;
        }
        // Vulkan's positive-height viewport has the opposite framebuffer Y
        // orientation from GL. Internal targets keep the shader's GL clip Y
        // orientation, so their front-face sense reverses; the presentation
        // target also flips clip Y in the shader and keeps the requested sense.
        const GLenum frontFace=(d.uniforms.inverted[2]!=0.f)?(d.frontFace==GL_CCW?GL_CW:GL_CCW):d.frontFace;
        auto p=pipeline({d.bitmap,d.familyResolve,!d.palette.empty(),bool(z&&d.depthTest),bool(z&&d.depthWrite),d.blend,d.cull,d.biasFactor!=0||d.biasUnits!=0,d.depthFunc,d.cullFace,frontFace,d.srcRGB,d.dstRGB,d.srcAlpha,d.dstAlpha,d.equationRGB,d.equationAlpha,d.topology,d.colorMask,target.format,z?z->format:vk::Format::eUndefined,metadata?metadata->format:vk::Format::eUndefined,meshShaderSpecialization(d,specializeShaders)});
        if(boundPipeline!=p){cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,p);boundPipeline=p;}
        const float viewportY=d.color==0?float(target.extent.height)-d.viewport[1]-d.viewport[3]:float(d.viewport[1]);
        const std::array<float,6> viewport{float(d.viewport[0]),viewportY,float(d.viewport[2]),float(d.viewport[3]),d.nearDepth,d.farDepth};
        if(!boundViewport||*boundViewport!=viewport){cmd.setViewport(0,vk::Viewport(viewport[0],viewport[1],viewport[2],viewport[3],viewport[4],viewport[5]));boundViewport=viewport;}
        vk::Rect2D sc({0,0},target.extent);
        if(d.scissorEnabled){const int32_t y=d.color==0?int32_t(target.extent.height)-d.scissor[1]-d.scissor[3]:d.scissor[1];sc.offset=vk::Offset2D(d.scissor[0],y);sc.extent=vk::Extent2D(uint32_t(std::max(0,d.scissor[2])),uint32_t(std::max(0,d.scissor[3])));}
        if(!boundScissor||*boundScissor!=sc){cmd.setScissor(0,sc);boundScissor=sc;}
        const std::array<float,2> bias{d.biasUnits,d.biasFactor};
        if(!boundDepthBias||*boundDepthBias!=bias){cmd.setDepthBias(bias[0],0,bias[1]);boundDepthBias=bias;}
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,d.texturePalette?*palettePipelineLayout:*pipelineLayout,0,set,nullptr);
        const auto vertexBuffer=*activeVertexBuffer->storage.buffer;
        if(boundVertexBuffer!=vertexBuffer){cmd.bindVertexBuffers(0,vertexBuffer,{vk::DeviceSize(0)});boundVertexBuffer=vertexBuffer;}
        cmd.draw(uint32_t(d.vertices.size()),1,uint32_t(vertexOffset/sizeof(Vertex)),0);
        activeVertexOffset+=vertexBytes;activeUniformOffset=uniformOffset+sizeof(Uniforms);
        ++activeDrawCount;
    }
    void readPixels(unsigned id,int x,int y,int w,int h,uint8_t* rgba) override {
        Image& im=image(id);if(w<=0||h<=0)return; BufferData dst(size_t(w)*h*4,vk::BufferUsageFlagBits::eTransferDst);
        const bool flipReadback=id==0;
        const int imageY=flipReadback?int(im.extent.height)-y-h:y;
        immediate([&](vk::CommandBuffer cmd){barrier(cmd,im,vk::ImageLayout::eTransferSrcOptimal);vk::BufferImageCopy region(0,w,h,{vk::ImageAspectFlagBits::eColor,0,0,1},{x,imageY,0},{uint32_t(w),uint32_t(h),1});cmd.copyImageToBuffer(*im.image,vk::ImageLayout::eTransferSrcOptimal,*dst.buffer,region);barrier(cmd,im,vk::ImageLayout::eShaderReadOnlyOptimal);});
        std::vector<uint8_t> raw(size_t(w)*h*4);dst.download(raw.size(),raw.data());
        for(int row=0;row<h;row++)for(int col=0;col<w;col++){
            const unsigned sourceRow=flipReadback?unsigned(h-1-row):unsigned(row);
            const auto* src=raw.data()+(size_t(sourceRow)*w+col)*4;auto* dstPixel=rgba+(size_t(row)*w+col)*4;
            if(im.format==vk::Format::eA2B10G10R10UnormPack32){uint32_t packed;std::memcpy(&packed,src,4);dstPixel[0]=((packed&1023)*255+511)/1023;dstPixel[1]=(((packed>>10)&1023)*255+511)/1023;dstPixel[2]=(((packed>>20)&1023)*255+511)/1023;dstPixel[3]=(((packed>>30)&3)*255+1)/3;}
            else std::memcpy(dstPixel,src,4);
        }
    }
    void present(unsigned w,unsigned h) override {
        Image& out=image(0);if(out.extent.width!=w||out.extent.height!=h)return;
        auto cmd=startBatch();
        endRenderPass(cmd);
        barrier(cmd,out,vk::ImageLayout::eShaderReadOnlyOptimal);
        flushBatch();
        ctx->PresentFrame(*out.image,*out.view,out.extent,h?float(w)/float(h):1.f);
        timing.present();
    }
};
}
std::unique_ptr<Backend> makeVulkanBackend() { return std::make_unique<VulkanBackend>(); }
#ifdef HIKARU_NATIVE_TESTING
std::unique_ptr<Backend> makeVulkanBackendForTesting(bool specialize) { return std::make_unique<VulkanBackend>(specialize); }
#endif
} // namespace hikaru::graphics
#else
namespace hikaru::graphics { std::unique_ptr<Backend> makeVulkanBackend() { throw std::runtime_error("Vulkan support is not enabled"); } }
#endif
