/*
	Copyright 2024 flyinghead
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
#include "game_scanner.h"
#include "stdclass.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "cfg/option.h"
#include "oslib/i18n.h"

#include <algorithm>
#include <array>
#include <cctype>

static bool operator<(const GameMedia &left, const GameMedia &right)
{
	return (int)left.arcade < (int)right.arcade
			|| (left.arcade == right.arcade && i18n::locale()(left.name, right.name));
}

namespace {

std::string trimAndUnescapeXml(const std::string& value)
{
	std::string trimmed = trim_ws(value);
	const std::string cdataStart = "<![CDATA[";
	const std::string cdataEnd = "]]>";
	if (trimmed.size() > cdataStart.size() + cdataEnd.size()
			&& trimmed.compare(0, cdataStart.size(), cdataStart) == 0
			&& trimmed.compare(trimmed.size() - cdataEnd.size(), cdataEnd.size(), cdataEnd) == 0)
		trimmed = trimmed.substr(cdataStart.size(), trimmed.size() - cdataStart.size() - cdataEnd.size());

	auto replaceAll = [](std::string source, const std::string& oldText, const std::string& newText)
	{
		size_t pos = 0;
		while ((pos = source.find(oldText, pos)) != std::string::npos)
		{
			source.replace(pos, oldText.size(), newText);
			pos += newText.size();
		}
		return source;
	};

	trimmed = replaceAll(trimmed, "&lt;", "<");
	trimmed = replaceAll(trimmed, "&gt;", ">");
	trimmed = replaceAll(trimmed, "&quot;", "\"");
	trimmed = replaceAll(trimmed, "&apos;", "'");
	trimmed = replaceAll(trimmed, "&amp;", "&");
	return trimmed;
}

std::string normalizeMetadataKey(std::string key)
{
	key = trim_ws(key);
	while (key.size() > 2 && key[0] == '.' && (key[1] == '/' || key[1] == '\\'))
		key = key.substr(2);
	key = trim_ws(key);
	const size_t slash = key.find_last_of("/\\");
	if (slash != std::string::npos && slash + 1 < key.size())
		key = key.substr(slash + 1);
	key = get_file_basename(key);
	string_tolower(key);
	return key;
}

std::string extractTagValue(const std::string& block, const char* tag)
{
	const std::string openTagStart = std::string("<") + tag;
	const size_t openTagStartPos = block.find(openTagStart);
	if (openTagStartPos == std::string::npos)
		return {};
	const size_t openTagEnd = block.find('>', openTagStartPos);
	if (openTagEnd == std::string::npos || openTagEnd == openTagStartPos + 1)
		return {};
	const std::string closeTag = std::string("</") + tag + ">";
	const size_t closeTagPos = block.find(closeTag, openTagEnd + 1);
	if (closeTagPos == std::string::npos)
		return {};
	return trimAndUnescapeXml(block.substr(openTagEnd + 1, closeTagPos - openTagEnd - 1));
}

std::string mediaKeyFromPath(const std::string& pathOrName)
{
	return normalizeMetadataKey(get_file_basename(pathOrName));
}

bool isLikelyRemotePath(const std::string& value)
{
	return value.find("://") != std::string::npos;
}

std::string normalizeRelativeStoragePath(std::string path)
{
	path = trim_ws(path);
	std::replace(path.begin(), path.end(), '\\', '/');
	while (path.size() > 2 && path[0] == '.' && path[1] == '/')
		path = path.substr(2);
	return path;
}

std::string normalizeManualPathIfNeeded(const std::string& manualPath, const std::string& gamelistPath)
{
	const std::string trimmed = trim_ws(manualPath);
	if (trimmed.empty() || isAbsolutePath(trimmed) || isLikelyRemotePath(trimmed))
		return trimmed;
	if (gamelistPath.empty())
		return trimmed;
	return hostfs::storage().getSubPath(hostfs::storage().getParentPath(gamelistPath), normalizeRelativeStoragePath(trimmed));
}

size_t findNextGameStart(const std::string& xml, size_t searchPos)
{
	while (true)
	{
		const size_t gameStart = xml.find("<game", searchPos);
		if (gameStart == std::string::npos)
			return std::string::npos;

		const size_t tagCharPos = gameStart + 5;
		if (tagCharPos < xml.size() && (std::isspace(static_cast<unsigned char>(xml[tagCharPos])) || xml[tagCharPos] == '>'))
			return gameStart;

		searchPos = tagCharPos;
	}
}

GameListMetadataMap parseGamelistMetadata(const std::string& gamelistPath)
{
	GameListMetadataMap metadata;
	if (gamelistPath.empty())
		return metadata;

	std::string xml;
	try {
		hostfs::File* file = hostfs::storage().openFile(gamelistPath, "rb");
		if (file == nullptr)
			return metadata;
		const s64 size = file->size();
		if (size <= 0)
		{
			delete file;
			return metadata;
		}
		std::vector<char> buffer(static_cast<size_t>(size));
		const bool readOk = file->read(buffer.data(), 1, buffer.size()) == buffer.size();
		delete file;
		if (!readOk)
			return metadata;
		xml.assign(buffer.begin(), buffer.end());
	}
	catch (const hostfs::StorageException&)
	{
		return metadata;
	}

	const std::string gameOpenTag = "<game";
	const std::string gameCloseTag = "</game>";
	size_t searchPos = 0;
	while (true)
	{
		const size_t gameStart = findNextGameStart(xml, searchPos);
		if (gameStart == std::string::npos)
			break;

		const size_t gameOpenEnd = xml.find('>', gameStart);
		if (gameOpenEnd == std::string::npos)
			break;

		const size_t gameEnd = xml.find(gameCloseTag, gameOpenEnd + 1);
		if (gameEnd == std::string::npos)
			break;

		const std::string gameBlock = xml.substr(gameStart, gameEnd - gameStart + gameCloseTag.size());
		const std::string key = normalizeMetadataKey(extractTagValue(gameBlock, "path"));
		if (key.empty() || metadata.find(key) != metadata.end())
		{
			searchPos = gameEnd + gameCloseTag.size();
			continue;
		}

		GameListMetadata data;
		data.name = extractTagValue(gameBlock, "name");
		data.desc = extractTagValue(gameBlock, "desc");
		data.developer = extractTagValue(gameBlock, "developer");
		data.publisher = extractTagValue(gameBlock, "publisher");
		data.genre = extractTagValue(gameBlock, "genre");
		data.players = extractTagValue(gameBlock, "players");
		data.releaseDate = extractTagValue(gameBlock, "releaseDate");
		if (data.releaseDate.empty())
			data.releaseDate = extractTagValue(gameBlock, "releasedate");
		data.manualPath = extractTagValue(gameBlock, "manual");
		metadata[key] = std::move(data);

		searchPos = gameEnd + gameCloseTag.size();
	}

	return metadata;
}

std::unordered_map<std::string, std::string> buildManualPathIndex(const std::string& mediaRoot)
{
	static const std::array<const char*, 2> manualFolders = { "manual", "manuals" };
	std::unordered_map<std::string, std::string> manualIndex;
	if (mediaRoot.empty())
		return manualIndex;

	for (const std::string& manualFolder : manualFolders)
	{
		try {
			const std::string manualRoot = hostfs::storage().getSubPath(mediaRoot, manualFolder);
			hostfs::DirectoryTree tree(manualRoot);
			for (const hostfs::FileInfo& item : tree)
			{
				if (item.isDirectory)
					continue;
				const std::string key = mediaKeyFromPath(item.name);
				if (key.empty() || manualIndex.find(key) != manualIndex.end())
					continue;
				manualIndex[key] = item.path;
			}
		} catch (const hostfs::StorageException&) {
			continue;
		}
	}

	return manualIndex;
}

void populateMetadata(GameMedia& media, const GameListMetadataMap& metadata, const std::unordered_map<std::string, std::string>& manualIndex,
		const std::string& gamelistPath)
{
	const std::string key = mediaKeyFromPath(media.path.empty() ? media.fileName : media.path);
	if (key.empty())
		return;

	const auto it = metadata.find(key);
	if (it != metadata.end())
	{
		const GameListMetadata& entry = it->second;
		if (!entry.name.empty())
			media.name = entry.name;
		media.desc = entry.desc;
		media.developer = entry.developer;
		media.publisher = entry.publisher;
		media.genre = entry.genre;
		media.players = entry.players;
		media.releaseDate = entry.releaseDate;
		media.manualPath = normalizeManualPathIfNeeded(entry.manualPath, gamelistPath);
		if (!media.manualPath.empty() && !isLikelyRemotePath(media.manualPath))
		{
			try {
				if (!hostfs::storage().exists(media.manualPath))
					media.manualPath.clear();
			} catch (const hostfs::StorageException&) {
				media.manualPath.clear();
			}
		}
	}

	if (!media.manualPath.empty())
		return;
	const auto manualIt = manualIndex.find(key);
	if (manualIt != manualIndex.end())
		media.manualPath = manualIt->second;
}

GameMedia makeGameMedia(const std::string& name, const std::string& path, const std::string& fileName,
		const std::string& gameName, bool arcade, bool device, size_t size)
{
	GameMedia media;
	media.name = name;
	media.path = path;
	media.fileName = fileName;
	media.gameName = gameName;
	media.arcade = arcade;
	media.device = device;
	media.size = size;
	return media;
}

} // namespace

static size_t getFileSize(const hostfs::FileInfo& item)
{
	if (item.size != 0)
		return item.size;
	try {
		return hostfs::storage().getFileInfo(item.path).size;
	} catch (const hostfs::StorageException&) {
		return 0;
	}
}

void GameScanner::add_game_directory(const std::string& path, const GameListMetadataMap& metadata,
		const std::unordered_map<std::string, std::string>& manualIndex, const std::string& gamelistPath, bool hideLegacy, std::vector<GameMedia>& result)
{
	hostfs::DirectoryTree tree(path);
	std::string emptyParentPath;
	for (const hostfs::FileInfo& item : tree)
	{
		if (!running)
			break;

		if (result.empty())
		{
			// This won't work for android content uris
			size_t slash = get_last_slash_pos(item.path);
			std::string parentPath;
			if (slash != 0 && slash != std::string::npos)
				parentPath = item.path.substr(0, slash);
			else
				parentPath = item.path;
			if (parentPath != emptyParentPath)
			{
				++empty_folders_scanned;
				emptyParentPath = parentPath;
				if (empty_folders_scanned > 1000)
					content_path_looks_incorrect = true;
			}
		}
		else
		{
			content_path_looks_incorrect = false;
		}

		if (item.name.substr(0, 2) == "._")
			// Ignore Mac OS turds
			continue;
		std::string fileName(item.name);
		std::string gameName(get_file_basename(item.name));
		std::string extension = get_file_extension(item.name);
		if (extension == "zip" || extension == "7z")
		{
			string_tolower(gameName);
			auto it = arcade_games.find(gameName);
			if (it == arcade_games.end())
				continue;
			gameName = it->second->description;
			fileName = fileName + " (" + gameName + ")";
			GameMedia game = makeGameMedia(fileName, item.path, item.name, gameName, true, false, getFileSize(item));
			game.system = arcadeSystemName(*it->second);
			populateMetadata(game, metadata, manualIndex, gamelistPath);
			result.push_back(std::move(game));
			continue;
		}
		else if (extension == "bin" || extension == "lst" || extension == "dat")
		{
			if (!hideLegacy)
			{
				GameMedia game = makeGameMedia(fileName, item.path, item.name, gameName, true, false, getFileSize(item));
				game.system = "Naomi"; // Legacy ROM files always load as Naomi.
				populateMetadata(game, metadata, manualIndex, gamelistPath);
				result.push_back(std::move(game));
			}
			continue;
		}
		else if (extension == "chd" || extension == "gdi")
		{
			// Hide arcade gdroms
			std::string basename = gameName;
			string_tolower(basename);
			if (arcade_gdroms.count(basename) != 0)
				continue;
		}
		else if (extension != "cdi" && extension != "cue")
			continue;
		GameMedia game = makeGameMedia(fileName, item.path, item.name, gameName, false, false, getFileSize(item));
		populateMetadata(game, metadata, manualIndex, gamelistPath);
		result.push_back(std::move(game));
	}
}

void GameScanner::stop()
{
	LockGuard _(threadMutex);
	running = false;
    empty_folders_scanned = 0;
    content_path_looks_incorrect = false;
	if (scan_thread && scan_thread->joinable())
		scan_thread->join();
}

void GameScanner::fetch_game_list()
{
	LockGuard _(threadMutex);
	if (scan_done || running)
		return;
	if (scan_thread && scan_thread->joinable())
		scan_thread->join();
	running = true;
	// A scan owns its configuration and working list. Publish one sorted snapshot
	// so large libraries do not shift thousands of rich records for every file,
	// block the UI on insertion, or expose a half-rebuilt list during refresh.
	scan_thread = std::make_unique<std::thread>([this, paths = config::ContentPath.get(),
			gamelistPath = config::GameListPath.get(), artworkPath = config::BoxartPath.get(),
			hideLegacy = config::HideLegacyNaomiRoms.get(),
			hideDrives = config::loadBool("config", "HideCdromDrives", false)]()
		{
			ThreadName _("GameScanner");
			if (arcade_games.empty())
				for (int gameid = 0; Games[gameid].name != nullptr; gameid++)
				{
					const Game *game = &Games[gameid];
					arcade_games[game->name] = game;
					if (game->gdrom_name != nullptr)
						arcade_gdroms.insert(game->gdrom_name);
				}
			const GameListMetadataMap metadata = parseGamelistMetadata(gamelistPath);
			const std::unordered_map<std::string, std::string> manualIndex = buildManualPathIndex(artworkPath);
			std::vector<GameMedia> result;
			for (const auto& path : paths)
			{
				try {
					add_game_directory(path, metadata, manualIndex, gamelistPath, hideLegacy, result);
				} catch (const hostfs::StorageException& e) {
					// ignore
				}
				if (!running)
					break;
			}
			std::stable_sort(result.begin(), result.end());
			std::string dcbios = hostfs::findFlash("dc_", "%bios.bin;%boot.bin");
			{
				if (!hideDrives)
				{
					// CD-ROM devices
					for (const auto& drive : hostfs::getCdromDrives())
					{
						std::string name;
						if (drive.substr(0, 4) == "\\\\.\\")
							name = drive.substr(4);
						else
							name = drive;
						result.insert(result.begin(), makeGameMedia(name, drive, name, "", false, true, 0));
					}
				}
				// Dreamcast BIOS
				if (!dcbios.empty())
					result.insert(result.begin(), { i18n::T("Dreamcast BIOS") });
			}
			if (running)
			{
				LockGuard _(mutex);
				game_list.swap(result);
				++generation;
				scan_done = true;
			}
			running = false;
		});
}

void GameScanner::fetch_game_list_sync()
{
	refresh();
	fetch_game_list();
	LockGuard _(threadMutex);
	if (scan_thread && scan_thread->joinable())
		scan_thread->join();
}
