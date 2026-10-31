/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast, distributed under the GNU General Public
	License, version 2 or (at your option) any later version.
*/
#include "rend/postprocess/shader_chain.h"
#include "cfg/option.h"
#include "archive/ZipArchive.h"
#include "oslib/resources.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

class ShaderLayerTest : public testing::Test {
protected:
	std::filesystem::path directory = std::filesystem::temp_directory_path()
			/ ("hollycast_effect_layers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::string previousTechnique = config::PostProcessShaderTechnique.get();
	std::string previousDirectory = config::PostProcessShaderDirectory.get();
	std::vector<std::string> previousPaths = config::PostProcessShaders.get();

	void SetUp() override { std::filesystem::create_directories(directory); }
	void TearDown() override {
		config::PostProcessShaderTechnique.get() = previousTechnique;
		config::PostProcessShaderDirectory.get() = previousDirectory;
		config::PostProcessShaders.get() = previousPaths;
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	std::string writeEffect(const char *filename) {
		const auto path = directory / filename;
		std::ofstream(path) <<
			"texture BackupTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; };\n"
			"sampler Backup { Texture = BackupTex; };\n"
			"float4 SavePS() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n"
			"float4 RestorePS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(Backup, uv); }\n"
			"technique Top { pass { PixelShader = SavePS; RenderTarget = BackupTex; } }\n"
			"technique Bottom { pass { PixelShader = RestorePS; } }\n";
		return path.string();
	}
};

TEST_F(ShaderLayerTest, InstallsTheCompleteReleaseShaderLibrary)
{
	const auto library = directory / "shaders";
	std::string error;
	ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
	config::PostProcessShaderDirectory.get() = library.string();
	config::PostProcessShaderTechnique.get().clear();
	unsigned selectable = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(library)) {
		if (!entry.is_regular_file() || postprocess::detectFormat(entry.path().string()) == postprocess::ShaderFormat::Unknown)
			continue;
		SCOPED_TRACE(entry.path().string());
		postprocess::ShaderChain chain;
		ASSERT_TRUE(postprocess::loadShaderChain(entry.path().string(), chain, error)) << error;
		EXPECT_FALSE(chain.passes.empty());
		++selectable;
	}
	EXPECT_EQ(36u, selectable);
	EXPECT_TRUE(std::filesystem::is_regular_file(library / "shaders/ReShade.fxh"));
	EXPECT_TRUE(std::filesystem::is_regular_file(library / "Textures/lut.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(library / "Textures/crt-newpixie/crtframe.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(library / "Textures/UIMask.png"));
	EXPECT_TRUE(std::filesystem::is_regular_file(library / "LICENSES.txt"));
}

TEST_F(ShaderLayerTest, PreservesUserShadersAndRepairsMissingBundledAssets)
{
	const auto library = directory / "shaders";
	std::string error;
	ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
	const std::string custom = "// user-edited shader\nvoid main() {}\n";
	std::ofstream(library / "scanlines.glsl") << custom;
	std::ofstream(library / "my-shader.glsl") << custom;
	ASSERT_TRUE(std::filesystem::remove(library / "Textures/lut.png"));
	ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
	for (const char *name : { "scanlines.glsl", "my-shader.glsl" }) {
		std::ifstream file(library / name);
		EXPECT_EQ(custom, std::string(std::istreambuf_iterator<char>(file), {}));
	}
	EXPECT_GT(std::filesystem::file_size(library / "Textures/lut.png"), 0u);
}

TEST_F(ShaderLayerTest, UpgradesOnlyUnmodifiedBundledShaders)
{
	const auto library = directory / "shaders";
	std::string error;
	ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
	size_t archiveSize = 0;
	auto archiveData = resource::load("shaders/library.zip", archiveSize);
	ZipArchive archive;
	ASSERT_TRUE(archiveData && archive.Open(archiveData.get(), archiveSize));
	for (const char *name : { "CRT-HYLLIAN.glsl", "shaders/deband.glsl" }) {
		SCOPED_TRACE(name);
		std::unique_ptr<ArchiveFile> previous(archive.OpenFile(("previous/" + std::string(name)).c_str()));
		ASSERT_NE(nullptr, previous);
		std::vector<char> contents(previous->length());
		ASSERT_EQ(contents.size(), previous->Read(contents.data(), static_cast<u32>(contents.size())));
		{
			std::ofstream file(library / name, std::ios::binary);
			file.write(contents.data(), contents.size());
		}
		ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
		{
			std::ifstream file(library / name, std::ios::binary);
			EXPECT_NE(std::string(contents.begin(), contents.end()), std::string(std::istreambuf_iterator<char>(file), {}));
		}
		// Equal lengths still require an exact match before replacing a file.
		std::string custom(contents.begin(), contents.end());
		ASSERT_GT(custom.size(), 5u);
		custom[5] = custom[5] == 'X' ? 'Y' : 'X';
		{
			std::ofstream file(library / name, std::ios::binary);
			file << custom;
		}
		ASSERT_TRUE(postprocess::installBundledShaders(library.string(), error)) << error;
		std::ifstream file(library / name, std::ios::binary);
		EXPECT_EQ(custom, std::string(std::istreambuf_iterator<char>(file), {}));
	}
	EXPECT_FALSE(std::filesystem::exists(library / "previous"));
}

TEST_F(ShaderLayerTest, DoesNotInstallThroughAnEscapingDirectorySymlink)
{
	const auto library = directory / "shaders";
	const auto outside = directory / "outside";
	std::filesystem::create_directories(library);
	std::filesystem::create_directories(outside);
	std::error_code errorCode;
	std::filesystem::create_directory_symlink(outside, library / "Textures", errorCode);
	if (errorCode)
		GTEST_SKIP() << "Directory symlinks are unavailable: " << errorCode.message();
	std::string error;
	EXPECT_FALSE(postprocess::installBundledShaders(library.string(), error));
	EXPECT_FALSE(error.empty());
	EXPECT_TRUE(std::filesystem::is_empty(outside));
}

TEST_F(ShaderLayerTest, SharesNamedTargetsAcrossTechniquesOfTheSameEffect)
{
	const std::string effect = writeEffect("mask.fx");
	const auto middle = directory / "middle.glsl";
	std::ofstream(middle) << "void main() { FragColor = texture(Source, vTexCoord) * 0.5; }\n";
	const std::vector<std::string> paths { effect, middle.string(), effect };
	config::PostProcessShaders.get() = paths;
	EXPECT_EQ(paths, postprocess::configuredShaderPaths());
	config::PostProcessShaderTechnique.get() = "stack:0=Top;2=Bottom";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(paths, chain, error)) << error;
	ASSERT_EQ(3u, chain.passes.size());
	const auto backup = std::find_if(chain.passes[2].inputs.begin(), chain.passes[2].inputs.end(),
			[](const postprocess::ShaderPassInput& input) { return input.resourceName == "BackupTex"; });
	ASSERT_NE(chain.passes[2].inputs.end(), backup);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, backup->kind);
	EXPECT_EQ(0u, backup->sourcePass);
	EXPECT_EQ(0u, backup->sourceOutput);
	ASSERT_EQ(2u, chain.techniqueGroups.size());
	EXPECT_EQ(2u, chain.techniqueGroups[1].layerIndex);
}

