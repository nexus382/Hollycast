// Portions Copyright 2026 The Hollycast Authors
#include "screenscraper.h"
#include "screenscraper_build_credentials.h"
#include "oslib/http_client.h"
#include "stdclass.h"
#include <nowide/cstdio.hpp>
#include <stb_image.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

u64 number(const json& object, const char *key)
{
	const auto it = object.find(key);
	if (it == object.end())
		return 0;
	if (it->is_number_unsigned())
		return it->get<u64>();
	if (it->is_number_integer())
		return static_cast<u64>(std::max<s64>(0, it->get<s64>()));
	if (!it->is_string())
		return 0;
	const auto& text = it->get_ref<const std::string&>();
	u64 result = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
	return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? result : 0;
}

std::string text(const json& object, const char *key)
{
	const auto it = object.find(key);
	return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string normalizedName(std::string name)
{
	// Region/disc tags in ROM filenames are not part of the game's name.
	name = name.substr(0, name.find_first_of("(["));
	std::string result;
	for (unsigned char c : name)
		if (c == '&') result += "and";
		else if (std::isalnum(c) || c >= 128) result += static_cast<char>(std::tolower(c));
	return result;
}

std::string lowerCase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::vector<std::string> regions(u32 region)
{
	std::vector<std::string> result;
	if (region & GameBoxart::USA) result.push_back("us");
	if (region & GameBoxart::EUROPE) result.push_back("eu");
	if (region & GameBoxart::JAPAN) result.push_back("jp");
	for (const char *fallback : {"wor", "us", "eu", "jp", "ss", ""})
		if (std::find(result.begin(), result.end(), fallback) == result.end())
			result.emplace_back(fallback);
	return result;
}

std::string regionalText(const json& entries, const std::vector<std::string>& preference)
{
	if (!entries.is_array())
		return {};
	for (const auto& region : preference)
		for (const auto& entry : entries)
			if (text(entry, "region") == region && !text(entry, "text").empty())
				return text(entry, "text");
	for (const auto& entry : entries)
		if (!text(entry, "text").empty())
			return text(entry, "text");
	return {};
}

const screenscraper::Credentials& developerCredentials()
{
	// Official builds carry the application's credentials. Runtime overrides are
	// useful for local development, but must replace the complete pair together.
	static const auto result = [] {
		screenscraper::Credentials credentials;
		credentials.developerId = screenscraper::buildCredentials::developerId;
		credentials.developerPassword = screenscraper::buildCredentials::developerPassword;
		const char *id = std::getenv("HOLLYCAST_SCREENSCRAPER_DEV_ID");
		const char *password = std::getenv("HOLLYCAST_SCREENSCRAPER_DEV_PASSWORD");
		if (id || password)
		{
			credentials.developerId = id ? id : "";
			credentials.developerPassword = password ? password : "";
		}
		return credentials;
	}();
	return result;
}

struct Connection
{
	std::shared_ptr<screenscraper::Session> session;
	std::shared_ptr<screenscraper::Session> retiring;
	std::future<void> login;
	~Connection()
	{
		if (session) session->cancel();
		if (retiring) retiring->cancel();
	}
};

// Only the UI thread accesses this owner. Workers retain immutable session snapshots.
Connection& connection()
{
	static Connection result;
	return result;
}

bool imageValid(const std::vector<u8>& bytes)
{
	if (bytes.empty() || bytes.size() > 32 * 1024 * 1024)
		return false;
	int width, height, channels;
	if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels)
			|| width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return false;
	u8 *decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
	if (!decoded)
		return false;
	stbi_image_free(decoded);
	return true;
}

void saveImage(const std::string& path, const std::vector<u8>& bytes)
{
	static std::atomic<u64> nextFile{0};
	// Multiple discs of one game may finish together. Publish only a complete file.
	const std::string temporary = path + ".part-" + std::to_string(nextFile++);
	FILE *file = nowide::fopen(temporary.c_str(), "wb");
	if (!file)
		throw std::runtime_error("ScreenScraper: cannot create the artwork file.");
	const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	const bool closed = fclose(file) == 0;
	std::error_code error;
	if (written && closed)
		std::filesystem::rename(std::filesystem::u8path(temporary), std::filesystem::u8path(path), error);
	if (!written || !closed || error)
	{
		std::filesystem::remove(std::filesystem::u8path(temporary), error);
		if (!file_exists(path))
			throw std::runtime_error("ScreenScraper: cannot save the artwork file.");
	}
}
} // namespace

