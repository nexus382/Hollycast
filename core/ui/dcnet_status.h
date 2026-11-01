// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <string>
#include <vector>

namespace dcnet
{

enum class ReleaseStage { Unspecified, Beta, Alpha };

struct Game
{
	std::string id;
	std::string name;
	ReleaseStage stage = ReleaseStage::Unspecified;
	bool hasLiveStatus = false;
	bool online = false;
	// Some servers do not report counts; keep that distinct from an empty lobby.
	int players = -1;
	int matches = -1;
	std::string reportedName;
	std::string thumbnailUrl;
	bool hasPlayerActivity = false;
	// Nonempty on both editions when their counts and players share one service.
	std::string sharedStatusId;
	// Keep edition identity stable when a live feed starts reporting a new ID.
	std::string catalogId;
	const std::string& titleId() const { return catalogId.empty() ? id : catalogId; }
	bool isActive() const { return players > 0 || matches > 0 || hasPlayerActivity; }
};

struct Player
{
	std::string name;
	std::string gameId;
	std::string accessPoint;
	std::string gameName;
};

struct Status
{
	std::vector<Game> games;
	std::vector<Player> players;
	// Count each reported service once, including services shared by editions.
	long long playerCount = 0;
	long long matchCount = 0;
	int activeGames = 0;
};

// Exact title keys ignore file extensions and release tags, not title words.
std::string normalizeTitle(const std::string& title);
std::vector<std::string> titleKeys(const Game& game);

// Parse both responses together so a failed refresh never publishes half a snapshot.
Status parseStatus(const std::string& gamesJson, const std::string& playersJson);

}
