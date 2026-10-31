/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "rend/postprocess/shader_chain.h"
#include "rend/postprocess/reshade_fx.h"
#include "cfg/option.h"
#include "stdclass.h"
#if defined(USE_VULKAN)
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/vulkan_shader_chain.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <regex>
#include <sstream>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#ifdef HAVE_D3D11
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace {

bool compileHlsl(const std::string& source, const char *target, std::string& error, D3D11_SHADER_VARIABLE_DESC *parameterLayout = nullptr)
{
	using CompileFunction = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
			LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
	const HMODULE compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
	if (compilerModule == nullptr) {
		error = "d3dcompiler_47.dll is unavailable.";
		return false;
	}
	const auto compiler = reinterpret_cast<CompileFunction>(GetProcAddress(compilerModule, "D3DCompile"));
	ID3DBlob *shader = nullptr;
	ID3DBlob *diagnostics = nullptr;
	const HRESULT result = compiler == nullptr ? E_NOINTERFACE : compiler(source.data(), source.size(), nullptr,
			nullptr, nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &diagnostics);
	if (FAILED(result))
		error = diagnostics != nullptr ? static_cast<const char *>(diagnostics->GetBufferPointer())
				: "D3DCompile failed without diagnostics.";
	if (SUCCEEDED(result) && parameterLayout != nullptr) {
		using ReflectFunction = HRESULT (WINAPI *)(LPCVOID, SIZE_T, REFIID, void **);
		const auto reflect = reinterpret_cast<ReflectFunction>(GetProcAddress(compilerModule, "D3DReflect"));
		ID3D11ShaderReflection *reflection = nullptr;
		if (reflect != nullptr && SUCCEEDED(reflect(shader->GetBufferPointer(), shader->GetBufferSize(),
				__uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(&reflection)))) {
			D3D11_SHADER_DESC shaderDescription {};
			reflection->GetDesc(&shaderDescription);
			for (UINT buffer = 0; buffer < shaderDescription.ConstantBuffers; ++buffer) {
				auto constants = reflection->GetConstantBufferByIndex(buffer);
				D3D11_SHADER_BUFFER_DESC description {};
				constants->GetDesc(&description);
				for (UINT variable = 0; variable < description.Variables; ++variable) {
					D3D11_SHADER_VARIABLE_DESC value {};
					constants->GetVariableByIndex(variable)->GetDesc(&value);
					if (value.Name != nullptr && std::string(value.Name).find("Parameters") != std::string::npos) {
						*parameterLayout = value;
						parameterLayout->Name = nullptr; // Reflection owns this string.
					}
				}
			}
			reflection->Release();
		}
	}
	if (diagnostics != nullptr)
		diagnostics->Release();
	if (shader != nullptr)
		shader->Release();
	FreeLibrary(compilerModule);
	return SUCCEEDED(result);
}

} // namespace
#endif

static void expectHlslCompiles(const std::string& source, const char *target)
{
#ifdef _WIN32
	std::string error;
	const bool compiled = compileHlsl(source, target, error);
	if (!compiled) {
		if (const char *directory = std::getenv("HOLLYCAST_SHADER_DIAGNOSTICS")) {
			std::filesystem::create_directories(directory);
			const auto filename = std::filesystem::path(directory)
					/ (std::to_string(std::hash<std::string>{}(source)) + "." + target + ".hlsl");
			std::ofstream(filename) << source;
		}
	}
	EXPECT_TRUE(compiled) << error;
#else
	EXPECT_FALSE(source.empty());
#endif
}
TEST(PostProcessingChainTest, ParsesSlangPresetPassesAndParameters)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = shaders/scanlines.slang\n"
		"filter_linear0 = true\n"
		"scale_type0 = viewport\n"
		"scale_x0 = 2.0\n"
		"shader1 = shaders/blur.slang\n"
		"scale_type_x1 = source\n"
		"scale_type_y1 = absolute\n"
		"scale_x1 = 1.5\n"
		"scale_y1 = 720\n"
		"frame_count_mod1 = 4\n"
		"parameters = SCANLINE; BLOOM\n"
		"SCANLINE = 0.75\n"
		"BLOOM = 0.25\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangPreset(preset, "C:/Shaders/crt.slangp", chain, error)) << error;
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(2u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::Slang, chain.passes[0].format);
	EXPECT_TRUE(chain.passes[0].linearFilter);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[0].scaleTypeX);
	EXPECT_EQ(postprocess::ScaleType::Absolute, chain.passes[1].scaleTypeY);
	EXPECT_EQ(4u, chain.passes[1].frameCountModulo);
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_EQ("SCANLINE", chain.parameters[0].name);
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].values[0]);
	EXPECT_FLOAT_EQ(0.25f, chain.parameters[1].value);
	EXPECT_FLOAT_EQ(0.25f, chain.parameters[1].values[0]);
}

TEST(PostProcessingChainTest, RejectsUnsupportedPassFormat)
{
	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangPreset("shaders = 1\nshader0 = pass.glsl\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, ParsesAndSerializesShaderNumbersWithClassicLocale)
{
	class DecimalComma final : public std::numpunct<char> {
	protected:
		char do_decimal_point() const override { return ','; }
	};
	const std::locale previous = std::locale();
	std::locale::global(std::locale(previous, new DecimalComma));
	struct RestoreLocale {
		std::locale value;
		~RestoreLocale() { std::locale::global(value); }
	} restore { previous };

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangPreset(
			"shaders = 1\nshader0 = pass.slang\nscale_x0 = 1.5\nparameters = LEVEL\nLEVEL = 0.25\n",
			"C:/Shaders/test.slangp", chain, error)) << error;
	EXPECT_FLOAT_EQ(1.5f, chain.passes[0].scaleX);
	EXPECT_EQ("LEVEL=0.25", postprocess::serializeShaderChainParameterValues(chain.parameters));
}

TEST(PostProcessingChainTest, RejectsEscapedSymlinkedPresetStage)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory { std::filesystem::temp_directory_path()
			/ ("hollycast-shader-root-" + std::to_string(
					std::chrono::high_resolution_clock::now().time_since_epoch().count())) };
	const std::filesystem::path package = directory.path / "package";
	const std::filesystem::path outside = directory.path / "outside.slang";
	std::filesystem::create_directories(package);
	std::ofstream(outside) << "#version 450\n#pragma stage fragment\nvoid main() { gl_FragColor = vec4(1.0); }\n";
	std::error_code linkError;
	std::filesystem::create_symlink(outside, package / "escaped.slang", linkError);
	if (linkError)
		GTEST_SKIP() << "File symlinks are unavailable in this test environment.";

	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangPreset("shaders = 1\nshader0 = escaped.slang\n",
			(package / "test.slangp").string(), chain, error));
	EXPECT_NE(std::string::npos, error.find("package root"));
}

TEST(PostProcessingChainTest, EnforcesShaderTargetLimits)
{
	postprocess::ShaderPass pass;
	pass.scaleTypeX = postprocess::ScaleType::Absolute;
	pass.scaleTypeY = postprocess::ScaleType::Absolute;
	pass.scaleX = static_cast<float>(postprocess::MaxShaderTargetDimension + 1);
	pass.scaleY = 1.f;
	postprocess::ShaderFrameInfo frame { 640, 480, 1280, 960, 0, false };
	int width;
	int height;
	std::string error;
	EXPECT_FALSE(postprocess::resolveTargetSize(pass, frame, width, height, error));

	uint64_t bytes = 0;
	EXPECT_FALSE(postprocess::accumulateShaderTargetBytes(16384, 16384,
			postprocess::ShaderTargetFormat::Float, "R32G32B32A32_SFLOAT", true, 1, bytes, error));
}

TEST(PostProcessingChainTest, ParsesRetroArchGlslPreset)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = passes/first.glsl\n"
		"filter_linear0 = true\n"
		"scale_type0 = source\n"
		"shader1 = passes/final.glsl\n"
		"scale_type1 = viewport\n"
		"frame_count_mod1 = 8\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslPreset(preset, "C:/Shaders/crt.glslp", chain, error)) << error;
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(2u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::GLSL, chain.passes[0].format);
	EXPECT_NE(std::string::npos, chain.passes[0].sourcePath.find("passes/first.glsl"));
	EXPECT_TRUE(chain.passes[0].linearFilter);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[1].scaleTypeX);
	EXPECT_EQ(8u, chain.passes[1].frameCountModulo);
	EXPECT_EQ(postprocess::ShaderFormat::GlslPreset, postprocess::detectFormat("crt.glslp"));

	EXPECT_FALSE(postprocess::parseGlslPreset("shaders = 1\nshader0 = ../outside.glsl\n",
			"C:/Shaders/crt.glslp", chain, error));
	EXPECT_NE(std::string::npos, error.find("relative"));
}

TEST(PostProcessingChainTest, PreservesPresetScalingPrecedenceAndSamplingDefaults)
{
	const std::string preset =
		"shaders = 3\nshader0 = first.glsl\nshader1 = second.glsl\nshader2 = final.glsl\n"
		"scale0 = 2\nscale_x0 = 3\nscale_y0 = 4\nscale_type0 = source\nscale_type_x0 = absolute\n"
		"scale_x1 = 0.5\nwrap_mode1 = repeat\ntextures = LUT\nLUT = lookup.png\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslPreset(preset, "C:/Shaders/test.glslp", chain, error)) << error;
	ASSERT_EQ(3u, chain.passes.size());
	EXPECT_FLOAT_EQ(2.f, chain.passes[0].scaleX);
	EXPECT_FLOAT_EQ(2.f, chain.passes[0].scaleY);
	EXPECT_EQ(postprocess::ScaleType::Source, chain.passes[0].scaleTypeX);
	EXPECT_FLOAT_EQ(0.5f, chain.passes[1].scaleX);
	EXPECT_FLOAT_EQ(1.f, chain.passes[1].scaleY);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[2].scaleTypeX);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[2].scaleTypeY);
	EXPECT_EQ(postprocess::ShaderWrapMode::ClampToBorder, chain.passes[0].wrapMode);
	EXPECT_EQ(postprocess::ShaderWrapMode::Repeat, chain.passes[1].wrapMode);
	ASSERT_EQ(1u, chain.textures.size());
	EXPECT_TRUE(chain.textures[0].linearFilter);
	EXPECT_EQ(postprocess::ShaderWrapMode::ClampToBorder, chain.textures[0].wrapMode);
}

TEST(PostProcessingChainTest, LoadsReferencedPresetsWithinSelectedShaderFolder)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		std::string previousRoot = config::PostProcessShaderDirectory.get();
		~TemporaryDirectory() {
			config::PostProcessShaderDirectory.get() = previousRoot;
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_preset_references_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path / "base");
	std::filesystem::create_directories(directory.path / "custom");
	config::PostProcessShaderDirectory.get() = directory.path.string();
	std::ofstream(directory.path / "shared.glsl")
			<< "#pragma parameter GAIN \"Gain\" 0.5 0 1 0.1\n"
			"void main() { FragColor = texture(Source, vTexCoord) * GAIN; }\n";
	std::ofstream(directory.path / "base" / "original.glslp")
			<< "shaders = 1\nshader0 = ../shared.glsl\nparameters = GAIN\nGAIN = 0.25\n";
	const auto selected = directory.path / "custom" / "selected.glslp";
	std::ofstream(selected) << "#reference \"../base/original.glslp\"\nGAIN = 0.75\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(selected.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	ASSERT_EQ(1u, chain.parameters.size());
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].value);
	EXPECT_EQ(directory.path.string(), chain.passes[0].packageRoot);
	EXPECT_TRUE(std::filesystem::equivalent(directory.path / "shared.glsl", chain.passes[0].sourcePath));
	std::ofstream(selected) << "#reference \"selected.glslp\"\n";
	EXPECT_FALSE(postprocess::loadShaderChain(selected.string(), chain, error));
	EXPECT_NE(std::string::npos, error.find("cycle"));
	std::ofstream(selected) << "#reference \"../../outside.glslp\"\n";
	EXPECT_FALSE(postprocess::loadShaderChain(selected.string(), chain, error));
	EXPECT_NE(std::string::npos, error.find("boundary"));
}

TEST(PostProcessingChainTest, ResolvesAbsolutePassHistoryAndFeedbackSamplers)
{
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(
		"#ifdef FRAGMENT\n"
		"uniform sampler2D Pass1Texture;\nuniform sampler2D PrevTexture;\n"
		"uniform sampler2D Prev2Texture;\nuniform sampler2D FeedbackTexture;\n"
		"void main() { FragColor = texture(Pass1Texture, vTexCoord); }\n#endif\n", source, error)) << error;
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	chain.feedbackPass = 1;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	ASSERT_EQ(4u, chain.passes[2].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[2].inputs[0].kind);
	EXPECT_EQ(0u, chain.passes[2].inputs[0].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::History, chain.passes[2].inputs[1].kind);
	EXPECT_EQ(1u, chain.passes[2].inputs[1].historyOffset);
	EXPECT_EQ(3u, chain.passes[2].inputs[2].historyOffset);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[2].inputs[3].kind);
	EXPECT_EQ(1u, chain.passes[2].inputs[3].sourcePass);
}

TEST(PostProcessingChainTest, ParsesCrLfRetroArchPresetAndCommaSeparatedParameters)
{
	const std::string preset =
		"shaders = \"1\"\r\n"
		"shader0 = \"shaders/test.glsl\"\r\n"
		"parameters = \"FIRST, SECOND\"\r\n"
		"FIRST = \"0.25\"\r\n"
		"SECOND = \"0.75\"\r\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslPreset(preset, "C:/Shaders/test.glslp", chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::GLSL, chain.passes[0].format);
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_EQ("FIRST", chain.parameters[0].name);
	EXPECT_EQ("SECOND", chain.parameters[1].name);
}

