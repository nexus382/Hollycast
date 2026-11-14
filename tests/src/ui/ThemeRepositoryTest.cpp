// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "ui/theme_repository.h"
#include "ui/theme.h"
#include "stdclass.h"
#include "json.hpp"
#include <zip.h>
#include <stb_image_write.h>
#include <chrono>
#include <algorithm>
#include <map>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
const std::string url = "https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/main/catalog.json";
Json catalog()
{
	return {{"formatVersion", 1}, {"themes", Json::array({{
		{"id", "online-test"}, {"name", "Online Test"}, {"creator", "Theme Artist"},
		{"description", "A theme from the repository."}, {"version", "1.0"},
		{"package", "packages/online-test-1.0.zip"}, {"screenshots", {"screenshots/online-test.png"}}
	}})}};
}
const std::string manifest = R"({"formatVersion":1,"id":"online-test","name":"Online Test",
"layouts":[{"id":"grid","name":"Grid","mode":"grid","browser":[0,0,1,1]}]})";

class ThemeRepositoryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previousData = get_writable_data_path("");
		root = fs::temp_directory_path() / ("hollycast-repository-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(root / "data");
		set_user_data_dir((root / "data").u8string());
	}
	void TearDown() override
	{
		set_user_data_dir(previousData);
		themes::reload();
		fs::remove_all(root);
	}
	std::vector<unsigned char> zip(const std::vector<std::pair<std::string, std::string>>& entries, bool symlink = false)
	{
		const fs::path path = root / "download.zip";
		zip_t* archive = zip_open(path.u8string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
		if (!archive) throw std::runtime_error("Cannot create fixture archive");
		for (const auto& entry : entries)
		{
			auto* source = zip_source_buffer(archive, entry.second.data(), entry.second.size(), 0);
			const auto index = zip_file_add(archive, entry.first.c_str(), source, ZIP_FL_ENC_UTF_8);
			if (symlink) zip_file_set_external_attributes(archive, index, 0, ZIP_OPSYS_UNIX, 0120777u << 16);
		}
		if (zip_close(archive)) throw std::runtime_error("Cannot write fixture archive");
		std::ifstream in(path, std::ios::binary);
		return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {});
	}
	themes::repository::Theme entry() { return themes::repository::parse(catalog().dump(), url).front(); }
	static themes::repository::Theme entryFor(const std::string& id)
	{
		themes::repository::Theme theme;
		theme.id = id;
		theme.packageUrl = "https://example.com/packages/" + id + ".zip";
		return theme;
	}
	static themes::repository::Transport servePackages(
		const std::map<std::string, std::vector<unsigned char>>& packages)
	{
		return [&packages](const std::string& requested, std::vector<unsigned char>& out) {
			const auto found = packages.find(requested);
			if (found == packages.end()) return 404;
			out = found->second;
			return 200;
		};
	}
	themes::repository::Transport transport(const std::vector<unsigned char>& data, int status = 200)
	{
		return [data, status](const std::string&, std::vector<unsigned char>& out) { out = data; return status; };
	}
	void expectNoInstall()
	{
		EXPECT_FALSE(fs::exists(fs::path(themes::directory()) / "online-test"));
		if (fs::exists(themes::directory()))
			for (const auto& file : fs::directory_iterator(themes::directory()))
			{
				EXPECT_NE(file.path().filename().u8string().rfind(".install-", 0), 0u);
				EXPECT_NE(file.path().filename().u8string().rfind(".download-", 0), 0u);
			}
	}
	fs::path root;
	std::string previousData;
};
}

