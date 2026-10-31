/*
	Portions Copyright 2026 The Hollycast Authors

	SPDX-License-Identifier: GPL-2.0-or-later
*/
#if defined(USE_OPENGL) && defined(USE_SDL) && !defined(LIBRETRO) && !defined(GLES)
#include "rend/gles/gl_shader_chain.h"
#include "cfg/option.h"

#include <SDL.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

class OpenGLShaderChainTest : public testing::Test {
protected:
	std::filesystem::path directory = std::filesystem::temp_directory_path()
			/ ("hollycast_gl_shaders_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::vector<std::string> previousPaths = config::PostProcessShaders.get();
	std::string previousPath = config::PostProcessShader.get();
	std::string previousDirectory = config::PostProcessShaderDirectory.get();
	std::string previousTechnique = config::PostProcessShaderTechnique.get();
	int previousMajor = gl.gl_major, previousMinor = gl.gl_minor;
	bool previousGles = gl.is_gles;
	const char *previousVersion = gl.gl_version, *previousHeader = gl.glsl_version_header;
	SDL_Window *window = nullptr;
	SDL_GLContext context = nullptr;
	bool videoInitialized = false;

	void SetUp() override {
		// Headless CI still runs compiler tests. Opting in makes context creation
		// mandatory, so a missing GPU cannot silently turn a pixel check into a pass.
		if (std::getenv("HOLLYCAST_SHADER_GL_TESTS") == nullptr)
			GTEST_SKIP() << "Set HOLLYCAST_SHADER_GL_TESTS=1 to run OpenGL pixel checks.";
		ASSERT_EQ(0, SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
		videoInitialized = true;
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
		window = SDL_CreateWindow("Shader regression", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
				16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
		ASSERT_NE(nullptr, window) << SDL_GetError();
		context = SDL_GL_CreateContext(window);
		ASSERT_NE(nullptr, context) << SDL_GetError();
		ASSERT_NE(0, gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)));
		// Drivers may return a newer context. Exercise the 4.1 language selection
		// used on macOS even when this machine also supports GLSL 4.30.
		gl.gl_major = 4; gl.gl_minor = 1; gl.is_gles = false;
		gl.gl_version = "GL3"; gl.glsl_version_header = "#version 130";
		glcache.Reset();
		std::filesystem::create_directories(directory);
		config::PostProcessShaders.get().clear();
		config::PostProcessShader.get().clear();
		config::PostProcessShaderTechnique.get().clear();
		config::PostProcessShaderDirectory.get() = directory.string();
	}

	void TearDown() override {
		config::PostProcessShaders.get() = previousPaths;
		config::PostProcessShader.get() = previousPath;
		config::PostProcessShaderDirectory.get() = previousDirectory;
		config::PostProcessShaderTechnique.get() = previousTechnique;
		gl.gl_major = previousMajor; gl.gl_minor = previousMinor; gl.is_gles = previousGles;
		gl.gl_version = previousVersion; gl.glsl_version_header = previousHeader;
		glcache.Reset();
		if (context != nullptr) SDL_GL_DeleteContext(context);
		if (window != nullptr) SDL_DestroyWindow(window);
		if (videoInitialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	std::string writeShader(const char *name, const std::string& source) {
		const auto path = directory / name;
		std::ofstream(path) << source;
		return path.string();
	}

	void expectPixels(const std::vector<std::string>& paths, const std::array<int, 4>& expected) {
		postprocess::ShaderChain chain;
		std::string error;
		ASSERT_TRUE(postprocess::loadShaderChain(paths, chain, error)) << error;
		GlShaderChain renderer;
		const postprocess::ShaderFrameInfo frame { 8, 8, 8, 8 };
		bool transformed = false;
		// Synchronize the empty configuration before activating this test's graph.
		renderer.render(0, frame, transformed);
		ASSERT_TRUE(renderer.activate(chain, error)) << error;
		GlFramebuffer input(8, 8, false, true);
		input.bind();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glcache.ClearColor(0.f, 0.f, 1.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		GlFramebuffer *output = renderer.render(input.getTexture(), frame, transformed);
		ASSERT_NE(nullptr, output) << postprocess::getShaderChainStatus().error;
		output->bind();
		std::array<unsigned char, 4> pixel {};
		glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
		for (unsigned channel = 0; channel < pixel.size(); ++channel)
			EXPECT_NEAR(expected[channel], pixel[channel], 1) << "channel " << channel;
		EXPECT_EQ(static_cast<GLenum>(GL_NO_ERROR), glGetError());
	}
};

TEST_F(OpenGLShaderChainTest, RendersDefaultFxAndIsolatesPassMasks)
{
	const auto numeric = writeShader("numeric.fx",
			"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target"
			" { return float4(0.125f, 0.25F, 0.5l, 1.0L); }\n"
			"technique Numeric { pass { PixelShader = Pixel; } }\n");
	expectPixels({ numeric }, { 32, 64, 128, 255 });
	const auto red = writeShader("red.fx",
			"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target"
			" { return float4(1.0, 0.0, 0.0, 1.0); }\n"
			"technique Red { pass { PixelShader = Pixel; ColorWriteMask = 1; } }\n");
	const auto green = writeShader("green.glsl", "void main() { FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
	const auto greenMask = writeShader("green.fx",
			"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target"
			" { return float4(0.0, 1.0, 0.0, 1.0); }\n"
			"technique Green { pass { PixelShader = Pixel; ColorWriteMask = 2; } }\n");
	expectPixels({ green }, { 0, 255, 0, 255 });
	expectPixels({ red }, { 255, 0, 255, 255 });
	expectPixels({ red, green }, { 0, 255, 0, 255 });
	// The second masked FX must seed all channels from the first pass's output.
	expectPixels({ red, greenMask }, { 255, 255, 255, 255 });
}

} // namespace
#endif