TEST(PostProcessingChainTest, CombinesSelectedShaderFilesAsOrderedLayers)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_stack_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const std::string sourceShader =
		"#pragma parameter SHARED \"Shared value\" 0.5 0.0 1.0 0.1\n"
		"#if defined(FRAGMENT)\n"
		"void main() { FragColor = texture(Source, vTexCoord) * SHARED; }\n"
		"#endif\n";
	const std::string previousPassShader =
		"#if defined(FRAGMENT)\n"
		"uniform sampler2D PassPrev0Texture;\n"
		"void main() { FragColor = texture(PassPrev0Texture, vTexCoord); }\n"
		"#endif\n";
	const auto standalone = directory.path / "standalone.glsl";
	const auto presetFirst = directory.path / "preset-first.glsl";
	const auto presetSecond = directory.path / "preset-second.glsl";
	const auto preset = directory.path / "stack.glslp";
	std::ofstream(standalone) << sourceShader;
	std::ofstream(presetFirst) << sourceShader;
	std::ofstream(presetSecond) << previousPassShader;
	std::ofstream(preset) << "shaders = 2\nshader0 = preset-first.glsl\nshader1 = preset-second.glsl\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(
			std::vector<std::string> { standalone.string(), preset.string() }, chain, error)) << error;
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_EQ("SHARED", chain.parameters[0].name);
	EXPECT_EQ("SHARED", chain.parameters[1].name);
	EXPECT_NE(postprocess::shaderParameterKey(chain.parameters[0]),
			postprocess::shaderParameterKey(chain.parameters[1]));
	EXPECT_NE(postprocess::shaderSelectionLabel({ standalone.string(), preset.string() }),
			postprocess::shaderSelectionLabel({ preset.string(), standalone.string() }));
	EXPECT_EQ(0u, chain.parameters[0].layerIndex);
	EXPECT_EQ(1u, chain.parameters[1].layerIndex);
	EXPECT_EQ(0u, chain.passes[0].layerIndex);
	EXPECT_EQ(1u, chain.passes[1].layerIndex);
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(3u, chain.passes.size());
	EXPECT_FALSE(chain.passes[0].composeDisplayInput);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[0].scaleTypeX);
	EXPECT_FALSE(chain.passes[1].composeDisplayInput);
	EXPECT_FALSE(chain.passes[2].composeDisplayInput);
	ASSERT_EQ(3u, chain.sources.size());
	const auto previous = std::find_if(chain.passes[2].inputs.begin(), chain.passes[2].inputs.end(),
			[](const postprocess::ShaderPassInput& input) { return input.bindingName == "PassPrev0Texture"; });
	ASSERT_NE(chain.passes[2].inputs.end(), previous);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, previous->kind);
	EXPECT_EQ(1u, previous->sourcePass);

	ASSERT_TRUE(postprocess::loadShaderChain(
			std::vector<std::string> { preset.string(), standalone.string() }, chain, error)) << error;
	ASSERT_EQ(3u, chain.passes.size());
	EXPECT_FALSE(chain.passes[0].composeDisplayInput);
	EXPECT_FALSE(chain.passes[1].composeDisplayInput);
	EXPECT_FALSE(chain.passes[2].composeDisplayInput);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[2].scaleTypeX);
}

TEST(PostProcessingChainTest, AllowsCombinedLayerParametersWithinEachLayerLimit)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_layer_parameters_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	auto writeShader = [&](const char *name, const char *prefix) {
		const std::filesystem::path path = directory.path / name;
		std::ofstream stream(path);
		for (unsigned index = 0; index < 13; ++index)
			stream << "#pragma parameter " << prefix << index << " \"Parameter\" 0.5 0.0 1.0 0.1\n";
		stream << "#if defined(FRAGMENT)\nvoid main() { FragColor = texture(Source, vTexCoord); }\n#endif\n";
		return path;
	};
	const std::filesystem::path first = writeShader("first.glsl", "FIRST_");
	const std::filesystem::path second = writeShader("second.glsl", "SECOND_");

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(
			std::vector<std::string> { first.string(), second.string() }, chain, error)) << error;
	EXPECT_EQ(26u, chain.parameters.size());
}

TEST(PostProcessingChainTest, PreservesPresetValuesWhenReflectingParameterMetadata)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_parameters_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const auto shader = directory.path / "parameter.glsl";
	const auto preset = directory.path / "parameter.glslp";
	std::ofstream(shader) <<
			"#pragma parameter GAIN \"Gain\" 0.25 0.0 1.0 0.05\n"
			"#if defined(FRAGMENT)\n"
			"void main() { FragColor = texture(Source, vTexCoord) * GAIN; }\n"
			"#endif\n";
	std::ofstream(preset) <<
			"shaders = 1\nshader0 = parameter.glsl\nparameters = GAIN\nGAIN = 0.75\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(preset.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.parameters.size());
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].value);
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].values[0]);
	EXPECT_FLOAT_EQ(0.f, chain.parameters[0].minimum);
	EXPECT_FLOAT_EQ(1.f, chain.parameters[0].maximum);
	EXPECT_FLOAT_EQ(0.05f, chain.parameters[0].step);
}

TEST(PostProcessingChainTest, IgnoresIncludeTextInsideShaderComments)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_include_comments_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const auto shader = directory.path / "comments.glsl";
	std::ofstream(shader) <<
			"// #include \"missing-one.glsl\"\n"
			"/* #include \"missing-two.glsl\" */\n"
			"#if defined(FRAGMENT)\n"
			"void main() { FragColor = texture(Source, vTexCoord); }\n"
			"#endif\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(shader.string(), chain, error)) << error;
}

TEST(PostProcessingChainTest, KeepsReShadeTechniqueSelectionsPerLayer)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	struct TechniqueConfigRestore {
		std::string value = config::PostProcessShaderTechnique.get();
		~TechniqueConfigRestore() { config::PostProcessShaderTechnique.get() = value; }
	} restore;
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_techniques_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const std::string effect =
			"float4 FirstPS() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n"
			"float4 SecondPS() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n"
			"technique First { pass { PixelShader = FirstPS; } }\n"
			"technique Second { pass { PixelShader = SecondPS; } }\n";
	const auto first = directory.path / "first.fx";
	const auto second = directory.path / "second.fx";
	std::ofstream(first) << effect;
	std::ofstream(second) << effect;
	config::PostProcessShaderTechnique.get() = "stack:0=Second;1=First";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(
			std::vector<std::string> { first.string(), second.string() }, chain, error)) << error;
	ASSERT_EQ(2u, chain.techniqueGroups.size());
	EXPECT_EQ(1, chain.techniqueGroups[0].selectedTechnique);
	EXPECT_EQ(0, chain.techniqueGroups[1].selectedTechnique);
	EXPECT_EQ("stack:0=Second;1=First",
			postprocess::serializeShaderTechniqueSelections(chain.techniqueGroups));
}

TEST(PostProcessingChainTest, ValidatesAndReflectsReShadeRenderStates)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_states_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const auto stencilPass = directory.path / "stencil-pass.fx";
	const auto supportedSampler = directory.path / "supported-sampler.fx";
	const auto customSampler = directory.path / "custom-sampler.fx";
	const auto disabledBlend = directory.path / "disabled-blend.fx";
	const auto enabledBlend = directory.path / "enabled-blend.fx";
	std::ofstream(stencilPass) <<
			"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
			"technique Main { pass { PixelShader = MainPS; StencilEnable = true; } }\n";
	std::ofstream(supportedSampler) <<
			"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; };\n"
			"sampler2D SourceSampler { Texture = SourceTex; Filter = MIN_MAG_MIP_LINEAR; };\n"
			"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
			"technique Main { pass { PixelShader = MainPS; } }\n";
	std::ofstream(customSampler) <<
			"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; };\n"
			"sampler2D SourceSampler { Texture = SourceTex; AddressU = WRAP; };\n"
			"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
			"technique Main { pass { PixelShader = MainPS; } }\n";
	std::ofstream(disabledBlend) <<
			"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
			"technique Main { pass { PixelShader = MainPS; BlendEnable = false; SrcBlend = SRC_ALPHA; } }\n";
	std::ofstream(enabledBlend) <<
			"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
			"technique Main { pass { PixelShader = MainPS; BlendEnable = true; SrcBlend = SRC_ALPHA; DestBlend = INV_SRC_ALPHA; } }\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(stencilPass.string(), chain, error)) << error;
	EXPECT_TRUE(chain.passes[0].renderState.stencilEnabled);
	ASSERT_TRUE(postprocess::loadShaderChain(supportedSampler.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	ASSERT_EQ(1u, chain.passes[0].samplers.size());
	EXPECT_TRUE(chain.passes[0].samplers[0].linearFilter);
	EXPECT_TRUE(chain.passes[0].samplers[0].useMipmaps);
	ASSERT_TRUE(postprocess::loadShaderChain(customSampler.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	ASSERT_EQ(1u, chain.passes[0].samplers.size());
	EXPECT_EQ(postprocess::ShaderWrapMode::Repeat, chain.passes[0].samplers[0].wrapMode);
	ASSERT_TRUE(postprocess::loadShaderChain(disabledBlend.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	EXPECT_FALSE(chain.passes[0].blendEnabled);
	EXPECT_FALSE(chain.passes[0].renderState.attachments[0].blendEnabled);
	EXPECT_TRUE(chain.passes[0].preserveBackbuffer);
	ASSERT_TRUE(postprocess::loadShaderChain(enabledBlend.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	EXPECT_TRUE(chain.passes[0].blendEnabled);
}

TEST(PostProcessingChainTest, SkipsNestedConditionsInInactiveRetroArchStage)
{
	const std::string shader = R"(
#if defined(VERTEX)
void main() { gl_Position = VertexCoord; }
#elif defined(FRAGMENT)
#define FRAGMENT_ONLY 1
#if FRAGMENT_ONLY
void main() { FragColor = texture(Source, vTexCoord); }
#endif
#endif
)";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_NE(std::string::npos, source.vertex.find("gl_Position"));
	EXPECT_NE(std::string::npos, source.fragment.find("FragColor"));
}

TEST(PostProcessingChainTest, NormalizesCrLfRetroArchGlslInterface)
{
	const std::string shader =
		"#if defined(FRAGMENT)\r\n"
		"uniform vec2 TextureSize;\r\n"
		"#define SourceSize vec4(TextureSize, 1.0 / TextureSize)\r\n"
		"void main() { FragColor = vec4(SourceSize.xy, 0.0, 1.0); }\r\n"
		"#endif\r\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize"));
	EXPECT_NE(std::string::npos, source.fragment.find("vec4(SourceSize, 1.0 / SourceSize)"));
}

TEST(PostProcessingChainTest, PreprocessorPreservesFunctionMacroSeparatorsAndStripsComments)
{
	const std::string shader =
		"#define SAMPLE(value) vec4(value) // compatibility helper\n"
		"mat4 result = mat4(SAMPLE(1.0), SAMPLE(2.0));\n";
	std::string preprocessed;
	std::string error;
	ASSERT_TRUE(postprocess::preprocessShaderSource(shader, {}, preprocessed, error)) << error;
	EXPECT_NE(std::string::npos, preprocessed.find("mat4(vec4(1.0), vec4(2.0));"));
}

TEST(PostProcessingChainTest, PreprocessorDoesNotExpandMacrosInsideStringsOrComments)
{
	const std::string shader =
		"#define TOKEN 7\n"
		"const char *label = \"TOKEN\"; // TOKEN\n"
		"/* TOKEN */ int value = TOKEN;\n";
	std::string preprocessed;
	std::string error;
	ASSERT_TRUE(postprocess::preprocessShaderSource(shader, {}, preprocessed, error)) << error;
	EXPECT_NE(std::string::npos, preprocessed.find("\"TOKEN\"; // TOKEN"));
	EXPECT_NE(std::string::npos, preprocessed.find("/* TOKEN */ int value = 7;"));
}

TEST(PostProcessingChainTest, FunctionMacroArgumentsDoNotRewriteInjectedSwizzles)
{
	const std::string shader =
			"#define SAT(x) clamp((x), 0.0, 1.0)\n"
			"#if defined(FRAGMENT)\n"
			"void main() { vec2 pos = vTexCoord; FragColor = vec4(SAT(pos.x * pos.x)); }\n"
			"#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.fragment.find("pos.pos"));
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, hlsl, error)) << error;
#ifdef _WIN32
	EXPECT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << error;
#endif
}

TEST(PostProcessingChainTest, RescansNestedObjectAndFunctionMacroAliases)
{
	const std::string shader =
		"#define VALUE 0.5\n"
		"#define SCALE(x) (VALUE * (x))\n"
		"#define SCALE_ALIAS SCALE\n"
		"#define OUTER(x) SCALE_ALIAS(x)\n"
		"#define RESULT OUTER(1.0)\n"
		"#if defined(FRAGMENT)\n"
		"void main() { FragColor = vec4(RESULT + SCALE_ALIAS(1.0)); }\n#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.fragment.find("VALUE"));
	EXPECT_EQ(std::string::npos, source.fragment.find("OUTER"));
	EXPECT_EQ(std::string::npos, source.fragment.find("SCALE_ALIAS"));
	EXPECT_NE(std::string::npos, source.fragment.find("0.5"));
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, MacroCyclesTerminateWithoutRepeatedExpansion)
{
	const std::string shader =
		"#define A B\n#define B A\n"
		"#define SELF(x) SELF(x)\n"
		"int a = A; int b = SELF(1);\n";
	std::string preprocessed;
	std::string error;
	ASSERT_TRUE(postprocess::preprocessShaderSource(shader, {}, preprocessed, error)) << error;
	EXPECT_NE(std::string::npos, preprocessed.find("int a = A; int b = SELF(1);"));
}

TEST(PostProcessingChainTest, ExpandsManyIndependentMacroUses)
{
	std::string shader = "#define K 7\n";
	for (int i = 0; i < 10001; ++i) shader += "K ";
	shader += "\n";
	std::string preprocessed;
	std::string error;
	ASSERT_TRUE(postprocess::preprocessShaderSource(shader, {}, preprocessed, error)) << error;
	EXPECT_EQ(std::string::npos, preprocessed.find("K ", preprocessed.find('\n') + 1));
}

TEST(PostProcessingChainTest, ExpandsSelfReferencingSizeMacrosOnlyOnce)
{
	const std::string shader =
		"#if defined(FRAGMENT)\n"
		"uniform vec2 OutputSize;\nuniform vec2 TextureSize;\n"
		"#define OutputSize vec4(OutputSize, 1.0 / OutputSize)\n"
		"#define SourceSize vec4(TextureSize, 1.0 / TextureSize)\n"
		"#define VIEW OutputSize\n"
		"void main() { FragColor = VIEW + SourceSize; }\n#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("vec4(OutputSize, 1.0 / OutputSize)"));
	EXPECT_EQ(std::string::npos, source.fragment.find("#define"));
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, {}, {}, 1, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
#if defined(USE_VULKAN)
	std::string fragment, vertex;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, {}, {}, 1, fragment, vertex, error)) << error;
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, CompilesMultilineParameterUniformDeclarations)
{
	const std::string shader =
		"#pragma parameter amp \"Amplitude\" 1.25 0.0 2.0 0.05\n"
		"#pragma parameter phase \"Phase\" 0.5 0.0 2.0 0.05\n"
		"#if defined(FRAGMENT)\nuniform float amp,\n phase;\n"
		"void main() { FragColor = vec4(amp, phase, 0.0, 1.0); }\n#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	ASSERT_EQ(2u, source.parameters.size());
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, source.parameters, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, SuppliesLegacyHistoryCoordinatesAndColumnWiseMvp)
{
	const std::string shader =
		"#if defined(VERTEX)\n"
		"in vec4 VertexCoord;\nin vec2 TexCoord;\nin vec2 PrevTexCoord;\nin vec4 Prev6TexCoord;\n"
		"uniform mat4 MVPMatrix;\nout vec2 Previous;\nout vec2 Oldest;\n"
		"void main() { vec4 p = VertexCoord.x * MVPMatrix[0]; p += VertexCoord.y * MVPMatrix[1];"
		" p += VertexCoord.z * MVPMatrix[2]; p += VertexCoord.w * MVPMatrix[3]; gl_Position = p;"
		" Previous = PrevTexCoord; Oldest = Prev6TexCoord.xy; }\n"
		"#elif defined(FRAGMENT)\n"
		"in vec2 Previous;\nin vec2 Oldest;\n"
		"uniform sampler2D PrevTexture;\nuniform sampler2D Prev6Texture;\n"
		"void main() { FragColor = (texture(PrevTexture, Previous) + texture(Prev6Texture, Oldest)) * 0.5; }\n#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	const std::vector<postprocess::ShaderPassInput> inputs {
		{ "PrevTexture", postprocess::ShaderInputKind::History },
		{ "Prev6Texture", postprocess::ShaderInputKind::History }
	};
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, {}, inputs, hlsl, error, false, true)) << error;
	expectHlslCompiles(hlsl, "vs_4_0");
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, {}, inputs, 1, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
#if defined(USE_VULKAN)
	std::string fragment, vertex;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, {}, inputs, 1, fragment, vertex, error)) << error;
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vertex, error)) << error;
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, CompilesDisplayGlslHostContract)
{
	const std::string shader = R"(
/*
[configuration]
[OptionRangeFloat]
GUIName = Strength
OptionName = STRENGTH
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 1.0
[OptionRangeInteger]
GUIName = Mode
OptionName = MODE
MinValue = 0
MaxValue = 3
StepAmount = 1
DefaultValue = 1
[/configuration]
*/
CONSTANT vec3 Tint = vec3(0.1, 0.2, 0.3);
mat4x4 FilterMatrix() { return mat4x4(1.0); }
vec3 MatrixHelper()
{
	mat4x3 pos = mat4x3(Tint, Tint, Tint, Tint);
	return pos * vec4(1.0);
}
void main()
{
	float2 pixel = GetFragCoord();
	float2 pos = GetCoordinates();
	float2 SourceSize = GetNativeSize();
	mat4x4 transform = FilterMatrix();
	mat4x3 colors = mat4x3(Tint, Tint, Tint, Tint);
	vec4 weights = vec4(1.0) * transform;
	vec3 filtered = colors * weights;
	filtered += MatrixHelper() * (pos * SourceSize).x * 0.0;
	float4 color = Sample();
	if (GetOption(MODE) > 0)
		color.rgb = saturate(color.rgb + filtered * frac(pixel.x) * GetOption(STRENGTH));
	SetOutput(color);
}
)";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_TRUE(source.displayGlsl);
	ASSERT_EQ(2u, source.parameters.size());
	EXPECT_EQ(postprocess::SlangParameterType::Float, source.parameters[0].type);
	EXPECT_EQ(postprocess::SlangParameterType::Int, source.parameters[1].type);
	EXPECT_EQ("Strength", source.parameters[0].label);
	EXPECT_FLOAT_EQ(1.f, source.parameters[0].value);

	std::vector<postprocess::SlangParameter> parameters;
	for (const postprocess::ShaderParameter& parameter : source.parameters) {
		postprocess::SlangParameter typed;
		typed.name = parameter.name;
		typed.type = parameter.type;
		parameters.push_back(std::move(typed));
	}
	const std::vector<postprocess::ShaderPassInput> inputs {
		{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
	};
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, parameters, inputs, 1, hlsl, error)) << error;
#ifdef _WIN32
	EXPECT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << error;
