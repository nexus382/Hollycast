// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "device.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
namespace hikaru::graphics {
thread_local Device* Device::active;
Device::Device(std::unique_ptr<Backend> backend) : backend(std::move(backend)) {
    if (!this->backend) throw std::runtime_error("Hikaru graphics backend is unavailable");
    dispatch.BindFragDataLocation = [](GLuint program, GLuint color, const GLchar *name) { active->BindFragDataLocation(program, color, name); };
    dispatch.ClearBufferuiv = [](GLenum buffer, GLint drawbuffer, const GLuint *value) { try { active->ClearBufferuiv(buffer, drawbuffer, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BeginOcclusion = [] { try { active->flushDraw(); active->backend->beginOcclusion(); } catch (...) { active->error=std::current_exception(); } };
    dispatch.EndOcclusion = []() -> GLboolean { try { active->flushDraw(); return active->backend->endOcclusion(); } catch (...) { active->error=std::current_exception(); return false; } };
    dispatch.ActiveTexture = [](GLenum texture) -> void { try { active->ActiveTexture(texture); } catch (...) { active->error=std::current_exception(); } };
    dispatch.AttachShader = [](GLuint program, GLuint shader) -> void { try { active->AttachShader(program, shader); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindAttribLocation = [](GLuint program, GLuint index, const GLchar * name) -> void { try { active->BindAttribLocation(program, index, name); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindBuffer = [](GLenum target, GLuint buffer) -> void { try { active->BindBuffer(target, buffer); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindFramebuffer = [](GLenum target, GLuint framebuffer) -> void { try { active->BindFramebuffer(target, framebuffer); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindRenderbuffer = [](GLenum target, GLuint renderbuffer) -> void { try { active->BindRenderbuffer(target, renderbuffer); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindTexture = [](GLenum target, GLuint texture) -> void { try { active->BindTexture(target, texture); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BindVertexArray = [](GLuint array) -> void { try { active->BindVertexArray(array); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BlendEquation = [](GLenum mode) -> void { try { active->BlendEquation(mode); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BlendEquationSeparate = [](GLenum modeRGB, GLenum modeAlpha) -> void { try { active->BlendEquationSeparate(modeRGB, modeAlpha); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BlendFunc = [](GLenum sfactor, GLenum dfactor) -> void { try { active->BlendFunc(sfactor, dfactor); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BlendFuncSeparate = [](GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) -> void { try { active->BlendFuncSeparate(sfactorRGB, dfactorRGB, sfactorAlpha, dfactorAlpha); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BufferData = [](GLenum target, GLsizeiptr size, const void * data, GLenum usage) -> void { try { active->BufferData(target, size, data, usage); } catch (...) { active->error=std::current_exception(); } };
    dispatch.BufferSubData = [](GLenum target, GLintptr offset, GLsizeiptr size, const void * data) -> void { try { active->BufferSubData(target, offset, size, data); } catch (...) { active->error=std::current_exception(); } };
    dispatch.CheckFramebufferStatus = [](GLenum target) -> GLenum { try { return active->CheckFramebufferStatus(target); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.Clear = [](GLbitfield mask) -> void { try { active->Clear(mask); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ClearColor = [](GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) -> void { try { active->ClearColor(red, green, blue, alpha); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ClearDepth = [](GLdouble depth) -> void { try { active->ClearDepth(depth); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ClearDepthf = [](GLfloat d) -> void { try { active->ClearDepthf(d); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ColorMask = [](GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) -> void { try { active->ColorMask(red, green, blue, alpha); } catch (...) { active->error=std::current_exception(); } };
    dispatch.CompileShader = [](GLuint shader) -> void { try { active->CompileShader(shader); } catch (...) { active->error=std::current_exception(); } };
    dispatch.CreateProgram = [](void) -> GLuint { try { return active->CreateProgram(); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.CreateShader = [](GLenum type) -> GLuint { try { return active->CreateShader(type); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.CullFace = [](GLenum mode) -> void { try { active->CullFace(mode); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteBuffers = [](GLsizei n, const GLuint * buffers) -> void { try { active->DeleteBuffers(n, buffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteFramebuffers = [](GLsizei n, const GLuint * framebuffers) -> void { try { active->DeleteFramebuffers(n, framebuffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteProgram = [](GLuint program) -> void { try { active->DeleteProgram(program); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteRenderbuffers = [](GLsizei n, const GLuint * renderbuffers) -> void { try { active->DeleteRenderbuffers(n, renderbuffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteShader = [](GLuint shader) -> void { try { active->DeleteShader(shader); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteTextures = [](GLsizei n, const GLuint * textures) -> void { try { active->DeleteTextures(n, textures); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DeleteVertexArrays = [](GLsizei n, const GLuint * arrays) -> void { try { active->DeleteVertexArrays(n, arrays); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DepthFunc = [](GLenum func) -> void { try { active->DepthFunc(func); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DepthMask = [](GLboolean flag) -> void { try { active->DepthMask(flag); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DepthRange = [](GLdouble n, GLdouble f) -> void { try { active->DepthRange(n, f); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DepthRangef = [](GLfloat n, GLfloat f) -> void { try { active->DepthRangef(n, f); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Disable = [](GLenum cap) -> void { try { active->Disable(cap); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DrawArrays = [](GLenum mode, GLint first, GLsizei count) -> void { try { active->DrawArrays(mode, first, count); } catch (...) { active->error=std::current_exception(); } };
    dispatch.DrawBuffers = [](GLsizei n, const GLenum * bufs) -> void { try { active->DrawBuffers(n, bufs); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Enable = [](GLenum cap) -> void { try { active->Enable(cap); } catch (...) { active->error=std::current_exception(); } };
    dispatch.EnableVertexAttribArray = [](GLuint index) -> void { try { active->EnableVertexAttribArray(index); } catch (...) { active->error=std::current_exception(); } };
    dispatch.FramebufferRenderbuffer = [](GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) -> void { try { active->FramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer); } catch (...) { active->error=std::current_exception(); } };
    dispatch.FramebufferTexture2D = [](GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) -> void { try { active->FramebufferTexture2D(target, attachment, textarget, texture, level); } catch (...) { active->error=std::current_exception(); } };
    dispatch.FrontFace = [](GLenum mode) -> void { try { active->FrontFace(mode); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GenBuffers = [](GLsizei n, GLuint * buffers) -> void { try { active->GenBuffers(n, buffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GenFramebuffers = [](GLsizei n, GLuint * framebuffers) -> void { try { active->GenFramebuffers(n, framebuffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GenRenderbuffers = [](GLsizei n, GLuint * renderbuffers) -> void { try { active->GenRenderbuffers(n, renderbuffers); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GenTextures = [](GLsizei n, GLuint * textures) -> void { try { active->GenTextures(n, textures); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GenVertexArrays = [](GLsizei n, GLuint * arrays) -> void { try { active->GenVertexArrays(n, arrays); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetActiveUniform = [](GLuint program, GLuint index, GLsizei bufSize, GLsizei * length, GLint * size, GLenum * type, GLchar * name) -> void { try { active->GetActiveUniform(program, index, bufSize, length, size, type, name); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetBooleanv = [](GLenum pname, GLboolean * data) -> void { try { active->GetBooleanv(pname, data); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetBufferSubData = [](GLenum target, GLintptr offset, GLsizeiptr size, void * data) -> void { try { active->GetBufferSubData(target, offset, size, data); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetError = [](void) -> GLenum { try { return active->GetError(); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.GetFloatv = [](GLenum pname, GLfloat * data) -> void { try { active->GetFloatv(pname, data); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetIntegerv = [](GLenum pname, GLint * data) -> void { try { active->GetIntegerv(pname, data); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetProgramInfoLog = [](GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog) -> void { try { active->GetProgramInfoLog(program, bufSize, length, infoLog); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetProgramiv = [](GLuint program, GLenum pname, GLint * params) -> void { try { active->GetProgramiv(program, pname, params); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetShaderInfoLog = [](GLuint shader, GLsizei bufSize, GLsizei * length, GLchar * infoLog) -> void { try { active->GetShaderInfoLog(shader, bufSize, length, infoLog); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetShaderiv = [](GLuint shader, GLenum pname, GLint * params) -> void { try { active->GetShaderiv(shader, pname, params); } catch (...) { active->error=std::current_exception(); } };
    dispatch.GetString = [](GLenum name) -> const GLubyte * { try { return active->GetString(name); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.GetUniformLocation = [](GLuint program, const GLchar * name) -> GLint { try { return active->GetUniformLocation(program, name); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.IsBuffer = [](GLuint buffer) -> GLboolean { try { return active->IsBuffer(buffer); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.IsEnabled = [](GLenum cap) -> GLboolean { try { return active->IsEnabled(cap); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.IsProgram = [](GLuint program) -> GLboolean { try { return active->IsProgram(program); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.IsVertexArray = [](GLuint array) -> GLboolean { try { return active->IsVertexArray(array); } catch (...) { active->error=std::current_exception(); return {}; } };
    dispatch.LinkProgram = [](GLuint program) -> void { try { active->LinkProgram(program); } catch (...) { active->error=std::current_exception(); } };
    dispatch.PixelStorei = [](GLenum pname, GLint param) -> void { try { active->PixelStorei(pname, param); } catch (...) { active->error=std::current_exception(); } };
    dispatch.PolygonOffset = [](GLfloat factor, GLfloat units) -> void { try { active->PolygonOffset(factor, units); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ReadPixels = [](GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void * pixels) -> void { try { active->ReadPixels(x, y, width, height, format, type, pixels); } catch (...) { active->error=std::current_exception(); } };
    dispatch.RenderbufferStorage = [](GLenum target, GLenum internalformat, GLsizei width, GLsizei height) -> void { try { active->RenderbufferStorage(target, internalformat, width, height); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Scissor = [](GLint x, GLint y, GLsizei width, GLsizei height) -> void { try { active->Scissor(x, y, width, height); } catch (...) { active->error=std::current_exception(); } };
    dispatch.ShaderSource = [](GLuint shader, GLsizei count, const GLchar *const* string, const GLint * length) -> void { try { active->ShaderSource(shader, count, string, length); } catch (...) { active->error=std::current_exception(); } };
    dispatch.TexImage2D = [](GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void * pixels) -> void { try { active->TexImage2D(target, level, internalformat, width, height, border, format, type, pixels); } catch (...) { active->error=std::current_exception(); } };
    dispatch.TexParameterf = [](GLenum target, GLenum pname, GLfloat param) -> void { try { active->TexParameterf(target, pname, param); } catch (...) { active->error=std::current_exception(); } };
    dispatch.TexParameteri = [](GLenum target, GLenum pname, GLint param) -> void { try { active->TexParameteri(target, pname, param); } catch (...) { active->error=std::current_exception(); } };
    dispatch.TexSubImage2D = [](GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void * pixels) -> void { try { active->TexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform1f = [](GLint location, GLfloat v0) -> void { try { active->Uniform1f(location, v0); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform1fv = [](GLint location, GLsizei count, const GLfloat * value) -> void { try { active->Uniform1fv(location, count, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform1i = [](GLint location, GLint v0) -> void { try { active->Uniform1i(location, v0); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform2f = [](GLint location, GLfloat v0, GLfloat v1) -> void { try { active->Uniform2f(location, v0, v1); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform2fv = [](GLint location, GLsizei count, const GLfloat * value) -> void { try { active->Uniform2fv(location, count, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform3f = [](GLint location, GLfloat v0, GLfloat v1, GLfloat v2) -> void { try { active->Uniform3f(location, v0, v1, v2); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Uniform3fv = [](GLint location, GLsizei count, const GLfloat * value) -> void { try { active->Uniform3fv(location, count, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.UniformMatrix3fv = [](GLint location, GLsizei count, GLboolean transpose, const GLfloat * value) -> void { try { active->UniformMatrix3fv(location, count, transpose, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.UniformMatrix4fv = [](GLint location, GLsizei count, GLboolean transpose, const GLfloat * value) -> void { try { active->UniformMatrix4fv(location, count, transpose, value); } catch (...) { active->error=std::current_exception(); } };
    dispatch.UseProgram = [](GLuint program) -> void { try { active->UseProgram(program); } catch (...) { active->error=std::current_exception(); } };
    dispatch.VertexAttribPointer = [](GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer) -> void { try { active->VertexAttribPointer(index, size, type, normalized, stride, pointer); } catch (...) { active->error=std::current_exception(); } };
    dispatch.Viewport = [](GLint x, GLint y, GLsizei width, GLsizei height) -> void { try { active->Viewport(x, y, width, height); } catch (...) { active->error=std::current_exception(); } };
    pixelStore[GL_UNPACK_ALIGNMENT]=4;
    arrays[0]={}; targets[0]={0,0};
}
Device::~Device() = default;
Device::Scope::Scope(Device* device) : previous(active), previousDispatch(hikaru_graphics_set_dispatch(device ? &device->dispatch : nullptr)) { active=device; }
Device::Scope::~Scope() { active=previous; hikaru_graphics_set_dispatch(previousDispatch); }
void Device::checkError() {
    if (error) { auto e=error; error=nullptr; std::rethrow_exception(e); }
    flushDraw();
}
void Device::flushDraw() {
    if(!pendingDraw) return;
    auto draw=std::move(*pendingDraw);
    pendingDraw.reset();
    backend->draw(draw);
}
void Device::queueDraw(Draw draw) {
    // Triangle sorting replays individual triangles. Merge only adjacent,
    // identical draws, preserving their order and bounding staging memory.
    auto key=[](const Draw& d) {
        return std::tie(d.color,d.depth,d.metadata,d.familyResolve,d.textures,d.viewport,d.scissor,
            d.scissorEnabled,d.depthTest,d.depthWrite,d.blend,d.cull,d.bitmap,
            d.depthFunc,d.cullFace,d.frontFace,d.srcRGB,d.dstRGB,d.srcAlpha,d.dstAlpha,
            d.equationRGB,d.equationAlpha,d.topology,d.colorMask,d.biasFactor,
            d.biasUnits,d.nearDepth,d.farDepth);
    };
    bool merge=pendingDraw && draw.topology==GL_TRIANGLES
        && pendingDraw->vertices.size()+draw.vertices.size()<=16384
        && key(*pendingDraw)==key(draw)
        && std::memcmp(&pendingDraw->uniforms,&draw.uniforms,sizeof(Uniforms))==0;
    for(unsigned i=0;merge&&i<4;i++) {
        const auto& a=pendingDraw->sampling[i];const auto& b=draw.sampling[i];
        merge=std::tie(a.minFilter,a.magFilter,a.wrapS,a.wrapT,a.maxLevel)
            ==std::tie(b.minFilter,b.magFilter,b.wrapS,b.wrapT,b.maxLevel);
    }
    if(merge) pendingDraw->vertices.insert(pendingDraw->vertices.end(),draw.vertices.begin(),draw.vertices.end());
    else { flushDraw();pendingDraw=std::move(draw); }
}
void Device::begin(unsigned w,unsigned h) {
    if (w!=width || h!=height) { flushDraw();width=w;height=h; Texture t; t.width=w;t.height=h;t.revision=++textures[0].revision; textures[0]=t; backend->texture(0,t,0,TextureUpload::Definition,nullptr,0); }
}
void Device::present() { checkError(); backend->present(width,height); }
Draw Device::drawState() const {
    Draw d=state;
    const auto& target=targets.at(framebuffer);
    d.color=target[0];d.depth=target[1];d.metadata=target[3]==2?target[2]:0;d.textures=boundTextures;
    for(unsigned i=0;i<4;i++){auto t=textures.find(boundTextures[i]);if(t!=textures.end())d.sampling[i]=t->second;}
    d.scissorEnabled=enabled.count(GL_SCISSOR_TEST);d.depthTest=enabled.count(GL_DEPTH_TEST);
    d.blend=enabled.count(GL_BLEND);d.cull=enabled.count(GL_CULL_FACE);
    if (!enabled.count(GL_POLYGON_OFFSET_FILL)) d.biasFactor=d.biasUnits=0;
    return d;
}
Uniforms Device::uniforms(const Program& p) const {
    if(p.cachedUniforms) return *p.cachedUniforms;
    Uniforms u;
    auto value=[&](const std::string& name)->const std::vector<float>& {
        static const std::vector<float> empty;
        auto loc=p.locations.find(name);if(loc==p.locations.end())return empty;
        auto v=p.values.find(loc->second);return v==p.values.end()?empty:v->second;
    };
    auto copy=[&](const std::string& name,auto& out) { const auto& v=value(name);std::copy_n(v.begin(),std::min(v.size(),out.size()),out.begin()); };
    auto scalar=[&](const std::string& name,float fallback=0.f) {const auto& v=value(name);return v.empty()?fallback:v[0];};
    auto def=[&](const std::string& name) {auto i=p.definitions.find(name);return i==p.definitions.end()?0.f:float(i->second);};
    copy("u_projection",u.projection);copy("u_modelview",u.modelview);copy("u_modelview_alternate",u.alternate);
    for(auto [name,out]: {std::pair<const char*,Matrix*>{"u_normal",&u.normal},{"u_normal_alternate",&u.alternateNormal}}) {
        const auto& v=value(name);for(unsigned c=0;c<3;c++)for(unsigned r=0;r<3;r++)if(c*3+r<v.size())(*out)[c*4+r]=v[c*3+r];
    }
    copy("u_ambient",u.ambient);copy("u_fog",u.fog);copy("u_fog_color",u.fogColor);
    copy("u_depth",u.depth);copy("u_family",u.family);
    u.family[3]=def("HAS_PATTERN")!=0.f?scalar("u_pattern"):65535.f;
    u.flags={def("HAS_TEXTURE"),def("SHADING_MODE"),def("HAS_TWO_SIDED_LIGHTING"),def("HAS_FOG")};
    u.material={def("ALPHA_MODE"),def("TEXTURE_MODE"),scalar("u_alpha_ref"),scalar("u_sort_scale")};
    u.blend={scalar("u_weigh_by_alpha"),scalar("u_force_coverage"),0,0};
    u.peel={scalar("u_peel_enabled"),scalar("u_peel_first"),1,1};
    const auto& peelSize=value("u_peel_size");if(peelSize.size()>=2){u.peel[2]=peelSize[0];u.peel[3]=peelSize[1];}
    for(unsigned i=0;i<4;i++) {
        auto prefix="u_lights["+std::to_string(i)+"].";auto& l=u.lights[i];
        copy(prefix+"position",l.position);copy(prefix+"direction",l.direction);copy(prefix+"diffuse",l.diffuse);
        copy(prefix+"specular",l.specular);copy(prefix+"extents",l.extents);l.extents[2]=scalar(prefix+"ambient");copy(prefix+"spot",l.spot);
        const auto& table=value(prefix+"spot_table[0]");for(size_t j=0;j<std::min(size_t(16),table.size());j++)l.table[j/4][j%4]=table[j];
        auto n=std::to_string(i);l.flags={def("HAS_LIGHT"+n),def("LIGHT"+n+"_TYPE"),def("LIGHT"+n+"_ATT_TYPE"),def("HAS_LIGHT"+n+"_SPECULAR")};
    }
    u.factors={scalar("u_factor"),scalar("u_mult_f",1),scalar("u_mult_b",1),0};
    u.multipliers={scalar("u_mult_1"),scalar("u_mult_2"),0,0};
    u.direct={scalar("u_direct_1"),scalar("u_direct_2"),0,0};u.keyed={scalar("u_keyed_1"),scalar("u_keyed_2"),0,0};
    u.inverted={scalar("u_invert_f"),scalar("u_invert_b"),0,0};
    p.cachedUniforms=u;
    return u;
}
void Device::uniform(GLint location,size_t count,const float* data) {
    if(location<0) return;
    auto& p=programs.at(program);
    auto& current=p.values[location];
    if(current.size()!=count || !std::equal(current.begin(),current.end(),data)) {
        current.assign(data,data+count);
        p.cachedUniforms.reset();
    }
}
void Device::ActiveTexture(GLenum t) {if(t<GL_TEXTURE0||t>=GL_TEXTURE0+4)throw std::runtime_error("Invalid Hikaru texture unit");activeTexture=t-GL_TEXTURE0;}
void Device::AttachShader(GLuint p,GLuint s){programs.at(p).shaders.push_back(s);}
void Device::BindAttribLocation(GLuint,GLuint,const GLchar*){}
void Device::BindBuffer(GLenum target,GLuint id){if(target==GL_ARRAY_BUFFER)buffer=id;else if(id)throw std::runtime_error("Unsupported Hikaru buffer binding");}
void Device::BindFramebuffer(GLenum target,GLuint id){if(target!=GL_READ_FRAMEBUFFER)framebuffer=id;if(target!=GL_DRAW_FRAMEBUFFER)readFramebuffer=id;}
void Device::BindRenderbuffer(GLenum,GLuint id){renderbuffer=id;}
void Device::BindTexture(GLenum target,GLuint id){if(target!=GL_TEXTURE_2D)throw std::runtime_error("Unsupported Hikaru texture target");boundTextures[activeTexture]=id;}
void Device::BindVertexArray(GLuint id){vao=id;}
void Device::BlendEquation(GLenum e){state.equationRGB=state.equationAlpha=e;}
void Device::BlendEquationSeparate(GLenum rgb,GLenum a){state.equationRGB=rgb;state.equationAlpha=a;}
void Device::BlendFunc(GLenum s,GLenum d){BlendFuncSeparate(s,d,s,d);}
void Device::BlendFuncSeparate(GLenum sr,GLenum dr,GLenum sa,GLenum da){state.srcRGB=sr;state.dstRGB=dr;state.srcAlpha=sa;state.dstAlpha=da;}
void Device::BufferData(GLenum target,GLsizeiptr size,const void* data,GLenum){if(target!=GL_ARRAY_BUFFER||size<0)throw std::runtime_error("Invalid Hikaru vertex buffer");auto& b=buffers.at(buffer);b.resize(size);if(data)std::memcpy(b.data(),data,size);}
void Device::BufferSubData(GLenum,GLintptr offset,GLsizeiptr size,const void* data){auto& b=buffers.at(buffer);if(offset<0||size<0||size_t(offset)>b.size()||size_t(size)>b.size()-size_t(offset))throw std::runtime_error("Hikaru vertex update exceeds buffer");std::memcpy(b.data()+offset,data,size);}
GLenum Device::CheckFramebufferStatus(GLenum){return GL_FRAMEBUFFER_COMPLETE;}
void Device::Clear(GLbitfield mask){flushDraw();backend->clear(drawState(),mask,clearColor,clearDepth);}
void Device::ClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){clearColor={r,g,b,a};}
void Device::ClearDepth(GLdouble d){clearDepth=d;}
void Device::ClearDepthf(GLfloat d){clearDepth=d;}
void Device::ColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){state.colorMask={bool(r),bool(g),bool(b),bool(a)};}
void Device::CompileShader(GLuint){}
GLuint Device::CreateProgram(){auto id=nextId++;programs[id]={};return id;}
GLuint Device::CreateShader(GLenum){auto id=nextId++;shaders[id]={};return id;}
void Device::CullFace(GLenum mode){state.cullFace=mode;}
void Device::DeleteBuffers(GLsizei n,const GLuint* ids){for(int i=0;i<n;i++)buffers.erase(ids[i]);}
void Device::DeleteFramebuffers(GLsizei n,const GLuint* ids){for(int i=0;i<n;i++)targets.erase(ids[i]);}
void Device::DeleteProgram(GLuint id){programs.erase(id);}
void Device::DeleteRenderbuffers(GLsizei n,const GLuint* ids){flushDraw();for(int i=0;i<n;i++){auto id=ids[i]|0x80000000u;textures.erase(id);backend->eraseTexture(id);}}
void Device::DeleteShader(GLuint id){shaders.erase(id);}
void Device::DeleteTextures(GLsizei n,const GLuint* ids){flushDraw();for(int i=0;i<n;i++){if(!ids[i])continue;textures.erase(ids[i]);backend->eraseTexture(ids[i]);}}
void Device::DeleteVertexArrays(GLsizei n,const GLuint* ids){for(int i=0;i<n;i++)arrays.erase(ids[i]);}
void Device::DepthFunc(GLenum f){state.depthFunc=f;}
void Device::DepthMask(GLboolean flag){state.depthWrite=flag;}
void Device::DepthRange(GLdouble n,GLdouble f){state.nearDepth=n;state.farDepth=f;}
void Device::DepthRangef(GLfloat n,GLfloat f){DepthRange(n,f);}
void Device::Disable(GLenum cap){enabled.erase(cap);}
void Device::DrawArrays(GLenum mode,GLint first,GLsizei count){
    if(first<0||count<0)throw std::runtime_error("Invalid Hikaru draw range");
    if(!count)return;
    auto d=drawState();auto& p=programs.at(program);d.bitmap=p.bitmap;d.familyResolve=p.familyResolve;d.uniforms=uniforms(p);d.topology=mode;d.uniforms.inverted[2]=d.color?1.f:0.f;
    // Fragment depth is already in window coordinates, so viewport range must
    // be applied in the shader. Range changes do not invalidate program uniforms.
    d.uniforms.depth[2]=d.nearDepth;d.uniforms.depth[3]=d.farDepth;
    d.vertices.resize(count);const auto& attrs=arrays.at(vao);
    auto read=[&](const Attribute& a,size_t i,Vec4& out){
        if(!a.enabled)return;
        const auto& b=buffers.at(a.buffer);size_t component=a.type==GL_FLOAT?4:a.type==GL_UNSIGNED_SHORT?2:1;
        size_t stride=a.stride?a.stride:size_t(a.size)*component;
        if(i>(SIZE_MAX-a.offset)/stride)throw std::runtime_error("Hikaru vertex index overflow");
        size_t off=a.offset+i*stride,bytes=size_t(a.size)*component;
        if(off>b.size()||bytes>b.size()-off||a.size<1||a.size>4)throw std::runtime_error("Hikaru vertex attribute exceeds buffer");
        for(int c=0;c<a.size;c++) {
            if(a.type==GL_FLOAT)std::memcpy(&out[c],b.data()+off+c*4,4);
            else if(a.type==GL_UNSIGNED_SHORT){uint16_t v;std::memcpy(&v,b.data()+off+c*2,2);out[c]=a.normalized?v/65535.f:float(v);}
            else if(a.type==GL_UNSIGNED_BYTE){uint8_t v=b[off+c];out[c]=a.normalized?v/255.f:float(v);}
            else throw std::runtime_error("Unsupported Hikaru vertex format");
        }
    };
    for(size_t i=0;i<size_t(count);i++) {
        auto& v=d.vertices[i];read(attrs[0],first+i,v.position);
        if(d.bitmap)read(attrs[1],first+i,v.uv);
        else {read(attrs[1],first+i,v.normal);read(attrs[2],first+i,v.diffuse);read(attrs[3],first+i,v.ambient);read(attrs[4],first+i,v.specular);read(attrs[5],first+i,v.unknown);read(attrs[6],first+i,v.uv);Vec4 a{},m{};read(attrs[7],first+i,a);read(attrs[8],first+i,m);v.extra={a[0],m[0],0,0};}
    }
    // Sampler uniforms select texture units; absent layers use the zero image.
    if(d.bitmap) {
        const char* names[]={"u_front","u_back","u_layer1","u_layer2"};
        for(unsigned i=0;i<4;i++) {
            auto location=p.locations.find(names[i]);
            unsigned unit=i;
            if(location!=p.locations.end()) {
                auto value=p.values.find(location->second);
                if(value!=p.values.end()&&!value->second.empty()) unit=unsigned(value->second[0]);
            }
            if(unit>=4) throw std::runtime_error("Invalid Hikaru bitmap sampler");
            d.textures[i]=boundTextures[unit];
            d.sampling[i]=d.textures[i]?textures.at(d.textures[i]):Texture{};
        }
    }
    for(unsigned i=0;i<4;i++) d.uniforms.lodBias[i]=d.sampling[i].lodBias;
    // The ES shader path supplies bias as a uniform, including when native
    // backends implement the dispatch without a real GL texture parameter.
    const auto bias=p.locations.find("u_lod_bias");
    if(bias!=p.locations.end()) {
        const auto value=p.values.find(bias->second);
        if(value!=p.values.end()&&!value->second.empty())d.uniforms.lodBias[0]=value->second[0];
    }
    queueDraw(std::move(d));
}
void Device::DrawBuffers(GLsizei n,const GLenum* values){
    if(n<1||n>2||values[0]!=GL_COLOR_ATTACHMENT0||(n==2&&values[1]!=GL_COLOR_ATTACHMENT1))
        throw std::runtime_error("Unsupported Hikaru draw attachments");
    targets.at(framebuffer)[3]=unsigned(n);
}
void Device::BindFragDataLocation(GLuint,GLuint,const GLchar*){}
void Device::ClearBufferuiv(GLenum buffer,GLint drawbuffer,const GLuint* value){
    // The family pass only clears its unsigned metadata to zero. Reject other
    // unsigned clears instead of losing bits through the float clear interface.
    if(buffer!=GL_COLOR||drawbuffer!=1||value[0]||value[1]||value[2]||value[3])
        throw std::runtime_error("Unsupported Hikaru unsigned clear");
    flushDraw();auto d=drawState();d.color=d.metadata;d.metadata=0;d.depth=0;
    if(!d.color||textures.at(d.color).format!=GL_RG32UI)
        throw std::runtime_error("Missing Hikaru family metadata attachment");
    backend->clear(d,GL_COLOR_BUFFER_BIT,Vec4{},1.0f);
}
void Device::Enable(GLenum cap){enabled.insert(cap);}
void Device::EnableVertexAttribArray(GLuint i){arrays.at(vao).at(i).enabled=true;}
void Device::FramebufferRenderbuffer(GLenum,GLenum attachment,GLenum,GLuint id){if(attachment!=GL_DEPTH_ATTACHMENT)throw std::runtime_error("Unsupported Hikaru depth attachment");targets.at(framebuffer)[1]=id? id|0x80000000u:0;}
void Device::FramebufferTexture2D(GLenum,GLenum attachment,GLenum,GLuint id,GLint level){if(level)throw std::runtime_error("Unsupported Hikaru target mip level");unsigned index=attachment==GL_DEPTH_ATTACHMENT?1:attachment==GL_COLOR_ATTACHMENT0?0:attachment==GL_COLOR_ATTACHMENT1?2:3;
    if(index==3)throw std::runtime_error("Unsupported Hikaru target attachment");
    targets.at(framebuffer)[index]=id;}
void Device::FrontFace(GLenum f){state.frontFace=f;}
void Device::GenBuffers(GLsizei n,GLuint* ids){for(int i=0;i<n;i++){ids[i]=nextId++;buffers[ids[i]]={};}}
void Device::GenFramebuffers(GLsizei n,GLuint* ids){for(int i=0;i<n;i++){ids[i]=nextId++;targets[ids[i]]={};}}
void Device::GenRenderbuffers(GLsizei n,GLuint* ids){for(int i=0;i<n;i++)ids[i]=nextId++;}
void Device::GenTextures(GLsizei n,GLuint* ids){for(int i=0;i<n;i++){ids[i]=nextId++;textures[ids[i]]={};}}
void Device::GenVertexArrays(GLsizei n,GLuint* ids){for(int i=0;i<n;i++){ids[i]=nextId++;arrays[ids[i]]={};}}
void Device::GetActiveUniform(GLuint,GLuint,GLsizei,GLsizei* length,GLint* size,GLenum* type,GLchar* name){if(length)*length=0;if(size)*size=0;if(type)*type=GL_FLOAT;if(name)*name=0;}
void Device::GetBooleanv(GLenum name,GLboolean* data){if(name==GL_COLOR_WRITEMASK){for(int i=0;i<4;i++)data[i]=state.colorMask[i];}else if(name==GL_DEPTH_WRITEMASK)*data=state.depthWrite;else *data=enabled.count(name);}
void Device::GetBufferSubData(GLenum,GLintptr offset,GLsizeiptr size,void* data){const auto& b=buffers.at(buffer);if(offset<0||size<0||size_t(offset)>b.size()||size_t(size)>b.size()-size_t(offset))throw std::runtime_error("Hikaru buffer read exceeds buffer");std::memcpy(data,b.data()+offset,size);}
GLenum Device::GetError(){return GL_NO_ERROR;}
void Device::GetFloatv(GLenum name,GLfloat* data){switch(name){case GL_COLOR_CLEAR_VALUE:std::copy(clearColor.begin(),clearColor.end(),data);break;case GL_DEPTH_CLEAR_VALUE:*data=clearDepth;break;case GL_DEPTH_RANGE:data[0]=state.nearDepth;data[1]=state.farDepth;break;case GL_POLYGON_OFFSET_FACTOR:*data=state.biasFactor;break;case GL_POLYGON_OFFSET_UNITS:*data=state.biasUnits;break;default:*data=0;}}
void Device::GetIntegerv(GLenum name,GLint* data){
    switch(name){
    case GL_DRAW_FRAMEBUFFER_BINDING:*data=framebuffer;break;case GL_READ_FRAMEBUFFER_BINDING:*data=readFramebuffer;break;
    case GL_RENDERBUFFER_BINDING:*data=renderbuffer;break;case GL_CURRENT_PROGRAM:*data=program;break;
    case GL_VERTEX_ARRAY_BINDING:*data=vao;break;case GL_ARRAY_BUFFER_BINDING:*data=buffer;break;
    case GL_ACTIVE_TEXTURE:*data=GL_TEXTURE0+activeTexture;break;case GL_TEXTURE_BINDING_2D:*data=boundTextures[activeTexture];break;
    case GL_VIEWPORT:std::copy(state.viewport.begin(),state.viewport.end(),data);break;case GL_SCISSOR_BOX:std::copy(state.scissor.begin(),state.scissor.end(),data);break;
    case GL_DEPTH_FUNC:*data=state.depthFunc;break;case GL_CULL_FACE_MODE:*data=state.cullFace;break;case GL_FRONT_FACE:*data=state.frontFace;break;
    case GL_BLEND_SRC_RGB:*data=state.srcRGB;break;case GL_BLEND_DST_RGB:*data=state.dstRGB;break;case GL_BLEND_SRC_ALPHA:*data=state.srcAlpha;break;case GL_BLEND_DST_ALPHA:*data=state.dstAlpha;break;
    case GL_BLEND_EQUATION_RGB:*data=state.equationRGB;break;case GL_BLEND_EQUATION_ALPHA:*data=state.equationAlpha;break;
    case GL_MAX_VIEWPORT_DIMS:data[0]=data[1]=16384;break;case GL_MAX_TEXTURE_SIZE:case GL_MAX_RENDERBUFFER_SIZE:*data=16384;break;
    case GL_MAJOR_VERSION:*data=3;break;case GL_MINOR_VERSION:*data=0;break;
    default:{auto it=pixelStore.find(name);*data=it==pixelStore.end()?0:it->second;break;}
    }
}
void Device::GetProgramInfoLog(GLuint,GLsizei n,GLsizei* length,GLchar* data){if(length)*length=0;if(n&&data)*data=0;}
void Device::GetProgramiv(GLuint id,GLenum name,GLint* value){*value=name==GL_LINK_STATUS?programs.count(id):0;}
void Device::GetShaderInfoLog(GLuint,GLsizei n,GLsizei* length,GLchar* data){if(length)*length=0;if(n&&data)*data=0;}
void Device::GetShaderiv(GLuint id,GLenum name,GLint* value){*value=name==GL_COMPILE_STATUS?shaders.count(id):0;}
const GLubyte* Device::GetString(GLenum name){static const GLubyte version[]="OpenGL ES 3.0 Hikaru native graphics dispatch";static const GLubyte empty[]="";return name==GL_VERSION?version:empty;}
GLint Device::GetUniformLocation(GLuint id,const GLchar* name){auto& p=programs.at(id);auto [i,added]=p.locations.emplace(name,uniformCounter);if(added)++uniformCounter;return i->second;}
GLboolean Device::IsBuffer(GLuint id){return buffers.count(id);}
GLboolean Device::IsEnabled(GLenum cap){return enabled.count(cap);}
GLboolean Device::IsProgram(GLuint id){return programs.count(id);}
GLboolean Device::IsVertexArray(GLuint id){return arrays.count(id);}
void Device::LinkProgram(GLuint id){auto& p=programs.at(id);p.cachedUniforms.reset();for(auto s:p.shaders){const auto& text=shaders.at(s);p.bitmap|=text.find("u_front")!=std::string::npos;p.familyResolve|=text.find("#define FAMILY_RESOLVE 1")!=std::string::npos;std::istringstream input(text);std::string line;while(std::getline(input,line)){std::istringstream words(line);std::string directive,name;int value;if(words>>directive>>name>>value && directive=="#define")p.definitions[name]=value;}}}
void Device::PixelStorei(GLenum name,GLint v){pixelStore[name]=v;}
void Device::PolygonOffset(GLfloat f,GLfloat u){state.biasFactor=f;state.biasUnits=u;}
void Device::ReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void* data){flushDraw();if(format!=GL_RGBA||type!=GL_UNSIGNED_BYTE)throw std::runtime_error("Unsupported Hikaru readback format");backend->readPixels(targets.at(readFramebuffer)[0],x,y,w,h,static_cast<uint8_t*>(data));}
void Device::RenderbufferStorage(GLenum,GLenum format,GLsizei w,GLsizei h){flushDraw();auto id=renderbuffer|0x80000000u;auto& t=textures[id];t.width=w;t.height=h;t.format=format;++t.revision;backend->texture(id,t,0,TextureUpload::Definition,nullptr,0);}
void Device::Scissor(GLint x,GLint y,GLsizei w,GLsizei h){state.scissor={x,y,w,h};}
void Device::ShaderSource(GLuint id,GLsizei count,const GLchar* const* strings,const GLint* lengths){auto& s=shaders.at(id);s.clear();for(int i=0;i<count;i++)s.append(strings[i],lengths&&lengths[i]>=0?size_t(lengths[i]):std::strlen(strings[i]));}
void Device::unpack(int w,int h,GLenum format,GLenum type,const void* data,std::vector<uint8_t>& out) const {
    if(w<=0||h<=0||size_t(w)>SIZE_MAX/size_t(h)/4)throw std::runtime_error("Invalid Hikaru texture dimensions");
    out.resize(size_t(w)*h*4);if(!data){std::fill(out.begin(),out.end(),0);return;}
    auto get=[&](GLenum n,int fallback){auto i=pixelStore.find(n);return i==pixelStore.end()?fallback:i->second;};
    size_t element=format==GL_RG?2:(type==GL_UNSIGNED_BYTE||type==GL_UNSIGNED_INT_2_10_10_10_REV||type==GL_UNSIGNED_INT)?4:2;
    size_t row=size_t(get(GL_UNPACK_ROW_LENGTH,w));if(!row)row=w;
    size_t align=std::max(get(GL_UNPACK_ALIGNMENT,4),1);size_t stride=(row*element+align-1)/align*align;
    const uint8_t* base=static_cast<const uint8_t*>(data)+size_t(get(GL_UNPACK_SKIP_ROWS,0))*stride+size_t(get(GL_UNPACK_SKIP_PIXELS,0))*element;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        auto* dst=out.data()+(size_t(y)*w+x)*4;const uint8_t* src=base+size_t(y)*stride+x*element;
        if(format==GL_RG){dst[0]=dst[1]=dst[2]=src[0];dst[3]=src[1];}
        else if(element==4)std::memcpy(dst,src,4);
        else {uint16_t v;std::memcpy(&v,src,2);if(type==GL_UNSIGNED_SHORT_1_5_5_5_REV){for(unsigned c=0;c<3;c++){unsigned q=(v>>(c*5))&31;dst[c]=(q<<3)|(q>>2);}dst[3]=(v&0x8000)?255:0;}
            else if(type==GL_UNSIGNED_SHORT_4_4_4_4_REV){for(unsigned c=0;c<4;c++)dst[c]=((v>>(c*4))&15)*17;}
            else throw std::runtime_error("Unsupported Hikaru packed texture format");}
    }
}
void Device::TexImage2D(GLenum,GLint level,GLint format,GLsizei w,GLsizei h,GLint,GLenum source,GLenum type,const void* data){
    flushDraw();
    auto id=boundTextures[activeTexture];if(!id)throw std::runtime_error("Hikaru texture zero is immutable");auto& t=textures.at(id);
    if(level==0){t.width=w;t.height=h;t.format=format==GL_RG8?GL_RGBA8:format;}++t.revision;
    std::vector<uint8_t> pixels;unpack(w,h,source,type,data,pixels);
    backend->texture(id,t,level,TextureUpload::Definition,data?pixels.data():nullptr,data?pixels.size():0);
}
void Device::TexParameterf(GLenum,GLenum name,GLfloat v){
    if(name==GL_TEXTURE_LOD_BIAS) {
        auto id=boundTextures[activeTexture];
        if(id) textures.at(id).lodBias=v;
    } else TexParameteri(GL_TEXTURE_2D,name,GLint(v));
}
void Device::TexParameteri(GLenum,GLenum name,GLint v){auto id=boundTextures[activeTexture];if(!id)return;auto& t=textures.at(id);switch(name){case GL_TEXTURE_MIN_FILTER:t.minFilter=v;break;case GL_TEXTURE_MAG_FILTER:t.magFilter=v;break;case GL_TEXTURE_WRAP_S:t.wrapS=v;break;case GL_TEXTURE_WRAP_T:t.wrapT=v;break;case GL_TEXTURE_MAX_LEVEL:t.maxLevel=v;break;default:break;}}
void Device::TexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum source,GLenum type,const void* data){
    if(target!=GL_TEXTURE_2D||level!=0||x!=0||y!=0)
        throw std::runtime_error("Unsupported Hikaru texture update region");
    const auto id=boundTextures[activeTexture];
    if(!id)throw std::runtime_error("Hikaru texture zero is immutable");
    auto& t=textures.at(id);
    if(w<=0||h<=0||unsigned(w)!=t.width||unsigned(h)!=t.height)
        throw std::runtime_error("Hikaru texture update must replace the full image");
    const bool rgba8Source=source==GL_RGBA&&(type==GL_UNSIGNED_BYTE||type==GL_UNSIGNED_SHORT_1_5_5_5_REV||type==GL_UNSIGNED_SHORT_4_4_4_4_REV);
    const bool packed10Source=source==GL_RGBA&&type==GL_UNSIGNED_INT_2_10_10_10_REV;
    const bool grayAlphaSource=source==GL_RG&&type==GL_UNSIGNED_BYTE;
    const bool compatible=t.format==GL_RGB10_A2?packed10Source:
        (t.format==GL_RGBA8||t.format==GL_RGB5_A1||t.format==GL_RGBA4)&&(rgba8Source||(t.format==GL_RGBA8&&grayAlphaSource));
    if(!compatible)throw std::runtime_error("Unsupported Hikaru full texture update format");
    if(!data)throw std::runtime_error("Hikaru full texture update requires pixel data");
    unpack(w,h,source,type,data,fullTextureUpdatePixels);
    // Preserve queued draws against the old texture contents before submitting the replacement.
    flushDraw();
    ++t.revision;
    backend->texture(id,t,level,TextureUpload::FullUpdate,fullTextureUpdatePixels.data(),fullTextureUpdatePixels.size());
}
void Device::Uniform1f(GLint l,GLfloat v){uniform(l,1,&v);}
void Device::Uniform1fv(GLint l,GLsizei n,const GLfloat* v){uniform(l,n,v);}
void Device::Uniform1i(GLint l,GLint v){float f=v;uniform(l,1,&f);}
void Device::Uniform2f(GLint l,GLfloat x,GLfloat y){float v[]={x,y};uniform(l,2,v);}
void Device::Uniform2fv(GLint l,GLsizei n,const GLfloat* v){uniform(l,size_t(n)*2,v);}
void Device::Uniform3f(GLint l,GLfloat x,GLfloat y,GLfloat z){float v[]={x,y,z};uniform(l,3,v);}
void Device::Uniform3fv(GLint l,GLsizei n,const GLfloat* v){uniform(l,size_t(n)*3,v);}
void Device::UniformMatrix3fv(GLint l,GLsizei n,GLboolean t,const GLfloat* v){if(t)throw std::runtime_error("Transposed Hikaru matrix upload");uniform(l,size_t(n)*9,v);}
void Device::UniformMatrix4fv(GLint l,GLsizei n,GLboolean t,const GLfloat* v){if(t)throw std::runtime_error("Transposed Hikaru matrix upload");uniform(l,size_t(n)*16,v);}
void Device::UseProgram(GLuint id){program=id;}
void Device::VertexAttribPointer(GLuint i,GLint size,GLenum type,GLboolean normalized,GLsizei stride,const void* pointer){auto& a=arrays.at(vao).at(i);bool wasEnabled=a.enabled;a={buffer,size,type,bool(normalized),stride,reinterpret_cast<size_t>(pointer),wasEnabled};}
void Device::Viewport(GLint x,GLint y,GLsizei w,GLsizei h){state.viewport={x,y,w,h};}
} // namespace hikaru::graphics
