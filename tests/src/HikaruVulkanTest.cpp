// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtest/gtest.h"
#include "HikaruNativeGraphicsCases.h"
#if defined(HAVE_D3D11) && defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#include "windows/comptr.h"
#endif
#if defined(USE_VULKAN) && defined(USE_SDL) && !defined(LIBRETRO)
#include "hw/hikaru/graphics/device.h"
#include "rend/vulkan/vulkan_context.h"
#include "sdl/sdl.h"
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "stdclass.h"
#include "imgui.h"
#include "log/LogManager.h"
#include "ui/gui.h"
#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace hikaru::graphics {
// Defined only when the native test executable compiles the Vulkan backend.
std::unique_ptr<Backend> makeVulkanBackendForTesting(bool specialize);
std::array<uint64_t,2> vulkanGeometryCountsForTesting(const Backend&);
std::array<uint64_t,3> vulkanFragmentCountsForTesting(const Backend&);
void disableVulkanFragmentListsForTesting(Backend&);
std::array<uint64_t,4> vulkanFragmentPoolForTesting(const Backend&);
void forceVulkanGeneralFragmentResolveForTesting(Backend&);
void limitVulkanPipelineCacheForTesting(Backend&,size_t);
std::array<uint64_t,8> vulkanReuseCountsForTesting(const Backend&);
void enableVulkanWaitProfilingForTesting(Backend&);
std::array<uint64_t,4> vulkanFragmentWaitCountsForTesting(const Backend&);
bool enableVulkanFragmentGpuTimingForTesting(Backend&);
std::array<uint64_t,4> vulkanFragmentGpuSamplesForTesting(Backend&);
std::array<double,4> vulkanFragmentGpuTimesForTesting(Backend&);
#if defined(HAVE_D3D11) && defined(_WIN32)
std::unique_ptr<Backend> makeDx11BackendForTesting(ID3D11Device*,ID3D11DeviceContext*,pD3DCompile,bool);
#endif
}
using namespace hikaru::graphics;
using namespace hikaru::graphics::test;
namespace {
class NativeTestLogListener final : public LogListener {
    void Log(LogTypes::LOG_LEVELS,const char* message) override { std::fprintf(stderr,"%s",message); }
};
class HikaruVulkanTest : public testing::Test {
protected:
    ImGuiContext* previousImGui=nullptr;
    ImGuiContext* testImGui=nullptr;
    RenderType previousRenderer=config::RendererType;
    decltype(settings.display) previousDisplay=settings.display;
    std::string configPath,dataPath;
    bool opened=false, configured=false, videoInitialized=false, createdLogger=false;
    void SetUp() override {
        if(!std::getenv("HOLLYCAST_NATIVE_VULKAN_TESTS"))GTEST_SKIP()<<"requires an explicitly requested physical Vulkan run";
        ASSERT_EQ(GraphicsContext::Instance(),nullptr);
        configPath=get_writable_config_path("");dataPath=get_writable_data_path("");
        const auto folder=std::filesystem::temp_directory_path()/
            ("hollycast-vulkan-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);set_user_config_dir(folder.string());set_user_data_dir(folder.string());
        configured=true;config::setAutoSave(false);config::RendererType=RenderType::Vulkan;
        if(!LogManager::GetInstance()){
            LogManager::Init();createdLogger=true;
            LogManager::GetInstance()->RegisterListener(LogListener::CONSOLE_LISTENER,new NativeTestLogListener());
            LogManager::GetInstance()->SetEnable(LogTypes::RENDERER,true);
            LogManager::GetInstance()->SetEnable(LogTypes::COMMON,true);
            LogManager::GetInstance()->SetLogLevel(LogTypes::LINFO);
        }
        previousImGui=ImGui::GetCurrentContext();gui_init();testImGui=ImGui::GetCurrentContext();
        // gtest_main replaces SDL2main in test builds. Explicitly perform its
        // readiness step, and report an unavailable video driver as a test failure.
        SDL_SetMainReady();ASSERT_EQ(SDL_InitSubSystem(SDL_INIT_VIDEO),0)<<SDL_GetError();videoInitialized=true;
        sdl_window_create();opened=true;
        ASSERT_TRUE(isVulkan(config::RendererType))<<"Vulkan initialization fell back to another API";
        void* window=nullptr;void* display=nullptr;GraphicsContext::Instance()->getWindow(&window,&display);
        if(window)SDL_HideWindow(static_cast<SDL_Window*>(window));
    }
    void TearDown() override {
        if(opened)sdl_window_destroy();else if(videoInitialized)SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if(createdLogger)LogManager::Shutdown();
        if(testImGui){gui_term();ImGui::SetCurrentContext(previousImGui);}
        if(configured){config::RendererType=previousRenderer;settings.display=previousDisplay;
            set_user_config_dir(configPath);set_user_data_dir(dataPath);config::setAutoSave(true);}
    }
};

Draw fragmentMesh(float z,const Vec4& color,unsigned mode=0) {
    auto draw=mesh();draw.depthTest=false;draw.depthWrite=false;
    draw.uniforms.flags={};draw.uniforms.blend={0,mode==1?1.f:0.f,float(mode),0};
    draw.uniforms.peel={1,1,16,16};draw.textures={0,0,2,0};
    draw.vertices.resize(6);
    const std::array<std::array<float,2>,6> corners={{{-1,-1},{1,-1},{-1,1},{1,-1},{1,1},{-1,1}}};
    for(size_t i=0;i<corners.size();++i){
        draw.vertices[i].position={corners[i][0],corners[i][1],z,1};
        draw.vertices[i].diffuse=color;draw.vertices[i].extra[0]=color[3];
    }
    return draw;
}
void drawOrderedFragment(Backend& backend,Draw draw) {
    draw.uniforms.peel[0]=0;draw.blend=true;
    const unsigned mode=unsigned(draw.uniforms.blend[2]);
    draw.srcRGB=mode==1?GL_DST_COLOR:mode==3?GL_ONE:GL_SRC_ALPHA;
    draw.dstRGB=mode==1?GL_ZERO:mode==2?GL_ONE:GL_ONE_MINUS_SRC_ALPHA;
    draw.srcAlpha=GL_ONE;draw.dstAlpha=mode==1?GL_ZERO:GL_ONE_MINUS_SRC_ALPHA;
    backend.draw(draw);
}
TEST_F(HikaruVulkanTest, FragmentListsMatchOrderedMixedBlendsAndDepthTies) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    const Vec4 background={0.2f,0.4f,0.6f,0.8f};
    std::vector<Draw> layers;
    for(unsigned i=0;i<12;++i){
        layers.push_back(fragmentMesh(-0.1f*float(i%4+1),
            {float(17*(i%5+1))/255.f,float(13*(i%7+1))/255.f,0.2f,0.4f},i%4));
    }
    backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
    auto ordered=layers;
    std::stable_sort(ordered.begin(),ordered.end(),[](const Draw& a,const Draw& b){
        return a.vertices[0].position[2]<b.vertices[0].position[2];
    });
    for(const auto& draw:ordered)drawOrderedFragment(*backend,draw);
    const auto expected=pixels(*backend);
    backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    for(const auto& draw:layers)backend->draw(draw);
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(expected,pixels(*backend),"mixed blends and equal-depth display order",1);
}
TEST_F(HikaruVulkanTest, FragmentListsSurviveBatchRolloverWithoutALayerArrayLimit) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    const auto render=[&](bool lists){
        clear(*backend);
        if(lists)EXPECT_TRUE(backend->beginFragmentList(0,1024*1024));
        for(unsigned i=0;i<300;++i){
            auto draw=fragmentMesh(-0.25f,{float(17*(i%8))/255.f,51.f/255.f,float(17*(i%3))/255.f,26.f/255.f});
            draw.scissorEnabled=true;draw.scissor={4,4,2,2};
            if(lists)backend->draw(draw);else drawOrderedFragment(*backend,draw);
        }
        if(lists)EXPECT_TRUE(backend->endFragmentList());
        return pixels(*backend);
    };
    const auto expected=render(false);
    equalPixels(expected,render(true),"300 tied layers across command batches",1);
    const auto counts=vulkanFragmentCountsForTesting(*backend);
    EXPECT_EQ(counts[1],1u);EXPECT_EQ(counts[2],0u);
}
TEST_F(HikaruVulkanTest, FragmentListOverflowPreservesTheSceneAndRecoversOnResize) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    const Vec4 background={0.2f,0.3f,0.4f,1};
    backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
    const auto before=pixels(*backend);
    // Exactly four nodes fit, one per block, after fixed storage overhead.
    ASSERT_TRUE(backend->beginFragmentList(0,16*16*8+32+64));
    backend->draw(fragmentMesh(-0.25f,{1,0,0,0.5f}));
    EXPECT_FALSE(backend->endFragmentList());
    equalPixels(before,pixels(*backend),"overflow must not commit partial color");
    // The same viewport can be replayed with the existing ordered/fallback path.
    drawOrderedFragment(*backend,fragmentMesh(-0.25f,{1,0,0,0.5f}));
    const auto recovered=pixels(*backend);
    EXPECT_NE(recovered,before);
    hikaru::graphics::Texture resized;resized.width=resized.height=32;
    backend->texture(0,resized,0,TextureUpload::Definition,nullptr,0);
    auto state=mesh();state.depth=0;state.viewport={0,0,32,32};
    backend->clear(state,GL_COLOR_BUFFER_BIT,background,1);
    const auto resizedBefore=pixels(*backend,32);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    // No fragments: an old viewport's nodes must not leak into the new image.
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(resizedBefore,pixels(*backend,32),"empty resized collection");
    disableVulkanFragmentListsForTesting(*backend);
    EXPECT_FALSE(backend->beginFragmentList(0,1024*1024));
    equalPixels(resizedBefore,pixels(*backend,32),"unsupported capability fallback");
    const auto counts=vulkanFragmentCountsForTesting(*backend);
    EXPECT_EQ(counts[0],2u);EXPECT_EQ(counts[1],1u);EXPECT_EQ(counts[2],1u);
}
TEST_F(HikaruVulkanTest, FragmentListsPreserveOpaqueDepthPatternAndPackedStripShading) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    auto opaque=fragmentMesh(-0.2f,{0.1f,0.4f,0.7f,1});
    opaque.depthTest=opaque.depthWrite=true;opaque.uniforms.peel[0]=0;
    clear(*backend);backend->draw(opaque);
    const auto opaqueOnly=pixels(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    backend->draw(fragmentMesh(-0.8f,{1,0,0,0.5f}));
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(opaqueOnly,pixels(*backend),"opaque depth rejects farther transparency");
    for(bool packed:{false,true}){
        if(packed&&!backend->supportsPackedVertices())continue;
        auto draw=fragmentMesh(-0.1f,{128.f/255.f,64.f/255.f,1,128.f/255.f});
        draw.uniforms.family[3]=float(0xa5a5);draw.uniforms.flags={0,0,0,1};
        draw.uniforms.fog={2,0,0,0};draw.uniforms.fogColor={0.2f,0.3f,0.4f,0};
        draw.vertices={draw.vertices[0],draw.vertices[1],draw.vertices[2],draw.vertices[4]};
        draw.topology=GL_TRIANGLE_STRIP;
        if(packed){
            for(const auto& source:draw.vertices){
                PackedVertex vertex;std::copy_n(source.position.begin(),3,vertex.position.begin());
                vertex.diffuse={128,64,255};vertex.alpha=128;draw.packedVertices.push_back(vertex);
            }
            draw.vertices.clear();
        }
        backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);
        drawOrderedFragment(*backend,draw);const auto expected=pixels(*backend);
        backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);
        ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
        backend->draw(draw);ASSERT_TRUE(backend->endFragmentList());
        equalPixels(expected,pixels(*backend),"packed/expanded strip pattern and fog",1);
    }
}


