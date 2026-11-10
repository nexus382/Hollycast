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

#include "gamesdb.h"
#include "oslib/http_client.h"
#include "stdclass.h"
#include "emulator.h"
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#define APIKEY "3fcc5e726a129924972be97abfd577ac5311f8f12398a9d9bcb5a377d4656fa8"

namespace {
std::string text(const json& object, const char* field)
{
    auto value = object.find(field);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}
std::string normalized(const std::string& title)
{
    std::string result;
    for (unsigned char c : title)
        if (std::isalnum(c) || c >= 128) result += static_cast<char>(std::tolower(c));
    return result;
}
std::string plainText(std::string value)
{
    for (const auto& replacement : {std::pair{"\\r\\n", "\n"}, std::pair{"\\n", "\n"}, std::pair{"\\'", "'"}})
        for (size_t pos = 0; (pos = value.find(replacement.first, pos)) != std::string::npos; pos += std::strlen(replacement.second))
            value.replace(pos, std::strlen(replacement.first), replacement.second);
    return value;
}
const char* imageTypes[] = {"boxart", "", "clearlogo", "banner", "", "fanart", "screenshot", "titlescreen", "", "boxart", ""};
}

TheGamesDb::TheGamesDb(Transport transport) : transport(std::move(transport)) {}

std::string TheGamesDb::makeUrl(const std::string& endpoint)
{
    return "https://api.thegamesdb.net/v1/" + endpoint + "?apikey=" APIKEY;
}

bool TheGamesDb::ready() const { return getTimeMs() >= blackoutPeriod; }

bool TheGamesDb::needsMedia(const GameBoxart& item, u32 selection)
{
    if (item.arcade) return !item.scraped;
    if ((selection & artwork::metadataMedia) && !item.tgdbSearched) return true;
    selection &= supportedMedia;
    for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
    {
        if (!(selection & (1u << i))) continue;
        const auto cached = item.tgdbMedia.find(artwork::mediaTypes[i].name);
        if (!(item.tgdbCheckedMedia & (1u << i))
                || (cached != item.tgdbMedia.end() && !file_exists(cached->second))) return true;
    }
    return false;
}

bool TheGamesDb::initialize(const std::string& saveDirectory)
{
    if (!Scraper::initialize(saveDirectory)) return false;
    http::init();
    directory = saveDirectory;
    if (!directory.empty() && directory.back() != '/') directory += '/';
    directory += "thegamesdb/";
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(directory), error);
    if (error) return false;
    try {
        std::ifstream file(std::filesystem::u8path(directory + "references.json"));
        if (file) file >> references;
        if (!references.is_object()) references = json::object();
    } catch (...) { references = json::object(); }
    return true;
}

json TheGamesDb::httpGet(const std::string& url)
{
    if (!ready()) throw std::runtime_error("");
    std::vector<u8> bytes;
    std::string type;
    const int status = transport ? transport(url, bytes, type) : http::get(url, bytes, type);
    if (!http::success(status) || bytes.empty())
    {
        blackoutPeriod = getTimeMs() + 60000;
        throw std::runtime_error("TheGamesDB request failed (" + std::to_string(status) + "). Retrying after one minute.");
    }
    json response;
    // Current libc++ only provides char_traits for text character types.
    const char* jsonText = reinterpret_cast<const char*>(bytes.data());
    try { response = json::parse(jsonText, jsonText + bytes.size()); }
    catch (...) {
        blackoutPeriod = getTimeMs() + 60000;
        throw std::runtime_error("TheGamesDB returned an invalid response.");
    }
    if (!response.contains("code") || !response["code"].is_number_integer() || !http::success(response["code"].get<int>()))
    {
        blackoutPeriod = getTimeMs() + 60000;
        throw std::runtime_error("TheGamesDB API request failed. Retrying after one minute.");
    }
    return response;
}