TEST_F(ShaderLayerTest, DoesNotShareTargetsWithAnUnrelatedEffect)
{
	const std::string first = writeEffect("first.fx");
	const std::string second = writeEffect("second.fx");
	config::PostProcessShaderTechnique.get() = "stack:0=Top;1=Bottom";
	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::loadShaderChain(std::vector<std::string> { first, second }, chain, error));
	EXPECT_NE(std::string::npos, error.find("same effect"));
	config::PostProcessShaderTechnique.get() = "Bottom";
	EXPECT_FALSE(postprocess::loadShaderChain(first, chain, error));
}

TEST_F(ShaderLayerTest, KeepsRepeatedShaderParameterOverridesSeparate)
{
	const auto path = directory / "gain.slang";
	std::ofstream(path) <<
		"#version 450\n#pragma parameter GAIN \"Gain\" 0.5 0.0 1.0 0.1\n"
		"uniform sampler2D Source;\nvoid main() { FragColor = texture(Source, vTexCoord) * GAIN; }\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(std::vector<std::string> { path.string(), path.string() }, chain, error)) << error;
	postprocess::applyShaderChainParameterValues(chain, "Layer1__GAIN=0.2;Layer2__GAIN=0.8");
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_FLOAT_EQ(0.2f, chain.parameters[0].value);
	EXPECT_FLOAT_EQ(0.8f, chain.parameters[1].value);
	ASSERT_EQ(2u, chain.sources.size());
	ASSERT_EQ(1u, chain.sources[0].slangParameters.size());
	ASSERT_EQ(1u, chain.sources[1].slangParameters.size());
	EXPECT_FLOAT_EQ(0.2f, chain.sources[0].slangParameters[0].defaultValue.values[0]);
	EXPECT_FLOAT_EQ(0.8f, chain.sources[1].slangParameters[0].defaultValue.values[0]);
}