#ifdef FLYCAST_TEST_FILES
TEST_F(ThemeRepositoryTest, InstallsEveryPublishedThemeManifestFromPinnedCatalogOffline)
{
	const fs::path repositoryRoot = fs::path(FLYCAST_TEST_FILES).parent_path().parent_path();
	const fs::path fixtureRoot = fs::path(FLYCAST_TEST_FILES) / "community-theme-catalog";
	std::ifstream catalogInput(fixtureRoot / "catalog.json", std::ios::binary);
	ASSERT_TRUE(catalogInput);
	const std::string catalogJson((std::istreambuf_iterator<char>(catalogInput)), {});
	const std::string catalogUrl = "https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/96e169558c9f9120b06f8dcd32e9534282a8f705/catalog.json";
	const auto entries = themes::repository::parse(catalogJson, catalogUrl);
	ASSERT_EQ(entries.size(), 5u);

	std::ifstream fontInput(repositoryRoot / "themes/demo-disc/assets/Orbitron-700.ttf", std::ios::binary);
	ASSERT_TRUE(fontInput);
	const std::string font((std::istreambuf_iterator<char>(fontInput)), {});
	ASSERT_FALSE(font.empty());

	std::map<std::string, std::vector<unsigned char>> packages;
	for (const auto& theme : entries)
	{
		SCOPED_TRACE(theme.id);
		const fs::path manifestPath = fixtureRoot / "themes" / theme.id / "theme.json";
		std::ifstream manifestInput(manifestPath, std::ios::binary);
		ASSERT_TRUE(manifestInput) << manifestPath;
		const std::string manifest((std::istreambuf_iterator<char>(manifestInput)), {});
		const Json manifestJson = Json::parse(manifest);
		ASSERT_EQ(manifestJson.at("id").get<std::string>(), theme.id);

		std::set<std::string> assets;
		const auto collectAssets = [&](const Json& value, const auto& visit) -> void {
			if (value.is_string())
			{
				const std::string path = value.get<std::string>();
				if (path.rfind("assets/", 0) == 0 && fs::path(path).has_extension())
					assets.insert(path);
			}
			else if (value.is_array())
				for (const auto& child : value) visit(child, visit);
			else if (value.is_object())
				for (auto it = value.begin(); it != value.end(); ++it) visit(it.value(), visit);
		};
		collectAssets(manifestJson, collectAssets);

		std::vector<std::pair<std::string, std::string>> files{{"theme.json", manifest}};
		for (const auto& asset : assets)
			files.emplace_back(asset, fs::path(asset).extension() == ".ttf" ? font : "fixture-image");
		packages.emplace(theme.packageUrl, zip(files));
	}

	const auto serve = servePackages(packages);
	for (const auto& theme : entries)
	{
		SCOPED_TRACE(theme.id);
		EXPECT_EQ(themes::repository::install(theme, serve), theme.id);
		themes::reload();
		const auto& local = themes::catalog();
		const auto installed = std::find_if(local.begin(), local.end(), [&](const auto& item) { return item.id == theme.id; });
		ASSERT_NE(installed, local.end());
		EXPECT_TRUE(installed->error.empty()) << installed->error;
	}
}
#endif
TEST_F(ThemeRepositoryTest, ParsesMetadataAndResolvesRelativeRepositoryAssets)
{
	const auto themes = themes::repository::parse(catalog().dump(), url);
	ASSERT_EQ(themes.size(), 1u);
	EXPECT_EQ(themes[0].creator, "Theme Artist");
	EXPECT_EQ(themes[0].version, "1.0");
	EXPECT_EQ(themes[0].packageUrl, "https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/main/packages/online-test-1.0.zip");
	EXPECT_EQ(themes[0].screenshots.front(), "https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/main/screenshots/online-test.png");
}

TEST_F(ThemeRepositoryTest, RejectsInvalidIdsDuplicatesVersionsAndMissingScreenshots)
{
	for (const auto& id : {"../escape", "Uppercase", "bad/id", ""})
	{
		auto json = catalog(); json["themes"][0]["id"] = id;
		EXPECT_THROW(themes::repository::parse(json.dump(), url), std::runtime_error);
	}
	auto json = catalog(); json["themes"].push_back(json["themes"][0]);
	EXPECT_THROW(themes::repository::parse(json.dump(), url), std::runtime_error);
	json = catalog(); json["formatVersion"] = 2;
	EXPECT_THROW(themes::repository::parse(json.dump(), url), std::runtime_error);
	json = catalog(); json["themes"][0]["screenshots"] = Json::array();
	EXPECT_THROW(themes::repository::parse(json.dump(), url), std::runtime_error);
	EXPECT_THROW(themes::repository::parse("not json", url), std::runtime_error);
}