void TheGamesDb::fetchPlatforms()
{
    if (dreamcastPlatformId && arcadePlatformId) return;
    for (const auto& platform : {std::pair{"Dreamcast", "Sega Dreamcast"}, std::pair{"Arcade", "Arcade"}})
    {
        const auto response = httpGet(makeUrl("Platforms/ByPlatformName") + "&name=" + platform.first);
        for (const auto& entry : response.at("data").at("platforms"))
            if (text(entry, "name") == platform.second)
            {
                if (std::string(platform.first) == "Dreamcast") dreamcastPlatformId = entry.at("id").get<int>();
                else arcadePlatformId = entry.at("id").get<int>();
            }
    }
    if (!dreamcastPlatformId || !arcadePlatformId) throw std::runtime_error("TheGamesDB platform lookup failed.");
}

json TheGamesDb::findGame(const GameBoxart& item)
{
    const std::string fields = "&fields=overview,players,publishers,genres,uids,alternates";
    const auto games = [](const json& response) { return response.at("data").at("games"); };
    if (item.tgdbId)
    {
        const auto matches = games(httpGet(makeUrl("Games/ByGameID") + "&id=" + std::to_string(item.tgdbId) + fields));
        for (const auto& game : matches)
            if (game.at("id").get<u32>() == item.tgdbId) return game;
        return {};
    }
    fetchPlatforms();
    const int platform = item.arcade ? arcadePlatformId : dreamcastPlatformId;
    const std::string filter = "&filter%5Bplatform%5D=" + std::to_string(platform);
    if (!item.uniqueId.empty() && item.uniqueId != "T0000" && item.uniqueId != "T0000M")
    {
        const auto matches = games(httpGet(makeUrl("Games/ByGameUniqueID") + fields + filter + "&uid=" + http::urlEncode(item.uniqueId)));
        json match;
        for (const auto& game : matches)
            if (game.value("platform", platform) == platform && game.contains("uids"))
                for (const auto& uid : game.at("uids"))
                    if (text(uid, "uid") == item.uniqueId)
                    {
                        if (!match.is_null() && match.at("id") != game.at("id")) return {};
                        match = game;
                    }
        if (!match.is_null()) return match;
    }
    std::set<std::string> searched;
    for (auto title : {item.searchName, item.name, get_file_basename(item.fileName)})
    {
        if (title.empty() || !searched.insert(title).second) continue;
        const size_t suffix = title.find(" (");
        if (suffix != std::string::npos) title.resize(suffix);
        const auto matches = games(httpGet(makeUrl("Games/ByGameName") + fields + filter + "&name=" + http::urlEncode(title)));
        json match;
        for (const auto& game : matches)
        {
            if (game.value("platform", platform) != platform || normalized(text(game, "game_title")) != normalized(title)) continue;
            if (!match.is_null() && match.at("id") != game.at("id")) return {};
            match = game;
        }
        if (!match.is_null()) return match;
    }
    return {};
}

std::string TheGamesDb::referenceNames(const json& game, const std::string& category)
{
    const auto ids = game.find(category);
    if (ids == game.end() || !ids->is_array() || ids->empty()) return {};
    bool missing = !references.contains(category) || !references[category].is_object();
    for (const auto& id : *ids)
        if (id.is_number_integer() && (!references.contains(category) || !references[category].contains(std::to_string(id.get<int>())))) missing = true;
    if (missing)
    {
        std::string endpoint = category;
        endpoint.front() = static_cast<char>(std::toupper(endpoint.front()));
        const auto response = httpGet(makeUrl(endpoint));
        json names = json::object();
        for (const auto& entry : response.at("data").at(category))
            if (entry.contains("id") && entry["id"].is_number_integer() && !text(entry, "name").empty())
                names[std::to_string(entry["id"].get<int>())] = text(entry, "name");
        references[category] = std::move(names);
        std::ofstream file(std::filesystem::u8path(directory + "references.json"));
        if (file) file << references.dump();
    }
    std::string result;
    for (const auto& id : *ids)
    {
        if (!id.is_number_integer()) continue;
        const auto name = references[category].find(std::to_string(id.get<int>()));
        if (name == references[category].end() || !name->is_string()) continue;
        if (!result.empty()) result += " / ";
        result += name->get<std::string>();
    }
    return result;
}