#endif
#if defined(USE_VULKAN)
	std::string vulkanFragment;
	std::string vulkanVertex;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, parameters, {}, 1,
			vulkanFragment, vulkanVertex, error)) << error;
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, vulkanFragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, ResolvesRetroArchPreviousPassSamplers)
{
	const std::string shader =
		"#if defined(FRAGMENT)\n"
		"uniform sampler2D PassPrevTexture;\n"
		"uniform sampler2D PassPrev1Texture;\n"
		"uniform sampler2D PassPrev3Texture;\n"
		"uniform sampler2D OrigTexture;\n"
		"void main() { FragColor = texture(PassPrevTexture, vTexCoord)"
				" + texture(PassPrev1Texture, vTexCoord) + texture(PassPrev3Texture, vTexCoord)"
				" + texture(OrigTexture, vTexCoord); }\n"
		"#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	ASSERT_EQ(4u, source.slangResources.size());
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	ASSERT_EQ(4u, chain.passes[2].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::Source, chain.passes[2].inputs[0].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[2].inputs[1].kind);
	EXPECT_EQ(1u, chain.passes[2].inputs[1].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[2].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[3].kind);
}

TEST(PostProcessingChainTest, LoadsExternalRetroArchGlslCompatibilityPack)
{
	const char *rootValue = std::getenv("HOLLYCAST_RETROARCH_GLSL_TEST_ROOT");
	if (rootValue == nullptr || *rootValue == '\0')
		GTEST_SKIP() << "Set HOLLYCAST_RETROARCH_GLSL_TEST_ROOT to run the upstream compatibility pack.";

	const std::filesystem::path root(rootValue);
	ASSERT_TRUE(std::filesystem::is_directory(root)) << root.string();
	size_t shaderCount = 0;
	size_t presetCount = 0;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file())
			continue;
		const std::string extension = entry.path().extension().string();
		if (extension == ".glsl") {
			std::ifstream stream(entry.path(), std::ios::binary);
			ASSERT_TRUE(stream) << entry.path().string();
			const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			postprocess::ShaderPassSource source;
			std::string error;
			ASSERT_TRUE(postprocess::parseGlslShader(text, source, error))
					<< entry.path().string() << ": " << error;
			EXPECT_EQ(std::string::npos, source.vertex.find("#define SourceSize")) << entry.path().string();
			EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize")) << entry.path().string();
			EXPECT_FALSE(std::regex_search(source.vertex, std::regex(R"(\bOutSize\b)", std::regex::icase)))
					<< entry.path().string();
			EXPECT_FALSE(std::regex_search(source.fragment, std::regex(R"(\bOutSize\b)", std::regex::icase)))
					<< entry.path().string();
			std::vector<postprocess::ShaderPassInput> inputs {
				{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
			};
			for (const postprocess::SlangResource& resource : source.slangResources)
				if (resource.name != "Source")
					inputs.push_back({ resource.name, postprocess::ShaderInputKind::Source, {},
							postprocess::InvalidShaderPass });
			std::vector<postprocess::SlangParameter> parameters;
			for (const postprocess::ShaderParameter& parameter : source.parameters) {
				postprocess::SlangParameter typed;
				typed.name = parameter.name;
				typed.type = parameter.type;
				parameters.push_back(std::move(typed));
			}
			std::string hlsl;
			ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, parameters, inputs, 1,
					hlsl, error))
					<< entry.path().string() << ": " << error;
#ifdef _WIN32
			ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << entry.path().string() << ": " << error;
#endif
#if defined(USE_VULKAN)
			std::vector<postprocess::ShaderPassInput> vulkanBindings(inputs.begin() + 1, inputs.end());
			std::string vulkanFragment;
			std::string vulkanVertex;
			ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, parameters, vulkanBindings, 1,
					vulkanFragment, vulkanVertex, error)) << entry.path().string() << ": " << error;
			ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, vulkanFragment, error))
					<< entry.path().string() << ": " << error;
			if (!vulkanVertex.empty())
				ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vulkanVertex, error))
						<< entry.path().string() << ": " << error;
#endif
			if (!source.vertex.empty())
			{
				ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, parameters, inputs, hlsl, error,
						source.nativeFxStages, source.retroArchGlsl))
						<< entry.path().string() << ": " << error;
#ifdef _WIN32
				ASSERT_TRUE(compileHlsl(hlsl, "vs_4_0", error)) << entry.path().string() << ": " << error;
#endif
			}
			++shaderCount;
		}
		else if (extension == ".glslp") {
			postprocess::ShaderChain chain;
			std::string error;
			ASSERT_TRUE(postprocess::loadShaderChain(entry.path().string(), chain, error))
					<< entry.path().string() << ": " << error;
			for (const postprocess::ShaderPass& pass : chain.passes) {
				postprocess::ShaderPassSource source;
				ASSERT_TRUE(postprocess::loadShaderPassSource(pass, source, error))
						<< pass.sourcePath << ": " << error;
			}
			++presetCount;
		}
	}
	EXPECT_GT(shaderCount, 0u);
	EXPECT_GT(presetCount, 0u);
}

TEST(PostProcessingChainTest, LoadsExternalReShadeFxCompatibilityPack)
{
	const char *rootValue = std::getenv("HOLLYCAST_RESHADE_FX_TEST_ROOT");
	if (rootValue == nullptr || *rootValue == '\0')
		GTEST_SKIP() << "Set HOLLYCAST_RESHADE_FX_TEST_ROOT to run the ReShade FX compatibility pack.";

	const std::filesystem::path root(rootValue);
	ASSERT_TRUE(std::filesystem::is_directory(root)) << root.string();
	size_t effectCount = 0;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".fx")
			continue;
		postprocess::ShaderChain chain;
		std::string error;
		ASSERT_TRUE(postprocess::loadShaderChain(entry.path().string(), chain, error))
				<< entry.path().string() << ": " << error;
		for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
			const postprocess::ShaderPass& pass = chain.passes[passIndex];
			const postprocess::ShaderPassSource& source = chain.sources[passIndex];
			const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
			std::vector<postprocess::ShaderPassInput> inputs = pass.inputs;
			if (std::none_of(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& input) {
					return input.bindingName == "Source";
			}))
				inputs.push_back({ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass });
			std::stable_sort(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& left,
					const postprocess::ShaderPassInput& right) {
				return left.bindingName == "Source" && right.bindingName != "Source";
			});
			std::string hlsl;
			ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters,
					inputs, outputCount, hlsl, error)) << entry.path().string() << ": " << error;
#ifdef _WIN32
			ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << entry.path().string() << ": " << error;
#endif
			if (!source.vertex.empty()) {
				ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, source.slangParameters,
						inputs, hlsl, error, source.nativeFxStages)) << entry.path().string() << ": " << error;
#ifdef _WIN32
				ASSERT_TRUE(compileHlsl(hlsl, "vs_4_0", error)) << entry.path().string() << ": " << error;
#endif
			}
#if defined(USE_VULKAN)
			std::vector<postprocess::ShaderPassInput> bindings;
			for (const postprocess::SlangResource& resource : source.slangResources) {
				if (resource.convention == postprocess::SlangResourceConvention::Source && resource.name == "Source")
					continue;
				const auto input = std::find_if(pass.inputs.begin(), pass.inputs.end(), [&](const auto& candidate) {
					return candidate.bindingName == resource.name;
				});
				ASSERT_NE(pass.inputs.end(), input) << entry.path().string() << ": " << resource.name;
				bindings.push_back(*input);
			}
			std::string fragment;
			std::string vertex;
			ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, source.slangParameters, bindings,
					outputCount, fragment, vertex, error)) << entry.path().string() << ": " << error;
			ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error))
					<< entry.path().string() << ": " << error;
			if (!vertex.empty())
				ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vertex, error))
						<< entry.path().string() << ": " << error;
#endif
		}
		++effectCount;
	}
	EXPECT_GT(effectCount, 0u);
}

TEST(PostProcessingChainTest, LowersRetroArchGlslInterfaceAndParameters)
{
	const std::string shader = R"(
#version 130
#pragma parameter SCANLINE "Scanline strength" 0.5 0.0 1.0 0.05
#if defined(VERTEX)
in vec4 VertexCoord;
in vec4 TexCoord;
out vec4 TEX0;
uniform mat4 MVPMatrix;
void main() {
	gl_Position = MVPMatrix * VertexCoord;
	TEX0 = TexCoord;
}
#elif defined(FRAGMENT)
uniform sampler2D Texture;
uniform vec2 InputSize;
uniform vec2 TextureSize;
uniform vec2 OutputSize;
uniform int FrameDirection;
uniform int FrameCount;
in vec4 TEX0;
out vec4 FragColor;
void main() {
	FragColor = texture(Texture, TEX0.xy) * (FrameDirection > 0 ? SCANLINE : 1.0);
}
#endif
)";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_TRUE(source.retroArchGlsl);
	EXPECT_EQ(std::string::npos, source.vertex.find("MVPMatrix"));
	EXPECT_NE(std::string::npos, source.vertex.find("vTexCoord = TexCoord"));
	EXPECT_NE(std::string::npos, source.fragment.find("texture(Source, vTexCoord)"));
	EXPECT_NE(std::string::npos, source.fragment.find("(1) > 0"));
	EXPECT_EQ(std::string::npos, source.fragment.find("TextureSize"));
	ASSERT_EQ(1u, source.parameters.size());
	EXPECT_EQ("SCANLINE", source.parameters[0].name);
	EXPECT_FLOAT_EQ(0.5f, source.parameters[0].value);
	EXPECT_FLOAT_EQ(0.05f, source.parameters[0].step);
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, source.parameters, hlsl, error)) << error;

	expectHlslCompiles(hlsl, "ps_4_0");
	ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, {}, {}, hlsl, error,
			source.nativeFxStages, source.retroArchGlsl)) << error;

	expectHlslCompiles(hlsl, "vs_4_0");
}

TEST(PostProcessingChainTest, LowersStandardRetroArchCompatibilityMacros)
{
	const std::string shader = R"(
#version 130
#pragma parameter SCANLINE "Scanline strength" 0.5 0.0 1.0 0.05
#pragma parameter YIQ "YIQ amount" 1.0 0.0 1.0 0.05
#if defined(VERTEX)
#define COMPAT_PRECISION
#define COMPAT_ATTRIBUTE in
#define COMPAT_VARYING out
COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 COLOR;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 COL0;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)
#define OutSize vec4(OutputSize, 1.0 / OutputSize)
void main() {
	gl_Position = VertexCoord.x * MVPMatrix[0] + VertexCoord.y * MVPMatrix[1]
			+ VertexCoord.z * MVPMatrix[2] + VertexCoord.w * MVPMatrix[3];
	TEX0.xy = TexCoord.xy;
	COL0 = COLOR;
}
#elif defined(FRAGMENT)
#define COMPAT_PRECISION
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;
#define Source Texture
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)
#define OutSize vec4(OutputSize, 1.0 / OutputSize)
#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float SCANLINE, YIQ;
#endif
void main() {
	vec4 color = COMPAT_TEXTURE(Source, vTexCoord);
	FragColor = color * (FrameDirection > 0 ? SCANLINE : YIQ)
			* (SourceSize.y > 0.0 && OutSize.y > 0.0 ? 1.0 : 0.0);
}
#endif
)";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.vertex.find("COMPAT_ATTRIBUTE"));
	EXPECT_EQ(std::string::npos, source.fragment.find("COMPAT_TEXTURE"));
	EXPECT_EQ(std::string::npos, source.fragment.find("uniform COMPAT_PRECISION"));
	EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize"));
	EXPECT_NE(std::string::npos, source.fragment.find("vec4(SourceSize, 1.0 / SourceSize)"));
	EXPECT_EQ(std::string::npos, source.vertex.find("MVPMatrix"));
	EXPECT_EQ(std::string::npos, source.vertex.find("COL0"));
	EXPECT_EQ(std::string::npos, source.vertex.find("COLOR"));
	EXPECT_NE(std::string::npos, source.fragment.find("texture(Source, vTexCoord)"));
	EXPECT_NE(std::string::npos, source.fragment.find("(1) > 0"));
	ASSERT_EQ(2u, source.parameters.size());
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, source.parameters, hlsl, error))
			<< error << '\n' << source.fragment;

	expectHlslCompiles(hlsl, "ps_4_0");
	ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, {}, {}, hlsl, error,
			source.nativeFxStages, source.retroArchGlsl)) << error;

	expectHlslCompiles(hlsl, "vs_4_0");
}

