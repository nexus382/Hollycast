// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/dcnet_status.h"
#include <algorithm>
#include <set>

TEST(DcnetStatusTest, SortsActiveGamesFirstAndKeepsServerStatus)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"idle","name":"A quiet game","online":true,"playerCount":0,"gameCount":0},
		{"gameId":"busy","name":"Busy game","online":true,"playerCount":4,"gameCount":1},
		{"gameId":"down","name":"Offline game","online":false,"playerCount":0,"gameCount":0}
	])", "[]");
	ASSERT_EQ(54u, status.games.size());
	EXPECT_EQ("busy", status.games[0].id);
	EXPECT_EQ(4, status.games[0].players);
	EXPECT_EQ(1, status.games[0].matches);
	const auto offline = std::find_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.id == "down";
	});
	ASSERT_NE(status.games.end(), offline);
	EXPECT_TRUE(offline->hasLiveStatus);
	EXPECT_FALSE(offline->online);
}

TEST(DcnetStatusTest, MissingCountsAreUnknownRatherThanZero)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"unknown","name":"Unknown counts","online":true,"playerCount":null}
	])", "[]");
	ASSERT_EQ(52u, status.games.size());
	const auto unknown = std::find_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.id == "unknown";
	});
	ASSERT_NE(status.games.end(), unknown);
	EXPECT_EQ(-1, unknown->players);
	EXPECT_EQ(-1, unknown->matches);
}

TEST(DcnetStatusTest, ParsesPlayersWithoutRequiringPrivateNetworkFields)
{
	const auto status = dcnet::parseStatus("[]", R"([
		{"name":"Nex","gameId":"pso","accessPoint":"DreamPi"},
		{"name":null,"loginName":"Guest","gameId":null,"accessPoint":null},
		{}
	])");
	ASSERT_EQ(3u, status.players.size());
	EXPECT_EQ("Nex", status.players[0].name);
	EXPECT_EQ("pso", status.players[0].gameId);
	EXPECT_EQ("DreamPi", status.players[0].accessPoint);
	EXPECT_EQ("Guest", status.players[1].name);
	EXPECT_TRUE(status.players[1].gameId.empty());
	EXPECT_EQ("Unknown player", status.players[2].name);
}

TEST(DcnetStatusTest, KeepsCatalogWithoutInventingActivity)
{
	const auto status = dcnet::parseStatus("[]", "[]");
	ASSERT_EQ(51u, status.games.size());
	for (const auto& game : status.games)
	{
		EXPECT_FALSE(game.hasLiveStatus) << game.name;
		EXPECT_EQ(-1, game.players) << game.name;
		EXPECT_EQ(-1, game.matches) << game.name;
	}
	EXPECT_TRUE(status.players.empty());
}

TEST(DcnetStatusTest, RejectsMalformedOrPartialSnapshots)
{
	EXPECT_ANY_THROW(dcnet::parseStatus("<html>Unavailable</html>", "[]"));
	EXPECT_ANY_THROW(dcnet::parseStatus("[]", "{}"));
	EXPECT_ANY_THROW(dcnet::parseStatus("[null]", "[]"));
	EXPECT_ANY_THROW(dcnet::parseStatus("[]", "[false]"));
	EXPECT_ANY_THROW(dcnet::parseStatus(R"([{"gameId":"pso","online":true}])", "[]"));
}

TEST(DcnetStatusTest, RejectsInvalidCountsAndDuplicateGames)
{
	for (const char *count : { "-1", "2147483648", "18446744073709551615", "1.5", "\"4\"" })
	{
		const std::string game = std::string(R"([{"gameId":"pso","name":"PSO","online":true,"playerCount":)") + count + "}]";
		EXPECT_ANY_THROW(dcnet::parseStatus(game, "[]"));
	}
	EXPECT_ANY_THROW(dcnet::parseStatus(R"([
		{"gameId":"pso","name":"PSO","online":true},
		{"gameId":"pso","name":"PSO","online":true}
	])", "[]"));
}

