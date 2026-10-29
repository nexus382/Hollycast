/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "gl_shader_chain.h"

#include "glcache.h"
#include "cfg/option.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"
#include <algorithm>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr char FullscreenVertexShader[] = R"(
in highp vec3 in_pos;
in mediump vec2 in_uv;
out mediump vec2 vTexCoord;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	vTexCoord = in_uv;
}
)";

constexpr char RotatedFullscreenVertexShader[] = R"(
in highp vec3 in_pos;
in mediump vec2 in_uv;
out mediump vec2 vTexCoord;

void main()
{
	gl_Position = vec4(-in_pos.y, in_pos.x, in_pos.z, 1.0);
	vTexCoord = in_uv;
}
)";

const char *postProcessGlslVersion()
{
	// The normal renderer keeps a GLSL 1.30 baseline for older OpenGL drivers.
	// Shader packages are allowed to use modern local expressions, so use the
	// full language version when the active context is already OpenGL 4.3+.
	if (gl.is_gles && gl.gl_major >= 3 && gl.gl_minor >= 1) return "#version 310 es";
	if (!gl.is_gles && (gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 3)))
		return "#version 430";
	if (!gl.is_gles && (gl.gl_major > 3 || (gl.gl_major == 3 && gl.gl_minor >= 3)))
		return "#version 330";
	return gl.glsl_version_header;
}

class PostProcessSource final : public ShaderSource
{
public:
	explicit PostProcessSource(bool nativeStages = false) : ShaderSource(postProcessGlslVersion())
	{
		addConstant("TARGET_GL", gl.gl_version);
		if (!nativeStages)
			addSource(ShaderCompatSource);
		else if (gl.is_gles)
			addSource("precision highp float;\nprecision highp int;\n");
	}
};

void removeVersionDirective(std::string& source)
{
	const size_t lineEnd = source.find_first_of("\r\n");
	const std::string firstLine = source.substr(0, lineEnd);
	if (firstLine.rfind("#version", 0) != 0)
		return;
	source.erase(0, lineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", lineEnd));
}

const char *glslType(postprocess::SlangParameterType type)
{
	return postprocess::shaderParameterGlslType(type);
}

bool hasUniformDeclaration(const std::string& source, const std::string& name)
{
	const std::regex declaration("\\buniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s+" + name + "\\b");
	return std::regex_search(source, declaration);
}

bool hasInputDeclaration(const std::string& source, const std::string& name)
{
	const std::regex declaration("\\b(?:in|varying)\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s+" + name + "\\b");
	return std::regex_search(source, declaration);
}

// A custom stage may supply its own varying at zero. Avoid injecting an unused
// convenience UV beside that interface while retaining it for portable shaders.
bool needsDefaultTexCoord(const std::string& source, const char *direction)
{
	if (std::regex_search(source, std::regex(R"(\bvTexCoord\b)")))
		return true;
	const std::regex customZero("\\blayout\\s*\\(\\s*location\\s*=\\s*0\\s*\\)\\s*"
			"(?:(?:flat|noperspective|centroid)\\s+)?" + std::string(direction) + "\\s+");
	return !std::regex_search(source, customZero);
}

std::string uniformPreamble(const std::vector<postprocess::ShaderParameter>& parameters,
		const std::vector<postprocess::SlangParameter>& slangParameters,
		const std::vector<postprocess::SlangResource>& resources, const std::string& source)
{
	std::string preamble;
	bool hasSource = false;
	for (const postprocess::SlangResource& resource : resources) {
		preamble += "uniform " + std::string(postprocess::shaderResourceGlslType(resource.type, resource.numericType)) + " " + resource.name + ";\n";
		hasSource = hasSource || resource.name == "Source";
	}
	if (!hasSource && !hasUniformDeclaration(source, "Source")) preamble += "uniform sampler2D Source;\n";
	if (!hasUniformDeclaration(source, "SourceSize")) preamble += "uniform mediump vec2 SourceSize;\n";
	if (!hasUniformDeclaration(source, "OriginalSize")) preamble += "uniform mediump vec2 OriginalSize;\n";
	if (!hasUniformDeclaration(source, "OutputSize")) preamble += "uniform mediump vec2 OutputSize;\n";
	if (!hasUniformDeclaration(source, "FinalViewportSize")) preamble += "uniform mediump vec2 FinalViewportSize;\n";
	if (!hasUniformDeclaration(source, "HasDepth")) preamble += "uniform int HasDepth;\n";
	if (!hasUniformDeclaration(source, "FrameCount")) preamble += "uniform int FrameCount;\n";
	if (!hasUniformDeclaration(source, "Rotation")) preamble += "uniform int Rotation;\n";
	std::unordered_set<std::string> emittedParameters;
	for (const postprocess::ShaderParameter& parameter : parameters)
		if (!hasUniformDeclaration(source, parameter.name) && emittedParameters.emplace(parameter.name).second)
			preamble += "uniform " + std::string(glslType(parameter.type)) + " " + parameter.name + ";\n";
	for (const postprocess::SlangParameter& parameter : slangParameters)
		if (!hasUniformDeclaration(source, parameter.name) && emittedParameters.emplace(parameter.name).second)
			preamble += "uniform " + std::string(glslType(parameter.type)) + " " + parameter.name + ";\n";
	return preamble;
}

std::string fragmentPreamble(const std::vector<postprocess::ShaderParameter>& parameters,
		const std::vector<postprocess::SlangParameter>& slangParameters,
		const std::vector<postprocess::SlangResource>& resources, const std::string& source, uint32_t outputCount)
{
	std::string preamble = uniformPreamble(parameters, slangParameters, resources, source);
	if (outputCount == 1)
		preamble += "#define FxOutput0 gl_FragColor\n";
	else
		for (uint32_t output = 0; output < outputCount; ++output)
			preamble += "layout(location = " + std::to_string(output) + ") out vec4 FxOutput" + std::to_string(output) + ";\n";
	if (!hasInputDeclaration(source, "vTexCoord") && needsDefaultTexCoord(source, "in"))
		preamble += "in mediump vec2 vTexCoord;\n";
	return preamble;
}

void removeResourceDeclarations(std::string& source)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:[iu]?sampler|texture)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)");
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);)
		if (!std::regex_match(line, resource))
			cleaned += line + '\n';
	source = std::move(cleaned);
}

std::string vertexResourcePreamble(const std::vector<postprocess::ShaderParameter>& parameters,
		const std::vector<postprocess::SlangParameter>& slangParameters,
		const std::vector<postprocess::SlangResource>& resources, const std::string& source)
{
	// Portable custom stages have their interface declarations stripped before
	// backend compilation, so recreate the fixed fullscreen varying here.
	std::string preamble = needsDefaultTexCoord(source, "out") ? "out mediump vec2 vTexCoord;\n" : "";
	preamble += uniformPreamble(parameters, slangParameters, resources, source);
	return preamble;
}

bool isTexture2D(postprocess::SlangResourceType type)
{
	return type == postprocess::SlangResourceType::Sampler2D || type == postprocess::SlangResourceType::Texture2D;
}

GLenum shaderTextureTarget(postprocess::SlangResourceType type)
{
	switch (type) {
#ifndef GLES
	case postprocess::SlangResourceType::Sampler1D: case postprocess::SlangResourceType::Texture1D: return GL_TEXTURE_1D;
#endif
#ifndef GLES2
	case postprocess::SlangResourceType::Sampler3D: case postprocess::SlangResourceType::Texture3D: return GL_TEXTURE_3D;
	case postprocess::SlangResourceType::Sampler2DArray: case postprocess::SlangResourceType::Texture2DArray: return GL_TEXTURE_2D_ARRAY;
#endif
	case postprocess::SlangResourceType::SamplerCube: case postprocess::SlangResourceType::TextureCube: return GL_TEXTURE_CUBE_MAP;
	default: return GL_TEXTURE_2D;
	}
}

GLuint compileComputeShader(const std::string& source, std::string& error)
{
#ifdef GL_COMPUTE_SHADER
	if (glDispatchCompute == nullptr || glBindImageTexture == nullptr) { error = "Compute effects require OpenGL 4.3 or GLES 3.1."; return 0; }
	const char *text = source.c_str();
	const GLuint shader = glCreateShader(GL_COMPUTE_SHADER); glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
	GLint valid = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &valid);
	if (!valid) { char log[8192] {}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); error = log; glDeleteShader(shader); return 0; }
	const GLuint program = glCreateProgram(); glAttachShader(program, shader); glLinkProgram(program); glDeleteShader(shader);
	glGetProgramiv(program, GL_LINK_STATUS, &valid);
	if (!valid) { char log[8192] {}; glGetProgramInfoLog(program, sizeof(log), nullptr, log); error = log; glDeleteProgram(program); return 0; }
	return program;
#else
	error = "Compute effects require OpenGL 4.3 or GLES 3.1."; return 0;
#endif
}

GLint glWrapMode(postprocess::ShaderWrapMode mode)
{
	switch (mode) {
	case postprocess::ShaderWrapMode::Repeat: return GL_REPEAT;
	case postprocess::ShaderWrapMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
	case postprocess::ShaderWrapMode::ClampToBorder:
		// Desktop GL guarantees transparent border sampling. GLES has no portable
		// border-clamp state, so retain edge clamp there.
#ifndef GLES
		return GL_CLAMP_TO_BORDER;
#else
		return GL_CLAMP_TO_EDGE;
#endif
	case postprocess::ShaderWrapMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
	}
	return GL_CLAMP_TO_EDGE;
}

GLenum glBlendFactor(postprocess::ShaderBlendFactor factor)
{
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return GL_ZERO;
	case postprocess::ShaderBlendFactor::One: return GL_ONE;
	case postprocess::ShaderBlendFactor::SourceColor: return GL_SRC_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR;
	case postprocess::ShaderBlendFactor::DestinationColor: return GL_DST_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return GL_ONE_MINUS_DST_COLOR;
	case postprocess::ShaderBlendFactor::SourceAlpha: return GL_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return GL_DST_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return GL_ONE_MINUS_DST_ALPHA;
	}
	return GL_ONE;
}

