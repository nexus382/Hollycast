// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gtest/gtest.h"
#include "cfg/option.h"
#include "stdclass.h"
#include "ui/custom_theme.h"
#include "ui/theme.h"
#include "ui/settings.h"
#include <stb_image.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

const std::string kManifest = R"json({
  "formatVersion": 1,
  "id": "install-test",
  "name": "Install Test",
  "layouts": [{"id": "grid", "name": "Grid", "mode": "grid", "browser": [0, 0, 1, 1]}]
})json";

void writeFile(const fs::path& path, const std::string& contents)
{
	fs::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary);
	output << contents;
	if (!output)
		throw std::runtime_error("Failed writing test fixture " + path.string());
}

class ThemeInstallTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		oldDataPath = get_writable_data_path("");
		oldCustomTheme = config::CustomTheme.get();
		oldThemeLayout = config::ThemeLayout.get();
		oldUITheme = config::UITheme.get();
		oldUICustomTheme = config::UICustomTheme.get();
		root = fs::temp_directory_path() /
			("hollycast-theme-install-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		appData = root / "app-data";
		sourceRoot = root / "sources";
		fs::create_directories(appData);
		fs::create_directories(sourceRoot);
		set_user_data_dir(appData.string());
		config::CustomTheme = "";
		config::ThemeLayout = "";
		themes::reload();
	}

	void TearDown() override
	{
		config::CustomTheme = oldCustomTheme;
		config::ThemeLayout = oldThemeLayout;
		config::UITheme = oldUITheme;
		config::UICustomTheme = oldUICustomTheme;
		set_user_data_dir(oldDataPath);
		themes::reload();
		std::error_code ec;
		fs::remove_all(root, ec);
	}

	fs::path makePackage(const std::string& id = "install-test", const std::string& manifest = kManifest)
	{
		fs::path source = sourceRoot / ("package-" + id + "-" + std::to_string(sourceIndex++));
		fs::create_directories(source);
		std::string adjusted = manifest;
		const std::string originalId = "\"id\": \"install-test\"";
		const size_t idPos = adjusted.find(originalId);
		if (idPos != std::string::npos)
			adjusted.replace(idPos, originalId.size(), "\"id\": \"" + id + "\"");
		writeFile(source / "theme.json", adjusted);
		writeFile(source / "art" / "pixel.bin", "fixture-image-bytes");
		return source;
	}

	void installValid(const std::string& id = "install-test")
	{
		fs::path source = makePackage(id);
		EXPECT_EQ(themes::install(source.u8string()), id);
	}

	static const themes::CatalogEntry* findEntry(const std::string& id)
	{
		const auto& entries = themes::catalog();
		for (const auto& entry : entries)
			if (entry.id == id)
				return &entry;
		return nullptr;
	}

	std::string oldDataPath;
	std::string oldCustomTheme, oldThemeLayout, oldUICustomTheme;
	int oldUITheme = 0;
	fs::path root, appData, sourceRoot;
	unsigned sourceIndex = 0;
};
} // namespace

TEST_F(ThemeInstallTest, InstallsValidatedPackageAndAddsItToCatalog)
{
	fs::path source = makePackage();
	EXPECT_EQ(themes::install(source.u8string()), "install-test");

	const fs::path installed = fs::path(themes::directory()) / "install-test";
	ASSERT_TRUE(fs::is_regular_file(installed / "theme.json"));
	ASSERT_TRUE(fs::is_regular_file(installed / "art" / "pixel.bin"));
	std::ifstream copied(installed / "art" / "pixel.bin", std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(copied)), std::istreambuf_iterator<char>());
	EXPECT_EQ(bytes, "fixture-image-bytes");

	const auto* entry = findEntry("install-test");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->name, "Install Test");
	EXPECT_TRUE(entry->error.empty());
	config::CustomTheme = "install-test";
	EXPECT_NE(themes::current(), nullptr);
	ASSERT_NE(themes::currentLayout(), nullptr);
	EXPECT_EQ(themes::currentLayout()->id, "grid");
}