TEST_F(ThemeRepositoryTest, RejectsInsecureOrEscapingAssetUrls)
{
	for (const auto& path : {"http://example.com/a.zip", "../a.zip", "//example.com/a.zip", "a/../b.zip", "a%2fb.zip", "a\\b.zip", "https://user@example.com/a.zip"})
	{
		auto json = catalog(); json["themes"][0]["package"] = path;
		EXPECT_THROW(themes::repository::parse(json.dump(), url), std::runtime_error) << path;
	}
	EXPECT_THROW(themes::repository::parse(catalog().dump(), "http://example.com/catalog.json"), std::runtime_error);
}

TEST_F(ThemeRepositoryTest, ReportsHttpFailuresAndInvalidCatalogues)
{
	EXPECT_THROW(themes::repository::fetch(url, transport({}, 404)), std::runtime_error);
	EXPECT_THROW(themes::repository::fetch(url, transport({'{'})), std::runtime_error);
	const auto json = catalog().dump();
	EXPECT_EQ(themes::repository::fetch(url, transport({json.begin(), json.end()})).size(), 1u);
}

TEST_F(ThemeRepositoryTest, InstallsDownloadedPackageAndProtectsExistingFiles)
{
	const auto bytes = zip({{"theme.json", manifest}, {"art/readme.txt", "preserved"}});
	EXPECT_EQ(themes::repository::install(entry(), transport(bytes)), "online-test");
	themes::reload();
	EXPECT_TRUE(fs::is_regular_file(fs::path(themes::directory()) / "online-test" / "art/readme.txt"));
	const auto& local = themes::catalog();
	EXPECT_NE(std::find_if(local.begin(), local.end(), [](const auto& item) { return item.id == "online-test" && item.error.empty(); }), local.end());
	bool requested = false;
	EXPECT_THROW(themes::repository::install(entry(), [&](const auto&, auto&) { requested = true; return 500; }), std::runtime_error);
	EXPECT_FALSE(requested);
}

TEST_F(ThemeRepositoryTest, InstallsMultipleThemesInSequence)
{
	const auto bytesFor = [this](const std::string& id) {
		std::string text = manifest;
		text.replace(text.find("online-test"), 11, id);
		return zip({{"theme.json", text}, {"art/readme.txt", id}});
	};
	const std::map<std::string, std::vector<unsigned char>> packages = {
		{"https://example.com/packages/first-test.zip", bytesFor("first-test")},
		{"https://example.com/packages/second-test.zip", bytesFor("second-test")},
	};
	const themes::repository::Transport serve = servePackages(packages);

	EXPECT_EQ(themes::repository::install(entryFor("first-test"), serve), "first-test");
	themes::reload();
	EXPECT_EQ(themes::repository::install(entryFor("second-test"), serve), "second-test");
	themes::reload();

	const auto& local = themes::catalog();
	for (const char* id : {"first-test", "second-test"})
	{
		const auto found = std::find_if(local.begin(), local.end(), [&](const auto& item) { return item.id == id; });
		ASSERT_NE(found, local.end()) << id;
		EXPECT_TRUE(found->error.empty()) << id << ": " << found->error;
		EXPECT_TRUE(fs::is_regular_file(fs::path(themes::directory()) / id / "theme.json")) << id;
	}
}

TEST_F(ThemeRepositoryTest, InstallsCurrentCatalogueFormatsInSequence)
{
	// Mirrors the published catalogue shapes: a list layout using the newer
	// navigation rect and releaseYear flag, and a carousel layout that this
	// build renders as a grid. Both must install, one after the other.
	const std::string archiveManifest = R"({"formatVersion":1,"id":"archive-test","name":"Archive Test",
"layouts":[{"id":"room","name":"Room","mode":"list","browser":[0.2,0.2,0.6,0.7],
"navigation":[0.04,0.25,0.16,0.4],"toolbar":"hidden","list":{"releaseYear":true}}]})";
	const std::string bloomManifest = R"({"formatVersion":1,"id":"bloom-test","name":"Bloom Test",
