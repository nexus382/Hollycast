// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/hikaru/graphics/device.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hikaru::graphics;

namespace {
struct RecordingBackend final : Backend {
    struct TextureCall { unsigned id=0, level=0; Texture info{}; TextureUpload upload=TextureUpload::Definition; std::vector<uint8_t> pixels; };
    bool paletteSupport=false, texturePaletteSupport=false, packedSupport=false;
    bool supportsDrawPalette() const override { return paletteSupport; }
    bool supportsTexturePalette() const override { return texturePaletteSupport; }
    bool supportsPackedVertices() const override { return packedSupport; }
    Draw lastDraw{}, lastClear{};
    GLbitfield lastClearMask=0;
    bool drew = false;
    unsigned draws = 0;
    unsigned lastPresentWidth = 0, lastPresentHeight = 0;
    std::unordered_map<unsigned, std::vector<uint8_t>> uploads;
    std::unordered_map<unsigned, Texture> infos;
    std::vector<unsigned> erased;
    std::vector<TextureCall> textureCalls;
    std::vector<Draw> drawHistory;
    std::vector<std::string> events;
    void texture(unsigned id, const Texture& info, unsigned level, TextureUpload upload, const void* pixels, size_t bytes) override {
        infos[id] = info;
        TextureCall call{id,level,info,upload,{}};
        if (pixels && bytes) {
            call.pixels.assign(static_cast<const uint8_t*>(pixels), static_cast<const uint8_t*>(pixels) + bytes);
            uploads[id]=call.pixels;
        }
        textureCalls.push_back(std::move(call));
        events.push_back(upload==TextureUpload::FullUpdate?"update":"definition");
    }
    void eraseTexture(unsigned id) override { erased.push_back(id); }
    void clear(const Draw& draw, GLbitfield mask, const Vec4&, float) override { lastClear=draw;lastClearMask=mask;events.push_back("clear"); }
    void draw(const Draw& draw) override { lastDraw = draw; drawHistory.push_back(draw); events.push_back("draw"); drew = true;++draws; }
    void readPixels(unsigned, int, int, int, int, uint8_t*) override {}
    unsigned queryStartDraws = 0;
    void beginOcclusion() override { queryStartDraws = draws; }
    bool endOcclusion() override { return draws != queryStartDraws; }
    void present(unsigned width, unsigned height) override { lastPresentWidth = width; lastPresentHeight = height; }
};

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

GLuint makeProgram(const char* source) {
    GLuint p = 0, s = 0;
    p = hikaru_glCreateProgram();
    s = hikaru_glCreateShader(GL_VERTEX_SHADER);
    const GLchar* strings[] = {source};
    hikaru_glShaderSource(s, 1, strings, nullptr);
    hikaru_glAttachShader(p, s);
    hikaru_glLinkProgram(p);
    hikaru_glUseProgram(p);
    return p;
}

void setAttribute(GLuint index, GLint size, GLenum type, GLboolean normalized, const void* data, size_t bytes) {
    GLuint b = 0;
    hikaru_glGenBuffers(1, &b);
    hikaru_glBindBuffer(GL_ARRAY_BUFFER, b);
    hikaru_glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
    hikaru_glEnableVertexAttribArray(index);
    hikaru_glVertexAttribPointer(index, size, type, normalized, 0, nullptr);
}

void issueTriangle() {
    GLuint vao = 0;
    hikaru_glGenVertexArrays(1, &vao);
    hikaru_glBindVertexArray(vao);
    const GLfloat pos[] = {1,2,3,4, 5,6,7,8, 9,10,11,12};
    const GLubyte norm[] = {0,127,255,255, 255,127,0,255, 10,20,30,40};
    const GLfloat matFlags[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    setAttribute(0, 4, GL_FLOAT, GL_FALSE, pos, sizeof(pos));
    setAttribute(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, norm, sizeof(norm));
    setAttribute(7, 1, GL_FLOAT, GL_FALSE, matFlags, sizeof(matFlags));
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
}

void testDispatchConversionUniformsAndState(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    device.begin(64, 32);
    hikaru_glViewport(2, 3, 40, 50);
    hikaru_glEnable(GL_BLEND);
    hikaru_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    GLuint p = makeProgram("#define HAS_TEXTURE 1\n#define SHADING_MODE 1\n#define HAS_LIGHT0 1\n#define ALPHA_MODE 0\n#define TEXTURE_MODE 2\n");
    const GLfloat projection[] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p,"u_lights[0].ambient"),0.25f);
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p,"u_weigh_by_alpha"),1.0f);
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p,"u_force_coverage"),0.0f);
    const GLfloat normal[] = {1,2,3,4,5,6,7,8,9};
    GLint loc = hikaru_glGetUniformLocation(p, "u_projection"); hikaru_glUniformMatrix4fv(loc, 1, GL_FALSE, projection);
    loc = hikaru_glGetUniformLocation(p, "u_normal"); hikaru_glUniformMatrix3fv(loc, 1, GL_FALSE, normal);
    loc = hikaru_glGetUniformLocation(p, "u_factor"); hikaru_glUniform1f(loc, 0.375f);
    loc = hikaru_glGetUniformLocation(p, "u_depth"); hikaru_glUniform2f(loc, 0.1f, 0.00002f);
    hikaru_glDepthRangef(0.2f, 0.9f);
    issueTriangle();
    device.checkError();
    check(backend.drew, "dispatch wrapper did not reach Backend::draw");
    check(backend.lastDraw.vertices.size() == 3, "draw vertex count mismatch");
    const auto& v = backend.lastDraw.vertices[0];
    check(v.position == Vec4{1,2,3,4}, "float vertex conversion failed");
    check(near(v.normal[0], 0) && near(v.normal[1], 127.f/255.f) && near(v.normal[2], 1), "normalized byte vertex conversion failed");
    const auto& u = backend.lastDraw.uniforms;
    check(u.flags[1]==1.0f&&u.material[0]==0.0f&&u.material[1]==2.0f,
          "native shader lost explicit GL shading/texture/alpha modes");
    check(u.lights[0].extents[2]==0.25f&&u.blend==Vec4{1,0,0,0},
          "measured lamp fill or premultiplied coverage inputs were lost");
    check(u.projection == Matrix{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, "matrix4 uniform packing failed");
    check(u.normal == Matrix{1,2,3,0,4,5,6,0,7,8,9,0,0,0,0,0}, "matrix3 std140 packing failed");
    check(near(u.factors[0], .375f) && near(u.factors[1], 1), "scalar uniform defaults failed");
    check(u.depth == Vec4{0.1f,0.00002f,0.2f,0.9f}, "canonical depth parameters or range were lost");
    check(backend.lastDraw.viewport == std::array<GLint,4>{2,3,40,50}, "viewport state missing from draw");
    check(backend.lastDraw.blend && backend.lastDraw.srcRGB == GL_SRC_ALPHA && backend.lastDraw.dstAlpha == GL_ZERO,
          "blend state missing from draw");
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p, "u_factor"), 0.625f);
    hikaru_glUniform2f(hikaru_glGetUniformLocation(p, "u_depth"), 0.3f, 0.0f);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(near(backend.lastDraw.uniforms.factors[0], 0.625f),
          "updated uniform did not invalidate the packed constant cache");
    check(backend.lastDraw.uniforms.depth == Vec4{0.3f,0.0f,0.2f,0.9f}, "zero bias did not replace packed depth parameters");
    hikaru_glDepthRangef(0.0f, 1.0f);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(backend.lastDraw.uniforms.depth == Vec4{0.3f,0.0f,0.0f,1.0f}, "cached uniforms retained an obsolete depth range");
    check(backend.lastDraw.color == 0 && backend.lastDraw.textures[0] == 0, "default target zero and unbound texture zero differ unexpectedly");
    check(backend.infos.at(0).width == 64 && backend.infos.at(0).height == 32,
          "default framebuffer target zero was not allocated separately from the unbound sampler");

    // Warm packed constants must accept matrix changes without disturbing
    // unrelated material/light state, and a later full repack must agree.
    const auto previous=backend.lastDraw.uniforms;
    const GLfloat moved[]={1,0,0,0, 0,1,0,0, 0,0,1,0, 7,8,9,1};
    const GLfloat rotated[]={9,8,7,6,5,4,3,2,1};
    hikaru_glUniformMatrix4fv(hikaru_glGetUniformLocation(p,"u_modelview_alternate"),1,GL_FALSE,moved);
    hikaru_glUniformMatrix3fv(hikaru_glGetUniformLocation(p,"u_normal_alternate"),1,GL_FALSE,rotated);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    const Matrix expectedModel={1,0,0,0,0,1,0,0,0,0,1,0,7,8,9,1};
    const Matrix expectedNormal={9,8,7,0,6,5,4,0,3,2,1,0,0,0,0,0};
    check(backend.lastDraw.uniforms.alternate==expectedModel&&backend.lastDraw.uniforms.alternateNormal==expectedNormal,
          "warm matrix update lost the alternate bank or normal padding");
    check(backend.lastDraw.uniforms.projection==previous.projection&&backend.lastDraw.uniforms.material==previous.material&&
          std::memcmp(previous.lights.data(),backend.lastDraw.uniforms.lights.data(),sizeof(previous.lights))==0,
          "matrix update changed unrelated packed constants");
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p,"u_factor"),0.875f);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.uniforms.alternate==expectedModel&&backend.lastDraw.uniforms.alternateNormal==expectedNormal&&
          backend.lastDraw.uniforms.factors[0]==0.875f,"full uniform repack disagrees with cached matrix updates");

    GLuint bitmap = makeProgram("// u_front sampler marks bitmap program\n");
    GLuint image = 0; hikaru_glGenTextures(1, &image);
    hikaru_glActiveTexture(GL_TEXTURE1); hikaru_glBindTexture(GL_TEXTURE_2D, image);
    const uint8_t texel[] = {1,2,3,4};
    hikaru_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    hikaru_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -0.75f);
    hikaru_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint sampler = hikaru_glGetUniformLocation(bitmap, "u_front"); hikaru_glUniform1i(sampler, 1);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(backend.lastDraw.color == 0 && backend.lastDraw.textures[0] == image,
          "render target zero was confused with a bound nonzero sampler image");
    check(near(backend.lastDraw.uniforms.lodBias[0], -0.75f)
          && backend.lastDraw.sampling[0].magFilter == GL_LINEAR,
          "sampler remapping lost filtering or fractional LOD bias");
    check(near(backend.lastDraw.uniforms.factors[3], 1.f),
          "bitmap aspect scale must default to native proportions");
    const GLint layerScale = hikaru_glGetUniformLocation(bitmap, "u_layer_x_scale");
    for (float scale : {0.75f, 0.5f, 1.f}) {
        hikaru_glUniform1f(layerScale, scale);
        hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
        device.checkError();
        check(near(backend.lastDraw.uniforms.factors[3], scale),
              "bitmap widescreen scale lost during native uniform packing");
    }
    hikaru_glUniform1f(hikaru_glGetUniformLocation(bitmap, "u_lod_bias"), -0.375f);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(near(backend.lastDraw.uniforms.lodBias[0], -0.375f), "ES uniform LOD bias was lost in native dispatch");
    hikaru_glBindTexture(GL_TEXTURE_2D, 0);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(backend.lastDraw.color == 0 && backend.lastDraw.textures[0] == 0,
          "unbound sampler did not remain texture zero while rendering to target zero");
    GLuint query[2] = {};
    hikaru_graphics_begin_occlusion(query);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    check(hikaru_graphics_end_occlusion(query), "query did not flush its pending draw");
    hikaru_graphics_begin_occlusion(query);
    check(!hikaru_graphics_end_occlusion(query), "empty query included an earlier draw");
    hikaru_graphics_delete_occlusion(query);
    device.checkError();
    const auto draws=backend.draws;
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(backend.draws==draws+1 && backend.lastDraw.vertices.size()==6,
          "adjacent identical triangles were not batched");
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    hikaru_glClear(GL_COLOR_BUFFER_BIT);
    check(backend.draws==draws+2, "clear did not flush preceding triangles");
}

