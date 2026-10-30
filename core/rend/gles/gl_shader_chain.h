/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include "gles.h"
#include "rend/postprocess/shader_chain.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Executes the portable chain data on OpenGL. A GLSL pass is a fragment-shader
// body: HollyCast supplies Source, SourceSize, OutputSize, FrameCount, and the
// fullscreen vertex stage so package authors do not need backend boilerplate.
class GlShaderChain
{
public:
	~GlShaderChain();

	// A candidate replaces the active chain only after every pass has compiled.
	// The previous chain remains available when loading or compilation fails.
	bool activate(const postprocess::ShaderChain& chain, std::string& error);
	GlFramebuffer *render(GLuint sourceTexture, const postprocess::ShaderFrameInfo& frame,
			bool& presentationTransformApplied, GLuint depthTexture = 0, int depthWidth = 0, int depthHeight = 0);
	bool hasActiveChain() const { return !programs.empty(); }
	void term();

private:
	class VertexArray final : public GlVertexArray
	{
	protected:
		void defineVtxAttribs() override;
	};

	struct ParameterUniform {
		GLint location = -1;
		std::string name;
		uint32_t layerIndex = 0;
		postprocess::SlangParameterType type = postprocess::SlangParameterType::Float;
		std::array<float, 16> values {};
	};
	struct ResourceUniform {
		GLint location = -1;
		postprocess::ShaderInputKind kind = postprocess::ShaderInputKind::Source;
		uint32_t sourcePass = postprocess::InvalidShaderPass;
		uint32_t sourceOutput = 0;
		uint32_t historyOffset = 1;
		std::string resourceName;
		uint32_t lookupIndex = postprocess::InvalidShaderPass;
		GLuint unit = 0;
		bool hasSamplerState = false;
		bool linearFilter = false;
		bool useMipmaps = false;
		postprocess::ShaderWrapMode wrapMode = postprocess::ShaderWrapMode::ClampToEdge;
		GLuint sampler = 0;
		GLuint mipSampler = 0;
		GLenum textureTarget = GL_TEXTURE_2D;
	};
	struct LookupTexture {
		postprocess::ShaderPresetTexture config;
		GLuint texture = 0;
	};
	struct Program {
		GLuint handle = 0;
		postprocess::ShaderPass pass;
		GLint sourceUniform = -1;
		GLint sourceSizeUniform = -1;
		GLint originalSizeUniform = -1;
		GLint outputSizeUniform = -1;
		GLint finalViewportSizeUniform = -1;
		GLint hasDepthUniform = -1;
		GLint frameCountUniform = -1;
		GLint rotationUniform = -1;
		bool displayGlsl = false;
		bool nativeFxStages = false;
		std::vector<ParameterUniform> parameters;
		std::vector<ResourceUniform> resources;
		std::vector<bool> outputNeedsMipmaps;
		uint32_t historyDepth = 1;
		int targetWidth = 0;
		int targetHeight = 0;
	};
	struct OutputTarget {
		std::unique_ptr<GlFramebuffer> framebuffer;
		GLuint texture = 0;
		GLenum target = GL_TEXTURE_2D;
		int width = 0, height = 0;
		uint32_t depth = 1;
		std::string formatName;
		~OutputTarget() { if (texture) glcache.DeleteTextures(1, &texture); }
		GLuint getTexture() const { return framebuffer ? framebuffer->getTexture() : texture; }
		int getWidth() const { return width; }
		int getHeight() const { return height; }
	};
	struct StencilTarget {
		GLuint buffer = 0;
		uint32_t frame = 0, layerIndex = postprocess::InvalidShaderPass;
		bool initialized = false;
		~StencilTarget() { if (buffer) glDeleteRenderbuffers(1, &buffer); }
	};
	std::map<std::pair<int, int>, StencilTarget> stencilTargets;
	std::vector<postprocess::ShaderParameter> hostParameters;
	struct MrtTarget {
		std::vector<std::unique_ptr<OutputTarget>> outputs;
		GLuint framebuffer = 0;
		uint32_t renderedFrame = 0;
		bool initialized = false;
		~MrtTarget() {
			if (framebuffer != 0)
				glDeleteFramebuffers(1, &framebuffer);
		}
	};
	struct HistoryTarget {
		std::unique_ptr<GlFramebuffer> framebuffer;
		uint32_t renderedFrame = 0;
		bool initialized = false;
	};
	using MrtTargetRing = std::vector<std::unique_ptr<MrtTarget>>;

	GLuint transferReadFramebuffer = 0, transferDrawFramebuffer = 0;
	bool copyOutput(const OutputTarget& source, OutputTarget& destination, std::string& error);
	bool clearOutput(OutputTarget& destination, std::string& error);
	bool ensureResources();
	bool ensureMrtTarget(MrtTarget& target, const postprocess::ShaderPass& pass, int width, int height, std::string& error);
	bool ensureLookupTextures(std::string& error);
	bool ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error);
	void deleteLookupTextures();
	void deleteLookupTextures(std::vector<LookupTexture>& textures);
	void updateConfiguredChain();
	bool compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate, std::string& error);
	static void deletePrograms(std::vector<Program>& programs);

	std::array<int, 4> stencilFrameSize {};
	std::vector<Program> programs;
	std::vector<std::string> configuredPaths;
	uint64_t configuredGeneration = 0;
	std::string activePath;
	std::vector<MrtTargetRing> targets;
	std::unique_ptr<GlFramebuffer> displayInput;
	std::unique_ptr<GlFramebuffer> depthInput;
	std::unique_ptr<GlFramebuffer> mipmapInput;
	std::vector<HistoryTarget> sourceHistory;
	uint32_t sourceHistoryDepth = 1;
	// Count completed display-chain frames, independently of PVR render-to-texture work.
	uint32_t renderedFrames = 0;
	bool needsSourceHistory = false;
	uint64_t parameterGeneration = 0;
	std::vector<LookupTexture> lookupTextures;
	std::unique_ptr<GlBuffer> vertexBuffer;
	GLuint depthCopyProgram = 0;
	GLint depthCopySourceUniform = -1;
	GLint depthCopyScaleUniform = -1;
	GLint depthCopyRotateUniform = -1;
	GLuint copyProgram = 0;
	GLuint copyRotateProgram = 0;
	GLint copySourceUniform = -1;
	GLint copyRotateSourceUniform = -1;
	VertexArray vertexArray;
};