TEST(DcnetStatusTest, IncludesEveryRequestedServerAndReleaseStage)
{
	const auto status = dcnet::parseStatus("[]", "[]");
	const std::set<std::string> expected = {
		"4x4 Evolution",
		"Aero Dancing F - Todoroki Tsubasa no Hatsu Hikou",
		"Aero Dancing i",
		"Aero Dancing i Jikai Saku Made Matemasen",
		"Alien Front Online", "ChuChu Rocket",
		"Cyber Troopers Virtual-On: Oratorio Tangram",
		"Daytona USA", "Dee Dee Planet", "Golf Shiyou Yo 2",
		"Hundred Swords", "Internet Game Pack", "Monaco Racing Simulation 2",
		"NBA 2K1", "NBA 2K2", "NCAA 2K2", "NFL 2K1", "NFL 2K2",
		"Ooga Booga", "Outtrigger", "PBA Bowling 2001", "Planet Ring",
		"POD: Speedzone", "Power Smash", "Propeller Arena", "Rune Jade",
		"Sega Tetris", "Speed Devils Online", "StarLancer", "The Next Tetris",
		"Toy Racer", "World Series Baseball 2K2", "Worms World Party",
		"Sega Rally 2", "Bomberman Online", "Culdcept II",
		"Marvel vs. Capcom 2 (JP)", "Power Stone 2 (JP)",
		"Street Fighter III 3rd Strike (JP)",
		"Street Fighter Zero 3 for Matching Service", "Spawn (JP)",
		"Vampire Chronicle for Matching Service", "Netto De Tennis",
		"Capcom vs. SNK Pro (JP)",
		"Jojo's Bizarre Adventure for Matching Service", "Project Justice (JP)",
		"Super Street Fighter IIX for Matching Service",
		"Tech Romancer for Matching Service", "Taisen Net Gimmick",
		"Heavy Metal: Geomatrix (JP)", "Super Puzzle Fighter IIX for Matching Service",
	};
	std::set<std::string> actual;
	std::set<std::string> ids;
	for (const auto& game : status.games)
	{
		actual.insert(game.name);
		ids.insert(game.id);
		if (game.stage == dcnet::ReleaseStage::Beta)
			EXPECT_EQ("Sega Rally 2", game.name);
	}
	EXPECT_EQ(expected, actual);
	EXPECT_EQ(status.games.size(), ids.size());
	EXPECT_EQ(1, std::count_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.stage == dcnet::ReleaseStage::Beta;
	}));
	EXPECT_EQ(17, std::count_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.stage == dcnet::ReleaseStage::Alpha;
	}));
}

TEST(DcnetStatusTest, MergesVerifiedIdsAndPreservesCatalogLabels)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"mvsc2","name":"Marvel vs. Capcom 2","online":true,"playerCount":3,"gameCount":1}
	])", "[]");
	ASSERT_EQ(51u, status.games.size());
	const auto& game = status.games.front();
	EXPECT_EQ("mvsc2", game.id);
	EXPECT_EQ("Marvel vs. Capcom 2 (JP)", game.name);
	EXPECT_EQ(dcnet::ReleaseStage::Alpha, game.stage);
	EXPECT_TRUE(game.hasLiveStatus);
	EXPECT_EQ(3, game.players);
	EXPECT_EQ(1, game.matches);
}

TEST(DcnetStatusTest, ExactTitleMatchesAdoptNewApiIdsWithoutDuplicateEntries)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"new4x4id","name":"4x4 Evolution","online":true,"playerCount":2,"gameCount":1}
	])", R"([{"name":"Nex","gameId":"new4x4id"}])");
	ASSERT_EQ(51u, status.games.size());
	EXPECT_EQ("new4x4id", status.games.front().id);
	EXPECT_EQ("4x4 Evolution", status.games.front().name);
	EXPECT_EQ(status.games.front().id, status.players.front().gameId);
}

TEST(DcnetStatusTest, KeepsAdditionalApiGamesSeparateFromSimilarCatalogTitles)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"capvsnkmf","name":"Capcom vs. SNK: Millennium Fight 2000","online":true,"playerCount":2,"gameCount":1},
		{"gameId":"future","name":"New DCnet Game","online":true,"playerCount":0,"gameCount":0}
	])", "[]");
	ASSERT_EQ(53u, status.games.size());
	EXPECT_EQ("capvsnkmf", status.games.front().id);
	const auto pro = std::find_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.name == "Capcom vs. SNK Pro (JP)";
	});
	ASSERT_NE(status.games.end(), pro);
	EXPECT_FALSE(pro->hasLiveStatus);
	EXPECT_EQ(-1, pro->players);
}