TEST_F(ThemeInstallTest, DuplicateInstallDoesNotOverwriteExistingPackage)
{
	installValid();
	const fs::path installed = fs::path(themes::directory()) / "install-test";
	writeFile(installed / "user-note.txt", "keep this file");
	fs::path duplicate = makePackage();
	EXPECT_THROW(themes::install(duplicate.u8string()), std::runtime_error);
	std::ifstream note(installed / "user-note.txt", std::ios::binary);
	const std::string value((std::istreambuf_iterator<char>(note)), std::istreambuf_iterator<char>());
	EXPECT_EQ(value, "keep this file");
	for (const auto& entry : fs::directory_iterator(themes::directory()))
		EXPECT_FALSE(entry.path().filename().string().rfind(".install-", 0) == 0) << entry.path();
}

TEST_F(ThemeInstallTest, InvalidPackageNeverLeavesPublishedOrStagingFolder)
{
	fs::path source = makePackage("install-test", "{ invalid json");
	EXPECT_THROW(themes::install(source.u8string()), std::runtime_error);
	for (const auto& entry : fs::directory_iterator(themes::directory()))
		EXPECT_FALSE(entry.path().filename().string().rfind(".install-", 0) == 0) << entry.path();
	EXPECT_FALSE(fs::exists(fs::path(themes::directory()) / "install-test"));
}

TEST_F(ThemeInstallTest, MissingAndInvalidSelectionsFallBackAndUnknownLayoutUsesFirstLayout)
{
	installValid();
	config::CustomTheme = "install-test";
	config::ThemeLayout = "missing-layout";
	ASSERT_NE(themes::current(), nullptr);
	ASSERT_NE(themes::currentLayout(), nullptr);
	EXPECT_EQ(themes::currentLayout()->id, "grid");

	config::CustomTheme = "not-installed";
	EXPECT_EQ(themes::current(), nullptr);
	EXPECT_NE(themes::error().find("unavailable"), std::string::npos);

	writeFile(fs::path(themes::directory()) / "broken" / "theme.json", "{}");
	themes::reload();
	config::CustomTheme = "broken";
	EXPECT_EQ(themes::current(), nullptr);
	EXPECT_NE(themes::error().find("unavailable"), std::string::npos);
}

TEST_F(ThemeInstallTest, FolderManifestIdMismatchAppearsAsCatalogError)
{
	writeFile(fs::path(themes::directory()) / "folder-name" / "theme.json", kManifest);
	themes::reload();
	const auto* entry = findEntry("folder-name");
	ASSERT_NE(entry, nullptr);
	EXPECT_FALSE(entry->error.empty());
	EXPECT_NE(entry->error.find("folder name must match"), std::string::npos);
	config::CustomTheme = "folder-name";
	EXPECT_EQ(themes::current(), nullptr);
	EXPECT_NE(themes::error().find("folder name must match"), std::string::npos);
}

TEST_F(ThemeInstallTest, ImportRejectsSymlinkEntries)
{
	fs::path source = makePackage();
	std::error_code ec;
	fs::create_symlink(source / "theme.json", source / "linked-manifest.json", ec);
	if (ec)
		GTEST_SKIP() << "Symlinks are unavailable in this test environment: " << ec.message();
	EXPECT_THROW(themes::install(source.u8string()), std::runtime_error);
	EXPECT_FALSE(fs::exists(fs::path(themes::directory()) / "install-test"));
	for (const auto& entry : fs::directory_iterator(themes::directory()))
		EXPECT_FALSE(entry.path().filename().string().rfind(".install-", 0) == 0) << entry.path();
}