GLenum glBlendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return GL_FUNC_ADD;
	case postprocess::ShaderBlendOperation::Subtract: return GL_FUNC_SUBTRACT;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
	case postprocess::ShaderBlendOperation::Minimum: return GL_MIN;
	case postprocess::ShaderBlendOperation::Maximum: return GL_MAX;
	}
	return GL_FUNC_ADD;
}

bool glTargetFormat(const postprocess::ShaderPass& pass, GLenum& internalFormat, GLenum& pixelFormat,
		GLenum& pixelType, std::string& error)
{
#ifndef GLES2
	if (pass.targetFormatName == "R8_UNORM") { internalFormat = GL_R8; pixelFormat = GL_RED; pixelType = GL_UNSIGNED_BYTE; return true; }
	if (pass.targetFormatName == "R16_UNORM") { internalFormat = GL_R16; pixelFormat = GL_RED; pixelType = GL_UNSIGNED_SHORT; return true; }
	if (pass.targetFormatName == "R16_SFLOAT") { internalFormat = GL_R16F; pixelFormat = GL_RED; pixelType = GL_HALF_FLOAT; return true; }
	if (pass.targetFormatName == "R32_SFLOAT") { internalFormat = GL_R32F; pixelFormat = GL_RED; pixelType = GL_FLOAT; return true; }
	if (pass.targetFormatName == "R32_SINT") { internalFormat = GL_R32I; pixelFormat = GL_RED_INTEGER; pixelType = GL_INT; return true; }
	if (pass.targetFormatName == "R32_UINT") { internalFormat = GL_R32UI; pixelFormat = GL_RED_INTEGER; pixelType = GL_UNSIGNED_INT; return true; }
	if (pass.targetFormatName == "R8G8_UNORM") { internalFormat = GL_RG8; pixelFormat = GL_RG; pixelType = GL_UNSIGNED_BYTE; return true; }
	if (pass.targetFormatName == "R16G16_UNORM") { internalFormat = GL_RG16; pixelFormat = GL_RG; pixelType = GL_UNSIGNED_SHORT; return true; }
	if (pass.targetFormatName == "R16G16_SFLOAT") { internalFormat = GL_RG16F; pixelFormat = GL_RG; pixelType = GL_HALF_FLOAT; return true; }
	if (pass.targetFormatName == "R32G32_SFLOAT") { internalFormat = GL_RG32F; pixelFormat = GL_RG; pixelType = GL_FLOAT; return true; }
	if (pass.targetFormatName == "R32G32B32A32_SINT") { internalFormat = GL_RGBA32I; pixelFormat = GL_RGBA_INTEGER; pixelType = GL_INT; return true; }
	if (pass.targetFormatName == "R32G32B32A32_UINT") { internalFormat = GL_RGBA32UI; pixelFormat = GL_RGBA_INTEGER; pixelType = GL_UNSIGNED_INT; return true; }
	if (pass.targetFormatName == "A2B10G10R10_UNORM_PACK32") { internalFormat = GL_RGB10_A2; pixelFormat = GL_RGBA; pixelType = GL_UNSIGNED_INT_2_10_10_10_REV; return true; }
	if (pass.targetFormatName == "B10G11R11_UFLOAT_PACK32") { internalFormat = GL_R11F_G11F_B10F; pixelFormat = GL_RGB; pixelType = GL_UNSIGNED_INT_10F_11F_11F_REV; return true; }
#endif
	if (!pass.targetFormatName.empty() && pass.targetFormatName != "R8G8B8A8_UNORM"
			&& pass.targetFormatName != "R8G8B8A8_SRGB" && pass.targetFormatName != "R16G16B16A16_SFLOAT"
			&& pass.targetFormatName != "R16G16B16A16_UNORM" && pass.targetFormatName != "R32G32B32A32_SFLOAT") {
		error = "OpenGL does not support post-processing target format '" + pass.targetFormatName + "'.";
		return false;
	}
	const bool wantsFloat = pass.targetFormat == postprocess::ShaderTargetFormat::Float
			|| pass.targetFormatName == "R16G16B16A16_SFLOAT" || pass.targetFormatName == "R32G32B32A32_SFLOAT";
	const bool wantsSrgb = pass.targetFormat == postprocess::ShaderTargetFormat::Srgb
			|| pass.targetFormatName == "R8G8B8A8_SRGB";
	if (pass.targetFormatName == "R16G16B16A16_UNORM") {
		if (gl.gl_major < 3) {
			error = "OpenGL post-processing 16-bit targets require OpenGL or GLES 3.0.";
			return false;
		}
		internalFormat = GL_RGBA16;
		pixelFormat = GL_RGBA;
		pixelType = GL_UNSIGNED_SHORT;
		return true;
	}
	if (wantsFloat || wantsSrgb) {
		if (gl.gl_major < 3) {
			error = "OpenGL post-processing float and sRGB targets require OpenGL or GLES 3.0.";
			return false;
		}
		if (wantsFloat) {
			internalFormat = pass.targetFormatName == "R32G32B32A32_SFLOAT" ? GL_RGBA32F : GL_RGBA16F;
			pixelFormat = GL_RGBA;
			pixelType = pass.targetFormatName == "R32G32B32A32_SFLOAT" ? GL_FLOAT : GL_HALF_FLOAT;
			return true;
		}
		internalFormat = GL_SRGB8_ALPHA8;
		pixelFormat = GL_RGBA;
		pixelType = GL_UNSIGNED_BYTE;
		return true;
	}
	internalFormat = gl.gl_major >= 3 ? GL_RGBA8 : GL_RGBA;
	pixelFormat = GL_RGBA;
	pixelType = GL_UNSIGNED_BYTE;
	return true;
}

bool usesSrgbFramebuffer(const postprocess::ShaderPass& pass)
{
	const auto isSrgb = [](postprocess::ShaderTargetFormat format, const std::string& formatName) {
		return format == postprocess::ShaderTargetFormat::Srgb || formatName == "R8G8B8A8_SRGB";
	};
	if (pass.outputs.empty())
		return isSrgb(pass.targetFormat, pass.targetFormatName);
	return std::any_of(pass.outputs.begin(), pass.outputs.end(), [&](const postprocess::ShaderPassOutput& output) {
		return isSrgb(output.targetFormat, output.targetFormatName);
	});
}

std::unique_ptr<GlFramebuffer> createTarget(const postprocess::ShaderPass& pass, int width, int height, std::string& error)
{
	GLenum internalFormat;
	GLenum pixelFormat;
	GLenum pixelType;
	if (!glTargetFormat(pass, internalFormat, pixelFormat, pixelType, error))
		return nullptr;
	return std::make_unique<GlFramebuffer>(width, height, false, 0, internalFormat, pixelFormat, pixelType);
}

} // namespace

namespace {
void attachOutputImage(GLenum framebufferTarget, GLenum textureTarget, GLuint texture, uint32_t layer)
{
#ifndef GLES
	if (textureTarget == GL_TEXTURE_1D) {
		glFramebufferTexture1D(framebufferTarget, GL_COLOR_ATTACHMENT0, textureTarget, texture, 0);
		return;
	}
#endif
#ifndef GLES2
	if (textureTarget == GL_TEXTURE_3D || textureTarget == GL_TEXTURE_2D_ARRAY) {
		glFramebufferTextureLayer(framebufferTarget, GL_COLOR_ATTACHMENT0, texture, 0, layer);
		return;
	}
#endif
	glFramebufferTexture2D(framebufferTarget, GL_COLOR_ATTACHMENT0, textureTarget, texture, 0);
}
} // namespace

bool GlShaderChain::clearOutput(OutputTarget& destination, std::string& error)
{
	if (!transferDrawFramebuffer) glGenFramebuffers(1, &transferDrawFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, transferDrawFramebuffer);
	glcache.Disable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glcache.InvalidateBlendState();
	for (uint32_t layer = 0; layer < destination.depth; ++layer) {
		attachOutputImage(GL_FRAMEBUFFER, destination.target, destination.getTexture(), layer);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			error = "OpenGL cannot clear the requested shader image format.";
			return false;
		}
#ifndef GLES2
		const auto numeric = postprocess::shaderFormatNumericType(destination.formatName);
		if (numeric == postprocess::ShaderNumericType::UnsignedInteger) {
			const GLuint zero[4] {}; glClearBufferuiv(GL_COLOR, 0, zero);
		}
		else if (numeric == postprocess::ShaderNumericType::SignedInteger) {
			const GLint zero[4] {}; glClearBufferiv(GL_COLOR, 0, zero);
		}
		else {
			const GLfloat zero[4] {}; glClearBufferfv(GL_COLOR, 0, zero);
		}
#else
		glcache.ClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
	attachOutputImage(GL_FRAMEBUFFER, destination.target, 0, 0);
	return true;
}