TEST_F(ShaderLayerTest, AcceptsNativeAndLegacyFxBlendFactorSpellings)
{
	for (const char *factor : { "SRCALPHA", "SRC_ALPHA" }) {
		const auto path = directory / "blend.fx";
		std::ofstream(path) <<
			"float4 MainPS() : SV_Target { return float4(1.0, 1.0, 1.0, 0.5); }\n"
			"technique Blend { pass { PixelShader = MainPS; BlendEnable = true; SrcBlend = "
			<< factor << "; DestBlend = INVSRCALPHA; } }\n";
		postprocess::ShaderChain chain;
		std::string error;
		config::PostProcessShaderTechnique.get().clear();
		ASSERT_TRUE(postprocess::loadShaderChain(path.string(), chain, error)) << error;
		ASSERT_EQ(1u, chain.passes.size());
		EXPECT_TRUE(chain.passes[0].blendEnabled);
		EXPECT_EQ(postprocess::ShaderBlendFactor::SourceAlpha, chain.passes[0].sourceBlend);
		EXPECT_EQ(postprocess::ShaderBlendFactor::OneMinusSourceAlpha, chain.passes[0].destinationBlend);
	}
}
TEST_F(ShaderLayerTest, KeepsDifferentLookupTypesSeparateAcrossLayers)
{
	const char* types[] { "sampler2D", "usampler2D", "sampler3D" };
	std::vector<std::string> paths;
	for (unsigned i = 0; i < 3; ++i) {
		const auto shader = directory / (std::to_string(i) + ".slang");
		std::ofstream(shader) << "#version 450\nuniform " << types[i] << " LUT;\nvoid main() { FragColor = vec4(texture(LUT, "
			<< (i == 2 ? "vec3(0.5)" : "vec2(0.5)") << ")); }\n";
		const auto preset = directory / (std::to_string(i) + ".slangp");
		std::ofstream(preset) << "shaders = 1\nshader0 = " << shader.filename().string()
			<< "\ntextures = LUT\nLUT = shared.tga\n";
		paths.push_back(preset.string());
	}
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(paths, chain, error)) << error;
	ASSERT_EQ(3u, chain.textures.size());
	EXPECT_EQ(postprocess::SlangResourceType::Sampler2D, chain.textures[0].type);
	EXPECT_EQ(postprocess::ShaderNumericType::UnsignedInteger, chain.textures[1].numericType);
	EXPECT_EQ(postprocess::SlangResourceType::Sampler3D, chain.textures[2].type);
	for (unsigned i = 0; i < 3; ++i) {
		const auto input = std::find_if(chain.passes[i].inputs.begin(), chain.passes[i].inputs.end(), [](const auto& value) {
			return value.kind == postprocess::ShaderInputKind::LookupTexture;
		});
		ASSERT_NE(chain.passes[i].inputs.end(), input);
		EXPECT_EQ(chain.textures[i].name, input->resourceName);
	}
	const auto incompatible = directory / "incompatible.slangp";
	std::ofstream(incompatible) << "shaders = 2\nshader0 = 0.slang\nshader1 = 1.slang\ntextures = LUT\nLUT = shared.tga\n";
	EXPECT_FALSE(postprocess::loadShaderChain(incompatible.string(), chain, error));
	EXPECT_NE(std::string::npos, error.find("incompatible types"));
}

TEST_F(ShaderLayerTest, KeepsDifferentLookupDimensionsSeparateAcrossEffects)
{
	config::PostProcessShaderTechnique.get().clear();
	std::vector<std::string> paths;
	for (unsigned width : { 2u, 4u }) {
		const auto effect = directory / (std::to_string(width) + ".fx");
		std::ofstream(effect) << "texture3D LUT < source = \"shared.tga\"; > { Width = " << width
			<< "; Height = 2; Depth = 2; };\nsampler3D ReadLUT { Texture = LUT; };\n"
			"float4 Pixel() : SV_Target { return tex3D(ReadLUT, float3(0.5, 0.5, 0.5)); }\n"
			"technique Main { pass { PixelShader = Pixel; } }\n";
		paths.push_back(effect.string());
	}
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(paths, chain, error)) << error;
	ASSERT_EQ(2u, chain.textures.size());
	EXPECT_EQ(2u, chain.textures[0].width);
	EXPECT_EQ(4u, chain.textures[1].width);
}
} // namespace