TEST_F(ThemeInstallTest, LiveThemeSwitchRestoresPaletteAndRetainsDisplayScale)
{
	struct ContextScope
	{
		ImGuiContext* previous = ImGui::GetCurrentContext();
		float scale = settings.display.uiScale;
		ContextScope() { ImGui::CreateContext(); settings.display.uiScale = 2.f; }
		~ContextScope()
		{
			ImGui::DestroyContext();
			ImGui::SetCurrentContext(previous);
			settings.display.uiScale = scale;
		}
	} context;
	config::UITheme = 0;
	applyCurrentTheme();
	const ImGuiStyle builtin = ImGui::GetStyle();
	std::string manifest = kManifest;
	manifest.insert(1, "\"colors\": {\"Text\": \"123456\"},");
	const auto package = makePackage("install-test", manifest);
	config::CustomTheme = themes::install(package.u8string());
	applyCurrentTheme();
	EXPECT_NEAR(ImGui::GetStyle().Colors[ImGuiCol_Text].x, 0x12 / 255.f, 0.001f);
	EXPECT_FLOAT_EQ(ImGui::GetStyle().FramePadding.x, builtin.FramePadding.x);
	EXPECT_FLOAT_EQ(ImGui::GetStyle().ScrollbarSize, builtin.ScrollbarSize);
	config::CustomTheme = "";
	applyCurrentTheme();
	EXPECT_FLOAT_EQ(ImGui::GetStyle().Colors[ImGuiCol_Text].x, builtin.Colors[ImGuiCol_Text].x);
	EXPECT_FLOAT_EQ(ImGui::GetStyle().ScrollbarSize, builtin.ScrollbarSize);
}

