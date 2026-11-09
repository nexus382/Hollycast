// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gtest/gtest.h"
#include "hw/hikaru/graphics/device.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace hikaru::graphics::test {
inline void identity(Matrix& matrix) { matrix={};matrix[0]=matrix[5]=matrix[10]=matrix[15]=1; }
inline std::vector<Vertex> triangle() {
    std::vector<Vertex> vertices(3);
    vertices[0].position={-1,-1,-0.25f,1};vertices[1].position={1,-1,-0.25f,1};vertices[2].position={-1,1,-0.25f,1};
    for(auto& v:vertices){v.normal={0,0,1,0};v.diffuse={0.8f,0.4f,0.2f,1};v.ambient={0.2f,0.3f,0.4f,0};
        v.specular={0.2f,0.3f,0.4f,0.1f};v.unknown={0.001f,0.002f,0.003f,0};v.uv={0.5f,0.5f,0,0};v.extra={0.75f,0,0,0};}
    return vertices;
}
inline Draw mesh() {
    Draw result;result.color=0;result.depth=2;result.viewport={0,0,16,16};result.depthTest=true;
    result.textures[0]=10;identity(result.uniforms.projection);identity(result.uniforms.modelview);
    identity(result.uniforms.alternate);identity(result.uniforms.normal);identity(result.uniforms.alternateNormal);
    result.uniforms.depth={0.01f,1,0,1};result.uniforms.material={0,0,0,1};result.uniforms.family[3]=65535;
    result.uniforms.flags={1,0,0,0};result.uniforms.ambient={0.15f,0.1f,0.05f,0};
    result.uniforms.peel={0,0,16,16};result.uniforms.fog={0.2f,0,0,0};result.uniforms.fogColor={0.1f,0.2f,0.3f,0};
    auto& light=result.uniforms.lights[0];light.position={0,0,1,0};light.direction={0,0,1,0};
    light.diffuse={0.8f,0.7f,0.6f,0};light.specular={0.2f,0.1f,0.3f,0};light.extents={0.2f,0.5f,0.25f,0};
    light.spot={-1,1,1,0};for(unsigned i=0;i<16;++i)light.table[i/4][i%4]=float(i+1)/16;
    result.vertices=triangle();return result;
}
inline void allocate(Backend& backend) {
    hikaru::graphics::Texture output;output.width=output.height=16;output.format=GL_RGBA8;
    backend.texture(0,output,0,TextureUpload::Definition,nullptr,0);
    hikaru::graphics::Texture depth=output;depth.format=GL_DEPTH_COMPONENT24;backend.texture(2,depth,0,TextureUpload::Definition,nullptr,0);
    hikaru::graphics::Texture source;source.width=source.height=1;source.maxLevel=0;
    const uint8_t red[]={255,0,0,255},green[]={0,255,0,255};
    backend.texture(10,source,0,TextureUpload::Definition,red,sizeof(red));
    backend.texture(11,source,0,TextureUpload::Definition,green,sizeof(green));
}
inline void clear(Backend& backend) {
    auto draw=mesh();backend.clear(draw,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);
}
inline std::vector<uint8_t> pixels(Backend& backend,unsigned width=16) {
    std::vector<uint8_t> result(size_t(width)*width*4);backend.readPixels(0,0,0,width,width,result.data());return result;
}
inline void equalPixels(const std::vector<uint8_t>& expected,const std::vector<uint8_t>& actual,const char* label,int tolerance=0) {
    ASSERT_EQ(expected.size(),actual.size());
    for(size_t i=0;i<actual.size();++i)if(std::abs(int(expected[i])-int(actual[i]))>tolerance){
        ADD_FAILURE()<<label<<" differs at byte "<<i<<": "<<int(expected[i])<<" vs "<<int(actual[i]);return;
    }
}
inline void exerciseNativeGraphics(Backend& generic,Backend& optimized) {
    allocate(generic);allocate(optimized);
    // Compare actual GPU output from the generic and specialized programs.
    // Shader modes cover the existing material, spotlight, fog and mask fixes.
    for(unsigned i=0;i<24;++i) {
        auto draw=mesh();draw.uniforms.flags={float(i%2),float(i%4),float((i/2)%2),float((i/3)%2)};
        draw.uniforms.material[0]=float((i/2)%2);draw.uniforms.material[1]=float(i%5);
        draw.uniforms.family[3]=i%3==0?float(0x5a5a):65535;
        for(unsigned light=0;light<4;++light){
            draw.uniforms.lights[light]=draw.uniforms.lights[0];
            draw.uniforms.lights[light].flags={float((i+light)%3!=0),float((i+light)%3),float((i+light)%4),float((i+light)%2)};
        }
        draw.uniforms.depth[1]=0.25f+float(i%3);draw.uniforms.material[3]=0.5f+float(i%2);
        clear(generic);generic.draw(draw);const auto expected=pixels(generic);
        clear(optimized);optimized.draw(draw);equalPixels(expected,pixels(optimized),"specialized mesh",1);
    }
    const bool textures=optimized.supportsTexturePalette();
    auto first=mesh();first.uniforms.flags={1,0,0,0};first.uniforms.material[1]=0;
    for(auto& v:first.vertices){v.diffuse={1,1,1,1};v.extra[0]=1;}
    first.uniforms.depth[1]=1;first.uniforms.material[3]=0.5f;
    auto second=first;second.textures[0]=textures?11:10;second.uniforms.material[3]=2;
    // The second triangle is farther in canonical written depth, even though
    // its placement scale differs. A combined palette draw must preserve that.
    second.uniforms.depth[1]=0.25f;
    clear(generic);generic.draw(first);generic.draw(second);const auto expected=pixels(generic);
    auto combined=first;combined.texturePalette=textures;combined.shadingPalette=textures;
    for(unsigned i=0;i<2;++i) {
        const auto& scalar=i?second:first;
        DrawRecord record;record.modelview=scalar.uniforms.modelview;record.alternate=scalar.uniforms.alternate;
        record.normal=scalar.uniforms.normal;record.alternateNormal=scalar.uniforms.alternateNormal;
        record.modes={scalar.uniforms.depth[1],scalar.uniforms.material[3],scalar.uniforms.material[1],scalar.uniforms.flags[1]};
        combined.palette.push_back(record);
        if(textures){combined.paletteTextures.push_back(scalar.textures[0]);combined.paletteSampling.push_back(scalar.sampling[0]);}
    }
    combined.vertices=first.vertices;for(auto vertex:second.vertices){vertex.extra[2]=1;combined.vertices.push_back(vertex);}
    clear(optimized);optimized.draw(combined);equalPixels(expected,pixels(optimized),"ordered palette depth");
    // Distinct transforms and texture slots must also survive the palette.
    first.depthTest=second.depthTest=false;first.vertices[1].position[0]=second.vertices[1].position[0]=0;
    second.uniforms.modelview[12]=1;
    clear(generic);generic.draw(first);generic.draw(second);const auto separated=pixels(generic);
    combined.depthTest=false;combined.vertices=first.vertices;
    for(auto vertex:second.vertices){vertex.extra[2]=1;combined.vertices.push_back(vertex);}
    combined.palette[1].modelview=second.uniforms.modelview;
    clear(optimized);optimized.draw(combined);equalPixels(separated,pixels(optimized),"palette transform and texture selection");
    // Matrix selector bits choose the alternate bank or bypass rotation and
    // translation independently. Compare the SSBO palette against scalar draws.
    first.uniforms.flags={1,1,0,0};second.uniforms.flags=first.uniforms.flags;
    first.uniforms.lights[0].flags={1,0,0,0};second.uniforms.lights=first.uniforms.lights;
    first.uniforms.modelview[12]=-0.1f;first.uniforms.alternate[12]=0.1f;
    second.uniforms.modelview[12]=0.8f;second.uniforms.alternate[12]=0.9f;
    first.uniforms.alternateNormal[10]=second.uniforms.alternateNormal[10]=-1;
    for(float selector:{0.f,1.f,64.f,65.f,128.f,129.f,192.f,193.f}) {
        for(auto* draw:{&first,&second})for(auto& vertex:draw->vertices)vertex.extra[1]=selector;
        clear(generic);generic.draw(first);generic.draw(second);const auto scalar=pixels(generic);
        combined=first;combined.texturePalette=textures;combined.shadingPalette=textures;
        combined.vertices=first.vertices;
        for(auto vertex:second.vertices){vertex.extra[2]=1;combined.vertices.push_back(vertex);}
        for(const auto* draw:{&first,&second}) {
            DrawRecord record;record.modelview=draw->uniforms.modelview;record.alternate=draw->uniforms.alternate;
            record.normal=draw->uniforms.normal;record.alternateNormal=draw->uniforms.alternateNormal;
            record.modes={draw->uniforms.depth[1],draw->uniforms.material[3],draw->uniforms.material[1],draw->uniforms.flags[1]};
            combined.palette.push_back(record);
            if(textures){combined.paletteTextures.push_back(draw->textures[0]);combined.paletteSampling.push_back(draw->sampling[0]);}
        }
        clear(optimized);optimized.draw(combined);equalPixels(scalar,pixels(optimized),"palette matrix selector",1);
    }
    // Devices below the 19-sampler limit still use transform palettes with
    // a single texture. Exercise that compact pipeline on the same GPU too.
    auto fixed=combined;fixed.texturePalette=false;fixed.shadingPalette=false;
    fixed.paletteTextures.clear();fixed.paletteSampling.clear();
    auto sameTexture=second;sameTexture.textures=first.textures;
    clear(generic);generic.draw(first);generic.draw(sameTexture);const auto scalarFixed=pixels(generic);
    clear(optimized);optimized.draw(fixed);equalPixels(scalarFixed,pixels(optimized),"single-texture transform palette",1);
    // Recycle multiple descriptor/stream batches, then redefine a sampled
    // image while prior draws still refer to it. Readback drains their fences.
    clear(optimized);auto repeated=first;repeated.uniforms.flags={1,0,0,0};
    for(unsigned i=0;i<2600;++i)optimized.draw(repeated);
    hikaru::graphics::Texture replacement;replacement.width=replacement.height=2;replacement.maxLevel=0;
    const std::array<uint8_t,16> blue={0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255};
    optimized.texture(10,replacement,0,TextureUpload::Definition,blue.data(),blue.size());
    optimized.draw(repeated);auto updated=pixels(optimized);
    const auto center=(size_t(4)*16+4)*4;
    EXPECT_EQ(updated[center],0);EXPECT_EQ(updated[center+1],0);EXPECT_EQ(updated[center+2],255);
    hikaru::graphics::Texture resized;resized.width=resized.height=8;
    optimized.draw(repeated);optimized.texture(0,resized,0,TextureUpload::Definition,nullptr,0);
    repeated.depth=0;repeated.viewport={0,0,8,8};optimized.clear(repeated,GL_COLOR_BUFFER_BIT,{0,1,0,1},1);
    auto smaller=pixels(optimized,8);EXPECT_EQ(smaller[1],255);
    optimized.draw(repeated); // destructor must drain this final pending batch
}
} // namespace hikaru::graphics::test