"layouts":[{"id":"gallery","name":"Gallery","mode":"carousel","browser":[0,0,1,1],"columns":5},
{"id":"index","name":"Index","mode":"list","browser":[0,0,1,1]}]})";
	const std::map<std::string, std::vector<unsigned char>> packages = {
		{"https://example.com/packages/archive-test.zip", zip({{"theme.json", archiveManifest}})},
		{"https://example.com/packages/bloom-test.zip", zip({{"theme.json", bloomManifest}})},
	};
	const themes::repository::Transport serve = servePackages(packages);

	EXPECT_EQ(themes::repository::install(entryFor("archive-test"), serve), "archive-test");
	themes::reload();
	EXPECT_EQ(themes::repository::install(entryFor("bloom-test"), serve), "bloom-test");
	themes::reload();

	const auto& local = themes::catalog();
	for (const char* id : {"archive-test", "bloom-test"})
	{
		const auto found = std::find_if(local.begin(), local.end(), [&](const auto& item) { return item.id == id; });
		ASSERT_NE(found, local.end()) << id;
		EXPECT_TRUE(found->error.empty()) << id << ": " << found->error;
	}
	const themes::Theme bloom = themes::load((fs::path(themes::directory()) / "bloom-test").u8string());
	ASSERT_EQ(bloom.layouts.size(), 2u);
	EXPECT_EQ(bloom.layouts[0].mode, "grid");
	EXPECT_EQ(bloom.layouts[1].mode, "list");
}

TEST_F(ThemeRepositoryTest, RejectsArchiveTraversalAndSymlinksWithoutPublishing)
{
	for (const auto& path : {"../escaped.txt", "/absolute.txt", "art/../../escaped.txt", "art\\escaped.txt", "C:/escaped.txt"})
	{
		EXPECT_THROW(themes::repository::install(entry(), transport(zip({{"theme.json", manifest}, {path, "bad"}}))), std::runtime_error);
		expectNoInstall();
	}
	EXPECT_THROW(themes::repository::install(entry(), transport(zip({{"theme.json", manifest}}, true))), std::runtime_error);
	expectNoInstall();
}

TEST_F(ThemeRepositoryTest, RejectsMismatchedManifestAndCorruptDownloads)
{
	std::string wrong = manifest;
	wrong.replace(wrong.find("online-test"), 11, "wrong-theme");
	EXPECT_THROW(themes::repository::install(entry(), transport(zip({{"theme.json", wrong}}))), std::runtime_error);
	EXPECT_THROW(themes::repository::install(entry(), transport(zip({{"theme.json", "{}"}}))), std::runtime_error);
	EXPECT_THROW(themes::repository::install(entry(), transport({'n','o','t','z','i','p'})), std::runtime_error);
	expectNoInstall();
}

TEST_F(ThemeRepositoryTest, RejectsOversizedAndOverpopulatedArchives)
{
	EXPECT_THROW(themes::repository::install(entry(), transport(zip({{"theme.json", manifest}, {"huge.txt", std::string(64 * 1024 * 1024, 'x')}}))), std::runtime_error);
	std::vector<std::pair<std::string, std::string>> files{{"theme.json", manifest}};
	for (int i = 0; i < 256; ++i) files.emplace_back("file" + std::to_string(i), "x");
	EXPECT_THROW(themes::repository::install(entry(), transport(zip(files))), std::runtime_error);
	expectNoInstall();
}

TEST_F(ThemeRepositoryTest, ValidatesScreenshotBeforeSavingAndAllowsRetry)
{
	const fs::path image = root / "preview.png";
	EXPECT_THROW(themes::repository::preview("https://example.com/preview.png", image.u8string(), transport({'b','a','d'})), std::runtime_error);
	EXPECT_FALSE(fs::exists(image));
	std::vector<unsigned char> png;
	const unsigned char pixel[] = {80, 120, 255, 255};
	ASSERT_TRUE(stbi_write_png_to_func([](void* context, void* data, int count) {
		auto& bytes = *static_cast<std::vector<unsigned char>*>(context);
		bytes.insert(bytes.end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + count);
	}, &png, 1, 1, 4, pixel, 4));
	themes::repository::preview("https://example.com/preview.png", image.u8string(), transport(png));
	EXPECT_EQ(fs::file_size(image), png.size());
}
