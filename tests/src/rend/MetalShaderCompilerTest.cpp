// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HOLLYCAST_SHADER_MSL
#include "rend/postprocess/shader_chain.h"
#include "cfg/option.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace postprocess;

TEST(MetalShaderCompilerTest, PreservesTypedParametersAndExplicitResourceBindings)
{
	ShaderPassSource source;
	source.fragment = "uniform float Mode;\nvoid main() { FragColor = texture(Source, vTexCoord)"
			" + texture(Lookup, vTexCoord) * float(Mode == 1) + vec4(1 - vTexCoord.y); }";
	SlangParameter parameter; parameter.name = "Mode";
	std::vector<ShaderPassInput> inputs { {"Source", ShaderInputKind::Source}, {"Lookup", ShaderInputKind::LookupTexture} };
	MetalShaderSource metal;
	std::string error;
	ASSERT_TRUE(translateShaderPassToMetal(source, { parameter }, inputs, metal, error)) << error;
	EXPECT_NE(std::string::npos, metal.fragment.find("[[texture(0)]]"));
	EXPECT_NE(std::string::npos, metal.fragment.find("[[texture(1)]]"));
	EXPECT_NE(std::string::npos, metal.fragment.find("[[buffer(0)]]"));
	EXPECT_NE(std::string::npos, metal.fragment.find("hollycast_main"));
	EXPECT_NE(std::string::npos, metal.vertex.find("[[attribute(0)]]"));
	EXPECT_TRUE(metal.compute.empty());
}

TEST(MetalShaderCompilerTest, UsesSeparateTextureSlotsForComputeStorage)
{
	ShaderPassSource source;
	source.compute = "layout(local_size_x=8, local_size_y=8) in;\n"
			"layout(rgba8, binding=0) uniform image2D Result;\n"
			"void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); imageStore(Result, p, texelFetch(Source, p, 0)); }";
	source.storageBindings = { { "Result", 0 } };
	MetalShaderSource metal;
	std::string error;
	ASSERT_TRUE(translateShaderPassToMetal(source, {}, {}, metal, error)) << error;
	EXPECT_NE(std::string::npos, metal.compute.find("[[texture(0)]]"));
	EXPECT_NE(std::string::npos, metal.compute.find("[[texture(16)]]"));
	EXPECT_NE(std::string::npos, metal.compute.find("kernel"));
	EXPECT_TRUE(metal.vertex.empty()); EXPECT_TRUE(metal.fragment.empty());
}

TEST(MetalShaderCompilerTest, TranslatesEveryBundledShader)
{
	struct Fixture {
		std::filesystem::path root = std::filesystem::temp_directory_path() /
				("hollycast_metal_compiler_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::string technique = config::PostProcessShaderTechnique.get();
		~Fixture() {
			config::PostProcessShaderTechnique.get() = technique;
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		}
	} fixture;
	std::string error;
	ASSERT_TRUE(installBundledShaders(fixture.root.string(), error)) << error;
	config::PostProcessShaderTechnique.get().clear();
	size_t count = 0;
	for (const auto& file : std::filesystem::recursive_directory_iterator(fixture.root)) {
		if (!file.is_regular_file() || detectFormat(file.path().string()) == ShaderFormat::Unknown) continue;
		SCOPED_TRACE(file.path().string());
		ShaderChain chain;
		ASSERT_TRUE(loadShaderChain(file.path().string(), chain, error)) << error;
		for (size_t index = 0; index < chain.passes.size(); ++index) {
			const auto& source = chain.sources[index];
			auto parameters = source.slangParameters;
			if (parameters.empty())
				for (const auto& value : chain.parameters)
					if (value.layerIndex == chain.passes[index].layerIndex) {
						SlangParameter parameter; parameter.name = value.name; parameter.type = value.type;
						parameters.push_back(std::move(parameter));
					}
			MetalShaderSource metal;
			EXPECT_TRUE(translateShaderPassToMetal(source, parameters, chain.passes[index].inputs, metal, error)) << error;
			++count;
		}
	}
	RecordProperty("translated_passes", (int)count);
	EXPECT_GT(count, 30u);
}
} // namespace
#endif
