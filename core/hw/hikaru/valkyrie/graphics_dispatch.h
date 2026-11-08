// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// GL types are supplied by vk/core.h before this internal header.
#ifdef __cplusplus
extern "C" {
#endif
// A sequential visibility query avoids copying every peel target to the CPU.
// query[0..1] are caller-owned GL storage, unused by native backends.
void hikaru_graphics_begin_occlusion(GLuint query[2]);
GLboolean hikaru_graphics_end_occlusion(const GLuint query[2]);
void hikaru_graphics_delete_occlusion(GLuint query[2]);
// Direct GL/GLES implements the matrix palette in GLSL. Native dispatchers
// retain their existing vertex/uniform contract until they support it too.
GLboolean hikaru_graphics_supports_draw_palette(void);
// Own attribute 9 only on direct GL. Nonzero leaves ARRAY_BUFFER bound to it;
// callers restore their ordinary vertex buffer before configuring attributes.
void hikaru_graphics_bind_draw_palette(GLuint state_buffer);
typedef struct HikaruGraphicsDispatch {
    void (*BeginOcclusion)(void);
    GLboolean (*EndOcclusion)(void);
    void (*ActiveTexture)(GLenum texture);
    void (*AttachShader)(GLuint program, GLuint shader);
    void (*BindAttribLocation)(GLuint program, GLuint index, const GLchar * name);
    void (*BindBuffer)(GLenum target, GLuint buffer);
    void (*BindFramebuffer)(GLenum target, GLuint framebuffer);
    void (*BindRenderbuffer)(GLenum target, GLuint renderbuffer);
    void (*BindTexture)(GLenum target, GLuint texture);
    void (*BindVertexArray)(GLuint array);
    void (*BlendEquation)(GLenum mode);
    void (*BlendEquationSeparate)(GLenum modeRGB, GLenum modeAlpha);
    void (*BlendFunc)(GLenum sfactor, GLenum dfactor);
    void (*BlendFuncSeparate)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
    void (*BufferData)(GLenum target, GLsizeiptr size, const void * data, GLenum usage);
    void (*BufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void * data);
    GLenum (*CheckFramebufferStatus)(GLenum target);
    void (*Clear)(GLbitfield mask);
    void (*ClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
    void (*ClearDepth)(GLdouble depth);
    void (*ClearDepthf)(GLfloat d);
    void (*ColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void (*CompileShader)(GLuint shader);
    GLuint (*CreateProgram)(void);
    GLuint (*CreateShader)(GLenum type);
    void (*CullFace)(GLenum mode);
    void (*DeleteBuffers)(GLsizei n, const GLuint * buffers);
    void (*DeleteFramebuffers)(GLsizei n, const GLuint * framebuffers);
    void (*DeleteProgram)(GLuint program);
    void (*DeleteRenderbuffers)(GLsizei n, const GLuint * renderbuffers);
    void (*DeleteShader)(GLuint shader);
    void (*DeleteTextures)(GLsizei n, const GLuint * textures);
    void (*DeleteVertexArrays)(GLsizei n, const GLuint * arrays);
    void (*DepthFunc)(GLenum func);
    void (*DepthMask)(GLboolean flag);
    void (*DepthRange)(GLdouble n, GLdouble f);
    void (*DepthRangef)(GLfloat n, GLfloat f);
    void (*Disable)(GLenum cap);
    void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
    void (*DrawBuffers)(GLsizei n, const GLenum * bufs);
    void (*Enable)(GLenum cap);
    void (*EnableVertexAttribArray)(GLuint index);
    void (*FramebufferRenderbuffer)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void (*FramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void (*FrontFace)(GLenum mode);
    void (*GenBuffers)(GLsizei n, GLuint * buffers);
    void (*GenFramebuffers)(GLsizei n, GLuint * framebuffers);
    void (*GenRenderbuffers)(GLsizei n, GLuint * renderbuffers);
    void (*GenTextures)(GLsizei n, GLuint * textures);
    void (*GenVertexArrays)(GLsizei n, GLuint * arrays);
    void (*GetActiveUniform)(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name);
    void (*GetBooleanv)(GLenum pname, GLboolean * data);
    void (*GetBufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, void * data);
    GLenum (*GetError)(void);
    void (*GetFloatv)(GLenum pname, GLfloat * data);
    void (*GetIntegerv)(GLenum pname, GLint * data);
    void (*GetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void (*GetProgramiv)(GLuint program, GLenum pname, GLint * params);
    void (*GetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
    void (*GetShaderiv)(GLuint shader, GLenum pname, GLint * params);
    const GLubyte * (*GetString)(GLenum name);
    GLint (*GetUniformLocation)(GLuint program, const GLchar * name);
    GLboolean (*IsBuffer)(GLuint buffer);
    GLboolean (*IsEnabled)(GLenum cap);
    GLboolean (*IsProgram)(GLuint program);
    GLboolean (*IsVertexArray)(GLuint array);
    void (*LinkProgram)(GLuint program);
    void (*PixelStorei)(GLenum pname, GLint param);
    void (*PolygonOffset)(GLfloat factor, GLfloat units);
    void (*ReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);
    void (*RenderbufferStorage)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void (*Scissor)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*ShaderSource)(GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length);
    void (*TexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels);
    void (*TexParameterf)(GLenum target, GLenum pname, GLfloat param);
    void (*TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*TexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels);
    void (*Uniform1f)(GLint location, GLfloat v0);
    void (*Uniform1fv)(GLint location, GLsizei count, const GLfloat * value);
    void (*Uniform1i)(GLint location, GLint v0);
    void (*Uniform2f)(GLint location, GLfloat v0, GLfloat v1);
    void (*Uniform2fv)(GLint location, GLsizei count, const GLfloat * value);
    void (*Uniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void (*Uniform3fv)(GLint location, GLsizei count, const GLfloat * value);
    void (*UniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void (*UniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
    void (*UseProgram)(GLuint program);
    void (*VertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer);
    void (*Viewport)(GLint x, GLint y, GLsizei width, GLsizei height);
    void (*BindFragDataLocation)(GLuint program, GLuint color, const GLchar * name);
    void (*ClearBufferuiv)(GLenum buffer, GLint drawbuffer, const GLuint * value);
} HikaruGraphicsDispatch;
void hikaru_glBindFragDataLocation(GLuint program, GLuint color, const GLchar * name);
void hikaru_glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint * value);
const HikaruGraphicsDispatch *hikaru_graphics_set_dispatch(const HikaruGraphicsDispatch *dispatch);
void hikaru_glActiveTexture(GLenum texture);
void hikaru_glAttachShader(GLuint program, GLuint shader);
void hikaru_glBindAttribLocation(GLuint program, GLuint index, const GLchar * name);
void hikaru_glBindBuffer(GLenum target, GLuint buffer);
void hikaru_glBindFramebuffer(GLenum target, GLuint framebuffer);
void hikaru_glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void hikaru_glBindTexture(GLenum target, GLuint texture);
void hikaru_glBindVertexArray(GLuint array);
void hikaru_glBlendEquation(GLenum mode);
void hikaru_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
void hikaru_glBlendFunc(GLenum sfactor, GLenum dfactor);
void hikaru_glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
void hikaru_glBufferData(GLenum target, GLsizeiptr size, const void * data, GLenum usage);
void hikaru_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void * data);
GLenum hikaru_glCheckFramebufferStatus(GLenum target);
void hikaru_glClear(GLbitfield mask);
void hikaru_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void hikaru_glClearDepth(GLdouble depth);
void hikaru_glClearDepthf(GLfloat d);
void hikaru_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void hikaru_glCompileShader(GLuint shader);
GLuint hikaru_glCreateProgram(void);
GLuint hikaru_glCreateShader(GLenum type);
void hikaru_glCullFace(GLenum mode);
void hikaru_glDeleteBuffers(GLsizei n, const GLuint * buffers);
void hikaru_glDeleteFramebuffers(GLsizei n, const GLuint * framebuffers);
void hikaru_glDeleteProgram(GLuint program);
void hikaru_glDeleteRenderbuffers(GLsizei n, const GLuint * renderbuffers);
void hikaru_glDeleteShader(GLuint shader);
void hikaru_glDeleteTextures(GLsizei n, const GLuint * textures);
void hikaru_glDeleteVertexArrays(GLsizei n, const GLuint * arrays);
void hikaru_glDepthFunc(GLenum func);
void hikaru_glDepthMask(GLboolean flag);
void hikaru_glDepthRange(GLdouble n, GLdouble f);
void hikaru_glDepthRangef(GLfloat n, GLfloat f);
void hikaru_glDisable(GLenum cap);
void hikaru_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void hikaru_glDrawBuffers(GLsizei n, const GLenum * bufs);
void hikaru_glEnable(GLenum cap);
void hikaru_glEnableVertexAttribArray(GLuint index);
void hikaru_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
void hikaru_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void hikaru_glFrontFace(GLenum mode);
void hikaru_glGenBuffers(GLsizei n, GLuint * buffers);
void hikaru_glGenFramebuffers(GLsizei n, GLuint * framebuffers);
void hikaru_glGenRenderbuffers(GLsizei n, GLuint * renderbuffers);
void hikaru_glGenTextures(GLsizei n, GLuint * textures);
void hikaru_glGenVertexArrays(GLsizei n, GLuint * arrays);
void hikaru_glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name);
void hikaru_glGetBooleanv(GLenum pname, GLboolean * data);
void hikaru_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void * data);
GLenum hikaru_glGetError(void);
void hikaru_glGetFloatv(GLenum pname, GLfloat * data);
void hikaru_glGetIntegerv(GLenum pname, GLint * data);
void hikaru_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
void hikaru_glGetProgramiv(GLuint program, GLenum pname, GLint * params);
void hikaru_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog);
void hikaru_glGetShaderiv(GLuint shader, GLenum pname, GLint * params);
const GLubyte * hikaru_glGetString(GLenum name);
GLint hikaru_glGetUniformLocation(GLuint program, const GLchar * name);
GLboolean hikaru_glIsBuffer(GLuint buffer);
GLboolean hikaru_glIsEnabled(GLenum cap);
GLboolean hikaru_glIsProgram(GLuint program);
GLboolean hikaru_glIsVertexArray(GLuint array);
void hikaru_glLinkProgram(GLuint program);
void hikaru_glPixelStorei(GLenum pname, GLint param);
void hikaru_glPolygonOffset(GLfloat factor, GLfloat units);
void hikaru_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels);
void hikaru_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void hikaru_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void hikaru_glShaderSource(GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length);
void hikaru_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels);
void hikaru_glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void hikaru_glTexParameteri(GLenum target, GLenum pname, GLint param);
void hikaru_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels);
void hikaru_glUniform1f(GLint location, GLfloat v0);
void hikaru_glUniform1fv(GLint location, GLsizei count, const GLfloat * value);
void hikaru_glUniform1i(GLint location, GLint v0);
void hikaru_glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void hikaru_glUniform2fv(GLint location, GLsizei count, const GLfloat * value);
void hikaru_glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void hikaru_glUniform3fv(GLint location, GLsizei count, const GLfloat * value);
void hikaru_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
void hikaru_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value);
void hikaru_glUseProgram(GLuint program);
void hikaru_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer);
void hikaru_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
#ifdef __cplusplus
}
#endif
#ifndef HIKARU_GRAPHICS_DISPATCH_IMPLEMENTATION
#undef glBindFragDataLocation
#define glBindFragDataLocation hikaru_glBindFragDataLocation
#undef glClearBufferuiv
#define glClearBufferuiv hikaru_glClearBufferuiv
#undef glActiveTexture
#define glActiveTexture hikaru_glActiveTexture
#undef glAttachShader
#define glAttachShader hikaru_glAttachShader
#undef glBindAttribLocation
#define glBindAttribLocation hikaru_glBindAttribLocation
#undef glBindBuffer
#define glBindBuffer hikaru_glBindBuffer
#undef glBindFramebuffer
#define glBindFramebuffer hikaru_glBindFramebuffer
#undef glBindRenderbuffer
#define glBindRenderbuffer hikaru_glBindRenderbuffer
#undef glBindTexture
#define glBindTexture hikaru_glBindTexture
#undef glBindVertexArray
#define glBindVertexArray hikaru_glBindVertexArray
#undef glBlendEquation
#define glBlendEquation hikaru_glBlendEquation
#undef glBlendEquationSeparate
#define glBlendEquationSeparate hikaru_glBlendEquationSeparate
#undef glBlendFunc
#define glBlendFunc hikaru_glBlendFunc
#undef glBlendFuncSeparate
#define glBlendFuncSeparate hikaru_glBlendFuncSeparate
#undef glBufferData
#define glBufferData hikaru_glBufferData
#undef glBufferSubData
#define glBufferSubData hikaru_glBufferSubData
#undef glCheckFramebufferStatus
#define glCheckFramebufferStatus hikaru_glCheckFramebufferStatus
#undef glClear
#define glClear hikaru_glClear
#undef glClearColor
#define glClearColor hikaru_glClearColor
#undef glClearDepth
#define glClearDepth hikaru_glClearDepth
#undef glClearDepthf
#define glClearDepthf hikaru_glClearDepthf
#undef glColorMask
#define glColorMask hikaru_glColorMask
#undef glCompileShader
#define glCompileShader hikaru_glCompileShader
#undef glCreateProgram
#define glCreateProgram hikaru_glCreateProgram
#undef glCreateShader
#define glCreateShader hikaru_glCreateShader
#undef glCullFace
#define glCullFace hikaru_glCullFace
#undef glDeleteBuffers
#define glDeleteBuffers hikaru_glDeleteBuffers
#undef glDeleteFramebuffers
#define glDeleteFramebuffers hikaru_glDeleteFramebuffers
#undef glDeleteProgram
#define glDeleteProgram hikaru_glDeleteProgram
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers hikaru_glDeleteRenderbuffers
#undef glDeleteShader
#define glDeleteShader hikaru_glDeleteShader
#undef glDeleteTextures
#define glDeleteTextures hikaru_glDeleteTextures
#undef glDeleteVertexArrays
#define glDeleteVertexArrays hikaru_glDeleteVertexArrays
#undef glDepthFunc
#define glDepthFunc hikaru_glDepthFunc
#undef glDepthMask
#define glDepthMask hikaru_glDepthMask
#undef glDepthRange
#define glDepthRange hikaru_glDepthRange
#undef glDepthRangef
#define glDepthRangef hikaru_glDepthRangef
#undef glDisable
#define glDisable hikaru_glDisable
#undef glDrawArrays
#define glDrawArrays hikaru_glDrawArrays
#undef glDrawBuffers
#define glDrawBuffers hikaru_glDrawBuffers
#undef glEnable
#define glEnable hikaru_glEnable
#undef glEnableVertexAttribArray
#define glEnableVertexAttribArray hikaru_glEnableVertexAttribArray
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer hikaru_glFramebufferRenderbuffer
#undef glFramebufferTexture2D
#define glFramebufferTexture2D hikaru_glFramebufferTexture2D
#undef glFrontFace
#define glFrontFace hikaru_glFrontFace
#undef glGenBuffers
#define glGenBuffers hikaru_glGenBuffers
#undef glGenFramebuffers
#define glGenFramebuffers hikaru_glGenFramebuffers
#undef glGenRenderbuffers
#define glGenRenderbuffers hikaru_glGenRenderbuffers
#undef glGenTextures
#define glGenTextures hikaru_glGenTextures
#undef glGenVertexArrays
#define glGenVertexArrays hikaru_glGenVertexArrays
#undef glGetActiveUniform
#define glGetActiveUniform hikaru_glGetActiveUniform
#undef glGetBooleanv
#define glGetBooleanv hikaru_glGetBooleanv
#undef glGetBufferSubData
#define glGetBufferSubData hikaru_glGetBufferSubData
#undef glGetError
#define glGetError hikaru_glGetError
#undef glGetFloatv
#define glGetFloatv hikaru_glGetFloatv
#undef glGetIntegerv
#define glGetIntegerv hikaru_glGetIntegerv
#undef glGetProgramInfoLog
#define glGetProgramInfoLog hikaru_glGetProgramInfoLog
#undef glGetProgramiv
#define glGetProgramiv hikaru_glGetProgramiv
#undef glGetShaderInfoLog
#define glGetShaderInfoLog hikaru_glGetShaderInfoLog
#undef glGetShaderiv
#define glGetShaderiv hikaru_glGetShaderiv
#undef glGetString
#define glGetString hikaru_glGetString
#undef glGetUniformLocation
#define glGetUniformLocation hikaru_glGetUniformLocation
#undef glIsBuffer
#define glIsBuffer hikaru_glIsBuffer
#undef glIsEnabled
#define glIsEnabled hikaru_glIsEnabled
#undef glIsProgram
#define glIsProgram hikaru_glIsProgram
#undef glIsVertexArray
#define glIsVertexArray hikaru_glIsVertexArray
#undef glLinkProgram
#define glLinkProgram hikaru_glLinkProgram
#undef glPixelStorei
#define glPixelStorei hikaru_glPixelStorei
#undef glPolygonOffset
#define glPolygonOffset hikaru_glPolygonOffset
#undef glReadPixels
#define glReadPixels hikaru_glReadPixels
#undef glRenderbufferStorage
#define glRenderbufferStorage hikaru_glRenderbufferStorage
#undef glScissor
#define glScissor hikaru_glScissor
#undef glShaderSource
#define glShaderSource hikaru_glShaderSource
#undef glTexImage2D
#define glTexImage2D hikaru_glTexImage2D
#undef glTexParameterf
#define glTexParameterf hikaru_glTexParameterf
#undef glTexParameteri
#define glTexParameteri hikaru_glTexParameteri
#undef glTexSubImage2D
#define glTexSubImage2D hikaru_glTexSubImage2D
#undef glUniform1f
#define glUniform1f hikaru_glUniform1f
#undef glUniform1fv
#define glUniform1fv hikaru_glUniform1fv
#undef glUniform1i
#define glUniform1i hikaru_glUniform1i
#undef glUniform2f
#define glUniform2f hikaru_glUniform2f
#undef glUniform2fv
#define glUniform2fv hikaru_glUniform2fv
#undef glUniform3f
#define glUniform3f hikaru_glUniform3f
#undef glUniform3fv
#define glUniform3fv hikaru_glUniform3fv
#undef glUniformMatrix3fv
#define glUniformMatrix3fv hikaru_glUniformMatrix3fv
#undef glUniformMatrix4fv
#define glUniformMatrix4fv hikaru_glUniformMatrix4fv
#undef glUseProgram
#define glUseProgram hikaru_glUseProgram
#undef glVertexAttribPointer
#define glVertexAttribPointer hikaru_glVertexAttribPointer
#undef glViewport
#define glViewport hikaru_glViewport
#endif
