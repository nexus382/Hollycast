// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk/core.h"
#define HIKARU_GRAPHICS_DISPATCH_IMPLEMENTATION
#include "graphics_dispatch.h"
static _Thread_local const HikaruGraphicsDispatch *current_dispatch;
const HikaruGraphicsDispatch *hikaru_graphics_set_dispatch(const HikaruGraphicsDispatch *dispatch) {
    const HikaruGraphicsDispatch *previous = current_dispatch; current_dispatch = dispatch; return previous;
}
void hikaru_glActiveTexture(GLenum texture) {
    if (current_dispatch) { current_dispatch->ActiveTexture(texture); return; }
    glActiveTexture(texture);
}
void hikaru_glAttachShader(GLuint program, GLuint shader) {
    if (current_dispatch) { current_dispatch->AttachShader(program, shader); return; }
    glAttachShader(program, shader);
}
void hikaru_glBindAttribLocation(GLuint program, GLuint index, const GLchar * name) {
    if (current_dispatch) { current_dispatch->BindAttribLocation(program, index, name); return; }
    glBindAttribLocation(program, index, name);
}
void hikaru_glBindBuffer(GLenum target, GLuint buffer) {
    if (current_dispatch) { current_dispatch->BindBuffer(target, buffer); return; }
    glBindBuffer(target, buffer);
}
void hikaru_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (current_dispatch) { current_dispatch->BindFramebuffer(target, framebuffer); return; }
    glBindFramebuffer(target, framebuffer);
}
void hikaru_glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    if (current_dispatch) { current_dispatch->BindRenderbuffer(target, renderbuffer); return; }
    glBindRenderbuffer(target, renderbuffer);
}
void hikaru_glBindTexture(GLenum target, GLuint texture) {
    if (current_dispatch) { current_dispatch->BindTexture(target, texture); return; }
    glBindTexture(target, texture);
}
void hikaru_glBindVertexArray(GLuint array) {
    if (current_dispatch) { current_dispatch->BindVertexArray(array); return; }
    glBindVertexArray(array);
}
void hikaru_glBlendEquation(GLenum mode) {
    if (current_dispatch) { current_dispatch->BlendEquation(mode); return; }
    glBlendEquation(mode);
}
void hikaru_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    if (current_dispatch) { current_dispatch->BlendEquationSeparate(modeRGB, modeAlpha); return; }
    glBlendEquationSeparate(modeRGB, modeAlpha);
}
void hikaru_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    if (current_dispatch) { current_dispatch->BlendFunc(sfactor, dfactor); return; }
    glBlendFunc(sfactor, dfactor);
}
void hikaru_glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    if (current_dispatch) { current_dispatch->BlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha); return; }
    glBlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha);
}
void hikaru_glBufferData(GLenum target, GLsizeiptr size, const void * data, GLenum usage) {
    if (current_dispatch) { current_dispatch->BufferData(target, size, data, usage); return; }
    glBufferData(target, size, data, usage);
}
void hikaru_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void * data) {
    if (current_dispatch) { current_dispatch->BufferSubData(target, offset, size, data); return; }
    glBufferSubData(target, offset, size, data);
}
GLenum hikaru_glCheckFramebufferStatus(GLenum target) {
    if (current_dispatch) { return current_dispatch->CheckFramebufferStatus(target); }
    return glCheckFramebufferStatus(target);
}
void hikaru_glClear(GLbitfield mask) {
    if (current_dispatch) { current_dispatch->Clear(mask); return; }
    glClear(mask);
}
void hikaru_glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    if (current_dispatch) { current_dispatch->ClearColor(red, green, blue, alpha); return; }
    glClearColor(red, green, blue, alpha);
}
void hikaru_glClearDepth(GLdouble depth) {
    if (current_dispatch) { current_dispatch->ClearDepth(depth); return; }
    glClearDepth(depth);
}
void hikaru_glClearDepthf(GLfloat d) {
    if (current_dispatch) { current_dispatch->ClearDepthf(d); return; }
#ifdef __APPLE__
    glClearDepth(d);
#else
    glClearDepthf(d);
#endif
}
void hikaru_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    if (current_dispatch) { current_dispatch->ColorMask(red, green, blue, alpha); return; }
    glColorMask(red, green, blue, alpha);
}
void hikaru_glCompileShader(GLuint shader) {
    if (current_dispatch) { current_dispatch->CompileShader(shader); return; }
    glCompileShader(shader);
}
GLuint hikaru_glCreateProgram(void) {
    if (current_dispatch) { return current_dispatch->CreateProgram(); }
    return glCreateProgram();
}
GLuint hikaru_glCreateShader(GLenum type) {
    if (current_dispatch) { return current_dispatch->CreateShader(type); }
    return glCreateShader(type);
}
void hikaru_glCullFace(GLenum mode) {
    if (current_dispatch) { current_dispatch->CullFace(mode); return; }
    glCullFace(mode);
}
void hikaru_glDeleteBuffers(GLsizei n, const GLuint * buffers) {
    if (current_dispatch) { current_dispatch->DeleteBuffers(n, buffers); return; }
    glDeleteBuffers(n, buffers);
}
void hikaru_glDeleteFramebuffers(GLsizei n, const GLuint * framebuffers) {
    if (current_dispatch) { current_dispatch->DeleteFramebuffers(n, framebuffers); return; }
    glDeleteFramebuffers(n, framebuffers);
}
void hikaru_glDeleteProgram(GLuint program) {
    if (current_dispatch) { current_dispatch->DeleteProgram(program); return; }
    glDeleteProgram(program);
}
void hikaru_glDeleteRenderbuffers(GLsizei n, const GLuint * renderbuffers) {
    if (current_dispatch) { current_dispatch->DeleteRenderbuffers(n, renderbuffers); return; }
    glDeleteRenderbuffers(n, renderbuffers);
}
void hikaru_glDeleteShader(GLuint shader) {
    if (current_dispatch) { current_dispatch->DeleteShader(shader); return; }
    glDeleteShader(shader);
}
void hikaru_glDeleteTextures(GLsizei n, const GLuint * textures) {
    if (current_dispatch) { current_dispatch->DeleteTextures(n, textures); return; }
    glDeleteTextures(n, textures);
}
void hikaru_glDeleteVertexArrays(GLsizei n, const GLuint * arrays) {
    if (current_dispatch) { current_dispatch->DeleteVertexArrays(n, arrays); return; }
    glDeleteVertexArrays(n, arrays);
}
void hikaru_glDepthFunc(GLenum func) {
    if (current_dispatch) { current_dispatch->DepthFunc(func); return; }
    glDepthFunc(func);
}
void hikaru_glDepthMask(GLboolean flag) {
    if (current_dispatch) { current_dispatch->DepthMask(flag); return; }
    glDepthMask(flag);
}
void hikaru_glDepthRange(GLdouble n, GLdouble f) {
    if (current_dispatch) { current_dispatch->DepthRange(n, f); return; }
    glDepthRange(n, f);
}
void hikaru_glDepthRangef(GLfloat n, GLfloat f) {
    if (current_dispatch) { current_dispatch->DepthRangef(n, f); return; }
#ifdef __APPLE__
    glDepthRange(n, f);
#else
    glDepthRangef(n, f);
#endif
}
void hikaru_glDisable(GLenum cap) {
    if (current_dispatch) { current_dispatch->Disable(cap); return; }
    glDisable(cap);
}
void hikaru_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (current_dispatch) { current_dispatch->DrawArrays(mode, first, count); return; }
    glDrawArrays(mode, first, count);
}
void hikaru_glDrawBuffers(GLsizei n, const GLenum * bufs) {
    if (current_dispatch) { current_dispatch->DrawBuffers(n, bufs); return; }
    glDrawBuffers(n, bufs);
}
void hikaru_glEnable(GLenum cap) {
    if (current_dispatch) { current_dispatch->Enable(cap); return; }
    glEnable(cap);
}
void hikaru_glEnableVertexAttribArray(GLuint index) {
    if (current_dispatch) { current_dispatch->EnableVertexAttribArray(index); return; }
    glEnableVertexAttribArray(index);
}
void hikaru_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    if (current_dispatch) { current_dispatch->FramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); return; }
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}
void hikaru_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    if (current_dispatch) { current_dispatch->FramebufferTexture2D(target, attachment, textarget, texture, level); return; }
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
void hikaru_glFrontFace(GLenum mode) {
    if (current_dispatch) { current_dispatch->FrontFace(mode); return; }
    glFrontFace(mode);
}
void hikaru_glGenBuffers(GLsizei n, GLuint * buffers) {
    if (current_dispatch) { current_dispatch->GenBuffers(n, buffers); return; }
    glGenBuffers(n, buffers);
}
void hikaru_glGenFramebuffers(GLsizei n, GLuint * framebuffers) {
    if (current_dispatch) { current_dispatch->GenFramebuffers(n, framebuffers); return; }
    glGenFramebuffers(n, framebuffers);
}
void hikaru_glGenRenderbuffers(GLsizei n, GLuint * renderbuffers) {
    if (current_dispatch) { current_dispatch->GenRenderbuffers(n, renderbuffers); return; }
    glGenRenderbuffers(n, renderbuffers);
}
void hikaru_glGenTextures(GLsizei n, GLuint * textures) {
    if (current_dispatch) { current_dispatch->GenTextures(n, textures); return; }
    glGenTextures(n, textures);
}
void hikaru_glGenVertexArrays(GLsizei n, GLuint * arrays) {
    if (current_dispatch) { current_dispatch->GenVertexArrays(n, arrays); return; }
    glGenVertexArrays(n, arrays);
}
void hikaru_glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name) {
    if (current_dispatch) { current_dispatch->GetActiveUniform(program, index, bufSize, length, size, type, name); return; }
    glGetActiveUniform(program, index, bufSize, length, size, type, name);
}
void hikaru_glGetBooleanv(GLenum pname, GLboolean * data) {
    if (current_dispatch) { current_dispatch->GetBooleanv(pname, data); return; }
    glGetBooleanv(pname, data);
}
void hikaru_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void * data) {
    if (current_dispatch) { current_dispatch->GetBufferSubData(target, offset, size, data); return; }
    glGetBufferSubData(target, offset, size, data);
}
GLenum hikaru_glGetError(void) {
    if (current_dispatch) { return current_dispatch->GetError(); }
    return glGetError();
}
void hikaru_glGetFloatv(GLenum pname, GLfloat * data) {
    if (current_dispatch) { current_dispatch->GetFloatv(pname, data); return; }
    glGetFloatv(pname, data);
}
void hikaru_glGetIntegerv(GLenum pname, GLint * data) {
    if (current_dispatch) { current_dispatch->GetIntegerv(pname, data); return; }
    glGetIntegerv(pname, data);
}
void hikaru_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog) {
    if (current_dispatch) { current_dispatch->GetProgramInfoLog(program, bufSize, length, infoLog); return; }
    glGetProgramInfoLog(program, bufSize, length, infoLog);
}
void hikaru_glGetProgramiv(GLuint program, GLenum pname, GLint * params) {
    if (current_dispatch) { current_dispatch->GetProgramiv(program, pname, params); return; }
    glGetProgramiv(program, pname, params);
}
void hikaru_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog) {
    if (current_dispatch) { current_dispatch->GetShaderInfoLog(shader, bufSize, length, infoLog); return; }
    glGetShaderInfoLog(shader, bufSize, length, infoLog);
}
void hikaru_glGetShaderiv(GLuint shader, GLenum pname, GLint * params) {
    if (current_dispatch) { current_dispatch->GetShaderiv(shader, pname, params); return; }
    glGetShaderiv(shader, pname, params);
}
const GLubyte * hikaru_glGetString(GLenum name) {
    if (current_dispatch) { return current_dispatch->GetString(name); }
    return glGetString(name);
}
GLint hikaru_glGetUniformLocation(GLuint program, const GLchar * name) {
    if (current_dispatch) { return current_dispatch->GetUniformLocation(program, name); }
    return glGetUniformLocation(program, name);
}
GLboolean hikaru_glIsBuffer(GLuint buffer) {
    if (current_dispatch) { return current_dispatch->IsBuffer(buffer); }
    return glIsBuffer(buffer);
}
GLboolean hikaru_glIsEnabled(GLenum cap) {
    if (current_dispatch) { return current_dispatch->IsEnabled(cap); }
    return glIsEnabled(cap);
}
GLboolean hikaru_glIsProgram(GLuint program) {
    if (current_dispatch) { return current_dispatch->IsProgram(program); }
    return glIsProgram(program);
}
GLboolean hikaru_glIsVertexArray(GLuint array) {
    if (current_dispatch) { return current_dispatch->IsVertexArray(array); }
    return glIsVertexArray(array);
}
void hikaru_glLinkProgram(GLuint program) {
    if (current_dispatch) { current_dispatch->LinkProgram(program); return; }
    glLinkProgram(program);
}
void hikaru_glPixelStorei(GLenum pname, GLint param) {
    if (current_dispatch) { current_dispatch->PixelStorei(pname, param); return; }
    glPixelStorei(pname, param);
}
void hikaru_glPolygonOffset(GLfloat factor, GLfloat units) {
    if (current_dispatch) { current_dispatch->PolygonOffset(factor, units); return; }
    glPolygonOffset(factor, units);
}
void hikaru_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels) {
    if (current_dispatch) { current_dispatch->ReadPixels(x, y, width, height, format, type, pixels); return; }
    glReadPixels(x, y, width, height, format, type, pixels);
}
void hikaru_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    if (current_dispatch) { current_dispatch->RenderbufferStorage(target, internalformat, width, height); return; }
    glRenderbufferStorage(target, internalformat, width, height);
}
void hikaru_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (current_dispatch) { current_dispatch->Scissor(x, y, width, height); return; }
    glScissor(x, y, width, height);
}
void hikaru_glShaderSource(GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length) {
    if (current_dispatch) { current_dispatch->ShaderSource(shader, count, string, length); return; }
    glShaderSource(shader, count, string, length);
}
void hikaru_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels) {
    if (current_dispatch) { current_dispatch->TexImage2D(target, level, internalformat, width, height, border, format, type, pixels); return; }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
void hikaru_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    if (current_dispatch) { current_dispatch->TexParameterf(target, pname, param); return; }
    glTexParameterf(target, pname, param);
}
void hikaru_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (current_dispatch) { current_dispatch->TexParameteri(target, pname, param); return; }
    glTexParameteri(target, pname, param);
}
void hikaru_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels) {
    if (current_dispatch) { current_dispatch->TexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); return; }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}