TEST(PostProcessingChainTest, CompilesRepresentativeShaderForDx11AndVulkan)
{
	const std::string shader =
		"#pragma parameter GAIN \"Gain\" 0.5 0.0 1.0 0.1\n"
		"#if defined(FRAGMENT)\n"
		"void main() { FragColor = texture(Source, vTexCoord) * GAIN; }\n"
		"#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	postprocess::SlangParameter parameter;
	parameter.name = source.parameters[0].name;
	parameter.type = source.parameters[0].type;
	parameter.defaultValue.values = source.parameters[0].values;
	parameter.defaultValue.componentCount = source.parameters[0].componentCount;
	const std::vector<postprocess::SlangParameter> parameters { parameter };
	const std::vector<postprocess::ShaderPassInput> inputs {
		{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
	};
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, parameters, inputs, 1, hlsl, error)) << error;
#ifdef _WIN32
	ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << error;
#endif
#if defined(USE_VULKAN)
	std::string fragment;
	std::string vertex;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, parameters, {}, 1, fragment, vertex, error)) << error;
	ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, ParsesNamedPresetResourcesAndTargetRequests)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = first.slang\n"
		"alias0 = First\n"
		"float_framebuffer0 = true\n"
		"shader1 = final.slang\n"
		"alias1 = Final\n"
		"srgb_framebuffer1 = true\n"
		"feedback_pass = 1\n"
		"textures = LUT\n"
		"LUT = assets/lut.png\n"
		"LUT_linear = true\n"
		"LUT_mipmap = true\n"
		"LUT_wrap_mode = repeat\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangPreset(preset, "C:/Shaders/crt.slangp", chain, error)) << error;
	EXPECT_EQ(postprocess::ShaderTargetFormat::Float, chain.passes[0].targetFormat);
	EXPECT_EQ(postprocess::ShaderTargetFormat::Srgb, chain.passes[1].targetFormat);
	EXPECT_TRUE(chain.passes[1].feedbackTarget);
	EXPECT_EQ(1u, chain.feedbackPass);
	ASSERT_EQ(1u, chain.textures.size());
	EXPECT_EQ("LUT", chain.textures[0].name);
	std::string texturePath = chain.textures[0].path;
	std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
	EXPECT_EQ("C:/Shaders/assets/lut.png", texturePath);
	EXPECT_TRUE(chain.textures[0].linearFilter);
	EXPECT_TRUE(chain.textures[0].useMipmaps);
	EXPECT_EQ(postprocess::ShaderWrapMode::Repeat, chain.textures[0].wrapMode);
}

TEST(PostProcessingChainTest, ComputesOnlyRequiredOutputMipChains)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	chain.passes[0].outputs.resize(2);
	chain.passes[0].outputs[0].useMipmaps = true;
	chain.passes[2].useMipmaps = true;
	postprocess::ShaderPassInput alias;
	alias.kind = postprocess::ShaderInputKind::PassOutput;
	alias.sourcePass = 0;
	alias.sourceOutput = 1;
	chain.passes[2].inputs.push_back(alias);

	const std::vector<std::vector<bool>> requirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	ASSERT_EQ(3u, requirements.size());
	ASSERT_EQ(2u, requirements[0].size());
	EXPECT_TRUE(requirements[0][0]);
	EXPECT_TRUE(requirements[0][1]);
	ASSERT_EQ(1u, requirements[1].size());
	EXPECT_TRUE(requirements[1][0]);
	ASSERT_EQ(1u, requirements[2].size());
	EXPECT_FALSE(requirements[2][0]);
}

TEST(PostProcessingChainTest, RejectsUnsafeOrAmbiguousPresetResources)
{
	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangPreset("shaders = 1\nshader0 = ../outside.slang\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_NE(std::string::npos, error.find("relative"));
	EXPECT_FALSE(postprocess::parseSlangPreset(
			"shaders = 1\nshader0 = pass.slang\nalias0 = Pass\nfloat_framebuffer0 = true\nsrgb_framebuffer0 = true\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_NE(std::string::npos, error.find("both float and sRGB"));
}

TEST(PostProcessingChainTest, SplitsSlangStagesAndReflectsParameters)
{
	const std::string source =
		"#version 450\n"
		"#pragma format R16G16B16A16_SFLOAT\n"
		"#pragma parameter BLOOM \"Bloom strength\" 0.5 0.0 1.0 0.05\n"
		"#pragma stage vertex\n"
		"void vertexMain() {}\n"
		"#pragma stage fragment\n"
		"void fragmentMain() {}\n";

	postprocess::SlangShaderSource shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangShader(source, shader, error)) << error;
	EXPECT_NE(std::string::npos, shader.vertex.find("vertexMain"));
	EXPECT_EQ(std::string::npos, shader.vertex.find("fragmentMain"));
	EXPECT_NE(std::string::npos, shader.fragment.find("fragmentMain"));
	ASSERT_EQ(1u, shader.parameters.size());
	EXPECT_EQ("BLOOM", shader.parameters[0].name);
	EXPECT_EQ("Bloom strength", shader.parameters[0].label);
	EXPECT_FLOAT_EQ(0.05f, shader.parameters[0].step);

	postprocess::SlangShaderIR metadata;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, metadata, error)) << error;
	EXPECT_EQ("R16G16B16A16_SFLOAT", metadata.format);
}

TEST(PostProcessingChainTest, ResolvesSlangResourceGraphBindings)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(2);
	chain.passes[0].alias = "First";
	chain.passes[1].alias = "Final";
	chain.feedbackPass = 1;
	chain.passes[1].feedbackTarget = true;
	chain.textures.push_back({ "LUT", "C:/Shaders/lut.png" });

	postprocess::ShaderPassSource source;
	source.slangResources = {
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Source, "Source" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Original, "Original" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::PassAlias, "FirstTexture", "First" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::History, "FirstHistory", "First" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Feedback, "FinalFeedback", "Final" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::PassAlias, "LUT", "LUT" },
	};

	std::string error;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 1, source, error)) << error;
	ASSERT_EQ(6u, chain.passes[1].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[1].inputs[2].kind);
	EXPECT_EQ(0u, chain.passes[1].inputs[2].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::History, chain.passes[1].inputs[3].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[1].inputs[4].kind);
	EXPECT_EQ(1u, chain.passes[1].inputs[4].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::LookupTexture, chain.passes[1].inputs[5].kind);

	EXPECT_FALSE(postprocess::resolveShaderPassInputs(chain, 0, source, error));
	EXPECT_NE(std::string::npos, error.find("earlier pass alias"));
}

TEST(PostProcessingChainTest, ResolvesFeedbackFromALaterPassTarget)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(2);
	chain.passes[0].alias = "First";
	chain.passes[1].alias = "Later";
	postprocess::ShaderPassSource source;
	source.slangResources.push_back({ postprocess::SlangResourceType::Sampler2D,
			postprocess::SlangResourceConvention::Feedback, "LaterFeedback", "Later" });

	std::string error;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 0, source, error)) << error;
	ASSERT_EQ(1u, chain.passes[0].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[0].inputs[0].kind);
	EXPECT_EQ(1u, chain.passes[0].inputs[0].sourcePass);
}

TEST(PostProcessingChainTest, ClassifiesSlangTextureSuffixConventions)
{
	const std::string source =
		"#version 450\n"
		"uniform sampler2D SourceTexture;\n"
		"uniform sampler2D OriginalTexture;\n"
		"uniform sampler2D FirstHistoryTexture;\n"
		"uniform sampler2D FinalFeedbackTexture;\n"
		"uniform sampler2D FirstTexture;\n"
		"void main() {}\n";

	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, shader, error)) << error;
	ASSERT_EQ(5u, shader.resources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, shader.resources[0].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::Original, shader.resources[1].convention);
	EXPECT_EQ("First", shader.resources[2].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::History, shader.resources[2].convention);
	EXPECT_EQ("Final", shader.resources[3].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::Feedback, shader.resources[3].convention);
	EXPECT_EQ("First", shader.resources[4].alias);
}

TEST(PostProcessingChainTest, ResolvesStandardSlangIndexedResources)
{
	const std::string shaderText =
		"#version 450\n"
		"layout(binding = 0) uniform sampler2D OriginalHistory0;\n"
		"layout(binding = 1) uniform sampler2D OriginalHistory2;\n"
		"layout(binding = 2) uniform sampler2D PassOutput0;\n"
		"layout(binding = 3) uniform sampler2D PassFeedback1;\n"
		"layout(binding = 4) uniform sampler2D User0Sampler;\n"
		"#pragma stage fragment\nvoid main() {}\n";
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(shaderText, shader, error)) << error;

	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	chain.textures.push_back({ "LUT", "C:/Shaders/lut.png" });
	postprocess::ShaderPassSource source;
	source.slangResources = shader.resources;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	ASSERT_EQ(5u, chain.passes[2].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[0].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::History, chain.passes[2].inputs[1].kind);
	EXPECT_EQ(2u, chain.passes[2].inputs[1].historyOffset);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[2].inputs[2].kind);
	EXPECT_EQ(0u, chain.passes[2].inputs[2].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[2].inputs[3].kind);
	EXPECT_EQ(1u, chain.passes[2].inputs[3].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::LookupTexture, chain.passes[2].inputs[4].kind);
	EXPECT_EQ("LUT", chain.passes[2].inputs[4].resourceName);
}

TEST(PostProcessingChainTest, RejectsExcessiveHistoryRetention)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(1);
	postprocess::ShaderPassSource source;
	postprocess::SlangResource history;
	history.type = postprocess::SlangResourceType::Sampler2D;
	history.convention = postprocess::SlangResourceConvention::History;
	history.name = "OriginalHistory32";
	history.alias = "Original";
	history.historyOffset = 32;
	source.slangResources.push_back(std::move(history));
	std::string error;

	EXPECT_FALSE(postprocess::resolveShaderPassInputs(chain, 0, source, error));
	EXPECT_NE(std::string::npos, error.find("at most 32 frames"));
}

TEST(PostProcessingChainTest, ResolvesPassTargetsFromTheSharedFrameContract)
{
	postprocess::ShaderPass pass;
	pass.scaleTypeX = postprocess::ScaleType::Viewport;
	pass.scaleTypeY = postprocess::ScaleType::Source;
	pass.scaleX = 0.5f;
	pass.scaleY = 2.f;
	postprocess::ShaderFrameInfo frame { 320, 240, 1280, 720, 42 };
	int width = 0;
	int height = 0;
	std::string error;

	EXPECT_TRUE(postprocess::resolveTargetSize(pass, frame, width, height, error));
	EXPECT_EQ(640, width);
	EXPECT_EQ(480, height);

	pass.scaleTypeX = postprocess::ScaleType::Absolute;
	pass.scaleX = 0.f;
	EXPECT_FALSE(postprocess::resolveTargetSize(pass, frame, width, height, error));
}

TEST(PostProcessingChainTest, ComposesOnlyNormalDisplayInputs)
{
	postprocess::ShaderFrameInfo frame { 640, 480, 1280, 960, 42, false };
	EXPECT_TRUE(postprocess::shouldComposeDisplayInput(frame, false));
	EXPECT_FALSE(postprocess::shouldComposeDisplayInput(frame, true));
	frame.sourceWidth = frame.viewportWidth;
	frame.sourceHeight = frame.viewportHeight;
	EXPECT_FALSE(postprocess::shouldComposeDisplayInput(frame, false));
	frame.rotationPending = true;
	EXPECT_TRUE(postprocess::shouldComposeDisplayInput(frame, false));
}

TEST(PostProcessingChainTest, TranslatesPortableGlslFragmentBodiesToHlsl)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { gl_FragColor = mix(texture(Source, vTexCoord), vec4(1.0), 0.25); }", hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, TranslatesPortableScanlineShaderToHlsl)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { vec4 color = texture(Source, vTexCoord); float scanlineFactor = mix(0.55, 1.0, mod(floor(vTexCoord.y * OutputSize.y), 2.0)); gl_FragColor = vec4(color.rgb * scanlineFactor, color.a); }",
			hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, TranslatesGlslScalarVectorConstructors)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { float luminance = 0.5; gl_FragColor = vec4(vec3(luminance), 1.0); }",
			hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, PacksDx11ParametersContiguously)
{
	std::vector<postprocess::SlangParameter> parameters(3);
	parameters[0].name = "RED";
	parameters[1].name = "GREEN";
	parameters[2].name = "BLUE";
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { gl_FragColor = texture(Source, vTexCoord) * vec4(RED, GREEN, BLUE, 1.0); }",
			parameters, hlsl, error)) << error;
#ifdef _WIN32
	D3D11_SHADER_VARIABLE_DESC parameterLayout {};
	ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error, &parameterLayout)) << error;
	EXPECT_EQ(48u, parameterLayout.StartOffset);
	EXPECT_EQ(64u * 16u, parameterLayout.Size);
#else
	expectHlslCompiles(hlsl, "ps_4_0");
#endif
}