namespace screenscraper {

Session::Session(Credentials credentials, Transport transport)
	: credentials(std::move(credentials)), transport(transport ? std::move(transport)
		: Transport([](const std::string& url, std::vector<u8>& bytes, std::string& type) {
			return http::get(url, bytes, type);
		}))
{
	account.personalAccount = !this->credentials.username.empty();
}

Account Session::status() const
{
	std::lock_guard<std::mutex> guard(mutex);
	return account;
}

bool Session::ready() const
{
	std::lock_guard<std::mutex> guard(mutex);
	return account.connected && !cancelled && Clock::now() >= retryAt;
}

void Session::cancel()
{
	std::lock_guard<std::mutex> guard(mutex);
	cancelled = true;
	account.connected = false;
	account.message = "Signed out.";
	wake.notify_all();
}

void Session::waitUntilIdle()
{
	std::unique_lock<std::mutex> lock(mutex);
	wake.wait(lock, [&] { return account.activeRequests == 0; });
}

void Session::pause(const std::string& message)
{
	std::lock_guard<std::mutex> guard(mutex);
	account.message = message;
	retryAt = Clock::now() + std::chrono::minutes(1);
	wake.notify_all();
}

void Session::fail(int code)
{
	std::lock_guard<std::mutex> guard(mutex);
	const char *message;
	switch (code)
	{
	case 401: message = "ScreenScraper rejected the account or is closed to this account."; break;
	case 403: message = "ScreenScraper rejected the application or personal account credentials."; break;
	case 423: message = "ScreenScraper is temporarily closed. Reconnect later."; break;
	case 426: message = "This Hollycast application version is not allowed by ScreenScraper."; break;
	case 429: message = "ScreenScraper is busy or rate limited. Retrying after one minute."; break;
	case 430: message = "ScreenScraper daily request allowance reached. Reconnect after it resets."; break;
	case 431: message = "ScreenScraper daily unmatched-game allowance reached. Reconnect after it resets."; break;
	case 400: message = "ScreenScraper rejected the request parameters."; break;
	default: message = "ScreenScraper request failed. Retrying after one minute."; break;
	}
	account.message = message;
	if (code == 400 || code == 401 || code == 403 || code == 423 || code == 426 || code == 430 || code == 431)
		account.connected = false;
	else
		retryAt = Clock::now() + std::chrono::minutes(1);
	wake.notify_all();
	throw std::runtime_error(message);
}

int Session::request(const std::string& endpoint, const std::string& parameters,
		std::vector<u8>& bytes, std::string& contentType)
{
	Clock::time_point started;
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (true)
		{
			if (cancelled || (!account.connected && !account.connecting))
				throw std::runtime_error("ScreenScraper is not connected.");
			const auto now = Clock::now();
			if (now < retryAt)
				throw std::runtime_error("ScreenScraper is waiting before retrying.");
			while (!recentRequests.empty() && now - recentRequests.front() >= std::chrono::minutes(1))
				recentRequests.pop_front();
			if ((account.requestsPerDay && account.requestsToday >= account.requestsPerDay)
					|| (account.missesPerDay && account.missesToday >= account.missesPerDay))
			{
				const int code = account.requestsPerDay && account.requestsToday >= account.requestsPerDay ? 430 : 431;
				lock.unlock();
				fail(code);
			}
			if (account.activeRequests < account.maxThreads
					&& (!account.requestsPerMinute || recentRequests.size() < account.requestsPerMinute)
					&& (!account.missesPerDay || account.missesToday + static_cast<u64>(account.activeRequests) < account.missesPerDay)
					&& now >= nextDownload)
			{
				++account.activeRequests;
				++account.requestsToday;
				recentRequests.push_back(now);
				started = now;
				break;
			}
			wake.wait_for(lock, std::chrono::milliseconds(100));
		}
	}
	// Build URLs only at the transport boundary: never retain or log a credential-bearing URL.
	const std::string url = "https://api.screenscraper.fr/api2/" + endpoint
		+ "?devid=" + http::urlEncode(credentials.developerId)
		+ "&devpassword=" + http::urlEncode(credentials.developerPassword)
		+ "&softname=Hollycast&output=json&ssid=" + http::urlEncode(credentials.username)
		+ "&sspassword=" + http::urlEncode(credentials.password) + parameters;
	int code = 0;
	try { code = transport(url, bytes, contentType); }
	catch (...) { /* Transport exceptions can contain the URL. Report only a fixed message below. */ }
	{
		std::unique_lock<std::mutex> lock(mutex);
		// Share the KB/s allowance across workers, counting time already spent transferring.
		if (account.downloadKBPerSecond && !bytes.empty())
		{
			const double seconds = static_cast<double>(bytes.size()) / (1024.0 * account.downloadKBPerSecond);
			nextDownload = std::max(nextDownload, started)
				+ std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
			while (!cancelled && Clock::now() < nextDownload)
				wake.wait_for(lock, std::chrono::milliseconds(100));
		}
		--account.activeRequests;
		if (code == 404) ++account.missesToday;
		wake.notify_all();
		if (cancelled)
			throw std::runtime_error("ScreenScraper download cancelled.");
	}
	if (!http::success(code) && code != 404)
		fail(code);
	return code;
}

