// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gtest/gtest.h"
#if defined(HAVE_D3D11) && defined(_WIN32) && !defined(LIBRETRO)
#include "HikaruNativeGraphicsCases.h"
#include "rend/dx11/dx11_shaders.h"
#include <d3d11sdklayers.h>
#include <cstdlib>

namespace hikaru::graphics {
std::unique_ptr<Backend> makeDx11BackendForTesting(ID3D11Device*,ID3D11DeviceContext*,pD3DCompile,bool);
}
using namespace hikaru::graphics;
using namespace hikaru::graphics::test;
namespace {
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
    bitmap.uniforms.factors={0,1,1,0};bitmap.uniforms.multipliers={1,0,0,0};bitmap.uniforms.keyed={1,0,0,0};
    const std::array<std::array<float,2>,6> points={{{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}}};
    for(auto point:points){Vertex vertex;vertex.position={point[0],point[1],0,1};vertex.uv={point[0],point[1],0,0};bitmap.vertices.push_back(vertex);}
    backend->clear(bitmap,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);backend->draw(bitmap);image=pixels(*backend);
    EXPECT_NEAR(image[inside],128,1);EXPECT_NEAR(image[inside+1],127,1);EXPECT_EQ(image[inside+2],0);EXPECT_EQ(image[inside+3],255);
    bitmap.uniforms.direct[0]=1;backend->draw(bitmap);image=pixels(*backend);EXPECT_EQ(image[inside],0);EXPECT_EQ(image[inside+1],255);
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
    auto backend=makeDx11BackendForTesting(device,context,D3DCompile,true);allocate(*backend);
    for(bool palette:{false,true})for(bool textured:{false,true})for(unsigned selector:{0u,1u,64u,65u,128u,129u,192u,193u}) {
        auto expanded=mesh();expanded.depthTest=false;expanded.uniforms.flags={float(textured),1,1,1};
        expanded.uniforms.lights[0].flags={1,0,0,1};expanded.uniforms.modelview[12]=-0.05f;expanded.uniforms.alternate[12]=0.05f;
        expanded.uniforms.alternateNormal[10]=-1;
        auto packed=expanded;packed.vertices.clear();
        for(unsigned i=0;i<3;++i) {
            PackedVertex vertex;std::copy_n(expanded.vertices[i].position.begin(),3,vertex.position.begin());
            vertex.normal={0.25f,0.75f,1};vertex.diffuse={96,128,191};vertex.ambient={48,64,80};vertex.alpha=191;
            vertex.specular={80,50,20,5};vertex.unknown={10,20,30};vertex.matrixSelect=uint8_t(selector);vertex.uv={0.25f,0.75f};
            vertex.drawIndex=palette?1.f:0.f;packed.packedVertices.push_back(vertex);
            auto& decoded=expanded.vertices[i];std::copy(vertex.normal.begin(),vertex.normal.end(),decoded.normal.begin());
            for(unsigned c=0;c<3;++c){decoded.diffuse[c]=vertex.diffuse[c]/255.f;decoded.ambient[c]=vertex.ambient[c]/255.f;decoded.unknown[c]=vertex.unknown[c]/65535.f;}
            for(unsigned c=0;c<4;++c)decoded.specular[c]=vertex.specular[c]/255.f;
            decoded.uv={0.25f,0.75f,0,0};decoded.extra={191.f/255.f,float(selector),vertex.drawIndex,0};
        }
        if(palette) {
            for(unsigned i=0;i<2;++i) {
                DrawRecord record;record.modelview=expanded.uniforms.modelview;record.alternate=expanded.uniforms.alternate;
                record.normal=expanded.uniforms.normal;record.alternateNormal=expanded.uniforms.alternateNormal;
                record.modes={1,1,0,1};record.modelview[12]+=float(i)*0.1f;
                expanded.palette.push_back(record);expanded.paletteTextures.push_back(i?11:10);expanded.paletteSampling.push_back(expanded.sampling[0]);
            }
            expanded.texturePalette=textured;packed.palette=expanded.palette;packed.paletteTextures=expanded.paletteTextures;
            packed.paletteSampling=expanded.paletteSampling;packed.texturePalette=textured;
        }
        clear(*backend);backend->draw(expanded);const auto expected=pixels(*backend);
        clear(*backend);backend->draw(packed);equalPixels(expected,pixels(*backend),"packed normalized attributes and selector",1);
        // The two strides are not multiples of one another. Alternating them
        // catches truncated first-vertex offsets and stale IA stride/layout.
        backend->draw(expanded);equalPixels(expected,pixels(*backend),"expanded following packed stream",1);
        backend->draw(packed);equalPixels(expected,pixels(*backend),"packed following expanded stream",1);
    }
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
}
#endif
