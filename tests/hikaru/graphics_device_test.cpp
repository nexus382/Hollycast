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
#include <vector>

using namespace hikaru::graphics;

namespace {
struct RecordingBackend final : Backend {
    Draw lastDraw{};
    bool drew = false;
    unsigned draws = 0;
    unsigned lastPresentWidth = 0, lastPresentHeight = 0;
    std::unordered_map<unsigned, std::vector<uint8_t>> uploads;
    std::unordered_map<unsigned, Texture> infos;
    std::vector<unsigned> erased;
    void texture(unsigned id, const Texture& info, unsigned, const void* pixels, size_t bytes) override {
        infos[id] = info;
        if (pixels && bytes) uploads[id].assign(static_cast<const uint8_t*>(pixels), static_cast<const uint8_t*>(pixels) + bytes);
    }
    void eraseTexture(unsigned id) override { erased.push_back(id); }
    void clear(const Draw&, GLbitfield, const Vec4&, float) override {}
    void draw(const Draw& draw) override { lastDraw = draw; drew = true;++draws; }
    void readPixels(unsigned, int, int, int, int, uint8_t*) override {}
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
    GLuint p = makeProgram("#define HAS_TEXTURE 1\n#define USE_PRELIT_COLOR 1\n");
    const GLfloat projection[] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    const GLfloat normal[] = {1,2,3,4,5,6,7,8,9};
    GLint loc = hikaru_glGetUniformLocation(p, "u_projection"); hikaru_glUniformMatrix4fv(loc, 1, GL_FALSE, projection);
    loc = hikaru_glGetUniformLocation(p, "u_normal"); hikaru_glUniformMatrix3fv(loc, 1, GL_FALSE, normal);
    loc = hikaru_glGetUniformLocation(p, "u_factor"); hikaru_glUniform1f(loc, 0.375f);
    issueTriangle();
    device.checkError();
    check(backend.drew, "dispatch wrapper did not reach Backend::draw");
    check(backend.lastDraw.vertices.size() == 3, "draw vertex count mismatch");
    const auto& v = backend.lastDraw.vertices[0];
    check(v.position == Vec4{1,2,3,4}, "float vertex conversion failed");
    check(near(v.normal[0], 0) && near(v.normal[1], 127.f/255.f) && near(v.normal[2], 1), "normalized byte vertex conversion failed");
    const auto& u = backend.lastDraw.uniforms;
    check(u.projection == Matrix{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, "matrix4 uniform packing failed");
    check(u.normal == Matrix{1,2,3,0,4,5,6,0,7,8,9,0,0,0,0,0}, "matrix3 std140 packing failed");
    check(near(u.factors[0], .375f) && near(u.factors[1], 1), "scalar uniform defaults failed");
    check(backend.lastDraw.viewport == std::array<GLint,4>{2,3,40,50}, "viewport state missing from draw");
    check(backend.lastDraw.blend && backend.lastDraw.srcRGB == GL_SRC_ALPHA && backend.lastDraw.dstAlpha == GL_ZERO,
          "blend state missing from draw");
    hikaru_glUniform1f(hikaru_glGetUniformLocation(p, "u_factor"), 0.625f);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(near(backend.lastDraw.uniforms.factors[0], 0.625f),
          "updated uniform did not invalidate the packed constant cache");
    check(backend.lastDraw.color == 0 && backend.lastDraw.textures[0] == 0, "default target zero and unbound texture zero differ unexpectedly");
    check(backend.infos.at(0).width == 64 && backend.infos.at(0).height == 32,
          "default framebuffer target zero was not allocated separately from the unbound sampler");

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
    hikaru_glBindTexture(GL_TEXTURE_2D, 0);
    hikaru_glDrawArrays(GL_TRIANGLES, 0, 3);
    device.checkError();
    check(backend.lastDraw.color == 0 && backend.lastDraw.textures[0] == 0,
          "unbound sampler did not remain texture zero while rendering to target zero");
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
        run("texture unpack and packed precision", [&] { testTexturePacking(*record, device); });
        run("nested scope restoration and exception containment", [&] { testScopeAndExceptionContainment(device); });
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "Hikaru graphics Device dispatch test failed: " << e.what() << "\n";
        return 1;
    }
}