TEST(PostProcessingChainTest, RejectsUnbindableGlslResourcesForHlslTranslation)
{
	std::string hlsl;
	std::string error;
	EXPECT_FALSE(postprocess::translateGlslFragmentToHlsl(
			"uniform sampler2D Other; void main() { gl_FragColor = texture(Other, vec2(0.0)); }", hlsl, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, LowersReShadeFxStructsHelpersResourcesAndControlFlow)
{
	const std::string effect =
		"#define ENABLE_BLOOM 1\n"
		"uniform float BLOOM < ui_label = \"Bloom\"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.05; > = 0.75;\n"
		"uniform float4 Tint = float4(1.0, 0.9, 0.8, 1.0);\n"
		"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
		"sampler2D SourceSampler { Texture = SourceTex; Filter = MIN_MAG_MIP_LINEAR; };\n"
		"struct PixelData { float2 texcoord : TEXCOORD0; };\n"
		"float4 ApplyBloom(float4 color) { for (int i = 0; i < 1; ++i) color = lerp(color, Tint, BLOOM); return saturate(color); }\n"
		"float4 MainPS(PixelData input) : SV_Target { return ApplyBloom(tex2D(SourceSampler, input.texcoord)); }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("void main"));
	EXPECT_TRUE(source.nativeFxStages);
	EXPECT_NE(std::string::npos, source.fragment.find("ApplyBloom"));
	ASSERT_EQ(2u, source.parameters.size());
	EXPECT_EQ("BLOOM", source.parameters[0].name);
	EXPECT_EQ("Tint", source.parameters[1].name);
	ASSERT_EQ(2u, source.slangParameters.size());
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, source.slangResources[0].convention);
}

TEST(PostProcessingChainTest, RejectsNonPortableReShadeFxBeforeActivation)
{
	postprocess::ShaderPassSource source;
	std::string error;
	EXPECT_FALSE(postprocess::parseReShadeFx(
			"technique Main { pass { PixelShader = MainPS; } }\n"
			"float4 MainPS() : SV_Target { return tex2D(OtherSampler, float2(0.0, 0.0)); }\n", source, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, AssignsDx11VaryingSemanticsByName)
{
	const std::string vertex =
			"out vec2 Second;\n"
			"out vec4 First;\n"
			"void main() { First = vec4(1.0); Second = TexCoord; gl_Position = VertexCoord; }";
	const std::string fragment =
			"in vec4 First;\n"
			"in vec2 Second;\n"
			"void main() { gl_FragColor = First * Second.x; }";
	std::string vertexHlsl;
	std::string fragmentHlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslVertexToHlsl(vertex, {}, vertexHlsl, error)) << error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(fragment,
			std::vector<postprocess::SlangParameter> {}, fragmentHlsl, error)) << error;
	EXPECT_NE(std::string::npos, vertexHlsl.find("First : TEXCOORD1"));
	EXPECT_NE(std::string::npos, fragmentHlsl.find("First : TEXCOORD1"));
	EXPECT_NE(std::string::npos, vertexHlsl.find("Second : TEXCOORD2"));
	EXPECT_NE(std::string::npos, fragmentHlsl.find("Second : TEXCOORD2"));
}

#if defined(USE_VULKAN)
TEST(PostProcessingChainTest, PreservesExplicitVulkanVaryingLocationsWithoutCollisions)
{
	postprocess::ShaderPassSource source;
	source.vertex =
		"layout(location = 2) out vec2 Fixed;\n"
		"out vec2 Automatic;\n"
		"void main() { gl_Position = vec4(VertexCoord, 1.0); Fixed = TexCoord; Automatic = TexCoord; }\n";
	source.fragment =
		"layout(location = 2) in vec2 Fixed;\n"
		"in vec2 Automatic;\n"
		"void main() { FxOutput0 = texture(Source, Fixed + Automatic * 0.0); }\n";
	std::string fragment;
	std::string vertex;
	std::string error;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, {}, {}, 1, fragment, vertex, error)) << error;
	EXPECT_NE(std::string::npos, vertex.find("layout(location = 2) out vec2 Fixed"));
	EXPECT_NE(std::string::npos, vertex.find("layout (location = 1) out vec2 Automatic"));
	EXPECT_NE(std::string::npos, fragment.find("layout (location = 1) in vec2 Automatic"));

	source.fragment =
		"layout(location = 3) in vec2 Fixed;\n"
		"void main() { FxOutput0 = texture(Source, Fixed); }\n";
	EXPECT_FALSE(postprocess::buildVulkanShaderSources(source, {}, {}, 1, fragment, vertex, error));
	EXPECT_NE(std::string::npos, error.find("changes explicit location"));
}
#endif

TEST(PostProcessingChainTest, ReflectsReShadeChoiceParameters)
{
	const std::string effect =
		"uniform int Mode < ui_label = \"Quality\"; ui_type = \"combo\"; ui_items = \"Low\\0Medium\\0High\\0\"; > = 1;\n"
		"float4 MainPS() : SV_Target { return float4(Mode, 0.0, 0.0, 1.0); }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	ASSERT_EQ(1u, source.parameters.size());
	const postprocess::ShaderParameter& parameter = source.parameters[0];
	EXPECT_EQ(postprocess::SlangParameterType::Int, parameter.type);
	EXPECT_EQ(0.f, parameter.minimum);
	EXPECT_EQ(2.f, parameter.maximum);
	EXPECT_EQ(1.f, parameter.step);
	EXPECT_EQ((std::vector<std::string> { "Low", "Medium", "High" }), parameter.choiceLabels);
}

TEST(PostProcessingChainTest, SnapsPersistedParametersToDeclaredSteps)
{
	postprocess::ShaderChain chain;
	postprocess::ShaderParameter parameter;
	parameter.name = "MODE";
	parameter.label = "Mode";
	parameter.minimum = parameter.minimumValues[0] = 0.f;
	parameter.maximum = parameter.maximumValues[0] = 1.f;
	parameter.step = parameter.stepValues[0] = 1.f;
	chain.parameters.push_back(parameter);

	postprocess::applyShaderChainParameterValues(chain, "MODE=0.6");
	EXPECT_EQ(1.f, chain.parameters[0].value);
}

TEST(PostProcessingChainTest, PreservesEverySlangMatrixComponentThroughSavedOverrides)
{
	for (unsigned columns = 2; columns <= 4; ++columns) {
		for (unsigned rows = 2; rows <= 4; ++rows) {
			const unsigned components = columns * rows;
			const std::string type = "mat" + std::to_string(columns)
					+ (columns == rows ? "" : "x" + std::to_string(rows));
			SCOPED_TRACE(type);
			std::string initial, minimum, maximum, step, overrideValues;
			for (unsigned i = 0; i < components; ++i) {
				if (i != 0) { initial += ','; minimum += ','; maximum += ','; step += ','; overrideValues += ','; }
				initial += std::to_string(i + 1);
				minimum += '0'; maximum += "20"; step += '1';
				overrideValues += std::to_string(components - i);
			}
			const std::string shader = "#version 450\n#pragma parameter " + type + " M \"Matrix\" "
					+ type + '(' + initial + ") " + type + '(' + minimum + ") "
					+ type + '(' + maximum + ") " + type + '(' + step + ")\nvoid main() {}\n";
			postprocess::SlangShaderSource source;
			std::string error;
			ASSERT_TRUE(postprocess::parseSlangShader(shader, source, error)) << error;
			ASSERT_EQ(1u, source.parameters.size());
			EXPECT_EQ(components, source.parameters[0].componentCount);
			EXPECT_EQ("M=" + initial, postprocess::serializeShaderChainParameterValues(source.parameters));
			postprocess::ShaderChain chain;
			chain.parameters = source.parameters;
			postprocess::applyShaderChainParameterValues(chain, "M=" + overrideValues);
			EXPECT_EQ("M=" + overrideValues, postprocess::serializeShaderChainParameterValues(chain.parameters));
			for (unsigned i = 0; i < components; ++i)
				EXPECT_FLOAT_EQ(static_cast<float>(components - i), chain.parameters[0].values[i]);
		}
	}
}

TEST(PostProcessingChainTest, ParsesReShadeFxIntoBackendNeutralSyntaxIr)
{
	const std::string effect =
		"// Lexer must ignore include-looking comments. #include \"other.fx\"\n"
		"#define ENABLE_CRT 1\n"
		"uniform float4 Tint < ui_label = \"Tint Color\"; > = float4(1.0, 0.9, 0.8, 1.0);\n"
		"texture2D SourceTex < string source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
		"sampler2D SourceSampler { Texture = SourceTex; };\n"
		"struct PixelData { float2 texcoord : TEXCOORD0; };\n"
		"float4 MainPS(PixelData input) : SV_Target { return tex2D(SourceSampler, input.texcoord) * Tint; }\n"
		"technique Main { pass First { VertexShader = FullscreenVS; PixelShader = MainPS; } }\n";

	postprocess::ReShadeEffect parsed;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeEffect(effect, parsed, error)) << error;
	EXPECT_FALSE(parsed.preprocessedSource.empty());
	ASSERT_EQ(1u, parsed.uniforms.size());
	EXPECT_EQ(postprocess::ReShadeValueType::Float4, parsed.uniforms[0].valueType);
	EXPECT_EQ("Tint Color", parsed.uniforms[0].annotations[0].value);
	ASSERT_EQ(1u, parsed.textures.size());
	ASSERT_EQ(1u, parsed.samplers.size());
	ASSERT_EQ(1u, parsed.structs.size());
	ASSERT_EQ(1u, parsed.functions.size());
	EXPECT_NE(std::string::npos, parsed.functions[0].body.find("tex2D"));
	ASSERT_TRUE(parsed.selectedTechnique.has_value());
	ASSERT_TRUE(parsed.selectedPass.has_value());
	EXPECT_EQ("MainPS", parsed.techniques[*parsed.selectedTechnique].passes[*parsed.selectedPass].pixelShader);
}

TEST(PostProcessingChainTest, RejectsReShadeFxIncludesBeforeParsing)
{
	postprocess::ReShadeEffect parsed;
	std::string error;
	EXPECT_FALSE(postprocess::parseReShadeEffect("# include \\\"outside.fx\\\"\n", parsed, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, AppliesReShadeFxConditionalPreprocessingBeforeParsing)
{
	const std::string effect =
		"#define ENABLE_EFFECT 1\n"
		"#if ENABLE_EFFECT\n"
		"float4 EnabledPS() : SV_Target { return (float4)1.0; }\n"
		"#else\n"
		"float4 DisabledPS() : SV_Target { return (float4)0.0; }\n"
		"#endif\n"
		"technique Main { pass { PixelShader = EnabledPS; } }\n";
	postprocess::ReShadeEffect parsed;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeEffect(effect, parsed, error)) << error;
	ASSERT_EQ(1u, parsed.functions.size());
	EXPECT_EQ("EnabledPS", parsed.functions[0].name);
}

TEST(PostProcessingChainTest, AppliesReShadeFxCompoundPreprocessorExpressionsAndMacros)
{
	const std::string effect =
		"#define QUALITY 2\n"
		"#define ENABLED 1\n"
		"#define COLOR(value) ((float4)(value))\n"
		"#if defined(ENABLED) && QUALITY >= 2 && !defined(DISABLED)\n"
		"#define RETURN_COLOR COLOR(1.0)\n"
		"#else\n"
		"#define RETURN_COLOR (float4)0.0\n"
		"#endif\n"
		"float4 MainPS() : SV_Target { return RETURN_COLOR; }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_TRUE(source.nativeFxStages);
	EXPECT_EQ(1u, source.outputCount);
}

TEST(PostProcessingChainTest, AppliesReShadeFxFunctionMacrosInArithmeticConditions)
{
	const std::string effect =
		"#define VERSION(major,minor,build) (10000 * (major) + 100 * (minor) + (build))\n"
		"#define SUPPORTED(major,minor,build) (__RESHADE__ >= VERSION(major,minor,build))\n"
		"#if SUPPORTED(4, 0, 1)\n"
		"float4 MainPS() : SV_Target { return (float4)1.0; }\n"
		"#else\n"
		"float4 MainPS() : SV_Target { return (float4)0.0; }\n"
		"#endif\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_TRUE(source.nativeFxStages);
	EXPECT_EQ(1u, source.outputCount);
}

TEST(PostProcessingChainTest, LowersReShadeNamespacesAndReflectsSceneDepth)
{
	const std::string header =
		"namespace ReShade {\n"
		"texture BackBufferTex : COLOR;\n"
		"texture DepthBufferTex : DEPTH;\n"
		"sampler BackBuffer { Texture = BackBufferTex; };\n"
		"sampler DepthBuffer { Texture = DepthBufferTex; };\n"
		"}\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(header
			+ "float3 MainPS(float2 uv : TEXCOORD) : SV_Target { return tex2D(ReShade::BackBuffer, uv).rgb; }\n"
			+ "technique Main { pass { PixelShader = MainPS; } }\n", source, error)) << error;
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, source.slangResources[0].convention);
	EXPECT_EQ(std::string::npos, source.fragment.find("ReShade::"));
	EXPECT_TRUE(source.nativeFxStages);
	EXPECT_EQ(1u, source.outputCount);

	ASSERT_TRUE(postprocess::parseReShadeFx(header
			+ "float MainPS(float2 uv : TEXCOORD) : SV_Target { return tex2D(ReShade::DepthBuffer, uv).r; }\n"
			+ "technique Main { pass { PixelShader = MainPS; } }\n", source, error)) << error;
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Depth, source.slangResources[0].convention);
}

TEST(PostProcessingChainTest, LowersReShadeOutTargetsAndHlslVectorSplatsAcrossBackends)
{
	const std::string effect =
		"#pragma once\n"
		"uniform bool Enabled = true;\n"
		"uniform float2 NativePixelSize < source = \"normalized_native_pixel_size\"; >;\n"
		"uniform int HostFrame < source = \"framecount\"; >;\n"
		"uniform float HostTimer < source = \"timer\"; >;\n"
		"texture2D BackBufferTex : COLOR;\n"
		"sampler2D BackBuffer { Texture = BackBufferTex; };\n"
		"static const float PI = 3.1415926536;\n"
		"float3 Helper(float3 color) { if (color.r > 0.5) { return pow(color, 2.0); } else { return 0.0; } }\n"
		"void main(float2 texcoord : TEXCOORD, out float4 result : SV_Target0) {\n"
		"  float3 input = tex2D(BackBuffer, texcoord).rgb;\n"
		"  float2 scale = 1.0;\n"
		"  float amount = Enabled * (scale.x > 0.0 && scale.y > 0.0);\n"
		"  result = float4(lerp(Helper(input), 0.0, amount) + (float3)(dot(input, 0.0) + PI * 0.0 + NativePixelSize.x * 0.0 + HostFrame * 0.0 + HostTimer * 0.0), 1.0);\n"
		"}\n"
		"technique Main { pass { PixelShader = main; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_EQ(1u, source.outputCount);
	ASSERT_EQ(2u, source.parameters.size());
	EXPECT_EQ("Enabled", source.parameters[0].name);
	EXPECT_EQ("timer", source.parameters[1].hostSource);
	EXPECT_TRUE(source.nativeFxStages);

	const std::vector<postprocess::ShaderPassInput> inputs {
		{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass },
		{ "BackBuffer", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
	};
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, inputs, 1, hlsl, error)) << error;
#ifdef _WIN32
	ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << error;
