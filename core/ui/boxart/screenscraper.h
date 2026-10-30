// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "scraper.h"
#include "cfg/option.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace screenscraper {

struct MediaType
{
	const char *name;
	const char *label;
	const char *type;
	const char *alternate;
	config::LibraryCoverMediaMode mode;
};

// These bits are persisted in settings and the artwork database. Append new types.
inline constexpr std::array<MediaType, 11> mediaTypes{{
	{"cover", "Cover", "box-2D", "", config::LibraryCoverMediaMode::Cover},
	{"miximage", "Mix Image", "mixrbv2", "mixrbv1", config::LibraryCoverMediaMode::MixImage},
	{"title", "Wheel / Marquee", "wheel-hd", "wheel", config::LibraryCoverMediaMode::Title},
	{"banner", "Banner", "screenmarquee", "", config::LibraryCoverMediaMode::Banner},
	{"physical", "Physical Media", "support-2D", "", config::LibraryCoverMediaMode::Physical},
	{"fanart", "Fan Art", "fanart", "", config::LibraryCoverMediaMode::FanArt},
	{"screenshot", "Screenshot", "ss", "", config::LibraryCoverMediaMode::Screenshot},
	{"titlescreen", "Title Screen", "sstitle", "", config::LibraryCoverMediaMode::TitleScreen},
	{"case", "Case / 3D Box", "box-3D", "", config::LibraryCoverMediaMode::Case},
	{"backcover", "Back Cover", "box-2D-back", "", config::LibraryCoverMediaMode::BackCover},
	{"spine", "Spine", "box-2D-side", "", config::LibraryCoverMediaMode::Spine},
}};
inline constexpr u32 allMedia = (1u << mediaTypes.size()) - 1;
inline constexpr u32 coverMedia = 1u;

struct Credentials
{
	std::string developerId, developerPassword, username, password;
};

struct Account
{
	bool connected = false;
	bool connecting = false;
	bool personalAccount = false;
	int maxThreads = 1;
	int activeRequests = 0;
	u64 requestsToday = 0, requestsPerDay = 0;
	u64 missesToday = 0, missesPerDay = 0;
	u64 requestsPerMinute = 0, downloadKBPerSecond = 0;
	std::string message = "Not connected to ScreenScraper.";
};

// One session owns the shared quota gate, including login and media requests.
// Credentials stay in memory and are never written to config, cache URLs or logs.
class Session
{
public:
	using Transport = std::function<int(const std::string&, std::vector<u8>&, std::string&)>;
	explicit Session(Credentials credentials, Transport transport = {});
	void login();
	void cancel();
	void waitUntilIdle();
	void pause(const std::string& message);
	bool ready() const;
	Account status() const;
	json requestJson(const std::string& endpoint, const std::string& parameters);
	std::vector<u8> requestImage(u32 gameId, const std::string& type, const std::string& region);

private:
	int request(const std::string& endpoint, const std::string& parameters,
		std::vector<u8>& bytes, std::string& contentType);
	void updateLimits(const json& response);
	void fail(int code);

	const Credentials credentials;
	const Transport transport;
	mutable std::mutex mutex;
	std::condition_variable wake;
	Account account;
	bool cancelled = false;
	std::deque<std::chrono::steady_clock::time_point> recentRequests;
	std::chrono::steady_clock::time_point retryAt{}, nextDownload{};
};

bool developerConfigured();
void connect(const std::string& username, const std::string& password);
void disconnect();
Account status();
std::shared_ptr<Session> currentSession();
bool needsMedia(const GameBoxart& item, u32 selection);

} // namespace screenscraper

class ScreenScraper final : public Scraper
{
public:
	explicit ScreenScraper(std::shared_ptr<screenscraper::Session> session);
	bool initialize(const std::string& saveDirectory) override;
	void scrape(GameBoxart& item) override;
	void scrape(std::vector<GameBoxart>& items) override;

private:
	json findGame(const GameBoxart& item);
	std::shared_ptr<screenscraper::Session> session;
	std::string directory;
};
