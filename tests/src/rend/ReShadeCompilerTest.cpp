/*
	Portions Copyright 2026 The Hollycast Authors

	SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "rend/postprocess/shader_chain.h"

#include <gtest/gtest.h>

TEST(ReShadeCompilerTest, PreservesIntegerVectorControlsAndSignedViewportBounds)
{
	const std::string effect =
		"#define BUFFER_SCREEN_SIZE float2(BUFFER_WIDTH, BUFFER_HEIGHT)\n"
		"uniform int2 offset < ui_min = -BUFFER_SCREEN_SIZE; ui_max = BUFFER_SCREEN_SIZE; ui_step = 1; > = int2(2, -3);\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(offset, 0, 1); }\n"
		"technique Offset { pass { PixelShader = Pixel; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(1u, source.parameters.size());
	const auto& parameter = source.parameters.front();
	ASSERT_EQ(2u, parameter.componentCount);
	EXPECT_FLOAT_EQ(2.f, parameter.values[0]);
	EXPECT_FLOAT_EQ(-3.f, parameter.values[1]);
	for (unsigned component = 0; component < 2; ++component) {
		EXPECT_FLOAT_EQ(-static_cast<float>(postprocess::MaxShaderTargetDimension), parameter.minimumValues[component]);
		EXPECT_FLOAT_EQ(static_cast<float>(postprocess::MaxShaderTargetDimension), parameter.maximumValues[component]);
		EXPECT_FLOAT_EQ(1.f, parameter.stepValues[component]);
	}
	EXPECT_NE(std::string::npos, source.fragment.find("uniform vec2 offset;"));
	EXPECT_NE(std::string::npos, source.fragment.find("ivec2(offset)"));
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, {}, 1, hlsl, error)) << error;
}

TEST(ReShadeCompilerTest, PreservesUniformArrayElementsAndSingleElementArrays)
{
	const std::string effect =
		"uniform float values[2] = { 0.5, 1.0 };\n"
		"uniform float2 single[1] = { float2(2.0, 3.0) };\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(values[0], values[1], single[0]); }\n"
		"technique Array { pass { PixelShader = Pixel; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(3u, source.parameters.size());
	EXPECT_FLOAT_EQ(0.5f, source.parameters[0].value);
	EXPECT_FLOAT_EQ(1.f, source.parameters[1].value);
	EXPECT_FLOAT_EQ(2.f, source.parameters[2].values[0]);
	EXPECT_FLOAT_EQ(3.f, source.parameters[2].values[1]);
	EXPECT_NE(std::string::npos, source.fragment.find("float[2]("));
	EXPECT_NE(std::string::npos, source.fragment.find("vec2[1]("));
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, {}, 1, hlsl, error)) << error;
}

TEST(ReShadeCompilerTest, PreservesNonsquareMatricesAndBooleanVectorControls)
{
	const std::string effect =
		"uniform float2x3 transform = float2x3(1, 2, 3, 4, 5, 6);\n"
		"uniform bool2 flags = bool2(true, false);\n"
		"uniform bool enabled = true;\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(transform[0], enabled && flags.x); }\n"
		"technique Matrix { pass { PixelShader = Pixel; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(3u, source.parameters.size());
	EXPECT_EQ(postprocess::SlangParameterType::Float2x3, source.parameters[0].type);
	EXPECT_EQ(6u, source.parameters[0].componentCount);
	EXPECT_FLOAT_EQ(6.f, source.parameters[0].values[5]);
	EXPECT_FLOAT_EQ(0.f, source.parameters[2].minimum);
	EXPECT_FLOAT_EQ(1.f, source.parameters[2].maximum);
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, {}, 1, hlsl, error)) << error;
}

TEST(ReShadeCompilerTest, ReflectsIndependentBlendMasksStencilAndDrawState)
{
	const std::string effect =
		"texture2D A { Width = 16; Height = 8; Format = RGBA16F; };\n"
		"texture2D B { Width = 16; Height = 8; Format = RGBA16F; };\n"
		"void Pixel(out float4 first : SV_Target0, out float4 second : SV_Target1) { first = 1; second = 0; }\n"
		"technique States { pass { PixelShader = Pixel; RenderTarget0 = A; RenderTarget1 = B;"
		" BlendEnable0 = true; SrcBlend0 = SRCALPHA; DestBlend0 = INVSRCALPHA;"
		" SrcBlendAlpha0 = ONE; DestBlendAlpha0 = ZERO; BlendOpAlpha0 = MAX; ColorWriteMask1 = 3;"
		" StencilEnable = true; StencilFunc = GEQUAL; StencilPassOp = REPLACE; StencilRef = 7;"
		" StencilReadMask = 15; StencilWriteMask = 31; ClearRenderTargets = true; GenerateMipmaps = false;"
		" PrimitiveTopology = TRIANGLESTRIP; VertexCount = 4; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	const auto& state = source.renderState;
	EXPECT_TRUE(state.attachments[0].blendEnabled);
	EXPECT_FALSE(state.attachments[1].blendEnabled);
	EXPECT_EQ(postprocess::ShaderBlendFactor::SourceAlpha, state.attachments[0].source);
	EXPECT_EQ(postprocess::ShaderBlendOperation::Maximum, state.attachments[0].alphaOperation);
	EXPECT_EQ(3u, state.attachments[1].writeMask);
	EXPECT_TRUE(state.stencilEnabled);
	EXPECT_EQ(postprocess::ShaderComparison::GreaterEqual, state.stencilComparison);
	EXPECT_EQ(postprocess::ShaderStencilOperation::Replace, state.stencilPass);
	EXPECT_EQ(7u, state.stencilReference);
	EXPECT_EQ(15u, state.stencilReadMask);
	EXPECT_EQ(31u, state.stencilWriteMask);
	EXPECT_TRUE(state.clearRenderTargets);
	EXPECT_FALSE(state.generateMipmaps);
	EXPECT_EQ(postprocess::ShaderPrimitiveTopology::TriangleStrip, state.topology);
	EXPECT_EQ(4u, state.vertexCount);
	EXPECT_FALSE(source.updatesSource);
}

TEST(ReShadeCompilerTest, ReflectsTypedVolumeSamplesAndIntegerStorage)
{
	const std::string effect =
		"texture3D Volume { Width = 8; Height = 4; Depth = 2; Format = RGBA16F; };\n"
		"sampler3D SampleVolume { Texture = Volume; };\n"
		"texture2D Counter { Width = 16; Height = 8; Format = R32U; };\n"
		"storage2D<uint> WriteCounter { Texture = Counter; };\n"
		"[numthreads(8, 4, 1)] void Compute(uint3 id : SV_DispatchThreadID) { tex2Dstore(WriteCounter, int2(id.xy), uint(tex3D(SampleVolume, float3(0.5, 0.5, 0.5)).x)); }\n"
		"technique Typed { pass { ComputeShader = Compute; DispatchSizeX = 2; DispatchSizeY = 2; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ(postprocess::SlangResourceType::Sampler3D, source.slangResources[0].type);
	ASSERT_EQ(1u, source.storageOutputs.size());
	EXPECT_EQ("R32_UINT", source.storageOutputs[0].targetFormatName);
	EXPECT_EQ(8u, source.localSize[0]);
	EXPECT_EQ(4u, source.localSize[1]);
	EXPECT_EQ("2", source.dispatchExpressions[0]);
	EXPECT_EQ("16", source.widthExpression);
	postprocess::ShaderPassInput input;
	input.bindingName = "SampleVolume"; input.type = postprocess::SlangResourceType::Sampler3D;
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslComputeToHlsl(source, source.slangParameters, { input }, hlsl, error)) << error;
}

TEST(ReShadeCompilerTest, HandlesLineDirectivesInsideAnnotationsAndFunctionSignatures)
{
	const std::string effect =
		"#define LABEL \"continued label\"\n"
		"#define PS_NAME Pixel\n"
		"uniform float value < ui_label = \"A \"\n LABEL; ui_min = 0; ui_max = 1; > = 0.5;\n"
		"float4 PS_NAME(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return value; }\n"
		"technique Label { pass { PixelShader = PS_NAME; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	ASSERT_EQ(1u, source.parameters.size());
	EXPECT_EQ("A continued label", source.parameters.front().label);
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, {}, 1, hlsl, error)) << error;
}

TEST(ReShadeCompilerTest, BindsEscapedNamespacedUniformsAndSamplers)
{
	const std::string effect =
		"namespace Effect {\n"
		"uniform float Gain < ui_min = 0; ui_max = 1; > = 0.5;\n"
		"texture BackBuffer : COLOR; sampler Input { Texture = BackBuffer; };\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(Input, uv) * Gain; }\n"
		"technique Namespace { pass { PixelShader = Pixel; } }\n"
		"}\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	postprocess::ShaderPassInput input;
	input.bindingName = "Input";
	input.kind = postprocess::ShaderInputKind::Source;
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, { input }, 1, hlsl, error)) << error;
}
TEST(ReShadeCompilerTest, SeparatesEffectLocalsFromHostUniformNames)
{
	const std::string effect =
		"uniform float2 NativePixelSize < source = \"normalized_native_pixel_size\"; >;\n"
		"struct Values { float2 OutputSize; };\n"
		"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {\n"
		"float OriginalSize = 2.0; Values values; values.OutputSize = NativePixelSize;\n"
		"return float4(values.OutputSize * OriginalSize, 0, 1); }\n"
		"technique Names { pass { PixelShader = Pixel; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("_OriginalSize"));
	EXPECT_NE(std::string::npos, source.fragment.find("_OutputSize"));
	std::string hlsl;
	EXPECT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, source.slangParameters, {}, 1, hlsl, error)) << error;
}