void testPeelDispatchAndDeferredQueries(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    device.begin(16,16);
    const GLuint program=makeProgram("#define PEEL_COMPOSITE 1\n uniform sampler2D u_front; uniform usampler2D u_peel_meta;\n");
    GLuint images[2]={};hikaru_glGenTextures(2,images);
    hikaru_glActiveTexture(GL_TEXTURE0);hikaru_glBindTexture(GL_TEXTURE_2D,images[0]);
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    // Remapping metadata inside the original four units must work without
    // texture-palette support (Metal and reduced-sampler Vulkan devices).
    hikaru_glActiveTexture(GL_TEXTURE3);hikaru_glBindTexture(GL_TEXTURE_2D,images[1]);
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RG32UI,16,16,0,GL_RG_INTEGER,GL_UNSIGNED_INT,nullptr);
    hikaru_glUniform1i(hikaru_glGetUniformLocation(program,"u_front"),0);
    hikaru_glUniform1i(hikaru_glGetUniformLocation(program,"u_peel_meta"),3);
    issueTriangle();device.checkError();
    check(backend.lastDraw.bitmap&&backend.lastDraw.peelComposite&&backend.lastDraw.textures[0]==images[0]&&
          backend.lastDraw.textures[2]==images[1]&&backend.lastDraw.sampling[2].format==GL_RG32UI,
          "peel program lost its dedicated shader or integer sampler remapping");
    for(int mode=0;mode<4;++mode) {
        hikaru_glUniform1i(hikaru_glGetUniformLocation(program,"u_peel_mode"),mode);
        hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
        check(backend.lastDraw.uniforms.peel[1]==float(mode),"peel blend mode was not packed");
    }
    GLuint queries[3][2]={};
    for(unsigned i=0;i<3;++i) {
        hikaru_graphics_begin_occlusion(queries[i]);
        if(i!=1)hikaru_glDrawArrays(GL_TRIANGLES,0,3);
        hikaru_graphics_finish_occlusion(queries[i]);
    }
    for(unsigned i=0;i<3;++i)
        check(bool(hikaru_graphics_occlusion_result(queries[i]))==(i!=1),"deferred query FIFO changed result ownership");
    device.begin(32,32);
    hikaru_graphics_begin_occlusion(queries[0]);
    check(!hikaru_graphics_end_occlusion(queries[0]),"resize inherited an old query result");
    makeProgram("uniform sampler2D u_front;\n");issueTriangle();device.checkError();
    check(backend.lastDraw.bitmap&&!backend.lastDraw.peelComposite,"ordinary bitmap inherited peel composition");
    for(auto& query:queries)hikaru_graphics_delete_occlusion(query);
    hikaru_glDeleteTextures(2,images);device.checkError();
}