void Session::updateLimits(const json& response)
{
	const auto it = response.find("ssuser");
	if (it == response.end() || !it->is_object())
		return;
	const auto& user = *it;
	std::lock_guard<std::mutex> guard(mutex);
	if (const u64 threads = number(user, "maxthreads"))
		account.maxThreads = static_cast<int>(std::min<u64>(threads, 64));
	if (user.contains("maxdownloadspeed")) account.downloadKBPerSecond = number(user, "maxdownloadspeed");
	if (user.contains("maxrequestsperday")) account.requestsPerDay = number(user, "maxrequestsperday");
	if (user.contains("maxrequestskoperday")) account.missesPerDay = number(user, "maxrequestskoperday");
	if (user.contains("maxrequestspermin")) account.requestsPerMinute = number(user, "maxrequestspermin");
	else if (user.contains("maxrequestsperdmin"))
		account.requestsPerMinute = number(user, "maxrequestsperdmin"); // API game responses use this spelling.
	// Concurrent replies may be older than local reservations. Never move counters backwards.
	account.requestsToday = std::max(account.requestsToday, number(user, "requeststoday"));
	account.missesToday = std::max(account.missesToday, number(user, "requestskotoday"));
	wake.notify_all();
}

json Session::requestJson(const std::string& endpoint, const std::string& parameters)
{
	std::vector<u8> bytes;
	std::string contentType;
	if (request(endpoint, parameters, bytes, contentType) == 404)
		return json::object();
	// The guest-capability reply includes the system catalog, which is larger than game replies.
	const size_t limit = endpoint == "systemesListe.php" ? 16 * 1024 * 1024 : 4 * 1024 * 1024;
	const json root = bytes.size() <= limit ? json::parse(bytes.begin(), bytes.end(), nullptr, false) : json();
	if (!root.is_object() || !root.contains("response") || !root["response"].is_object())
		fail(0);
	updateLimits(root["response"]);
	return root["response"];
}

void Session::login()
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		account.connecting = true;
		account.message = "Connecting to ScreenScraper...";
	}
	try
	{
		// ssuserInfos requires a member login, even when the application is valid.
		// The system catalog also reports the server's guest thread and quota limits.
		const auto response = requestJson(account.personalAccount ? "ssuserInfos.php" : "systemesListe.php", "");
		if (!response.contains("ssuser") || !number(response["ssuser"], "maxthreads")
				|| (account.personalAccount && lowerCase(text(response["ssuser"], "id")) != lowerCase(credentials.username)))
			fail(401); // Reject silent anonymous fallback; it must not borrow guest limits.
		std::lock_guard<std::mutex> guard(mutex);
		account.connected = !cancelled;
		account.connecting = false;
		account.message = cancelled ? "Signed out." : account.personalAccount
			? "Signed in to ScreenScraper." : "Using ScreenScraper guest access.";
	}
	catch (...)
	{
		std::lock_guard<std::mutex> guard(mutex);
		account.connecting = false;
		account.connected = false;
		throw;
	}
}

