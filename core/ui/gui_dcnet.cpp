// Portions Copyright 2026 The Hollycast Authors
#include "gui_dcnet.h"
#include "dcnet_status.h"
#include "gui_util.h"
#include "gui_font.h"
#include "gui_menu.h"
#include "oslib/http_client.h"
#include "boxart/boxart.h"
#include "game_scanner.h"
#include "imgui_driver.h"
#include "IconsFontAwesome6.h"
#include "deps/md5/md5.h"
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <unordered_map>

using namespace i18n;

namespace
{

using Clock = std::chrono::steady_clock;
constexpr auto refreshInterval = std::chrono::seconds(15);
dcnet::Status status;
std::future<dcnet::Status> pending;
Clock::time_point nextRefresh;
Clock::time_point lastSuccess;
bool hasStatus = false;
std::string error;
std::string selectedGame;
ImGuiTextFilter filter;
bool activeOnly = false;
bool ownedOnly = false;
Clock::time_point nextLibraryRefresh;
// Presence in this map means ownership, even when no local cover is available.
std::unordered_map<std::string, std::string> ownedCovers;
struct CachedCover
{
	std::string path;
	bool requested = false;
	bool ready = false;
};
std::unordered_map<std::string, CachedCover> covers;
std::future<std::string> pendingCover;
std::string pendingCoverUrl;

void refreshLibrary(GameScanner& scanner, Boxart& boxart)
{
	if (Clock::now() < nextLibraryRefresh)
		return;
	nextLibraryRefresh = Clock::now() + std::chrono::seconds(2);
	scanner.fetch_game_list();
	std::vector<GameMedia> library;
	{
		std::lock_guard<std::mutex> guard(scanner.get_mutex());
		library = scanner.get_game_list();
	}
	std::vector<GameBoxart> metadata;
	metadata.reserve(library.size());
	std::unordered_map<std::string, std::vector<size_t>> titles;
	for (size_t i = 0; i < library.size(); ++i)
	{
		const auto& media = library[i];
		metadata.push_back(boxart.getCachedBoxart(media));
		if (media.device || media.path.empty())
			continue;
		const GameBoxart& art = metadata.back();
		for (const auto *title : { &media.name, &media.fileName, &media.gameName, &art.name, &art.searchName })
		{
			const std::string key = dcnet::normalizeTitle(*title);
			if (!key.empty())
			{
				auto& matches = titles[key];
				if (matches.empty() || matches.back() != i)
					matches.push_back(i);
			}
		}
	}
	boxart.refreshCustomBoxartIndex(false);
	ownedCovers.clear();
	for (const auto& game : status.games)
	{
		for (const auto& key : dcnet::titleKeys(game))
		{
			const auto matches = titles.find(key);
			if (matches == titles.end())
				continue;
			for (size_t index : matches->second)
			{
				const auto& media = library[index];
				// VF4 is the NAOMI service; other catalog entries need a Dreamcast
				// copy, rather than an identically named arcade ROM.
				if (media.arcade != (game.id == "vf4"))
					continue;
				std::string cover = boxart.getCustomMediaPath(media, config::LibraryCoverMediaMode::Cover);
				if (cover.empty() || !file_exists(cover))
					cover = metadata[index].boxartPath;
				if (!cover.empty() && !file_exists(cover))
					cover.clear();
				auto owned = ownedCovers.emplace(game.id, cover);
				if (owned.first->second.empty())
					owned.first->second = std::move(cover);
			}
		}
	}
}

std::string downloadCover(const std::string& url, const std::string& path)
{
	std::vector<u8> body;
	if (http::get(url, body) != 200 || body.empty() || body.size() > 2 * 1024 * 1024)
		return {};
	int width, height, channels;
	if (!stbi_info_from_memory(body.data(), static_cast<int>(body.size()), &width, &height, &channels)
			|| width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return {};
	const std::string temporary = path + ".tmp";
	FILE *file = nowide::fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return {};
	bool written = std::fwrite(body.data(), 1, body.size(), file) == body.size();
	if (std::fclose(file) != 0)
		written = false;
	if (written && nowide::rename(temporary.c_str(), path.c_str()) == 0)
		return path;
	nowide::remove(temporary.c_str());
	return {};
}

std::string remoteCover(const std::string& url)
{
	// Only retrieve thumbnails from the service's public image directory.
	constexpr char prefix[] = "https://dcnet.flyca.st/gamepic/";
	if (url.compare(0, sizeof(prefix) - 1, prefix) != 0)
		return {};
	auto cached = covers.find(url);
	if (cached == covers.end())
	{
		const std::string directory = get_writable_data_path("dcnet-covers");
		if (!file_exists(directory) && !make_directory(directory))
			return {};
		MD5_CTX context;
		MD5_Init(&context);
		MD5_Update(&context, url.data(), static_cast<unsigned long>(url.size()));
		u8 digest[16];
		MD5_Final(digest, &context);
		const char *hex = "0123456789abcdef";
		std::string filename;
		for (u8 byte : digest)
		{
			filename += hex[byte >> 4];
			filename += hex[byte & 15];
		}
		const std::string path = directory + "/" + filename + ".img";
		CachedCover entry;
		entry.path = path;
		entry.ready = file_exists(path);
		entry.requested = entry.ready;
		cached = covers.emplace(url, std::move(entry)).first;
	}
	if (!cached->second.requested && !pendingCover.valid())
	{
		cached->second.requested = true;
		pendingCoverUrl = url;
		const std::string path = cached->second.path;
		try {
			pendingCover = std::async(std::launch::async, [url, path] { return downloadCover(url, path); });
		} catch (const std::exception&) {
			pendingCoverUrl.clear();
			// Covers are optional; the activity feed must remain usable.
		}
	}
	return cached->second.ready ? cached->second.path : std::string();
}

std::string fetchStatus(const char *endpoint)
{
	std::vector<u8> body;
	const http::Headers headers { { "Cache-Control", "no-cache" } };
	const int code = http::get(std::string("https://dcnet.flyca.st/status/api/") + endpoint, body, &headers);
	if (code != 200)
		throw std::runtime_error("DCnet is unavailable (HTTP " + std::to_string(code) + ").");
	return std::string(body.begin(), body.end());
}

void refreshStatus()
{
	if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			status = pending.get();
			nextLibraryRefresh = {};
			hasStatus = true;
			lastSuccess = Clock::now();
			error.clear();
		} catch (const std::exception&) {
			// Keep the last complete snapshot, but never present it as current data.
			error = T("Unable to refresh DCnet. Check your connection and try again.");
		}
		nextRefresh = Clock::now() + refreshInterval;
	}
	if (!pending.valid() && Clock::now() >= nextRefresh)
	{
		try {
			pending = std::async(std::launch::async, [] {
				const std::string games = fetchStatus("games");
				const std::string players = fetchStatus("players");
				return dcnet::parseStatus(games, players);
			});
		} catch (const std::exception&) {
			error = T("Unable to start a DCnet refresh. Please try again.");
			nextRefresh = Clock::now() + refreshInterval;
		}
	}
}