void testTexturePacking(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    GLuint tex = 0;
    hikaru_glGenTextures(1, &tex); hikaru_glBindTexture(GL_TEXTURE_2D, tex);
    // Each one-pixel row uses 8-byte pitch due to alignment. Skip one row and
    // one pixel within that source row before uploading a 1x1 RGBA texel.
    const uint8_t rows[] = {
        0,0,0,0, 0xee,0xee,0xee,0xee,
        1,2,3,4, 5,6,7,8
    };
    hikaru_glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    hikaru_glPixelStorei(GL_UNPACK_ROW_LENGTH, 1);
    hikaru_glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    hikaru_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    hikaru_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rows);
    check(backend.uploads.at(tex) == std::vector<uint8_t>({5,6,7,8}), "row length/skip/alignment unpack failed");
    hikaru_glPixelStorei(GL_UNPACK_ALIGNMENT, 4); hikaru_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); hikaru_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); hikaru_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    GLuint packed = 0; hikaru_glGenTextures(1, &packed); hikaru_glBindTexture(GL_TEXTURE_2D, packed);
    const uint32_t raw = (2u << 30) | (777u << 20) | (513u << 10) | 257u;
    hikaru_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, 1, 1, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &raw);
    check(backend.uploads.at(packed).size() == sizeof(raw), "packed RGB10_A2 upload size mismatch");
    uint32_t observed = 0; std::memcpy(&observed, backend.uploads.at(packed).data(), sizeof(observed));
    check(observed == raw, "RGB10_A2 packed precision/order was changed");

    GLuint rg = 0; hikaru_glGenTextures(1, &rg); hikaru_glBindTexture(GL_TEXTURE_2D, rg);
    const uint8_t grayAlpha[] = {21, 73, 44, 88};
    hikaru_glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 2, 1, 0, GL_RG, GL_UNSIGNED_BYTE, grayAlpha);
    check(backend.uploads.at(rg) == std::vector<uint8_t>({21,21,21,73, 44,44,44,88}), "RG luminance-alpha expansion failed");
}

