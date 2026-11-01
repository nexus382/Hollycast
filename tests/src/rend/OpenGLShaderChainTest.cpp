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

	void expectPixels(const std::vector<std::string>& paths, const std::array<int, 4>& expected,
			const postprocess::ShaderFrameInfo& frame = { 8, 8, 8, 8 }, bool expectComposed = false) {
		postprocess::ShaderChain chain;
		std::string error;
		ASSERT_TRUE(postprocess::loadShaderChain(paths, chain, error)) << error;
		GlShaderChain renderer;
		bool transformed = false;
		// Synchronize the empty configuration before activating this test's graph.
		renderer.render(0, frame, transformed);
		ASSERT_TRUE(renderer.activate(chain, error)) << error;
		GlFramebuffer input(frame.sourceWidth, frame.sourceHeight, false, true);
		input.bind();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glcache.ClearColor(0.f, 0.f, 1.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		const GLboolean srgbEnabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
		GlFramebuffer *output = renderer.render(input.getTexture(), frame, transformed);
		ASSERT_NE(nullptr, output) << postprocess::getShaderChainStatus().error;
		EXPECT_EQ(expectComposed, transformed);
		EXPECT_EQ(srgbEnabled, glIsEnabled(GL_FRAMEBUFFER_SRGB));
		EXPECT_EQ(frame.viewportWidth, output->getWidth());
		EXPECT_EQ(frame.viewportHeight, output->getHeight());
		output->bind();
		std::array<unsigned char, 4> pixel {};
		glReadPixels(output->getWidth() / 2, output->getHeight() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
		for (unsigned channel = 0; channel < pixel.size(); ++channel)
			EXPECT_NEAR(expected[channel], pixel[channel], 1) << "channel " << channel;
		EXPECT_EQ(static_cast<GLenum>(GL_NO_ERROR), glGetError());
	}
};

TEST_F(OpenGLShaderChainTest, CopiesGameInputBeforeGeneratingMipmaps)
{
	writeShader("mipmap.glsl", "void main() { FragColor = textureLod(Source, vTexCoord, 1.0); }\n");
	const auto preset = writeShader("mipmap.glslp", "shaders = 1\nshader0 = mipmap.glsl\nmipmap_input0 = true\n");
	expectPixels({ preset }, { 0, 0, 255, 255 });
}

TEST_F(OpenGLShaderChainTest, PreservesLinearColorThroughSrgbIntermediate)
{
	writeShader("color.glsl", "void main() { FragColor = vec4(0.25, 0.5, 0.75, 1.0); }\n");
	writeShader("copy.glsl", "void main() { FragColor = texture(Source, vTexCoord); }\n");
	const auto preset = writeShader("srgb.glslp",
			"shaders = 2\nshader0 = color.glsl\nsrgb_framebuffer0 = true\nshader1 = copy.glsl\n");
	// The pass must encode its sRGB attachment and restore either caller state.
	glDisable(GL_FRAMEBUFFER_SRGB);
	expectPixels({ preset }, { 64, 128, 191, 255 });
	glEnable(GL_FRAMEBUFFER_SRGB);
	expectPixels({ preset }, { 64, 128, 191, 255 });
}

TEST_F(OpenGLShaderChainTest, PresentsComposedInputForAuxiliaryOnlyEffect)
{
	const auto effect = writeShader("auxiliary.fx",
			"texture2D Auxiliary { Width = 4; Height = 4; Format = RGBA8; };\n"
			"float4 Pixel() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n"
			"technique AuxiliaryOnly { pass { PixelShader = Pixel; RenderTarget = Auxiliary; } }\n");
	postprocess::ShaderFrameInfo frame { 8, 8, 16, 8 };
	frame.rotationPending = true;
	expectPixels({ effect }, { 0, 0, 255, 255 }, frame, true);
	const auto green = writeShader("green.glsl", "void main() { FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
	expectPixels({ green, effect }, { 0, 255, 0, 255 }, frame, true);
}

TEST_F(OpenGLShaderChainTest, AppliesVolumeSamplerWrapToDepthAxis)
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GLContext computeContext = SDL_GL_CreateContext(window);
	if (computeContext == nullptr)
		GTEST_SKIP() << "OpenGL 4.3 is required for the volume compute pass: " << SDL_GetError();
	SDL_GL_DeleteContext(context);
	context = computeContext;
	ASSERT_NE(0, gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)));
	gl.gl_major = 4; gl.gl_minor = 3;
	// Use an effect output so this exercises sampler objects, not the lookup
	// texture's own wrap settings. The two depth slices contain different colors.
	const auto effect = writeShader("volume.fx",
			"texture3D Volume { Width = 1; Height = 1; Depth = 2; Format = RGBA8; };\n"
			"storage3D WriteVolume { Texture = Volume; };\n"
			"sampler3D VolumeSampler { Texture = Volume; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;"
			" AddressU = CLAMP; AddressV = CLAMP; AddressW = CLAMP; };\n"
			"[numthreads(1,1,1)] void Fill(uint3 id : SV_DispatchThreadID)"
			" { tex3Dstore(WriteVolume, int3(id), float4(0.0, 0.0, id.z, 1.0)); }\n"
			"float4 Pixel() : SV_Target { return tex3D(VolumeSampler, float3(0.25, 0.25, 1.25)); }\n"
			"technique VolumeEdge { pass Write { ComputeShader = Fill; DispatchSizeX = 1; DispatchSizeY = 1; DispatchSizeZ = 2; }"
			" pass Present { PixelShader = Pixel; } }\n");
	expectPixels({ effect }, { 0, 0, 255, 255 });
}

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

