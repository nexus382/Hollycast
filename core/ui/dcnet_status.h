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
};

struct Player
{
	std::string name;
	std::string gameId;
	std::string accessPoint;
};

struct Status
{
	std::vector<Game> games;
	std::vector<Player> players;
};

// Parse both responses together so a failed refresh never publishes half a snapshot.
Status parseStatus(const std::string& gamesJson, const std::string& playersJson);

}
