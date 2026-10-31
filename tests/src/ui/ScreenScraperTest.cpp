// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/boxart/screenscraper.h"
#include "ui/boxart/boxart.h"
#include "ui/boxart/gamesdb.h"
#include "ui/game_scanner.h"
#include "oslib/http_client.h"
#include <stb_image_write.h>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <thread>

namespace {
using namespace screenscraper;

json user(int threads = 10)
{
	return {{"id", "tester"}, {"maxthreads", std::to_string(threads)},
		{"requeststoday", "20"}, {"maxrequestsperday", "1000"},
		{"requestskotoday", "2"}, {"maxrequestskoperday", "100"},
		{"maxrequestspermin", "0"}, {"maxdownloadspeed", "0"}};
}

int reply(std::vector<u8>& bytes, const json& response)
{
	const std::string body = json{{"response", response}}.dump();
	bytes.assign(body.begin(), body.end());
	return 200;
}

Credentials credentials()
{
	return {"test-app", "app-secret", "tester", "user-secret &"};
}

std::vector<u8> png()
{
	std::vector<u8> bytes;
	const u8 pixel[] = {100, 120, 140, 255};
	stbi_write_png_to_func([](void *context, void *data, int size) {
		auto& output = *static_cast<std::vector<u8>*>(context);
		const auto *first = static_cast<u8*>(data);
		output.insert(output.end(), first, first + size);
	}, &bytes, 1, 1, 4, pixel, 4);
	return bytes;
}

json game()
{
	return {{"id", "1234"}, {"systeme", {{"id", "23"}}},
		{"noms", json::array({{{"region", "us"}, {"text", "Sonic Adventure"}}})},
		{"dates", json::array({{{"region", "us"}, {"text", "1999-09-09"}}})},
		{"joueurs", {{"text", "1"}}}, {"developpeur", {{"text", "Sonic Team"}}},
		{"medias", json::array({
			{{"type", "box-2D"}, {"region", "us"}},
			{{"type", "mixrbv1"}, {"region", "us"}},
			{{"type", "mixrbv2"}, {"region", "us"}},
			{{"type", "wheel-hd"}, {"region", "wor"}},
			{{"type", "screenmarquee"}, {"region", "wor"}}})}};
}

class ScreenScraperTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previousSelection = config::ScreenScraperMedia;
		static std::atomic<unsigned> next{0};
		root = std::filesystem::temp_directory_path()
			/ ("hollycast-screenscraper-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
				+ "-" + std::to_string(next++));
		std::filesystem::create_directories(root);
	}
	void TearDown() override
	{
		config::ScreenScraperMedia = previousSelection;
		std::filesystem::remove_all(root);
	}
	GameBoxart item() const
	{
		GameBoxart result;
		result.fileName = "Sonic Adventure (USA).chd";
		result.gamePath = "/games/" + result.fileName;
		result.name = "Sonic Adventure";
		result.searchName = "SONIC ADVENTURE";
		result.region = GameBoxart::USA;
		result.requestedMedia = coverMedia;
		return result;
	}
	std::filesystem::path root;
	int previousSelection = 0;
};

TEST_F(ScreenScraperTest, DownloadsOnlySelectedMediaAndPersistsCredentialFreePaths)
{
	std::vector<std::string> downloads;
	int searches = 0;
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		EXPECT_NE(url.find("sspassword=user-secret%20%26"), std::string::npos);
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		if (url.find("jeuRecherche.php") != std::string::npos)
		{
			++searches;
			return reply(bytes, {{"jeux", json::array({game()})}});
		}
		downloads.push_back(url);
		bytes = png();
		return 200;
	});
	session->login();
	ASSERT_EQ(session->status().maxThreads, 10);
	ScreenScraper scraper(session);
	const std::string base = root.u8string() + '/';
	ASSERT_TRUE(scraper.initialize(base));
	auto art = item();
	art.requestedMedia = 3; // cover + mix
	scraper.scrape(art);
	ASSERT_EQ(downloads.size(), 2u);
	EXPECT_NE(downloads[0].find("box-2D%28us%29"), std::string::npos);
	EXPECT_NE(downloads[1].find("mixrbv2%28us%29"), std::string::npos);
	EXPECT_EQ(art.screenScraperMedia.size(), 2u);
	EXPECT_TRUE(std::filesystem::exists(std::filesystem::u8path(art.screenScraperMedia.at("miximage"))));
	EXPECT_EQ(art.developer, "Sonic Team");
	EXPECT_EQ(art.releaseDate, "1999-09-09");
	EXPECT_FALSE(needsMedia(art, 3));
	EXPECT_TRUE(needsMedia(art, 7));
	scraper.scrape(art);
	EXPECT_EQ(searches, 1);
	EXPECT_EQ(downloads.size(), 2u);
	const json saved = art.to_json(base);
	EXPECT_EQ(saved.dump().find("secret"), std::string::npos);
	EXPECT_FALSE(saved["screenscraper_media"]["miximage"].get<std::string>().empty());
	EXPECT_EQ(saved["screenscraper_media"]["miximage"].get<std::string>().find(base), std::string::npos);
	const GameBoxart restored(saved, base);
	EXPECT_EQ(restored.screenScraperMedia, art.screenScraperMedia);
	EXPECT_EQ(restored.requestedMedia, 0u) << "Manual bulk requests must not restart after loading the database";
	std::filesystem::remove(std::filesystem::u8path(art.screenScraperMedia.at("miximage")));
	EXPECT_TRUE(needsMedia(art, 3));
}