void testFullTextureUpdates(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    hikaru_glActiveTexture(GL_TEXTURE0);
    GLuint tex=0;hikaru_glGenTextures(1,&tex);hikaru_glBindTexture(GL_TEXTURE_2D,tex);
    hikaru_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,1);
    hikaru_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    const uint8_t initial[]={1,2,3,4,5,6,7,8};
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,initial);
    const Texture defined=device.textureInfo(tex);

    // Exercise the same unpack row/skip rules for a full replacement.
    uint8_t rows[28]={};const uint8_t replacement[]={9,10,11,12,13,14,15,16};
    std::memcpy(rows+20,replacement,sizeof(replacement));
    hikaru_glPixelStorei(GL_UNPACK_ALIGNMENT,8);hikaru_glPixelStorei(GL_UNPACK_ROW_LENGTH,3);
    hikaru_glPixelStorei(GL_UNPACK_SKIP_ROWS,1);hikaru_glPixelStorei(GL_UNPACK_SKIP_PIXELS,1);
    hikaru_glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,1,GL_RGBA,GL_UNSIGNED_BYTE,rows);
    device.checkError();
    hikaru_glPixelStorei(GL_UNPACK_ALIGNMENT,4);hikaru_glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    hikaru_glPixelStorei(GL_UNPACK_SKIP_ROWS,0);hikaru_glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    const auto& replaced=backend.textureCalls.back();
    check(replaced.upload==TextureUpload::FullUpdate&&replaced.pixels==std::vector<uint8_t>(replacement,replacement+sizeof(replacement)),
          "full texture update did not replace pixels through unpack state");
    const auto& updated=device.textureInfo(tex);
    check(updated.width==defined.width&&updated.height==defined.height&&updated.format==defined.format
          &&updated.maxLevel==defined.maxLevel&&updated.minFilter==defined.minFilter&&updated.revision==defined.revision+1,
          "full texture update changed definition metadata or missed its revision");

    // A queued draw must consume the old revision before the replacement is issued.
    GLuint bitmap=makeProgram("// u_front sampler marks bitmap program\n");
    hikaru_glUniform1i(hikaru_glGetUniformLocation(bitmap,"u_front"),0);
    const size_t eventStart=backend.events.size();const unsigned drawStart=backend.draws;
    issueTriangle();
    const uint64_t oldRevision=device.textureInfo(tex).revision;
    hikaru_glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,1,GL_RGBA,GL_UNSIGNED_BYTE,initial);
    device.checkError();
    check(backend.draws==drawStart+1&&backend.drawHistory.back().sampling[0].revision==oldRevision,
          "queued draw did not capture the pre-update texture revision");
    check(backend.events.size()>=eventStart+2&&backend.events[eventStart]=="draw"&&backend.events[eventStart+1]=="update",
          "texture update was issued before the queued draw");
    issueTriangle();device.checkError();
    check(backend.draws==drawStart+2&&backend.drawHistory.back().sampling[0].revision==device.textureInfo(tex).revision,
          "draw after a full update did not observe the new texture revision");

    const size_t callsBeforeInvalid=backend.textureCalls.size();const uint64_t revisionBeforeInvalid=device.textureInfo(tex).revision;
    auto expectUpdateError=[&](GLint level,GLint x,GLint y,GLsizei width,GLsizei height,GLenum format,GLenum type,const void* pixels,const char* message) {
        hikaru_glTexSubImage2D(GL_TEXTURE_2D,level,x,y,width,height,format,type,pixels);
        bool threw=false;try{device.checkError();}catch(const std::runtime_error&){threw=true;}
        check(threw,message);
    };
    expectUpdateError(0,1,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial,"partial texture bounds were accepted");
    expectUpdateError(1,0,0,2,1,GL_RGBA,GL_UNSIGNED_BYTE,initial,"nonzero texture mip level was accepted");
    expectUpdateError(0,0,0,2,1,GL_RGB,GL_UNSIGNED_BYTE,initial,"incompatible texture source format was accepted");
    expectUpdateError(0,0,0,0,1,GL_RGBA,GL_UNSIGNED_BYTE,initial,"zero-width texture update was accepted");
    expectUpdateError(0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial,"partial-size texture update was accepted");
    expectUpdateError(0,0,0,2,1,GL_RGBA,GL_UNSIGNED_BYTE,nullptr,"null full texture update was accepted");
    check(backend.textureCalls.size()==callsBeforeInvalid&&device.textureInfo(tex).revision==revisionBeforeInvalid,
          "invalid full texture update changed backend state");

    const uint8_t redefined[]={20,21,22,23,24,25,26,27};
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,redefined);
    device.checkError();
    check(backend.textureCalls.back().upload==TextureUpload::Definition
          &&backend.textureCalls.back().pixels==std::vector<uint8_t>(redefined,redefined+sizeof(redefined))
          &&device.textureInfo(tex).maxLevel==defined.maxLevel,
          "full TexImage2D redefinition followed the update path or lost mip metadata");

    GLuint packed16=0;hikaru_glGenTextures(1,&packed16);hikaru_glBindTexture(GL_TEXTURE_2D,packed16);
    const uint16_t abgr1555[]={0x801f,0x83e0,0x7c00};
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGB5_A1,3,1,0,GL_RGBA,GL_UNSIGNED_SHORT_1_5_5_5_REV,abgr1555);
    hikaru_glTexSubImage2D(GL_TEXTURE_2D,0,0,0,3,1,GL_RGBA,GL_UNSIGNED_SHORT_1_5_5_5_REV,abgr1555);
    device.checkError();
    check(backend.textureCalls.back().pixels==std::vector<uint8_t>({255,0,0,255,0,255,0,255,0,0,255,0}),
          "ABGR1555 full update changed its channel lanes or alpha bit");

    GLuint packed10=0;hikaru_glGenTextures(1,&packed10);hikaru_glBindTexture(GL_TEXTURE_2D,packed10);
    const uint32_t a2bgr10=(2u<<30)|(17u<<20)|(513u<<10)|777u;
    hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGB10_A2,1,1,0,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,&a2bgr10);
    const uint32_t a2bgr10Updated=(1u<<30)|(1001u<<20)|(23u<<10)|42u;
    hikaru_glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,&a2bgr10Updated);
    device.checkError();
    check(backend.textureCalls.back().pixels.size()==sizeof(a2bgr10Updated),"A2BGR10 full update byte count mismatch");
    uint32_t packedObserved=0;std::memcpy(&packedObserved,backend.textureCalls.back().pixels.data(),sizeof(packedObserved));
    check(packedObserved==a2bgr10Updated,
          "A2BGR10 full update changed packed channels or alpha bits");
    const size_t packedCalls=backend.textureCalls.size();
    expectUpdateError(0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,&a2bgr10Updated,"incompatible A2BGR10 source type was accepted");
    check(backend.textureCalls.size()==packedCalls,"invalid A2BGR10 update reached the backend");
}

void testFamilyAttachments(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    GLuint images[3]={},fbo=0;
    hikaru_glGenTextures(3,images);
    for(unsigned i=0;i<3;++i){
        hikaru_glActiveTexture(GL_TEXTURE0);hikaru_glBindTexture(GL_TEXTURE_2D,images[i]);
        GLenum internal=i==0?GL_RGBA8:i==1?GL_RG32UI:GL_DEPTH_COMPONENT24;
        GLenum format=i==0?GL_RGBA:i==1?GL_RG_INTEGER:GL_DEPTH_COMPONENT;
        hikaru_glTexImage2D(GL_TEXTURE_2D,0,internal,8,8,0,format,i==0?GL_UNSIGNED_BYTE:GL_UNSIGNED_INT,nullptr);
    }
    hikaru_glGenFramebuffers(1,&fbo);hikaru_glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    hikaru_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,images[0],0);
    hikaru_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D,images[1],0);
    hikaru_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,images[2],0);
    const GLenum buffers[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1};
    hikaru_glDrawBuffers(2,buffers);
    GLuint program=makeProgram("#define HAS_TEXTURE 0\n");
    hikaru_glUniform3f(hikaru_glGetUniformLocation(program,"u_family"),0,1,4194304);
    issueTriangle();device.checkError();
    check(backend.lastDraw.color==images[0]&&backend.lastDraw.depth==images[2]
          &&backend.lastDraw.metadata==images[1],"family MRT attachment routing lost an image");
    check(backend.lastDraw.uniforms.family==Vec4{0,1,4194304,65535},"family ordinal or mode lost during native packing");
    check(backend.infos.at(images[1]).format==GL_RG32UI,"unsigned family metadata was converted to RGBA8");
    const GLuint empty[4]={};hikaru_glClearBufferuiv(GL_COLOR,1,empty);device.checkError();
    check(backend.lastClear.color==images[1]&&!backend.lastClear.depth&&!backend.lastClear.metadata
          &&backend.lastClearMask==GL_COLOR_BUFFER_BIT,"metadata clear touched the family color/depth target");
    hikaru_glDrawBuffers(1,buffers);hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(!backend.lastDraw.metadata,"single-target draw retained the family MRT attachment");
    hikaru_glBindFramebuffer(GL_FRAMEBUFFER,0);
    makeProgram("#define FAMILY_RESOLVE 1\n// u_front u_back u_layer1 u_layer2\n");
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.bitmap&&backend.lastDraw.familyResolve&&!backend.lastDraw.metadata,
          "family resolve reached the ordinary mesh/bitmap native shader");
    for(unsigned i=0;i<4;++i){hikaru_glActiveTexture(GL_TEXTURE0+i);hikaru_glBindTexture(GL_TEXTURE_2D,0);}
    hikaru_glActiveTexture(GL_TEXTURE0);hikaru_glDeleteFramebuffers(1,&fbo);hikaru_glDeleteTextures(3,images);
    device.checkError();
}