bool GlShaderChain::copyOutput(const OutputTarget& source, OutputTarget& destination, std::string& error)
{
	if (source.width != destination.width || source.height != destination.height || source.depth != destination.depth
			|| source.target != destination.target)
		return true; // A resize starts a new, zero-initialized texture version.
#if defined(GL_VERSION_4_3) || defined(GL_ES_VERSION_3_2)
	if (glCopyImageSubData) {
		glCopyImageSubData(source.getTexture(), source.target, 0, 0, 0, 0, destination.getTexture(), destination.target,
				0, 0, 0, 0, source.width, source.height, source.depth);
		return true;
	}
#endif
#ifndef GLES2
	if (!transferReadFramebuffer) glGenFramebuffers(1, &transferReadFramebuffer);
	if (!transferDrawFramebuffer) glGenFramebuffers(1, &transferDrawFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, transferReadFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, transferDrawFramebuffer);
	glcache.Disable(GL_SCISSOR_TEST);
	for (uint32_t layer = 0; layer < source.depth; ++layer) {
		attachOutputImage(GL_READ_FRAMEBUFFER, source.target, source.getTexture(), layer);
		attachOutputImage(GL_DRAW_FRAMEBUFFER, destination.target, destination.getTexture(), layer);
		if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE
				|| glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			error = "OpenGL cannot copy a persistent shader image on this context.";
			return false;
		}
		glBlitFramebuffer(0, 0, source.width, source.height, 0, 0, destination.width, destination.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	attachOutputImage(GL_READ_FRAMEBUFFER, source.target, 0, 0);
	attachOutputImage(GL_DRAW_FRAMEBUFFER, destination.target, 0, 0);
	return true;
#else
	error = "Persistent effect textures require OpenGL or GLES 3.0.";
	return false;
#endif
}

bool GlShaderChain::ensureMrtTarget(MrtTarget& target, const postprocess::ShaderPass& pass,
		int width, int height, std::string& error)
{
	GLint maxTextureSize = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	if (width <= 0 || height <= 0 || width > maxTextureSize || height > maxTextureSize) {
		error = "OpenGL post-processing target exceeds the device texture-size limit.";
		return false;
	}
	const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
	const bool matching = target.outputs.size() == outputCount && std::all_of(target.outputs.begin(), target.outputs.end(),
			[&](const std::unique_ptr<OutputTarget>& output) {
				return output != nullptr && output->getWidth() == width && output->getHeight() == height;
			});
	if (matching && (pass.compute || target.framebuffer != 0))
		return true;
	target.initialized = false;
	if (target.framebuffer != 0) {
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
	}
	target.outputs.clear();
	target.outputs.reserve(outputCount);
	for (uint32_t output = 0; output < outputCount; ++output) {
		postprocess::ShaderPass outputPass = pass;
		if (!pass.outputs.empty()) {
			outputPass.targetFormat = pass.outputs[output].targetFormat;
			outputPass.targetFormatName = pass.outputs[output].targetFormatName;
		}
		auto resource = std::make_unique<OutputTarget>();
		resource->width = width; resource->height = height;
		resource->depth = pass.outputs.empty() ? 1u : pass.outputs[output].depth;
		resource->formatName = outputPass.targetFormatName;
		const auto type = pass.outputs.empty() ? postprocess::SlangResourceType::Sampler2D : pass.outputs[output].type;
		resource->target = shaderTextureTarget(type);
		if (resource->target == GL_TEXTURE_2D) {
			resource->framebuffer = createTarget(outputPass, width, height, error);
			if (!resource->framebuffer) return false;
		} else {
#ifndef GLES2
			GLenum format, pixels, pixelType;
			if (!glTargetFormat(outputPass, format, pixels, pixelType, error)) return false;
			glGenTextures(1, &resource->texture); glBindTexture(resource->target, resource->texture);
			const uint32_t texelBytes = postprocess::shaderFormatBytes(resource->formatName);
			std::vector<uint8_t> zeros(static_cast<size_t>(width) * height * resource->depth * (texelBytes ? texelBytes : 4u));
			glTexParameteri(resource->target, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(resource->target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#ifndef GLES
			if (resource->target == GL_TEXTURE_1D) glTexImage1D(resource->target, 0, format, width, 0, pixels, pixelType, zeros.data());
			else
#endif
			glTexImage3D(resource->target, 0, format, width, height, pass.outputs[output].depth, 0, pixels, pixelType, zeros.data());
#else
			error = "This shader texture dimension requires GLES 3.0 or desktop GL."; return false;
#endif
		}
		if (!clearOutput(*resource, error)) return false;
		target.outputs.push_back(std::move(resource));
	}
	if (pass.compute) return true;
	glGenFramebuffers(1, &target.framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	std::array<GLenum, 8> attachments {};
	for (uint32_t output = 0; output < pass.colorOutputCount; ++output) {
		attachments[output] = GL_COLOR_ATTACHMENT0 + output;
		glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[output], GL_TEXTURE_2D, target.outputs[output]->getTexture(), 0);
	}
	if (pass.colorOutputCount > 1)
	{
#ifdef GLES2
		error = "OpenGL MRT shader passes require OpenGL or GLES 3.0.";
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
		target.outputs.clear();
		return false;
#else
		GLint maxDrawBuffers = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
		if (maxDrawBuffers < static_cast<GLint>(pass.colorOutputCount)) {
			error = "OpenGL does not provide enough draw buffers for this shader pass.";
			glDeleteFramebuffers(1, &target.framebuffer);
			target.framebuffer = 0;
			target.outputs.clear();
			return false;
		}
		glDrawBuffers(pass.colorOutputCount, attachments.data());
#endif
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		error = "OpenGL could not create a complete multi-render-target framebuffer.";
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
		target.outputs.clear();
		return false;
	}
#ifndef GLES2
	if (usesSrgbFramebuffer(pass))
		glcache.Enable(GL_FRAMEBUFFER_SRGB);
	else
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	// Each attachment was initialized through its own typed view above.
#ifndef GLES2
	glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	return true;
}

GlShaderChain::~GlShaderChain()
{
	term();
}

void GlShaderChain::VertexArray::defineVtxAttribs()
{
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, nullptr);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5,
			reinterpret_cast<void *>(sizeof(float) * 3));
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE1_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS1_ARRAY);
	glDisableVertexAttribArray(VERTEX_UV1_ARRAY);
}

bool GlShaderChain::ensureResources()
{
	if (vertexBuffer != nullptr)
		return true;

	static constexpr float vertices[][5] = {
		{ -1.f, -1.f, 1.f, 0.f, 0.f },
		{ -1.f,  1.f, 1.f, 0.f, 1.f },
		{  1.f, -1.f, 1.f, 1.f, 0.f },
		{  1.f,  1.f, 1.f, 1.f, 1.f },
	};
	vertexBuffer = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER, GL_STATIC_DRAW);
	vertexBuffer->update(vertices, sizeof(vertices));
	PostProcessSource vertexSource;
	vertexSource.addSource(VertexCompatShader).addSource(FullscreenVertexShader);
	PostProcessSource fragmentSource;
	fragmentSource.addSource(PixelCompatShader)
			.addSource("in mediump vec2 vTexCoord;\nuniform sampler2D Source;\n"
					"void main() { gl_FragColor = texture(Source, vTexCoord); }\n");
	copyProgram = gl_CompileAndLink(vertexSource.generate().c_str(), fragmentSource.generate().c_str());
	copySourceUniform = glGetUniformLocation(copyProgram, "Source");
	PostProcessSource rotatedVertexSource;
	rotatedVertexSource.addSource(VertexCompatShader).addSource(RotatedFullscreenVertexShader);
	copyRotateProgram = gl_CompileAndLink(rotatedVertexSource.generate().c_str(), fragmentSource.generate().c_str());
	copyRotateSourceUniform = glGetUniformLocation(copyRotateProgram, "Source");
	return true;
}

void GlShaderChain::deletePrograms(std::vector<Program>& programs)
{
	for (Program& program : programs) {
#ifndef GLES2
		if (glDeleteSamplers != nullptr) {
			for (ResourceUniform& resource : program.resources) {
				if (resource.sampler != 0) glDeleteSamplers(1, &resource.sampler);
				if (resource.mipSampler != 0) glDeleteSamplers(1, &resource.mipSampler);
			}
		}
#endif
		if (program.handle != 0)
			glcache.DeleteProgram(program.handle);
	}
	programs.clear();
}

void GlShaderChain::deleteLookupTextures()
{
	deleteLookupTextures(lookupTextures);
}

void GlShaderChain::deleteLookupTextures(std::vector<LookupTexture>& textures)
{
	for (LookupTexture& lookup : textures)
		if (lookup.texture != 0)
			glcache.DeleteTextures(1, &lookup.texture);
	textures.clear();
}

bool GlShaderChain::ensureLookupTextures(std::string& error)
{
	return ensureLookupTextures(lookupTextures, error);
}

bool GlShaderChain::ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error)
{
	uint64_t bytes = 0;
	for (auto& lookup : textures) {
		if (lookup.texture) continue;
		postprocess::ShaderTextureData data;
		if (!postprocess::loadShaderTexture(lookup.config, data, error)) return false;
		if (data.pixels.size() > postprocess::MaxShaderChainTargetBytes - bytes) { error = "Shader lookups exceed the chain memory budget."; return false; }
		bytes += data.pixels.size();
		postprocess::ShaderPass formatPass; formatPass.targetFormatName = data.formatName;
		GLenum format, pixels, pixelType;
		if (!glTargetFormat(formatPass, format, pixels, pixelType, error)) return false;
		const GLenum target = shaderTextureTarget(data.type);
		if (lookup.config.numericType != postprocess::ShaderNumericType::Float && (lookup.config.linearFilter || lookup.config.useMipmaps)) { error = "Integer textures require nearest filtering without generated mipmaps."; return false; }
		glGenTextures(1, &lookup.texture); glActiveTexture(GL_TEXTURE0); glcache.InvalidateTextureBinding(); glBindTexture(target, lookup.texture);
#ifndef GLES
		if (target == GL_TEXTURE_1D) glTexImage1D(target, 0, format, data.width, 0, pixels, pixelType, data.pixels.data());
		else
#endif
		if (target == GL_TEXTURE_CUBE_MAP) {
			const size_t faceBytes = static_cast<size_t>(data.width) * data.height * postprocess::shaderFormatBytes(data.formatName);
			for (GLuint face = 0; face < 6; ++face) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, format, data.width, data.height, 0, pixels, pixelType, data.pixels.data() + face * faceBytes);
		} else if (target == GL_TEXTURE_2D) glTexImage2D(target, 0, format, data.width, data.height, 0, pixels, pixelType, data.pixels.data());
#ifndef GLES2
		else glTexImage3D(target, 0, format, data.width, data.height, data.depth * data.layers, 0, pixels, pixelType, data.pixels.data());
#else
		else { error = "This texture dimension requires GLES 3.0 or desktop GL."; return false; }
#endif
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, lookup.config.useMipmaps ? (lookup.config.linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : lookup.config.linearFilter ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, lookup.config.linearFilter ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(target, GL_TEXTURE_WRAP_S, glWrapMode(lookup.config.wrapMode)); glTexParameteri(target, GL_TEXTURE_WRAP_T, glWrapMode(lookup.config.wrapMode));
#ifndef GLES2
		if (target != GL_TEXTURE_2D) glTexParameteri(target, GL_TEXTURE_WRAP_R, glWrapMode(lookup.config.wrapMode));
#endif
		if (lookup.config.useMipmaps) glGenerateMipmap(target);
	}
	return true;
}

