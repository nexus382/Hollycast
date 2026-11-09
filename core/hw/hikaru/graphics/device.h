// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(__APPLE__) && !defined(USE_OPENGL)
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif
#define HIKARU_GRAPHICS_DISPATCH_IMPLEMENTATION
#include "hw/hikaru/valkyrie/graphics_dispatch.h"
#undef HIKARU_GRAPHICS_DISPATCH_IMPLEMENTATION
#include <array>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hikaru::graphics {
using Vec4 = std::array<float, 4>;
using Matrix = std::array<float, 16>;
struct Vertex { Vec4 position{}, normal{}, diffuse{}, ambient{}, specular{}, unknown{}, uv{}, extra{}; };
// The board's interleaved vertex body is 56 bytes. DX11 can normalize its
// byte/word colors in the input assembler, avoiding CPU float expansion.
// The final float is our per-triangle palette index, not guest memory.
struct PackedVertex {
    std::array<float,3> position{};
    std::array<uint8_t,3> diffuse{}; uint8_t alpha=0;
    std::array<float,3> normal{};
    std::array<uint8_t,3> ambient{}; uint8_t matrixSelect=0;
    std::array<float,2> uv{};
    std::array<uint8_t,4> specular{};
    uint32_t reserved=0;
    std::array<uint16_t,3> unknown{}; uint16_t reservedTail=0;
    float drawIndex=0;
};
static_assert(offsetof(PackedVertex,drawIndex)==56&&sizeof(PackedVertex)==60);
struct Light {
    Vec4 position{}, direction{}, diffuse{}, specular{}, extents{}, spot{}, flags{};
    std::array<Vec4,4> table{};
};
// Identical 16-byte fields in std140 GLSL and Metal constant buffers.
struct Uniforms {
    Matrix projection{}, modelview{}, alternate{}, normal{}, alternateNormal{};
    Vec4 ambient{}, fog{}, fogColor{}, flags{}, material{}, peel{}, depth{}, family{}, blend{};
    std::array<Light,4> lights{};
    Vec4 factors{}, multipliers{}, direct{}, keyed{}, inverted{}, lodBias{};
};
// std430 records mirror the existing GL palette. Indices remain per vertex,
// so one ordered draw retains each triangle's transform and sorting scales.
inline constexpr unsigned DrawPaletteSize = 16;
struct DrawRecord {
    Matrix modelview{}, alternate{}, normal{}, alternateNormal{};
    Vec4 modes{}; // placement scale, sort scale, texture mode, shading mode
    Vec4 sampling{}; // texture LOD bias
};
static_assert(sizeof(DrawRecord) == 288);
static_assert(sizeof(Vertex) == 128);
static_assert(sizeof(Uniforms) % 16 == 0);
struct Texture {
    unsigned width = 0, height = 0;
    GLenum format = GL_RGBA8;
    GLenum minFilter = GL_NEAREST, magFilter = GL_NEAREST;
    GLenum wrapS = GL_CLAMP_TO_EDGE, wrapT = GL_CLAMP_TO_EDGE;
    unsigned maxLevel = 1000;
    float lodBias = 0;
    uint64_t revision = 0;
};
// Distinguish TexImage storage definitions from full level-zero replacements.
enum class TextureUpload : uint8_t { Definition, FullUpdate };
struct Draw {
    unsigned color = 0, depth = 0, metadata = 0;
    std::array<unsigned,4> textures{};
    std::array<Texture,4> sampling{};
    std::array<GLint,4> viewport{}, scissor{};
    bool scissorEnabled = false, depthTest = false, depthWrite = true, blend = false, cull = false, bitmap = false, familyResolve = false;
    GLenum depthFunc = GL_LESS, cullFace = GL_BACK, frontFace = GL_CCW;
    GLenum srcRGB = GL_ONE, dstRGB = GL_ZERO, srcAlpha = GL_ONE, dstAlpha = GL_ZERO;
    GLenum equationRGB = GL_FUNC_ADD, equationAlpha = GL_FUNC_ADD, topology = GL_TRIANGLES;
    std::array<bool,4> colorMask{true,true,true,true};
    float biasFactor = 0, biasUnits = 0, nearDepth = 0, farDepth = 1;
    Uniforms uniforms{};
    std::vector<Vertex> vertices;
    std::vector<PackedVertex> packedVertices; // Mutually exclusive with vertices.
    std::vector<DrawRecord> palette;
    std::vector<unsigned> paletteTextures;
    std::vector<Texture> paletteSampling;
    bool texturePalette = false, shadingPalette = false;
};
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool supportsDrawPalette() const { return false; }
    virtual bool supportsTexturePalette() const { return false; }
    virtual bool supportsPackedVertices() const { return false; }
    // Pixel data is borrowed for the call; implementations copy or stage it before returning.
    virtual void texture(unsigned id, const Texture& info, unsigned level, TextureUpload upload, const void* pixels, size_t bytes) = 0;
    virtual void eraseTexture(unsigned id) = 0;
    virtual void clear(const Draw& state, GLbitfield mask, const Vec4& color, float depth) = 0;
    virtual void draw(const Draw& draw) = 0;
    virtual void readPixels(unsigned id, int x, int y, int width, int height, uint8_t* rgba) = 0;
    virtual void present(unsigned width, unsigned height) = 0;
    virtual void beginOcclusion() = 0;
    virtual bool endOcclusion() = 0;
};
std::unique_ptr<Backend> makeMetalBackend();
std::unique_ptr<Backend> makeVulkanBackend();
std::unique_ptr<Backend> makeDx11Backend();
class Device {
public:
    explicit Device(std::unique_ptr<Backend> backend);
    ~Device();
    void begin(unsigned width, unsigned height);
    void present();
    void checkError();
    class Scope {
    public:
        explicit Scope(Device* device);
        ~Scope();
    private:
        Device* previous;
        const HikaruGraphicsDispatch* previousDispatch;
    };
    const Texture& textureInfo(unsigned id) const { return textures.at(id); }
    static thread_local Device* active;
private:
    struct Attribute { unsigned buffer=0; GLint size=4; GLenum type=GL_FLOAT; bool normalized=false; GLsizei stride=0; size_t offset=0; bool enabled=false; };
    struct Program {
        bool bitmap=false, familyResolve=false;
        std::unordered_map<std::string,int> definitions;
        std::unordered_map<std::string,GLint> locations;
        std::unordered_map<GLint,std::vector<float>> values;
        std::vector<unsigned> shaders;
        mutable std::optional<Uniforms> cachedUniforms;
        struct MatrixUniform { Matrix Uniforms::*field; unsigned dimension; };
        std::unordered_map<GLint,MatrixUniform> matrixLocations;
        struct VectorUniform { Vec4 Uniforms::*field; unsigned offset, count; };
        std::unordered_map<GLint,VectorUniform> vectorLocations;
        // Matrix/material changes need not repack the unchanged light set.
        std::unordered_set<GLint> lightLocations;
        mutable std::optional<std::array<Light,4>> cachedLights;
        std::unordered_set<GLint> paletteLocations;
    };
    std::unique_ptr<Backend> backend;
    std::optional<Draw> pendingDraw;
    std::vector<Vertex> vertexScratch;
    std::vector<PackedVertex> packedVertexScratch;
    HikaruGraphicsDispatch dispatch{};
    std::exception_ptr error;
    unsigned nextId=1, program=0, buffer=0, vao=0, framebuffer=0, readFramebuffer=0, renderbuffer=0, activeTexture=0;
    unsigned width=0,height=0;
    std::array<unsigned,4 + DrawPaletteSize> boundTextures{}, boundSamplers{};
    std::unordered_map<unsigned,Texture> samplers;
    std::unordered_map<unsigned,std::vector<uint8_t>> buffers;
    std::unordered_map<unsigned,std::array<Attribute,16>> arrays;
    std::unordered_map<unsigned,Texture> textures;
    std::vector<uint8_t> fullTextureUpdatePixels;
    std::unordered_map<unsigned,std::array<unsigned,4>> targets;
    std::unordered_map<unsigned,std::string> shaders;
    std::unordered_map<unsigned,Program> programs;
    std::unordered_map<GLenum,GLint> pixelStore;
    std::unordered_set<GLenum> enabled;
    Draw state;
    Vec4 clearColor{};
    float clearDepth=1;
    GLint uniformCounter=0;
    Draw drawState() const;
    Texture sampling(unsigned unit) const;
    void packPalette(Draw& draw, const Program& program) const;
    void BindDrawPalette(GLuint stateBuffer);
    void GenSamplers(GLsizei count, GLuint* ids);
    void DeleteSamplers(GLsizei count, const GLuint* ids);
    void BindSampler(GLuint unit, GLuint id);
    void SamplerParameteri(GLuint id, GLenum name, GLint value);
    void SamplerParameterf(GLuint id, GLenum name, GLfloat value);
    void Uniform1iv(GLint location, GLsizei count, const GLint* values);
    void flushDraw();
    void queueDraw(Draw draw);
    Uniforms uniforms(const Program& p) const;
    void uniform(GLint location, size_t count, const float* data);
    void unpack(int width, int height, GLenum format, GLenum type, const void* data, std::vector<uint8_t>& pixels) const;
    void ActiveTexture(GLenum texture);
    void AttachShader(GLuint program, GLuint shader);
    void BindAttribLocation(GLuint program, GLuint index, const GLchar * name);
    void BindBuffer(GLenum target, GLuint buffer);
    void BindFramebuffer(GLenum target, GLuint framebuffer);
    void BindRenderbuffer(GLenum target, GLuint renderbuffer);
    void BindTexture(GLenum target, GLuint texture);
    void BindVertexArray(GLuint array);
    void BlendEquation(GLenum mode);
    void BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void BlendFunc(GLenum sfactor, GLenum dfactor);
    void BlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void BufferData(GLenum target, GLsizeiptr size, const void * data, GLenum usage);
    void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void * data);
    GLenum CheckFramebufferStatus(GLenum target);
    void Clear(GLbitfield mask);
    void ClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void ClearDepth(GLdouble depth);
    void ClearDepthf(GLfloat d);
    void ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void CompileShader(GLuint shader);
    GLuint CreateProgram(void);
    GLuint CreateShader(GLenum type);
    void CullFace(GLenum mode);
    void DeleteBuffers(GLsizei n, const GLuint * buffers);
    void DeleteFramebuffers(GLsizei n, const GLuint * framebuffers);
    void DeleteProgram(GLuint program);
    void DeleteRenderbuffers(GLsizei n, const GLuint * renderbuffers);
    void DeleteShader(GLuint shader);
    void DeleteTextures(GLsizei n, const GLuint * textures);
    void DeleteVertexArrays(GLsizei n, const GLuint * arrays);
    void DepthFunc(GLenum func);
    void DepthMask(GLboolean flag);
    void DepthRange(GLdouble n, GLdouble f);
    void DepthRangef(GLfloat n, GLfloat f);
    void Disable(GLenum cap);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void DrawBuffers(GLsizei n, const GLenum * bufs);
    void BindFragDataLocation(GLuint program, GLuint color, const GLchar * name);
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint * value);
    void Enable(GLenum cap);
    void EnableVertexAttribArray(GLuint index);
    void FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void FrontFace(GLenum mode);
    void GenBuffers(GLsizei n, GLuint * buffers);
    void GenFramebuffers(GLsizei n, GLuint * framebuffers);
    void GenRenderbuffers(GLsizei n, GLuint * renderbuffers);
    void GenTextures(GLsizei n, GLuint * textures);
    void GenVertexArrays(GLsizei n, GLuint * arrays);
    void GetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name);
    void GetBooleanv(GLenum pname, GLboolean * data);
    void GetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void * data);
    GLenum GetError(void);
    void GetFloatv(GLenum pname, GLfloat * data);
    void GetIntegerv(GLenum pname, GLint * data);
    void GetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void GetProgramiv(GLuint program, GLenum pname, GLint * params);
    void GetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void GetShaderiv(GLuint shader, GLenum pname, GLint * params);
    const GLubyte * GetString(GLenum name);
    GLint GetUniformLocation(GLuint program, const GLchar * name);
    GLboolean IsBuffer(GLuint buffer);
    GLboolean IsEnabled(GLenum cap);
    GLboolean IsProgram(GLuint program);
    GLboolean IsVertexArray(GLuint array);
    void LinkProgram(GLuint program);
    void PixelStorei(GLenum pname, GLint param);
    void PolygonOffset(GLfloat factor, GLfloat units);
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);
    void RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void Scissor(GLint x, GLint y, GLsizei width, GLsizei height);
    void ShaderSource(GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length);
    void TexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels);
    void TexParameterf(GLenum target, GLenum pname, GLfloat param);
    void TexParameteri(GLenum target, GLenum pname, GLint param);
    void TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels);
    void Uniform1f(GLint location, GLfloat v0);
    void Uniform1fv(GLint location, GLsizei count, const GLfloat * value);
    void Uniform1i(GLint location, GLint v0);
    void Uniform2f(GLint location, GLfloat v0, GLfloat v1);
    void Uniform2fv(GLint location, GLsizei count, const GLfloat * value);
    void Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void Uniform3fv(GLint location, GLsizei count, const GLfloat * value);
    void UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void UseProgram(GLuint program);
    void VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer);
    void Viewport(GLint x, GLint y, GLsizei width, GLsizei height);
};
} // namespace hikaru::graphics