TEST_F(ScreenScraperTest, AmbiguousMatchesPreserveExistingArtworkAndDoNotRepeat)
{
	int searches = 0;
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		++searches;
		auto other = game();
		other["id"] = "5678";
		return reply(bytes, {{"jeux", json::array({game(), other})}});
	});
	session->login();
	ScreenScraper scraper(session);
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	auto art = item();
	art.requestedMedia = allMedia;
	art.boxartPath = "existing-cover.png";
	scraper.scrape(art);
	EXPECT_EQ(art.boxartPath, "existing-cover.png");
	EXPECT_EQ(art.screenScraperId, 0u);
	EXPECT_TRUE(art.screenScraperMedia.empty());
	EXPECT_FALSE(needsMedia(art, allMedia));
	scraper.scrape(art);
	EXPECT_EQ(searches, 1);
}

TEST_F(ScreenScraperTest, FilenamePunctuationAndWholeWordSearchRecoverPreviouslyMissedTitles)
{
	struct Title { const char *file; const char *header; const char *query; const char *matched; u32 id; };
	const Title titles[] = {
		{"90 Minutes - Sega Championship Football (Europe).chd", "90MINUTES -SEGA CHAMPIONSHIP FOOTBALL-", "90 Minutes - Sega Championship Football", "90 Minutes : Sega Championship Football", 13696},
		{"AeroWings 2 - Airstrike (USA).chd", "AEROWINGS 2 AIR STRIKE", "AeroWings 2 - Airstrike", "AeroWings 2 : Airstrike", 13738},
		{"Capcom vs. SNK (USA).chd", "CAPCOM VS SNK", "Capcom vs. SNK", "Capcom vs. SNK", 13565},
		{"Dragons Blood (Europe).chd", "DRAGONS_BLOOD", "blood", "Dragon's Blood", 13573},
	};
	for (const auto& title : titles)
	{
		SCOPED_TRACE(title.file);
		int images = 0;
		auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
			if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
			if (url.find("jeuRecherche.php") != std::string::npos)
			{
				if (url.find("&recherche=" + http::urlEncode(title.query)) == std::string::npos)
					return reply(bytes, {{"jeux", json::array()}});
				auto found = game();
				found["id"] = std::to_string(title.id);
				found["noms"] = json::array({{{"region", "us"}, {"text", title.matched}}});
				auto unrelated = found;
				unrelated["id"] = "999999";
				unrelated["noms"] = json::array({{{"region", "us"}, {"text", std::string(title.matched) + " Pro"}}});
				return reply(bytes, {{"jeux", json::array({unrelated, found})}});
			}
			++images;
			bytes = png();
			return 200;
		});
		session->login();
		ScreenScraper scraper(session);
		ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
		auto art = item();
		art.fileName = title.file;
		art.name = title.file;
		art.searchName = title.header;
		scraper.scrape(art);
		EXPECT_EQ(art.screenScraperId, title.id);
		EXPECT_EQ(images, 1);
	}
}

TEST_F(ScreenScraperTest, WordSearchNeverAcceptsAnUnrelatedTopResult)
{
	auto session = std::make_shared<Session>(credentials(), [](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		EXPECT_NE(url.find("jeuRecherche.php"), std::string::npos);
		return reply(bytes, {{"jeux", json::array({game()})}});
	});
	session->login();
	ScreenScraper scraper(session);
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	auto art = item();
	art.fileName = "Dragons Blood.chd";
	art.name = "Dragons Blood";
	art.searchName = "DRAGONS_BLOOD";
	scraper.scrape(art);
	EXPECT_EQ(art.screenScraperId, 0u);
	EXPECT_TRUE(art.screenScraperMedia.empty());
}

