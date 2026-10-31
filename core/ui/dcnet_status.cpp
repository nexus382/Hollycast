// Portions Copyright 2026 The Hollycast Authors
#include "dcnet_status.h"
#include "json.hpp"

#include <algorithm>
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
		game.hasLiveStatus = true;
		game.online = item.at("online").get<bool>();
		game.players = readCount(item, "playerCount");
		game.matches = readCount(item, "gameCount");
		const auto known = std::find_if(status.games.begin(), status.games.end(), [&](const Game& entry) {
			return entry.id == game.id || (entry.id.compare(0, 8, "catalog:") == 0 && entry.name == game.name);
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
	std::sort(status.games.begin(), status.games.end(), [](const Game& a, const Game& b) {
		const bool aActive = a.players > 0 || a.matches > 0;
		const bool bActive = b.players > 0 || b.matches > 0;
		if (aActive != bActive)
			return aActive;
		if (std::max(a.players, 0) != std::max(b.players, 0))
			return a.players > b.players;
		return a.name < b.name;
	});
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
		status.players.push_back(std::move(player));
	}
	return status;
}

}
