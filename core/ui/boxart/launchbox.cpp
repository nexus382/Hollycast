/* Copyright 2026 The Hollycast Authors. SPDX-License-Identifier: GPL-2.0-or-later */
#include "launchbox.h"
#include "archive/ZipArchive.h"
#include "oslib/http_client.h"
#include "oslib/directory.h"
#include "stdclass.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <memory>
#include <string_view>

namespace {
constexpr const char *metadataUrl = "https://gamesdb.launchbox-app.com/Metadata.zip";
constexpr const char *imageBaseUrl = "https://images.launchbox-app.com/";

// The export's fields are text, escaped XML or CDATA. Do not expand DTDs or
// external entities. Numeric entities are decoded to UTF-8 for regional names.
std::string xmlText(std::string_view text)
{
	std::string out;
	for (size_t i = 0; i < text.size();)
	{
		if (text.substr(i, 9) == "<![CDATA[")
		{
			const size_t end = text.find("]]>", i + 9);
			if (end == text.npos)
				throw std::runtime_error("Invalid LaunchBox CDATA");
			out.append(text.substr(i + 9, end - i - 9));
			i = end + 3;
			continue;
		}
		if (text[i] != '&')
		{
			out += text[i++];
			continue;
		}
		const size_t end = text.find(';', i);
		if (end == text.npos)
			throw std::runtime_error("Invalid LaunchBox XML entity");
		const std::string entity(text.substr(i + 1, end - i - 1));
		if (entity == "amp") out += '&';
		else if (entity == "lt") out += '<';
		else if (entity == "gt") out += '>';
		else if (entity == "quot") out += '"';
		else if (entity == "apos") out += '\'';
		else if (!entity.empty() && entity[0] == '#')
		{
			const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
			size_t used = 0;
			const std::string digits = entity.substr(hex ? 2 : 1);
			const unsigned long code = std::stoul(digits, &used, hex ? 16 : 10);
			if (used != digits.size() || code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
				throw std::runtime_error("Invalid LaunchBox Unicode entity");
			if (code < 0x80) out += static_cast<char>(code);
			else {
				if (code >= 0x10000) out += static_cast<char>(0xf0 | (code >> 18));
				if (code >= 0x800) out += static_cast<char>((code >= 0x10000 ? 0x80 : 0xe0) | ((code >> 12) & 0x3f));
				out += static_cast<char>((code >= 0x800 ? 0x80 : 0xc0) | ((code >> 6) & 0x3f));
				out += static_cast<char>(0x80 | (code & 0x3f));
			}
		}
		else throw std::runtime_error("Unknown LaunchBox XML entity");
		i = end + 1;
	}
	return trim_ws(out);
}

std::string field(std::string_view record, const std::string& tag)
{
	const std::string open = "<" + tag + ">";
	const size_t start = record.find(open);
	if (start == record.npos)
		return {};
	const size_t end = record.find("</" + tag + ">", start + open.size());
	if (end == record.npos)
		throw std::runtime_error("Incomplete LaunchBox field");
	return xmlText(record.substr(start + open.size(), end - start - open.size()));
}

bool supportedPlatform(const std::string& platform)
{
	return platform == "Sega Dreamcast" || platform == "Arcade";
}

bool supportedCover(const std::string& type)
{
	return type == "Box - Front" || type == "Box - Front - Reconstructed" || type == "Advertisement Flyer - Front";
}

bool validImageFile(const std::string& file)
{
	// Export filenames are CDN basenames, never paths or arbitrary URLs.
	if (file.empty() || file.size() > 128)
		return false;
	for (unsigned char c : file)
		if (!std::isalnum(c) && c != '-' && c != '.')
			return false;
	const std::string extension = get_file_extension(file);
	return extension == "png" || extension == "jpg" || extension == "jpeg";
}

int regionScore(const std::string& region, u32 mask)
{
	if ((region == "Japan" && (mask & GameBoxart::JAPAN))
			|| (region == "North America" && (mask & GameBoxart::USA))
			|| ((region == "Europe" || region == "United Kingdom") && (mask & GameBoxart::EUROPE)))
		return 3;
	if (region.empty() || region == "World")
		return 2;
	return 1;
}

json readJson(const std::string& path)
{
	std::unique_ptr<FILE, decltype(&fclose)> file(nowide::fopen(path.c_str(), "rb"), fclose);
	if (!file)
		return {};
	return json::parse(file.get(), nullptr, false);
}

void writeJson(const std::string& path, const json& data)
{
	const std::string temporary = path + ".tmp";
	const std::string text = data.dump();
	FILE *file = nowide::fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return;
	const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool closed = fclose(file) == 0;
	if (!written || !closed || flycast::rename(temporary.c_str(), path.c_str()) != 0)
	{
		nowide::remove(temporary.c_str());
		WARN_LOG(COMMON, "Cannot save LaunchBox cache: %s", path.c_str());
	}
}
} // namespace

std::string LaunchBoxCatalog::normalizeTitle(std::string title)
{
	title = trim_ws(title);
	// ROM set suffixes are not part of the game title. The internal disc title
	// is tried first, before a filename with any region/disc suffixes removed.
	while (!title.empty() && (title.back() == ')' || title.back() == ']'))
	{
		const size_t start = title.find_last_of(title.back() == ')' ? '(' : '[');
		if (start == title.npos)
			break;
		title = trim_ws(title.substr(0, start));
	}
	std::string normalized;
	for (unsigned char c : title)
	{
		if (c >= 0x80 || std::isalnum(c))
			normalized += static_cast<char>(c >= 0x80 ? c : std::tolower(c));
	}
	// LaunchBox commonly files titles starting with "The" under "..., The".
	if (normalized.size() > 3 && title.size() > 4 && stricmp(title.substr(0, 4).c_str(), "the ") == 0)
		normalized = normalized.substr(3) + "the";
	return normalized;
}

void LaunchBoxCatalog::read(const Reader& reader)
{
	LaunchBoxCatalog parsed;
	std::array<char, 65536> chunk;
	std::string buffer;
	bool complete = false;
	while (const size_t size = reader(chunk.data(), chunk.size()))
	{
		if (size > chunk.size())
			throw std::runtime_error("Cannot read LaunchBox metadata");
		buffer.append(chunk.data(), size);
		size_t position = 0;
		while (true)
		{
			const size_t start = buffer.find('<', position);
			if (start == buffer.npos) { position = buffer.size(); break; }
			const size_t endTag = buffer.find('>', start);
			if (endTag == buffer.npos) { position = start; break; }
			const std::string tag = buffer.substr(start + 1, endTag - start - 1);
			if (tag == "/LaunchBox") complete = true;
			if (tag != "Game" && tag != "GameAlternateName" && tag != "GameImage")
			{
				position = endTag + 1;
				continue;
			}
			const std::string close = "</" + tag + ">";
			const size_t end = buffer.find(close, endTag + 1);
			if (end == buffer.npos) { position = start; break; }
			const std::string_view record(buffer.data() + endTag + 1, end - endTag - 1);
			if (tag == "Game")
			{
				const std::string platform = field(record, "Platform");
				if (supportedPlatform(platform))
				{
					Game game;
					game.id = std::stoi(field(record, "DatabaseID"));
					game.name = field(record, "Name");
					game.platform = platform;
					if (game.id > 0 && !game.name.empty())
						parsed.games.emplace(game.id, std::move(game));
				}
			}
			else
			{
				const int id = std::stoi(field(record, "DatabaseID"));
				auto game = parsed.games.find(id);
				if (game != parsed.games.end())
				{
					if (tag == "GameAlternateName")
						game->second.aliases.push_back(field(record, "AlternateName"));
					else
					{
						Cover cover{field(record, "FileName"), field(record, "Region"), field(record, "Type")};
						if (supportedCover(cover.type) && validImageFile(cover.file))
							game->second.covers.push_back(std::move(cover));
					}
				}
			}
			position = end + close.size();
		}
		buffer.erase(0, position);
		if (buffer.size() > 1024 * 1024)
			throw std::runtime_error("Oversized LaunchBox XML record");
	}
	if (!complete || parsed.games.empty() || !trim_ws(buffer).empty())
		throw std::runtime_error("Incomplete LaunchBox catalog");
	parsed.indexNames();
	*this = std::move(parsed);
}

void LaunchBoxCatalog::indexNames()
{
	names.clear();
	for (const auto& entry : games)
	{
		const int id = entry.first;
		const Game& game = entry.second;
		auto add = [&](const std::string& name) {
			const std::string key = normalizeTitle(name);
			if (!key.empty()) names[game.platform + ":" + key].push_back(id);
		};
		add(game.name);
		for (const std::string& alias : game.aliases) add(alias);
	}
}

json LaunchBoxCatalog::toJson() const
{
	json records = json::array();
	for (const auto& [id, game] : games)
	{
		json covers = json::array();
		for (const Cover& cover : game.covers)
			covers.push_back({{"file", cover.file}, {"region", cover.region}, {"type", cover.type}});
		records.push_back({{"id", id}, {"name", game.name}, {"platform", game.platform}, {"aliases", game.aliases}, {"covers", covers}});
	}
	return {{"version", 1}, {"games", records}};
}

void LaunchBoxCatalog::loadJson(const json& data)
{
	if (data.at("version") != 1)
		throw std::runtime_error("Unsupported LaunchBox catalog version");
	LaunchBoxCatalog parsed;
	for (const auto& record : data.at("games"))
	{
		Game game;
		game.id = record.at("id");
		game.name = record.at("name");
		game.platform = record.at("platform");
		game.aliases = record.at("aliases").get<std::vector<std::string>>();
		for (const auto& image : record.at("covers"))
		{
			Cover cover{image.at("file"), image.at("region"), image.at("type")};
			if (supportedCover(cover.type) && validImageFile(cover.file)) game.covers.push_back(std::move(cover));
		}
		if (game.id > 0 && supportedPlatform(game.platform)) parsed.games.emplace(game.id, std::move(game));
	}
	if (parsed.games.empty()) throw std::runtime_error("Empty LaunchBox catalog");
	parsed.indexNames();
	*this = std::move(parsed);
}

LaunchBoxCatalog::Match LaunchBoxCatalog::match(const GameBoxart& item) const
{
	const std::string platform = item.arcade ? "Arcade" : "Sega Dreamcast";
	for (const std::string& title : {item.searchName, item.name, get_file_basename(item.fileName)})
	{
		const auto found = names.find(platform + ":" + normalizeTitle(title));
		if (found == names.end()) continue;
		const Game *game = nullptr;
		bool ambiguous = false;
		for (int id : found->second)
		{
			if (game != nullptr && game->id != id) { ambiguous = true; break; }
			game = &games.at(id);
		}
		if (ambiguous || game == nullptr) continue;
		const Cover *best = nullptr;
		int bestScore = -1;
		for (const Cover& cover : game->covers)
		{
			const int typeScore = cover.type == "Box - Front" ? 30 : cover.type == "Box - Front - Reconstructed" ? 20 : 10;
			const int score = 100 * regionScore(cover.region, item.region) + typeScore;
			if (score > bestScore || (score == bestScore && best != nullptr && cover.file < best->file))
			{
				best = &cover;
				bestScore = score;
			}
		}
		if (best != nullptr) return {game->id, game->name, imageBaseUrl + best->file};
	}
	return {};
}

std::string LaunchBoxCatalog::identity(const GameBoxart& item)
{
	// Path and filename deliberately do not participate in a disc identity.
	const bool hasSerial = !item.uniqueId.empty() && item.uniqueId != "T0000" && item.uniqueId != "T0000M";
	const std::string id = hasSerial ? "serial:" + item.uniqueId : "title:" + normalizeTitle(item.searchName);
	if (!hasSerial && normalizeTitle(item.searchName).empty()) return {};
	return std::string(item.arcade ? "arcade:" : "disc:") + id + ":" + std::to_string(item.region);
}

bool LaunchBox::initialize(const std::string& saveDirectory)
{
	Scraper::initialize(saveDirectory);
	directory = saveDirectory;
	if (!file_exists(directory) && !make_directory(directory)) return false;
	http::init();
	try {
		const json saved = readJson(directory + "launchbox-matches.json");
		if (saved.is_object() && saved.value("version", 0) == 1 && saved.contains("matches") && saved["matches"].is_object())
			matches = saved["matches"];
	} catch (const json::exception& e) {
		WARN_LOG(COMMON, "Ignoring invalid LaunchBox match cache: %s", e.what());
	}
	return true;
}

void LaunchBox::loadCatalog()
{
	if (catalogLoaded) return;
	const std::string path = directory + "launchbox-catalog.json";
	json saved = readJson(path);
	try {
		catalog.loadJson(saved);
		catalogLoaded = true;
		if (std::time(nullptr) - saved.value("downloaded_at", std::time_t(0)) < 7 * 24 * 60 * 60)
			return;
	} catch (const std::exception&) { }
	try {
		std::vector<u8> data;
		const int status = http::get(metadataUrl, data);
		if (!http::success(status)) throw std::runtime_error("LaunchBox catalog HTTP " + std::to_string(status));
		ZipArchive archive;
		if (!archive.Open(data.data(), data.size())) throw std::runtime_error("Invalid LaunchBox ZIP");
		std::unique_ptr<ArchiveFile> xml(archive.OpenFile("Metadata.xml"));
		if (!xml) throw std::runtime_error("LaunchBox ZIP has no Metadata.xml");
		catalog.read([&](char *buffer, size_t size) { return xml->Read(buffer, static_cast<u32>(size)); });
		catalogLoaded = true;
		json index = catalog.toJson();
		index["downloaded_at"] = std::time(nullptr);
		writeJson(path, index);
	} catch (const std::exception& e) {
		if (!catalogLoaded) throw;
		WARN_LOG(COMMON, "Using cached LaunchBox catalog: %s", e.what());
	}
}

void LaunchBox::saveMatches()
{
	writeJson(directory + "launchbox-matches.json", {{"version", 1}, {"matches", matches}});
}

void LaunchBox::scrape(GameBoxart& item)
{
	if (item.scraped) return;
	if (std::chrono::steady_clock::now() < retryAfter) throw std::runtime_error("");
	try {
		const std::string key = LaunchBoxCatalog::identity(item);
		LaunchBoxCatalog::Match match;
		std::string imagePath;
		const auto cached = matches.find(key);
		if (cached != matches.end() && cached->is_object())
		{
			match = {cached->value("id", 0), cached->value("name", ""), cached->value("url", "")};
			imagePath = cached->value("path", "");
			if (match.url.find(imageBaseUrl) != 0 || !validImageFile(match.url.substr(std::char_traits<char>::length(imageBaseUrl))))
				match = {};
		}
		if (match.id == 0)
		{
			loadCatalog();
			match = catalog.match(item);
			imagePath.clear();
		}
		if (match.id != 0)
		{
			if (imagePath.empty() || !file_exists(imagePath))
			{
				imagePath = makeUniqueFilename(match.url);
				if (!downloadImage(match.url, imagePath)) throw std::runtime_error("LaunchBox artwork download failed");
			}
			item.name = match.name;
			item.launchboxId = match.id;
			item.setBoxartPath(imagePath);
			item.boxartUrl = match.url;
			if (!key.empty())
			{
				matches[key] = {{"id", match.id}, {"name", match.name}, {"url", match.url}, {"path", imagePath}};
				saveMatches();
			}
		}
		item.scraped = true;
	} catch (const std::exception& e) {
		retryAfter = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		throw std::runtime_error(e.what());
	}
}