void renderCount(int count)
{
	if (count < 0)
		ImGui::TextDisabled("-");
	else
		ImGui::Text("%d", count);
}

std::string displayGameName(const dcnet::Game& game)
{
	if (game.stage == dcnet::ReleaseStage::Alpha)
		return game.name + " [" + T("Alpha") + "]";
	if (game.stage == dcnet::ReleaseStage::Beta)
		return game.name + " [" + T("Beta") + "]";
	return game.name;
}

void renderPlayers(const std::string& gameId)
{
	if (!hasStatus)
	{
		ImGui::TextWrapped("%s", T("Live player information is unavailable."));
		return;
	}
	bool any = false;
	const bool showGame = gameId.empty();
	if (ImGui::BeginTable("dcnetPlayers", showGame ? 3 : 2,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn(T("Player"), ImGuiTableColumnFlags_WidthStretch, 2.f);
		if (showGame)
			ImGui::TableSetupColumn(T("Game"), ImGuiTableColumnFlags_WidthStretch, 3.f);
		ImGui::TableSetupColumn(T("Access point"), ImGuiTableColumnFlags_WidthStretch, 1.f);
		ImGui::TableHeadersRow();
		for (const auto& player : status.players)
		{
			if (!gameId.empty() && player.gameId != gameId)
				continue;
			const auto game = std::find_if(status.games.begin(), status.games.end(), [&](const dcnet::Game& item) {
				return item.id == player.gameId;
			});
			const std::string& gameName = game != status.games.end() ? game->name : player.gameId;
			if (showGame && !filter.PassFilter((player.name + " " + gameName).c_str()))
				continue;
			any = true;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", player.name.c_str());
			if (showGame)
			{
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", gameName.empty() ? T("Online") : gameName.c_str());
			}
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", player.accessPoint.empty() ? "-" : player.accessPoint.c_str());
		}
		ImGui::EndTable();
	}
	if (!any)
		ImGui::TextWrapped("%s", showGame && filter.IsActive() ? T("No players match your search.") : T("No players reported right now."));
}

}