std::vector<u8> Session::requestImage(u32 gameId, const std::string& type, const std::string& region)
{
	std::vector<u8> bytes;
	std::string contentType;
	const std::string selector = type + (region.empty() ? "" : "(" + region + ")");
	if (request("mediaJeu.php", "&systemeid=23&jeuid=" + std::to_string(gameId)
			+ "&media=" + http::urlEncode(selector) + "&outputformat=png&maxwidth=1920&maxheight=1920", bytes, contentType) == 404)
		return {};
	if (bytes.size() < 32 && trim_ws(std::string(bytes.begin(), bytes.end())) == "NOMEDIA")
		return {};
	if (!imageValid(bytes))
		fail(0);
	return bytes;
}

bool developerConfigured()
{
	const auto& credentials = developerCredentials();
	return !credentials.developerId.empty() && !credentials.developerPassword.empty();
}

void connect(const std::string& username, const std::string& password)
{
	if (!developerConfigured() || (username.empty() != password.empty()))
		return;
	auto& owner = connection();
	if (owner.login.valid())
	{
		if (owner.login.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;
		try { owner.login.get(); } catch (...) { }
	}
	auto previous = owner.session ? owner.session : std::move(owner.retiring);
	if (previous) previous->cancel();
	auto credentials = developerCredentials();
	credentials.username = username;
	credentials.password = password;
	// Capability requests happen before the artwork worker initializes its scrapers.
	http::init();
	owner.session = std::make_shared<Session>(std::move(credentials));
	owner.login = std::async(std::launch::async, [previous = std::move(previous), session = owner.session]() mutable {
		// A reconnect must not overlap the old account's in-flight calls and exceed its allowance.
		if (previous) previous->waitUntilIdle();
		previous.reset();
		session->login();
	});
}

void disconnect()
{
	auto& owner = connection();
	if (owner.session) owner.session->cancel();
	// Preserve the draining session across a quick provider change or sign-out.
	if (owner.session) owner.retiring = std::move(owner.session);
}

Account status()
{
	auto& owner = connection();
	if (owner.login.valid() && owner.login.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		try { owner.login.get(); } catch (...) { }
	Account result = owner.session ? owner.session->status() : Account{};
	if (owner.session && owner.login.valid() && !result.connecting && !result.connected)
	{
		result.connecting = true;
		result.message = "Waiting for previous requests to finish before connecting...";
	}
	return result;
}

std::shared_ptr<Session> currentSession()
{
	// Guest access uses Hollycast's application credentials and server-reported limits.
	// Keep failed sessions until an explicit reconnect, avoiding a retry on every frame.
	if (!connection().session && developerConfigured())
		connect("", "");
	return connection().session;
}

bool needsMedia(const GameBoxart& item, u32 selection)
{
	if (item.arcade) return false;
	if ((selection & artwork::metadataMedia) && !item.screenScraperSearched) return true;
	selection &= allMedia;
	if (!selection) return false;
	if (!item.screenScraperSearched || (item.screenScraperCheckedMedia & selection) != selection)
		return true;
	for (size_t i = 0; i < mediaTypes.size(); ++i)
	{
		const auto it = item.screenScraperMedia.find(mediaTypes[i].name);
		if ((selection & (1u << i)) && it != item.screenScraperMedia.end() && !file_exists(it->second))
			return true;
	}
	return false;
}
} // namespace screenscraper

ScreenScraper::ScreenScraper(std::shared_ptr<screenscraper::Session> session)
	: session(std::move(session))
{
}

bool ScreenScraper::initialize(const std::string& saveDirectory)
{
	if (!session || !session->ready() || saveDirectory.rfind("content://", 0) == 0)
		return false;
	directory = saveDirectory + "screenscraper/";
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::u8path(directory), error);
	return !error;
}

