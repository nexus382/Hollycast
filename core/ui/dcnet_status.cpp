// Portions Copyright 2026 The Hollycast Authors
#include "dcnet_status.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dcnet
{
namespace
{

// Keep the supported server catalog visible even when a title is absent from the
// live feed. Only verified API IDs (or an exact title match) receive live counts.
const Game catalog[] = {
	{ "catalog:4x4-evolution", "4x4 Evolution" },
	{ "aerof", "Aero Dancing F - Todoroki Tsubasa no Hatsu Hikou" },
	{ "aeroi", "Aero Dancing i" },
	{ "catalog:aero-dancing-i-jikai", "Aero Dancing i Jikai Saku Made Matemasen" },
	{ "afo", "Alien Front Online" },
	{ "chuchu", "ChuChu Rocket" },
	{ "virtualon", "Cyber Troopers Virtual-On: Oratorio Tangram" },
	{ "daytona", "Daytona USA" },
	{ "deedee", "Dee Dee Planet" },
	{ "golfshiyou2", "Golf Shiyou Yo 2" },
	{ "hundredswords", "Hundred Swords" },
	{ "igp", "Internet Game Pack" },
	{ "monaco2", "Monaco Racing Simulation 2" },
	{ "nba2k1", "NBA 2K1" },
	{ "nba2k2", "NBA 2K2" },
	{ "ncaa2k2", "NCAA 2K2" },
	{ "nfl2k1", "NFL 2K1" },
	{ "nfl2k2", "NFL 2K2" },
	{ "oogabooga", "Ooga Booga" },
	{ "outtrigger", "Outtrigger" },
	{ "catalog:pba-bowling-2001", "PBA Bowling 2001" },
	{ "planetring", "Planet Ring" },
	{ "pod", "POD: Speedzone" },
	{ "powersmash", "Power Smash" },
	{ "propeller", "Propeller Arena" },
	{ "runejade", "Rune Jade" },
	{ "segatetris", "Sega Tetris" },
	{ "sdo", "Speed Devils Online" },
	{ "starlancer", "StarLancer" },
	{ "nexttetris", "The Next Tetris" },
	{ "toyracer", "Toy Racer" },
	{ "wsb2k2", "World Series Baseball 2K2" },
	{ "wwp", "Worms World Party" },
	{ "catalog:sega-rally-2", "Sega Rally 2", ReleaseStage::Beta },
	{ "bomberman", "Bomberman Online", ReleaseStage::Alpha },
	{ "culdcept", "Culdcept II", ReleaseStage::Alpha },
	{ "mvsc2", "Marvel vs. Capcom 2 (JP)", ReleaseStage::Alpha },
	{ "pstone2", "Power Stone 2 (JP)", ReleaseStage::Alpha },
	{ "sf3strike3", "Street Fighter III 3rd Strike (JP)", ReleaseStage::Alpha },
	{ "sfzero3", "Street Fighter Zero 3 for Matching Service", ReleaseStage::Alpha },
	{ "spawn", "Spawn (JP)", ReleaseStage::Alpha },
	{ "vampire", "Vampire Chronicle for Matching Service", ReleaseStage::Alpha },
	{ "nettennis", "Netto De Tennis", ReleaseStage::Alpha },
	{ "catalog:capcom-vs-snk-pro", "Capcom vs. SNK Pro (JP)", ReleaseStage::Alpha },
	{ "jojo", "Jojo's Bizarre Adventure for Matching Service", ReleaseStage::Alpha },
	{ "pjustice", "Project Justice (JP)", ReleaseStage::Alpha },
	{ "ssf2", "Super Street Fighter IIX for Matching Service", ReleaseStage::Alpha },
	{ "techromancer", "Tech Romancer for Matching Service", ReleaseStage::Alpha },
	{ "taisenng", "Taisen Net Gimmick", ReleaseStage::Alpha },
	{ "hmgeo", "Heavy Metal: Geomatrix (JP)", ReleaseStage::Alpha },
	{ "puzzle2", "Super Puzzle Fighter IIX for Matching Service", ReleaseStage::Alpha },
};

std::string readText(const nlohmann::json& item, const char *key)
{
	const auto it = item.find(key);
	return it == item.end() || it->is_null() ? std::string() : it->get<std::string>();
}

int readCount(const nlohmann::json& item, const char *key)
{
	const auto it = item.find(key);
	if (it == item.end() || it->is_null())
		return -1;
	if (!it->is_number_integer() || *it < 0 || *it > std::numeric_limits<int>::max())
		throw std::runtime_error("Invalid DCnet player or match count");
	return it->get<int>();
}

}

std::string normalizeTitle(const std::string& title)
{
	std::string value = title;
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const auto dot = value.find_last_of('.');
	if (dot != std::string::npos)
	{
		const std::string extension = value.substr(dot);
		for (const char *supported : { ".chd", ".cdi", ".gdi", ".cue", ".iso", ".zip", ".7z", ".bin", ".m3u" })
			if (extension == supported)
			{
				value.resize(dot);
				break;
			}
	}
	std::string key;
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] == '(' || value[i] == '[')
		{
			const auto end = value.find(value[i] == '(' ? ')' : ']', i + 1);
			if (end != std::string::npos)
			{
				i = end;
				continue;
			}
		}
		if ((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= '0' && value[i] <= '9'))
			key += value[i];
	}
	return key;
}

