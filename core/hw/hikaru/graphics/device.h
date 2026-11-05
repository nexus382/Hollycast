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
struct Light {
    Vec4 position{}, direction{}, diffuse{}, specular{}, extents{}, spot{}, flags{};
    std::array<Vec4,4> table{};
};
// Identical 16-byte fields in std140 GLSL and Metal constant buffers.
struct Uniforms {
    Matrix projection{}, modelview{}, alternate{}, normal{}, alternateNormal{};
    Vec4 ambient{}, fog{}, fogColor{}, flags{}, material{}, peel{};
    std::array<Light,4> lights{};
    Vec4 factors{}, multipliers{}, direct{}, keyed{}, inverted{}, lodBias{};
};
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
    unsigned color = 0, depth = 0;
    std::array<unsigned,4> textures{};
    std::array<Texture,4> sampling{};
    std::array<GLint,4> viewport{}, scissor{};
    bool scissorEnabled = false, depthTest = false, depthWrite = true, blend = false, cull = false, bitmap = false;
    GLenum depthFunc = GL_LESS, cullFace = GL_BACK, frontFace = GL_CCW;
    GLenum srcRGB = GL_ONE, dstRGB = GL_ZERO, srcAlpha = GL_ONE, dstAlpha = GL_ZERO;
    GLenum equationRGB = GL_FUNC_ADD, equationAlpha = GL_FUNC_ADD, topology = GL_TRIANGLES;
    std::array<bool,4> colorMask{true,true,true,true};
    float biasFactor = 0, biasUnits = 0, nearDepth = 0, farDepth = 1;
    Uniforms uniforms{};
    std::vector<Vertex> vertices;
};
class Backend {
public:
    virtual ~Backend() = default;
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
        bool bitmap=false;
        std::unordered_map<std::string,int> definitions;
        std::unordered_map<std::string,GLint> locations;
        std::unordered_map<GLint,std::vector<float>> values;
        std::vector<unsigned> shaders;
        mutable std::optional<Uniforms> cachedUniforms;
    };
    std::unique_ptr<Backend> backend;
    std::optional<Draw> pendingDraw;
    HikaruGraphicsDispatch dispatch{};
    std::exception_ptr error;
    unsigned nextId=1, program=0, buffer=0, vao=0, framebuffer=0, readFramebuffer=0, renderbuffer=0, activeTexture=0;
    unsigned width=0,height=0;
    std::array<unsigned,4> boundTextures{};
    std::unordered_map<unsigned,std::vector<uint8_t>> buffers;
    std::unordered_map<unsigned,std::array<Attribute,16>> arrays;
    std::unordered_map<unsigned,Texture> textures;
    std::vector<uint8_t> fullTextureUpdatePixels;
    std::unordered_map<unsigned,std::array<unsigned,2>> targets;
    std::unordered_map<unsigned,std::string> shaders;
    std::unordered_map<unsigned,Program> programs;
    std::unordered_map<GLenum,GLint> pixelStore;
    std::unordered_set<GLenum> enabled;
    Draw state;
    Vec4 clearColor{};
    float clearDepth=1;
    GLint uniformCounter=0;
    Draw drawState() const;
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