TEST(DcnetStatusTest, OwnershipKeysHandleReleaseTagsAndKeepDistinctTitlesSeparate)
{
	const auto catalog = dcnet::parseStatus("[]", "[]");
	struct MatchCase { const char *id; const char *filename; bool owned; };
	const MatchCase cases[] = {
		{ "catalog:4x4-evolution", "4x4 Evolution (USA).chd", true },
		{ "catalog:4x4-evolution", "4x4 Evo (USA).chd", true },
		{ "aerof", "Aero Dancing F (Japan).GDI", true },
		{ "nfl2k1", "NFL 2K1 (USA) [Rev A].chd", true },
		{ "nfl2k1", "NFL 2K2 (USA).chd", false },
		{ "sdo", "Speed Devils - Online Racing (USA).chd", true },
		{ "sdo", "Speed Devils 2 (USA).chd", false },
		{ "sdo", "Speed Devils Online (USA).chd", true },
		{ "sdo", "Speed Devils (USA).chd", false },
		{ "catalog:pba-bowling-2001", "PBA Tour Bowling 2001 (USA).cdi", true },
		{ "virtualon", "Virtual-On - Oratorio Tangram (USA).chd", true },
		{ "mvsc2", "Marvel vs. Capcom 2 - New Age of Heroes (Japan).chd", true },
		{ "catalog:capcom-vs-snk-pro", "Capcom vs. SNK - Millennium Fight 2000 Pro (Japan).chd", true },
		{ "catalog:capcom-vs-snk-pro", "Capcom vs. SNK - Millennium Fight 2000 (Japan).chd", false },
	};
	for (const auto& item : cases)
	{
		const auto game = std::find_if(catalog.games.begin(), catalog.games.end(), [&](const dcnet::Game& game) {
			return game.id == item.id;
		});
		ASSERT_NE(catalog.games.end(), game);
		const auto keys = dcnet::titleKeys(*game);
		EXPECT_EQ(item.owned, std::find(keys.begin(), keys.end(), dcnet::normalizeTitle(item.filename)) != keys.end())
				<< item.filename;
	}
}

TEST(DcnetStatusTest, KeepsReportedTitleAndThumbnailWhenCatalogNameDiffers)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"mvsc2","name":"Marvel vs. Capcom 2","thumbnail":"https://dcnet.flyca.st/gamepic/mvsc2.jpg","online":true}
	])", "[]");
	const auto game = std::find_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
		return game.id == "mvsc2";
	});
	ASSERT_NE(status.games.end(), game);
	EXPECT_EQ("Marvel vs. Capcom 2 (JP)", game->name);
	EXPECT_EQ("Marvel vs. Capcom 2", game->reportedName);
	EXPECT_EQ("https://dcnet.flyca.st/gamepic/mvsc2.jpg", game->thumbnailUrl);
}

TEST(DcnetStatusTest, CatalogFallbackAcceptsVerifiedAliasWithoutBorrowingAnotherGamesStatus)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"pba","name":"PBA Tour Bowling 2001","online":true,"playerCount":2,"gameCount":1}
	])", "[]");
	ASSERT_EQ(51u, status.games.size());
	EXPECT_EQ("pba", status.games.front().id);
	EXPECT_EQ("PBA Bowling 2001", status.games.front().name);
	EXPECT_EQ(2, status.games.front().players);
}

TEST(DcnetStatusTest, RecoversMissingGameFromPlayerFeedWithoutInventingServerHealthOrMatches)
{
	const auto status = dcnet::parseStatus("[]", R"([
		{"name":"Nex","gameId":"4x4","gameName":"4x4 Evolution","thumbnail":"https://dcnet.flyca.st/gamepic/4x4.jpg"},
		{"name":"Friend","gameId":"4x4","gameName":"4x4 Evolution"}
	])");
	ASSERT_EQ(51u, status.games.size());
	const auto& game = status.games.front();
	EXPECT_EQ("4x4", game.id);
	EXPECT_EQ("4x4 Evolution", game.name);
	EXPECT_EQ(2, game.players);
	EXPECT_EQ(-1, game.matches);
	EXPECT_FALSE(game.hasLiveStatus);
	EXPECT_TRUE(game.hasPlayerActivity);
	EXPECT_EQ("https://dcnet.flyca.st/gamepic/4x4.jpg", game.thumbnailUrl);
}

TEST(DcnetStatusTest, PlayerFeedSuppliesPositiveCountWhenGameCountIsUnavailable)
{
	const auto status = dcnet::parseStatus(R"([
		{"gameId":"vf4","name":"Virtua Fighter 4","online":true}
	])", R"([{"name":"Nex","gameId":"vf4","gameName":"Virtua Fighter 4"}])");
	EXPECT_EQ("vf4", status.games.front().id);
	EXPECT_TRUE(status.games.front().hasLiveStatus);
	EXPECT_EQ(1, status.games.front().players);
	EXPECT_EQ(-1, status.games.front().matches);
}

TEST(DcnetStatusTest, AdditionalPlayerOnlyGamesAreKeptAndMissingActivityDoesNotStick)
{
	const auto live = dcnet::parseStatus("[]", R"([
		{"name":"Nex","gameId":"new-game","gameName":"New Online Game"}
	])");
	ASSERT_EQ(52u, live.games.size());
	EXPECT_EQ("New Online Game", live.games.front().name);
	EXPECT_EQ(1, live.games.front().players);
	const auto next = dcnet::parseStatus("[]", "[]");
	EXPECT_EQ(51u, next.games.size());
	EXPECT_TRUE(std::none_of(next.games.begin(), next.games.end(), [](const dcnet::Game& game) {
		return game.hasPlayerActivity || game.players >= 0;
	}));
}