void testUnavailableDrawPalettes(Device& device) {
    Device::Scope scope(&device);
    check(!hikaru_graphics_supports_draw_palette(), "native backend advertised GL draw palettes");
    const GLuint program = makeProgram("#define HAS_DRAW_PALETTE 0\n#define HAS_TEXTURE_PALETTE 0\n#define HAS_SHADING_PALETTE 0\n");
    for (const char* name : {"u_modelview_palette", "u_modelview_alternate_palette",
            "u_normal_palette", "u_normal_alternate_palette", "u_draw_depth",
            "u_textures", "u_texture_modes", "u_shading_modes"}) {
        check(hikaru_glGetUniformLocation(program, name) == -1,
            "native backend advertised an unsupported draw-palette uniform");
        const std::string indexed = std::string(name) + "[0]";
        check(hikaru_glGetUniformLocation(program, indexed.c_str()) == -1,
            "native backend advertised an unsupported indexed draw-palette uniform");
    }
    // These are required by the native shader's ordinary mesh/texture contract.
    check(hikaru_glGetUniformLocation(program, "u_projection") >= 0 &&
          hikaru_glGetUniformLocation(program, "u_texture") >= 0 &&
          hikaru_glGetUniformLocation(program, "u_lights[0].diffuse") >= 0,
          "native palette capability check disabled ordinary mesh uniforms");
    hikaru_glDeleteProgram(program);
    device.checkError();
}

void testLightCacheUpdates(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    const GLuint program=makeProgram("#define HAS_LIGHT0 1\n#define HAS_LIGHT1 1\n");
    const GLint diffuse=hikaru_glGetUniformLocation(program,"u_lights[0].diffuse");
    const GLint table=hikaru_glGetUniformLocation(program,"u_lights[0].spot_table[0]");
    const GLfloat spotTable[]={1,2,3,4,5};
    hikaru_glUniform3f(diffuse,0.25f,0.5f,0.75f);
    hikaru_glUniform3f(hikaru_glGetUniformLocation(program,"u_lights[1].position"),7,8,9);
    hikaru_glUniform1fv(table,5,spotTable);
    issueTriangle();device.checkError();
    const auto lights=backend.lastDraw.uniforms.lights;
    const GLfloat projection[]={1,0,0,0, 0,2,0,0, 0,0,3,0, 0,0,0,1};
    hikaru_glUniformMatrix4fv(hikaru_glGetUniformLocation(program,"u_projection"),1,GL_FALSE,projection);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(std::memcmp(lights.data(),backend.lastDraw.uniforms.lights.data(),sizeof(lights))==0,
          "matrix update changed the cached light set");
    hikaru_glUniform3f(diffuse,1,0,0);
    hikaru_glUniform1fv(table,1,spotTable);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    const auto& updated=backend.lastDraw.uniforms.lights;
    check(updated[0].diffuse==Vec4{1,0,0,0}&&updated[1].position==Vec4{7,8,9,0},
          "light update retained stale data or lost another light");
    check(updated[0].table[0]==Vec4{1,0,0,0}&&updated[0].table[1]==Vec4{},
          "shorter spotlight update retained old cached entries");
    hikaru_glLinkProgram(program);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.uniforms.lights[0].diffuse==Vec4{1,0,0,0},
          "relink lost existing native light values");
}

void testCachedDepthAndFamilyUpdates(RecordingBackend& backend,Device& device) {
    Device::Scope scope(&device);
    const GLuint program=makeProgram("#define HAS_PATTERN 1\n#define HAS_TEXTURE 1\n");
    hikaru_glUniform1f(hikaru_glGetUniformLocation(program,"u_pattern"),float(0x5a5a));
    hikaru_glDepthRangef(0.2f,0.8f);issueTriangle();device.checkError();
    const auto previous=backend.lastDraw.uniforms;
    const GLint depth=hikaru_glGetUniformLocation(program,"u_depth");
    hikaru_glUniform2f(depth,0.125f,0.25f);
    hikaru_glUniform1f(hikaru_glGetUniformLocation(program,"u_sort_scale"),2.f);
    hikaru_glUniform3f(hikaru_glGetUniformLocation(program,"u_family"),1.f,2.f,3.f);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    const auto warm=backend.lastDraw.uniforms;
    check(warm.depth==Vec4{0.125f,0.25f,0.2f,0.8f}&&warm.family==Vec4{1,2,3,float(0x5a5a)}&&warm.material[3]==2,
          "warm depth/family update lost range, sort scale or pattern coverage");
    check(warm.projection==previous.projection&&warm.flags==previous.flags&&
          std::memcmp(warm.lights.data(),previous.lights.data(),sizeof(warm.lights))==0,
          "depth update disturbed unrelated packed state");
    // A separate material update forces the full packer to rebuild the same
    // depth/family fields; it must agree with the warm incremental path.
    hikaru_glUniform1f(hikaru_glGetUniformLocation(program,"u_factor"),0.5f);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.uniforms.depth==warm.depth&&backend.lastDraw.uniforms.family==warm.family&&
          backend.lastDraw.uniforms.material==warm.material,"full repack disagrees with warm depth/family constants");
    // Short uploads preserve zero defaults for omitted vector components.
    hikaru_glUniform1f(depth,0.375f);hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.uniforms.depth==Vec4{0.375f,0,0.2f,0.8f},"short depth upload retained a stale placement scale");
}