#endif
#if defined(USE_VULKAN)
	std::string fragment;
	std::string vertex;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, source.slangParameters,
			{ inputs[1] }, 1, fragment, vertex, error)) << error;
	ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, LoadsBomPrefixedReShadePackagesWithSiblingTextures)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	} directory { std::filesystem::temp_directory_path()
			/ ("hollycast_reshade_package_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) };
	const std::filesystem::path shaders = directory.path / "Shaders";
	const std::filesystem::path effects = shaders / "effects";
	const std::filesystem::path shared = shaders / "shared";
	const std::filesystem::path textures = directory.path / "Textures";
	std::filesystem::create_directories(effects);
	std::filesystem::create_directories(shared);
	std::filesystem::create_directories(textures);
	const std::filesystem::path effectPath = effects / "LUT.fx";
	const std::filesystem::path texturePath = textures / "lut.png";
	std::ofstream(shaders / "ReShade.fxh") << "// shared package header\n";
	std::ofstream(shared / "Common.fxh") << "#define APPLY_LOOKUP(value) (value)\n";
	{
		std::ofstream effect(effectPath, std::ios::binary);
		effect << "\xEF\xBB\xBF"
				"#include \"ReShade.fxh\"\n"
				"#include \"../shared/Common.fxh\"\n"
				"texture2D Lookup < source = \"../Textures/lut.png\"; >;\n"
				"sampler2D LookupSampler { Texture = Lookup; };\n"
				"float4 MainPS(float2 uv : TEXCOORD) : SV_Target { return APPLY_LOOKUP(tex2D(LookupSampler, uv)); }\n"
				"technique Main { pass { PixelShader = MainPS; } }\n";
	}
	std::ofstream(texturePath, std::ios::binary) << "placeholder";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(effectPath.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.textures.size());
	EXPECT_TRUE(std::filesystem::equivalent(texturePath, chain.textures[0].path));
	std::filesystem::path previousShaders = shaders;
	for (const char *name : { "shaders", "sHaDeRs" }) {
		SCOPED_TRACE(name);
		const auto renamed = directory.path / name;
		// An intermediate name also exercises case changes on Windows filesystems.
		std::filesystem::rename(previousShaders, directory.path / "shader-staging");
		std::filesystem::rename(directory.path / "shader-staging", renamed);
		ASSERT_TRUE(postprocess::loadShaderChain((renamed / "effects/LUT.fx").string(), chain, error)) << error;
		ASSERT_EQ(1u, chain.textures.size());
		EXPECT_TRUE(std::filesystem::equivalent(texturePath, chain.textures[0].path));
		previousShaders = renamed;
	}
}

TEST(PostProcessingChainTest, ResolvesReShadeRenderTargetFeedbackAndRuntimeUiBounds)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	} directory { std::filesystem::temp_directory_path()
			/ ("hollycast_reshade_feedback_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) };
	std::filesystem::create_directories(directory.path);
	const std::filesystem::path effectPath = directory.path / "Feedback.fx";
	std::ofstream(effectPath) <<
			"uniform int Height < ui_min = 1; ui_max = BUFFER_HEIGHT; > = 480;\n"
			"texture2D SourceTex < source = \"color\"; >;\n"
			"sampler2D SourceSampler { Texture = SourceTex; };\n"
			"texture2D Accum { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };\n"
			"sampler2D AccumSampler { Texture = Accum; };\n"
			"float4 AccumPS(float2 uv : TEXCOORD) : SV_Target { return tex2D(AccumSampler, uv) + tex2D(SourceSampler, uv); }\n"
			"float4 SavePS(float2 uv : TEXCOORD) : SV_Target { return tex2D(SourceSampler, uv); }\n"
			"technique Main {\n"
			"  pass Accumulate { PixelShader = AccumPS; }\n"
			"  pass Save { PixelShader = SavePS; RenderTarget = Accum; }\n"
			"}\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(effectPath.string(), chain, error)) << error;
	ASSERT_EQ(2u, chain.passes.size());
	const auto feedback = std::find_if(chain.passes[0].inputs.begin(), chain.passes[0].inputs.end(),
			[](const postprocess::ShaderPassInput& input) { return input.bindingName == "AccumSampler"; });
	ASSERT_NE(chain.passes[0].inputs.end(), feedback);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, feedback->kind);
	EXPECT_EQ(1u, feedback->sourcePass);
	ASSERT_EQ(1u, chain.parameters.size());
	EXPECT_EQ(static_cast<float>(postprocess::MaxShaderTargetDimension), chain.parameters[0].maximum);
}

TEST(PostProcessingChainTest, EnforcesReShadeParameterComponentLimitAndEvaluatesConstantDefaults)
{
	auto effect = [](uint32_t uniformCount, const std::string& firstDefault) {
		std::string source;
		for (uint32_t index = 0; index < uniformCount; ++index)
			source += "uniform float4 Value" + std::to_string(index) + " = "
					+ (index == 0 ? firstDefault : "float4(0.0, 0.0, 0.0, 0.0)") + ";\n";
		source += "float4 MainPS() : SV_Target { return Value0; }\n"
				"technique Main { pass { PixelShader = MainPS; } }\n";
		return source;
	};
	postprocess::ShaderPassSource source;
	std::string error;
	constexpr uint32_t maxUniforms = postprocess::MaxShaderParameterComponents / 4;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect(maxUniforms, "float4(0.0, 0.0, 0.0, 0.0)"), source, error)) << error;
	EXPECT_EQ(maxUniforms, source.parameters.size());
	EXPECT_FALSE(postprocess::parseReShadeFx(effect(maxUniforms + 1, "float4(0.0, 0.0, 0.0, 0.0)"), source, error));
	EXPECT_NE(std::string::npos, error.find("256-component parameter budget"));
	ASSERT_TRUE(postprocess::parseReShadeFx(effect(1, "float4(1.0 + 2.0, 0.0, 0.0, 0.0)"), source, error)) << error;
	ASSERT_EQ(1u, source.parameters.size());
	EXPECT_FLOAT_EQ(3.f, source.parameters[0].values[0]);
	EXPECT_FALSE(postprocess::parseReShadeFx(effect(1, "float4(Unknown, 0.0, 0.0, 0.0)"), source, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, RejectsInvalidTypedSlangParameterRanges)
{
	const std::string shader =
		"#version 450\n"
		"#pragma parameter float GAIN \"Gain\" 2.0 0.0 1.0 0.1\n"
		"#pragma stage fragment\n"
		"void main() { FragColor = vec4(GAIN); }\n";
	postprocess::SlangShaderSource source;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangShader(shader, source, error));
	EXPECT_NE(std::string::npos, error.find("invalid default, range, or step"));
}

TEST(PostProcessingChainTest, SerializesRichShaderParameterValues)
{
	postprocess::ShaderParameter parameter;
	parameter.name = "Tint";
	parameter.label = "Tint";
	parameter.componentCount = 3;
	parameter.type = postprocess::SlangParameterType::Float3;
	parameter.values = { 0.25f, 0.5f, 0.75f };
	parameter.minimumValues = { 0.f, 0.f, 0.f };
	parameter.maximumValues = { 1.f, 1.f, 1.f };
	parameter.stepValues = { 0.01f, 0.01f, 0.01f };
	parameter.value = parameter.values[0];
	std::vector<postprocess::ShaderParameter> parameters { parameter };
	postprocess::ShaderChain chain;
	chain.parameters = parameters;
	postprocess::applyShaderChainParameterValues(chain,
			postprocess::serializeShaderChainParameterValues(parameters));
	ASSERT_EQ(1u, chain.parameters.size());
	EXPECT_FLOAT_EQ(0.25f, chain.parameters[0].values[0]);
	EXPECT_FLOAT_EQ(0.5f, chain.parameters[0].values[1]);
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].values[2]);
}

TEST(PostProcessingChainTest, LowersReShadeFxResourcesForBackendBinding)
{
	postprocess::ShaderPassSource source;
	std::string error;
	EXPECT_TRUE(postprocess::parseReShadeFx(
			"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
			"sampler2D SourceSampler { Texture = SourceTex; };\n"
			"float4 MainPS(float2 uv : TEXCOORD) : SV_Target { return tex2D(SourceSampler, uv); }\n"
			"technique Main { pass { PixelShader = MainPS; } }\n", source, error));
	EXPECT_TRUE(error.empty());
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ("SourceSampler", source.slangResources[0].name);
}

TEST(PostProcessingChainTest, LowersSelectedReShadeTechniquePassAndVertexStage)
{
	const std::string effect =
		"struct VertexData { float4 position : SV_Position; float2 texcoord : TEXCOORD0; };\n"
		"VertexData MainVS(float4 position : POSITION, float2 texcoord : TEXCOORD0) { VertexData output; output.position = position; output.texcoord = texcoord; return output; }\n"
		"float4 FirstPS() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n"
		"float4 SecondPS() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n"
		"technique Main { pass First { VertexShader = MainVS; PixelShader = FirstPS; } pass Second { PixelShader = SecondPS; } }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	EXPECT_TRUE(source.nativeFxStages);
	EXPECT_FALSE(source.vertex.empty());
	EXPECT_NE(std::string::npos, source.fragment.find("FirstPS"));
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 1, source, error)) << error;
	EXPECT_FALSE(source.vertex.empty());
	EXPECT_NE(std::string::npos, source.fragment.find("SecondPS"));

	const std::string vertexIdEffect =
		"struct Varyings { float2 scale : TEXCOORD1; };\n"
		"void PostProcessVS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0, out Varyings values) { texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0); position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); values.scale = (float2)1.0; }\n"
		"float4 MainPS(float2 texcoord : TEXCOORD0, Varyings values) : SV_Target { return float4(texcoord * values.scale, 0.0, 1.0); }\n"
		"technique Main { pass { VertexShader = PostProcessVS; PixelShader = MainPS; } }\n";
	ASSERT_TRUE(postprocess::compileReShadeFxPass(vertexIdEffect, 0, 0, source, error)) << error;
	EXPECT_NE(std::string::npos, source.vertex.find("gl_VertexID"));
	EXPECT_NE(std::string::npos, source.vertex.find("layout(location = 1)"));
	const std::vector<postprocess::ShaderPassInput> inputs {
		{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
	};
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, source.slangParameters,
			inputs, hlsl, error, source.nativeFxStages)) << error;
#ifdef _WIN32
	ASSERT_TRUE(compileHlsl(hlsl, "vs_4_0", error)) << error;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters,
			inputs, 1, hlsl, error)) << error;
	ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << error;
#endif
#if defined(USE_VULKAN)
	std::string fragment;
	std::string vertexSource;
	ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, source.slangParameters, {}, 1,
			fragment, vertexSource, error)) << error;
	ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vertexSource, error)) << error;
	ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
#endif
}

TEST(PostProcessingChainTest, ReflectsAndRoutesReShadeMultiRenderTargets)
{
	const std::string effect =
		"texture ColorTarget { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };\n"
		"texture BloomTarget { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };\n"
		"struct Outputs { float4 color : SV_Target0; float4 bloom : SV_Target1; };\n"
		"Outputs MainPS() { Outputs output; output.color = (float4)1.0; output.bloom = (float4)0.5; return output; }\n"
		"technique Main { pass { PixelShader = MainPS; RenderTarget0 = ColorTarget; RenderTarget1 = BloomTarget; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	EXPECT_EQ(2u, source.outputCount);
	EXPECT_TRUE(source.nativeFxStages);

	postprocess::ShaderChain chain;
	chain.passes.resize(2);
	chain.passes[0].alias = "ColorTarget";
	chain.passes[0].outputs.push_back({ "ColorTarget" });
	chain.passes[0].outputs.push_back({ "BloomTarget" });
	postprocess::ShaderPassSource consumer;
	postprocess::SlangResource bloomSampler;
	bloomSampler.type = postprocess::SlangResourceType::Sampler2D;
	bloomSampler.convention = postprocess::SlangResourceConvention::PassAlias;
	bloomSampler.name = "BloomSampler";
	bloomSampler.alias = "BloomTarget";
	consumer.slangResources.push_back(std::move(bloomSampler));
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 1, consumer, error)) << error;
	ASSERT_EQ(1u, chain.passes[1].inputs.size());
	EXPECT_EQ(0u, chain.passes[1].inputs[0].sourcePass);
	EXPECT_EQ(1u, chain.passes[1].inputs[0].sourceOutput);
}

TEST(PostProcessingChainTest, CompilesCompleteExternalShaderInventory)
{
	const char *roots = std::getenv("HOLLYCAST_SHADER_TEST_ROOTS");
	if (roots == nullptr || *roots == '\0')
		GTEST_SKIP() << "Set HOLLYCAST_SHADER_TEST_ROOTS to shader folders or preset files separated by semicolons.";
	struct InventoryFixture {
		std::string previousTechnique = config::PostProcessShaderTechnique.get();
		std::filesystem::path directory = std::filesystem::temp_directory_path()
				/ ("hollycast_shader_inventory_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		~InventoryFixture() {
			config::PostProcessShaderTechnique.get() = previousTechnique;
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} fixture;
	std::filesystem::create_directories(fixture.directory);
	const auto middleEffect = fixture.directory / "middle.glsl";
	std::ofstream(middleEffect) << "void main() { FragColor = texture(Source, vTexCoord) * 0.5; }\n";
	std::vector<std::filesystem::path> files;
	std::istringstream rootList(roots);
	for (std::string root; std::getline(rootList, root, ';');) {
		const std::filesystem::path path(root);
		if (std::filesystem::is_regular_file(path))
			files.push_back(path);
		else if (std::filesystem::is_directory(path)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
				if (entry.is_regular_file() && postprocess::detectFormat(entry.path().string()) != postprocess::ShaderFormat::Unknown)
					files.push_back(entry.path());
		}
		else
			ADD_FAILURE() << "Shader inventory path does not exist: " << root;
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	EXPECT_FALSE(files.empty());
#if defined(USE_VULKAN)
	ShaderCompiler::Init();
#endif
	size_t compiledPasses = 0;
	for (const auto& path : files) {
		SCOPED_TRACE(path.string());
		config::PostProcessShaderTechnique.get().clear();
		postprocess::ShaderChain chain;
		std::string error;
		if (!postprocess::loadShaderChain(path.string(), chain, error)) {
			ADD_FAILURE() << error;
			continue;
		}
		std::vector<std::string> techniques = chain.techniques;
		if (techniques.empty())
			techniques.emplace_back();
		for (const std::string& technique : techniques) {
			SCOPED_TRACE(technique);
			config::PostProcessShaderTechnique.get() = technique;
			bool loaded;
			if (technique == "UIMask_Bottom") {
				// This technique restores pixels saved before the intervening
				// effects. It cannot run alone without its matching producer.
				config::PostProcessShaderTechnique.get() = "stack:0=UIMask_Top;2=UIMask_Bottom";
				loaded = postprocess::loadShaderChain(
						std::vector<std::string> { path.string(), middleEffect.string(), path.string() }, chain, error);
			}
			else
				loaded = postprocess::loadShaderChain(path.string(), chain, error);
			if (!loaded) {
				ADD_FAILURE() << error;
				continue;
			}
			for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
				SCOPED_TRACE(passIndex);
				const auto& pass = chain.passes[passIndex];
				const auto& source = chain.sources[passIndex];
				std::vector<postprocess::SlangParameter> parameters = source.slangParameters;
				if (parameters.empty())
					for (const auto& parameter : chain.parameters) {
						if (parameter.layerIndex != pass.layerIndex)
							continue;
						postprocess::SlangParameter typed;
						typed.name = parameter.name;
						typed.type = parameter.type;
						parameters.push_back(std::move(typed));
					}
				std::vector<postprocess::ShaderPassInput> inputs = pass.inputs;
				if (std::none_of(inputs.begin(), inputs.end(), [](const auto& input) { return input.bindingName == "Source"; }))
					inputs.push_back({ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass });
				std::stable_sort(inputs.begin(), inputs.end(), [](const auto& left, const auto& right) {
					return left.bindingName == "Source" && right.bindingName != "Source";
				});
				const uint32_t outputCount = pass.colorOutputCount;
				std::string hlsl;
				if (pass.compute ? postprocess::translateGlslComputeToHlsl(source, parameters, inputs, hlsl, error)
						: postprocess::translateGlslGraphFragmentToHlsl(source, parameters, inputs, outputCount, hlsl, error))
					expectHlslCompiles(hlsl, pass.compute ? "cs_5_0" : "ps_5_0");
				else
					ADD_FAILURE() << "DirectX fragment: " << error;
				if (!source.vertex.empty()) {
					if (postprocess::translateGlslGraphVertexToHlsl(source.vertex, parameters, inputs, hlsl, error,
							source.nativeFxStages, source.retroArchGlsl))
						expectHlslCompiles(hlsl, "vs_4_0");
					else
						ADD_FAILURE() << "DirectX vertex: " << error;
				}
#if defined(USE_VULKAN)
				std::vector<postprocess::ShaderPassInput> bindings(inputs.begin() + 1, inputs.end());
				std::string fragment;
				std::string vertex;
				if (pass.compute ? postprocess::buildVulkanComputeSource(source, parameters, bindings, fragment, error)
						: postprocess::buildVulkanShaderSources(source, parameters, bindings, outputCount, fragment, vertex, error)) {
					EXPECT_TRUE(ShaderCompiler::Validate(pass.compute ? vk::ShaderStageFlagBits::eCompute : vk::ShaderStageFlagBits::eFragment, fragment, error)) << error;
					if (!vertex.empty())
						EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vertex, error)) << error;
				}
				else
					ADD_FAILURE() << "Vulkan stages: " << error;
#endif
				++compiledPasses;
			}
		}
	}