bool GlShaderChain::compileCandidate(const postprocess::ShaderChain& chain,
		std::vector<Program>& candidate, std::string& error)
{
	if (chain.passes.empty()) {
		error = "A shader chain must contain at least one pass.";
		return false;
	}

	GLint textureUnitLimit = 0;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnitLimit);
	if (textureUnitLimit <= 0) {
		error = "OpenGL did not report usable fragment texture units.";
		return false;
	}

	candidate.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const postprocess::ShaderPass& pass = chain.passes[passIndex];
		postprocess::ShaderPassSource loadedSource;
		const postprocess::ShaderPassSource *passSource = passIndex < chain.sources.size()
				? &chain.sources[passIndex] : &loadedSource;
		if (passSource == &loadedSource && !postprocess::loadShaderPassSource(pass, loadedSource, error)) {
			deletePrograms(candidate);
			return false;
		}
		std::string shaderBody = passSource->fragment;
		removeVersionDirective(shaderBody);
		removeResourceDeclarations(shaderBody);
		std::string vertexBody = passSource->vertex;
		removeVersionDirective(vertexBody);
		removeResourceDeclarations(vertexBody);
		if (passSource->nativeFxStages) {
			// Keep top-left effect UVs aligned with the renderer's GL texture origin.
			vertexBody = "#define main HollycastEffectVertexMain\n" + vertexBody
					+ "\n#undef main\nvoid main() { HollycastEffectVertexMain(); gl_Position.y = -gl_Position.y; }\n";
		}
		const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
		if (gl.is_gles && (std::any_of(passSource->slangResources.begin(), passSource->slangResources.end(), [](const auto& resource) {
			return resource.type == postprocess::SlangResourceType::Sampler1D || resource.type == postprocess::SlangResourceType::Texture1D;
		}) || std::any_of(pass.outputs.begin(), pass.outputs.end(), [](const auto& output) {
			return output.type == postprocess::SlangResourceType::Sampler1D;
		}))) {
			error = "GLES does not expose native one-dimensional shader images.";
			deletePrograms(candidate); return false;
		}
#if defined(GL_VERSION_4_0) || defined(GL_ES_VERSION_3_2)
		if (pass.renderState.reflected && pass.colorOutputCount > 1 && (!glBlendFuncSeparatei || !glBlendEquationSeparatei || !glColorMaski || !glEnablei || !glDisablei)) {
			const auto& first = pass.renderState.attachments[0];
			for (uint32_t i = 1; i < pass.colorOutputCount; ++i) {
				const auto& a = pass.renderState.attachments[i];
				if (a.blendEnabled != first.blendEnabled || a.source != first.source || a.destination != first.destination || a.sourceAlpha != first.sourceAlpha || a.destinationAlpha != first.destinationAlpha || a.operation != first.operation || a.alphaOperation != first.alphaOperation || a.writeMask != first.writeMask) {
					error = "This effect requires independent per-target blend state unavailable on this GL context."; deletePrograms(candidate); return false;
				}
			}
		}
#endif
		const bool supportsExplicitMrtLocations = gl.is_gles ? gl.gl_major >= 3
				: gl.gl_major > 3 || (gl.gl_major == 3 && gl.gl_minor >= 3);
		if (pass.colorOutputCount > 1 && !supportsExplicitMrtLocations) {
			error = "OpenGL MRT shader passes require OpenGL 3.3 or GLES 3.0.";
			deletePrograms(candidate);
			return false;
		}

		if (!pass.storageBindings.empty()) {
#ifdef GL_COMPUTE_SHADER
			if (!glBindImageTexture || !glMemoryBarrier) {
				error = "Writable shader images require OpenGL 4.2 or GLES 3.1.";
				deletePrograms(candidate);
				return false;
			}
			if (pass.compute) {
				GLint invocations = 0; glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
				for (GLuint axis = 0; axis < 3; ++axis) {
					GLint size = 0; glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, axis, &size);
					if (pass.localSize[axis] == 0 || pass.localSize[axis] > static_cast<uint32_t>(size)) {
						error = "Shader workgroup dimensions exceed this OpenGL device's compute limits.";
						deletePrograms(candidate); return false;
					}
				}
				if (static_cast<uint64_t>(pass.localSize[0]) * pass.localSize[1] * pass.localSize[2] > static_cast<uint32_t>(invocations)) {
					error = "Shader workgroup invocations exceed this OpenGL device's compute limit.";
					deletePrograms(candidate); return false;
				}
			}
			GLint imageUnits = 0;
			glGetIntegerv(GL_MAX_IMAGE_UNITS, &imageUnits);
			if (pass.storageBindings.size() > static_cast<size_t>(imageUnits)) {
				error = "Shader storage images exceed the device image-unit limit.";
				deletePrograms(candidate);
				return false;
			}
#else
			error = "Writable shader images are unavailable on this OpenGL build.";
			deletePrograms(candidate);
			return false;
#endif
		}
		try {
			std::vector<postprocess::ShaderParameter> passParameters;
			std::copy_if(chain.parameters.begin(), chain.parameters.end(), std::back_inserter(passParameters),
					[&](const postprocess::ShaderParameter& parameter) {
						return parameter.layerIndex == pass.layerIndex;
					});
			PostProcessSource vertexSource(passSource->nativeFxStages);
			if (!passSource->nativeFxStages)
				vertexSource.addSource(VertexCompatShader);
			if (passSource->nativeFxStages)
				vertexSource.addSource(uniformPreamble(passParameters, passSource->slangParameters,
						passSource->slangResources, vertexBody)).addSource(vertexBody);
			else if (vertexBody.empty())
				vertexSource.addSource(FullscreenVertexShader);
			else {
				// Portable stages use the fixed fullscreen VBO after their package
				// input declarations have been normalized by the frontend.
				if (!hasInputDeclaration(vertexBody, "in_pos"))
					vertexSource.addSource("in highp vec3 in_pos;\n");
				if (!hasInputDeclaration(vertexBody, "in_uv"))
					vertexSource.addSource("in mediump vec2 in_uv;\n");
				vertexSource.addSource(passSource->retroArchGlsl
						? "#define VertexCoord vec4(in_pos, 1.0)\n#define Position VertexCoord\n#define TexCoord in_uv\n#define aTexCoord in_uv\n"
						: "#define VertexCoord in_pos\n#define Position in_pos\n#define TexCoord in_uv\n#define aTexCoord in_uv\n")
						.addSource(vertexResourcePreamble(passParameters, passSource->slangParameters,
								passSource->slangResources, vertexBody))
						.addSource(vertexBody);
			}
			PostProcessSource fragmentSource(passSource->nativeFxStages);
			if (passSource->nativeFxStages)
				fragmentSource.addSource(uniformPreamble(passParameters, passSource->slangParameters,
						passSource->slangResources, shaderBody));
			else
				fragmentSource.addSource(PixelCompatShader)
						.addSource(fragmentPreamble(passParameters, passSource->slangParameters,
								passSource->slangResources, shaderBody, pass.colorOutputCount));
			fragmentSource.addSource(shaderBody);
			Program program;
			program.displayGlsl = passSource->displayGlsl;
			program.nativeFxStages = passSource->nativeFxStages;
			const std::string vertexShader = vertexSource.generate();
			if (pass.compute) {
				std::string compute = passSource->compute; removeVersionDirective(compute); removeResourceDeclarations(compute);
				// Compute entry points keep their native image declarations and local-size layout.
				PostProcessSource computeSource(true);
				computeSource.addSource(uniformPreamble(passParameters, passSource->slangParameters, passSource->slangResources, compute)).addSource(compute);
				program.handle = compileComputeShader(computeSource.generate(), error);
				if (!program.handle) { deletePrograms(candidate); return false; }
			} else program.handle = gl_CompileAndLink(vertexShader.c_str(), fragmentSource.generate().c_str(), &error);
			program.pass = pass;
			program.sourceUniform = glGetUniformLocation(program.handle, "Source");
			program.sourceSizeUniform = glGetUniformLocation(program.handle, "SourceSize");
			program.originalSizeUniform = glGetUniformLocation(program.handle, "OriginalSize");
			program.outputSizeUniform = glGetUniformLocation(program.handle, "OutputSize");
			program.finalViewportSizeUniform = glGetUniformLocation(program.handle, "FinalViewportSize");
			program.hasDepthUniform = glGetUniformLocation(program.handle, "HasDepth");
			program.frameCountUniform = glGetUniformLocation(program.handle, "FrameCount");
			program.rotationUniform = glGetUniformLocation(program.handle, "Rotation");
			for (const postprocess::ShaderParameter& parameter : passParameters)
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						parameter.layerIndex, parameter.type, parameter.values });
			for (const postprocess::SlangParameter& parameter : passSource->slangParameters) {
				if (std::any_of(program.parameters.begin(), program.parameters.end(), [&](const ParameterUniform& value) {
					return value.name == parameter.name;
				}))
					continue;
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						pass.layerIndex, parameter.type, parameter.defaultValue.values });
			}

			GLuint textureUnit = 0;
			bool hasSource = false;
			for (const postprocess::SlangResource& resource : passSource->slangResources) {
				if (resource.arrayCount != 1) {
					error = "OpenGL supports only single sampler2D resources; '" + resource.name + "' is unsupported.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				if (textureUnit >= static_cast<GLuint>(textureUnitLimit)) {
					error = "Shader pass requires more texture units than this OpenGL device supports.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				auto input = std::find_if(pass.inputs.begin(), pass.inputs.end(), [&](const postprocess::ShaderPassInput& value) {
					return value.bindingName == resource.name;
				});
				if (input == pass.inputs.end()) {
					error = "Shader resource '" + resource.name + "' has no resolved graph input.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				uint32_t lookupIndex = postprocess::InvalidShaderPass;
				if (input->kind == postprocess::ShaderInputKind::LookupTexture) {
					const auto lookup = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const postprocess::ShaderPresetTexture& texture) {
						return texture.name == input->resourceName;
					});
					if (lookup == chain.textures.end()) {
						error = "Shader lookup texture '" + input->resourceName + "' was not resolved.";
						glcache.DeleteProgram(program.handle);
						deletePrograms(candidate);
						return false;
					}
					lookupIndex = static_cast<uint32_t>(std::distance(chain.textures.begin(), lookup));
				}
				ResourceUniform binding { glGetUniformLocation(program.handle, resource.name.c_str()), input->kind,
						input->sourcePass, input->sourceOutput, input->historyOffset, input->resourceName,
						lookupIndex, textureUnit++ };
				binding.textureTarget = shaderTextureTarget(resource.type);
				const auto samplerConfig = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& sampler) {
					return sampler.bindingName == resource.name;
				});
				if (samplerConfig != pass.samplers.end() && input->kind != postprocess::ShaderInputKind::LookupTexture) {
					binding.hasSamplerState = true;
					binding.linearFilter = samplerConfig->linearFilter;
					binding.useMipmaps = samplerConfig->useMipmaps;
					binding.wrapMode = samplerConfig->wrapMode;
				}
				if (resource.name == "Source") {
					hasSource = true;
				}
				program.resources.push_back(binding);
			}
			if (!hasSource) {
				if (textureUnit >= static_cast<GLuint>(textureUnitLimit)) {
					error = "Shader pass requires more texture units than this OpenGL device supports.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				program.resources.push_back({ program.sourceUniform, postprocess::ShaderInputKind::Source,
						postprocess::InvalidShaderPass, 0, 1, {}, postprocess::InvalidShaderPass, textureUnit });
			}
