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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
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
			std::exit(HasFailure() ? 1 : 0);
		}, testing::ExitedWithCode(0), "");
	}

	std::string writeShader(const char *name, const std::string& source) {
		const auto path = directory / name;
		std::ofstream(path) << source;
		return path.string();
	}

	void checkPattern(const std::vector<std::string>& paths, bool expectCopy = false) {
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
		for (unsigned frame = 0; frame < 3; ++frame)
			ASSERT_TRUE(chain.render(inputView, { 320, 240, 640, 480, frame }, output, transformed, depthView))
					<< postprocess::getShaderChainStatus().error;
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
		if (expectCopy) {
			for (unsigned y : { 79u, 243u, 403u }) {
				const auto *row = static_cast<const uint8_t *>(mapped.pData) + mapped.RowPitch * y;
				for (unsigned x : { 131u, 325u, 507u }) {
					const bool bright = ((x / 32 + y / 32) & 1) != 0;
					EXPECT_NEAR(x / 2 * 255 / 319, row[x * 4 + 2], 2) << "red at " << x << ',' << y;
					EXPECT_NEAR(y / 2 * 127 / 239 + (bright ? 128 : 0), row[x * 4 + 1], 2) << "green at " << x << ',' << y;
					EXPECT_NEAR(bright ? 224 : 32, row[x * 4], 2) << "blue at " << x << ',' << y;
				}
			}
		}
		context->getDeviceContext()->Unmap(readback, 0);
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
} // namespace
#endif