std::vector<std::string> titleKeys(const Game& game)
{
	std::vector<std::string> keys;
	auto add = [&](const std::string& title) {
		const std::string key = normalizeTitle(title);
		if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
			keys.push_back(key);
	};
	add(game.name);
	add(game.reportedName);
	// Explicit alternate release names avoid substring matches between sequels
	// or between an offline release and its distinct online edition.
	const std::pair<const char *, const char *> aliases[] = {
		{ "catalog:4x4-evolution", "4x4 Evo" },
		{ "capvsnkmf", "Capcom vs. SNK" },
		{ "sdo", "Speed Devils - Online Racing" },
		{ "aerof", "Aero Dancing F" },
		{ "virtualon", "Virtual-On - Oratorio Tangram" },
		{ "catalog:pba-bowling-2001", "PBA Tour Bowling 2001" },
		{ "catalog:capcom-vs-snk-pro", "Capcom vs. SNK - Millennium Fight 2000 Pro" },
		{ "nettennis", "Net de Tennis" },
		{ "mvsc2", "Marvel vs. Capcom 2 - New Age of Heroes" },
		{ "sf3strike3", "Street Fighter III - 3rd Strike - Fight for the Future" },
		{ "sfzero3", "Street Fighter Zero 3 - Saikyou-ryuu Doujou for Matching Service" },
		{ "pjustice", "Moero! Justice Gakuen" },
	};
	for (const auto& alias : aliases)
		if (game.id == alias.first)
			add(alias.second);
	return keys;
}