#ifndef GLES2
			if (glGenSamplers != nullptr && glSamplerParameteri != nullptr) {
				for (ResourceUniform& binding : program.resources) {
					if (!binding.hasSamplerState)
						continue;
					glGenSamplers(1, &binding.sampler);
					glGenSamplers(1, &binding.mipSampler);
					if (binding.sampler == 0 || binding.mipSampler == 0) {
						if (glDeleteSamplers != nullptr)
							for (ResourceUniform& allocated : program.resources) {
								if (allocated.sampler != 0) glDeleteSamplers(1, &allocated.sampler);
								if (allocated.mipSampler != 0) glDeleteSamplers(1, &allocated.mipSampler);
							}
						error = "OpenGL could not create ReShade sampler state.";
						glcache.DeleteProgram(program.handle);
						deletePrograms(candidate);
						return false;
					}
					const GLint wrap = glWrapMode(binding.wrapMode);
					for (const GLuint sampler : { binding.sampler, binding.mipSampler }) {
						glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, binding.linearFilter ? GL_LINEAR : GL_NEAREST);
						glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
						glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
					}
					glSamplerParameteri(binding.sampler, GL_TEXTURE_MIN_FILTER,
							binding.linearFilter ? GL_LINEAR : GL_NEAREST);
					glSamplerParameteri(binding.mipSampler, GL_TEXTURE_MIN_FILTER,
							binding.linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
				}
			}
#endif
			candidate.push_back(std::move(program));
		} catch (const RendererException& exception) {
			if (error.empty())
				error = exception.what();
			deletePrograms(candidate);
			return false;
		}
	}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		candidate[passIndex].outputNeedsMipmaps = mipmapRequirements[passIndex];
	return true;
}

bool GlShaderChain::activate(const postprocess::ShaderChain& chain, std::string& error)
{
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error))
		return false;
	std::vector<LookupTexture> candidateLookupTextures;
	candidateLookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		candidateLookupTextures.push_back({ texture });
	if (!ensureLookupTextures(candidateLookupTextures, error)) {
		deletePrograms(candidate);
		deleteLookupTextures(candidateLookupTextures);
		return false;
	}

	deletePrograms(programs);
	deleteLookupTextures();
	programs = std::move(candidate);
	hostParameters = chain.parameters;
	lookupTextures = std::move(candidateLookupTextures);
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			if (input.kind != postprocess::ShaderInputKind::History
					&& input.kind != postprocess::ShaderInputKind::Feedback)
				continue;
			const uint32_t requiredDepth = input.kind == postprocess::ShaderInputKind::Feedback
					? 2u : input.historyOffset + 1;
			if (input.sourcePass == postprocess::InvalidShaderPass) {
				sourceHistoryDepth = std::max(sourceHistoryDepth, requiredDepth);
				needsSourceHistory = true;
			}
			else {
				programs[input.sourcePass].historyDepth =
						std::max(programs[input.sourcePass].historyDepth, requiredDepth);
			}
		}
	for (const auto& program : programs)
		for (const auto& output : program.pass.outputs)
			if (output.previousFrame && output.previousPass != postprocess::InvalidShaderPass)
				programs[output.previousPass].historyDepth = std::max(2u, programs[output.previousPass].historyDepth);
	targets.clear();
	stencilTargets.clear();
	displayInput.reset();
	depthInput.reset();
	mipmapInput.reset();
	sourceHistory.clear();
	error.clear();
	return true;
}

