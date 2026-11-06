/*
 * Portions Copyright 2026 The Hollycast Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#if defined(_WIN32) && defined(HAVE_D3D11) && defined(USE_SDL) && !defined(LIBRETRO)
#include "rend/dx11/dx11_shader_chain.h"
#include "rend/dx11/dx11context.h"
#include "rend/dx11/dx11_quad.h"
#include "cfg/option.h"
#include "sdl/sdl.h"
#include "imgui.h"
#include "ui/gui.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <optional>
#include <set>
#include <sstream>

namespace {
class DirectXShaderChainTest : public testing::Test {
protected:
	std::filesystem::path directory = std::filesystem::temp_directory_path()
			/ ("hollycast_dx11_shaders_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	RenderType previousRenderer = config::RendererType;
	decltype(settings.display) previousDisplay = settings.display;
	std::vector<std::string> previousPaths = config::PostProcessShaders.get();
	std::string previousPath = config::PostProcessShader.get();
	std::string previousDirectory = config::PostProcessShaderDirectory.get();
	std::string previousParameters = config::PostProcessShaderParameters.get();
	std::string previousTechnique = config::PostProcessShaderTechnique.get();
	bool initialized = false;
	ImGuiContext *imgui = nullptr;
	std::unique_ptr<Quad> quad;
	ComPtr<ID3D11Texture2D> input;
	ComPtr<ID3D11ShaderResourceView> inputView;
	ComPtr<ID3D11ShaderResourceView> depthView;

	void SetUp() override {
		// CI without a graphics device still runs compiler coverage. Opting in
		// requires real rendering, including the application's quad and samplers.
		if (!std::getenv("HOLLYCAST_SHADER_DX11_TESTS"))
			GTEST_SKIP() << "Set HOLLYCAST_SHADER_DX11_TESTS=1 to run DirectX pixel checks.";
	}

	void initializeGraphics() {
		ASSERT_EQ(nullptr, GraphicsContext::Instance());
		ASSERT_EQ(0, SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError();
		gui_init();
		imgui = ImGui::GetCurrentContext();
		config::RendererType = RenderType::DirectX11;
		DX11Context::Create(nullptr);
		initialized = true;
		auto context = DX11Context::Instance();
		quad = std::make_unique<Quad>();
		quad->init(context->getDevice(), context->getDeviceContext(), &context->getShaders());
		// A nonuniform image catches collapsed UVs and unbound texture reads,
		// which a constant-color compiler/render smoke test cannot distinguish.
		std::vector<uint32_t> pixels(320 * 240);
		for (unsigned y = 0; y < 240; ++y)
			for (unsigned x = 0; x < 320; ++x) {
				const bool bright = ((x / 16 + y / 16) & 1) != 0;
				pixels[y * 320 + x] = 0xff000000u | ((x * 255 / 319) << 16)
						| ((y * 127 / 239 + (bright ? 128 : 0)) << 8) | (bright ? 224 : 32);
			}
		D3D11_TEXTURE2D_DESC description {};
		description.Width = 320; description.Height = 240;
		description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data { pixels.data(), 320 * 4, 0 };
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateTexture2D(&description, &data, &input.get())));
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateShaderResourceView(input, nullptr, &inputView.get())));
		std::vector<float> depth(pixels.size());
		for (unsigned y = 0; y < 240; ++y)
			for (unsigned x = 0; x < 320; ++x)
				depth[y * 320 + x] = 0.9f + 0.09999f * x / 319.f;
		description.Format = DXGI_FORMAT_R32_FLOAT;
		data.pSysMem = depth.data();
		ComPtr<ID3D11Texture2D> depthTexture;
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateTexture2D(&description, &data, &depthTexture.get())));
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateShaderResourceView(depthTexture, nullptr, &depthView.get())));
		config::PostProcessShaderParameters.get().clear();
		config::PostProcessShaderTechnique.get() = "stack:";
		std::filesystem::create_directories(directory);
		config::PostProcessShaderDirectory.get() = directory.string();
	}

	void TearDown() override {
		depthView.reset(); inputView.reset(); input.reset(); quad.reset();
		if (initialized) sdl_window_destroy();
		if (imgui) gui_term();
		config::RendererType = previousRenderer;
		settings.display = previousDisplay;
		config::PostProcessShaders.get() = previousPaths;
		config::PostProcessShader.get() = previousPath;
		config::PostProcessShaderDirectory.get() = previousDirectory;
		config::PostProcessShaderParameters.get() = previousParameters;
		config::PostProcessShaderTechnique.get() = previousTechnique;
		postprocess::finishShaderCompilation();
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	void runInGraphicsProcess(const std::function<void()>& check) {
		// GTest serializes Windows process/pipe handles using the global locale;
		// grouping separators from earlier locale tests break its child arguments.
		struct RestoreLocale {
			std::locale previous = std::locale::global(std::locale::classic());
			~RestoreLocale() { std::locale::global(previous); }
		} restoreLocale;
		// The real DX11 context initializes application-wide GUI/font caches.
		// Keep those out of other tests' lightweight ImGui contexts. On Windows,
		// ASSERT_EXIT runs this block in a fresh process with the same environment.
		ASSERT_EXIT({
			initializeGraphics();
			if (!HasFatalFailure()) check();
			TearDown();
			// GTest's parent captures stderr, while ordinary assertions write to
			// stdout. Retain the actual pixel failure when a child exits with 1.
			const auto *result = testing::UnitTest::GetInstance()->current_test_info()->result();
			for (int index = 0; index < result->total_part_count(); ++index)
				if (result->GetTestPartResult(index).failed())
					std::fprintf(stderr, "%s\n", result->GetTestPartResult(index).summary());
			std::exit(HasFailure() ? 1 : 0);
		}, testing::ExitedWithCode(0), "");
	}

	std::string writeShader(const char *name, const std::string& source) {
		const auto path = directory / name;
		std::ofstream(path) << source;
		return path.string();
	}

	void checkPattern(const std::vector<std::string>& paths, bool expectCopy = false,
			const std::optional<std::array<int, 3>>& expectedColor = std::nullopt,
			const postprocess::ShaderPresentation& presentation = {}) {
		const std::string label = postprocess::shaderSelectionLabel(paths);
		SCOPED_TRACE(label);
		config::PostProcessShaders.get() = paths;
		config::PostProcessShader.get() = paths.front();
		postprocess::requestShaderChainReload();
		postprocess::prepareShaderCompilation();
		postprocess::markShaderCompilationDrawn();
		postprocess::advanceShaderCompilationFrame();
		auto context = DX11Context::Instance();
		Dx11ShaderChain chain;
		chain.init(context->getDevice(), context->getDeviceContext(), quad.get());
		ComPtr<ID3D11ShaderResourceView> output;
		bool transformed = false;
		for (unsigned frame = 0; frame < 3; ++frame) {
			postprocess::ShaderFrameInfo frameInfo { 320, 240,
					presentation.hasBorders() ? presentation.contentWidth : 640,
					presentation.hasBorders() ? presentation.contentHeight : 480, frame };
			frameInfo.presentation = presentation;
			ASSERT_TRUE(chain.render(inputView, frameInfo, output, transformed, depthView))
					<< postprocess::getShaderChainStatus().error;
		}
		ASSERT_NE(nullptr, output.get());
		// Present through the regular quad, as the real renderer does. This also
		// converts float/sRGB pass outputs to a consistent format for readback.
		D3D11_TEXTURE2D_DESC description {};
		description.Width = 640; description.Height = 480;
		description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> target;
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateTexture2D(&description, nullptr, &texture.get())));
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateRenderTargetView(texture, nullptr, &target.get())));
		D3D11_VIEWPORT viewport { 0.f, 0.f, 640.f, 480.f, 0.f, 1.f };
		context->getDeviceContext()->RSSetViewports(1, &viewport);
		context->getDeviceContext()->OMSetRenderTargets(1, &target.get(), nullptr);
		context->getDeviceContext()->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(output, context->getSamplers().getSampler(false));
		context->getDeviceContext()->OMSetRenderTargets(0, nullptr, nullptr);
		description.Usage = D3D11_USAGE_STAGING;
		description.BindFlags = description.MiscFlags = 0;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> readback;
		ASSERT_TRUE(SUCCEEDED(context->getDevice()->CreateTexture2D(&description, nullptr, &readback.get())));
		context->getDeviceContext()->CopyResource(readback, texture);
		D3D11_MAPPED_SUBRESOURCE mapped {};
		ASSERT_TRUE(SUCCEEDED(context->getDeviceContext()->Map(readback, 0, D3D11_MAP_READ, 0, &mapped)));
		std::set<uint32_t> colors;
		for (unsigned y = description.Height / 4; y < description.Height * 3 / 4; y += 8) {
			const auto *row = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) + mapped.RowPitch * y);
			for (unsigned x = description.Width / 4; x < description.Width * 3 / 4; x += 8)
				colors.insert(row[x] & 0xffffff);
		}
		if (expectCopy || expectedColor) {
			for (unsigned y : { 79u, 243u, 403u }) {
				const auto *row = static_cast<const uint8_t *>(mapped.pData) + mapped.RowPitch * y;
				for (unsigned x : { 131u, 325u, 507u }) {
					if (expectedColor) {
						for (unsigned channel = 0; channel < 3; ++channel)
							EXPECT_NEAR((*expectedColor)[channel], row[x * 4 + 2 - channel], 1) << "channel " << channel;
						continue;
					}
					if (presentation.hasBorders() && (x < static_cast<unsigned>(presentation.x)
							|| y < static_cast<unsigned>(presentation.y)
							|| x >= static_cast<unsigned>(presentation.x + presentation.contentWidth)
							|| y >= static_cast<unsigned>(presentation.y + presentation.contentHeight))) {
						for (unsigned channel = 0; channel < 3; ++channel)
							EXPECT_NEAR(presentation.borderColor[channel] * 255, row[x * 4 + 2 - channel], 1);
						continue;
					}
					const unsigned sourceX = presentation.hasBorders() ? (x - presentation.x) * 320 / presentation.contentWidth : x / 2;
					const unsigned sourceY = presentation.hasBorders() ? (y - presentation.y) * 240 / presentation.contentHeight : y / 2;
					const bool bright = ((sourceX / 16 + sourceY / 16) & 1) != 0;
					EXPECT_NEAR(sourceX * 255 / 319, row[x * 4 + 2], 2) << "red at " << x << ',' << y;
					EXPECT_NEAR(sourceY * 127 / 239 + (bright ? 128 : 0), row[x * 4 + 1], 2) << "green at " << x << ',' << y;
					EXPECT_NEAR(bright ? 224 : 32, row[x * 4], 2) << "blue at " << x << ',' << y;
				}
			}
		}
		context->getDeviceContext()->Unmap(readback, 0);
		if (!expectedColor)
			EXPECT_GT(colors.size(), 1u) << "Shader lost the spatial variation in the input image";
	}
};

TEST_F(DirectXShaderChainTest, FragmentOnlyStagesPreserveTextureCoordinates)
{
	runInGraphicsProcess([&] {
		const auto copy = writeShader("copy.glsl", "void main() { SetOutput(Sample()); }\n");
		checkPattern({ copy }, true);
		checkPattern({ copy, copy }, true);
	});
}

TEST_F(DirectXShaderChainTest, ReusesComputeBindingsAndPacksConfiguredVectorParameters)
{
	runInGraphicsProcess([&] {
		const auto effect = writeShader("compute-tint.fx",
				"texture2D Result { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };\n"
				"storage2D WriteResult { Texture = Result; };\n"
				"sampler2D ReadResult { Texture = Result; MinFilter = POINT; MagFilter = POINT; };\n"
				"uniform float4 Tint < ui_min = 0.0; ui_max = 1.0; > = float4(0.25, 0.5, 0.75, 1.0);\n"
				"[numthreads(8,8,1)] void Fill(uint3 id : SV_DispatchThreadID)"
				" { tex2Dstore(WriteResult, int2(id.xy), Tint); }\n"
				"float4 Pixel(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(ReadResult, uv); }\n"
				"technique Main { pass Write { ComputeShader = Fill; DispatchSizeX = BUFFER_WIDTH / 8; DispatchSizeY = BUFFER_HEIGHT / 8; }"
				" pass Present { PixelShader = Pixel; } }\n");
		checkPattern({ effect }, false, std::array<int, 3> { 64, 128, 191 });
		postprocess::ShaderChain parsed;
		std::string error;
		ASSERT_TRUE(postprocess::loadShaderChain(effect, parsed, error)) << error;
		auto tint = std::find_if(parsed.parameters.begin(), parsed.parameters.end(), [](const auto& parameter) {
			return parameter.name == "Tint";
		});
		ASSERT_NE(parsed.parameters.end(), tint);
		tint->values = { 0.75f, 0.25f, 0.5f, 1.f };
		tint->value = tint->values[0];
		config::PostProcessShaderParameters.get() = postprocess::serializeShaderChainParameterValues(parsed.parameters);
		checkPattern({ effect }, false, std::array<int, 3> { 191, 64, 128 });
	});
}

TEST_F(DirectXShaderChainTest, ComposesAndFiltersTheWholePresentation)
{
	runInGraphicsProcess([&] {
		const auto copy = writeShader("borders.glsl", "void main() { SetOutput(Sample()); }\n");
		checkPattern({ copy }, true, std::nullopt,
				{ 640, 480, 160, 120, 320, 240, { 0.25f, 0.f, 0.f, 1.f } });
	});
}

TEST_F(DirectXShaderChainTest, FragmentOnlyStagesPreservePixelCoordinates)
{
	runInGraphicsProcess([&] {
		const auto copy = writeShader("coordinates.glsl",
				"void main() { FragColor = texture(Source, gl_FragCoord.xy / OutputSize); }\n");
		checkPattern({ copy }, true);
	});
}

TEST_F(DirectXShaderChainTest, ExternalPresetsPreserveImageDetail)
{
	const char *paths = std::getenv("HOLLYCAST_SHADER_PIXEL_PATHS");
	if (!paths) GTEST_SKIP() << "Set HOLLYCAST_SHADER_PIXEL_PATHS to presets separated by semicolons.";
	runInGraphicsProcess([&] {
		std::istringstream list(paths);
		for (std::string path; std::getline(list, path, ';');) {
			if (path.empty()) continue;
			checkPattern({ path });
		}
	});
}

TEST_F(DirectXShaderChainTest, KeepsLookupSamplerBindingsIndependent)
{
	runInGraphicsProcess([&] {
		std::string image(18, '\0');
		image[2] = 2; image[12] = 2; image[14] = 1; image[16] = 24; image[17] = 0x20;
		std::ofstream(directory / "lookup.tga", std::ios::binary)
				<< image << std::string("\0\0\xff\xff\0\0", 6);
		const auto effect = writeShader("lookup.fx",
				"texture2D Lookup < source = \"lookup.tga\"; >;\n"
				"sampler2D PointSampler { Texture = Lookup; MinFilter = LINEAR; MagFilter = POINT; MipFilter = NONE; };\n"
				"sampler2D LinearSampler { Texture = Lookup; MinFilter = POINT; MagFilter = LINEAR; MipFilter = NONE; };\n"
				"float4 Pixel() : SV_Target { float4 a = tex2D(PointSampler, float2(0.5, 0.5));"
				" float4 b = tex2D(LinearSampler, float2(0.5, 0.5)); return float4(a.r, b.r, b.b, 1.0); }\n"
				"technique Main { pass { PixelShader = Pixel; } }\n");
		checkPattern({ effect }, false, std::array<int, 3>{ 0, 128, 128 });
	});
}
} // namespace
#endif
