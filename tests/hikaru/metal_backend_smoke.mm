// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#import <Metal/Metal.h>
#include "hw/hikaru/graphics/metal_test.h"
#include <algorithm>
#include <array>
#include <iostream>

using namespace hikaru::graphics;

namespace {
bool pixelIs(const std::array<uint8_t,64>& image,int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    const auto* p=image.data()+(size_t(y)*4+x)*4;
    return p[0]==r && p[1]==g && p[2]==b && p[3]==a;
}
void identity(Matrix& m) { m[0]=m[5]=m[10]=m[15]=1; }
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device){std::cerr<<"Metal device unavailable\n";return 2;}
        auto backend=makeMetalBackendForTesting(device,[device newCommandQueue]);

        Texture output;output.width=4;output.height=4;output.format=GL_RGBA8;output.revision=1;
        backend->texture(0,output,0,nullptr,0);
        Texture internal=output;backend->texture(3,internal,0,nullptr,0);
        const uint8_t texel[4]={20,40,60,255};Texture source;source.width=1;source.height=1;source.format=GL_RGBA8;source.revision=1;
        backend->texture(1,source,0,texel,sizeof(texel));
        const uint8_t alphaTexel[4]={200,100,50,80};backend->texture(14,source,0,alphaTexel,sizeof(alphaTexel));

        Draw state;state.color=0;state.viewport={0,0,4,4};
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        Draw bitmap;bitmap.color=0;bitmap.bitmap=true;bitmap.viewport={0,0,4,4};bitmap.textures[0]=1;bitmap.uniforms.factors={0,1,1,0};
        Vertex a,b,c,d;a.position={0,0,0,1};a.uv={0,0,0,0};b.position={1,0,0,1};b.uv={1,0,0,0};c.position={0,1,0,1};c.uv={0,1,0,0};d.position={1,1,0,1};d.uv={1,1,0,0};
        bitmap.vertices={a,b,c,b,d,c};backend->draw(bitmap);
        std::array<uint8_t,64> pixels{};backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,20,40,60,255)){std::cerr<<"bitmap draw/readback mismatch\n";return 3;}
        backend->clear(state,GL_COLOR_BUFFER_BIT,{10.f/255,20.f/255,30.f/255,40.f/255},1);
        Draw rgbOnly=bitmap;rgbOnly.colorMask={true,true,true,false};backend->draw(rgbOnly);
        Draw alphaOnly=bitmap;alphaOnly.colorMask={false,false,false,true};alphaOnly.textures[0]=14;backend->draw(alphaOnly);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,20,40,60,80)){std::cerr<<"RGB-only/alpha-only draw masks mismatch\n";return 15;}
        backend->clear(state,GL_COLOR_BUFFER_BIT,{10.f/255,20.f/255,30.f/255,40.f/255},1);
        Draw redClear=state;redClear.colorMask={true,false,false,false};backend->clear(redClear,GL_COLOR_BUFFER_BIT,{1,0,0,1},1);
        Draw alphaClear=state;alphaClear.colorMask={false,false,false,true};backend->clear(alphaClear,GL_COLOR_BUFFER_BIT,{0,0,0,0.5f},1);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,255,20,30,128)){std::cerr<<"RGB-only/alpha-only clear masks mismatch\n";return 16;}
        Draw scissored=state;scissored.scissorEnabled=true;scissored.scissor={0,0,2,2};
        backend->clear(scissored,GL_COLOR_BUFFER_BIT,{1,1,0,1},1);pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,0,0,255,255,0,255)||!pixelIs(pixels,3,3,255,20,30,128)){std::cerr<<"scissored clear/readback orientation mismatch\n";return 6;}

        Texture depth;depth.width=4;depth.height=4;depth.format=GL_DEPTH_COMPONENT24;depth.revision=1;
        backend->texture(2,depth,0,nullptr,0);
        Draw clearDepth;clearDepth.color=0;clearDepth.depth=2;clearDepth.viewport={0,0,4,4};
        backend->clear(clearDepth,GL_DEPTH_BUFFER_BIT,{0,0,0,1},0.5f);
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        Draw clearInternal=state;clearInternal.color=3;backend->clear(clearInternal,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);

        Draw mesh;mesh.color=3;mesh.viewport={0,0,4,4};mesh.textures[2]=2;mesh.uniforms.peel={1,1,4,4};
        identity(mesh.uniforms.projection);identity(mesh.uniforms.modelview);mesh.uniforms.material[0]=1;mesh.uniforms.inverted[2]=1;
        auto quad=[&](float z){Vertex q0,q1,q2,q3;for(auto* q:{&q0,&q1,&q2,&q3}){q->diffuse={0.8f,0.4f,0.2f,1};q->extra={1,0,0,0};}q0.position={-1,-1,z,1};q1.position={1,-1,z,1};q2.position={-1,1,z,1};q3.position={1,1,z,1};mesh.vertices={q0,q1,q2,q1,q3,q2};};
        quad(0.4f);backend->draw(mesh);pixels.fill(0);backend->readPixels(3,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,0,0,0,255)){std::cerr<<"depth texture failed to reject behind fragment\n";return 4;}

        quad(-0.4f);mesh.cull=true;mesh.cullFace=GL_BACK;mesh.frontFace=GL_CCW;backend->draw(mesh);pixels.fill(0);backend->readPixels(3,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,204,102,51,255)){std::cerr<<"internal target culling/depth sampling mismatch\n";return 5;}

        // Encoder state is shared by adjacent draws to this target. A reversed
        // triangle is culled first; disabling culling for the next draw in the
        // same pass must restore coverage and use its new vertex color.
        backend->clear(clearInternal,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        Draw transition=mesh;transition.cull=true;transition.cullFace=GL_BACK;
        for(auto& v:transition.vertices)v.diffuse={1,1,0,1};
        transition.vertices={mesh.vertices[0],mesh.vertices[2],mesh.vertices[1],mesh.vertices[3],mesh.vertices[5],mesh.vertices[4]};
        backend->draw(transition);
        transition.cull=false;
        for(auto& v:transition.vertices)v.diffuse={0,1,0,1};
        backend->draw(transition);
        pixels.fill(0);backend->readPixels(3,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,0,255,0,255)){std::cerr<<"cull state did not reset between draws sharing an encoder\n";return 13;}

        // GL does not update depth storage when the depth test is disabled,
        // even if the depth mask remains enabled. A following farther draw
        // must still pass against the clear value.
        backend->clear(clearDepth,GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        Draw depthDisabled=mesh;depthDisabled.color=0;depthDisabled.depth=2;
        depthDisabled.depthTest=false;depthDisabled.depthWrite=true;
        quad(-0.8f);for(auto& v:mesh.vertices)v.diffuse={1,0,0,1};
        depthDisabled.vertices=mesh.vertices;backend->draw(depthDisabled);
        Draw depthEnabled=depthDisabled;depthEnabled.depthTest=true;depthEnabled.depthFunc=GL_LESS;
        quad(0.4f);for(auto& v:mesh.vertices)v.diffuse={0,1,0,1};
        depthEnabled.vertices=mesh.vertices;backend->draw(depthEnabled);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,1,0,255,0,255)){std::cerr<<"disabled depth test incorrectly wrote depth\n";return 17;}

        Texture peelColor;peelColor.width=4;peelColor.height=4;peelColor.format=GL_RGBA8;peelColor.revision=1;
        Texture peelDepth;peelDepth.width=4;peelDepth.height=4;peelDepth.format=GL_DEPTH_COMPONENT24;peelDepth.revision=1;
        backend->texture(8,peelColor,0,nullptr,0);backend->texture(10,peelColor,0,nullptr,0);
        backend->texture(7,peelDepth,0,nullptr,0);backend->texture(9,peelDepth,0,nullptr,0);
        backend->texture(11,peelDepth,0,nullptr,0);backend->texture(13,peelColor,0,nullptr,0);
        Draw opaqueClear;opaqueClear.color=13;opaqueClear.depth=11;opaqueClear.viewport={0,0,4,4};opaqueClear.depthWrite=true;
        backend->clear(opaqueClear,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,1},1);
        Draw opaque;opaque.color=13;opaque.depth=11;opaque.viewport={0,0,4,4};opaque.depthTest=true;opaque.depthFunc=GL_LESS;opaque.depthWrite=true;opaque.colorMask={false,false,false,false};
        opaque.uniforms.projection[0]=opaque.uniforms.projection[5]=opaque.uniforms.projection[10]=opaque.uniforms.projection[15]=1;
        opaque.uniforms.modelview[0]=opaque.uniforms.modelview[5]=opaque.uniforms.modelview[10]=opaque.uniforms.modelview[15]=1;opaque.uniforms.inverted[2]=1;
        Vertex o0,o1,o2,o3;for(auto* q:{&o0,&o1,&o2,&o3}){q->diffuse={1,1,1,1};q->extra={1,0,0,0};}
        o0.position={-1,0,-0.8f,1};o1.position={1,0,-0.8f,1};o2.position={-1,1,-0.8f,1};o3.position={1,1,-0.8f,1};opaque.vertices={o0,o1,o2,o1,o3,o2};backend->draw(opaque);
        Draw peel=mesh;peel.depthTest=true;peel.depthFunc=GL_LEQUAL;peel.depthWrite=true;peel.cull=true;peel.cullFace=GL_BACK;peel.frontFace=GL_CCW;
        peel.uniforms.peel={1,1,4,4};peel.textures[1]=0;peel.textures[2]=11;
        auto coloredQuad=[&](float z,float r,float g,float b){Vertex q0,q1,q2,q3;for(auto* q:{&q0,&q1,&q2,&q3}){q->diffuse={r,g,b,1};q->extra={128.f/255.f,0,0,0};}q0.position={-1,-1,z,1};q1.position={1,-1,z,1};q2.position={-1,1,z,1};q3.position={1,1,z,1};peel.vertices={q0,q1,q2,q1,q3,q2};};
        Draw peelClear;peelClear.color=8;peelClear.depth=7;peelClear.viewport={0,0,4,4};peelClear.depthWrite=true;
        peel.color=8;peel.depth=7;backend->clear(peelClear,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,0},1);
        coloredQuad(0.2f,0,200.f/255.f,0);backend->draw(peel); // Far green writes first.
        coloredQuad(-0.6f,200.f/255.f,0,0);backend->draw(peel); // Near red wins the first depth pass.
        pixels.fill(0);backend->readPixels(8,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,0,0,0,0,0)||!pixelIs(pixels,1,3,200,0,0,128)){std::cerr<<"first peel depth/coordinate sampling mismatch: ";for(int y=0;y<4;y++){const auto* q=pixels.data()+(size_t(y)*4+1)*4;std::cerr<<y<<"="<<int(q[0])<<","<<int(q[1])<<","<<int(q[2])<<","<<int(q[3])<<" ";}std::cerr<<"\n";return 7;}

        peel.color=10;peel.depth=9;peel.uniforms.peel[1]=0;peel.textures[1]=7;peelClear.color=10;peelClear.depth=9;
        backend->clear(peelClear,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,{0,0,0,0},1);
        coloredQuad(0.2f,0,200.f/255.f,0);backend->draw(peel);
        coloredQuad(-0.6f,200.f/255.f,0,0);backend->draw(peel); // Rejected because it matches the previous depth.
        pixels.fill(0);backend->readPixels(10,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,0,0,0,0,0)||!pixelIs(pixels,1,3,0,200,0,128)){std::cerr<<"second peel depth/coordinate sampling mismatch\n";return 10;}

        backend->clear(state,GL_COLOR_BUFFER_BIT,{20.f/255.f,40.f/255.f,60.f/255.f,1.f},1);
        Draw composite=bitmap;composite.color=0;composite.blend=true;
        composite.srcRGB=composite.srcAlpha=GL_SRC_ALPHA;composite.dstRGB=composite.dstAlpha=GL_ONE_MINUS_SRC_ALPHA;
        composite.textures[0]=10;backend->draw(composite); // Back-to-front composition.
        composite.textures[0]=8;backend->draw(composite);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,1,0,20,40,60,255)||!pixelIs(pixels,1,3,105,60,15,159)){const auto* low=pixels.data()+(size_t(0)*4+1)*4;const auto* high=pixels.data()+(size_t(3)*4+1)*4;std::cerr<<"peeled two-layer alpha composite mismatch: low="<<int(low[0])<<","<<int(low[1])<<","<<int(low[2])<<","<<int(low[3])<<" high="<<int(high[0])<<","<<int(high[1])<<","<<int(high[2])<<","<<int(high[3])<<"\n";return 11;}

        Texture mip;mip.width=4;mip.height=4;mip.format=GL_RGBA8;mip.maxLevel=2;mip.revision=1;
        std::array<uint8_t,64> baseMip{};std::array<uint8_t,16> midMip{};const uint8_t green[4]={0,255,0,255},magenta[4]={255,0,255,255};
        for(size_t i=3;i<baseMip.size();i+=4)baseMip[i]=255;
        for(size_t i=0;i<midMip.size();i+=4)std::copy(green,green+4,midMip.begin()+i);
        backend->texture(6,mip,0,baseMip.data(),baseMip.size());backend->texture(6,mip,1,midMip.data(),midMip.size());backend->texture(6,mip,2,magenta,sizeof(magenta));
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        Draw mipDraw=bitmap;mipDraw.color=0;mipDraw.viewport={0,0,2,2};mipDraw.textures[0]=6;
        mipDraw.sampling[0]=mip;mipDraw.sampling[0].minFilter=GL_NEAREST;mipDraw.sampling[0].magFilter=GL_NEAREST;
        backend->draw(mipDraw);pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,0,0,0,0,0,255)){std::cerr<<"nonmip minification sampled an undefined mip level\n";return 14;}
        backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);
        mipDraw.sampling[0]=mip;mipDraw.sampling[0].minFilter=GL_NEAREST_MIPMAP_NEAREST;mipDraw.sampling[0].magFilter=GL_NEAREST;
        backend->draw(mipDraw);pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,0,0,0,255,0,255)){std::cerr<<"mip level selection mismatch: "
            <<int(pixels[0])<<","<<int(pixels[1])<<","<<int(pixels[2])<<","<<int(pixels[3])<<"\n";return 8;}
        mipDraw.uniforms.lodBias[0]=1;backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);backend->draw(mipDraw);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,0,0,255,0,255,255)){std::cerr<<"texture LOD bias mismatch\n";return 12;}
        mipDraw.uniforms.lodBias[0]=0;
        mipDraw.sampling[0].maxLevel=0;backend->clear(state,GL_COLOR_BUFFER_BIT,{0,0,0,1},1);backend->draw(mipDraw);
        pixels.fill(0);backend->readPixels(0,0,0,4,4,pixels.data());
        if(!pixelIs(pixels,0,0,0,0,0,255)){std::cerr<<"texture max-level clamp mismatch: "
            <<int(pixels[0])<<","<<int(pixels[1])<<","<<int(pixels[2])<<","<<int(pixels[3])<<"\n";return 9;}

        std::cout<<"Hikaru Metal bitmap, mip completeness/minification, peel depth, composition, culling state, scissor, and readback passed\n";
        return 0;
    }
}