void gui_display_dcnet(GameScanner& scanner, Boxart& boxart)
{
	if (status.games.empty())
		status = dcnet::parseStatus("[]", "[]");
	refreshStatus();
	refreshLibrary(scanner, boxart);
	if (pendingCover.valid() && pendingCover.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			covers[pendingCoverUrl].ready = !pendingCover.get().empty();
		} catch (const std::exception&) {
		}
		pendingCoverUrl.clear();
	}
	ImGui::PushFont(largeFont, uiLargeFontSize());
	ImGui::TextUnformatted("DCnet");
	ImGui::PopFont();
	ImGui::TextWrapped("%s", T("Find Dreamcast players and see which games are active."));
	{
		DisabledScope disabled(pending.valid());
		if (ImGui::Button(pending.valid() ? T("Refreshing...") : T("Refresh")))
		{
			nextRefresh = {};
			nextLibraryRefresh = {};
			for (auto& cover : covers)
				if (!cover.second.ready && cover.first != pendingCoverUrl)
					cover.second.requested = false;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("Network settings")))
		GuiMenu::openNetworkSettings();
	if (!error.empty())
		ImGui::TextWrapped("%s", error.c_str());
	if (!hasStatus)
	{
		if (error.empty())
			ImGui::TextWrapped("%s", T("Loading live activity from DCnet..."));
	}

	if (hasStatus)
	{
		const auto age = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - lastSuccess).count();
		ImGui::TextWrapped(T("%s %llds ago. Auto-refresh every 15s while this tab is open."),
				error.empty() ? T("Updated") : T("Showing saved activity from"), static_cast<long long>(age));
		long long players = 0;
		long long matches = 0;
		int activeGames = 0;
		for (const auto& game : status.games)
		{
			players += std::max(game.players, 0);
			matches += std::max(game.matches, 0);
			if (game.players > 0 || game.matches > 0)
				activeGames++;
		}
		ImGui::TextWrapped(T("Players: %lld  /  Active games: %d  /  Lobbies / matches: %lld"), players, activeGames, matches);
	}
	ImGui::Text(T("Games in catalog: %zu  /  In your library: %zu"), status.games.size(), ownedCovers.size());
	const float searchWidth = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(T("Search")).x
			- ImGui::GetStyle().ItemInnerSpacing.x;
	filter.Draw(T("Search"), std::clamp(searchWidth, uiScaled(60), uiScaled(320)));
	ImGui::Checkbox(T("Active games only"), &activeOnly);
	if (ImGui::GetContentRegionAvail().x > uiScaled(400))
		ImGui::SameLine();
	ImGui::Checkbox(T("Owned games only"), &ownedOnly);
	ImGui::TextWrapped("%s", T("Some supported games do not publish live status. A dash means no count is available."));

	if (ImGui::BeginChild("dcnetActivity", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling))
	{
		if (ImGui::BeginTabBar("dcnetViews"))
		{
			if (ImGui::BeginTabItem(T("Games")))
			{
				bool any = false;
				bool openDetails = false;
				if (ImGui::BeginTable("dcnetGames", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("##cover", ImGuiTableColumnFlags_WidthFixed, uiScaled(36));
					ImGui::TableSetupColumn(T("Game"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
					ImGui::TableSetupColumn(T("Players"));
					ImGui::TableSetupColumn(T("Lobbies / matches"));
					ImGui::TableSetupColumn(T("Server"));
					ImGui::TableHeadersRow();
					for (const auto& game : status.games)
					{
						const auto owned = ownedCovers.find(game.id);
						if ((activeOnly && game.players <= 0 && game.matches <= 0)
								|| (ownedOnly && owned == ownedCovers.end()) || !filter.PassFilter(game.name.c_str()))
							continue;
						any = true;
						ImguiID id(game.id.c_str());
						const ImVec2 coverSize = ScaledVec2(32, 40);
						ImGui::TableNextRow(ImGuiTableRowFlags_None, coverSize.y + ImGui::GetStyle().CellPadding.y * 2);
						ImGui::TableNextColumn();
						std::string coverPath = owned == ownedCovers.end() ? std::string() : owned->second;
						if (coverPath.empty() && ImGui::IsRectVisible(coverSize))
							coverPath = remoteCover(game.thumbnailUrl);
						ImguiFileTexture cover(coverPath, true);
						const ImTextureID texture = coverPath.empty() ? ImTextureID() : cover.getId();
						if (texture != ImTextureID())
						{
							const float aspect = imguiDriver->getAspectRatio(texture);
							ImVec2 size = coverSize;
							if (aspect > 0)
							{
								if (aspect > size.x / size.y)
									size.y = size.x / aspect;
								else
									size.x = size.y * aspect;
							}
							cover.draw(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, 1.f);
						}
						else
							ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(),
									ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_GAMEPAD);
						ImGui::Dummy(coverSize);
						ImGui::TableNextColumn();
						const std::string title = displayGameName(game);
						const ImVec2 labelPos = ImGui::GetCursorPos();
						const float rowHeight = std::max(coverSize.y,
								ImGui::CalcTextSize(title.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y);
						if (ImGui::Selectable("##game", selectedGame == game.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight)))
						{
							selectedGame = game.id;
							openDetails = true;
						}
						ImGui::SetCursorPos(labelPos);
						ImGui::TextWrapped("%s", title.c_str());
						ImGui::TableNextColumn();
						renderCount(game.players);
						ImGui::TableNextColumn();
						renderCount(game.matches);
						ImGui::TableNextColumn();
						ImGui::TextWrapped("%s", game.hasLiveStatus ? (game.online ? T("Online") : T("Offline"))
								: game.hasPlayerActivity ? T("Players online") : T("Status unavailable"));
						if (!game.hasLiveStatus && ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", T("DCnet does not publish server status for this title. Online play may still be supported."));
					}
					ImGui::EndTable();
				}
				if (!any)
					ImGui::TextWrapped("%s", ownedOnly ? T("No games from your library match these filters.")
							: activeOnly ? T("No active games reported right now.") : T("No games match your search."));
				if (openDetails)
					ImGui::OpenPopup(T("Game activity"));
				const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
				const ImVec2 available(std::max(1.f, displaySize.x - uiScaled(24)),
						std::max(1.f, displaySize.y - uiScaled(24)));
				// Auto-resizing a stretch table collapses the popup to its header width
				// after the first frame. Keep a stable size and scroll long player lists.
				ImGui::SetNextWindowSize(ImVec2(std::min(uiScaled(700), available.x),
						std::min(uiScaled(420), available.y)), ImGuiCond_Appearing);
				ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(uiScaled(320), available.x),
						std::min(uiScaled(240), available.y)), available);
				ImGui::SetNextWindowPos(ImVec2(displaySize.x / 2, displaySize.y / 2),
						ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				if (ImGui::BeginPopupModal(T("Game activity"), nullptr, ImGuiWindowFlags_NoSavedSettings))
				{
					const auto selected = std::find_if(status.games.begin(), status.games.end(), [](const dcnet::Game& game) {
						return game.id == selectedGame;
					});
					if (ImGui::BeginChild("gameDetails", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
							ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling))
					{
						if (selected != status.games.end())
						{
							ImGui::TextWrapped("%s", displayGameName(*selected).c_str());
							ImGui::TextUnformatted(selected->hasLiveStatus ? (selected->online ? T("Server online") : T("Server offline"))
									: selected->hasPlayerActivity ? T("Players are online; server status is unavailable.")
									: T("DCnet does not publish live status for this title. Online play may still be supported."));
							ImGui::Text("%s:", T("Players"));
							ImGui::SameLine();
							renderCount(selected->players);
							ImGui::Text("%s:", T("Lobbies / matches"));
							ImGui::SameLine();
							renderCount(selected->matches);
							if (!error.empty())
								ImGui::TextWrapped("%s", error.c_str());
							ImGui::Separator();
							renderPlayers(selected->id);
						}
						else
							ImGui::TextWrapped("%s", T("This game is no longer reported by DCnet."));
						scrollWhenDraggingOnVoid();
						windowDragScroll();
					}
					ImGui::EndChild();
					if (ImGui::Button(T("Close")))
						ImGui::CloseCurrentPopup();
					ImGui::EndPopup();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(T("Players")))
			{
				renderPlayers({});
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::EndChild();
}

void gui_term_dcnet()
{
	// Finish the worker before the shared HTTP subsystem is shut down.
	if (pending.valid())
	{
		pending.wait();
		pending = {};
	}
	if (pendingCover.valid())
	{
		pendingCover.wait();
		pendingCover = {};
	}
	covers.clear();
	pendingCoverUrl.clear();
	ownedCovers.clear();
	nextLibraryRefresh = {};
	status = {};
	hasStatus = false;
	error.clear();
	selectedGame.clear();
	nextRefresh = {};
}
