// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_repository.h"
#include "theme.h"
#include "oslib/http_client.h"
#include "json.hpp"
#include <stb_image.h>
#include <zip.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>

namespace themes::repository
{
namespace
{
namespace fs = std::filesystem;
constexpr size_t packageLimit = 64 * 1024 * 1024;

void require(bool condition, const std::string& message)
{
	if (!condition) throw std::runtime_error(message);
}

bool https(const std::string& url)
{
	if (url.rfind("https://", 0) != 0 || url.size() > 2048) return false;
	const size_t end = url.find_first_of("/?#", 8);
	const std::string authority = url.substr(8, end == std::string::npos ? end : end - 8);
	return !authority.empty() && authority.find('@') == std::string::npos
		&& url.find_first_of("\\ \t\r\n") == std::string::npos
		&& std::none_of(url.begin(), url.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

bool safePath(const std::string& path)
{
	if (path.empty() || path.size() > 512 || path.front() == '/' || path.back() == '/'
			|| path.find_first_of("\\:\0", 0, 3) != std::string::npos) return false;
	size_t start = 0, depth = 0;
	while (start < path.size())
	{
		const size_t end = path.find('/', start);
		const std::string part = path.substr(start, end == std::string::npos ? end : end - start);
		if (part.empty() || part == "." || part == ".." || ++depth > 9) return false;
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return true;
}

std::string resolve(const std::string& base, const std::string& path)
{
	if (https(path)) return path;
	require(safePath(path) && path.find_first_of("%?#") == std::string::npos,
			"Catalogue assets must be HTTPS URLs or relative repository paths.");
	return base.substr(0, base.rfind('/') + 1) + path;
}

std::vector<unsigned char> download(const std::string& url, size_t limit, const Transport& transport)
{
	require(https(url), "Theme downloads require an HTTPS URL.");
	std::vector<unsigned char> bytes;
	const int status = transport ? transport(url, bytes) : http::get(url, bytes);
	require(http::success(status), "Theme download failed (HTTP " + std::to_string(status) + "). Check your connection and retry.");
	require(!bytes.empty() && bytes.size() <= limit, "Theme download is empty or exceeds the size limit.");
	return bytes;
}

void write(const fs::path& path, const std::vector<unsigned char>& bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	out.close();
	require(static_cast<bool>(out), "Cannot save the downloaded theme file.");
}

struct TemporaryDirectory
{
	fs::path path;
	TemporaryDirectory()
	{
		// The app data directory is writable on mobile/console builds too, where
		// std::filesystem's default /tmp may not exist.
		const fs::path root = fs::u8path(themes::directory());
		fs::create_directories(root);
		path = root / (".download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		require(fs::create_directory(path), "Cannot create the theme download folder.");
	}
	~TemporaryDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
}

std::vector<Theme> parse(const std::string& json, const std::string& catalogUrl)
{
	require(https(catalogUrl) && catalogUrl.find_first_of("?#") == std::string::npos
		&& catalogUrl.find('/', 8) != std::string::npos, "Enter the HTTPS URL of the repository's catalog.json file.");
	require(json.size() <= 2 * 1024 * 1024, "Theme catalogue exceeds 2 MiB.");
	try
	{
		const auto root = nlohmann::json::parse(json);
		require(root.at("formatVersion").is_number_integer() && root.at("formatVersion") == 1, "Unsupported theme catalogue version.");
		const auto& entries = root.at("themes");
		require(entries.is_array() && entries.size() <= 512, "Theme catalogue must contain at most 512 themes.");
		std::set<std::string> ids;
		std::vector<Theme> result;
		for (const auto& item : entries)
		{
			const auto field = [&](const char* key, size_t maximum) {
				const auto& value = item.at(key);
				require(value.is_string(), std::string("Catalogue field must be text: ") + key);
				std::string text = value.get<std::string>();
				require(!text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos,
					std::string("Invalid catalogue field: ") + key);
				return text;
			};
			Theme theme;
			theme.id = field("id", 64);
			require(std::all_of(theme.id.begin(), theme.id.end(), [](char c) {
				return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
			}) && ids.insert(theme.id).second, "Theme catalogue has an invalid or duplicate theme id.");
			theme.name = field("name", 160);
			theme.creator = field("creator", 160);
			theme.description = field("description", 4096);
			theme.version = field("version", 64);
			theme.packageUrl = resolve(catalogUrl, field("package", 2048));
			const auto& screenshots = item.at("screenshots");
			require(screenshots.is_array() && !screenshots.empty() && screenshots.size() <= 8,
				"Each theme needs between one and eight screenshots.");
			for (const auto& screenshot : screenshots)
				theme.screenshots.push_back(resolve(catalogUrl, screenshot.get<std::string>()));
			result.push_back(std::move(theme));
		}
		return result;
	}
	catch (const nlohmann::json::exception& e)
	{
		throw std::runtime_error(std::string("Invalid theme catalogue: ") + e.what());
	}
}

std::vector<Theme> fetch(const std::string& catalogUrl, const Transport& transport)
{
	const auto bytes = download(catalogUrl, 2 * 1024 * 1024, transport);
	return parse(std::string(bytes.begin(), bytes.end()), catalogUrl);
}

std::string install(const Theme& theme, const Transport& transport)
{
	// Validate even when a caller constructs an entry without going through parse().
	require(!theme.id.empty() && theme.id.size() <= 64 && std::all_of(theme.id.begin(), theme.id.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
	}), "Invalid theme id.");
	require(!fs::exists(fs::u8path(themes::directory()) / theme.id), "This theme is already installed.");
	const auto bytes = download(theme.packageUrl, packageLimit, transport);
	TemporaryDirectory temporary;
	zip_source_t* source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, nullptr);
	require(source != nullptr, "Cannot read the downloaded theme ZIP.");
	std::unique_ptr<zip_t, decltype(&zip_discard)> archive(zip_open_from_source(source, ZIP_RDONLY, nullptr), zip_discard);
	if (!archive) { zip_source_free(source); throw std::runtime_error("The theme download is not a valid ZIP archive."); }
	const auto count = zip_get_num_entries(archive.get(), 0);
	require(count > 0 && count <= 512, "Theme ZIP contains too many entries.");
	uint64_t total = 0;
	size_t files = 0;
	std::set<std::string> paths;
	for (zip_int64_t i = 0; i < count; ++i)
	{
		zip_stat_t stat;
		require(zip_stat_index(archive.get(), i, 0, &stat) == 0 && stat.name != nullptr, "Cannot read theme ZIP entry.");
		std::string path = stat.name;
		const bool directory = !path.empty() && path.back() == '/';
		if (directory) path.pop_back();
		require(safePath(path) && paths.insert(path).second, "Theme ZIP has an unsafe or duplicate file path.");
		zip_uint8_t system = 0;
		zip_uint32_t attributes = 0;
		require(zip_file_get_external_attributes(archive.get(), i, 0, &system, &attributes) == 0,
			"Cannot read theme ZIP file attributes.");
		const auto type = (attributes >> 16) & 0170000;
		require(system != ZIP_OPSYS_UNIX || type == 0 || type == (directory ? 0040000 : 0100000),
			"Theme ZIP may only contain regular files and folders.");
		if (directory) continue;
		require(++files <= 256 && stat.size <= packageLimit - total, "Theme ZIP exceeds 256 files or 64 MiB unpacked.");
		total += stat.size;
		std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(archive.get(), i, 0), zip_fclose);
		require(file != nullptr, "Cannot extract a theme ZIP file.");
		std::vector<unsigned char> contents(static_cast<size_t>(stat.size));
		size_t offset = 0;
		while (offset < contents.size())
		{
			const auto read = zip_fread(file.get(), contents.data() + offset, contents.size() - offset);
			require(read > 0, "Theme ZIP is truncated or corrupt.");
			offset += static_cast<size_t>(read);
		}
		unsigned char extra;
		require(zip_fread(file.get(), &extra, 1) == 0, "Theme ZIP is corrupt.");
		write(temporary.path / fs::u8path(path), contents);
	}
	const themes::Theme validated = themes::load(temporary.path.u8string());
	require(validated.id == theme.id, "Downloaded theme id does not match its catalogue entry.");
	return themes::install(temporary.path.u8string(), false);
}

void preview(const std::string& url, const std::string& path, const Transport& transport)
{
	const auto bytes = download(url, 8 * 1024 * 1024, transport);
	int width = 0, height = 0, channels = 0;
	require(stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels)
		&& width > 0 && height > 0 && width <= 8192 && height <= 8192
		&& static_cast<int64_t>(width) * height <= 16000000, "Theme screenshot is invalid or too large.");
	write(fs::u8path(path), bytes);
}
}
