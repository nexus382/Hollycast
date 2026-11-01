// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/gui_util.h"
#include "ui/imgui_driver.h"
#include "imgui.h"
#include <stb_image_write.h>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>

namespace {
class ImageTestDriver : public ImGuiDriver
{
public:
	struct Image { ImTextureID id; size_t bytes; u8 red; bool nearest; };
	std::unordered_map<std::string, Image> images;
	std::thread::id owner = std::this_thread::get_id();
	ImTextureID nextId = 1;
	void newFrame() override {}
	void renderDrawData(ImDrawData*, bool) override {}
	void present() override {}
	ImTextureID getTexture(const std::string& name) override {
		const auto it = images.find(name);
		return it == images.end() ? ImTextureID{} : it->second.id;
	}
	ImTextureID updateTexture(const std::string& name, const u8* pixels, int width, int height, bool nearest) override {
		EXPECT_EQ(owner, std::this_thread::get_id());
		const ImTextureID id = nextId++;
		images[name] = {id, static_cast<size_t>(width) * height * 4, pixels[0], nearest};
		return id;
	}
	void deleteTexture(const std::string& name) override {
		EXPECT_EQ(owner, std::this_thread::get_id());
		images.erase(name);
	}
};
}

class LibraryImageTest : public ::testing::Test
{
protected:
	void SetUp() override {
		previousContext = ImGui::GetCurrentContext();
		gui_init();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		previousDriver = std::move(imguiDriver);
		auto driver = std::make_unique<ImageTestDriver>();
		gpu = driver.get();
		imguiDriver = std::move(driver);
		unsigned char* data;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&data, &width, &height);
		ImguiFileTexture::invalidateFileImages();
		nextFrame();
		directory = std::filesystem::temp_directory_path()
				/ ("hollycast-images-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(directory);
	}
	void TearDown() override {
		ImguiFileTexture::stopImageLoader();
		ImguiFileTexture::resetLoadCount();
		ImguiFileTexture::clearThemeImageFailures();
		imguiDriver = std::move(previousDriver);
		gui_term();
		ImGui::SetCurrentContext(previousContext);
		std::filesystem::remove_all(directory);
	}
	void nextFrame() {
		ImGui::NewFrame();
		ImGui::EndFrame();
		ImguiFileTexture::resetLoadCount();
	}
	std::string writeImage(const char* name, u8 red = 20, int width = 32, int height = 16) {
		const std::string path = (directory / name).string();
		std::vector<u8> pixels(static_cast<size_t>(width) * height * 4, red);
		EXPECT_NE(0, stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4));
		return path;
	}
	ImTextureID waitForImage(ImguiFileTexture image, const std::string& keepHot = {}) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do {
			if (!keepHot.empty()) ImguiFileTexture(keepHot).getId();
			if (const auto id = image.getId()) return id;
			nextFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		ADD_FAILURE() << "Image did not become available";
		return {};
	}
	ImGuiContext* previousContext;
	std::unique_ptr<ImGuiDriver> previousDriver;
	ImageTestDriver* gpu;
	std::filesystem::path directory;
};

TEST_F(LibraryImageTest, ReadsAsynchronouslyAndUploadsOnTheUiThread)
{
	const auto path = writeImage("cover.png");
	ImguiFileTexture image(path);
	EXPECT_EQ(ImTextureID{}, image.getId());
	EXPECT_TRUE(gpu->images.empty());
	const auto id = waitForImage(image);
	ASSERT_NE(ImTextureID{}, id);
	EXPECT_FLOAT_EQ(2.f, image.getAspectRatio());
	EXPECT_EQ(id, image.getId());
	EXPECT_EQ(1u, gpu->images.size());
}

TEST_F(LibraryImageTest, RefreshDiscardsPendingPixelsAndReplacesTheSamePath)
{
	const auto path = writeImage("cover.png", 20);
	ImguiFileTexture(path).getId();
	ImguiFileTexture::invalidateFileImages();
	nextFrame();
	writeImage("cover.png", 80);
	waitForImage(ImguiFileTexture(path));
	ASSERT_EQ(1u, gpu->images.size());
	EXPECT_EQ(80, gpu->images.at(path).red);
	writeImage("cover.png", 100, 16, 32);
	ImguiFileTexture::invalidateFileImages();
	nextFrame();
	waitForImage(ImguiFileTexture(path));
	EXPECT_EQ(100, gpu->images.at(path).red);
	EXPECT_FLOAT_EQ(0.5f, ImguiFileTexture(path).getAspectRatio());
}

TEST_F(LibraryImageTest, MissingArtworkRetriesAfterItIsPublishedWithoutARefresh)
{
	const auto missing = (directory / "new.png").string();
	ImguiFileTexture(missing).getId();
	const auto available = writeImage("other.png");
	ASSERT_NE(ImTextureID{}, waitForImage(ImguiFileTexture(available)));
	writeImage("new.png", 42);
	ASSERT_NE(ImTextureID{}, waitForImage(ImguiFileTexture(missing)));
	EXPECT_EQ(42, gpu->images.at(missing).red);
}

TEST_F(LibraryImageTest, BudgetEvictsOldImagesButKeepsVisibleImagesAndReloadsEvictions)
{
	const auto hot = writeImage("hot.png", 20, 2048, 1024);
	ASSERT_NE(ImTextureID{}, waitForImage(ImguiFileTexture(hot)));
	const auto originalHot = gpu->getTexture(hot);
	for (int i = 0; i < 40; ++i)
	{
		const auto path = (directory / (std::to_string(i) + ".png")).string();
		std::filesystem::copy_file(hot, path);
		ASSERT_NE(ImTextureID{}, waitForImage(ImguiFileTexture(path), hot));
	}
	for (int i = 0; i < 4; ++i) { ImguiFileTexture(hot).getId(); nextFrame(); }
	size_t bytes = 0;
	for (const auto& image : gpu->images) bytes += image.second.bytes;
	EXPECT_LE(bytes, 256u * 1024 * 1024);
	EXPECT_EQ(originalHot, gpu->getTexture(hot));
	const auto evicted = (directory / "0.png").string();
	EXPECT_EQ(ImTextureID{}, gpu->getTexture(evicted));
	EXPECT_NE(ImTextureID{}, waitForImage(ImguiFileTexture(evicted), hot));
}

TEST_F(LibraryImageTest, PixelArtworkKeepsItsOwnTextureAndSampling)
{
	const auto path = writeImage("cover.png");
	ImguiFileTexture original(path), pixel(path, true);
	themes::PixelArt style;
	style.resolution = 16;
	style.levels = 4;
	pixel.setPixelArt(style);
	const auto plainId = waitForImage(original);
	const auto pixelId = waitForImage(pixel);
	EXPECT_NE(plainId, pixelId);
	EXPECT_EQ(2u, gpu->images.size());
	for (const auto& image : gpu->images)
		if (image.second.id == pixelId) EXPECT_TRUE(image.second.nearest);
}
