// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rend/metal/metal_shader_chain.h"
#include "cfg/option.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace postprocess;

class MetalShaderChainTest : public testing::Test {
protected:
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLTexture> input = nil;
	std::unique_ptr<MetalShaderChain> chain;
	std::filesystem::path directory = std::filesystem::temp_directory_path() /
			("hollycast_metal_pixels_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::vector<std::string> previousPaths = config::PostProcessShaders.get();
	std::string previousPath = config::PostProcessShader.get(), previousDirectory = config::PostProcessShaderDirectory.get();
	std::string previousParameters = config::PostProcessShaderParameters.get(), previousTechnique = config::PostProcessShaderTechnique.get();
	bool transformed = false, fullPresentation = false;

	void SetUp() override {
		if (std::getenv("HOLLYCAST_SHADER_METAL_TESTS") == nullptr)
			GTEST_SKIP() << "Set HOLLYCAST_SHADER_METAL_TESTS=1 on a Mac to run native Metal pixel checks.";
		device = MTLCreateSystemDefaultDevice(); ASSERT_TRUE(device != nil);
		queue = [device newCommandQueue]; ASSERT_TRUE(queue != nil);
		std::filesystem::create_directories(directory);
		config::PostProcessShaders.get().clear(); config::PostProcessShader.get().clear();
		config::PostProcessShaderParameters.get().clear(); config::PostProcessShaderTechnique.get().clear();
		config::PostProcessShaderDirectory.get() = directory.string();
		chain = std::make_unique<MetalShaderChain>(device, queue);
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
		desc.storageMode = metalCpuTextureStorageMode(device); desc.usage = MTLTextureUsageShaderRead;
		input = [device newTextureWithDescriptor:desc]; ASSERT_TRUE(input != nil);
		std::array<uint8_t, 8 * 8 * 4> pixels {};
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x) {
				pixels[(y * 8 + x) * 4 + (y < 4 ? 0 : 1)] = 128;
				pixels[(y * 8 + x) * 4 + 3] = 255;
			}
		[input replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:32];
	}
	void TearDown() override {
		if (queue) { id<MTLCommandBuffer> command = [queue commandBuffer]; [command commit]; [command waitUntilCompleted]; }
		chain.reset();
		config::PostProcessShaders.get() = previousPaths; config::PostProcessShader.get() = previousPath;
		config::PostProcessShaderDirectory.get() = previousDirectory;
		config::PostProcessShaderParameters.get() = previousParameters; config::PostProcessShaderTechnique.get() = previousTechnique;
		reportShaderChainStatus({}, {}); finishShaderCompilation();
		std::error_code ignored; std::filesystem::remove_all(directory, ignored);
	}
	std::string write(const char *name, const std::string& text) {
		const auto path = directory / name; std::ofstream(path) << text; return path.string();
	}
	void select(const std::vector<std::string>& paths) {
		config::PostProcessShaders.get() = paths;
		requestShaderChainReload();
		prepareShaderCompilation(); markShaderCompilationDrawn(); advanceShaderCompilationFrame();
	}
	id<MTLTexture> render(const ShaderFrameInfo& frame = {8,8,8,8}, id<MTLTexture> depth = nil) {
		id<MTLTexture> output = nil;
		if (!chain->render(input, depth, frame, output, transformed, fullPresentation))
			ADD_FAILURE() << getShaderChainStatus().error;
		return output;
	}
	std::array<int,4> pixel(id<MTLTexture> output, int x, int y) {
		if (!output) { ADD_FAILURE() << "No Metal shader output"; return {}; }
		id<MTLBuffer> buffer = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> command = [queue commandBuffer];
		id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
		[encoder copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x,y,0)
			sourceSize:MTLSizeMake(1,1,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
		[encoder endEncoding]; [command commit]; [command waitUntilCompleted];
		EXPECT_EQ(MTLCommandBufferStatusCompleted, command.status);
		EXPECT_TRUE(getShaderChainStatus().error.empty()) << getShaderChainStatus().error;
		const auto *data = (const uint8_t *)buffer.contents;
		return { data[0], data[1], data[2], data[3] };
	}
};

TEST_F(MetalShaderChainTest, DisabledChainDoesNotProcessTheFrame)
{
	id<MTLTexture> output = nil;
	EXPECT_FALSE(chain->render(input, nil, {8,8,8,8}, output, transformed, fullPresentation));
	EXPECT_TRUE(output == nil); EXPECT_FALSE(transformed); EXPECT_FALSE(fullPresentation);
}

TEST_F(MetalShaderChainTest, PreservesOrientationThroughMultiplePasses)
{
	write("copy.glsl", "void main() { FragColor = texture(Source, vTexCoord); }");
	const auto preset = write("copy.glslp", "shaders=2\nshader0=copy.glsl\nshader1=copy.glsl\n");
	select({preset});
	auto output = render();
	EXPECT_EQ((std::array<int,4>{128,0,0,255}), pixel(output,1,1));
	EXPECT_EQ((std::array<int,4>{0,128,0,255}), pixel(output,1,6));
	EXPECT_FALSE(transformed);
}

TEST_F(MetalShaderChainTest, LiveParametersAndRejectedReloadRetainTheActiveChain)
{
	const auto shader = write("gain.glsl", "#pragma parameter Gain \"Gain\" 0.5 0.0 1.0 0.1\n"
			"void main() { FragColor = vec4(texture(Source, vTexCoord).rgb * Gain, 1); }");
	select({shader});
	EXPECT_NEAR(64, pixel(render(),1,1)[0], 1);
	const auto status = getShaderChainStatus(); ASSERT_FALSE(status.parameters.empty());
	ASSERT_TRUE(setShaderChainParameter(shaderParameterKey(status.parameters.front()), 1.f));
	EXPECT_EQ(128, pixel(render(),1,1)[0]);
	const auto invalid = write("invalid.glsl", "void main() { this is invalid; }");
	select({invalid});
	id<MTLTexture> output = nil;
	ASSERT_TRUE(chain->render(input, nil, {8,8,8,8}, output, transformed, fullPresentation));
	EXPECT_EQ(status.activePath, getShaderChainStatus().activePath);
	EXPECT_FALSE(getShaderChainStatus().error.empty());
	// Re-selecting the active graph clears the rejected candidate without losing functionality.
	select({shader}); EXPECT_NEAR(64, pixel(render(),1,1)[0], 1);
}

TEST_F(MetalShaderChainTest, ShadesBordersAndConsumesRotationOnce)
{
	const auto shader = write("borders.glsl", "void main() { SetOutput(Sample() + vec4(0,0,0.25,0)); }");
	select({shader});
	ShaderFrameInfo frame {8,8,8,8};
	frame.rotationPending = true;
	frame.presentation = {12,8,2,0,8,8,{0,0,0,1}};
	auto output = render(frame);
	ASSERT_TRUE(output != nil); EXPECT_EQ(12u, output.width); EXPECT_EQ(8u, output.height);
	EXPECT_TRUE(transformed); EXPECT_TRUE(fullPresentation);
	EXPECT_NEAR(64, pixel(output,0,2)[2], 1);
	EXPECT_EQ(0, pixel(output,0,2)[0]);
	EXPECT_EQ(128, pixel(output,3,2)[0]);
	EXPECT_EQ(128, pixel(output,8,2)[1]);
}

TEST_F(MetalShaderChainTest, PreservesSrgbIntermediatesAndInputMipmaps)
{
	write("copy.glsl", "void main() { FragColor = textureLod(Source, vTexCoord, 1.0); }");
	const auto preset = write("mip.glslp", "shaders=2\nshader0=copy.glsl\nshader1=copy.glsl\n"
			"mipmap_input0=true\nmipmap_input1=true\nsrgb_framebuffer0=true\n");
	select({preset});
	EXPECT_NEAR(128, pixel(render(),0,0)[0], 2);
}

TEST_F(MetalShaderChainTest, ExecutesComputeOutputAndResizesIt)
{
	const auto shader = write("compute.slang", "#version 450\n#pragma stage compute\n"
			"layout(local_size_x=4, local_size_y=4) in;\nlayout(rgba8,binding=0) uniform image2D Result;\n"
			"void main() { ivec2 p=ivec2(gl_GlobalInvocationID.xy); if(all(lessThan(p,imageSize(Result)))) imageStore(Result,p,vec4(0.25,0.5,0.75,1)); }");
	select({shader});
	auto output = render();
	EXPECT_NEAR(64, pixel(output,0,0)[0], 1); EXPECT_NEAR(191, pixel(output,7,7)[2], 1);
	output = render({8,8,13,9}); ASSERT_TRUE(output != nil);
	EXPECT_NEAR(128, pixel(output,0,0)[1], 1);
}

TEST_F(MetalShaderChainTest, UsesDepthAndRecoversFromMissingSceneDepth)
{
	const auto shader = write("depth.fx", "texture2D DepthTex : DEPTH;\n"
			"sampler2D ReadDepth { Texture=DepthTex; MinFilter=POINT; MagFilter=POINT; };\n"
			"float4 Pixel(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target { return float4(tex2D(ReadDepth,uv).rrr,1); }\n"
			"technique Depth { pass { PixelShader=Pixel; } }");
	select({shader});
	id<MTLTexture> output = nil;
	EXPECT_FALSE(chain->render(input, nil, {8,8,8,8}, output, transformed, fullPresentation));
	EXPECT_TRUE(getShaderChainStatus().frameError);
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:8 height:8 mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead; desc.storageMode = metalCpuTextureStorageMode(device);
	id<MTLTexture> depth = [device newTextureWithDescriptor:desc]; ASSERT_TRUE(depth != nil);
	std::array<float,64> values; values.fill(0.25f);
	[depth replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:values.data() bytesPerRow:32];
	ShaderFrameInfo frame {8,8,8,8}; frame.presentation = {12,8,2,0,8,8,{0,0,0,1}};
	output = render(frame,depth);
	EXPECT_NEAR(64, pixel(output,4,4)[0], 1); EXPECT_EQ(0, pixel(output,0,4)[0]);
	EXPECT_FALSE(getShaderChainStatus().frameError);
}

TEST_F(MetalShaderChainTest, ConvertsIntegerComputeOutputsWithTypedReads)
{
	for (bool signedValues : { false, true }) {
		const std::string type = signedValues ? "i" : "u";
		const auto shader = write("integer.slang", "#version 450\n#pragma stage compute\n"
				"layout(local_size_x=4,local_size_y=4) in;\nlayout(rgba32" + type + ",binding=0) uniform " + type + "image2D Result;\n"
				"void main() { imageStore(Result,ivec2(gl_GlobalInvocationID.xy)," + type + "vec4(1,0,1,1)); }");
		select({shader});
		EXPECT_EQ((std::array<int,4>{255,0,255,255}), pixel(render(),1,1));
	}
}

TEST_F(MetalShaderChainTest, ResizingDoesNotRestartFrameCount)
{
	const auto shader = write("counter.glsl", "void main() { FragColor = vec4(float(FrameCount)/255.0,0,0,1); }");
	select({shader});
	EXPECT_EQ(0, pixel(render(),0,0)[0]);
	EXPECT_EQ(1, pixel(render({8,8,16,8}),0,0)[0]);
	EXPECT_EQ(2, pixel(render({8,8,8,8}),0,0)[0]);
}

TEST_F(MetalShaderChainTest, CompilesAndExecutesEveryBundledShader)
{
	std::string error;
	ASSERT_TRUE(installBundledShaders(directory.string(), error)) << error;
	for (const auto& file : std::filesystem::recursive_directory_iterator(directory)) {
		if (!file.is_regular_file() || detectFormat(file.path().string()) == ShaderFormat::Unknown) continue;
		SCOPED_TRACE(file.path().string());
		select({file.path().string()});
		const auto output = render();
		EXPECT_EQ(shaderSelectionLabel({file.path().string()}), getShaderChainStatus().activePath);
		// Readback waits behind the actual shader commands and checks their error
		// status. Artistic effects may legitimately produce black at this size.
		pixel(output,0,0);
	}
}
} // namespace
