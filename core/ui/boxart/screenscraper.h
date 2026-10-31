// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "scraper.h"
#include "artwork_media.h"
#include "cfg/option.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace screenscraper {

struct MediaType : artwork::MediaType
{
	const char *type;
	const char *alternate;
};

// These bits are persisted in settings and the artwork database. Append new types.
inline constexpr std::array<MediaType, 12> mediaTypes{{
	{artwork::mediaTypes[0], "box-2D", ""},
	{artwork::mediaTypes[1], "mixrbv2", "mixrbv1"},
	{artwork::mediaTypes[2], "wheel-hd", "wheel"},
	{artwork::mediaTypes[3], "screenmarquee", ""},
	{artwork::mediaTypes[4], "support-2D", ""},
	{artwork::mediaTypes[5], "fanart", ""},
	{artwork::mediaTypes[6], "ss", ""},
	{artwork::mediaTypes[7], "sstitle", ""},
	{artwork::mediaTypes[8], "box-3D", ""},
	{artwork::mediaTypes[9], "box-2D-back", ""},
	{artwork::mediaTypes[10], "box-2D-side", ""},
	{artwork::mediaTypes[11], "box-texture", ""},
}};
using artwork::allMedia;
using artwork::coverMedia;

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
	// Bounded striped locks serialize identical cache paths across worker threads,
	// without holding the session quota lock or growing a lock map with the library.
	std::array<std::mutex, 64> mediaLocks;
	std::array<std::unordered_set<std::string>, 64> unavailableMedia;
};