#if defined(USE_VULKAN)
	ShaderCompiler::Term();
#endif
	RecordProperty("shader_files", static_cast<int>(files.size()));
	RecordProperty("shader_passes", static_cast<int>(compiledPasses));
	EXPECT_GT(compiledPasses, 0u);
}
TEST(PostProcessingChainTest, UsesTheReferencedGlslTextureForEarlierPassSizes)
{
	const std::string shader =
		"#ifdef FRAGMENT\n"
		"uniform sampler2D Texture;\n"
		"uniform sampler2D PassPrev1Texture;\n"
		"uniform vec2 PassPrev1InputSize;\n"
		"uniform vec2 PassPrev1TextureSize;\n"
		"uniform vec2 InputSize;\n"
		"varying vec2 TEX0;\n"
		"void main() { gl_FragColor = texture2D(PassPrev1Texture, TEX0.xy)"
		" * vec4(PassPrev1InputSize / PassPrev1TextureSize, InputSize / PassPrev1InputSize); }\n"
		"#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("textureSize(PassPrev1Texture, 0)"));
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	const auto input = std::find_if(chain.passes[2].inputs.begin(), chain.passes[2].inputs.end(),
			[](const postprocess::ShaderPassInput& value) { return value.bindingName == "PassPrev1Texture"; });
	ASSERT_NE(chain.passes[2].inputs.end(), input);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, input->kind);
	EXPECT_EQ(1u, input->sourcePass);
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, {}, chain.passes[2].inputs, 1, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}
TEST(PostProcessingChainTest, CompilesUndefinedGlslFallthroughReturnsForDirectX)
{
	const std::string shader =
		"vec3 mask(float mode) { if (mode == 0.0) return vec3(1.0); if (mode == 1.0) return vec3(0.5); }\n"
		"void main() { gl_FragColor = vec4(mask(SourceSize.x), 1.0); }\n";
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(shader, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
}

TEST(PostProcessingChainTest, PreservesCustomDirectXLocationZeroVaryings)
{
	postprocess::ShaderPassSource source;
	source.vertex = "layout(location=0) out vec2 uv;\nvoid main() { uv = TexCoord; gl_Position = vec4(VertexCoord, 1.0); }";
	source.fragment = "layout(location=0) in vec2 uv;\nvoid main() { gl_FragColor = texture(Source, uv); }";
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, {}, {}, 1, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "ps_4_0");
	ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, {}, {}, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "vs_4_0");
}

namespace {
struct ShaderResourceDirectory {
	std::filesystem::path path = std::filesystem::temp_directory_path()
			/ ("hollycast_shader_resources_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ShaderResourceDirectory() { std::filesystem::create_directory(path); }
	~ShaderResourceDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
constexpr char ComputeGradientEffect[] =
	"uniform float weights[2] = { 0.25, 0.75 };\n"
	"texture2D Image { Width = 16; Height = 8; Format = RGBA32F; };\n"
	"storage2D WriteImage { Texture = Image; };\n"
	"sampler2D ReadImage { Texture = Image; MinFilter = POINT; MagFilter = POINT; };\n"
	"[numthreads(8,4,1)] void Compute(uint3 id : SV_DispatchThreadID) { tex2Dstore(WriteImage, int2(id.xy), float4(weights[0], weights[1], id.x, id.y)); }\n"
	"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(ReadImage, uv); }\n"
	"technique ComputeGradient { pass Write { ComputeShader = Compute; DispatchSizeX = 2; DispatchSizeY = 2; }"
	" pass Modify { ComputeShader = Compute<8,4>; DispatchSizeX = 2; DispatchSizeY = 2; }"
	" pass Present { PixelShader = Pixel; } }\n";
} // namespace

TEST(PostProcessingChainTest, PreservesComputeTextureVersionsAndDispatchGroupCounts)
{
	ShaderResourceDirectory directory;
	const auto path = directory.path / "compute.fx";
	std::ofstream(path) << ComputeGradientEffect;
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(path.string(), chain, error)) << error;
	ASSERT_EQ(3u, chain.passes.size());
	const auto& first = chain.passes[0];
	const auto& second = chain.passes[1];
	EXPECT_TRUE(first.compute);
	EXPECT_FALSE(first.updatesSource);
	EXPECT_FALSE(second.updatesSource);
	EXPECT_TRUE(chain.passes[2].updatesSource);
	ASSERT_EQ(1u, first.outputs.size());
	EXPECT_EQ(1u, first.outputs[0].previousPass);
	EXPECT_TRUE(first.outputs[0].previousFrame);
	EXPECT_EQ(0u, second.outputs[0].previousPass);
	EXPECT_FALSE(second.outputs[0].previousFrame);
	ASSERT_EQ(1u, chain.passes[2].inputs.size());
	EXPECT_EQ(1u, chain.passes[2].inputs[0].sourcePass);
	postprocess::ShaderFrameInfo frame { 640, 480, 1280, 720, 3 };
	int width, height;
	ASSERT_TRUE(postprocess::resolveTargetSize(first, frame, width, height, error)) << error;
	EXPECT_EQ(16, width); EXPECT_EQ(8, height);
	std::array<uint32_t, 3> groups;
	ASSERT_TRUE(postprocess::resolveShaderDispatchGroups(first, frame, groups, error)) << error;
	EXPECT_EQ((std::array<uint32_t, 3>{2, 2, 1}), groups);
	const auto& source = chain.sources[0];
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslComputeToHlsl(source, source.slangParameters, {}, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "cs_5_0");
#if defined(USE_VULKAN)
	std::string compute;
	ASSERT_TRUE(postprocess::buildVulkanComputeSource(source, source.slangParameters, {}, compute, error)) << error;
	ShaderCompiler::Init();
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eCompute, compute, error)) << error;
	ShaderCompiler::Term();
#endif
}

TEST(PostProcessingChainTest, LoadsSlangComputeOutputsAndRoundedDispatchGeometry)
{
	ShaderResourceDirectory directory;
	const auto path = directory.path / "compute.slang";
	std::ofstream(path) << "#version 450\n#pragma stage compute\n"
		"layout(local_size_x=8, local_size_y=4, local_size_z=1) in;\n"
		"layout(set=0, binding=3, rgba32f) uniform image2D Result;\n"
		"void main() { imageStore(Result, ivec2(gl_GlobalInvocationID.xy), vec4(1)); }\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(path.string(), chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	const auto& pass = chain.passes[0];
	EXPECT_TRUE(pass.compute); EXPECT_TRUE(pass.updatesSource);
	EXPECT_EQ(0u, pass.colorOutputCount);
	std::array<uint32_t, 3> groups;
	ASSERT_TRUE(postprocess::resolveShaderDispatchGroups(pass, {17, 9, 17, 9}, groups, error)) << error;
	EXPECT_EQ((std::array<uint32_t, 3>{3, 3, 1}), groups);
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslComputeToHlsl(chain.sources[0], {}, {}, hlsl, error)) << error;
	expectHlslCompiles(hlsl, "cs_5_0");
#if defined(USE_VULKAN)
	std::string compute;
	ASSERT_TRUE(postprocess::buildVulkanComputeSource(chain.sources[0], {}, {}, compute, error)) << error;
	ShaderCompiler::Init();
	EXPECT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eCompute, compute, error)) << error;
	ShaderCompiler::Term();
#endif
}

TEST(PostProcessingChainTest, ProvidesRealTimeAndKeyMouseSourceSemantics)
{
	const std::string effect =
		"uniform float Timer < source = \"timer\"; >;\n"
		"uniform float Delta < source = \"frametime\"; >;\n"
		"uniform bool Held < source = \"key\"; keycode = 0x41; >;\n"
		"uniform bool Pressed < source = \"key\"; keycode = 0x41; mode = \"press\"; >;\n"
		"uniform bool Toggled < source = \"key\"; keycode = 0x41; mode = \"toggle\"; >;\n"
		"uniform float2 Point < source = \"mousepoint\"; >;\n"
		"uniform float2 Movement < source = \"mousedelta\"; >;\n"
		"uniform float2 Wheel < source = \"mousewheel\"; min = 0; max = 10; > = float2(1, 0);\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return Timer + Delta + Held + Pressed + Toggled + Point.x + Movement.x + Wheel.x; }\n"
		"technique Inputs { pass { PixelShader = Pixel; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(8u, source.parameters.size());
	auto& parameters = source.parameters;
	postprocess::setShaderHostKey(4, false);
	postprocess::setShaderHostMouse(10, 20, 0);
	postprocess::updateShaderHostParameters(parameters, {});
	EXPECT_GE(parameters[0].value, 0.f); EXPECT_GE(parameters[1].value, 0.f);
	postprocess::setShaderHostKey(4, true);
	postprocess::setShaderHostMouse(13, 24, 0, 2);
	postprocess::updateShaderHostParameters(parameters, {});
	EXPECT_FLOAT_EQ(1.f, parameters[2].value); EXPECT_FLOAT_EQ(1.f, parameters[3].value);
	const float toggle = parameters[4].value;
	EXPECT_FLOAT_EQ(13.f, parameters[5].values[0]); EXPECT_FLOAT_EQ(24.f, parameters[5].values[1]);
	EXPECT_FLOAT_EQ(3.f, parameters[6].values[0]); EXPECT_FLOAT_EQ(4.f, parameters[6].values[1]);
	EXPECT_FLOAT_EQ(3.f, parameters[7].values[0]); EXPECT_FLOAT_EQ(2.f, parameters[7].values[1]);
	postprocess::updateShaderHostParameters(parameters, {});
	EXPECT_FLOAT_EQ(1.f, parameters[2].value); EXPECT_FLOAT_EQ(0.f, parameters[3].value);
	EXPECT_FLOAT_EQ(toggle, parameters[4].value);
	EXPECT_FLOAT_EQ(3.f, parameters[7].values[0]); EXPECT_FLOAT_EQ(0.f, parameters[7].values[1]);
	postprocess::setShaderHostKey(4, false);
	postprocess::setShaderHostKey(4, true);
	postprocess::updateShaderHostParameters(parameters, {});
	EXPECT_FLOAT_EQ(1.f - toggle, parameters[4].value);
	postprocess::setShaderHostKey(4, false);
}

TEST(PostProcessingChainTest, PreflightsMissingImagesAndDecodesCubeFacesAndIntegerSlices)
{
	ShaderResourceDirectory directory;
	postprocess::ShaderPresetTexture texture;
	texture.name = "Required"; texture.path = (directory.path / "required.tga").string();
	postprocess::ShaderChain chain; chain.textures.push_back(texture);
	std::string error;
	EXPECT_FALSE(postprocess::preflightShaderAssets(chain, error));
	EXPECT_NE(std::string::npos, error.find("required.tga"));
	std::vector<uint8_t> tga(18 + 6 * 4, 0);
	tga[2] = 2; tga[12] = 6; tga[14] = 1; tga[16] = 32; tga[17] = 0x28;
	for (unsigned face = 0; face < 6; ++face) { tga[18 + face * 4 + 2] = static_cast<uint8_t>(face * 20); tga[18 + face * 4 + 3] = 255; }
	std::ofstream(texture.path, std::ios::binary).write(reinterpret_cast<const char*>(tga.data()), tga.size());
	texture.type = postprocess::SlangResourceType::SamplerCube;
	postprocess::ShaderTextureData data;
	ASSERT_TRUE(postprocess::loadShaderTexture(texture, data, error)) << error;
	EXPECT_EQ(1u, data.width); EXPECT_EQ(1u, data.height); EXPECT_EQ(6u, data.layers);
	ASSERT_EQ(24u, data.pixels.size()); EXPECT_EQ(100u, data.pixels[20]);
	texture.type = postprocess::SlangResourceType::Sampler3D;
	texture.width = texture.height = 1; texture.depth = 6;
	texture.numericType = postprocess::ShaderNumericType::UnsignedInteger;
	ASSERT_TRUE(postprocess::loadShaderTexture(texture, data, error)) << error;
	EXPECT_EQ(6u, data.depth); EXPECT_EQ("R32G32B32A32_UINT", data.formatName);
	ASSERT_EQ(96u, data.pixels.size());
	uint32_t red; std::memcpy(&red, data.pixels.data() + 5 * 16, sizeof(red)); EXPECT_EQ(100u, red);
	// Header dimensions are rejected before decoding a potentially huge image.
	tga[12] = tga[13] = tga[14] = tga[15] = 255;
	std::ofstream(texture.path, std::ios::binary).write(reinterpret_cast<const char*>(tga.data()), tga.size());
	EXPECT_FALSE(postprocess::loadShaderTexture(texture, data, error));
}

TEST(PostProcessingChainTest, DecodesThreeDimensionalCubeLutsWithValidatedEntryCounts)
{
	ShaderResourceDirectory directory;
	const auto path = directory.path / "lookup.cube";
	{
		std::ofstream file(path);
		file << "TITLE \"test\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
		for (int i = 0; i < 8; ++i) file << (i & 1) << ' ' << ((i >> 1) & 1) << ' ' << ((i >> 2) & 1) << '\n';
	}
	postprocess::ShaderPresetTexture texture;
	texture.path = path.string(); texture.type = postprocess::SlangResourceType::Sampler3D;
	postprocess::ShaderTextureData data;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderTexture(texture, data, error)) << error;
	EXPECT_EQ(2u, data.width); EXPECT_EQ(2u, data.height); EXPECT_EQ(2u, data.depth);
	EXPECT_EQ("R32G32B32A32_SFLOAT", data.formatName);
	ASSERT_EQ(8u * 4u * sizeof(float), data.pixels.size());
	float last[4]; std::memcpy(last, data.pixels.data() + data.pixels.size() - sizeof(last), sizeof(last));
	for (float component : last) EXPECT_FLOAT_EQ(1.f, component);
	std::ofstream(path, std::ios::app) << "1 1 1\n";
	EXPECT_FALSE(postprocess::loadShaderTexture(texture, data, error));
}

TEST(PostProcessingChainTest, RoutesSourceMipmapsAroundAuxiliaryEffectPasses)
{
	postprocess::ShaderChain chain; chain.passes.resize(3);
	chain.passes[1].updatesSource = false;
	chain.passes[2].useMipmaps = true;
	const auto mipmaps = postprocess::resolveShaderPassOutputMipmaps(chain);
	EXPECT_TRUE(mipmaps[0][0]); EXPECT_FALSE(mipmaps[1][0]);
}

