// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// GL types are supplied by vk/core.h before this internal header.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HikaruGraphicsDispatch {
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
} HikaruGraphicsDispatch;
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
#define glActiveTexture hikaru_glActiveTexture
#define glAttachShader hikaru_glAttachShader
#define glBindAttribLocation hikaru_glBindAttribLocation
#define glBindBuffer hikaru_glBindBuffer
#define glBindFramebuffer hikaru_glBindFramebuffer
#define glBindRenderbuffer hikaru_glBindRenderbuffer
#define glBindTexture hikaru_glBindTexture
#define glBindVertexArray hikaru_glBindVertexArray
#define glBlendEquation hikaru_glBlendEquation
#define glBlendEquationSeparate hikaru_glBlendEquationSeparate
#define glBlendFunc hikaru_glBlendFunc
#define glBlendFuncSeparate hikaru_glBlendFuncSeparate
#define glBufferData hikaru_glBufferData
#define glBufferSubData hikaru_glBufferSubData
#define glCheckFramebufferStatus hikaru_glCheckFramebufferStatus
#define glClear hikaru_glClear
#define glClearColor hikaru_glClearColor
#define glClearDepth hikaru_glClearDepth
#define glClearDepthf hikaru_glClearDepthf
#define glColorMask hikaru_glColorMask
#define glCompileShader hikaru_glCompileShader
#define glCreateProgram hikaru_glCreateProgram
#define glCreateShader hikaru_glCreateShader
#define glCullFace hikaru_glCullFace
#define glDeleteBuffers hikaru_glDeleteBuffers
#define glDeleteFramebuffers hikaru_glDeleteFramebuffers
#define glDeleteProgram hikaru_glDeleteProgram
#define glDeleteRenderbuffers hikaru_glDeleteRenderbuffers
#define glDeleteShader hikaru_glDeleteShader
#define glDeleteTextures hikaru_glDeleteTextures
#define glDeleteVertexArrays hikaru_glDeleteVertexArrays
#define glDepthFunc hikaru_glDepthFunc
#define glDepthMask hikaru_glDepthMask
#define glDepthRange hikaru_glDepthRange
#define glDepthRangef hikaru_glDepthRangef
#define glDisable hikaru_glDisable
#define glDrawArrays hikaru_glDrawArrays
#define glDrawBuffers hikaru_glDrawBuffers
#define glEnable hikaru_glEnable
#define glEnableVertexAttribArray hikaru_glEnableVertexAttribArray
#define glFramebufferRenderbuffer hikaru_glFramebufferRenderbuffer
#define glFramebufferTexture2D hikaru_glFramebufferTexture2D
#define glFrontFace hikaru_glFrontFace
#define glGenBuffers hikaru_glGenBuffers
#define glGenFramebuffers hikaru_glGenFramebuffers
#define glGenRenderbuffers hikaru_glGenRenderbuffers
#define glGenTextures hikaru_glGenTextures
#define glGenVertexArrays hikaru_glGenVertexArrays
#define glGetActiveUniform hikaru_glGetActiveUniform
#define glGetBooleanv hikaru_glGetBooleanv
#define glGetBufferSubData hikaru_glGetBufferSubData
#define glGetError hikaru_glGetError
#define glGetFloatv hikaru_glGetFloatv
#define glGetIntegerv hikaru_glGetIntegerv
#define glGetProgramInfoLog hikaru_glGetProgramInfoLog
#define glGetProgramiv hikaru_glGetProgramiv
#define glGetShaderInfoLog hikaru_glGetShaderInfoLog
#define glGetShaderiv hikaru_glGetShaderiv
#define glGetString hikaru_glGetString
#define glGetUniformLocation hikaru_glGetUniformLocation
#define glIsBuffer hikaru_glIsBuffer
#define glIsEnabled hikaru_glIsEnabled
#define glIsProgram hikaru_glIsProgram
#define glIsVertexArray hikaru_glIsVertexArray
#define glLinkProgram hikaru_glLinkProgram
#define glPixelStorei hikaru_glPixelStorei
#define glPolygonOffset hikaru_glPolygonOffset
#define glReadPixels hikaru_glReadPixels
#define glRenderbufferStorage hikaru_glRenderbufferStorage
#define glScissor hikaru_glScissor
#define glShaderSource hikaru_glShaderSource
#define glTexImage2D hikaru_glTexImage2D
#define glTexParameterf hikaru_glTexParameterf
#define glTexParameteri hikaru_glTexParameteri
#define glTexSubImage2D hikaru_glTexSubImage2D
#define glUniform1f hikaru_glUniform1f
#define glUniform1fv hikaru_glUniform1fv
#define glUniform1i hikaru_glUniform1i
#define glUniform2f hikaru_glUniform2f
#define glUniform2fv hikaru_glUniform2fv
#define glUniform3f hikaru_glUniform3f
#define glUniform3fv hikaru_glUniform3fv
#define glUniformMatrix3fv hikaru_glUniformMatrix3fv
#define glUniformMatrix4fv hikaru_glUniformMatrix4fv
#define glUseProgram hikaru_glUseProgram
#define glVertexAttribPointer hikaru_glVertexAttribPointer
#define glViewport hikaru_glViewport
#endif