void testVertexRanges(RecordingBackend& backend, Device& device) {
    Device::Scope scope(&device);
    makeProgram("");
    GLuint vao=0,buffer=0;
    hikaru_glGenVertexArrays(1,&vao);hikaru_glBindVertexArray(vao);
    hikaru_glGenBuffers(1,&buffer);hikaru_glBindBuffer(GL_ARRAY_BUFFER,buffer);
    const uint16_t data[]={99,0,1,2, 99,3,4,5, 99,6,7,8, 99,9,10,11, 99,12,13,14, 99,15,16,17};
    hikaru_glBufferData(GL_ARRAY_BUFFER,sizeof(data),data,GL_STATIC_DRAW);
    hikaru_glEnableVertexAttribArray(0);
    hikaru_glVertexAttribPointer(0,3,GL_UNSIGNED_SHORT,GL_TRUE,8,reinterpret_cast<const void*>(2));
    hikaru_glDrawArrays(GL_TRIANGLES,2,3);device.checkError();
    check(backend.lastDraw.vertices.size()==3&&
          near(backend.lastDraw.vertices.front().position[0],6.f/65535.f)&&
          near(backend.lastDraw.vertices.back().position[2],14.f/65535.f)&&
          backend.lastDraw.vertices[0].position[3]==0&&backend.lastDraw.vertices[0].normal==Vec4{},
          "strided, offset vertex conversion changed values or default components");
    const unsigned draws=backend.draws;
    hikaru_glDrawArrays(GL_TRIANGLES,4,3);
    bool failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed&&backend.draws==draws,"truncated final vertex reached the backend");
    hikaru_glVertexAttribPointer(0,3,GL_UNSIGNED_SHORT,GL_FALSE,8,reinterpret_cast<const void*>(UINTPTR_MAX));
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);
    failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed&&backend.draws==draws,"overflowing vertex offset reached the backend");
    hikaru_glVertexAttribPointer(0,3,GL_UNSIGNED_SHORT,GL_FALSE,-1,nullptr);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);
    failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed&&backend.draws==draws,"negative vertex stride reached the backend");
    // Native float attributes may be unaligned, interleaved, and start at a
    // nonzero vertex. Exercise all widths and ensure disabled tails stay zero.
    std::array<uint8_t,121> interleaved{};
    for(unsigned i=0;i<6;++i)for(unsigned c=0;c<4;++c) {
        const float value=float(i*4+c)+0.25f;
        std::memcpy(interleaved.data()+1+i*20+c*4,&value,sizeof(value));
    }
    hikaru_glBufferData(GL_ARRAY_BUFFER,interleaved.size(),interleaved.data(),GL_STATIC_DRAW);
    for(unsigned width=1;width<=4;++width) {
        hikaru_glVertexAttribPointer(0,GLint(width),GL_FLOAT,GL_FALSE,20,reinterpret_cast<const void*>(1));
        hikaru_glDrawArrays(GL_TRIANGLES,1,3);device.checkError();
        for(unsigned i=0;i<3;++i)for(unsigned c=0;c<4;++c)
            check(backend.lastDraw.vertices[i].position[c]==(c<width?float((i+1)*4+c)+0.25f:0.f),
                  "unaligned float attribute width, stride, first vertex or zero tail changed");
    }
    const float flags[]={0.5f,99,99,99, 0.25f,99,99,99, 1,99,99,99};
    const float selectors[]={64,99,99,99, 128,99,99,99, 192,99,99,99};
    setAttribute(7,4,GL_FLOAT,GL_FALSE,flags,sizeof(flags));
    setAttribute(8,4,GL_FLOAT,GL_FALSE,selectors,sizeof(selectors));
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(backend.lastDraw.vertices[0].extra==Vec4{0.5f,64,0,0}&&backend.lastDraw.vertices[2].extra==Vec4{1,192,0,0},
          "scalar alpha/selector conversion overwrote neighboring extra components");
    hikaru_glDeleteVertexArrays(1,&vao);hikaru_glDeleteBuffers(1,&buffer);
    device.checkError();
}

void testPackedVertexDispatch() {
    auto backend=std::make_unique<RecordingBackend>();auto* record=backend.get();record->packedSupport=true;record->paletteSupport=true;
    Device device(std::move(backend));Device::Scope scope(&device);device.begin(16,16);makeProgram("");
    std::array<PackedVertex,4> vertices{};
    for(unsigned i=0;i<vertices.size();++i){vertices[i].position={float(i),float(i+1),-1};vertices[i].diffuse={10,20,30};
        vertices[i].normal={0,0,1};vertices[i].alpha=191;vertices[i].ambient={40,50,60};vertices[i].matrixSelect=193;
        vertices[i].specular={70,80,90,100};vertices[i].unknown={101,202,303};vertices[i].uv={0.25f,0.75f};}
    constexpr size_t body=offsetof(PackedVertex,drawIndex);std::array<uint8_t,body*4> bytes{};
    for(unsigned i=0;i<vertices.size();++i)std::memcpy(bytes.data()+i*body,&vertices[i],body);
    GLuint vao=0,buffer=0;hikaru_glGenVertexArrays(1,&vao);hikaru_glBindVertexArray(vao);
    hikaru_glGenBuffers(1,&buffer);hikaru_glBindBuffer(GL_ARRAY_BUFFER,buffer);hikaru_glBufferData(GL_ARRAY_BUFFER,bytes.size(),bytes.data(),GL_STATIC_DRAW);
    const GLint widths[]={3,3,3,3,4,3,2,1,1};
    const GLenum formats[]={GL_FLOAT,GL_FLOAT,GL_UNSIGNED_BYTE,GL_UNSIGNED_BYTE,GL_UNSIGNED_BYTE,GL_UNSIGNED_SHORT,GL_FLOAT,GL_UNSIGNED_BYTE,GL_UNSIGNED_BYTE};
    const GLboolean normalized[]={GL_FALSE,GL_FALSE,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE,GL_FALSE,GL_TRUE,GL_FALSE};
    const size_t offsets[]={0,16,12,28,40,48,32,15,31};
    for(unsigned i=0;i<9;++i){hikaru_glVertexAttribPointer(i,widths[i],formats[i],normalized[i],body,reinterpret_cast<const void*>(offsets[i]));hikaru_glEnableVertexAttribArray(i);}
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);hikaru_glDrawArrays(GL_TRIANGLES,1,3);device.checkError();
    check(record->lastDraw.vertices.empty()&&record->lastDraw.packedVertices.size()==6,"compatible packed draws did not retain adjacent ordered merging");
    check(record->lastDraw.packedVertices.front().position==vertices[1].position&&record->lastDraw.packedVertices.back().unknown==vertices[3].unknown&&
          record->lastDraw.packedVertices[0].alpha==191&&record->lastDraw.packedVertices[0].matrixSelect==193,"packed first vertex, colors or selector bytes changed");
    // A layout that the input assembler cannot reproduce must keep the old
    // conversion path, including unusual normalization and missing padding.
    hikaru_glVertexAttribPointer(8,1,GL_UNSIGNED_BYTE,GL_TRUE,body,reinterpret_cast<const void*>(31));
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);device.checkError();
    check(record->lastDraw.packedVertices.empty()&&near(record->lastDraw.vertices[0].extra[1],193.f/255.f),"nonstandard normalized selector escaped generic conversion");
    hikaru_glVertexAttribPointer(8,1,GL_UNSIGNED_BYTE,GL_FALSE,body,reinterpret_cast<const void*>(31));
    hikaru_glBufferData(GL_ARRAY_BUFFER,bytes.size()-2,bytes.data(),GL_STATIC_DRAW);
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);device.checkError();
    check(record->lastDraw.packedVertices.empty()&&record->lastDraw.vertices.size()==3,"truncated unused padding was read by the packed copy");
    hikaru_glBufferData(GL_ARRAY_BUFFER,bytes.size(),bytes.data(),GL_STATIC_DRAW);
    const GLuint program=makeProgram("#define HAS_DRAW_PALETTE 1\n");
    std::array<float,4> depth={1,1,0.5f,2};hikaru_glUniform2fv(hikaru_glGetUniformLocation(program,"u_draw_depth[0]"),2,depth.data());
    GLuint indices=0;hikaru_glGenBuffers(1,&indices);hikaru_glBindBuffer(GL_ARRAY_BUFFER,indices);
    std::array<uint8_t,4> index={0,1,1,1};hikaru_glBufferData(GL_ARRAY_BUFFER,index.size(),index.data(),GL_DYNAMIC_DRAW);hikaru_graphics_bind_draw_palette(indices);
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);device.checkError();
    check(record->lastDraw.packedVertices.size()==3&&record->lastDraw.packedVertices[0].drawIndex==1&&record->lastDraw.palette.size()==2&&
          record->lastDraw.palette[1].modes==Vec4{0.5f,2,0,0},"packed palette record, depth or sort scale changed");
    const unsigned previous=record->draws;index[2]=0;hikaru_glBufferSubData(GL_ARRAY_BUFFER,0,index.size(),index.data());
    hikaru_glDrawArrays(GL_TRIANGLES,1,3);bool failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed&&record->draws==previous,"one packed triangle selected multiple palette records");
}

