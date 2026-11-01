// Copyright 2026 The Hollycast Authors
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
#include <filesystem>
#include <fstream>

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
