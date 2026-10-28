// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme.h"
#include "cfg/option.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "imgui.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace themes
{
namespace
{
namespace fs = std::filesystem;
bool initialized = false;
std::string loadedDirectory;
std::vector<CatalogEntry> entries;
std::map<std::string, Theme> loaded;
std::string catalogError, selectionError;

bool within(const fs::path& root, const fs::path& candidate)
{
	auto c = candidate.begin();
	for (auto r = root.begin(); r != root.end(); ++r, ++c)
		if (c == candidate.end() || *r != *c)
			return false;
	return true;
}

// Imports go through hostfs so Android's document provider can supply a folder.
// The installed copy is ordinary private app data, usable by the texture loader.
void copyFolder(const std::string& source, const fs::path& destination,
		unsigned depth, size_t& fileCount, uint64_t& bytes)
{
	if (depth > 8)
		throw std::runtime_error("Theme folders may be at most 8 levels deep.");
	for (const auto& entry : hostfs::storage().listContent(source))
	{
		if (entry.name == ".git" || entry.name == ".DS_Store")
			continue;
		if (entry.name.empty() || entry.name == "." || entry.name == ".."
				|| entry.name.find_first_of("/\\:") != std::string::npos
				|| entry.name.find('\0') != std::string::npos)
			throw std::runtime_error("Invalid file name in theme folder.");
		if (++fileCount > 256)
			throw std::runtime_error("Theme folders may contain at most 256 entries.");
		std::error_code ec;
		const auto status = fs::symlink_status(fs::u8path(entry.path), ec);
		if (!ec && fs::is_symlink(status))
			throw std::runtime_error("Installed theme folders cannot contain symbolic links.");
		if (!ec && !fs::is_directory(status) && !fs::is_regular_file(status))
			throw std::runtime_error("Theme folders may contain only regular files and directories.");
		const fs::path target = destination / fs::u8path(entry.name);
		if (entry.isDirectory)
		{
			fs::create_directory(target);
			copyFolder(entry.path, target, depth + 1, fileCount, bytes);
			continue;
		}
		std::unique_ptr<hostfs::File> input(hostfs::storage().openFile(entry.path, "rb"));
		if (!input)
			throw std::runtime_error("Cannot read theme file: " + entry.name);
		std::ofstream output(target, std::ios::binary);
		if (!output)
			throw std::runtime_error("Cannot write installed theme file: " + entry.name);
		char buffer[16384];
		size_t count;
		while ((count = input->read(buffer, 1, sizeof(buffer))) != 0)
		{
			bytes += count;
			if (bytes > 64 * 1024 * 1024)
				throw std::runtime_error("Theme folders may contain at most 64 MiB of files.");
			output.write(buffer, count);
			if (!output)
				throw std::runtime_error("Cannot write installed theme file: " + entry.name);
		}
		if (input->error())
			throw std::runtime_error("Cannot read theme file: " + entry.name);
		output.close();
		if (!output)
			throw std::runtime_error("Cannot finish writing installed theme file: " + entry.name);
	}
}

void ensureLoaded()
{
	if (!initialized || loadedDirectory != directory())
		reload();
}
}

std::string directory()
{
	return get_writable_data_path("library-themes/");
}

void reload()
{
	initialized = true;
	loadedDirectory = directory();
	entries.clear();
	loaded.clear();
	catalogError.clear();
	selectionError.clear();
	try
	{
		const fs::path root = fs::u8path(loadedDirectory);
		if (!fs::exists(root))
			return;
		for (const auto& folder : fs::directory_iterator(root))
		{
			const std::string id = folder.path().filename().u8string();
			if (id.empty() || id.front() == '.' || !folder.is_directory())
				continue;
			CatalogEntry entry{id, id, {}};
			try
			{
				Theme theme = load(folder.path().u8string());
				if (theme.id != id)
					throw std::runtime_error("Theme folder name must match its manifest id: " + theme.id);
				entry.name = theme.name;
				loaded.emplace(id, std::move(theme));
			}
			catch (const std::exception& e)
			{
				entry.error = e.what();
			}
			entries.push_back(std::move(entry));
		}
		std::sort(entries.begin(), entries.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
			return a.name == b.name ? a.id < b.id : a.name < b.name;
		});
	}
	catch (const std::exception& e)
	{
		catalogError = e.what();
	}
}

const std::vector<CatalogEntry>& catalog()
{
	ensureLoaded();
	return entries;
}

const Theme* current()
{
	ensureLoaded();
	selectionError.clear();
	const auto& id = config::CustomTheme.get();
	if (id.empty())
		return nullptr;
	auto theme = loaded.find(id);
	if (theme != loaded.end())
		return &theme->second;
	selectionError = "Theme '" + id + "' is unavailable. Using the selected UI theme.";
	for (const auto& entry : entries)
		if (entry.id == id && !entry.error.empty())
			selectionError += " " + entry.error;
	return nullptr;
}

const Layout* currentLayout()
{
	const Theme* theme = current();
	if (theme == nullptr || theme->layouts.empty())
		return nullptr;
	for (const auto& layout : theme->layouts)
		if (layout.id == config::ThemeLayout.get())
			return &layout;
	return &theme->layouts.front();
}

const std::string& error()
{
	current();
	return selectionError.empty() ? catalogError : selectionError;
}

std::string install(const std::string& sourceDirectory)
{
	const fs::path root = fs::u8path(directory());
	fs::create_directories(root);
	// Reject an ancestor of the installation root before creating the staging dir.
	std::error_code ec;
	const fs::path source = fs::weakly_canonical(fs::u8path(sourceDirectory), ec);
	if (!ec && within(source, fs::weakly_canonical(root)))
		throw std::runtime_error("Choose a single theme folder, outside the themes directory's parent folders.");
	const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path staging = root / (".install-" + std::to_string(nonce));
	if (!fs::create_directory(staging))
		throw std::runtime_error("Cannot create a temporary theme folder.");
	try
	{
		size_t fileCount = 0;
		uint64_t bytes = 0;
		copyFolder(sourceDirectory, staging, 0, fileCount, bytes);
		const Theme theme = load(staging.u8string());
		const fs::path destination = root / theme.id;
		if (fs::exists(destination))
			throw std::runtime_error("A theme with id '" + theme.id + "' is already installed. Remove it or use a different id.");
		fs::rename(staging, destination);
		reload();
		return theme.id;
	}
	catch (...)
	{
		fs::remove_all(staging, ec);
		throw;
	}
}

void applyPalette()
{
	const Theme* theme = current();
	if (theme == nullptr)
		return;
	for (int i = 0; i < ImGuiCol_COUNT; ++i)
	{
		auto color = theme->colors.find(ImGui::GetStyleColorName(i));
		if (color == theme->colors.end())
			continue;
		uint32_t rgba = color->second;
		ImGui::GetStyle().Colors[i] = ImVec4(((rgba >> 24) & 255) / 255.f,
				((rgba >> 16) & 255) / 255.f, ((rgba >> 8) & 255) / 255.f, (rgba & 255) / 255.f);
	}
}
}