TEST_F(HikaruVulkanTest, FragmentSmallListBoundariesMatchTheGeneralResolverExactly) {
    auto general=makeVulkanBackendForTesting(true),fastBackend=makeVulkanBackendForTesting(true);
    forceVulkanGeneralFragmentResolveForTesting(*general);
    allocate(*general);allocate(*fastBackend);
    for(unsigned count:{0u,1u,2u,7u,8u,9u,16u,64u,300u}) {
        SCOPED_TRACE(count);
        const auto render=[&](Backend& backend){
            clear(backend);EXPECT_TRUE(backend.beginFragmentList(0,1024*1024));
            for(unsigned i=0;i<count;++i) {
                auto draw=fragmentMesh(-0.1f*float(i%4+1),
                    {float(17*(i%8))/255.f,51.f/255.f,float(17*(i%3))/255.f,102.f/255.f},i%4);
                draw.scissorEnabled=true;draw.scissor={4,4,2,2};
                backend.draw(draw);
            }
            EXPECT_TRUE(backend.endFragmentList());
            return pixels(backend);
        };
        const auto expected=render(*general);
        equalPixels(expected,render(*fastBackend),"small/general boundary and tied blend order");
    }
}
TEST_F(HikaruVulkanTest, FragmentPoolAdaptsWithoutReallocatingEveryCollection) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    hikaru::graphics::Texture target;target.width=target.height=128;
    backend->texture(0,target,0,TextureUpload::Definition,nullptr,0);
    constexpr uint64_t budget=1024*1024;
    ASSERT_TRUE(backend->beginFragmentList(0,budget));
    const auto initial=vulkanFragmentPoolForTesting(*backend);
    EXPECT_EQ(initial[0],128u*128u);
    ASSERT_TRUE(backend->endFragmentList());
    for(unsigned i=1;i<120;++i) {
        ASSERT_TRUE(backend->beginFragmentList(0,budget));
        ASSERT_TRUE(backend->endFragmentList());
        EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[1],initial[1]);
    }
    ASSERT_TRUE(backend->beginFragmentList(0,budget));
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[0],4096u);
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[1],initial[1]+1);
    ASSERT_TRUE(backend->endFragmentList());
    const uint64_t fixed=128u*128u*8u+32u;
    ASSERT_TRUE(backend->beginFragmentList(0,fixed+64));
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[0],4u);
    ASSERT_TRUE(backend->endFragmentList());
    EXPECT_FALSE(backend->beginFragmentList(0,fixed));
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[0],0u);
    allocate(*backend);clear(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,budget));
    for(unsigned i=0;i<14;++i)backend->draw(fragmentMesh(-0.25f,{0.2f,0.3f,0.4f,0.5f}));
    ASSERT_TRUE(backend->endFragmentList());
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[0],4096u);
    ASSERT_TRUE(backend->beginFragmentList(0,budget));
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[0],8192u);
    ASSERT_TRUE(backend->endFragmentList());
    const auto grown=vulkanFragmentPoolForTesting(*backend);
    for(unsigned i=0;i<10;++i) {
        ASSERT_TRUE(backend->beginFragmentList(0,budget));
        ASSERT_TRUE(backend->endFragmentList());
        EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[1],grown[1]);
    }
    EXPECT_EQ(vulkanFragmentCountsForTesting(*backend)[2],0u);
}
TEST_F(HikaruVulkanTest, FragmentReuseOrdersBackToBackTargetsAndEmptyViewports) {
    auto reference=makeVulkanBackendForTesting(true),backend=makeVulkanBackendForTesting(true);
    allocate(*reference);allocate(*backend);
    for(auto* renderer:{reference.get(),backend.get()}) {
        hikaru::graphics::Texture target;target.width=target.height=16;
        renderer->texture(20,target,0,TextureUpload::Definition,nullptr,0);
        for(unsigned color:{0u,20u}) {
            auto state=mesh();state.color=color;
            renderer->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0.2f,0.3f,0.4f,1},1);
        }
    }
    enableVulkanWaitProfilingForTesting(*backend);
    const bool timed=enableVulkanFragmentGpuTimingForTesting(*backend);
    // No pixel readback between collections: the next viewport must reuse the
    // pool while the previous resolve is still submitted, including >8-node
    // lists whose resolver rewrites links. Alternate equal-sized color targets.
    for(unsigned view=0;view<12;++view) {
        const unsigned color=view%2?20:0;
        std::vector<Draw> layers;
        if(view%4!=1)for(unsigned layer=0;layer<9;++layer) {
            auto draw=fragmentMesh(-0.1f*float(layer%4+1),
                {float(17*(layer%5+1))/255.f,0.2f,0.4f,0.4f},(view+layer)%4);
            draw.color=color;draw.scissorEnabled=true;
            draw.scissor={GLint(view%3),GLint(view%2),12,12};
            layers.push_back(std::move(draw));
        }
        ASSERT_TRUE(backend->beginFragmentList(color,1024*1024));
        for(const auto& draw:layers)backend->draw(draw);
        ASSERT_TRUE(backend->endFragmentList());
        std::stable_sort(layers.begin(),layers.end(),[](const Draw& a,const Draw& b){
            return a.vertices[0].position[2]<b.vertices[0].position[2];
        });
        for(const auto& draw:layers)drawOrderedFragment(*reference,draw);
    }
    const auto waits=vulkanFragmentWaitCountsForTesting(*backend);
    EXPECT_EQ(waits[0],0u);EXPECT_EQ(waits[1],1u);
    EXPECT_EQ(waits[2],12u);EXPECT_EQ(waits[3],11u);
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[1],1u);
    for(unsigned color:{0u,20u}) {
        std::vector<uint8_t> referencePixels(16*16*4),actualPixels(referencePixels.size());
        reference->readPixels(color,0,0,16,16,referencePixels.data());
        backend->readPixels(color,0,0,16,16,actualPixels.data());
        equalPixels(referencePixels,actualPixels,"back-to-back fragment storage reuse",1);
    }
    if(timed) {
        const auto samples=vulkanFragmentGpuSamplesForTesting(*backend);
        const auto times=vulkanFragmentGpuTimesForTesting(*backend);
        for(unsigned phase=0;phase<4;++phase) {
            EXPECT_GT(samples[phase],0u);EXPECT_LE(samples[phase],12u);
            EXPECT_TRUE(std::isfinite(times[phase]));EXPECT_GE(times[phase],0);
        }
    }
}
TEST_F(HikaruVulkanTest, FragmentReuseRetiresBeforeBudgetChangesAndOverflow) {
    auto reference=makeVulkanBackendForTesting(true),backend=makeVulkanBackendForTesting(true);
    allocate(*reference);clear(*reference);allocate(*backend);clear(*backend);
    drawOrderedFragment(*reference,fragmentMesh(-0.25f,{1,0,0,0.4f}));
    const auto expected=pixels(*reference);
    enableVulkanWaitProfilingForTesting(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    backend->draw(fragmentMesh(-0.25f,{1,0,0,0.4f}));
    ASSERT_TRUE(backend->endFragmentList());
    // Resize nodes while that resolve is submitted. Overflow must leave its
    // completed color untouched, then release the pool for the fallback.
    constexpr uint64_t fixed=16*16*8+32;
    ASSERT_TRUE(backend->beginFragmentList(0,fixed+64));
    backend->draw(fragmentMesh(-0.25f,{0,1,0,0.5f}));
    EXPECT_FALSE(backend->endFragmentList());
    const auto waits=vulkanFragmentWaitCountsForTesting(*backend);
    EXPECT_EQ(waits[0],0u);EXPECT_EQ(waits[1],2u);EXPECT_EQ(waits[2],2u);
    const auto result=pixels(*backend);
    equalPixels(expected,result,"overflow preserves the preceding submitted resolve",1);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    ASSERT_TRUE(backend->endFragmentList());
    EXPECT_EQ(vulkanFragmentPoolForTesting(*backend)[1],3u);
    equalPixels(result,pixels(*backend),"empty collection after overflow recovery");
}
TEST_F(HikaruVulkanTest, FragmentGpuTimestampsRetireAcrossEmptyCollectionsAndResizes) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    if(!enableVulkanFragmentGpuTimingForTesting(*backend))GTEST_SKIP()<<"queue has no timestamp support";
    for(unsigned i=0;i<4;++i) {
        const unsigned width=i<2?16:32;
        hikaru::graphics::Texture target;target.width=target.height=width;
        backend->texture(0,target,0,TextureUpload::Definition,nullptr,0);
        auto state=mesh();state.depth=0;state.viewport={0,0,GLint(width),GLint(width)};
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
        if(i!=1) {
            auto draw=fragmentMesh(-0.25f,{0.2f,0.4f,0.6f,0.5f});
            draw.viewport=state.viewport;draw.uniforms.peel={0,1,float(width),float(width)};
            backend->draw(draw);
        }
        ASSERT_TRUE(backend->endFragmentList());
        (void)pixels(*backend,width);
    }
    const auto samples=vulkanFragmentGpuSamplesForTesting(*backend);
    const auto times=vulkanFragmentGpuTimesForTesting(*backend);
    for(unsigned phase=0;phase<4;++phase) {
        EXPECT_GT(samples[phase],0u);EXPECT_LE(samples[phase],4u);
        EXPECT_TRUE(std::isfinite(times[phase]));EXPECT_GE(times[phase],0);
    }
}