void testNativePalettes() {
    auto backend=std::make_unique<RecordingBackend>();auto* record=backend.get();
    record->paletteSupport=true;record->texturePaletteSupport=true;
    Device device(std::move(backend));Device::Scope scope(&device);device.begin(8,8);
    check(hikaru_graphics_supports_draw_palette()&&hikaru_graphics_native_texture_palette()==1,
          "native palette capabilities did not reach shared replay");
    GLint unitsAvailable=0;hikaru_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&unitsAvailable);
    check(unitsAvailable==20,"native sampler query disagrees with accepted palette units");
    const GLuint p=makeProgram("#define HAS_DRAW_PALETTE 1\n#define HAS_TEXTURE_PALETTE 1\n#define HAS_SHADING_PALETTE 1\n#define HAS_LIGHT0 1\n");
    const GLint model=hikaru_glGetUniformLocation(p,"u_modelview_palette[0]");
    check(model>=0&&model==hikaru_glGetUniformLocation(p,"u_modelview_palette"),"palette base/element-zero locations differ");
    std::array<GLfloat,16*16> matrices{};std::array<GLfloat,16*9> normals{};std::array<GLfloat,16*2> depths{};
    std::array<GLint,16> units{},textureModes{},shadingModes{};
    for(unsigned i=0;i<16;++i){for(unsigned j=0;j<4;++j)matrices[i*16+j*5]=1;matrices[i*16+12]=float(i);
        for(unsigned j=0;j<9;++j)normals[i*9+j]=float(i*9+j);depths[i*2]=float(i+1);depths[i*2+1]=float(i+2);
        units[i]=4+i;textureModes[i]=i%5;shadingModes[i]=i%4;}
    hikaru_glUniformMatrix4fv(model,16,GL_FALSE,matrices.data());
    hikaru_glUniformMatrix4fv(hikaru_glGetUniformLocation(p,"u_modelview_alternate_palette[0]"),16,GL_FALSE,matrices.data());
    hikaru_glUniformMatrix3fv(hikaru_glGetUniformLocation(p,"u_normal_palette[0]"),16,GL_FALSE,normals.data());
    hikaru_glUniformMatrix3fv(hikaru_glGetUniformLocation(p,"u_normal_alternate_palette[0]"),16,GL_FALSE,normals.data());
    hikaru_glUniform2fv(hikaru_glGetUniformLocation(p,"u_draw_depth[0]"),16,depths.data());
    hikaru_glUniform1iv(hikaru_glGetUniformLocation(p,"u_textures[0]"),16,units.data());
    hikaru_glUniform1iv(hikaru_glGetUniformLocation(p,"u_texture_modes[0]"),16,textureModes.data());
    hikaru_glUniform1iv(hikaru_glGetUniformLocation(p,"u_shading_modes[0]"),16,shadingModes.data());
    std::array<GLuint,16> textures{};hikaru_glGenTextures(16,textures.data());
    const uint8_t pixel[]={20,40,60,255};
    for(unsigned i=0;i<16;++i){hikaru_glActiveTexture(GL_TEXTURE0+4+i);hikaru_glBindTexture(GL_TEXTURE_2D,textures[i]);
        hikaru_glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        hikaru_glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,2);}
    GLuint sampler=0;hikaru_glGenSamplers(1,&sampler);
    hikaru_glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    hikaru_glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    hikaru_glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,GL_MIRRORED_REPEAT);
    hikaru_glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,GL_REPEAT);
    hikaru_glSamplerParameterf(sampler,GL_TEXTURE_LOD_BIAS,-0.75f);hikaru_glBindSampler(19,sampler);
    GLuint vao=0;hikaru_glGenVertexArrays(1,&vao);hikaru_glBindVertexArray(vao);
    const GLfloat positions[]={0,0,0,1, 1,0,0,1, 0,1,0,1};
    setAttribute(0,4,GL_FLOAT,GL_FALSE,positions,sizeof(positions));
    GLuint indices=0;hikaru_glGenBuffers(1,&indices);hikaru_glBindBuffer(GL_ARRAY_BUFFER,indices);
    std::array<uint8_t,3> index={15,15,15};
    hikaru_glBufferData(GL_ARRAY_BUFFER,index.size(),index.data(),GL_DYNAMIC_DRAW);hikaru_graphics_bind_draw_palette(indices);
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    const auto& draw=record->lastDraw;
    check(draw.palette.size()==16&&draw.vertices[2].extra[2]==15,"16-record palette index or payload was lost");
    check(draw.palette[15].modelview[12]==15&&draw.palette[15].alternate[12]==15&&
          draw.palette[15].normal==Matrix{135,136,137,0,138,139,140,0,141,142,143,0,0,0,0,0},
          "palette matrices or std430 normal padding changed");
    check(draw.palette[15].modes==Vec4{16,17,0,3}&&draw.paletteTextures[15]==textures[15],
          "placement/sort scales, material modes, or texture selection were lost");
    check(draw.paletteSampling[15].wrapS==GL_MIRRORED_REPEAT&&draw.paletteSampling[15].maxLevel==2&&
          draw.paletteSampling[15].minFilter==GL_LINEAR_MIPMAP_LINEAR&&draw.palette[15].sampling[0]==-0.75f,
          "sampler override lost LOD/filtering or replaced texture-only mip state");
    matrices[12]=99;hikaru_glUniformMatrix4fv(model,1,GL_FALSE,matrices.data());
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(record->lastDraw.palette[0].modelview[12]==99&&record->lastDraw.palette[15].modelview[12]==15,
          "short palette update destroyed the cached tail");
    const unsigned before=record->draws;
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(record->draws==before+2,"palette draws were joined without remapping their records");
    index={16,16,16};hikaru_glBufferSubData(GL_ARRAY_BUFFER,0,index.size(),index.data());
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);
    bool failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed,"17th record escaped native palette bounds");
    index={0,1,0};hikaru_glBufferSubData(GL_ARRAY_BUFFER,0,index.size(),index.data());hikaru_glDrawArrays(GL_TRIANGLES,0,3);
    failed=false;try{device.checkError();}catch(const std::runtime_error&){failed=true;}
    check(failed,"one triangle selected multiple palette records");
    hikaru_glDeleteSamplers(1,&sampler);index={15,15,15};hikaru_glBufferSubData(GL_ARRAY_BUFFER,0,index.size(),index.data());
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(record->lastDraw.palette[15].sampling[0]==0,"deleted sampler remained bound to a palette unit");
    const GLuint primaryOnly=makeProgram("#define HAS_DRAW_PALETTE 1\n");
    hikaru_glUniformMatrix4fv(hikaru_glGetUniformLocation(primaryOnly,"u_modelview_palette[0]"),16,GL_FALSE,matrices.data());
    hikaru_glUniformMatrix4fv(hikaru_glGetUniformLocation(primaryOnly,"u_modelview_alternate_palette[0]"),1,GL_FALSE,matrices.data());
    hikaru_glUniform2fv(hikaru_glGetUniformLocation(primaryOnly,"u_draw_depth[0]"),16,depths.data());
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();
    check(record->lastDraw.palette[15].modelview[12]==15&&record->lastDraw.palette[15].alternate==Matrix{},
          "unused shorter matrix bank rejected the run or lost GL's zero defaults");
    hikaru_graphics_bind_draw_palette(0);
    const GLuint scalar=makeProgram("#define HAS_DRAW_PALETTE 0\n#define HAS_TEXTURE_PALETTE 0\n");
    check(hikaru_glGetUniformLocation(scalar,"u_modelview_palette[0]")==-1&&
          hikaru_glGetUniformLocation(scalar,"u_textures[0]")==-1,"scalar variant advertised inactive arrays");
    hikaru_glDrawArrays(GL_TRIANGLES,0,3);device.checkError();check(record->lastDraw.palette.empty(),"palette disable leaked into scalar draws");
}

