/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast, distributed under the GNU General Public
	License, version 2 or (at your option) any later version.
*/
#include "rend/postprocess/shader_chain.h"
#include "cfg/option.h"

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
	std::vector<std::string> previousPaths = config::PostProcessShaders.get();

	void SetUp() override { std::filesystem::create_directories(directory); }
	void TearDown() override {
		config::PostProcessShaderTechnique.get() = previousTechnique;
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
} // namespace