TEST_F(HikaruVulkanTest, PackedVerticesMatchExpandedLightingAndMixedStreams) {
    auto backend=makeVulkanBackendForTesting(true);
    if(!backend->supportsPackedVertices())GTEST_SKIP()<<"compact vertex formats unavailable";
    exercisePackedVertices(*backend);
}
TEST_F(HikaruVulkanTest, RetainedGeometrySurvivesBatchRollover) {
    auto backend=makeVulkanBackendForTesting(true);
    exerciseGeometryReuse(*backend,vulkanGeometryCountsForTesting);
}
TEST_F(HikaruVulkanTest, UniformOffsetsPreserveQueuedDrawsAcrossBatches) {
    auto backend=makeVulkanBackendForTesting(true);
    allocate(*backend);
    auto draw=mesh();draw.depthTest=false;draw.scissorEnabled=true;
    draw.uniforms.flags={0,0,0,1};draw.uniforms.fog={4,0,0,0};
    draw.vertices.resize(6);
    const std::array<std::array<float,2>,6> corners={{{-1,-1},{1,-1},{-1,1},{1,-1},{1,1},{-1,1}}};
    for(size_t i=0;i<corners.size();++i) {
        draw.vertices[i].position={corners[i][0],corners[i][1],-0.25f,1};
        draw.vertices[i].extra[0]=1;
    }
    auto render=[&](bool serialized) {
        clear(*backend);
        // More than two complete batches reuse one material while every draw
        // has a distinct uniform offset. Reading each draw is the independent
        // reference: no later uniform can affect an already completed image.
        for(unsigned i=0;i<768;++i) {
            const unsigned cell=i%256;
            draw.scissor={GLint(cell%16),GLint(cell/16),1,1};
            draw.uniforms.fogColor={float(cell%16)/15.f,float(cell/16)/15.f,float(i/256)/2.f,0};
            backend->draw(draw);
            if(serialized)(void)pixels(*backend);
        }
        return pixels(*backend);
    };
    const auto reference=render(true);
    equalPixels(reference,render(false),"queued dynamic uniform offsets");
    EXPECT_NE(reference[0],reference[15*4]);
}
TEST_F(HikaruVulkanTest, MaskedClearsPreserveChannelsScissorDepthAndQueries) {
    for(GLenum format:{GL_RGBA8,GL_RGB10_A2})for(unsigned target:{0u,20u}) {
        SCOPED_TRACE(testing::Message()<<"format "<<format<<" target "<<target);
        auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
        hikaru::graphics::Texture texture;texture.width=texture.height=16;texture.format=format;
        backend->texture(target,texture,0,TextureUpload::Definition,nullptr,0);
        auto state=mesh();state.color=target;
        backend->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0.2f,0.4f,0.6f,1},1);
        enableVulkanWaitProfilingForTesting(*backend);
        state.scissorEnabled=true;state.scissor={2,3,4,5};state.colorMask={true,false,false,true};
        backend->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0.8f,0.9f,0.1f,0.25f},0);
        auto counts=vulkanReuseCountsForTesting(*backend);
        EXPECT_EQ(counts[6],1u);EXPECT_EQ(counts[7],0u); // no host readback for the clear
        std::vector<uint8_t> result(16*16*4);backend->readPixels(target,0,0,16,16,result.data());
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const size_t at=(y*16+x)*4;
            const bool inside=x>=2&&x<6&&y>=3&&y<8;
            EXPECT_NEAR(result[at],inside?204:51,1);
            EXPECT_NEAR(result[at+1],102,1);EXPECT_NEAR(result[at+2],153,1);
            EXPECT_EQ(result[at+3],inside?(format==GL_RGB10_A2?85:64):255);
        }
        auto probe=fragmentMesh(-0.25f,{1,1,1,1});probe.color=target;
        probe.uniforms.peel[0]=0;probe.uniforms.inverted[2]=target?1.f:0.f;
        probe.depthTest=true;probe.depthWrite=false;probe.scissorEnabled=true;probe.scissor=state.scissor;
        probe.colorMask={false,false,false,false};
        backend->beginOcclusion();backend->draw(probe);EXPECT_FALSE(backend->endOcclusion());
        state.colorMask={false,false,false,false};state.depthWrite=false;
        backend->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{1,1,1,1},1);
        backend->beginOcclusion();backend->draw(probe);EXPECT_FALSE(backend->endOcclusion());
        state.depthWrite=true;
        backend->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{},1);
        backend->beginOcclusion();backend->draw(probe);EXPECT_TRUE(backend->endOcclusion());
        std::vector<uint8_t> after(result.size());backend->readPixels(target,0,0,16,16,after.data());
        equalPixels(result,after,"all-false mask and depth-only clear preserve color");
        state.colorMask={true,false,false,false};state.scissor={30,30,3,3};
        const auto beforeEmpty=vulkanReuseCountsForTesting(*backend);
        backend->clear(state,GL_COLOR_BUFFER_BIT,{1,1,1,1},1);
        EXPECT_EQ(vulkanReuseCountsForTesting(*backend)[6],beforeEmpty[6]);
        state.scissor={2,3,4,5};
        backend->beginOcclusion();backend->clear(state,GL_COLOR_BUFFER_BIT,{},1);
        EXPECT_FALSE(backend->endOcclusion());
    }
}
TEST_F(HikaruVulkanTest, MaskedIntegerClearPreservesCoverageOwner) {
    auto backend=makeVulkanBackendForTesting(true);allocate(*backend);
    hikaru::graphics::Texture metadata;metadata.width=metadata.height=16;metadata.maxLevel=0;metadata.format=GL_RG32UI;
    std::array<uint32_t,16*16*2> first{},second{};
    for(size_t i=0;i<first.size();i+=2){first[i]=30;first[i+1]=1;second[i]=20;second[i+1]=2;}
    backend->texture(20,metadata,0,TextureUpload::Definition,first.data(),sizeof(first));
    backend->texture(21,metadata,0,TextureUpload::Definition,second.data(),sizeof(second));
    hikaru::graphics::Texture color=metadata;color.format=GL_RGBA8;
    std::array<uint8_t,16*16*4> red{},green{};
    for(size_t i=0;i<red.size();i+=4){red[i]=red[i+3]=green[i+1]=green[i+3]=255;}
    backend->texture(30,color,0,TextureUpload::Definition,red.data(),red.size());
    backend->texture(31,color,0,TextureUpload::Definition,green.data(),green.size());
    Draw resolve;resolve.bitmap=resolve.familyResolve=true;resolve.viewport={0,0,16,16};resolve.textures={30,31,20,21};
    for(const auto& point:std::array<std::array<float,2>,6>{{{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}}}) {
        Vertex vertex;vertex.position={point[0],point[1],0,1};resolve.vertices.push_back(vertex);
    }
    clear(*backend);backend->draw(resolve);const auto before=pixels(*backend);
    EXPECT_EQ(before[(4*16+4)*4+1],255);
    Draw reset;reset.color=20;reset.colorMask={true,false,false,false};
    backend->clear(reset,GL_COLOR_BUFFER_BIT,{},1);
    backend->draw(resolve);const auto after=pixels(*backend);
    // Clearing only raw depth must retain the integer owner/coverage channel.
    EXPECT_EQ(after[(4*16+4)*4],255);EXPECT_EQ(after[(4*16+4)*4+1],0);
}
TEST_F(HikaruVulkanTest, ImmutableUploadReuseSurvivesBatchRollover) {
    auto reference=makeVulkanBackendForTesting(true),backend=makeVulkanBackendForTesting(true);
    allocate(*reference);allocate(*backend);clear(*reference);clear(*backend);
    enableVulkanWaitProfilingForTesting(*backend);
    for(unsigned i=0;i<768;++i) {
        auto draw=fragmentMesh(-0.25f,{1,1,1,1});draw.uniforms.peel[0]=0;
        draw.uniforms.flags={0,0,0,1};draw.uniforms.fog={4,0,0,0};
        const unsigned group=i/4,cell=i%256;
        draw.uniforms.fogColor={float(group%16)/15.f,float((group/16)%16)/15.f,float(i/256)/2.f,0};
        draw.uniforms.family[2]=float(i+1); // unused owner rank must not defeat reuse
        draw.scissorEnabled=true;draw.scissor={GLint(cell%16),GLint(cell/16),1,1};
        for(unsigned record=0;record<1+group%2;++record) {
            DrawRecord value;value.modelview=draw.uniforms.modelview;value.alternate=draw.uniforms.alternate;
            value.normal=draw.uniforms.normal;value.alternateNormal=draw.uniforms.alternateNormal;
            value.modes={1,1,0,0};draw.palette.push_back(value);
        }
        reference->draw(draw);(void)pixels(*reference);
        backend->draw(draw);
    }
    const auto counts=vulkanReuseCountsForTesting(*backend);
    EXPECT_EQ(counts[0],192u);EXPECT_EQ(counts[1],576u);
    EXPECT_EQ(counts[2],192u);EXPECT_EQ(counts[3],576u);
    EXPECT_GT(counts[5],700u);
    equalPixels(pixels(*reference),pixels(*backend),"immutable uniform/palette ranges across batches");
}
TEST_F(HikaruVulkanTest, PipelineShortcutSurvivesEvictionAndGpuClear) {
    auto reference=makeVulkanBackendForTesting(false),backend=makeVulkanBackendForTesting(true);
    allocate(*reference);allocate(*backend);clear(*reference);clear(*backend);
    limitVulkanPipelineCacheForTesting(*backend,2);
    enableVulkanWaitProfilingForTesting(*backend);
    for(unsigned i=0;i<9;++i) {
        auto draw=fragmentMesh(-0.25f,{0.4f,0.6f,0.8f,1});draw.uniforms.peel[0]=0;
        draw.uniforms.flags={0,float(i%4),0,float(i%2)};draw.uniforms.material[1]=float(i%5);
        draw.scissorEnabled=true;draw.scissor={GLint(i),2,1,12};
        for(unsigned repeat=0;repeat<2;++repeat) {
            reference->draw(draw);(void)pixels(*reference);backend->draw(draw);
        }
        if(i==4) {
            auto reset=mesh();reset.scissorEnabled=true;reset.scissor={0,0,16,1};reset.colorMask={false,true,false,false};
            reference->clear(reset,GL_COLOR_BUFFER_BIT,{0,0.8f,0,1},1);
            backend->clear(reset,GL_COLOR_BUFFER_BIT,{0,0.8f,0,1},1);
            // The key shortcut can survive this clear, but the binding cannot.
            reference->draw(draw);(void)pixels(*reference);backend->draw(draw);
        }
    }
    EXPECT_GE(vulkanReuseCountsForTesting(*backend)[5],9u);
    equalPixels(pixels(*reference),pixels(*backend),"pipeline cache eviction and special clear bindings",1);
}
TEST_F(HikaruVulkanTest, SpecializedPixelsPalettesAndRetiredBatches) {
    auto generic=makeVulkanBackendForTesting(false),optimized=makeVulkanBackendForTesting(true);
    exerciseNativeGraphics(*generic,*optimized);
#if defined(HAVE_D3D11) && defined(_WIN32)
    // Compare the new HLSL backend against the independent GLSL renderer on
    // actual devices, including all light modes and palette matrix selectors.
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&level,1,
        D3D11_SDK_VERSION,&device.get(),nullptr,&context.get())));
    auto dx11=makeDx11BackendForTesting(device,context,D3DCompile,true);
    exerciseNativeGraphics(*generic,*dx11);
#endif
}
}
#endif