TEST_F(ScreenScraperTest, ImprovedMatchingRetriesOldFailuresWithoutInvalidatingSuccessfulDownloads)
{
	auto art = item();
	art.screenScraperSearched = true;
	art.screenScraperCheckedMedia = allMedia;
	const std::string base = root.u8string() + '/';
	auto oldFailure = art.to_json(base);
	oldFailure.erase("screenscraper_match_version");
	GameBoxart retry(oldFailure, base);
	EXPECT_FALSE(retry.screenScraperSearched);
	EXPECT_EQ(retry.screenScraperCheckedMedia, 0u);
	EXPECT_TRUE(needsMedia(retry, coverMedia));
	GameBoxart currentFailure(art.to_json(base), base);
	EXPECT_FALSE(needsMedia(currentFailure, coverMedia));
	oldFailure["screenscraper_id"] = 1234;
	GameBoxart matched(oldFailure, base);
	EXPECT_TRUE(matched.screenScraperSearched);
	EXPECT_EQ(matched.screenScraperCheckedMedia, allMedia);
}

TEST_F(ScreenScraperTest, MissingMediaAndAlternateMixAreHandledWithoutDownloadingOtherTypes)
{
	int images = 0;
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		if (url.find("jeuRecherche.php") != std::string::npos)
		{
			auto result = game();
			result["medias"].erase(2); // V2 absent; V1 still usable.
			return reply(bytes, {{"jeux", json::array({result})}});
		}
		++images;
		EXPECT_NE(url.find("mixrbv1"), std::string::npos);
		bytes = png();
		return 200;
	});
	session->login();
	ScreenScraper scraper(session);
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	auto art = item();
	art.requestedMedia = (1u << 1) | (1u << 5); // mix + unavailable fan art
	scraper.scrape(art);
	EXPECT_EQ(images, 1);
	EXPECT_FALSE(needsMedia(art, (1u << 1) | (1u << 5)));
	EXPECT_EQ(art.screenScraperMedia.count("fanart"), 0u);
}

TEST_F(ScreenScraperTest, AutomaticCoverRequestIgnoresFullManualSelection)
{
	config::ScreenScraperMedia = static_cast<int>(allMedia);
	int images = 0;
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		if (url.find("jeuRecherche.php") != std::string::npos) return reply(bytes, {{"jeux", json::array({game()})}});
		++images;
		EXPECT_NE(url.find("box-2D%28us%29"), std::string::npos);
		bytes = png();
		return 200;
	});
	session->login();
	ScreenScraper scraper(session);
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	auto art = item(); // The same cover-only bit used by normal library requests.
	scraper.scrape(art);
	EXPECT_EQ(images, 1);
	EXPECT_EQ(art.screenScraperMedia.size(), 1u);
	EXPECT_EQ(art.screenScraperMedia.count("cover"), 1u);
}

TEST_F(ScreenScraperTest, FullDownloadIncludesRealSpinesAndAllMetadataSurvivesReload)
{
	int images = 0;
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user(12)}});
		if (url.find("jeuRecherche.php") != std::string::npos)
		{
			auto result = game();
			result["dates"].push_back({{"region", "jp"}, {"text", "1998-12-23"}});
			result["synopsis"] = json::array({{{"langue", "fr"}, {"text", "French description"}},
				{{"langue", "en"}, {"text", "English description"}}});
			result["editeur"] = {{"text", "Sega"}};
			result["genres"] = json::array({
				{{"id", "7"}, {"noms", json::array({{{"langue", "fr"}, {"text", "Plateforme"}},
					{{"langue", "en"}, {"text", "Platform"}}})}},
				{{"langue", "en"}, {"text", "Adventure"}}});
			result["medias"] = json::array();
			for (const auto& kind : mediaTypes)
				result["medias"].push_back({{"type", kind.type}, {"region", "us"}});
			return reply(bytes, {{"jeux", json::array({result})}});
		}
		++images;
		bytes = png();
		return 200;
	});
	session->login();
	ScreenScraper scraper(session);
	const std::string base = root.u8string() + '/';
	ASSERT_TRUE(scraper.initialize(base));
	auto art = item();
	art.requestedMedia = allMedia;
	scraper.scrape(art);
	EXPECT_EQ(images, static_cast<int>(mediaTypes.size()));
	EXPECT_EQ(art.screenScraperMedia.size(), mediaTypes.size());
	ASSERT_EQ(art.screenScraperMedia.count("spine"), 1u);
	EXPECT_NE(art.screenScraperMedia.at("spine").find("box-2D-side-us"), std::string::npos);
	ASSERT_EQ(art.screenScraperMedia.count("boxtexture"), 1u);
	EXPECT_NE(art.screenScraperMedia.at("boxtexture").find("box-texture-us"), std::string::npos);
	const GameBoxart restored(art.to_json(base), base);
	GameMedia display;
	display.fileName = art.fileName;
	display.name = display.fileName;
	restored.applyMetadata(display);
	EXPECT_EQ(display.name, "Sonic Adventure");
	EXPECT_EQ(display.desc, "English description");
	EXPECT_EQ(display.releaseDate, "1999-09-09");
	EXPECT_EQ(display.developer, "Sonic Team");
	EXPECT_EQ(display.publisher, "Sega");
	EXPECT_EQ(display.genre, "Platform / Adventure");
	EXPECT_EQ(display.players, "1");
	GameMedia local = display;
	local.name = "My custom title";
	local.desc = "My description";
	local.players = "1-4";
	local.releaseDate = "2000";
	restored.applyMetadata(local);
	EXPECT_EQ(local.name, "My custom title");
	EXPECT_EQ(local.desc, "My description");
	EXPECT_EQ(local.players, "1-4");
	EXPECT_EQ(local.releaseDate, "2000");
}