json ScreenScraper::findGame(const GameBoxart& item)
{
	if (item.screenScraperId)
	{
		const auto response = session->requestJson("jeuInfos.php", "&systemeid=23&gameid=" + std::to_string(item.screenScraperId));
		const auto game = response.value("jeu", json());
		return number(game, "id") == item.screenScraperId
			&& (!game.contains("systeme") || number(game["systeme"], "id") == 23) ? game : json();
	}
	// Do not submit the hash of a compressed CHD as if it identified the original disc.
	// Name searches are system-scoped and accept only one exact normalized title.
	const std::array<std::string, 3> names{item.searchName, item.name, get_file_basename(item.fileName)};
	std::vector<std::string> queries(names.begin(), names.end());
	// ScreenScraper's search is punctuation-sensitive. Try the actual filename
	// spelling too, then at most two complete significant words to find aliases
	// such as "Dragon's Blood" from "Dragons Blood". Acceptance below still
	// requires one exact normalized full title, never just a word or top result.
	std::vector<std::string> words;
	std::string word;
	const std::string base = names.back().substr(0, names.back().find_first_of("([")) + ' ';
	for (unsigned char c : base)
	{
		if (std::isalnum(c) || c >= 128) word += static_cast<char>(std::tolower(c));
		else
		{
			if (word.size() >= 4 && std::find(words.begin(), words.end(), word) == words.end()) words.push_back(word);
			word.clear();
		}
	}
	std::stable_sort(words.begin(), words.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
	for (size_t i = 0; i < std::min<size_t>(2, words.size()); ++i) queries.push_back(words[i]);
	std::vector<std::string> searched;
	for (auto query : queries)
	{
		query = trim_ws(query.substr(0, query.find_first_of("([")));
		if (query.empty() || std::find(searched.begin(), searched.end(), lowerCase(query)) != searched.end())
			continue;
		searched.push_back(lowerCase(query));
		const auto response = session->requestJson("jeuRecherche.php", "&systemeid=23&recherche=" + http::urlEncode(query));
		const auto games = response.find("jeux");
		if (games == response.end() || !games->is_array())
			continue;
		json match;
		for (const auto& game : *games)
		{
			if (!number(game, "id") || number(game, "id") > UINT32_MAX
					|| (game.contains("systeme") && number(game["systeme"], "id") != 23))
				continue;
			bool exact = false;
			for (const auto& name : names)
			{
				const std::string normalized = normalizedName(name);
				if (normalized.empty()) continue;
				exact |= normalizedName(text(game, "nom")) == normalized;
				if (game.contains("noms") && game["noms"].is_array())
					for (const auto& candidate : game["noms"])
						exact |= normalizedName(text(candidate, "text")) == normalized;
			}
			if (!exact) continue;
			if (!match.is_null() && number(match, "id") != number(game, "id"))
				return {}; // Ambiguous matches must never silently download another game's art.
			match = game;
		}
		if (!match.is_null()) return match;
	}
	return {};
}

void ScreenScraper::scrape(GameBoxart& item)
{
	const u32 selection = item.requestedMedia & screenscraper::allMedia;
	if (!screenscraper::needsMedia(item, item.requestedMedia))
		return;
	const auto game = item.screenScraperDetails ? *item.screenScraperDetails : findGame(item);
	if (!game.is_object())
	{
		item.screenScraperSearched = true;
		// A missing game has no other image types to fetch in the second phase.
		item.screenScraperCheckedMedia |= screenscraper::allMedia;
		return;
	}
	if (!item.screenScraperDetails)
	{
		// Reuse one lookup across the two queue phases without retaining the API's
		// credential-bearing media URLs or user/profile data.
		json details = json::object();
		for (const char *key : {"id", "nom", "noms", "dates", "synopsis", "developpeur", "editeur", "joueurs", "genres"})
			if (game.contains(key)) details[key] = game[key];
		if (game.contains("medias") && game["medias"].is_array())
			for (const auto& media : game["medias"])
				details["medias"].push_back({{"type", text(media, "type")}, {"region", text(media, "region")}});
		item.screenScraperDetails = std::make_shared<const json>(std::move(details));
	}
	const auto preference = regions(item.region);
	item.screenScraperId = static_cast<u32>(number(game, "id"));
	item.screenScraperSearched = true;
	const auto names = game.find("noms");
	std::string name = names == game.end() ? std::string() : regionalText(*names, preference);
	if (name.empty()) name = text(game, "nom");
	// A partial provider response must not erase metadata from a previous scrape.
	const auto updateText = [](std::string& field, const std::string& value) {
		if (!value.empty()) field = value;
	};
	updateText(item.name, name);
	if (game.contains("dates")) updateText(item.releaseDate, regionalText(game["dates"], preference));
	if (game.contains("synopsis") && game["synopsis"].is_array())
	{
		for (const auto& entry : game["synopsis"])
			if (text(entry, "langue") == "en" && !text(entry, "text").empty()) { item.overview = text(entry, "text"); break; }
	}
	if (game.contains("developpeur")) updateText(item.developer, text(game["developpeur"], "text"));
	if (game.contains("editeur")) updateText(item.publisher, text(game["editeur"], "text"));
	if (game.contains("joueurs")) updateText(item.players, text(game["joueurs"], "text"));
	if (game.contains("genres") && game["genres"].is_array())
	{
		std::string genres;
		for (const auto& genre : game["genres"])
		{
			std::string label;
			if (text(genre, "langue") == "en") label = text(genre, "text");
			if (genre.contains("noms") && genre["noms"].is_array())
				for (const auto& localized : genre["noms"])
					if (text(localized, "langue") == "en") { label = text(localized, "text"); break; }
			if (!label.empty())
			{
				if (!genres.empty()) genres += " / ";
				genres += label;
			}
		}
		updateText(item.genre, genres);
	}
	const auto media = game.find("medias");
	if (media == game.end() || !media->is_array())
	{
		item.screenScraperCheckedMedia |= selection;
		return;
	}
	for (size_t i = 0; i < screenscraper::mediaTypes.size(); ++i)
	{
		const u32 bit = 1u << i;
		if (!(selection & bit)) continue;
		const auto& kind = screenscraper::mediaTypes[i];
		const auto cached = item.screenScraperMedia.find(kind.name);
		if ((item.screenScraperCheckedMedia & bit)
				&& (cached == item.screenScraperMedia.end() || file_exists(cached->second)))
			continue;
		const json *chosen = nullptr;
		for (const char *type : {kind.type, kind.alternate})
		{
			if (!*type) continue;
			for (const auto& region : preference)
			{
				for (const auto& entry : *media)
					if (text(entry, "type") == type && text(entry, "region") == region)
					{ chosen = &entry; break; }
				if (chosen) break;
			}
			if (!chosen)
				for (const auto& entry : *media)
				{
					const std::string region = text(entry, "region");
					if (text(entry, "type") == type && region.size() <= 12
						&& std::all_of(region.begin(), region.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; }))
					{ chosen = &entry; break; }
				}
			if (chosen) break;
		}
		if (chosen)
		{
			const std::string type = text(*chosen, "type"), region = text(*chosen, "region");
			const std::string path = directory + std::to_string(item.screenScraperId) + "-" + type + "-" + region + ".png";
			const size_t slot = std::hash<std::string>{}(path) % mediaLocks.size();
			std::lock_guard<std::mutex> guard(mediaLocks[slot]);
			if (unavailableMedia[slot].count(path))
			{
				item.screenScraperCheckedMedia |= bit;
				continue;
			}
			// Recheck after acquiring ownership: another disc may just have saved it.
			if (!file_exists(path))
			{
				const auto bytes = session->requestImage(item.screenScraperId, type, region);
				if (bytes.empty())
				{
					unavailableMedia[slot].insert(path);
					item.screenScraperCheckedMedia |= bit;
					continue;
				}
				try { saveImage(path, bytes); }
				catch (const std::runtime_error&)
				{
					session->pause("ScreenScraper could not save artwork. Check the writable data folder; retrying after one minute.");
					throw;
				}
			}
			item.screenScraperMedia[kind.name] = path;
			if (i == 0)
			{
				item.boxartPath = path;
				item.boxartUrl.clear();
			}
		}
		item.screenScraperCheckedMedia |= bit;
	}
}

void ScreenScraper::scrape(std::vector<GameBoxart>& items)
{
	std::atomic<size_t> next{0};
	std::vector<std::future<void>> workers;
	const size_t count = std::min(items.size(), static_cast<size_t>(session->status().maxThreads));
	for (size_t i = 0; i < count; ++i)
		workers.push_back(std::async(std::launch::async, [&] {
			for (size_t index = next++; index < items.size(); index = next++)
				scrape(items[index]);
		}));
	// Join every worker before propagating an error; items must not outlive their batch.
	std::exception_ptr error;
	for (auto& worker : workers)
		try { worker.get(); } catch (...) { if (!error) error = std::current_exception(); }
	if (error) std::rethrow_exception(error);
}