TEST_F(ThemeInstallTest, MissingExternalPackageRestoresTheSelectedAppCustomTheme)
{
	struct ContextScope
	{
		ImGuiContext* previous = ImGui::GetCurrentContext();
		float scale = settings.display.uiScale;
		ContextScope() { ImGui::CreateContext(); settings.display.uiScale = 2.f; }
		~ContextScope()
		{
			ImGui::DestroyContext();
			ImGui::SetCurrentContext(previous);
			settings.display.uiScale = scale;
		}
	} context;
	struct ConfigDirectoryScope
	{
		std::string previous = get_writable_config_path("");
		explicit ConfigDirectoryScope(const fs::path& path)
		{
			fs::create_directories(path);
			set_user_config_dir(path.string());
		}
		~ConfigDirectoryScope() { set_user_config_dir(previous.empty() ? "." : previous); }
	} configDirectory(root / "app-config");

	const fs::path appThemeDirectory = fs::path(CustomTheme::directory());
	writeFile(appThemeDirectory / "user-theme.json",
		R"json({"base":"Dark","colors":{"Text":"#123456"},"style":{"FrameRounding":9}})json");
	config::UITheme = CustomTheme::CustomThemeId;
	config::UICustomTheme = "user-theme.json";
	config::CustomTheme = "";
	config::ThemeLayout = "";
	themes::reload();
	applyCurrentTheme();
	const float appText = 0x12 / 255.f;
	const float appFrameRounding = ImGui::GetStyle().FrameRounding;
	ASSERT_NEAR(ImGui::GetStyle().Colors[ImGuiCol_Text].x, appText, 0.001f);
	ASSERT_FLOAT_EQ(appFrameRounding, 18.f);

	std::string packageManifest = kManifest;
	packageManifest.insert(1, R"json("baseTheme":1,"colors":{"Text":"ABCDEF"},)json");
	const fs::path package = makePackage("install-test", packageManifest);
	ASSERT_EQ(themes::install(package.u8string()), "install-test");
	config::CustomTheme = "install-test";
	applyCurrentTheme();
	EXPECT_NEAR(ImGui::GetStyle().Colors[ImGuiCol_Text].x, 0xAB / 255.f, 0.001f);

	config::CustomTheme = "removed-package";
	themes::reload();
	applyCurrentTheme();
	EXPECT_EQ(config::UITheme.get(), CustomTheme::CustomThemeId);
	EXPECT_EQ(config::UICustomTheme.get(), "user-theme.json");
	EXPECT_NEAR(ImGui::GetStyle().Colors[ImGuiCol_Text].x, appText, 0.001f);
	EXPECT_FLOAT_EQ(ImGui::GetStyle().FrameRounding, appFrameRounding);
}

#ifdef FLYCAST_TEST_FILES
TEST_F(ThemeInstallTest, PreviewThemesValidateAssetsNoticesLimitsAndEveryInstalledLayout)
{
	struct PackageExpectation
	{
		const char* folder;
		const char* id;
		size_t layoutCount;
		std::vector<const char*> notices;
	};
	const std::vector<PackageExpectation> packages = {
		{"disc-orbit", "disc-orbit", 1, {"SOURCE.md"}},
		{"preview-art-book-next", "preview-art-book-next", 1, {"SOURCE.md"}},
		{"preview-fanart-disc", "preview-fanart-disc", 1, {"SOURCE.md"}},
		{"preview-horizon-grid", "preview-horizon-grid", 1, {"SOURCE.md"}},
	};
	const fs::path repoThemes = fs::path(FLYCAST_TEST_FILES).parent_path().parent_path() / "themes";
	ASSERT_TRUE(fs::is_directory(repoThemes)) << repoThemes;

	for (const PackageExpectation& expected : packages)
	{
		SCOPED_TRACE(expected.folder);
		const fs::path source = repoThemes / expected.folder;
		ASSERT_TRUE(fs::is_directory(source));
		for (const char* notice : expected.notices)
		{
			const fs::path noticePath = source / notice;
			ASSERT_TRUE(fs::is_regular_file(noticePath)) << noticePath;
			EXPECT_GT(fs::file_size(noticePath), 0u) << noticePath;
		}

		size_t sourceFiles = 0;
		uint64_t sourceBytes = 0;
		for (const auto& entry : fs::recursive_directory_iterator(source))
		{
			if (!entry.is_regular_file())
				continue;
			++sourceFiles;
			sourceBytes += entry.file_size();
			const auto relative = entry.path().lexically_relative(source);
			EXPECT_LE(static_cast<size_t>(std::distance(relative.begin(), relative.end())) - 1, 8u)
					<< "Installer nesting limit exceeded: " << relative;
		}
		EXPECT_GT(sourceFiles, 0u);
		EXPECT_LE(sourceFiles, 256u);
		EXPECT_LE(sourceBytes, 64u * 1024u * 1024u);

		const themes::Theme sourceTheme = themes::load(source.u8string());
		ASSERT_EQ(sourceTheme.id, expected.id);
		ASSERT_EQ(sourceTheme.layouts.size(), expected.layoutCount);
		for (const auto& layout : sourceTheme.layouts)
			for (const auto& element : layout.elements)
				if (element.type == "image")
				{
					ASSERT_FALSE(element.resolvedAsset.empty());
					std::ifstream image(element.resolvedAsset, std::ios::binary);
					const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(image)), {});
					ASSERT_TRUE(image.good() || image.eof()) << element.resolvedAsset;
					ASSERT_LE(bytes.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
					int width = 0, height = 0, channels = 0;
					ASSERT_NE(stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels), 0)
							<< "Image header failed to decode: " << element.resolvedAsset;
					EXPECT_GT(width, 0);
					EXPECT_GT(height, 0);
				}

		EXPECT_EQ(themes::install(source.u8string()), expected.id);
		const fs::path installedPath = fs::path(themes::directory()) / expected.id;
		ASSERT_TRUE(fs::is_directory(installedPath));
		size_t installedFiles = 0;
		uint64_t installedBytes = 0;
		for (const auto& entry : fs::recursive_directory_iterator(installedPath))
			if (entry.is_regular_file())
			{
				++installedFiles;
				installedBytes += entry.file_size();
			}
		EXPECT_EQ(installedFiles, sourceFiles);
		EXPECT_EQ(installedBytes, sourceBytes);

		config::CustomTheme = expected.id;
		for (const auto& sourceLayout : sourceTheme.layouts)
		{
			config::ThemeLayout = sourceLayout.id;
			const themes::Theme* installedTheme = themes::current();
			ASSERT_NE(installedTheme, nullptr);
			const themes::Layout* selectedLayout = themes::currentLayout();
			ASSERT_NE(selectedLayout, nullptr);
			EXPECT_EQ(selectedLayout->id, sourceLayout.id);
			for (const auto& element : selectedLayout->elements)
				if (element.type == "image")
				{
					const fs::path resolved = fs::weakly_canonical(element.resolvedAsset);
					const fs::path canonicalInstalled = fs::weakly_canonical(installedPath);
					const fs::path relative = resolved.lexically_relative(canonicalInstalled);
					ASSERT_FALSE(relative.empty()) << resolved;
					ASSERT_NE(*relative.begin(), fs::path("..")) << resolved;
					EXPECT_EQ(resolved.string().find(".install-"), std::string::npos) << resolved;
				}
		}
	}
}
#endif