TEST_F(ScreenScraperTest, PartialMetadataResponseKeepsPreviousValues)
{
	auto session = std::make_shared<Session>(credentials(), [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user()}});
		auto result = game();
		result["developpeur"] = {{"text", ""}};
		result["editeur"] = {{"text", ""}};
		result["dates"] = json::array();
		result["genres"] = json::array();
		result["medias"] = json::array();
		return reply(bytes, {{"jeux", json::array({result})}});
	});
	session->login();
	ScreenScraper scraper(session);
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	auto art = item();
	art.developer = "Sonic Team";
	art.publisher = "Sega";
	art.genre = "Platform";
	art.releaseDate = "1999-09-09";
	scraper.scrape(art);
	EXPECT_EQ(art.developer, "Sonic Team");
	EXPECT_EQ(art.publisher, "Sega");
	EXPECT_EQ(art.genre, "Platform");
	EXPECT_EQ(art.releaseDate, "1999-09-09");
}

TEST(ScreenScraperAccountTest, GuestAccessUsesReportedLimitsWithoutPersonalCredentials)
{
	auto guestCredentials = credentials();
	guestCredentials.username.clear();
	guestCredentials.password.clear();
	Session guest(guestCredentials, [](const std::string& url, std::vector<u8>& bytes, std::string&) {
		EXPECT_NE(url.find("systemesListe.php"), std::string::npos);
		EXPECT_NE(url.find("&ssid=&sspassword="), std::string::npos);
		auto limits = user(1);
		limits["id"] = "";
		return reply(bytes, {{"ssuser", limits}});
	});
	guest.login();
	EXPECT_TRUE(guest.ready());
	EXPECT_FALSE(guest.status().personalAccount);
	EXPECT_EQ(guest.status().maxThreads, 1);
	EXPECT_EQ(guest.status().requestsPerDay, 1000u);
}

TEST(ScreenScraperAccountTest, RefusesGuestFallbackAndDoesNotExposeTransportSecrets)
{
	Session guest(credentials(), [](const std::string&, std::vector<u8>& bytes, std::string&) {
		auto anonymous = user(1);
		anonymous["id"] = "";
		return reply(bytes, {{"ssuser", anonymous}});
	});
	EXPECT_THROW(guest.login(), std::runtime_error);
	EXPECT_FALSE(guest.ready());
	Session failure(credentials(), [](const std::string& url, std::vector<u8>&, std::string&) -> int {
		throw std::runtime_error(url);
	});
	try { failure.login(); FAIL(); }
	catch (const std::runtime_error& error) { EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos); }
	EXPECT_EQ(failure.status().message.find("secret"), std::string::npos);
	EXPECT_EQ(failure.status().activeRequests, 0);
}

TEST(ScreenScraperAccountTest, EnforcesDailyReservationsAndHandlesApiLimits)
{
	int calls = 0;
	Session session(credentials(), [&](const std::string&, std::vector<u8>& bytes, std::string&) {
		if (++calls == 1)
		{
			auto limits = user();
			limits["requeststoday"] = "999";
			limits.erase("maxrequestspermin");
			limits["maxrequestsperdmin"] = "500";
			return reply(bytes, {{"ssuser", limits}});
		}
		return reply(bytes, json::object());
	});
	session.login();
	EXPECT_EQ(session.status().requestsPerMinute, 500u);
	session.requestJson("jeuRecherche.php", "&recherche=test");
	EXPECT_THROW(session.requestJson("jeuRecherche.php", "&recherche=test"), std::runtime_error);
	EXPECT_EQ(calls, 2);
	EXPECT_FALSE(session.ready());
	for (int code : {401, 403, 423, 426, 429, 430, 431, 500})
	{
		int requests = 0;
		Session limited(credentials(), [&](const std::string&, std::vector<u8>& bytes, std::string&) {
			return ++requests == 1 ? reply(bytes, {{"ssuser", user()}}) : code;
		});
		limited.login();
		EXPECT_THROW(limited.requestJson("jeuRecherche.php", ""), std::runtime_error);
		EXPECT_FALSE(limited.ready());
		EXPECT_EQ(limited.status().activeRequests, 0);
	}
}

