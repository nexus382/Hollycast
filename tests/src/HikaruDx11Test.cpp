// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtest/gtest.h"
#if defined(HAVE_D3D11) && defined(_WIN32) && !defined(LIBRETRO)
#include "HikaruNativeGraphicsCases.h"
#include "hw/hikaru/graphics/dx11_diagnostics.h"
#include "rend/dx11/dx11_shaders.h"
#include <d3d11sdklayers.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace hikaru::graphics {
std::unique_ptr<Backend> makeDx11BackendForTesting(ID3D11Device*,ID3D11DeviceContext*,pD3DCompile,bool);
std::unique_ptr<Backend> makeBackgroundDx11BackendForTesting(ID3D11Device*,ID3D11DeviceContext*,pD3DCompile);
void completeDx11CompilationForTesting(Backend&);
std::array<uint64_t,3> dx11FragmentCountsForTesting(const Backend&);
std::array<uint64_t,4> dx11FragmentPoolForTesting(const Backend&);
void forceDx11GeneralFragmentResolveForTesting(Backend&);
void disableDx11FragmentListsForTesting(Backend&);
}
using namespace hikaru::graphics;
using namespace hikaru::graphics::test;
namespace {
std::atomic<bool> failCompilation{false};
std::atomic<unsigned> compilationCalls{0};
class ScopedEnvironmentVariable {
    std::string name;
    std::string previous;
    bool wasSet=false;
public:
    ScopedEnvironmentVariable(const char* variable,const char* value):name(variable) {
        if(const char* existing=std::getenv(variable)){previous=existing;wasSet=true;}
        _putenv_s(name.c_str(),value?value:"");
    }
    ~ScopedEnvironmentVariable() {_putenv_s(name.c_str(),wasSet?previous.c_str():"");}
};
HRESULT WINAPI compileWithFailure(LPCVOID source,SIZE_T bytes,LPCSTR name,const D3D_SHADER_MACRO* defines,ID3DInclude* include,
    LPCSTR entry,LPCSTR profile,UINT flags,UINT flags2,ID3DBlob** code,ID3DBlob** errors) {
    ++compilationCalls;
    if(failCompilation.exchange(false))return E_FAIL;
    return D3DCompile(source,bytes,name,defines,include,entry,profile,flags,flags2,code,errors);
}
class HikaruDx11Test : public testing::Test {
protected:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> diagnostics;
    void SetUp() override {
        if(!std::getenv("HOLLYCAST_NATIVE_DX11_TESTS"))GTEST_SKIP()<<"requires explicitly requested native DX11 checks";
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        HRESULT result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,&level,1,
            D3D11_SDK_VERSION,&device.get(),nullptr,&context.get());
        if(result==DXGI_ERROR_SDK_COMPONENT_MISSING)
            result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,
                D3D11_SDK_VERSION,&device.get(),nullptr,&context.get());
        ASSERT_TRUE(SUCCEEDED(result))<<std::hex<<unsigned(result);
        if(SUCCEEDED(device.as(diagnostics)))diagnostics->ClearStoredMessages();
    }
    void TearDown() override {
        if(diagnostics)for(UINT64 i=0;i<diagnostics->GetNumStoredMessages();++i) {
            SIZE_T size=0;diagnostics->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if(SUCCEEDED(diagnostics->GetMessage(i,message,&size))&&message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR)
                ADD_FAILURE()<<message->pDescription;
        }
        if(context){context->ClearState();context->Flush();}
    }
};
TEST_F(HikaruDx11Test, MaterialsPalettesAndResourceLifetime) {
    auto generic=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto optimized=makeDx11BackendForTesting(device,context,D3DCompile,true);
    exerciseNativeGraphics(*generic,*optimized);
}
TEST_F(HikaruDx11Test, BackgroundCompilationMatchesGenericAndReleasesPendingWork) {
    auto generic=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto background=makeBackgroundDx11BackendForTesting(device,context,D3DCompile);
    allocate(*generic);allocate(*background);
    for(bool palette:{false,true})for(bool textured:{false,true}) {
        auto draw=mesh();draw.depthTest=false;draw.uniforms.flags={float(textured),1,1,1};
        draw.uniforms.lights[0].flags={1,2,1,1};
        if(palette) {
            DrawRecord record;record.modelview=draw.uniforms.modelview;record.alternate=draw.uniforms.alternate;
            record.normal=draw.uniforms.normal;record.alternateNormal=draw.uniforms.alternateNormal;
            record.modes={1,2,2,1};record.sampling[0]=-0.5f;draw.palette.push_back(record);
            draw.paletteTextures.push_back(10);draw.paletteSampling.push_back(draw.sampling[0]);draw.texturePalette=textured;
        }
        clear(*generic);generic->draw(draw);const auto expected=pixels(*generic);
        clear(*background);background->draw(draw);
        equalPixels(expected,pixels(*background),"background generic fallback",1);
        completeDx11CompilationForTesting(*background);
        background->draw(draw);equalPixels(expected,pixels(*background),"completed background specialization",1);
    }
    exerciseNativeGraphics(*generic,*background);completeDx11CompilationForTesting(*background);
    // Dropping the renderer during a new variant must join the worker before
    // its compiler/device can be released, without touching the context there.
    allocate(*background); // The stress helper resized the output attachments.
    auto pending=mesh();pending.uniforms.family={1,1,0,0};background->draw(pending);background.reset();
}
TEST_F(HikaruDx11Test, FailedBackgroundSpecializationKeepsExactFallbackWithoutRetries) {
    auto generic=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto background=makeBackgroundDx11BackendForTesting(device,context,compileWithFailure);
    allocate(*generic);allocate(*background);auto draw=mesh();
    clear(*generic);generic->draw(draw);const auto expected=pixels(*generic);
    failCompilation=true;clear(*background);background->draw(draw);completeDx11CompilationForTesting(*background);
    equalPixels(expected,pixels(*background),"failed specialization fallback",1);
    const unsigned calls=compilationCalls.load();
    background->draw(draw);completeDx11CompilationForTesting(*background);
    equalPixels(expected,pixels(*background),"cached failed specialization fallback",1);
    EXPECT_EQ(compilationCalls.load(),calls);
}
TEST_F(HikaruDx11Test, WindingRowsMaskedClearsQueriesAndBitmaps) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    auto draw=mesh();draw.cull=true;draw.cullFace=GL_BACK;
    for(auto& vertex:draw.vertices)vertex.extra[0]=1;
    clear(*backend);backend->draw(draw);auto image=pixels(*backend);
    const size_t inside=(3*16+3)*4,outside=(12*16+12)*4;
    EXPECT_EQ(image[inside],255);EXPECT_EQ(image[outside],0);
    draw.frontFace=GL_CW;clear(*backend);backend->draw(draw);image=pixels(*backend);EXPECT_EQ(image[inside],0);
    draw.frontFace=GL_CCW;
    hikaru::graphics::Texture offscreen;offscreen.width=offscreen.height=16;
    backend->texture(3,offscreen,0,TextureUpload::Definition,nullptr,0);
    draw.color=3;draw.uniforms.inverted[2]=1;
    backend->clear(draw,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);backend->draw(draw);
    std::vector<uint8_t> rows(16*16*4);backend->readPixels(3,0,0,16,16,rows.data());
    EXPECT_EQ(rows[inside],255);EXPECT_EQ(rows[outside],0);
    draw=mesh();draw.depthTest=false;draw.scissorEnabled=true;draw.scissor={2,3,4,5};draw.colorMask={true,false,false,false};
    clear(*backend);backend->clear(draw,GL_COLOR_BUFFER_BIT,{1,1,1,0},0);
    image=pixels(*backend);EXPECT_EQ(image[(3*16+2)*4],255);EXPECT_EQ(image[(2*16+2)*4],0);EXPECT_EQ(image[(3*16+2)*4+1],0);EXPECT_EQ(image[(3*16+2)*4+3],255);
    // Clear work is excluded from GL visibility queries. A farther triangle
    // remains occluded unless depth testing is explicitly disabled.
    draw=mesh();clear(*backend);draw.uniforms.material[3]=0.5f;backend->draw(draw);
    draw.uniforms.material[3]=2;backend->beginOcclusion();backend->draw(draw);EXPECT_FALSE(backend->endOcclusion());
    draw.depthTest=false;backend->beginOcclusion();backend->draw(draw);EXPECT_TRUE(backend->endOcclusion());
    backend->beginOcclusion();backend->clear(draw,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);EXPECT_FALSE(backend->endOcclusion());
    const uint8_t scene[]={128,0,0,128};hikaru::graphics::Texture texel;texel.width=texel.height=1;texel.maxLevel=0;
    backend->texture(12,texel,0,TextureUpload::Definition,scene,sizeof(scene));
    Draw bitmap;bitmap.bitmap=true;bitmap.color=0;bitmap.viewport={0,0,16,16};bitmap.textures={12,12,11,0};
    bitmap.uniforms.factors={0,1,1,1};bitmap.uniforms.multipliers={1,0,0,0};bitmap.uniforms.keyed={1,0,0,0};
    const std::array<std::array<float,2>,6> points={{{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}}};
    for(auto point:points){Vertex vertex;vertex.position={point[0],point[1],0,1};vertex.uv={point[0],point[1],0,0};bitmap.vertices.push_back(vertex);}
    backend->clear(bitmap,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);backend->draw(bitmap);image=pixels(*backend);
    EXPECT_NEAR(image[inside],128,1);EXPECT_NEAR(image[inside+1],127,1);EXPECT_EQ(image[inside+2],0);EXPECT_EQ(image[inside+3],255);
    bitmap.uniforms.direct[0]=1;backend->draw(bitmap);image=pixels(*backend);EXPECT_EQ(image[inside],0);EXPECT_EQ(image[inside+1],255);
    // At 16:9 the 4:3 overlay covers the middle twelve pixels of this row.
    // The 3D scene remains visible in both two-pixel side bands.
    bitmap.uniforms.factors[3]=0.75f;
    backend->draw(bitmap);image=pixels(*backend);
    for(unsigned x=0;x<16;++x) {
        const size_t pixel=(3*16+x)*4;
        const bool covered=x>=2&&x<14;
        EXPECT_EQ(image[pixel],covered?0:128) << x;
        EXPECT_EQ(image[pixel+1],covered?255:0) << x;
    }
    bitmap.uniforms.factors[3]=1.f;
    // Nonuniform texels catch a bitmap input layout that accidentally maps
    // UVs to the mesh shader's optimized-away attribute registers.
    const std::array<uint8_t,16> gradient={255,0,0,255,255,0,0,255,0,255,0,255,0,255,0,255};
    texel.width=texel.height=2;backend->texture(13,texel,0,TextureUpload::Definition,gradient.data(),gradient.size());
    bitmap.textures[0]=13;bitmap.uniforms.multipliers={};backend->draw(bitmap);image=pixels(*backend);
    EXPECT_EQ(image[(3*16+3)*4+1],255);EXPECT_EQ(image[(12*16+3)*4],255);
    bitmap.uniforms.inverted[0]=1;backend->draw(bitmap);image=pixels(*backend);
    EXPECT_EQ(image[(3*16+3)*4],255);EXPECT_EQ(image[(12*16+3)*4+1],255);
}
TEST_F(HikaruDx11Test, PackedVerticesMatchExpandedLightingAndMixedStreams) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    exercisePackedVertices(*backend);
}
TEST_F(HikaruDx11Test, SpecializedPassControlsMatchGenericDepthAndCoverage) {
    auto generic=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto optimized=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*generic);allocate(*optimized);
    for(unsigned i=0;i<24;++i) {
        auto draw=mesh();draw.uniforms.flags={0,0,0,0};draw.uniforms.family={float(i%3),float((i/3)%2),1,65535};
        draw.uniforms.peel={float((i/6)%3!=0),float((i/6)%3==1),16,16};
        draw.uniforms.blend={float(i%2),float((i/2)%2),0,0};
        draw.uniforms.projection[10]=0; // Keep distant geometry inside clip Z.
        const float z=i%3==0?0.25f:i%3==1?100.f:25000.f;
        for(auto& vertex:draw.vertices)vertex.position[2]=-z;
        draw.uniforms.material[3]=float(i%3); // disabled, common and separate depths
        draw.textures[1]=41;draw.textures[2]=42;draw.textures[3]=43;
        hikaru::graphics::Texture depth;depth.width=depth.height=16;depth.format=GL_DEPTH_COMPONENT24;depth.maxLevel=0;
        hikaru::graphics::Texture metadata=depth;metadata.format=GL_RG32UI;
        std::array<uint32_t,16*16*2> owners{};
        for(size_t j=0;j<owners.size();j+=2){owners[j]=i%2?0xffffffu:0u;owners[j+1]=1;}
        for(auto* backend:{generic.get(),optimized.get()}) {
            backend->texture(41,depth,0,TextureUpload::Definition,nullptr,0);backend->texture(42,depth,0,TextureUpload::Definition,nullptr,0);
            backend->texture(43,metadata,0,TextureUpload::Definition,owners.data(),sizeof(owners));
            auto reset=mesh();reset.depth=41;backend->clear(reset,GL_DEPTH_BUFFER_BIT,{},i%2?0.01f:0.4f);
            reset.depth=42;backend->clear(reset,GL_DEPTH_BUFFER_BIT,{},0.97f);
        }
        clear(*generic);generic->draw(draw);const auto expected=pixels(*generic);
        clear(*optimized);optimized->draw(draw);equalPixels(expected,pixels(*optimized),"specialized family/peel/depth/coverage",1);
    }
}
TEST_F(HikaruDx11Test, ConstantStreamsWrapAndRestoreAfterClears) {
    auto generic=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto optimized=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*generic);allocate(*optimized);
    auto draw=mesh();draw.depthTest=false;draw.uniforms.flags={0,0,0,1};
    auto render=[&](Backend& backend) {
        clear(backend);
        // More than three complete constant-buffer windows, with both stages
        // changing independently of geometry. The generic device forces the
        // legacy path; optimized devices stream only when both caps exist.
        for(unsigned i=0;i<192;++i) {
            draw.uniforms.modelview[12]=float(i%7)*0.01f;
            draw.uniforms.fogColor={float(i%17)/16.f,0.1f,0.3f,0};draw.uniforms.inverted[2]=float(i%2);
            backend.draw(draw);
        }
        backend.draw(draw);return pixels(backend);
    };
    const auto expected=render(*generic);equalPixels(expected,render(*optimized),"constant stream wrap",1);
    auto reset=draw;reset.scissorEnabled=true;reset.scissor={0,0,16,16};
    generic->clear(reset,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);generic->draw(draw);const auto afterClear=pixels(*generic);
    optimized->clear(reset,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);optimized->draw(draw);
    equalPixels(afterClear,pixels(*optimized),"clear discards only the pixel stream",1);
}
TEST_F(HikaruDx11Test, UnchangedConstantsSurviveShaderClears) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    auto draw=mesh();draw.depthTest=false;
    clear(*backend);backend->draw(draw);const auto expected=pixels(*backend);
    // Scissored clears use the same constant buffer as mesh draws. Repeating
    // an identical draw must restore its constants even when all its bindings
    // and CPU-side material values still match the preceding draw.
    auto reset=draw;reset.scissorEnabled=true;reset.scissor={0,0,16,16};
    backend->clear(reset,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
    backend->draw(draw);equalPixels(expected,pixels(*backend),"unchanged constants after shader clear");
    // Replacing sampled storage invalidates resource bindings without changing
    // the material constants. The next draw must sample the new image.
    const uint8_t blue[]={0,0,255,255};hikaru::graphics::Texture source;source.width=source.height=1;source.maxLevel=0;
    backend->texture(10,source,0,TextureUpload::Definition,blue,sizeof(blue));
    backend->draw(draw);const auto output=pixels(*backend);const size_t center=(3*16+3)*4;
    EXPECT_EQ(output[center],0);EXPECT_EQ(output[center+1],0);EXPECT_GT(output[center+2],0);
}
TEST_F(HikaruDx11Test, IntegerFamilyMetadataAndResolve) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    hikaru::graphics::Texture metadata;metadata.width=metadata.height=16;metadata.maxLevel=0;metadata.format=GL_RG32UI;
    std::array<uint32_t,16*16*2> first{},second{};
    for(size_t i=0;i<first.size();i+=2){first[i]=10;first[i+1]=1;second[i]=20;second[i+1]=2;}
    backend->texture(20,metadata,0,TextureUpload::Definition,first.data(),sizeof(first));
    backend->texture(21,metadata,0,TextureUpload::Definition,second.data(),sizeof(second));
    hikaru::graphics::Texture color=metadata;color.format=GL_RGBA8;
    std::array<uint8_t,16*16*4> red{},green{};
    for(size_t i=0;i<red.size();i+=4){red[i]=red[i+3]=green[i+1]=green[i+3]=255;}
    backend->texture(30,color,0,TextureUpload::Definition,red.data(),red.size());
    backend->texture(31,color,0,TextureUpload::Definition,green.data(),green.size());
    Draw resolve;resolve.bitmap=true;resolve.familyResolve=true;resolve.color=0;resolve.viewport={0,0,16,16};resolve.textures={30,31,20,21};
    const std::array<std::array<float,2>,6> points={{{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}}};
    for(auto point:points){Vertex v;v.position={point[0],point[1],0,1};resolve.vertices.push_back(v);}
    clear(*backend);backend->draw(resolve);auto output=pixels(*backend);EXPECT_EQ(output[(4*16+4)*4],255);EXPECT_EQ(output[(4*16+4)*4+1],0);
    for(size_t i=0;i<second.size();i+=2)second[i]=10;
    backend->texture(21,metadata,0,TextureUpload::FullUpdate,second.data(),sizeof(second));
    backend->draw(resolve);output=pixels(*backend);EXPECT_EQ(output[(4*16+4)*4],0);EXPECT_EQ(output[(4*16+4)*4+1],255);
    // Shader-written integer metadata must survive an MRT pass and then a
    // sample transition into the resolver; zero owner means no coverage.
    Draw clearMetadata;clearMetadata.color=20;clearMetadata.viewport={0,0,16,16};
    backend->clear(clearMetadata,GL_COLOR_BUFFER_BIT,{},1);clearMetadata.color=21;backend->clear(clearMetadata,GL_COLOR_BUFFER_BIT,{},1);
    auto meshDraw=mesh();meshDraw.color=30;meshDraw.metadata=20;meshDraw.uniforms.inverted[2]=1;meshDraw.uniforms.family={0,1,1,65535};
    backend->clear(meshDraw,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);backend->draw(meshDraw);
    // Family resolution runs inside the scene FBO, whose rows follow GL.
    // Use that path here rather than the top-left presentation framebuffer.
    backend->texture(3,color,0,TextureUpload::Definition,nullptr,0);resolve.color=3;resolve.uniforms.inverted[2]=1;
    backend->clear(resolve,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);backend->draw(resolve);
    backend->readPixels(3,0,0,16,16,output.data());
    EXPECT_GT(output[(3*16+3)*4],0);EXPECT_EQ(output[(12*16+12)*4],0);
}
Draw fragmentMesh(float z,const Vec4& color,unsigned mode=0) {
    auto draw=mesh();draw.depthTest=false;draw.depthWrite=false;
    draw.uniforms.flags={};draw.uniforms.blend={0,mode==1?1.f:0.f,float(mode),0};
    draw.uniforms.peel={1,1,16,16};draw.textures={0,0,2,0};
    draw.vertices.resize(6);
    const std::array<std::array<float,2>,6> corners={{{-1,-1},{1,-1},{-1,1},{1,-1},{1,1},{-1,1}}};
    for(size_t i=0;i<corners.size();++i) {
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
std::vector<uint8_t> targetPixels(Backend& backend,unsigned id,unsigned extent) {
    std::vector<uint8_t> result(size_t(extent)*extent*4);
    backend.readPixels(id,0,0,int(extent),int(extent),result.data());
    return result;
}
void clearTarget(Backend& backend,unsigned id,unsigned extent,const Vec4& color) {
    auto state=mesh();state.color=id;state.depth=0;state.viewport={0,0,int(extent),int(extent)};
    backend.clear(state,GL_COLOR_BUFFER_BIT,color,1);
}
TEST_F(HikaruDx11Test, FragmentListsMatchMixedBlendsAndD24TiesAcrossSceneFormats) {
    auto reference=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto lists=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*reference);allocate(*lists);
    std::vector<Draw> layers;
    for(unsigned i=0;i<12;++i) {
        layers.push_back(fragmentMesh(-0.1f*float(i%4+1),
            {float(17*(i%5+1))/255.f,float(13*(i%7+1))/255.f,0.2f,0.4f},i%4));
    }
    const Vec4 background={0.2f,0.4f,0.6f,0.8f};
    for(GLenum format:{GL_RGBA8,GL_RGB10_A2}) {
        hikaru::graphics::Texture target;target.width=target.height=16;target.format=format;
        reference->texture(0,target,0,TextureUpload::Definition,nullptr,0);
        lists->texture(0,target,0,TextureUpload::Definition,nullptr,0);
        auto state=mesh();
        reference->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
        auto ordered=layers;
        std::stable_sort(ordered.begin(),ordered.end(),[](const Draw& a,const Draw& b) {
            return a.vertices[0].position[2]<b.vertices[0].position[2];
        });
        for(const auto& draw:ordered)drawOrderedFragment(*reference,draw);
        const auto expected=pixels(*reference);
        lists->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
        ASSERT_TRUE(lists->beginFragmentList(0,1024*1024));
        for(const auto& draw:layers)lists->draw(draw);
        ASSERT_TRUE(lists->endFragmentList());
        equalPixels(expected,pixels(*lists),format==GL_RGBA8?"RGBA8 blend/depth ties":"RGB10A2 quantized blend/depth ties",
            format==GL_RGBA8?1:2);
    }
}
TEST_F(HikaruDx11Test, FragmentListsMatchThreeHundredScissoredLayersAcrossVertexStreamWrap) {
    auto reference=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto lists=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*reference);allocate(*lists);
    const auto render=[](Backend& backend,bool collect) {
        clear(backend);
        if(collect&&!backend.beginFragmentList(0,1024*1024)) {
            ADD_FAILURE()<<"DX11 rejected a valid 300-layer collection";
            return std::vector<uint8_t>{};
        }
        auto wrap=mesh();wrap.depthTest=false;wrap.vertices.resize(8190,wrap.vertices[0]);
        for(auto& vertex:wrap.vertices)vertex.position={3,3,-0.25f,1};
        if(collect)backend.draw(wrap);else drawOrderedFragment(backend,wrap);
        for(unsigned i=0;i<300;++i) {
            auto draw=fragmentMesh(-0.25f,{float(17*(i%8))/255.f,51.f/255.f,float(17*(i%3))/255.f,26.f/255.f},i%4);
            draw.scissorEnabled=true;draw.scissor={4,4,2,2};
            if(collect)backend.draw(draw);else drawOrderedFragment(backend,draw);
        }
        if(collect)EXPECT_TRUE(backend.endFragmentList());
        return pixels(backend);
    };
    const auto expected=render(*reference,false);
    equalPixels(expected,render(*lists,true),"300 tied layers across vertex-stream wrap",1);
}
TEST_F(HikaruDx11Test, FragmentListOverflowPreservesColorForFallbackAndResize) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    const Vec4 background={0.2f,0.3f,0.4f,1};
    backend->clear(mesh(),GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
    const auto before=pixels(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,16*16*8+32+64));
    backend->draw(fragmentMesh(-0.25f,{1,0,0,0.5f}));
    EXPECT_FALSE(backend->endFragmentList());
    equalPixels(before,pixels(*backend),"overflow must leave opaque target unchanged");
    drawOrderedFragment(*backend,fragmentMesh(-0.25f,{1,0,0,0.5f}));
    EXPECT_NE(pixels(*backend),before)<<"ordered fallback must remain usable";
    hikaru::graphics::Texture resized;resized.width=resized.height=32;
    backend->texture(0,resized,0,TextureUpload::Definition,nullptr,0);
    hikaru::graphics::Texture depth=resized;depth.format=GL_DEPTH_COMPONENT24;
    backend->texture(2,depth,0,TextureUpload::Definition,nullptr,0);
    auto state=mesh();state.viewport={0,0,32,32};
    backend->clear(state,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,background,1);
    const auto resizedBefore=pixels(*backend,32);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(resizedBefore,pixels(*backend,32),"empty collection after resize");
    const auto counts=dx11FragmentCountsForTesting(*backend);
    EXPECT_EQ(counts[0],2u);EXPECT_EQ(counts[1],1u);EXPECT_EQ(counts[2],1u);
}
TEST_F(HikaruDx11Test, FragmentListsPreserveOpaqueDepthPatternFogAndPackedStripShading) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    auto opaque=fragmentMesh(-0.2f,{0.1f,0.4f,0.7f,1});
    opaque.depthTest=opaque.depthWrite=true;opaque.uniforms.peel[0]=0;
    clear(*backend);backend->draw(opaque);const auto opaqueOnly=pixels(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    backend->draw(fragmentMesh(-0.8f,{1,0,0,0.5f}));
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(opaqueOnly,pixels(*backend),"opaque depth rejects farther transparency",1);
    ASSERT_TRUE(backend->supportsPackedVertices());
    for(bool packed:{false,true}) {
        auto strip=fragmentMesh(-0.1f,{128.f/255.f,64.f/255.f,1,128.f/255.f});
        strip.topology=GL_TRIANGLE_STRIP;strip.uniforms.family[3]=float(0xa5a5);
        strip.uniforms.flags={0,0,0,1};strip.uniforms.fog={2,0,0,0};strip.uniforms.fogColor={0.2f,0.3f,0.4f,0};
        strip.vertices.resize(4);
        const std::array<std::array<float,2>,4> corners={{{-1,-1},{1,-1},{-1,1},{1,1}}};
        for(size_t i=0;i<corners.size();++i) {
            strip.vertices[i].position={corners[i][0],corners[i][1],-0.1f,1};
            strip.vertices[i].diffuse={128.f/255.f,64.f/255.f,1,1};strip.vertices[i].extra[0]=128.f/255.f;
        }
        if(packed) {
            for(const auto& source:strip.vertices) {
                PackedVertex vertex;std::copy_n(source.position.begin(),3,vertex.position.begin());
                vertex.diffuse={128,64,255};vertex.alpha=128;strip.packedVertices.push_back(vertex);
            }
            strip.vertices.clear();
        }
        clear(*backend);drawOrderedFragment(*backend,strip);const auto expected=pixels(*backend);
        clear(*backend);
        ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
        backend->draw(strip);ASSERT_TRUE(backend->endFragmentList());
        equalPixels(expected,pixels(*backend),packed?"packed strip fog/pattern":"expanded strip fog/pattern",1);
    }
}
TEST_F(HikaruDx11Test, FragmentResolverBoundariesMatchGenericAndSpecializedPipelines) {
    auto general=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto shortLists=makeDx11BackendForTesting(device,context,D3DCompile,true);
    forceDx11GeneralFragmentResolveForTesting(*general);
    allocate(*general);allocate(*shortLists);
    for(unsigned count:{0u,1u,2u,7u,8u,9u,16u,64u,300u}) {
        SCOPED_TRACE(count);
        const auto render=[&](Backend& backend) {
            clear(backend);const auto before=pixels(backend);
            if(!backend.beginFragmentList(0,1024*1024)) {
                ADD_FAILURE()<<"DX11 rejected resolver boundary "<<count;
                return std::vector<uint8_t>{};
            }
            for(unsigned i=0;i<count;++i) {
                auto draw=fragmentMesh(-0.1f*float(i%4+1),
                    {float(17*(i%8))/255.f,51.f/255.f,float(17*(i%3))/255.f,102.f/255.f},i%4);
                draw.scissorEnabled=true;draw.scissor={4,4,2,2};backend.draw(draw);
            }
            EXPECT_TRUE(backend.endFragmentList());
            auto result=pixels(backend);
            if(count)EXPECT_NE(result,before)<<"nonempty resolver must produce visible fragments";
            return result;
        };
        equalPixels(render(*general),render(*shortLists),"general/small resolver boundary");
    }
}
TEST_F(HikaruDx11Test, FragmentPoolReusesSameSizeTargetsAndTracksExtentFormatBudget) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    constexpr uint64_t budget=1024*1024;
    ASSERT_TRUE(backend->beginFragmentList(0,budget));
    const auto initial=dx11FragmentPoolForTesting(*backend);
    ASSERT_GE(initial[0],4u);EXPECT_EQ(initial[0]%4,0u);EXPECT_GE(initial[2],initial[0]*16);
    ASSERT_TRUE(backend->endFragmentList());
    for(unsigned i=0;i<3;++i) {
        ASSERT_TRUE(backend->beginFragmentList(0,budget));
        ASSERT_TRUE(backend->endFragmentList());
        EXPECT_EQ(dx11FragmentPoolForTesting(*backend)[1],initial[1]);
    }
    hikaru::graphics::Texture alternate;alternate.width=alternate.height=16;
    backend->texture(20,alternate,0,TextureUpload::Definition,nullptr,0);
    const Vec4 alternateColor={0.2f,0.3f,0.4f,1};clearTarget(*backend,20,16,alternateColor);
    const auto sameSizeBefore=targetPixels(*backend,20,16);
    ASSERT_TRUE(backend->beginFragmentList(20,budget));
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(sameSizeBefore,targetPixels(*backend,20,16),"empty collection on same-size target switch");
    EXPECT_EQ(dx11FragmentPoolForTesting(*backend)[1],initial[1])<<"equal-size target switch should reuse storage";
    hikaru::graphics::Texture resized;resized.width=resized.height=32;
    backend->texture(20,resized,0,TextureUpload::Definition,nullptr,0);
    clearTarget(*backend,20,32,alternateColor);const auto extentBefore=targetPixels(*backend,20,32);
    ASSERT_TRUE(backend->beginFragmentList(20,budget));
    const auto extentPool=dx11FragmentPoolForTesting(*backend);
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(extentBefore,targetPixels(*backend,20,32),"empty collection after extent change");
    EXPECT_GT(extentPool[1],initial[1]);
    resized.format=GL_RGB10_A2;backend->texture(20,resized,0,TextureUpload::Definition,nullptr,0);
    clearTarget(*backend,20,32,alternateColor);const auto formatBefore=targetPixels(*backend,20,32);
    ASSERT_TRUE(backend->beginFragmentList(20,budget));
    const auto formatPool=dx11FragmentPoolForTesting(*backend);
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(formatBefore,targetPixels(*backend,20,32),"empty collection after format change");
    EXPECT_GT(formatPool[1],extentPool[1]);
    const uint64_t fixed=32*32*8+32;
    ASSERT_TRUE(backend->beginFragmentList(20,fixed+64));
    EXPECT_EQ(dx11FragmentPoolForTesting(*backend)[0],4u);
    ASSERT_TRUE(backend->endFragmentList());
    EXPECT_FALSE(backend->beginFragmentList(20,fixed));
}
TEST_F(HikaruDx11Test, FragmentListBudgetAndCapabilityFailuresLeaveTargetUsable) {
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    clear(*backend);const auto before=pixels(*backend);
    EXPECT_FALSE(backend->beginFragmentList(0,0));
    equalPixels(before,pixels(*backend),"zero budget leaves opaque color unchanged");
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    ASSERT_TRUE(backend->endFragmentList());
    disableDx11FragmentListsForTesting(*backend);
    const auto unsupportedBefore=pixels(*backend);
    EXPECT_FALSE(backend->beginFragmentList(0,1024*1024));
    equalPixels(unsupportedBefore,pixels(*backend),"unsupported collector leaves opaque color unchanged");
    drawOrderedFragment(*backend,fragmentMesh(-0.25f,{1,0,0,0.5f}));
    EXPECT_NE(pixels(*backend),unsupportedBefore)<<"ordered fallback must remain usable";
    auto unsupported=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*unsupported);
    clear(*unsupported);const auto unsupportedDrawBefore=pixels(*unsupported);
    ASSERT_TRUE(unsupported->beginFragmentList(0,1024*1024));
    auto bitmap=fragmentMesh(-0.25f,{1,0,0,0.5f});bitmap.bitmap=true;
    unsupported->draw(bitmap);
    EXPECT_FALSE(unsupported->endFragmentList());
    equalPixels(unsupportedDrawBefore,pixels(*unsupported),"unsupported draw must not partially commit");
    EXPECT_FALSE(unsupported->beginFragmentList(0,1024*1024));
    drawOrderedFragment(*unsupported,fragmentMesh(-0.25f,{1,0,0,0.5f}));
    EXPECT_NE(pixels(*unsupported),unsupportedDrawBefore)<<"depth-peel fallback remains usable";
}
TEST_F(HikaruDx11Test, FragmentCollectionSurvivesTextureUploadsAndPeelTargetRetirement) {
    auto reference=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*reference);allocate(*backend);
    hikaru::graphics::Texture peelTarget;peelTarget.width=peelTarget.height=16;
    for(auto* renderer:{reference.get(),backend.get()}) {
        renderer->texture(20,peelTarget,0,TextureUpload::Definition,nullptr,0);
        clearTarget(*renderer,20,16,{0.1f,0.2f,0.3f,1});
    }
    const uint8_t red[]={255,0,0,255},blue[]={0,0,255,255};
    hikaru::graphics::Texture decoded;decoded.width=decoded.height=1;decoded.maxLevel=0;
    reference->texture(40,decoded,0,TextureUpload::Definition,red,sizeof(red));
    reference->texture(40,decoded,0,TextureUpload::FullUpdate,blue,sizeof(blue));
    auto textured=fragmentMesh(-0.25f,{1,1,1,0.5f});
    textured.uniforms.flags[0]=1;textured.textures[0]=40;
    clear(*reference);drawOrderedFragment(*reference,textured);const auto expected=pixels(*reference);
    clear(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    // The renderer's decoded-texture miss and peel-target release happen after
    // list collection starts but before its first collected draw.
    backend->texture(40,decoded,0,TextureUpload::Definition,red,sizeof(red));
    backend->texture(40,decoded,0,TextureUpload::FullUpdate,blue,sizeof(blue));
    backend->eraseTexture(20);
    backend->draw(textured);
    ASSERT_TRUE(backend->endFragmentList());
    equalPixels(expected,pixels(*backend),"texture updates and unrelated target retirement during collection",1);
}
TEST_F(HikaruDx11Test, FragmentCollectorCompileFailureFallsBackWithoutRetryStorm) {
    auto reference=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto backend=makeDx11BackendForTesting(device,context,compileWithFailure,true);
    allocate(*reference);allocate(*backend);
    auto draw=fragmentMesh(-0.25f,{1,0,0,0.5f});
    clear(*reference);drawOrderedFragment(*reference,draw);const auto expected=pixels(*reference);
    clear(*backend);const auto before=pixels(*backend);
    compilationCalls.store(0);failCompilation=true;
    EXPECT_FALSE(backend->beginFragmentList(0,1024*1024));
    equalPixels(before,pixels(*backend),"collector compile failure leaves opaque color untouched");
    const unsigned calls=compilationCalls.load();EXPECT_GT(calls,0u);
    EXPECT_FALSE(backend->beginFragmentList(0,1024*1024));
    EXPECT_EQ(compilationCalls.load(),calls)<<"failed collector compilation must not be retried each frame";
    drawOrderedFragment(*backend,draw);
    equalPixels(expected,pixels(*backend),"ordered fallback after collector compile failure",1);
}
TEST_F(HikaruDx11Test, FragmentCollectionRemainsDistinctWhileBackgroundCompilationIsPending) {
    auto reference=makeDx11BackendForTesting(device,context,D3DCompile,false);
    auto background=makeBackgroundDx11BackendForTesting(device,context,D3DCompile);
    allocate(*reference);allocate(*background);
    auto draw=fragmentMesh(-0.25f,{0.2f,0.6f,0.9f,0.4f});
    clear(*reference);drawOrderedFragment(*reference,draw);const auto expected=pixels(*reference);
    clear(*background);
    auto warm=mesh();warm.depthTest=false;warm.uniforms.flags={1,1,1,1};warm.uniforms.lights[0].flags={1,2,1,1};
    background->draw(warm); // Leave a specialized material shader compiling in the background.
    clear(*background);
    ASSERT_TRUE(background->beginFragmentList(0,1024*1024));
    background->draw(draw);ASSERT_TRUE(background->endFragmentList());
    equalPixels(expected,pixels(*background),"collector shader while material specialization is pending",1);
    completeDx11CompilationForTesting(*background);
}
TEST_F(HikaruDx11Test, DiagnosticsFlagsTreatUnsetEmptyAndZeroAsDisabled) {
    const std::array<const char*,3> values={nullptr,"","0"};
    for(const char* value:values) {
        ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING",value);
        ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING",value);
        auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
        const auto snapshot=dx11DiagnosticsForTesting(*backend);
        EXPECT_FALSE(snapshot.enabled);
        EXPECT_FALSE(snapshot.gpuRequested);
        EXPECT_FALSE(snapshot.gpuSupported);
        EXPECT_EQ(snapshot.queriesCreated,0u);
        EXPECT_EQ(snapshot.gpuQueryObjects,0u);
    }
}
TEST_F(HikaruDx11Test, CpuDiagnosticsCountUploadsAndOwnerThreadCompilation) {
    ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING","1");
    ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING",nullptr);
    auto backend=makeBackgroundDx11BackendForTesting(device,context,D3DCompile);
    allocate(*backend);
    const auto before=dx11DiagnosticsForTesting(*backend);
    auto draw=mesh();draw.depthTest=false;draw.uniforms.flags={1,1,1,1};
    draw.uniforms.lights[0].flags={1,2,1,1};
    DrawRecord record;record.modelview=draw.uniforms.modelview;record.alternate=draw.uniforms.alternate;
    record.normal=draw.uniforms.normal;record.alternateNormal=draw.uniforms.alternateNormal;
    draw.palette.push_back(record);
    for(auto& vertex:draw.vertices)vertex.extra[2]=0;
    backend->draw(draw);
    const auto first=dx11DiagnosticsForTesting(*backend);
    EXPECT_EQ(first.shaderCompiles,before.shaderCompiles)<<"worker results are counted only when consumed on the owner thread";
    backend->draw(draw);
    const auto repeated=dx11DiagnosticsForTesting(*backend);
    EXPECT_EQ(repeated.vertexUploads-before.vertexUploads,2u);
    EXPECT_EQ(repeated.vertexBytes-before.vertexBytes,2u*draw.vertices.size()*sizeof(Vertex));
    EXPECT_EQ(repeated.uniformUploads-before.uniformUploads,1u);
    EXPECT_EQ(repeated.uniformHits-before.uniformHits,1u);
    EXPECT_EQ(repeated.uniformBytes-before.uniformBytes,sizeof(Uniforms)+sizeof(draw.uniforms.inverted));
    EXPECT_EQ(repeated.paletteUploads-before.paletteUploads,1u);
    EXPECT_EQ(repeated.paletteHits-before.paletteHits,1u);
    EXPECT_EQ(repeated.paletteBytes-before.paletteBytes,sizeof(DrawRecord));
    EXPECT_EQ(repeated.queriesCreated,0u);
    EXPECT_EQ(repeated.gpuQueryObjects,0u);
    completeDx11CompilationForTesting(*backend);
    const auto completed=dx11DiagnosticsForTesting(*backend);
    EXPECT_GT(completed.shaderCompiles,before.shaderCompiles);
    EXPECT_GE(completed.shaderCompileNanoseconds,first.shaderCompileNanoseconds);
}
TEST_F(HikaruDx11Test, GpuDiagnosticsRetireAsynchronouslyWithoutChangingPixels) {
    ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING","0");
    ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING","1");
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*backend);clear(*backend);backend->draw(mesh());
    const auto expected=pixels(*backend);
    finishDx11DiagnosticFrameForTesting(*backend);
    context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    Dx11DiagnosticsSnapshot snapshot;
    do {
        pollDx11DiagnosticFramesForTesting(*backend);
        snapshot=dx11DiagnosticsForTesting(*backend);
        if(snapshot.frame.samples)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    equalPixels(expected,pixels(*backend),"diagnostic timestamps preserve rendered pixels");
    EXPECT_TRUE(snapshot.enabled);
    EXPECT_TRUE(snapshot.gpuRequested);
    EXPECT_TRUE(snapshot.gpuSupported);
    EXPECT_EQ(snapshot.queriesCreated,0u);
    EXPECT_GT(snapshot.gpuQueryObjects,0u);
    ASSERT_GT(snapshot.frame.samples,0u);
    EXPECT_TRUE(std::isfinite(snapshot.frame.milliseconds));
    EXPECT_GE(snapshot.frame.milliseconds,0.0);
}
TEST_F(HikaruDx11Test, GpuDiagnosticsDiscardSkippedPresentationAndRecover) {
    ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING","0");
    ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING","1");
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*backend);clear(*backend);backend->draw(mesh());
    backend->present(15,16); // Mismatch returns before the offscreen presenter is needed.
    clear(*backend);backend->draw(mesh());
    finishDx11DiagnosticFrameForTesting(*backend);
    context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    Dx11DiagnosticsSnapshot snapshot;
    do {
        pollDx11DiagnosticFramesForTesting(*backend);
        snapshot=dx11DiagnosticsForTesting(*backend);
        if(snapshot.frame.samples)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    EXPECT_EQ(snapshot.frame.samples,1u);
    EXPECT_EQ(snapshot.gpuInvalid,1u);
}
TEST_F(HikaruDx11Test, GpuDiagnosticsKeepFirstOverflowSampleWhenLaterListSucceeds) {
    ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING","0");
    ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING","1");
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*backend);clear(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,16*16*8+32+64));
    backend->draw(fragmentMesh(-0.25f,{1,0,0,0.5f}));
    ASSERT_FALSE(backend->endFragmentList());
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    backend->draw(fragmentMesh(-0.25f,{0,1,0,0.5f}));
    ASSERT_TRUE(backend->endFragmentList());
    finishDx11DiagnosticFrameForTesting(*backend);
    context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    Dx11DiagnosticsSnapshot snapshot;
    do {
        pollDx11DiagnosticFramesForTesting(*backend);
        snapshot=dx11DiagnosticsForTesting(*backend);
        if(snapshot.frame.samples)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    ASSERT_EQ(snapshot.frame.samples,1u);
    EXPECT_EQ(snapshot.clear.samples,1u);
    EXPECT_EQ(snapshot.collect.samples,1u);
    EXPECT_EQ(snapshot.copy.samples,0u);
    EXPECT_EQ(snapshot.resolve.samples,0u);
    EXPECT_EQ(snapshot.gpuInvalid,0u);
}
TEST_F(HikaruDx11Test, GpuDiagnosticsReportOnlyCompletedFragmentListPhases) {
    ScopedEnvironmentVariable timing("HIKARU_TRACE_TIMING","0");
    ScopedEnvironmentVariable gpuTiming("HIKARU_TRACE_GPU_TIMING","1");
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);
    allocate(*backend);clear(*backend);
    ASSERT_TRUE(backend->beginFragmentList(0,1024*1024));
    backend->draw(fragmentMesh(-0.25f,{0.2f,0.6f,0.9f,0.5f}));
    ASSERT_TRUE(backend->endFragmentList());
    const auto expected=pixels(*backend);
    finishDx11DiagnosticFrameForTesting(*backend);
    context->Flush();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    Dx11DiagnosticsSnapshot snapshot;
    do {
        pollDx11DiagnosticFramesForTesting(*backend);
        snapshot=dx11DiagnosticsForTesting(*backend);
        if(snapshot.frame.samples)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    equalPixels(expected,pixels(*backend),"fragment timing preserves resolved pixels");
    ASSERT_GT(snapshot.frame.samples,0u);
    EXPECT_GT(snapshot.clear.samples,0u);
    EXPECT_GT(snapshot.collect.samples,0u);
    EXPECT_GT(snapshot.copy.samples,0u);
    EXPECT_GT(snapshot.resolve.samples,0u);
    for(const double milliseconds:{snapshot.frame.milliseconds,snapshot.clear.milliseconds,snapshot.collect.milliseconds,
        snapshot.copy.milliseconds,snapshot.resolve.milliseconds}) {
        EXPECT_TRUE(std::isfinite(milliseconds));
        EXPECT_GE(milliseconds,0.0);
    }
}

}
#endif