void GlShaderChain::updateConfiguredChain()
{
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return;
	configuredPaths = requestedPaths;
	configuredGeneration = generation;

	if (requestedPaths.empty()) {
		deletePrograms(programs);
		deleteLookupTextures();
		targets.clear();
		displayInput.reset();
		depthInput.reset();
		mipmapInput.reset();
		sourceHistory.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return;
	}

	std::string error;
	postprocess::ShaderChain candidate;
	if (!postprocess::loadShaderChain(requestedPaths, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedLabel.c_str(), error.c_str());
		return;
	}
	postprocess::applyShaderChainParameterValues(candidate, config::PostProcessShaderParameters.get());
	if (!activate(candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return;
	}
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, candidate.parameters,
			candidate.techniques, candidate.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
}

GlFramebuffer *GlShaderChain::render(GLuint sourceTexture, const postprocess::ShaderFrameInfo& frame,
		bool& presentationTransformApplied, GLuint depthTexture, int depthWidth, int depthHeight)
{
	const std::array<int,4> currentStencilSize { frame.sourceWidth, frame.sourceHeight, frame.viewportWidth, frame.viewportHeight };
	if (stencilFrameSize != currentStencilSize) { stencilTargets.clear(); stencilFrameSize = currentStencilSize; }

	presentationTransformApplied = false;
	bool composedDisplayInput = false;
	updateConfiguredChain();
	if (programs.empty() || sourceTexture == 0 || !ensureResources())
		return nullptr;
	const bool needsDepth = std::any_of(programs.begin(), programs.end(), [](const Program& program) {
		return std::any_of(program.resources.begin(), program.resources.end(), [](const ResourceUniform& resource) {
			return resource.kind == postprocess::ShaderInputKind::Depth;
		});
	});
	if (needsDepth && depthTexture == 0) {
		postprocess::reportShaderChainStatus(activePath, "This frame has no scene depth available for the selected shader.");
		return nullptr;
	}
	std::string lookupError;
	if (!ensureLookupTextures(lookupError)) {
		postprocess::reportShaderChainStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "Post-processing lookup texture was not loaded: %s", lookupError.c_str());
		return nullptr;
	}

	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty()) {
			for (Program& program : programs)
				for (ParameterUniform& parameter : program.parameters) {
					auto configured = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const postprocess::ShaderParameter& value) {
						return value.name == parameter.name && value.layerIndex == parameter.layerIndex;
					});
					if (configured != status.parameters.end())
						std::copy_n(configured->values.begin(), configured->componentCount, parameter.values.begin());
				}
		}
	}
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	postprocess::updateShaderHostParameters(hostParameters, frame);
	for (auto& program : programs) for (auto& uniform : program.parameters)
		for (const auto& host : hostParameters) if (!host.hostSource.empty() && host.name == uniform.name && host.layerIndex == uniform.layerIndex) uniform.values = host.values;
	GLuint inputTexture = sourceTexture;
	GLuint originalTexture = sourceTexture;
	bool inputHasMipmaps = false;
	struct SourceTextureState {
		GLuint texture;
		bool captured = false;
		GLint minFilter = GL_LINEAR;
		GLint magFilter = GL_LINEAR;
		GLint wrapS = GL_CLAMP_TO_EDGE;
		GLint wrapT = GL_CLAMP_TO_EDGE;
		void capture()
		{
			if (captured)
				return;
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);
			captured = true;
		}
		~SourceTextureState()
		{
			if (!captured)
				return;
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
		}
	} sourceTextureState { sourceTexture };
	// Composition and copy passes also adjust filtering; preserve the renderer's
	// texture state before any of those passes can touch the source texture.
	sourceTextureState.capture();
	SourceTextureState depthTextureState { depthTexture };
	if (needsDepth)
		depthTextureState.capture();
	uint64_t depthTargetBytes = 0;
	bool depthInputRotated = false;
	if (depthWidth <= 0) depthWidth = frame.sourceWidth;
	if (depthHeight <= 0) depthHeight = frame.sourceHeight;
	if (needsDepth && (depthWidth < frame.sourceWidth || depthHeight < frame.sourceHeight)) {
		postprocess::reportShaderChainStatus(activePath, "Scene depth is smaller than the current color frame.");
		return nullptr;
	}
	const bool rotateDepth = frame.rotationPending && std::any_of(programs.begin(), programs.end(),
			[](const Program& program) { return program.pass.composeDisplayInput; });
	if (needsDepth && (depthWidth != frame.sourceWidth || depthHeight != frame.sourceHeight || rotateDepth)) {
#ifndef GLES2
		// The OIT renderer reuses large depth surfaces. Snapshot its current
		// rectangle so normalized depth coordinates stay aligned after resizing.
		const int width = rotateDepth ? frame.sourceHeight : frame.sourceWidth;
		const int height = rotateDepth ? frame.sourceWidth : frame.sourceHeight;
		std::string error;
		if (!postprocess::accumulateShaderTargetBytes(width, height,
				postprocess::ShaderTargetFormat::Default, {}, false, 1, depthTargetBytes, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			return nullptr;
		}
		if (depthCopyProgram == 0) {
			PostProcessSource vertexSource;
			vertexSource.addSource(VertexCompatShader).addSource(FullscreenVertexShader);
			PostProcessSource fragmentSource;
			fragmentSource.addSource(PixelCompatShader).addSource(
					"in mediump vec2 vTexCoord;\nuniform highp sampler2D Source;\n"
					"uniform highp vec2 DepthUVScale;\nuniform int RotateDepth;\n"
					"void main() { vec2 uv = RotateDepth != 0 ? vec2(vTexCoord.y, 1.0 - vTexCoord.x) : vTexCoord;"
					" gl_FragColor = vec4(texture(Source, uv * DepthUVScale).r, 0.0, 0.0, 1.0); }\n");
			try {
				depthCopyProgram = gl_CompileAndLink(vertexSource.generate().c_str(), fragmentSource.generate().c_str(), &error);
			} catch (const RendererException&) {
				postprocess::reportShaderChainStatus(activePath, "OpenGL scene-depth copy shader failed: " + error);
				return nullptr;
			}
			if (depthCopyProgram == 0) {
				postprocess::reportShaderChainStatus(activePath, "OpenGL scene-depth copy shader failed: " + error);
				return nullptr;
			}
			depthCopySourceUniform = glGetUniformLocation(depthCopyProgram, "Source");
			depthCopyScaleUniform = glGetUniformLocation(depthCopyProgram, "DepthUVScale");
			depthCopyRotateUniform = glGetUniformLocation(depthCopyProgram, "RotateDepth");
		}
		if (!depthInput || depthInput->getWidth() != width || depthInput->getHeight() != height)
			depthInput = std::make_unique<GlFramebuffer>(width, height, false, 0, GL_R32F, GL_RED, GL_FLOAT);
		depthInput->bind();
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			postprocess::reportShaderChainStatus(activePath, "OpenGL cannot render a float scene-depth snapshot.");
			return nullptr;
		}
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
		glViewport(0, 0, width, height);
		glcache.UseProgram(depthCopyProgram);
		glActiveTexture(GL_TEXTURE0);
		if (glBindSampler != nullptr)
			glBindSampler(0, 0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, depthTexture);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glUniform1i(depthCopySourceUniform, 0);
		glUniform2f(depthCopyScaleUniform, static_cast<float>(frame.sourceWidth) / depthWidth,
				static_cast<float>(frame.sourceHeight) / depthHeight);
		glUniform1i(depthCopyRotateUniform, rotateDepth ? 1 : 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
		depthInputRotated = rotateDepth;
		if (!rotateDepth)
			depthTexture = depthInput->getTexture();
#else
		postprocess::reportShaderChainStatus(activePath, "Scene-depth snapshots require OpenGL or GLES 3.0.");
		return nullptr;
#endif
	}
	else
		depthInput.reset();
	bool displaySpaceReached = false;
	int originalWidth = passFrame.sourceWidth;
	int originalHeight = passFrame.sourceHeight;
	auto composeDisplayInput = [&]() {
		if (displaySpaceReached)
			return true;
		displaySpaceReached = true;
		if (!postprocess::shouldComposeDisplayInput(frame, false))
			return true;
		if (displayInput == nullptr || displayInput->getWidth() != frame.viewportWidth
				|| displayInput->getHeight() != frame.viewportHeight)
			displayInput = std::make_unique<GlFramebuffer>(frame.viewportWidth, frame.viewportHeight, false, true);
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		displayInput->bind();
		glViewport(0, 0, frame.viewportWidth, frame.viewportHeight);
		glcache.UseProgram(frame.rotationPending ? copyRotateProgram : copyProgram);
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, config::LinearInterpolation ? GL_LINEAR : GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config::LinearInterpolation ? GL_LINEAR : GL_NEAREST);
		const GLint sourceUniform = frame.rotationPending ? copyRotateSourceUniform : copySourceUniform;
		if (sourceUniform != -1)
			glUniform1i(sourceUniform, 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
		inputTexture = displayInput->getTexture();
		originalTexture = inputTexture;
		inputHasMipmaps = false;
		passFrame.sourceWidth = frame.viewportWidth;
		passFrame.sourceHeight = frame.viewportHeight;
		originalWidth = passFrame.sourceWidth;
		originalHeight = passFrame.sourceHeight;
		if (depthInputRotated)
			depthTexture = depthInput->getTexture();
		composedDisplayInput = true;
		return true;
	};
	if (programs.front().pass.composeDisplayInput && !composeDisplayInput())
		return nullptr;

	postprocess::ShaderFrameInfo sizeFrame = frame;
	bool sizeDisplaySpaceReached = false;
	uint64_t targetBytes = depthTargetBytes;
	std::string targetBudgetError;
	const auto displayBoundary = std::find_if(programs.begin(), programs.end(), [](const Program& program) {
		return program.pass.composeDisplayInput;
	});
	if (displayBoundary != programs.end() && postprocess::shouldComposeDisplayInput(frame, false)
			&& !postprocess::accumulateShaderTargetBytes(frame.viewportWidth, frame.viewportHeight,
					postprocess::ShaderTargetFormat::Default, {}, displayBoundary->pass.useMipmaps,
					1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		return nullptr;
	}
	if (programs.front().pass.useMipmaps
			&& !(programs.front().pass.composeDisplayInput && postprocess::shouldComposeDisplayInput(frame, false))
			&& !postprocess::accumulateShaderTargetBytes(frame.sourceWidth, frame.sourceHeight,
					postprocess::ShaderTargetFormat::Default, {}, true, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		return nullptr;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		if (programs[index].pass.composeDisplayInput && !sizeDisplaySpaceReached) {
			sizeDisplaySpaceReached = true;
			if (postprocess::shouldComposeDisplayInput(frame, false)) {
				sizeFrame.sourceWidth = frame.viewportWidth;
				sizeFrame.sourceHeight = frame.viewportHeight;
			}
		}
		std::string error;
		if (!postprocess::resolveTargetSize(programs[index].pass, sizeFrame,
				programs[index].targetWidth, programs[index].targetHeight, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			return nullptr;
		}
		if (programs[index].pass.updatesSource) { sizeFrame.sourceWidth = programs[index].targetWidth; sizeFrame.sourceHeight = programs[index].targetHeight; }
		const uint32_t outputCount = programs[index].pass.outputs.empty()
				? 1u : static_cast<uint32_t>(programs[index].pass.outputs.size());
		for (uint32_t output = 0; output < outputCount; ++output) {
			const postprocess::ShaderPassOutput config = programs[index].pass.outputs.empty()
					? postprocess::ShaderPassOutput {} : programs[index].pass.outputs[output];
			const postprocess::ShaderTargetFormat format = programs[index].pass.outputs.empty()
					? programs[index].pass.targetFormat : config.targetFormat;
			const std::string& formatName = programs[index].pass.outputs.empty()
					? programs[index].pass.targetFormatName : config.targetFormatName;
			if (!postprocess::accumulateShaderTargetBytes(programs[index].targetWidth, programs[index].targetHeight,
					format, formatName, programs[index].outputNeedsMipmaps[output],
					programs[index].historyDepth * (programs[index].pass.outputs.empty() ? 1u : programs[index].pass.outputs[output].depth), targetBytes, targetBudgetError)) {
				postprocess::reportShaderChainStatus(activePath, targetBudgetError);
				return nullptr;
			}
		}
		if (programs[index].pass.renderState.stencilEnabled && !postprocess::accumulateShaderTargetBytes(programs[index].targetWidth, programs[index].targetHeight, postprocess::ShaderTargetFormat::Default, "R32_UINT", false, 1, targetBytes, targetBudgetError)) { postprocess::reportShaderChainStatus(activePath, targetBudgetError); return nullptr; }
		MrtTargetRing& targetRing = targets[index];
		if (targetRing.size() != programs[index].historyDepth)
			targetRing.resize(programs[index].historyDepth);
		for (std::unique_ptr<MrtTarget>& target : targetRing) {
			if (target == nullptr)
				target = std::make_unique<MrtTarget>();
			if (!ensureMrtTarget(*target, programs[index].pass, programs[index].targetWidth,
					programs[index].targetHeight, error)) {
				postprocess::reportShaderChainStatus(activePath, error);
				return nullptr;
			}
		}
	}
	if (needsSourceHistory) {
		if (!postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
				postprocess::ShaderTargetFormat::Default, {}, false, sourceHistoryDepth,
				targetBytes, targetBudgetError)) {
			postprocess::reportShaderChainStatus(activePath, targetBudgetError);
			return nullptr;
		}
		if (sourceHistory.size() != sourceHistoryDepth)
			sourceHistory.resize(sourceHistoryDepth);
		for (HistoryTarget& target : sourceHistory)
			if (target.framebuffer == nullptr || target.framebuffer->getWidth() != passFrame.sourceWidth
					|| target.framebuffer->getHeight() != passFrame.sourceHeight) {
				target.framebuffer = std::make_unique<GlFramebuffer>(passFrame.sourceWidth, passFrame.sourceHeight, false, true);
				target.initialized = false;
				target.framebuffer->bind();
				glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
		HistoryTarget& historyTarget = sourceHistory[currentSourceTarget];
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		historyTarget.framebuffer->bind();
		glViewport(0, 0, passFrame.sourceWidth, passFrame.sourceHeight);
		glcache.UseProgram(copyProgram);
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, originalTexture);
		if (copySourceUniform != -1)
			glUniform1i(copySourceUniform, 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
		historyTarget.renderedFrame = frame.frameCount;
		historyTarget.initialized = true;
	}
	struct RestoreAttachmentState {
		GLuint count = 1;
		~RestoreAttachmentState() {
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#if defined(GL_VERSION_4_0) || defined(GL_ES_VERSION_3_2)
			if (glColorMaski && glDisablei) for (GLuint i = 0; i < count; ++i) { glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDisablei(GL_BLEND, i); }
#endif
			glcache.InvalidateBlendState();
		}
	} restoreAttachmentState;
	GlFramebuffer *output = nullptr;
	const auto disableFramebufferSrgb = [] {
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	};
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		restoreAttachmentState.count = std::max(restoreAttachmentState.count, program.pass.colorOutputCount);
		if (program.pass.composeDisplayInput && !composeDisplayInput())
			return nullptr;
		if (index == 0 || program.pass.layerIndex != programs[index - 1].pass.layerIndex) {
			originalTexture = inputTexture;
			originalWidth = passFrame.sourceWidth;
			originalHeight = passFrame.sourceHeight;
		}
		if (program.pass.useMipmaps && !inputHasMipmaps) {
			if (inputTexture == sourceTexture) {
				if (mipmapInput == nullptr || mipmapInput->getWidth() != passFrame.sourceWidth
						|| mipmapInput->getHeight() != passFrame.sourceHeight)
					mipmapInput = std::make_unique<GlFramebuffer>(passFrame.sourceWidth, passFrame.sourceHeight, false, true);
				glcache.Disable(GL_SCISSOR_TEST);
				glcache.Disable(GL_DEPTH_TEST);
				glcache.Disable(GL_STENCIL_TEST);
				glcache.Disable(GL_CULL_FACE);
				glcache.Disable(GL_BLEND);
#ifndef GLES2
				glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
				mipmapInput->bind();
				glViewport(0, 0, passFrame.sourceWidth, passFrame.sourceHeight);
				glcache.UseProgram(copyProgram);
				glActiveTexture(GL_TEXTURE0);
				sourceTextureState.capture();
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				if (copySourceUniform != -1)
					glUniform1i(copySourceUniform, 0);
				vertexArray.bind(vertexBuffer.get());
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				GlVertexArray::unbind();
				inputTexture = mipmapInput->getTexture();
			}
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
			glGenerateMipmap(GL_TEXTURE_2D);
			inputHasMipmaps = true;
		}
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		const int outputWidth = program.targetWidth;
		const int outputHeight = program.targetHeight;
		std::string error;
		MrtTargetRing& targetRing = targets[index];
		MrtTarget *target = targetRing[currentTarget].get();

		for (size_t outputIndex = 0; outputIndex < target->outputs.size(); ++outputIndex) {
			const auto& config = program.pass.outputs.empty() ? postprocess::ShaderPassOutput{} : program.pass.outputs[outputIndex];
			if (config.previousPass != postprocess::InvalidShaderPass && (!config.previousFrame || frame.frameCount != 0)) {
				const uint32_t requestedFrame = frame.frameCount - (config.previousFrame ? 1u : 0u);
				const auto& previous = targets[config.previousPass][requestedFrame % programs[config.previousPass].historyDepth];
				if (previous && previous->initialized && previous->renderedFrame == requestedFrame
						&& !copyOutput(*previous->outputs[config.previousOutput], *target->outputs[outputIndex], error)) {
					postprocess::reportShaderChainStatus(activePath, error);
					return nullptr;
				}
			}
			if (!target->initialized && program.outputNeedsMipmaps[outputIndex]) {
				const auto& image = *target->outputs[outputIndex];
				glActiveTexture(GL_TEXTURE0); glcache.InvalidateTextureBinding();
				glBindTexture(image.target, image.getTexture()); glGenerateMipmap(image.target);
			}
			if (program.pass.renderState.clearRenderTargets && !clearOutput(*target->outputs[outputIndex], error)) {
				postprocess::reportShaderChainStatus(activePath, error);
				return nullptr;
			}
		}
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
#ifndef GLES2
		if (usesSrgbFramebuffer(program.pass))
			glcache.Enable(GL_FRAMEBUFFER_SRGB);
		else
			glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		glViewport(0, 0, outputWidth, outputHeight);
		if (program.pass.blendEnabled && program.pass.updatesSource && !program.pass.renderState.clearRenderTargets) {
			// ReShade blends against the current backbuffer. Seed HollyCast's
			// private pass target with the current input before applying the pass.
			glcache.Disable(GL_BLEND);
			glcache.UseProgram(copyProgram);
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			if (copySourceUniform != -1)
				glUniform1i(copySourceUniform, 0);
			vertexArray.bind(vertexBuffer.get());
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			GlVertexArray::unbind();
			glcache.Enable(GL_BLEND);
			glcache.BlendFunc(glBlendFactor(program.pass.sourceBlend), glBlendFactor(program.pass.destinationBlend));
			glcache.BlendEquation(glBlendOperation(program.pass.blendOperation));
		}
		else
			glcache.Disable(GL_BLEND);
		if (!program.pass.compute && program.pass.renderState.reflected) {
			const auto& state = program.pass.renderState;
			const auto apply = [&](const postprocess::ShaderAttachmentState& attachment, GLuint index) {
#if defined(GL_VERSION_4_0) || defined(GL_ES_VERSION_3_2)
				if (glBlendFuncSeparatei && glBlendEquationSeparatei && glColorMaski && glEnablei && glDisablei) {
					if (attachment.blendEnabled) glEnablei(GL_BLEND, index); else glDisablei(GL_BLEND, index);
					glBlendFuncSeparatei(index, glBlendFactor(attachment.source), glBlendFactor(attachment.destination), glBlendFactor(attachment.sourceAlpha), glBlendFactor(attachment.destinationAlpha));
					glBlendEquationSeparatei(index, glBlendOperation(attachment.operation), glBlendOperation(attachment.alphaOperation));
					glColorMaski(index, attachment.writeMask & 1, attachment.writeMask & 2, attachment.writeMask & 4, attachment.writeMask & 8);
					return;
				}
#endif
				if (attachment.blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
				glBlendFuncSeparate(glBlendFactor(attachment.source), glBlendFactor(attachment.destination), glBlendFactor(attachment.sourceAlpha), glBlendFactor(attachment.destinationAlpha));
				glBlendEquationSeparate(glBlendOperation(attachment.operation), glBlendOperation(attachment.alphaOperation));
				glColorMask(attachment.writeMask & 1, attachment.writeMask & 2, attachment.writeMask & 4, attachment.writeMask & 8);
			};
			for (uint32_t i = 0; i < program.pass.colorOutputCount; ++i) apply(state.attachments[i], i);
			glcache.InvalidateBlendState();
			if (state.stencilEnabled) {
				auto& stencil = stencilTargets[{outputWidth, outputHeight}];
				if (!stencil.buffer) {
					glGenRenderbuffers(1, &stencil.buffer); glBindRenderbuffer(GL_RENDERBUFFER, stencil.buffer);
					glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, outputWidth, outputHeight);
				}
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil.buffer);
				if (!stencil.initialized || stencil.frame != frame.frameCount || stencil.layerIndex != program.pass.layerIndex) {
					glcache.StencilMask(255); glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT); stencil.frame = frame.frameCount; stencil.layerIndex = program.pass.layerIndex; stencil.initialized = true;
				}
				const GLenum operations[] { GL_ZERO, GL_KEEP, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
				glcache.Enable(GL_STENCIL_TEST); glcache.StencilMask(state.stencilWriteMask);
				glcache.StencilFunc(GL_NEVER + static_cast<GLenum>(state.stencilComparison), state.stencilReference, state.stencilReadMask);
				glcache.StencilOp(operations[static_cast<unsigned>(state.stencilFail)], operations[static_cast<unsigned>(state.stencilDepthFail)], operations[static_cast<unsigned>(state.stencilPass)]);
			}
		}
		glcache.UseProgram(program.handle);
		for (const ResourceUniform& resource : program.resources) {
			GLuint texture = inputTexture;
			const LookupTexture *lookup = nullptr;
			bool resourceHasMipmaps = false;
			if ((resource.kind == postprocess::ShaderInputKind::PassOutput
						|| resource.kind == postprocess::ShaderInputKind::History
						|| resource.kind == postprocess::ShaderInputKind::Feedback)
					&& resource.sourcePass != postprocess::InvalidShaderPass
					&& (resource.sourcePass >= targets.size()
							|| targets[resource.sourcePass].empty()
							|| resource.sourceOutput >= targets[resource.sourcePass].front()->outputs.size())) {
				error = "OpenGL shader graph references an invalid pass output.";
				postprocess::reportShaderChainStatus(activePath, error);
				disableFramebufferSrgb();
				return nullptr;
			}
			switch (resource.kind) {
			case postprocess::ShaderInputKind::Source:
				resourceHasMipmaps = inputHasMipmaps;
				break;
			case postprocess::ShaderInputKind::Original:
				texture = originalTexture;
				break;
			case postprocess::ShaderInputKind::Depth:
				texture = depthTexture;
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				MrtTarget *sourceTarget = targets[resource.sourcePass]
						[frame.frameCount % programs[resource.sourcePass].historyDepth].get();
				if (!sourceTarget->initialized || sourceTarget->renderedFrame != frame.frameCount) {
					error = "OpenGL shader pass input was not rendered for the current frame.";
					postprocess::reportShaderChainStatus(activePath, error);
					disableFramebufferSrgb();
					return nullptr;
				}
				texture = sourceTarget->outputs[resource.sourceOutput]->getTexture();
				resourceHasMipmaps = programs[resource.sourcePass].outputNeedsMipmaps[resource.sourceOutput];
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = resource.kind == postprocess::ShaderInputKind::Feedback ? 1 : resource.historyOffset;
				if (frame.frameCount < offset) {
					texture = program.nativeFxStages && resource.sourcePass != postprocess::InvalidShaderPass
							? targets[resource.sourcePass][programs[resource.sourcePass].historyDepth - 1]->outputs[resource.sourceOutput]->getTexture() : originalTexture;
					break;
				}
				const uint32_t requestedFrame = frame.frameCount - offset;
				if (resource.sourcePass == postprocess::InvalidShaderPass) {
					const HistoryTarget& history = sourceHistory[requestedFrame % sourceHistoryDepth];
					texture = history.initialized && history.renderedFrame == requestedFrame
							? history.framebuffer->getTexture() : originalTexture;
				}
				else {
					const uint32_t sourceDepth = programs[resource.sourcePass].historyDepth;
					const MrtTarget *history = targets[resource.sourcePass][requestedFrame % sourceDepth].get();
					texture = history->initialized && history->renderedFrame == requestedFrame
							? history->outputs[resource.sourceOutput]->getTexture() : program.nativeFxStages ? history->outputs[resource.sourceOutput]->getTexture() : originalTexture;
					resourceHasMipmaps = history->initialized && history->renderedFrame == requestedFrame
							&& programs[resource.sourcePass].outputNeedsMipmaps[resource.sourceOutput];
				}
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				if (resource.lookupIndex >= lookupTextures.size()) {
					disableFramebufferSrgb();
					return nullptr;
				}
				lookup = &lookupTextures[resource.lookupIndex];
				texture = lookup->texture;
				resourceHasMipmaps = lookup->config.useMipmaps;
				break;
			}
			glActiveTexture(GL_TEXTURE0 + resource.unit);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(resource.textureTarget, texture);
			if (texture == sourceTexture)
				sourceTextureState.capture();
			const bool linearFilter = lookup != nullptr ? lookup->config.linearFilter
					: resource.hasSamplerState ? resource.linearFilter : program.pass.linearFilter;
			const bool sampleMipmaps = resourceHasMipmaps && (lookup != nullptr ? lookup->config.useMipmaps
					: resource.hasSamplerState ? resource.useMipmaps : true);
			const postprocess::ShaderWrapMode wrapMode = lookup != nullptr ? lookup->config.wrapMode
					: resource.hasSamplerState ? resource.wrapMode : program.pass.wrapMode;
			glcache.TexParameteri(resource.textureTarget, GL_TEXTURE_MIN_FILTER,
					sampleMipmaps ? (linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
							: linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(resource.textureTarget, GL_TEXTURE_MAG_FILTER,
					linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(resource.textureTarget, GL_TEXTURE_WRAP_S, glWrapMode(wrapMode));
			glcache.TexParameteri(resource.textureTarget, GL_TEXTURE_WRAP_T, glWrapMode(wrapMode));
#ifndef GLES2
			if (glBindSampler != nullptr)
				glBindSampler(resource.unit, resource.sampler == 0 ? 0
						: sampleMipmaps ? resource.mipSampler : resource.sampler);
#endif
			if (resource.location != -1)
				glUniform1i(resource.location, static_cast<GLint>(resource.unit));
		}
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		if (program.sourceSizeUniform != -1)
			glUniform2f(program.sourceSizeUniform, static_cast<float>(passFrame.sourceWidth), static_cast<float>(passFrame.sourceHeight));
		if (program.originalSizeUniform != -1)
			glUniform2f(program.originalSizeUniform,
					static_cast<float>(program.displayGlsl ? frame.sourceWidth : originalWidth),
					static_cast<float>(program.displayGlsl ? frame.sourceHeight : originalHeight));
		if (program.outputSizeUniform != -1)
			glUniform2f(program.outputSizeUniform, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
		if (program.finalViewportSizeUniform != -1)
			glUniform2f(program.finalViewportSizeUniform, static_cast<float>(frame.viewportWidth), static_cast<float>(frame.viewportHeight));
		if (program.hasDepthUniform != -1)
			glUniform1i(program.hasDepthUniform, depthTexture != 0 ? 1 : 0);
		if (program.frameCountUniform != -1)
			glUniform1i(program.frameCountUniform, static_cast<GLint>(program.pass.frameCountModulo == 0
					? frame.frameCount : frame.frameCount % program.pass.frameCountModulo));
		if (program.rotationUniform != -1)
			glUniform1i(program.rotationUniform, frame.rotationPending ? 1 : 0);
		for (const ParameterUniform& parameter : program.parameters) {
			if (parameter.location == -1)
				continue;
			switch (parameter.type) {
			case postprocess::SlangParameterType::Float: glUniform1f(parameter.location, parameter.values[0]); break;
			case postprocess::SlangParameterType::Int: glUniform1i(parameter.location, static_cast<GLint>(parameter.values[0])); break;
			case postprocess::SlangParameterType::Bool: glUniform1i(parameter.location, parameter.values[0] != 0.f); break;
			case postprocess::SlangParameterType::Float2: glUniform2fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3: glUniform3fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4: glUniform4fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float2x2: glUniformMatrix2fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3x3: glUniformMatrix3fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4x4: glUniformMatrix4fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
#ifndef GLES2
			case postprocess::SlangParameterType::Float2x3: glUniformMatrix2x3fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float2x4: glUniformMatrix2x4fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3x2: glUniformMatrix3x2fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3x4: glUniformMatrix3x4fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4x2: glUniformMatrix4x2fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4x3: glUniformMatrix4x3fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
#endif
			}
		}

#ifdef GL_COMPUTE_SHADER
		for (uint32_t unit = 0; unit < program.pass.storageBindings.size(); ++unit) {
			const auto& binding = program.pass.storageBindings[unit];
			postprocess::ShaderPass formatPass; formatPass.targetFormatName = program.pass.outputs[binding.second].targetFormatName;
			GLenum format, pixels, type; if (!glTargetFormat(formatPass, format, pixels, type, error)) return nullptr;
			const auto& image = target->outputs[binding.second];
			glBindImageTexture(unit, image->getTexture(), 0, image->target == GL_TEXTURE_3D || image->target == GL_TEXTURE_2D_ARRAY, 0, GL_READ_WRITE, format);
			const GLint location = glGetUniformLocation(program.handle, binding.first.c_str()); if (location >= 0) glUniform1i(location, unit);
		}
		if (program.pass.compute) {
			std::array<uint32_t, 3> groups;
			if (!postprocess::resolveShaderDispatchGroups(program.pass, passFrame, groups, error)) {
				postprocess::reportShaderChainStatus(activePath, error); return nullptr;
			}
			glDispatchCompute(groups[0], groups[1], groups[2]);
		} else
#endif
		{
			vertexArray.bind(vertexBuffer.get());
			const GLenum topologies[] { GL_POINTS, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP };
			glDrawArrays(program.nativeFxStages ? topologies[static_cast<unsigned>(program.pass.renderState.topology)] : GL_TRIANGLE_STRIP,
					0, program.nativeFxStages ? program.pass.renderState.vertexCount : 4);
			GlVertexArray::unbind();
		}
#ifdef GL_COMPUTE_SHADER
		if (!program.pass.storageBindings.empty()) {
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
			for (uint32_t unit = 0; unit < program.pass.storageBindings.size(); ++unit) glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F);
		}
#endif
#ifndef GLES2
		if (glBindSampler != nullptr)
			for (const ResourceUniform& resource : program.resources)
				glBindSampler(resource.unit, 0);
#endif
		for (uint32_t outputIndex = 0; outputIndex < target->outputs.size(); ++outputIndex) {
			if (program.outputNeedsMipmaps[outputIndex] && program.pass.renderState.generateMipmaps) {
				glActiveTexture(GL_TEXTURE0);
				glcache.InvalidateTextureBinding();
				const auto& image = *target->outputs[outputIndex];
				glBindTexture(image.target, image.getTexture());
				glGenerateMipmap(image.target);
			}
		}

		target->renderedFrame = frame.frameCount;
		target->initialized = true;
		if (program.pass.updatesSource) {
		inputTexture = target->outputs[0]->getTexture();
		inputHasMipmaps = program.outputNeedsMipmaps[0];
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
		output = target->outputs[0]->framebuffer.get();
		}
	}
	disableFramebufferSrgb();
	glCheck();
	presentationTransformApplied = composedDisplayInput;
	return output;
}

void GlShaderChain::term()
{
	if (transferReadFramebuffer) glDeleteFramebuffers(1, &transferReadFramebuffer);
	if (transferDrawFramebuffer) glDeleteFramebuffers(1, &transferDrawFramebuffer);
	transferReadFramebuffer = transferDrawFramebuffer = 0;
	deletePrograms(programs);
	deleteLookupTextures();
	targets.clear();
	stencilTargets.clear();
	displayInput.reset();
	depthInput.reset();
	mipmapInput.reset();
	sourceHistory.clear();
	vertexBuffer.reset();
	if (depthCopyProgram != 0)
		glcache.DeleteProgram(depthCopyProgram);
	depthCopyProgram = 0;
	depthCopySourceUniform = depthCopyScaleUniform = depthCopyRotateUniform = -1;
	if (copyProgram != 0)
		glcache.DeleteProgram(copyProgram);
	if (copyRotateProgram != 0)
		glcache.DeleteProgram(copyRotateProgram);
	copyProgram = 0;
	copyRotateProgram = 0;
	copySourceUniform = -1;
	copyRotateSourceUniform = -1;
	vertexArray.term();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	postprocess::reportShaderChainStatus({}, {});
}