TEST(PostProcessingChainTest, WaitsForTheLoadingNoticeToReachAPreviousFrame)
{
	postprocess::finishShaderCompilation();
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	EXPECT_TRUE(postprocess::shaderCompilationPending());
	// Advancing frames without drawing the notice must not release the work.
	postprocess::advanceShaderCompilationFrame();
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	postprocess::markShaderCompilationDrawn();
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	postprocess::advanceShaderCompilationFrame();
	EXPECT_TRUE(postprocess::prepareShaderCompilation());
	postprocess::reportShaderChainStatus("active", "Compilation failed");
	EXPECT_FALSE(postprocess::shaderCompilationPending());
	postprocess::reportShaderChainStatus({}, {});
}

TEST(PostProcessingChainTest, KeepsLoadingVisibleAcrossOldFramesAndRearmsAfterCancellation)
{
	postprocess::reportShaderChainStatus("old", {});
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	postprocess::reportShaderFrameStatus("old", {});
	EXPECT_TRUE(postprocess::shaderCompilationPending());
	postprocess::markShaderCompilationDrawn();
	postprocess::finishShaderCompilation();
	postprocess::advanceShaderCompilationFrame();
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	postprocess::markShaderCompilationDrawn();
	postprocess::advanceShaderCompilationFrame();
	EXPECT_TRUE(postprocess::prepareShaderCompilation());
	postprocess::reportShaderChainStatus("new", {});
	EXPECT_TRUE(postprocess::shaderCompilationPending());
	postprocess::reportShaderFrameStatus("old", {});
	EXPECT_TRUE(postprocess::shaderCompilationPending());
	postprocess::reportShaderFrameStatus("new", {});
	EXPECT_FALSE(postprocess::shaderCompilationPending());
	postprocess::reportShaderChainStatus({}, {});
}

TEST(PostProcessingChainTest, UnblocksLoadingWhenTheActivatedGraphCannotRenderAFrame)
{
	postprocess::finishShaderCompilation();
	EXPECT_FALSE(postprocess::prepareShaderCompilation());
	postprocess::markShaderCompilationDrawn();
	postprocess::advanceShaderCompilationFrame();
	ASSERT_TRUE(postprocess::prepareShaderCompilation());
	postprocess::reportShaderChainStatus("new", {});
	EXPECT_TRUE(postprocess::shaderCompilationPending());
	postprocess::reportShaderFrameStatus("new", "Scene depth unavailable");
	EXPECT_FALSE(postprocess::shaderCompilationPending());
	EXPECT_EQ("Scene depth unavailable", postprocess::getShaderChainStatus().error);
	postprocess::reportShaderChainStatus({}, {});
}

TEST(PostProcessingChainTest, RecoversFrameErrorsWithoutLosingControlsOrHidingRejectedReloads)
{
	postprocess::ShaderParameter gain;
	gain.name = gain.label = "Gain";
	gain.minimum = 0.f; gain.maximum = 1.f; gain.step = 0.1f;
	postprocess::reportShaderChainStatus("active", {}, { gain }, { "Main" }, 0);
	postprocess::reportShaderFrameStatus("active", "Depth unavailable");
	EXPECT_TRUE(postprocess::getShaderChainStatus().frameError);
	ASSERT_TRUE(postprocess::setShaderChainParameter("Gain", 0.7f));
	postprocess::reportShaderFrameStatus("active", {});
	auto status = postprocess::getShaderChainStatus();
	ASSERT_EQ(1u, status.parameters.size());
	EXPECT_FLOAT_EQ(0.7f, status.parameters[0].value);
	EXPECT_EQ(0, status.selectedTechnique);
	EXPECT_TRUE(status.error.empty());
	EXPECT_FALSE(status.frameError);
	postprocess::reportShaderChainStatus("active", "Reload rejected");
	postprocess::reportShaderFrameStatus("active", {});
	EXPECT_EQ("Reload rejected", postprocess::getShaderChainStatus().error);
	postprocess::reportShaderChainStatus({}, {});
}

TEST(PostProcessingChainTest, ResolvesOnlyActiveIncludesAndPermitsGuardedRecursion)
{
	ShaderResourceDirectory directory;
	std::ofstream(directory.path / "a.inc") <<
		"#ifndef HEADER_A // guard comments are legal\n#define HEADER_A\n#include \"b.inc\"\n#define GAIN 0.5\n#endif\n";
	std::ofstream(directory.path / "b.inc") << "#pragma once\n#include \"a.inc\"\n";
	for (const char* extension : { ".glsl", ".slang", ".fx" }) {
		const auto path = directory.path / (std::string("guarded") + extension);
		std::ofstream file(path);
		if (std::string(extension) == ".slang") file << "#version 450\n";
		file << "#if 0\n#include \"missing.inc\"\n#endif\n"
			"#define SUM(x, y) ((x) + (y))\n#define HEADER \"a.inc\"\n"
			"#if SUM(1, 2) == 3\n#include HEADER // active macro include\n#endif\n";
		if (std::string(extension) == ".fx")
			file << "float4 Pixel() : SV_Target { return GAIN; }\ntechnique Main { pass { PixelShader = Pixel; } }\n";
		else
			file << "void main() { FragColor = vec4(GAIN); }\n";
		file.close();
		postprocess::ShaderChain chain;
		std::string error, hlsl;
		ASSERT_TRUE(postprocess::loadShaderChain(path.string(), chain, error)) << extension << ": " << error;
		ASSERT_EQ(1u, chain.sources.size());
		ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(chain.sources[0], {}, chain.passes[0].inputs, 1, hlsl, error)) << error;
		expectHlslCompiles(hlsl, "ps_5_0");
	}
}

TEST(PostProcessingChainTest, RejectsActiveMissingEscapedAndUnguardedIncludes)
{
	ShaderResourceDirectory directory;
	for (const char* extension : { ".glsl", ".slang", ".fx" }) {
		const auto path = directory.path / (std::string("invalid") + extension);
		for (const char* include : { "missing.inc", "../escaped.inc", "cycle.inc" }) {
			std::ofstream(directory.path / "cycle.inc") << "#include \"cycle.inc\"\n";
			std::ofstream(path) << "#include \"" << include << "\"\n";
			postprocess::ShaderChain chain;
			std::string error;
			EXPECT_FALSE(postprocess::loadShaderChain(path.string(), chain, error)) << extension << include;
			EXPECT_FALSE(error.empty());
		}
	}
}

TEST(PostProcessingChainTest, KeepsIndependentIncludeStateForGlslAndSlangStages)
{
	ShaderResourceDirectory directory;
	std::ofstream(directory.path / "shared.inc") << "#pragma once\nfloat stageValue() { return 0.5; }\n";
	for (const char* extension : { ".glsl", ".slang" }) {
		const bool slang = std::string(extension) == ".slang";
		const auto path = directory.path / (std::string("stages") + extension);
		std::ofstream file(path);
		if (slang) file << "#version 450\n";
		file << (slang ? "#pragma stage vertex\n" : "#if defined(VERTEX)\n")
			<< "#include \"shared.inc\"\n#define VERTEX_ONLY\n"
			"void main() { gl_Position = vec4(stageValue()); }\n"
			<< (slang ? "#pragma stage fragment\n" : "#elif defined(FRAGMENT)\n")
			<< "#ifdef VERTEX_ONLY\n#include \"must-not-load.inc\"\n#endif\n"
			"#include \"shared.inc\"\nvoid main() { FragColor = vec4(stageValue()); }\n"
			<< (slang ? "" : "#endif\n");
		file.close();
		postprocess::ShaderChain chain;
		std::string error;
		ASSERT_TRUE(postprocess::loadShaderChain(path.string(), chain, error)) << extension << ": " << error;
		ASSERT_EQ(1u, chain.sources.size());
		EXPECT_NE(std::string::npos, chain.sources[0].vertex.find("float stageValue()"));
		EXPECT_NE(std::string::npos, chain.sources[0].fragment.find("float stageValue()"));
	}
}

TEST(PostProcessingChainTest, UsesDefaultLibraryBoundaryForSiblingPresetSources)
{
	ShaderResourceDirectory directory;
	struct RestoreConfig {
		std::string data = get_writable_data_path("");
		std::string library = config::PostProcessShaderDirectory.get();
		~RestoreConfig() { set_user_data_dir(data); config::PostProcessShaderDirectory.get() = library; }
	} restore;
	set_user_data_dir(directory.path.string());
	config::PostProcessShaderDirectory.get().clear();
	const auto library = directory.path / "shaders";
	std::filesystem::create_directories(library / "presets");
	std::filesystem::create_directories(library / "shared");
	std::ofstream(library / "shared" / "gain.inc") << "#define GAIN 0.5\n";
	std::ofstream(library / "shared" / "pass.slang") << "#version 450\n#include \"gain.inc\"\nvoid main() { FragColor = vec4(GAIN); }\n";
	const auto preset = library / "presets" / "example.slangp";
	std::ofstream(preset) << "shaders = 1\nshader0 = ../shared/pass.slang\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(preset.string(), chain, error)) << error;
	EXPECT_TRUE(std::filesystem::equivalent(library, chain.passes[0].packageRoot));
}

TEST(PostProcessingChainTest, ReusesPreparedSourcesAndImagesOnlyUntilExplicitReload)
{
	ShaderResourceDirectory directory;
	struct RestoreConfig {
		std::vector<std::string> paths = config::PostProcessShaders.get();
		std::string technique = config::PostProcessShaderTechnique.get();
		~RestoreConfig() {
			config::PostProcessShaders.get() = paths;
			config::PostProcessShaderTechnique.get() = technique;
			postprocess::requestShaderChainReload();
		}
	} restore;
	const auto path = directory.path / "cached.glsl";
	std::ofstream(path) << "void main() { FragColor = vec4(0.5); }\n";
	config::PostProcessShaders.get() = { path.string() };
	config::PostProcessShaderTechnique.get().clear();
	postprocess::ShaderChain prepared;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(path.string(), prepared, error)) << error;
	const auto image = directory.path / "lookup.tga";
	std::vector<uint8_t> tga(22, 255);
	std::fill_n(tga.begin(), 18, 0);
	tga[2] = 2; tga[12] = tga[14] = 1; tga[16] = 32; tga[17] = 0x28;
	std::ofstream(image, std::ios::binary).write(reinterpret_cast<const char*>(tga.data()), tga.size());
	prepared.textures.push_back({ "Lookup", image.string() });
	ASSERT_TRUE(postprocess::preflightShaderAssets(prepared, error)) << error;
	const auto pixels = prepared.textures[0].decoded;
	ASSERT_TRUE(pixels);
	postprocess::requestShaderChainReload(&prepared);
	std::filesystem::remove(path);
	std::filesystem::remove(image);
	postprocess::ShaderChain active;
	const std::vector<std::string> paths { path.string() };
	ASSERT_TRUE(postprocess::loadConfiguredShaderChain(paths, postprocess::shaderChainReloadGeneration(), active, error)) << error;
	ASSERT_EQ(1u, active.textures.size());
	EXPECT_EQ(pixels, postprocess::loadShaderTextureData(active.textures[0], error));
	postprocess::requestShaderChainReload(&prepared);
	postprocess::requestShaderChainReload();
	EXPECT_FALSE(postprocess::loadConfiguredShaderChain(paths, postprocess::shaderChainReloadGeneration(), active, error));
}

TEST(PostProcessingChainTest, TranslatesForSupportedDirectXFeatureLevels)
{
	postprocess::ShaderPassSource source;
	source.fragment = "void main() { FragColor = texture(Source, vTexCoord); }";
	std::string hlsl, error;
	for (uint32_t model : { 40u, 41u, 50u }) {
		ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, {}, {}, 1, hlsl, error, model)) << error;
		expectHlslCompiles(hlsl, model == 40 ? "ps_4_0" : model == 41 ? "ps_4_1" : "ps_5_0");
	}
}

#if defined(_WIN32) && defined(HAVE_D3D11)
TEST(PostProcessingChainTest, ExecutesTranslatedComputeShaderAndReadsBackArrayParameters)
{
	using Microsoft::WRL::ComPtr;
	postprocess::ShaderPassSource source;
	std::string error, hlsl;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(ComputeGradientEffect, 0, 0, source, error)) << error;
	ASSERT_TRUE(postprocess::translateGlslComputeToHlsl(source, source.slangParameters, {}, hlsl, error)) << error;
	ComPtr<ID3DBlob> code, diagnostics;
	HRESULT result = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &diagnostics);
	ASSERT_TRUE(SUCCEEDED(result)) << (diagnostics ? static_cast<const char*>(diagnostics->GetBufferPointer()) : "D3DCompile failed");
	ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
	const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
	result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &requested, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
	ASSERT_TRUE(SUCCEEDED(result)) << "Unable to create the DirectX software device: " << result;
	ComPtr<ID3D11ComputeShader> compute;
	ASSERT_TRUE(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compute)));
	D3D11_TEXTURE2D_DESC description {};
	description.Width = 16; description.Height = 8; description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
	description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	ComPtr<ID3D11Texture2D> image;
	ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &image)));
	ComPtr<ID3D11UnorderedAccessView> output;
	ASSERT_TRUE(SUCCEEDED(device->CreateUnorderedAccessView(image.Get(), nullptr, &output)));
	std::array<float, 12 + postprocess::MaxShaderParameterComponents> uniforms {};
	uint32_t offset = 12;
	for (const auto& parameter : source.parameters) {
		std::copy_n(parameter.values.begin(), parameter.componentCount, uniforms.begin() + offset); offset += parameter.componentCount;
	}
	D3D11_BUFFER_DESC bufferDescription {};
	bufferDescription.ByteWidth = sizeof(uniforms); bufferDescription.Usage = D3D11_USAGE_IMMUTABLE; bufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA initial { uniforms.data(), 0, 0 };
	ComPtr<ID3D11Buffer> constants;
	ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&bufferDescription, &initial, &constants)));
	context->CSSetShader(compute.Get(), nullptr, 0);
	ID3D11Buffer *rawConstants = constants.Get(); context->CSSetConstantBuffers(0, 1, &rawConstants);
	ID3D11UnorderedAccessView *rawOutput = output.Get(); context->CSSetUnorderedAccessViews(0, 1, &rawOutput, nullptr);
	context->Dispatch(2, 2, 1);
	rawOutput = nullptr; context->CSSetUnorderedAccessViews(0, 1, &rawOutput, nullptr);
	description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> readback;
	ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &readback)));
	context->CopyResource(readback.Get(), image.Get());
	D3D11_MAPPED_SUBRESOURCE mapped {};
	ASSERT_TRUE(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
	const float *pixel = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) + 7 * mapped.RowPitch + 15 * sizeof(float) * 4);
	EXPECT_FLOAT_EQ(0.25f, pixel[0]); EXPECT_FLOAT_EQ(0.75f, pixel[1]); EXPECT_FLOAT_EQ(15.f, pixel[2]); EXPECT_FLOAT_EQ(7.f, pixel[3]);
	context->Unmap(readback.Get(), 0);
}
#endif