TEST(ScreenScraperAccountTest, SharesThreadAllowanceAcrossConcurrentRequests)
{
	for (int allowance : {1, 10, 12})
	{
		SCOPED_TRACE(allowance);
		std::atomic<int> active{0}, peak{0}, calls{0};
		std::mutex barrierMutex;
		std::condition_variable barrier;
		Session session(credentials(), [&](const std::string&, std::vector<u8>& bytes, std::string&) {
			if (++calls == 1) return reply(bytes, {{"ssuser", user(allowance)}});
			const int current = ++active;
			int oldPeak = peak;
			while (oldPeak < current && !peak.compare_exchange_weak(oldPeak, current)) { }
			{
				std::unique_lock<std::mutex> lock(barrierMutex);
				barrier.notify_all();
				barrier.wait_for(lock, std::chrono::seconds(1), [&] { return peak >= allowance; });
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			--active;
			return reply(bytes, json::object());
		});
		session.login();
		std::vector<std::future<void>> work;
		for (int i = 0; i < allowance + 2; ++i)
			work.push_back(std::async(std::launch::async, [&] { session.requestJson("jeuRecherche.php", ""); }));
		for (auto& request : work) request.get();
		EXPECT_EQ(peak, allowance);
		EXPECT_EQ(session.status().activeRequests, 0);
		session.cancel();
		EXPECT_THROW(session.requestJson("jeuRecherche.php", ""), std::runtime_error);
		EXPECT_EQ(calls, allowance + 3);
	}
}

TEST(ScreenScraperAccountTest, SwitchingSessionWaitsForCancelledRequestsToFinish)
{
	std::promise<void> entered, release;
	auto enteredFuture = entered.get_future();
	auto releaseFuture = release.get_future();
	int calls = 0;
	Session session(credentials(), [&](const std::string&, std::vector<u8>& bytes, std::string&) {
		if (++calls == 1) return reply(bytes, {{"ssuser", user(1)}});
		entered.set_value();
		releaseFuture.wait();
		return reply(bytes, json::object());
	});
	session.login();
	auto request = std::async(std::launch::async, [&] { session.requestJson("jeuRecherche.php", ""); });
	EXPECT_EQ(enteredFuture.wait_for(std::chrono::seconds(1)), std::future_status::ready);
	session.cancel();
	auto idle = std::async(std::launch::async, [&] { session.waitUntilIdle(); });
	EXPECT_EQ(idle.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	release.set_value();
	EXPECT_THROW(request.get(), std::runtime_error);
	idle.get();
	EXPECT_EQ(session.status().activeRequests, 0);
}
} // namespace

class ScreenScraperQueueTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		oldDataPath = get_writable_data_path("");
		root = std::filesystem::temp_directory_path() / ("hollycast-queue-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		set_user_data_dir(root.u8string());
		previousProvider = config::ArtworkProvider;
		previousSource = config::BoxartSourceMode;
		previousFetch = config::FetchBoxart;
		previousSelection = config::ScreenScraperMedia;
		config::ArtworkProvider = 1;
		config::BoxartSourceMode = 0;
		config::FetchBoxart = false; // Exercise queue state without making network requests.
		config::ScreenScraperMedia = static_cast<int>(screenscraper::allMedia);
		boxart.databaseLoaded = true;
		media.path = "/games/test.chd";
		media.fileName = "test.chd";
		media.name = "Test";
	}
	void TearDown() override
	{
		++boxart.screenGeneration;
		boxart.artworkWake.notify_all();
		if (boxart.screenFetching.valid()) boxart.screenFetching.get();
		config::ArtworkProvider = previousProvider;
		config::BoxartSourceMode = previousSource;
		config::FetchBoxart = previousFetch;
		config::ScreenScraperMedia = previousSelection;
		set_user_data_dir(oldDataPath);
		std::filesystem::remove_all(root);
	}
	void seedPrepared(int count)
	{
		for (int i = 0; i < count; ++i)
		{
			GameBoxart item;
			item.fileName = "Game " + std::to_string(i) + ".chd";
			item.gamePath = "/games/" + item.fileName;
			item.screenScraperId = 100 + i;
			item.region = GameBoxart::USA;
			item.workRevision = i + 1;
			item.offlineArtState = OfflineArtState::NoArtwork;
			item.requestedMedia = 3;
			item.busy = true;
			boxart.games[item.sourceKey()] = item;
			boxart.toFetch.push_back(item);
		}
	}
	std::shared_ptr<Session> mockSession(int threads, const Session::Transport& imageTransport, std::atomic<int>& lookups)
	{
		auto result = std::make_shared<Session>(credentials(), [=, &lookups](const std::string& url, std::vector<u8>& bytes, std::string& type) {
			if (url.find("ssuserInfos.php") != std::string::npos) return reply(bytes, {{"ssuser", user(threads)}});
			if (url.find("jeuInfos.php") != std::string::npos)
			{
				++lookups;
				auto response = game();
				response["id"] = std::to_string(std::stoul(url.substr(url.find("&gameid=") + 8)));
				return reply(bytes, {{"jeu", response}});
			}
			return imageTransport(url, bytes, type);
		});
		result->login();
		return result;
	}
	void run(const std::shared_ptr<Session>& session)
	{
		boxart.runScreenScraperQueue(session, boxart.screenGeneration, root.u8string() + '/');
	}
	bool runWithProducer(const std::shared_ptr<Session>& session, GameBoxart& preparing)
	{
		boxart.physicalWorking = true;
		boxart.screenFetching = std::async(std::launch::async, [&] { run(session); });
		const bool waiting = boxart.screenFetching.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
		{
			std::lock_guard<std::mutex> lock(boxart.mutex);
			finish(preparing);
			boxart.physicalWorking = false;
			boxart.artworkWake.notify_all();
		}
		boxart.screenFetching.get();
		return waiting;
	}
	void stopQueue()
	{
		std::lock_guard<std::mutex> lock(boxart.mutex);
		++boxart.screenGeneration;
		boxart.artworkWake.notify_all();
	}
	bool hasTransientLookups() const
	{
		return std::any_of(boxart.games.begin(), boxart.games.end(), [](const auto& item) { return !!item.second.screenScraperDetails; });
	}
	std::filesystem::path root;
	std::string oldDataPath;
	GameBoxart queue(u32 mask = screenscraper::coverMedia)
	{
		return boxart.getBoxartAndQueue(media, false, mask);
	}
	GameBoxart takeWork()
	{
		GameBoxart item = boxart.toFetch.front();
		boxart.toFetch.erase(boxart.toFetch.begin());
		return item;
	}
	void finish(GameBoxart& item) { if (config::ArtworkProvider.get() != 0) item.tgdbCheckedMedia = TheGamesDb::supportedMedia; boxart.completeArtworkWork(item, true, item.arcade ? 0 : config::ArtworkProvider.get()); }
	void seedArcadeRequest()
	{
		GameBoxart arcade;
		arcade.fileName = "arcade.zip";
		arcade.gamePath = "/games/arcade.zip";
		arcade.arcade = true;
		arcade.busy = true;
		boxart.games[arcade.sourceKey()] = arcade;
		boxart.toFetch.push_back(arcade);
	}
	const GameBoxart& record() { return boxart.games.at(queueKey); }
	size_t queued() { return boxart.toFetch.size(); }
	Boxart boxart;
	GameMedia media;
	std::string queueKey = "disc:/games/test.chd";
	int previousProvider = 0, previousSource = 0, previousSelection = 0;
	bool previousFetch = false;
};