void hikaru_glUniform1f(GLint location, GLfloat v0) {
    if (current_dispatch) { current_dispatch->Uniform1f(location, v0); return; }
    glUniform1f(location, v0);
}
void hikaru_glUniform1fv(GLint location, GLsizei count, const GLfloat * value) {
    if (current_dispatch) { current_dispatch->Uniform1fv(location, count, value); return; }
    glUniform1fv(location, count, value);
}
void hikaru_glUniform1i(GLint location, GLint v0) {
    if (current_dispatch) { current_dispatch->Uniform1i(location, v0); return; }
    glUniform1i(location, v0);
}
void hikaru_glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    if (current_dispatch) { current_dispatch->Uniform2f(location, v0, v1); return; }
    glUniform2f(location, v0, v1);
}
void hikaru_glUniform2fv(GLint location, GLsizei count, const GLfloat * value) {
    if (current_dispatch) { current_dispatch->Uniform2fv(location, count, value); return; }
    glUniform2fv(location, count, value);
}
void hikaru_glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    if (current_dispatch) { current_dispatch->Uniform3f(location, v0, v1, v2); return; }
    glUniform3f(location, v0, v1, v2);
}
void hikaru_glUniform3fv(GLint location, GLsizei count, const GLfloat * value) {
    if (current_dispatch) { current_dispatch->Uniform3fv(location, count, value); return; }
    glUniform3fv(location, count, value);
}
void hikaru_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value) {
    if (current_dispatch) { current_dispatch->UniformMatrix3fv(location, count, transpose, value); return; }
    glUniformMatrix3fv(location, count, transpose, value);
}
void hikaru_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat * value) {
    if (current_dispatch) { current_dispatch->UniformMatrix4fv(location, count, transpose, value); return; }
    glUniformMatrix4fv(location, count, transpose, value);
}
void hikaru_glUseProgram(GLuint program) {
    if (current_dispatch) { current_dispatch->UseProgram(program); return; }
    glUseProgram(program);
}
void hikaru_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer) {
    if (current_dispatch) { current_dispatch->VertexAttribPointer(index, size, type, normalized, stride, pointer); return; }
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
void hikaru_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (current_dispatch) { current_dispatch->Viewport(x, y, width, height); return; }
    glViewport(x, y, width, height);
}

void hikaru_graphics_begin_occlusion(GLuint query[2]) {
    if (current_dispatch) { current_dispatch->BeginOcclusion(); return; }
    if (!query[0]) {
        glGenQueries(1, &query[0]);
        const char *version = (const char *)glGetString(GL_VERSION);
        query[1] = version && !strncmp(version, "OpenGL ES", 9) ? GL_ANY_SAMPLES_PASSED : GL_SAMPLES_PASSED;
    }
    glBeginQuery(query[1], query[0]);
}
GLboolean hikaru_graphics_end_occlusion(const GLuint query[2]) {
    if (current_dispatch) return current_dispatch->EndOcclusion();
    GLuint samples = 0;
    glEndQuery(query[1]);
    glGetQueryObjectuiv(query[0], GL_QUERY_RESULT, &samples);
    return samples != 0;
}
void hikaru_graphics_delete_occlusion(GLuint query[2]) {
    if (!current_dispatch && query[0]) glDeleteQueries(1, &query[0]);
    query[0] = query[1] = 0;
}
