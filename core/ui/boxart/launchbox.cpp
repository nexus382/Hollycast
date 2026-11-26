// Portions Copyright 2026 The Hollycast Authors
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
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

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

// The position is the shared persisted artwork bit, independent of provider.
std::pair<int, int> imageCategory(const std::string& type)
{
 if (type == "Box - Front") return {0, 30};
 if (type == "Box - Front - Reconstructed") return {0, 20};
 if (type == "Advertisement Flyer - Front") return {0, 10};
 if (type == "Clear Logo") return {2, 30};
 if (type == "Arcade - Marquee") return {2, 20};
 if (type == "Banner") return {3, 30};
 if (type == "Disc") return {4, 30};
 if (type == "Cart - Front") return {4, 20};
 if (type == "Fanart - Background") return {5, 30};
 if (type == "Screenshot - Gameplay") return {6, 30};
 if (type == "Screenshot - Game Title") return {7, 30};
 if (type == "Box - 3D") return {8, 30};
 if (type == "Cart - 3D") return {8, 20};
 if (type == "Box - Back") return {9, 30};
 if (type == "Box - Back - Reconstructed") return {9, 20};
 if (type == "Advertisement Flyer - Back") return {9, 10};
 if (type == "Box - Spine") return {10, 30};
 return {-1, 0};
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

json mergeMatchEntry(const json& previous, const json& update)
{
	json merged = previous.is_object() ? previous : json::object();
	if (!update.is_object()) return merged;
	for (auto field = update.begin(); field != update.end(); ++field)
		if (field.key() != "media") merged[field.key()] = field.value();
	json media = merged.value("media", json::object());
	if (!media.is_object()) media = json::object();
	const json incomingMedia = update.value("media", json::object());
	if (incomingMedia.is_object())
	{
		for (auto image = incomingMedia.begin(); image != incomingMedia.end(); ++image)
		{
			json imageData = image.value().is_object() ? image.value() : json::object();
			const auto existing = media.find(image.key());
			if (existing != media.end() && existing->is_object())
			{
				const std::string oldUrl = existing->value("url", "");
				const std::string newUrl = imageData.value("url", "");
				if (newUrl.empty()) imageData["url"] = oldUrl;
				if (imageData.value("path", "").empty()
						&& (newUrl.empty() || newUrl == oldUrl))
					imageData["path"] = existing->value("path", "");
			}
			media[image.key()] = std::move(imageData);
		}
	}
	merged["media"] = std::move(media);
	return merged;
}

void writeAtomicText(const std::string& path, const std::string& text)
{
	const std::string temporary = path + ".tmp";
	FILE *file = nowide::fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		throw std::runtime_error("Cannot open LaunchBox match cache for writing: " + path);
	const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
	const bool flushed = fflush(file) == 0;
	const bool closed = fclose(file) == 0;
	if (!written || !flushed || !closed || flycast::rename(temporary.c_str(), path.c_str()) != 0)
	{
		const int error = errno;
		nowide::remove(temporary.c_str());
		throw std::runtime_error("Cannot publish LaunchBox match cache: " + path + " (" + std::to_string(error) + ")");
	}
}

void truncateUpdateLog(const std::string& path)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) throw std::runtime_error("Cannot truncate LaunchBox match update log: " + path);
	file.flush();
	if (!file) throw std::runtime_error("Cannot flush LaunchBox match update log: " + path);
	file.close();
	if (!file) throw std::runtime_error("Cannot close LaunchBox match update log: " + path);
}

void removeIncompleteUpdateTail(const std::string& path)
{
	std::error_code typeError;
	if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), typeError) || typeError) return;
	std::ifstream file(path, std::ios::binary);
	if (!file) return;
	file.seekg(0, std::ios::end);
	const std::streamoff size = file.tellg();
	if (size <= 0) return;
	file.seekg(-1, std::ios::end);
	char last = 0;
	file.get(last);
	if (last == '\n') return;
	file.clear();
	file.seekg(0, std::ios::beg);
	const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const size_t lastLine = contents.rfind('\n');
	std::error_code error;
	std::filesystem::resize_file(std::filesystem::u8path(path),
		lastLine == std::string::npos ? 0 : lastLine + 1, error);
	if (error) throw std::runtime_error("Cannot repair LaunchBox match update log: " + path);
}
} // namespace