TEST_F(ScreenScraperQueueTest, BasicLibraryQueuesOnlyCoverRegardlessOfManualChoices)
{
	const auto automatic = queue();
	EXPECT_EQ(automatic.requestedMedia, screenscraper::coverMedia);
	EXPECT_EQ(queued(), 1u);
	EXPECT_EQ(takeWork().requestedMedia, screenscraper::coverMedia);
}

TEST_F(ScreenScraperQueueTest, FullMediaRequestSurvivesAnInFlightCoverJob)
{
	queue();
	auto coverJob = takeWork();
	queue(screenscraper::allMedia);
	EXPECT_EQ(queued(), 0u) << "The existing worker owns this revision";
	coverJob.offlineArtState = OfflineArtState::NoArtwork;
	coverJob.screenScraperSearched = true;
	coverJob.screenScraperCheckedMedia = screenscraper::coverMedia;
	finish(coverJob);
	ASSERT_EQ(queued(), 1u);
	auto expanded = takeWork();
	EXPECT_EQ(expanded.requestedMedia, screenscraper::allMedia);
	EXPECT_EQ(expanded.screenScraperCheckedMedia, screenscraper::coverMedia);
	EXPECT_TRUE(record().busy);
	expanded.screenScraperCheckedMedia = screenscraper::allMedia;
	finish(expanded);
	EXPECT_EQ(queued(), 0u);
	EXPECT_FALSE(record().busy);
	EXPECT_EQ(record().requestedMedia, 0u);
}

