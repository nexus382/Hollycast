/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "rend/postprocess/slang_frontend.h"

#include <gtest/gtest.h>

TEST(SlangFrontendTest, ParsesTypedParametersStagesAndConventionalResources)
{
	const std::string source =
		"#version 450\n"
		"#pragma name \"CRT\"\n"
		"#pragma parameter float BLOOM \"Bloom strength\" 0.5 0.0 1.0 0.05\n"
		"#pragma parameter vec3 TINT \"Tint\" vec3(1, 0.9, 0.8) vec3(0, 0, 0) vec3(1, 1, 1) vec3(0.1, 0.1, 0.1)\n"
		"uniform sampler2D Source;\n"
		"uniform sampler2D Original;\n"
		"uniform sampler2D PassAHistory;\n"
		"uniform sampler2D PassAFeedback;\n"
		"uniform sampler3D LUT;\n"
		"#pragma stage vertex\n"
		"void CustomVertex() {}\n"
		"#pragma stage fragment\n"
		"void main() {}\n";

	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, shader, error)) << error;
	EXPECT_EQ("CRT", shader.displayName);
	ASSERT_EQ(2u, shader.parameters.size());
	EXPECT_EQ(postprocess::SlangParameterType::Float3, shader.parameters[1].type);
	EXPECT_FLOAT_EQ(0.9f, shader.parameters[1].defaultValue.values[1]);
	ASSERT_EQ(5u, shader.resources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, shader.resources[0].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::Original, shader.resources[1].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::History, shader.resources[2].convention);
	EXPECT_EQ("PassA", shader.resources[2].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::Feedback, shader.resources[3].convention);
	EXPECT_NE(std::string::npos, shader.vertex.source.find("CustomVertex"));
	EXPECT_NE(std::string::npos, shader.fragment.source.find("void main"));
}

TEST(SlangFrontendTest, RejectsMalformedParametersAndResources)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangFrontend(
			"#version 450\n#pragma parameter int STEPS \"Steps\" 1.5 0 4 1\nvoid main() {}\n", shader, error));
	EXPECT_NE(std::string::npos, error.find("parameter"));
	EXPECT_FALSE(postprocess::parseSlangFrontend(
			"#version 450\nuniform sampler2D Broken[0];\nvoid main() {}\n", shader, error));
	EXPECT_NE(std::string::npos, error.find("resource"));
}

TEST(SlangFrontendTest, PreservesStandardExplicitResourceBindings)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
			"#version 450\n"
			"layout(set = 2, binding = 7) uniform sampler2D Source;\n"
			"layout(binding = 8) uniform sampler2DArray PassA[2];\n"
			"void main() {}\n", shader, error)) << error;
	ASSERT_EQ(2u, shader.resources.size());
	EXPECT_TRUE(shader.resources[0].hasExplicitBinding);
	EXPECT_EQ(2u, shader.resources[0].bindingSet);
	EXPECT_EQ(7u, shader.resources[0].binding);
	EXPECT_EQ(8u, shader.resources[1].binding);
	EXPECT_EQ(2u, shader.resources[1].arrayCount);
}

TEST(SlangFrontendTest, LowersUniformBlocksAndPushConstantsToHostSemantics)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
		"#version 450\n"
		"#pragma parameter GAIN \"Gain\" 0.5 0 1\n"
		"#pragma alias ColorPass\n"
		"layout(std140, binding=0) uniform Frame { mat4 MVP; vec4 SourceSize; } frame;\n"
		"layout(push_constant) uniform Parameters { float GAIN; uint FrameCount; vec4 FinalViewportSize; } params;\n"
		"layout(binding=1) uniform sampler2D Source;\n"
		"#pragma stage vertex\n"
		"layout(location=0) in vec4 Position;\nlayout(location=1) in vec2 TexCoord;\n"
		"layout(location=0) out vec2 vTexCoord;\n"
		"void main() { gl_Position = frame.MVP * Position; vTexCoord = TexCoord; }\n"
		"#pragma stage fragment\n"
		"layout(location=0) in vec2 vTexCoord;\nlayout(location=0) out vec4 FragColor;\n"
		"void main() { FragColor = texture(Source, vTexCoord) * params.GAIN"
		" + vec4(frame.SourceSize.xy / params.FinalViewportSize.xy, float(params.FrameCount), 0.0); }\n",
		shader, error)) << error;
	EXPECT_EQ("ColorPass", shader.displayName);
	ASSERT_EQ(1u, shader.resources.size());
	ASSERT_EQ(1u, shader.parameters.size());
	EXPECT_FLOAT_EQ(0.1f, shader.parameters[0].step.values[0]);
	EXPECT_EQ(std::string::npos, shader.vertex.source.find("uniform Frame"));
	EXPECT_EQ(std::string::npos, shader.fragment.source.find("push_constant"));
	EXPECT_EQ(std::string::npos, shader.fragment.source.find("params."));
	EXPECT_NE(std::string::npos, shader.vertex.source.find("vec4(in_pos, 1.0)"));
	EXPECT_NE(std::string::npos, shader.fragment.source.find("vec4(SourceSize, vec2(1.0) / SourceSize)"));
	EXPECT_NE(std::string::npos, shader.fragment.source.find("uint(FrameCount)"));
}