struct LaunchBox::MatchCacheState
{
	std::mutex mutex;
	json matches = json::object();
	uint64_t sequence = 0;
	uint64_t snapshotSequence = 0;
	size_t bulkOwners = 0;
	std::unordered_set<std::string> pendingKeys;
	bool loaded = false;
};

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
     for (const auto& entry : {std::pair{"release_date", "ReleaseDate"}, {"overview", "Overview"},
       {"developer", "Developer"}, {"publisher", "Publisher"}, {"genre", "Genres"}, {"players", "MaxPlayers"}})
      game.metadata[entry.first] = field(record, entry.second);
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
						Image image{field(record, "FileName"), field(record, "Region"), field(record, "Type")};
      if (imageCategory(image.type).first >= 0 && validImageFile(image.file))
       game->second.images.push_back(std::move(image));
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
		json images = json::array();
		for (const Image& image : game.images)
   images.push_back({{"file", image.file}, {"region", image.region}, {"type", image.type}});
		records.push_back({{"id", id}, {"name", game.name}, {"platform", game.platform}, {"aliases", game.aliases}, {"images", images}, {"metadata", game.metadata}});
	}
	return {{"version", 2}, {"games", records}};
}

void LaunchBoxCatalog::loadJson(const json& data)
{
	if (data.at("version") != 1 && data.at("version") != 2)
		throw std::runtime_error("Unsupported LaunchBox catalog version");
	LaunchBoxCatalog parsed;
	for (const auto& record : data.at("games"))
	{
		Game game;
		game.id = record.at("id");
		game.name = record.at("name");
		game.platform = record.at("platform");
		game.aliases = record.at("aliases").get<std::vector<std::string>>();
		game.metadata = record.value("metadata", json::object());
  for (const auto& image : record.at(data.at("version") == 1 ? "covers" : "images"))
		{
			Image candidate{image.at("file"), image.at("region"), image.at("type")};
   if (imageCategory(candidate.type).first >= 0 && validImageFile(candidate.file)) game.images.push_back(std::move(candidate));
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
  Match result;
  result.id = game->id;
  result.name = game->name;
  result.metadata = game->metadata;
  for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
  {
   const Image *best = nullptr;
   int bestScore = -1;
   for (const Image& image : game->images)
   {
    const auto category = imageCategory(image.type);
    if (category.first != static_cast<int>(i)) continue;
    const int score = 100 * regionScore(image.region, item.region) + category.second;
    if (score > bestScore || (score == bestScore && best && image.file < best->file))
    {
     best = &image;
     bestScore = score;
    }
   }
   if (best) result.media[artwork::mediaTypes[i].name] = imageBaseUrl + best->file;
  }
  const auto cover = result.media.find("cover");
  if (cover != result.media.end()) result.url = cover->second;
  return result;
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

std::shared_ptr<LaunchBox::MatchCacheState> LaunchBox::sharedCacheState(const std::string& saveDirectory)
{
	static std::mutex registryMutex;
	static std::unordered_map<std::string, std::weak_ptr<MatchCacheState>> registry;
	std::error_code error;
	const auto absolute = std::filesystem::absolute(std::filesystem::u8path(saveDirectory), error);
	const std::string key = (error ? std::filesystem::u8path(saveDirectory) : absolute).lexically_normal().string();
	std::lock_guard<std::mutex> lock(registryMutex);
	auto& weak = registry[key];
	std::shared_ptr<MatchCacheState> state = weak.lock();
	if (!state)
	{
		state = std::make_shared<MatchCacheState>();
		weak = state;
	}
	return state;
}

bool LaunchBox::initialize(const std::string& saveDirectory)
{
	Scraper::initialize(saveDirectory);
	directory = saveDirectory;
 if (!directory.empty() && directory.back() != '/') directory += '/';
 imageDirectory = directory + "launchbox/";
 std::error_code error;
 std::filesystem::create_directories(std::filesystem::u8path(imageDirectory), error);
 if (error) return false;
	matchCache = sharedCacheState(directory);
	{
		std::lock_guard<std::mutex> lock(matchCache->mutex);
		if (!matchCache->loaded)
		{
			uint64_t snapshotSequence = 0;
			try
			{
				const json saved = readJson(directory + "launchbox-matches.json");
				if (saved.is_object() && (saved.value("version", 0) == 1 || saved.value("version", 0) == 2)
						&& saved.contains("matches") && saved["matches"].is_object())
				{
					matchCache->matches = saved["matches"];
					snapshotSequence = saved.value("sequence", uint64_t(0));
					matchCache->sequence = snapshotSequence;
					matchCache->snapshotSequence = snapshotSequence;
				}
			}
			catch (const json::exception& e)
			{
				WARN_LOG(COMMON, "Ignoring invalid LaunchBox match cache: %s", e.what());
			}
			removeIncompleteUpdateTail(directory + "launchbox-matches.json.updates");
			std::ifstream updatesFile(directory + "launchbox-matches.json.updates", std::ios::binary);
			if (updatesFile)
			{
				const std::string updates((std::istreambuf_iterator<char>(updatesFile)), std::istreambuf_iterator<char>());
				for (size_t start = 0; start < updates.size();)
				{
					const size_t end = updates.find('\n', start);
					if (end == std::string::npos) break;
					const json record = json::parse(updates.substr(start, end - start), nullptr, false);
					start = end + 1;
					if (!record.is_object() || !record.contains("sequence") || !record.contains("key")
							|| !record.contains("match") || !record["key"].is_string() || !record["match"].is_object())
						continue;
					if (!record["sequence"].is_number_unsigned()
							&& (!record["sequence"].is_number_integer() || record["sequence"].get<int64_t>() < 0))
						continue;
					const uint64_t sequence = record["sequence"].get<uint64_t>();
					matchCache->sequence = std::max(matchCache->sequence, sequence);
					if (sequence <= snapshotSequence) continue;
					const std::string key = record["key"].get<std::string>();
					auto existing = matchCache->matches.find(key);
					matchCache->matches[key] = mergeMatchEntry(existing == matchCache->matches.end() ? json::object() : *existing,
						record["match"]);
				}
			}
			matchCache->loaded = true;
		}
	}
	http::init();
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
		if (saved.value("version", 0) >= 2 && std::time(nullptr) - saved.value("downloaded_at", std::time_t(0)) < 7 * 24 * 60 * 60)
			return;
	} catch (const std::exception&) { }
	try {
		std::vector<u8> data;
		std::string contentType;
  const int status = transport ? transport(metadataUrl, data, contentType) : http::get(metadataUrl, data, contentType);
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
		if (!catalogLoaded || saved.value("version", 0) < 2) throw;
		WARN_LOG(COMMON, "Using cached LaunchBox catalog: %s", e.what());
	}
}

void LaunchBox::saveMatches(const std::string& key, const json& match)
{
	const auto started = std::chrono::steady_clock::now();
	if (!matchCache) throw std::runtime_error("LaunchBox match cache is not initialized");
	std::lock_guard<std::mutex> lock(matchCache->mutex);
	auto existing = matchCache->matches.find(key);
	const json updated = mergeMatchEntry(existing == matchCache->matches.end() ? json::object() : *existing, match);
	matchCache->matches[key] = updated;
	matchCache->pendingKeys.insert(key);
	if (bulkActive)
	{
		const std::string path = directory + "launchbox-matches.json.updates";
		removeIncompleteUpdateTail(path);
		const uint64_t sequence = ++matchCache->sequence;
		const std::string serialized = json{{"sequence", sequence}, {"key", key}, {"match", updated}}.dump() + "\n";
		std::ofstream file(path, std::ios::binary | std::ios::app);
		if (!file) throw std::runtime_error("Cannot open LaunchBox match update log: " + path);
		file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
		file.flush();
		if (!file) throw std::runtime_error("Cannot write LaunchBox match update log: " + path);
		file.close();
		if (!file) throw std::runtime_error("Cannot close LaunchBox match update log: " + path);
		metrics.recordsSerialized += 1;
		metrics.bytesWritten += serialized.size();
		++metrics.physicalWrites;
	}
	else
	{
		const std::string serialized = json{{"version", 2}, {"sequence", matchCache->sequence},
			{"matches", matchCache->matches}}.dump();
		writeAtomicText(directory + "launchbox-matches.json", serialized);
		matchCache->snapshotSequence = matchCache->sequence;
		metrics.recordsSerialized += matchCache->matches.size();
		metrics.bytesWritten += serialized.size();
		++metrics.physicalWrites;
		truncateUpdateLog(directory + "launchbox-matches.json.updates");
		++metrics.physicalWrites;
		matchCache->pendingKeys.clear();
	}
	matchCache->pendingKeys.erase(key);
	metrics.elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
}

void LaunchBox::beginBulk()
{
	if (bulkActive) return;
	if (!matchCache) throw std::runtime_error("LaunchBox match cache is not initialized");
	std::lock_guard<std::mutex> lock(matchCache->mutex);
	bulkActive = true;
	++matchCache->bulkOwners;
}

void LaunchBox::endBulk()
{
	if (!bulkActive || !matchCache) return;
	std::lock_guard<std::mutex> lock(matchCache->mutex);
	const bool lastOwner = matchCache->bulkOwners <= 1;
	if (lastOwner
			&& (matchCache->sequence > matchCache->snapshotSequence
				|| std::filesystem::exists(directory + "launchbox-matches.json.updates")))
	{
		const auto started = std::chrono::steady_clock::now();
		const std::string serialized = json{{"version", 2}, {"sequence", matchCache->sequence},
			{"matches", matchCache->matches}}.dump();
		writeAtomicText(directory + "launchbox-matches.json", serialized);
		matchCache->snapshotSequence = matchCache->sequence;
		metrics.recordsSerialized += matchCache->matches.size();
		metrics.bytesWritten += serialized.size();
		++metrics.physicalWrites;
		truncateUpdateLog(directory + "launchbox-matches.json.updates");
		++metrics.physicalWrites;
		matchCache->pendingKeys.clear();
		metrics.elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
	}
	bulkActive = false;
	if (matchCache->bulkOwners) --matchCache->bulkOwners;
}

LaunchBox::~LaunchBox()
{
	try { endBulk(); }
	catch (const std::exception& error) { WARN_LOG(COMMON, "Cannot compact LaunchBox match cache: %s", error.what()); }
}

bool LaunchBox::ready() const
{
 return std::chrono::steady_clock::now() >= retryAfter;
}

bool LaunchBox::needsMedia(const GameBoxart& item, u32 selection)
{
 if ((selection & artwork::metadataMedia) && !item.launchboxSearched) return true;
 return artwork::needsImages(selection & supportedMedia, item.launchboxCheckedMedia, item.launchboxMedia);
}

void LaunchBox::scrape(GameBoxart& item)
{
 const u32 selection = (item.requestedMedia ? item.requestedMedia : artwork::coverMedia) & supportedMedia;
 const std::string identity = LaunchBoxCatalog::identity(item);
 bool persistencePending = false;
 if (matchCache && !identity.empty())
 {
  std::lock_guard<std::mutex> lock(matchCache->mutex);
  persistencePending = matchCache->pendingKeys.count(identity) != 0;
 }
 if (!persistencePending && !needsMedia(item, item.requestedMedia ? item.requestedMedia : artwork::coverMedia)) return;
 if (!ready()) throw std::runtime_error("");
 if (persistencePending)
 {
  json pendingMatch;
  {
   std::lock_guard<std::mutex> lock(matchCache->mutex);
   const auto found = matchCache->matches.find(identity);
   if (found != matchCache->matches.end()) pendingMatch = found.value();
  }
  if (!pendingMatch.is_discarded() && !pendingMatch.is_null())
  {
   saveMatches(identity, pendingMatch);
   if (!needsMedia(item, item.requestedMedia ? item.requestedMedia : artwork::coverMedia)) return;
  }
 }
 // Keep category progress current in memory, but serialize once per item.
 bool matchesChanged = false;
 std::string cacheKey;
 json changedMatch;
 try {
  cacheKey = LaunchBoxCatalog::identity(item);
  LaunchBoxCatalog::Match match;
  json cachedImages = json::object();
  json cachedEntry;
  {
   if (!matchCache) throw std::runtime_error("LaunchBox match cache is not initialized");
   std::lock_guard<std::mutex> lock(matchCache->mutex);
   const auto cached = matchCache->matches.find(cacheKey);
   if (cached != matchCache->matches.end()) cachedEntry = cached.value();
  }
  bool expanded = false;
  if (cachedEntry.is_object())
  {
   match.id = cachedEntry.value("id", 0);
   match.name = cachedEntry.value("name", "");
   match.metadata = cachedEntry.value("metadata", json::object());
   expanded = cachedEntry.value("expanded", false);
   cachedImages = cachedEntry.value("media", json::object());
   // Upgrade the PR's original cover-only match cache without a new download.
   if (!cachedEntry.value("url", "").empty())
    cachedImages["cover"] = {{"url", cachedEntry.value("url", "")}, {"path", cachedEntry.value("path", "")}};
   for (const auto& image : cachedImages.items())
   {
    const std::string url = image.value().value("url", "");
    if (url.find(imageBaseUrl) == 0 && validImageFile(url.substr(std::char_traits<char>::length(imageBaseUrl))))
     match.media[image.key()] = url;
   }
  }
  if (!match.id || (!expanded && (selection & ~artwork::coverMedia)))
  {
   loadCatalog();
   match = catalog.match(item);
   expanded = true;
  }
  if (match.id)
  {
   item.launchboxId = match.id;
   if (!match.name.empty()) item.name = match.name;
   for (const auto& field : {std::pair{"release_date", &item.releaseDate}, {"overview", &item.overview},
     {"developer", &item.developer}, {"publisher", &item.publisher}, {"genre", &item.genre}, {"players", &item.players}})
   {
    const auto value = match.metadata.find(field.first);
    if (value != match.metadata.end() && value->is_string() && !value->get_ref<const std::string&>().empty())
     *field.second = value->get<std::string>();
   }
   for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
   {
    const u32 bit = 1u << i;
    if (!(selection & bit)) continue;
    const std::string kind = artwork::mediaTypes[i].name;
    const auto existing = item.launchboxMedia.find(kind);
    if ((item.launchboxCheckedMedia & bit) && (existing == item.launchboxMedia.end() || file_exists(existing->second))) continue;
    const auto remote = match.media.find(kind);
    if (remote != match.media.end())
    {
     std::string path;
     const auto prior = cachedImages.find(kind);
     if (prior != cachedImages.end() && prior->value("url", "") == remote->second)
      path = prior->value("path", "");
     if (path.empty() || !file_exists(path))
     {
      path = imageDirectory + std::to_string(match.id) + "-" + kind + "." + get_file_extension(remote->second);
      if (!file_exists(path) && !downloadMediaImage(remote->second, path, transport)) path.clear();
     }
     if (!path.empty())
     {
      item.launchboxMedia[kind] = path;
      cachedImages[kind] = {{"url", remote->second}, {"path", path}};
      if (i == 0) { item.boxartPath = path; item.boxartUrl.clear(); }
     }
    }
    const auto remaining = item.launchboxMedia.find(kind);
    if (remaining != item.launchboxMedia.end() && !file_exists(remaining->second)) item.launchboxMedia.erase(remaining);
    item.launchboxCheckedMedia |= bit;
    // Keep partial paths and catalogue URLs for the item-level flush.
   if (!cacheKey.empty())
    {
     for (const auto& remote : match.media)
      cachedImages[remote.first]["url"] = remote.second;
     changedMatch = {{"id", match.id}, {"name", match.name}, {"metadata", match.metadata}, {"expanded", expanded}, {"media", cachedImages}};
     matchesChanged = true;
    }
   }
  }
  else item.launchboxCheckedMedia |= supportedMedia;
  item.launchboxSearched = true;
  item.scraped = true;
 } catch (const std::exception&) {
  retryAfter = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  if (matchesChanged)
  {
   try { saveMatches(cacheKey, changedMatch); }
   catch (const std::exception& error) { WARN_LOG(COMMON, "Cannot save partial LaunchBox match cache: %s", error.what()); throw; }
  }
  throw;
 }
 if (matchesChanged)
 {
  try { saveMatches(cacheKey, changedMatch); }
  catch (const std::exception&)
  {
   retryAfter = std::chrono::steady_clock::now() + std::chrono::minutes(1);
   throw;
  }
 }
}