TEST_F(ScreenScraperQueueTest, TheGamesDbMergesExpandedMediaIntoAnInFlightCoverJob)
{
	config::ArtworkProvider = 0;
	queue();
	auto coverJob = takeWork();
	queue(artwork::allMedia);
	EXPECT_EQ(queued(), 0u);
	coverJob.offlineArtState = OfflineArtState::NoArtwork;
	coverJob.tgdbCheckedMedia = artwork::coverMedia;
	finish(coverJob);
	ASSERT_EQ(queued(), 1u);
	auto expanded = takeWork();
	EXPECT_EQ(expanded.requestedMedia, TheGamesDb::supportedMedia);
	expanded.tgdbCheckedMedia = TheGamesDb::supportedMedia;
	finish(expanded);
	EXPECT_EQ(queued(), 0u);
	EXPECT_FALSE(record().busy);
}

TEST_F(ScreenScraperQueueTest, AddingAnotherGameAfterBulkRequestStillQueuesOnlyCover)
{
	queue(screenscraper::allMedia);
	media.path = "/games/new.chd";
	media.fileName = "new.chd";
	const auto newlyAdded = queue();
	EXPECT_EQ(newlyAdded.requestedMedia, screenscraper::coverMedia);
}

TEST_F(ScreenScraperQueueTest, RetryingDreamcastMediaPreservesArcadeRequests)
{
	queue();
	seedArcadeRequest();
	boxart.retryArtworkMedia();
	ASSERT_EQ(queued(), 1u);
	EXPECT_TRUE(takeWork().arcade);
}

TEST_F(ScreenScraperQueueTest, SlowCoverDoesNotIdleOtherWorkersAndAllCoversPrecedeExtras)
{
	seedPrepared(6);
	std::atomic<int> covers{0}, extras{0}, lookups{0};
	std::atomic<bool> drainedWhileSlow{false}, coverPriority{true};
	std::mutex barrierMutex;
	std::condition_variable barrier;
	auto session = mockSession(2, [&](const std::string& url, std::vector<u8>& bytes, std::string&) {
		if (url.find("box-2D") != std::string::npos)
		{
			if (url.find("jeuid=100&") != std::string::npos)
			{
				std::unique_lock<std::mutex> lock(barrierMutex);
				drainedWhileSlow = barrier.wait_for(lock, std::chrono::seconds(2), [&] { return covers == 5; });
			}
			++covers;
			barrier.notify_all();
		}
		else { if (covers != 6) coverPriority = false; ++extras; }
		bytes = png();
		return 200;
	}, lookups);
	run(session);
	EXPECT_TRUE(drainedWhileSlow);
	EXPECT_TRUE(coverPriority);
	EXPECT_EQ(covers, 6);
	EXPECT_EQ(extras, 6);
	EXPECT_EQ(lookups, 6) << "Cover and extra phases must share their sanitized lookup";
	EXPECT_EQ(queued(), 0u);
	EXPECT_EQ(boxart.downloadProgress().games, 0u);
	EXPECT_FALSE(hasTransientLookups());
}

TEST_F(ScreenScraperQueueTest, ProducerRefillsWorkersWithoutAnotherUiFrame)
{
	seedPrepared(1);
	auto preparing = takeWork();
	std::atomic<int> lookups{0};
	auto session = mockSession(1, [](const std::string&, std::vector<u8>& bytes, std::string&) { bytes = png(); return 200; }, lookups);
	EXPECT_TRUE(runWithProducer(session, preparing));
	EXPECT_EQ(lookups, 1);
	EXPECT_EQ(boxart.downloadProgress().games, 0u);
}

TEST_F(ScreenScraperQueueTest, SettingsGenerationStopsRefillAndPreservesRemainingDownloads)
{
	seedPrepared(4);
	std::atomic<int> downloads{0}, lookups{0};
	auto session = mockSession(1, [&](const std::string&, std::vector<u8>& bytes, std::string&) {
		if (++downloads == 1) stopQueue();
		bytes = png();
		return 200;
	}, lookups);
	run(session);
	EXPECT_EQ(downloads, 1);
	EXPECT_EQ(boxart.downloadProgress().covers, 3u);
	EXPECT_EQ(boxart.downloadProgress().games, 4u);
	run(session);
	EXPECT_EQ(downloads, 8);
	EXPECT_EQ(lookups, 4);
	EXPECT_EQ(boxart.downloadProgress().games, 0u);
}