TEST_F(OpenGLShaderChainTest, LinksFxVaryingsBySemanticLocation)
{
	const auto parameters = writeShader("parameters.fx",
			"void Vertex(uint id : SV_VertexID, out float4 position : SV_Position,"
			" out float2 first : TEXCOORD0, out float2 second : TEXCOORD1) {"
			" position = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);"
			" first = float2(0.25, 0.5); second = float2(0.75, 1.0); }\n"
			"float4 Pixel(float2 left : TEXCOORD0, float2 right : TEXCOORD1) : SV_Target"
			" { return float4(left, right); }\n"
			"technique Parameters { pass { VertexShader = Vertex; PixelShader = Pixel; } }\n");
	expectPixels({ parameters }, { 64, 128, 191, 255 });
	const auto structure = writeShader("structure.fx",
			"struct VertexOutput { float4 position : SV_Position; float2 coordinates : TEXCOORD0; };\n"
			"VertexOutput Vertex(uint id : SV_VertexID) { VertexOutput result;"
			" result.position = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);"
			" result.coordinates = float2(0.25, 0.75); return result; }\n"
			"float4 Pixel(float2 uv : TEXCOORD0) : SV_Target { return float4(uv, 0.5, 1.0); }\n"
			"technique Structure { pass { VertexShader = Vertex; PixelShader = Pixel; } }\n");
	expectPixels({ structure }, { 64, 191, 128, 255 });
}

TEST_F(OpenGLShaderChainTest, RendersPortableAndFxStagesWithGlesPrecision)
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
	SDL_GLContext esCompatibleContext = SDL_GL_CreateContext(window);
	if (esCompatibleContext == nullptr)
		GTEST_SKIP() << "OpenGL 4.5 ES 3.1 shader compatibility is unavailable: " << SDL_GetError();
	SDL_GL_DeleteContext(context);
	context = esCompatibleContext;
	ASSERT_NE(0, gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress)));
	GLint major = 0, minor = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	// OpenGL 4.5 includes ES 3.1 shader compatibility. Use its actual shader
	// compiler/pixel output here; older desktop contexts cannot run ESSL 3.10.
	if (major < 4 || (major == 4 && minor < 5))
		GTEST_SKIP() << "OpenGL 4.5 ES 3.1 shader compatibility is required.";
	gl.is_gles = true;
	gl.gl_major = 3; gl.gl_minor = 1;
	gl.gl_version = "GLES3"; gl.glsl_version_header = "#version 300 es";
	const auto portable = writeShader("precision.glsl",
			"#pragma parameter GAIN \"Gain\" 0.25 0.0 1.0 0.05\n"
			"void main() { vec4 color = texture(Source, vTexCoord); float gain = GAIN;"
			" FragColor = vec4(gain, color.b * 0.5, 0.125, 1.0); }\n");
	expectPixels({ portable }, { 64, 128, 32, 255 });
	const auto effect = writeShader("precision.fx",
			"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target"
			" { return float4(0.125, 0.25, 0.5, 1.0); }\n"
			"technique Numeric { pass { PixelShader = Pixel; } }\n");
	expectPixels({ effect }, { 32, 64, 128, 255 });
	const auto signedInput = writeShader("signed-vertex.fx",
			"float4 Vertex(int id : SV_VertexID) : SV_Position {"
			" return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0); }\n"
			"float4 Pixel() : SV_Target { return float4(0.5, 0.25, 0.125, 1.0); }\n"
			"technique Signed { pass { VertexShader = Vertex; PixelShader = Pixel; } }\n");
	expectPixels({ signedInput }, { 128, 64, 32, 255 });
	const auto structuredInput = writeShader("structured-vertex.fx",
			"struct Input { uint id : SV_VertexID; };\n"
			"float4 Vertex(Input input) : SV_Position { return float4(input.id == 2 ? 3.0 : -1.0,"
			" input.id == 1 ? 3.0 : -1.0, 0.0, 1.0); }\n"
			"float4 Pixel() : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }\n"
			"technique Structured { pass { VertexShader = Vertex; PixelShader = Pixel; } }\n");
	expectPixels({ structuredInput }, { 64, 128, 191, 255 });
	const auto scalarSwizzles = writeShader("scalar-swizzles.fx",
			"uniform float Gain = 0.25;\n"
			"float4 Pixel() : SV_Target { float value = Gain; value.x = value.x * 2.0;"
			" uint index = 1u; bool enabled = true;"
			" float4 color = mad(value.xxxx, 1.0, 0.0); uint2 indices = index.xx; bool2 flags = enabled.xx;"
			" return float4(color.x, Gain.xx.x, indices.y * 0.125, flags.y); }\n"
			"technique Scalar { pass { PixelShader = Pixel; } }\n");
	expectPixels({ scalarSwizzles }, { 128, 64, 32, 255 });

	const auto library = directory / "bundled";
	std::string error;
	ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
	config::PostProcessShaderDirectory.get() = library.string();
	unsigned selectable = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(library)) {
		if (!entry.is_regular_file() || postprocess::detectFormat(entry.path().string()) == postprocess::ShaderFormat::Unknown)
			continue;
		SCOPED_TRACE(entry.path().string());
		postprocess::ShaderChain chain;
		if (!postprocess::loadShaderChain(entry.path().string(), chain, error)) {
			ADD_FAILURE() << error;
			continue;
		}
		GlShaderChain renderer;
		EXPECT_TRUE(renderer.activate(chain, error)) << error;
		++selectable;
	}
	EXPECT_EQ(36u, selectable);
}

} // namespace
#endif