TEST(SlangFrontendTest, ReflectsSizeOnlyTexturesAndMatchingDuplicateParameters)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
		"#version 450\n"
		"#pragma parameter GAIN \"Gain\" 0.5 0 1 0.1\n"
		"#pragma parameter GAIN \"Gain\" 0.5 0 1 0.1\n"
		"layout(push_constant) uniform Size { vec4 PassOutput0Size; } pc;\n"
		"void main() { vec2 size = pc.PassOutput0Size.xy; }\n", shader, error)) << error;
	ASSERT_EQ(1u, shader.parameters.size());
	ASSERT_EQ(1u, shader.resources.size());
	EXPECT_EQ("PassOutput0", shader.resources[0].name);
	EXPECT_EQ(postprocess::SlangResourceConvention::PassAlias, shader.resources[0].convention);
	EXPECT_NE(std::string::npos, shader.fragment.source.find("textureSize(PassOutput0, 0)"));
	EXPECT_FALSE(postprocess::parseSlangFrontend(
		"#version 450\n#pragma parameter GAIN \"Gain\" 0.5 0 1 0.1\n"
		"#pragma parameter GAIN \"Gain\" 0.7 0 1 0.1\nvoid main() {}\n", shader, error));
	EXPECT_NE(std::string::npos, error.find("Conflicting"));
}

TEST(SlangFrontendTest, DistinguishesTextureCallsFromResourceDeclarations)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
		"#version 450\n"
		"layout(binding=1) uniform sampler2D Source;\n"
		"void main() {\n"
		"  vec4 samples = vec4(texture(Source, vec2(0.0)).r,\n"
		"                      texture(Source, vec2(0.5)).r,\n"
		"                      texture (Source, vec2(1.0)).r, 1.0);\n"
		"}\n", shader, error)) << error;
	ASSERT_EQ(1u, shader.resources.size());
	EXPECT_EQ("Source", shader.resources[0].name);
	EXPECT_NE(std::string::npos, shader.fragment.source.find("texture(Source, vec2(0.5))"));
}
TEST(SlangFrontendTest, ExpandsMultilineFunctionMacrosBeforeStageLowering)
{
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(
		"#version 450\r\n"
		"#define DEFINE_HELPER(T) \\\r\n"
		"  T helper(T value) { \\\r\n"
		"    return value; \\\r\n"
		"  }\r\n"
		"DEFINE_HELPER(vec2)\r\n"
		"void main() { vec2 result = helper(vec2(1.0)); }\r\n", shader, error)) << error;
	EXPECT_NE(std::string::npos, shader.fragment.source.find("vec2 helper(vec2 value)"));
	EXPECT_EQ(std::string::npos, shader.fragment.source.find("DEFINE_HELPER"));
	EXPECT_EQ(std::string::npos, shader.fragment.source.find("T helper"));
	EXPECT_EQ(std::string::npos, shader.fragment.source.find('\\'));
}
TEST(SlangFrontendTest, ParsesComputeImagesAndTypedMultidimensionalSamples)
{
	const std::string source =
		"#version 450\n"
		"uniform usampler3D Counts;\n"
		"uniform samplerCube Environment;\n"
		"layout(binding = 3, rgba32f) uniform image2D Result;\n"
		"#pragma stage compute\n"
		"layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;\n"
		"void main() { imageStore(Result, ivec2(gl_GlobalInvocationID.xy), vec4(1)); }\n";
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, shader, error)) << error;
	EXPECT_TRUE(shader.compute.declared);
	EXPECT_FALSE(shader.fragment.declared);
	ASSERT_EQ(3u, shader.resources.size());
	EXPECT_EQ(postprocess::ShaderNumericType::UnsignedInteger, shader.resources[0].numericType);
	EXPECT_EQ(postprocess::SlangResourceType::Sampler3D, shader.resources[0].type);
	EXPECT_EQ(postprocess::SlangResourceType::SamplerCube, shader.resources[1].type);
	EXPECT_EQ(postprocess::SlangResourceType::Image2D, shader.resources[2].type);
	EXPECT_EQ("RGBA32F", shader.resources[2].formatName);
	EXPECT_FALSE(postprocess::parseSlangFrontend(source + "#pragma stage fragment\nvoid main() {}\n", shader, error));
}
