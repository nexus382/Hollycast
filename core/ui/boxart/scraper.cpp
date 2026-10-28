/*
	Copyright 2022 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "scraper.h"
#include "oslib/http_client.h"
#include "oslib/storage.h"
#include "stdclass.h"
#include "emulator.h"
#include "imgread/common.h"
#include "imgread/isofs.h"
#include "reios/reios.h"
#include "pvrparser.h"
#include <stb_image_write.h>
#include <mutex>
#include <random>
#include <memory>

json GameBoxart::to_json(const std::string& baseArtPath) const
{
	// Convert absolute path to base path-relative ones
	std::string artPath;
	if (!boxartPath.empty() && boxartPath.substr(0, baseArtPath.length()) == baseArtPath)
		artPath = boxartPath.substr(baseArtPath.length());
	else
		artPath = boxartPath;
	json j = {
		{ "file_name", fileName },
		{ "name", name },
		{ "unique_id", uniqueId },
		{ "search_name", searchName },
		{ "region", region },
		{ "release_date", releaseDate },
		{ "overview", overview },
		{ "boxart_path", artPath },
		{ "boxart_url", boxartUrl },
		{ "parsed", offlineArtState == OfflineArtState::Complete || offlineArtState == OfflineArtState::NoArtwork },
		{ "offline_art_state", offlineArtState == OfflineArtState::Complete ? "complete"
				: offlineArtState == OfflineArtState::NoArtwork ? "no_artwork" : "pending" },
		{ "game_path", gamePath },
		{ "scraped", scraped },
		{ "arcade", arcade },
	};
	return j;
}

GameBoxart::GameBoxart(const json& j, const std::string& baseArtPath)
{
	loadProperty(fileName, j, "file_name");
	loadProperty(name, j, "name");
	loadProperty(uniqueId, j, "unique_id");
	loadProperty(searchName, j, "search_name");
	loadProperty(region, j, "region");
	loadProperty(releaseDate, j, "release_date");
	loadProperty(overview, j, "overview");
	loadProperty(boxartPath, j, "boxart_path");
	loadProperty(boxartUrl, j, "boxart_url");
	loadProperty(gamePath, j, "game_path");
	std::string state;
	loadProperty(state, j, "offline_art_state");
	if (state == "complete")
		offlineArtState = OfflineArtState::Complete;
	else if (state == "no_artwork")
		offlineArtState = OfflineArtState::NoArtwork;
	else if (state.empty())
	{
		bool parsed = false;
		loadProperty(parsed, j, "parsed");
		if (parsed && !boxartPath.empty())
			offlineArtState = OfflineArtState::Complete;
	}
	loadProperty(scraped, j, "scraped");
	loadProperty(arcade, j, "arcade");
	if (!boxartPath.empty() && !isAbsolutePath(boxartPath))
		boxartPath = baseArtPath + boxartPath;
}

bool Scraper::downloadImage(const std::string& url, const std::string& localName, bool mute)
{
	DEBUG_LOG(COMMON, "downloading %s", url.c_str());
	std::vector<u8> content;
	std::string contentType;
	if (!http::success(http::get(url, content, contentType)))
	{
		if (!mute)
			WARN_LOG(COMMON, "downloadImage http error: %s", url.c_str());
		return false;
	}
	if (contentType.substr(0, 6) != "image/")
	{
		if (!mute)
			WARN_LOG(COMMON, "downloadImage bad content type %s", contentType.c_str());
		return false;
	}
	if (content.empty())
	{
		if (!mute)
			WARN_LOG(COMMON, "downloadImage: empty content");
		return false;
	}
	FILE *f = nowide::fopen(localName.c_str(), "wb");
	if (f == nullptr)
	{
		WARN_LOG(COMMON, "can't create local file %s: error %d", localName.c_str(), errno);
		return false;
	}
	fwrite(&content[0], 1, content.size(), f);
	fclose(f);

	return true;
}

std::string Scraper::makeUniqueFilename(const std::string& url)
{
	static std::random_device randomDev;
	static std::mt19937 mt(randomDev());
	static std::uniform_int_distribution<int> dist(1, 1000000000);
	static std::mutex filenameMutex;

	std::lock_guard<std::mutex> guard(filenameMutex);
	std::string extension = get_file_extension(url);
	std::string path;
	do {
		path = saveDirectory + std::to_string(dist(mt)) + "." + extension;
	} while (file_exists(path));
	return path;
}

void OfflineScraper::scrape(GameBoxart& item)
{
	if (item.offlineArtState != OfflineArtState::Pending)
		return;
	item.offlineArtState = OfflineArtState::Failed;
	if (item.searchName.empty())
		item.searchName = item.name;
	try {
		int platform = getGamePlatform(item.fileName);
		if (platform == DC_PLATFORM_DREAMCAST)
		{
			if (item.gamePath.empty())
				return;
			std::unique_ptr<Disc> disc(OpenDisc(item.gamePath));
			if (disc == nullptr)
				return;

			u8 sector[2048];
			disc->ReadSectors(disc->GetBaseFAD(), 1, sector, sizeof(sector));
			ip_meta_t diskId;
			memcpy(&diskId, sector, sizeof(diskId));
			if (memcmp(diskId.hardware_id, "SEGA SEGAKATANA ", sizeof(diskId.hardware_id))
					|| memcmp(diskId.maker_id, "SEGA ENTERPRISES", sizeof(diskId.maker_id)))
			{
				WARN_LOG(COMMON, "Invalid IP META for disk %s", item.gamePath.c_str());
				return;
			}

			item.uniqueId = trim_trailing_ws(std::string(diskId.product_number, sizeof(diskId.product_number)));
			std::replace_if(item.uniqueId.begin(), item.uniqueId.end(), [](u8 c) {
				return !std::isprint(c);
			}, ' ');

			item.searchName = trim_trailing_ws(std::string(diskId.software_name, sizeof(diskId.software_name)));
			if (item.searchName.empty())
				item.searchName = item.name;
			else
			{
				std::replace_if(item.searchName.begin(), item.searchName.end(), [](u8 c) {
					return !std::isprint(c);
				}, ' ');
			}

			if (diskId.area_symbols[0] != '\0')
			{
				item.region = 0;
				if (diskId.area_symbols[0] == 'J')
					item.region |= GameBoxart::JAPAN;
				if (diskId.area_symbols[1] == 'U')
					item.region |= GameBoxart::USA;
				if (diskId.area_symbols[2] == 'E')
					item.region |= GameBoxart::EUROPE;
			}
			else
				item.region = GameBoxart::JAPAN | GameBoxart::USA | GameBoxart::EUROPE;
			if (!item.boxartPath.empty())
			{
				item.offlineArtState = OfflineArtState::Complete;
				return;
			}
			IsoFs isofs(disc.get());
			bool valid = false;
			std::unique_ptr<IsoFs::Directory> root(isofs.getRoot(&valid));
			if (!valid || root == nullptr)
				return;
			std::unique_ptr<IsoFs::Entry> entry(root->getEntry("0GDTEX.PVR"));
			if (entry == nullptr || entry->isDirectory())
			{
				item.offlineArtState = OfflineArtState::NoArtwork;
				return;
			}
			auto *file = static_cast<IsoFs::File *>(entry.get());
			std::vector<u8> data(file->getSize());
			if (file->read(data.data(), data.size()) != data.size())
				return;
			std::vector<u8> out;
			u32 w, h;
			if (!pvrParse(data.data(), data.size(), w, h, out))
				return;
			const std::string path = makeUniqueFilename("gdtex.png");
			FILE *output = nowide::fopen(path.c_str(), "wb");
			if (output == nullptr)
				return;
			struct PngOutput { FILE *file; bool failed = false; } context{output};
			const auto save = [](void *opaque, void *bytes, int size) {
				auto& output = *static_cast<PngOutput *>(opaque);
				if (fwrite(bytes, 1, size, output.file) != static_cast<size_t>(size))
					output.failed = true;
			};
			stbi_flip_vertically_on_write(0);
			const bool encoded = stbi_write_png_to_func(save, &context, w, h, 4, out.data(), 0) != 0;
			const bool closed = fclose(output) == 0;
			if (!encoded || context.failed || !closed)
			{
				nowide::remove(path.c_str());
				return;
			}
			item.setBoxartPath(path);
			item.offlineArtState = OfflineArtState::Complete;
		}
		else
		{
			item.uniqueId.clear();
			// Use first one in case of alternate names (Virtua Tennis / Power Smash)
			size_t spos = item.searchName.find('/');
			if (spos != std::string::npos)
				item.searchName = trim_trailing_ws(item.searchName.substr(0, spos));
			// Delete trailing (...) and [...]
			while (!item.searchName.empty())
			{
				size_t pos{ std::string::npos };
				if (item.searchName.back() == ')')
					pos = item.searchName.find_last_of('(');
				else if (item.searchName.back() == ']')
					pos = item.searchName.find_last_of('[');
				if (pos == std::string::npos)
					break;
				item.searchName = trim_trailing_ws(item.searchName.substr(0, pos));
			}
			std::string extension = get_file_extension(item.fileName);
			if (extension != "zip" && extension != "7z" && extension != "lst")
			{
				std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(item.gamePath, "rb"));
				if (file == nullptr)
					return;
				file->seek(0x30, SEEK_SET);
				u8 buf[0x20];
				if (file->read(buf, 1, sizeof(buf)) != sizeof(buf))
					return;
				item.uniqueId = trim_trailing_ws(std::string(&buf[0], &buf[0x20]));
			}
			item.offlineArtState = OfflineArtState::NoArtwork;
		}
	} catch (const std::exception& e) {
		WARN_LOG(COMMON, "Physical boxart failed for %s: %s", item.gamePath.c_str(), e.what());
	} catch (...) {
		WARN_LOG(COMMON, "Physical boxart failed for %s", item.gamePath.c_str());
	}
}