void TheGamesDb::applyMetadata(GameBoxart& item, const json& game)
{
    const auto update = [](std::string& field, const std::string& value) { if (!value.empty()) field = value; };
    update(item.name, text(game, "game_title"));
    update(item.releaseDate, text(game, "release_date"));
    update(item.overview, plainText(text(game, "overview")));
    update(item.developer, referenceNames(game, "developers"));
    update(item.publisher, referenceNames(game, "publishers"));
    update(item.genre, referenceNames(game, "genres"));
    if (game.contains("players") && game["players"].is_number_integer() && game["players"].get<int>() > 0)
        item.players = std::to_string(game["players"].get<int>());
}

bool TheGamesDb::downloadMedia(const std::string& url, const std::string& path)
{
 try { return downloadMediaImage(url, path, transport); }
 catch (...) { blackoutPeriod = getTimeMs() + 60000; throw; }
}

void TheGamesDb::scrape(GameBoxart& item)
{
    const u32 selection = (item.requestedMedia ? item.requestedMedia : artwork::coverMedia) & supportedMedia;
    if (!needsMedia(item, item.requestedMedia ? item.requestedMedia : artwork::coverMedia)) return;
    const auto game = findGame(item);
    if (!game.is_object()) {
        item.tgdbSearched = true;
        item.tgdbCheckedMedia |= supportedMedia;
        item.scraped = true;
        return;
    }
    item.tgdbId = game.at("id").get<u32>();
    applyMetadata(item, game);
    if (!selection) { item.tgdbSearched = true; return; }
    json images = json::array();
    std::string baseUrl;
    for (int page = 1; page <= 32; ++page)
    {
        const auto response = httpGet(makeUrl("Games/Images") + "&games_id=" + std::to_string(item.tgdbId) + "&page=" + std::to_string(page));
        const auto& data = response.at("data");
        if (baseUrl.empty())
            for (const char* size : {"original", "large", "medium", "small", "thumb"})
            {
                baseUrl = text(data.at("base_url"), size);
                if (!baseUrl.empty()) break;
            }
        const auto entries = data.at("images").find(std::to_string(item.tgdbId));
        if (entries != data.at("images").end() && entries->is_array())
            for (const auto& image : *entries) images.push_back(image);
        if (!response.contains("pages") || text(response.at("pages"), "next").empty()) break;
        if (page == 32) throw std::runtime_error("TheGamesDB image pagination exceeded its limit.");
    }
    if (baseUrl.substr(0, 8) != "https://") throw std::runtime_error("TheGamesDB returned no secure image base URL.");
    for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
    {
        const u32 bit = 1u << i;
        if (!(selection & bit)) continue;
        const auto& kind = artwork::mediaTypes[i];
        const auto cached = item.tgdbMedia.find(kind.name);
        if ((item.tgdbCheckedMedia & bit) && (cached == item.tgdbMedia.end() || file_exists(cached->second))) continue;
        const json* chosen = nullptr;
        for (const auto& image : images)
        {
            if (text(image, "type") != imageTypes[i]) continue;
            const auto side = text(image, "side");
            if (i == 0 && !side.empty() && side != "front") continue;
            if (i == 9 && side != "back") continue;
            if (!text(image, "filename").empty()) { chosen = &image; break; }
        }
        if (chosen)
        {
            const auto filename = text(*chosen, "filename");
            if (filename.front() == '/' || filename.find("..") != std::string::npos || filename.find(":") != std::string::npos
                    || filename.find_first_of("?#\\") != std::string::npos) throw std::runtime_error("TheGamesDB returned an invalid image path.");
            const auto path = directory + std::to_string(item.tgdbId) + "-" + kind.name + "." + get_file_extension(filename);
            if (file_exists(path) || downloadMedia(baseUrl + filename, path))
            {
                item.tgdbMedia[kind.name] = path;
                if (i == 0) { item.boxartPath = path; item.boxartUrl.clear(); }
            }
        }
        // A deleted local file whose remote image disappeared is unavailable,
        // rather than a missing download that should be queued forever.
        const auto remaining = item.tgdbMedia.find(kind.name);
        if (remaining != item.tgdbMedia.end() && !file_exists(remaining->second))
            item.tgdbMedia.erase(remaining);
        item.tgdbCheckedMedia |= bit;
    }
    item.tgdbSearched = true;
    item.scraped = true;
}

void TheGamesDb::scrape(std::vector<GameBoxart>& items)
{
    for (auto& item : items) scrape(item);
}