const HikaruGraphicsDispatch* priorDispatch = nullptr;
void restoredClearColor(GLfloat, GLfloat, GLfloat, GLfloat) { priorDispatch = reinterpret_cast<const HikaruGraphicsDispatch*>(uintptr_t(1)); }

void testScopeAndExceptionContainment(Device& device) {
    HikaruGraphicsDispatch sentinel{}; sentinel.ClearColor = restoredClearColor;
    hikaru_graphics_set_dispatch(&sentinel);
    {
        Device::Scope outer(&device);
        hikaru_glClearColor(1,2,3,4);
        {
            Device nested(std::make_unique<RecordingBackend>());
            Device::Scope inner(&nested);
            hikaru_glClearColor(4,3,2,1);
        }
        hikaru_glClearColor(1,2,3,4);
        bool threw = false;
        hikaru_glActiveTexture(GL_TEXTURE0 + 9); // callback catches the unsupported operation
        try { device.checkError(); } catch (const std::runtime_error&) { threw = true; }
        check(threw, "dispatch callback did not capture backend operation exception");
        hikaru_glClearColor(0,0,0,1);
        device.checkError();
    }
    hikaru_glClearColor(0,0,0,0);
    check(priorDispatch != nullptr, "Scope failed to restore the previous dispatch");
    hikaru_graphics_set_dispatch(nullptr);
}
}

int main() {
    try {
        auto backend = std::make_unique<RecordingBackend>();
        auto* record = backend.get();
        Device device(std::move(backend));
        unsigned failures = 0;
        auto run = [&](const char* name, auto&& fn) {
            try { fn(); std::cout << "PASS: " << name << "\n"; }
            catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << name << ": " << e.what() << "\n"; }
        };
        run("dispatch conversion, uniforms, draw state", [&] { testDispatchConversionUniformsAndState(*record, device); });
        run("four-unit peel dispatch and deferred query ownership", [&] { testPeelDispatchAndDeferredQueries(*record, device); });
        run("texture unpack and packed precision", [&] { testTexturePacking(*record, device); });
        run("full texture update semantics", [&] { testFullTextureUpdates(*record, device); });
        run("family MRT, unsigned metadata and resolve dispatch", [&] { testFamilyAttachments(*record, device); });
        run("native draw-palette capability and optional uniforms", [&] { testUnavailableDrawPalettes(device); });
        run("light cache across matrices, light edits and relink", [&] { testLightCacheUpdates(*record, device); });
        run("cached placement, sort depth and family updates", [&] { testCachedDepthAndFamilyUpdates(*record, device); });
        run("strided vertex conversion and complete-range bounds", [&] { testVertexRanges(*record, device); });
        run("packed layout, fallback, ordering and palette records", [&] { testPackedVertexDispatch(); });
        run("native matrix, texture and shading palettes", [&] { testNativePalettes(); });
        run("nested scope restoration and exception containment", [&] { testScopeAndExceptionContainment(device); });
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "Hikaru graphics Device dispatch test failed: " << e.what() << "\n";
        return 1;
    }
}