// Opt in explicitly: ordinary test runs must never consume a user's API quota.
TEST(ScreenScraperLiveTest, DISABLED_RecoversPreviouslyUnmatchedDreamcastTitles)
{
	const char *output = std::getenv("HOLLYCAST_SCREENSCRAPER_LIVE_OUTPUT");
	if (!output || !*output) GTEST_SKIP() << "Set a live output directory for manual verification.";
	const auto session = screenscraper::currentSession();
	ASSERT_TRUE(session);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	auto account = screenscraper::status();
	while (account.connecting && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		account = screenscraper::status();
	}
	ASSERT_TRUE(account.connected) << account.message;
	ScreenScraper scraper(session);
	const std::string base = std::string(output) + '/';
	ASSERT_TRUE(scraper.initialize(base));
	struct Title { const char *file; const char *header; u32 id; u32 region; };
	const Title titles[] = {
		{"90 Minutes - Sega Championship Football (Europe).chd", "90MINUTES -SEGA CHAMPIONSHIP FOOTBALL-", 13696, GameBoxart::EUROPE},
		{"AeroWings 2 - Airstrike (USA).chd", "AEROWINGS 2 AIR STRIKE", 13738, GameBoxart::USA},
		{"Capcom vs. SNK (USA).chd", "CAPCOM VS SNK", 13565, GameBoxart::USA},
		{"Dragons Blood (Europe).chd", "DRAGONS_BLOOD", 13573, GameBoxart::EUROPE},
	};
	for (const auto& title : titles)
	{
		SCOPED_TRACE(title.file);
		GameBoxart art;
		art.fileName = title.file;
		art.name = title.file;
		art.searchName = title.header;
		art.region = title.region;
		art.requestedMedia = screenscraper::coverMedia;
		try { scraper.scrape(art); }
		catch (...) { FAIL() << session->status().message; }
		EXPECT_EQ(art.screenScraperId, title.id);
		EXPECT_EQ(art.screenScraperMedia.count("cover"), 1u);
		std::ofstream(std::filesystem::u8path(base + std::to_string(title.id) + "-result.json")) << art.to_json(base).dump(2);
		std::cout << "Live match: " << title.file << " -> " << art.screenScraperId << std::endl;
	}
	screenscraper::disconnect();
}

TEST(ScreenScraperLiveTest, DISABLED_DownloadsDreamcastMetadataAndArtwork)
{
	const char *output = std::getenv("HOLLYCAST_SCREENSCRAPER_LIVE_OUTPUT");
	if (!output || !*output)
		GTEST_SKIP() << "Set HOLLYCAST_SCREENSCRAPER_LIVE_OUTPUT for a manual live scrape.";
	ASSERT_TRUE(screenscraper::developerConfigured());
	const char *username = std::getenv("HOLLYCAST_SCREENSCRAPER_TEST_USER");
	const char *password = std::getenv("HOLLYCAST_SCREENSCRAPER_TEST_PASSWORD");
	screenscraper::connect(username ? username : "", password ? password : "");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	auto account = screenscraper::status();
	while (account.connecting && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		account = screenscraper::status();
	}
	ASSERT_TRUE(account.connected) << account.message;
	const auto session = screenscraper::currentSession();
	ScreenScraper scraper(session);
	const std::string base = std::string(output) + '/';
	ASSERT_TRUE(scraper.initialize(base));
	GameBoxart art;
	art.fileName = "Sonic Adventure (USA).chd";
	art.name = "Sonic Adventure";
	art.searchName = "SONIC ADVENTURE";
	art.region = GameBoxart::USA;
	art.requestedMedia = screenscraper::allMedia;
	try { scraper.scrape(art); }
	catch (...) { FAIL() << session->status().message; }
	ASSERT_NE(art.screenScraperId, 0u);
	EXPECT_FALSE(art.overview.empty());
	EXPECT_FALSE(art.releaseDate.empty());
	EXPECT_FALSE(art.developer.empty());
	EXPECT_FALSE(art.publisher.empty());
	EXPECT_FALSE(art.genre.empty());
	EXPECT_FALSE(art.players.empty());
	EXPECT_EQ(art.screenScraperMedia.count("cover"), 1u);
	EXPECT_EQ(art.screenScraperMedia.count("spine"), 1u);
	for (const auto& media : art.screenScraperMedia)
		EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::u8path(media.second)));
	std::ofstream(std::filesystem::u8path(base + "live-result.json")) << art.to_json(base).dump(2);
	std::cout << "Live ScreenScraper: " << (account.personalAccount ? "personal account" : "guest")
		<< ", allowed threads=" << account.maxThreads << ", game=" << art.screenScraperId
		<< ", downloaded image types=" << art.screenScraperMedia.size() << std::endl;
	screenscraper::disconnect();
}