Status parseStatus(const std::string& gamesJson, const std::string& playersJson)
{
	const auto games = nlohmann::json::parse(gamesJson);
	const auto players = nlohmann::json::parse(playersJson);
	if (!games.is_array() || !players.is_array())
		throw std::runtime_error("Invalid DCnet status response");

	Status status;
	status.games.assign(std::begin(catalog), std::end(catalog));
	std::unordered_set<std::string> gameIds;
	for (const auto& item : games)
	{
		if (!item.is_object())
			throw std::runtime_error("Invalid DCnet game");
		Game game;
		game.id = readText(item, "gameId");
		game.name = readText(item, "name");
		if (game.id.empty() || game.name.empty() || !gameIds.insert(game.id).second)
			throw std::runtime_error("Invalid DCnet game identity");
		game.reportedName = game.name;
		game.thumbnailUrl = readText(item, "thumbnail");
		game.hasLiveStatus = true;
		game.online = item.at("online").get<bool>();
		game.players = readCount(item, "playerCount");
		game.matches = readCount(item, "gameCount");
		const auto known = std::find_if(status.games.begin(), status.games.end(), [&](const Game& entry) {
			if (entry.id == game.id)
				return true;
			if (entry.id.compare(0, 8, "catalog:") != 0)
				return false;
			const auto keys = titleKeys(entry);
			return std::find(keys.begin(), keys.end(), normalizeTitle(game.name)) != keys.end();
		});
		if (known == status.games.end())
			status.games.push_back(std::move(game));
		else
		{
			game.name = known->name;
			game.stage = known->stage;
			*known = std::move(game);
		}
	}
	for (const auto& item : players)
	{
		if (!item.is_object())
			throw std::runtime_error("Invalid DCnet player");
		Player player;
		player.name = readText(item, "name");
		if (player.name.empty())
			player.name = readText(item, "loginName");
		if (player.name.empty())
			player.name = "Unknown player";
		player.gameId = readText(item, "gameId");
		player.accessPoint = readText(item, "accessPoint");
		player.gameName = readText(item, "gameName");
		if (!player.gameId.empty())
		{
			auto game = std::find_if(status.games.begin(), status.games.end(), [&](const Game& entry) {
				return entry.id == player.gameId;
			});
			if (game == status.games.end() && !player.gameName.empty())
			{
				const std::string key = normalizeTitle(player.gameName);
				auto candidate = status.games.end();
				for (auto it = status.games.begin(); it != status.games.end(); ++it)
				{
					if (it->hasLiveStatus || it->hasPlayerActivity)
						continue;
					const auto keys = titleKeys(*it);
					if (std::find(keys.begin(), keys.end(), key) == keys.end())
						continue;
					if (candidate != status.games.end())
					{
						candidate = status.games.end();
						break;
					}
					candidate = it;
				}
				if (candidate != status.games.end())
				{
					candidate->id = player.gameId;
					game = candidate;
				}
				else
				{
					Game additional;
					additional.id = player.gameId;
					additional.name = player.gameName;
					status.games.push_back(std::move(additional));
					game = std::prev(status.games.end());
				}
			}
			if (game != status.games.end())
			{
				game->hasPlayerActivity = true;
				if (game->reportedName.empty())
					game->reportedName = player.gameName;
				if (game->thumbnailUrl.empty())
					game->thumbnailUrl = readText(item, "thumbnail");
			}
		}
		status.players.push_back(std::move(player));
	}
	// The player feed can identify activity for titles omitted from /games, or
	// supply a positive player count when the game endpoint has no count field.
	std::unordered_map<std::string, int> observedPlayers;
	for (const auto& player : status.players)
		if (!player.gameId.empty())
		{
			int& count = observedPlayers[player.gameId];
			if (count < std::numeric_limits<int>::max())
				++count;
		}
	for (auto& game : status.games)
	{
		if (game.players < 0)
		{
			const auto it = observedPlayers.find(game.id);
			if (it != observedPlayers.end())
				game.players = it->second;
		}
		// Tally reported services before projecting their counts onto editions.
		status.playerCount += std::max(game.players, 0);
		status.matchCount += std::max(game.matches, 0);
		if (game.players > 0 || game.matches > 0)
			++status.activeGames;
	}
	// IWANGO maps i and iSD to AeroDancingI (iwango-server/common.h).
	// Capcom's original and Pro releases both register network game code 8
	// (network/capcom.cpp). Keep title/ownership identity separate from status.
	const std::pair<const char *, const char *> sharedServices[] = {
		{ "catalog:aero-dancing-i-jikai", "aeroi" },
		{ "catalog:capcom-vs-snk-pro", "capvsnkmf" },
	};
	for (const auto& link : sharedServices)
	{
		const auto edition = std::find_if(status.games.begin(), status.games.end(), [&](const Game& game) {
			return game.id == link.first;
		});
		const auto source = std::find_if(status.games.begin(), status.games.end(), [&](const Game& game) {
			return game.id == link.second;
		});
		// A future edition-specific report takes precedence over shared reporting.
		if (edition == status.games.end() || source == status.games.end()
				|| edition->hasLiveStatus || edition->hasPlayerActivity)
			continue;
		source->sharedStatusId = source->id;
		edition->sharedStatusId = source->id;
		edition->hasLiveStatus = source->hasLiveStatus;
		edition->online = source->online;
		edition->players = source->players;
		edition->matches = source->matches;
		edition->hasPlayerActivity = source->hasPlayerActivity;
	}
	std::sort(status.games.begin(), status.games.end(), [](const Game& a, const Game& b) {
		const bool aActive = a.players > 0 || a.matches > 0;
		const bool bActive = b.players > 0 || b.matches > 0;
		if (aActive != bActive)
			return aActive;
		if (std::max(a.players, 0) != std::max(b.players, 0))
			return a.players > b.players;
		return a.name < b.name;
	});
	return status;
}

}
