// Portions Copyright 2026 The Hollycast Authors
#include "gui_dcnet.h"
#include "gui.h"
#include "dcnet_status.h"
#include "dcnet_probe.h"
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
#include <array>
#include <chrono>
#include <future>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace i18n;

namespace
{

using Clock = std::chrono::steady_clock;
constexpr auto refreshInterval = std::chrono::seconds(15);
dcnet::Status status;
struct StatusResult
{
	dcnet::Status value;
	Clock::time_point fetched;
};
std::future<StatusResult> pending;
Clock::time_point nextRefresh;
Clock::time_point lastSuccess;
bool hasStatus = false;
std::string error;
std::string selectedGame;
ImGuiTextFilter filter;
bool activeOnly = false;
bool ownedOnly = false;
size_t selectedCopy = 0;
Clock::time_point nextLibraryRefresh;

bool hasFreshStatus()
{
	return hasStatus && error.empty() && Clock::now() - lastSuccess < refreshInterval * 3;
}
struct OwnedTitle
{
	std::vector<GameMedia> copies;
	std::string cover;
};
// Keep launchable library copies separate from shared online-service identities.
std::unordered_map<std::string, OwnedTitle> ownedGames;
struct CachedCover
{
	std::string path;
	bool requested = false;
	bool ready = false;
};
std::unordered_map<std::string, CachedCover> covers;
std::future<std::string> pendingCover;
std::string pendingCoverUrl;

struct ServiceResult
{
	bool responding;
	Clock::time_point checked;
};

struct ServiceCheck
{
	const char *gameId;
	std::future<ServiceResult> pending;
	Clock::time_point checked;
	Clock::time_point nextCheck;
	bool responding = false;
};
// Separate availability from the feed, so probes cannot change activity totals.
std::array<ServiceCheck, 2> serviceChecks{{{"catalog:pba-bowling-2001"}, {"catalog:sega-rally-2"}}};
constexpr auto serviceRefreshInterval = std::chrono::minutes(2);
constexpr auto serviceExpiry = std::chrono::minutes(3);

void refreshServices()
{
	for (size_t i = 0; i < serviceChecks.size(); ++i)
	{
		auto& check = serviceChecks[i];
		if (check.pending.valid() && check.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			try {
				const auto result = check.pending.get();
				check.responding = result.responding;
				check.checked = result.checked;
			} catch (const std::exception&) {
				check.responding = false;
				check.checked = {};
			}
			check.nextCheck = check.checked == Clock::time_point{} ? Clock::now() + serviceRefreshInterval
					: check.checked + serviceRefreshInterval;
		}
		if (!check.pending.valid() && Clock::now() >= check.nextCheck)
		{
			try {
				check.pending = std::async(std::launch::async, [i] {
					const bool responding = i == 0 ? dcnet::probePba("dcnet.flyca.st") : dcnet::probeSegaRally("dcnet.flyca.st");
					return ServiceResult{responding, Clock::now()};
				});
			} catch (const std::exception&) {
				check.responding = false;
				check.checked = {};
				check.nextCheck = Clock::now() + serviceRefreshInterval;
			}
		}
	}
}

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
	ownedGames.clear();
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
				auto& owned = ownedGames[game.titleId()];
				if (std::none_of(owned.copies.begin(), owned.copies.end(), [&](const GameMedia& copy) {
					return copy.path == media.path;
				}))
					owned.copies.push_back(media);
				if (owned.cover.empty())
					owned.cover = std::move(cover);
			}
		}
	}
}

std::string downloadCover(const std::string& url, const std::string& path)
{
	std::vector<u8> body;
	if (http::get(url, body, nullptr, nullptr, 10000) != 200 || body.empty() || body.size() > 2 * 1024 * 1024)
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
	const int code = http::get(std::string("https://dcnet.flyca.st/status/api/") + endpoint, body, &headers, nullptr, 10000);
	if (code != 200)
		throw std::runtime_error("DCnet is unavailable (HTTP " + std::to_string(code) + ").");
	return std::string(body.begin(), body.end());
}

void refreshStatus()
{
	if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			auto result = pending.get();
			status = std::move(result.value);
			nextLibraryRefresh = {};
			hasStatus = true;
			lastSuccess = result.fetched;
			error.clear();
		} catch (const std::exception&) {
			// Keep the last complete snapshot, but never present it as current data.
			error = T("Unable to refresh DCnet. Check your connection and try again.");
		}
		nextRefresh = error.empty() ? lastSuccess + refreshInterval : Clock::now() + refreshInterval;
	}
	if (!pending.valid() && Clock::now() >= nextRefresh)
	{
		try {
			pending = std::async(std::launch::async, [] {
				const std::string games = fetchStatus("games");
				const std::string players = fetchStatus("players");
				return StatusResult{dcnet::parseStatus(games, players), Clock::now()};
			});
		} catch (const std::exception&) {
			error = T("Unable to start a DCnet refresh. Please try again.");
			nextRefresh = Clock::now() + refreshInterval;
		}
	}
}

void renderCount(int count, bool shared = false)
{
	if (count < 0)
		ImGui::TextDisabled("%s", shared ? "-*" : "-");
	else
		ImGui::Text("%d%s", count, shared ? "*" : "");
	if (shared && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("Shared service count. Includes both editions and is counted once in totals."));
}

std::string displayGameName(const dcnet::Game& game)
{
	if (game.stage == dcnet::ReleaseStage::Alpha)
		return game.name + " [" + T("Alpha") + "]";
	if (game.stage == dcnet::ReleaseStage::Beta)
		return game.name + " [" + T("Beta") + "]";
	return game.name;
}

void renderCover(const dcnet::Game& game, const std::string& localCover, const ImVec2& bounds)
{
	std::string path = localCover;
	if (path.empty() && ImGui::IsRectVisible(bounds))
		path = remoteCover(game.thumbnailUrl);
	ImguiFileTexture cover(path, true);
	const ImTextureID texture = path.empty() ? ImTextureID() : cover.getId();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	if (texture != ImTextureID())
	{
		const float aspect = imguiDriver->getAspectRatio(texture);
		ImVec2 size = bounds;
		if (aspect > 0)
		{
			if (aspect > size.x / size.y)
				size.y = size.x / aspect;
			else
				size.x = size.y * aspect;
		}
		cover.draw(ImGui::GetWindowDrawList(), ImVec2(pos.x + (bounds.x - size.x) / 2, pos.y), size, 1.f);
	}
	else
	{
		ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + bounds.x, pos.y + bounds.y),
				ImGui::GetColorU32(ImGuiCol_FrameBg), uiScaled(6));
		const ImVec2 iconSize = ImGui::CalcTextSize(ICON_FA_GAMEPAD);
		ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + (bounds.x - iconSize.x) / 2,
				pos.y + (bounds.y - iconSize.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_GAMEPAD);
	}
	ImGui::Dummy(bounds);
}

void renderServerStatus(const dcnet::Game& game)
{
	const auto check = std::find_if(serviceChecks.begin(), serviceChecks.end(), [&](const ServiceCheck& item) {
		return game.titleId() == item.gameId;
	});
	const bool checked = check != serviceChecks.end() && check->checked != Clock::time_point{}
			&& Clock::now() - check->checked < serviceExpiry;
	const bool responding = checked && check->responding;
	const bool live = hasFreshStatus() && game.hasLiveStatus;
	const bool saved = game.hasLiveStatus && !live;
	const bool active = hasFreshStatus() && game.isActive();
	const char *label = live ? (game.online ? (active ? T("Playing") : T("Online")) : T("Offline"))
			: active ? T("Playing") : responding ? T("Responding") : saved ? T("Saved") : checked ? T("No response")
			: check != serviceChecks.end() && check->pending.valid() ? T("Checking...") : T("No report");
	const ImVec4 color = live && !game.online ? ImVec4(0.93f, 0.43f, 0.40f, 1.f)
			: active ? ImVec4(0.39f, 0.78f, 0.98f, 1.f)
			: live || responding ? ImVec4(0.40f, 0.80f, 0.59f, 1.f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 padding = ScaledVec2(8, 4);
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const ImVec2 size(textSize.x + padding.x * 2, textSize.y + padding.y * 2);
	ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
			ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.14f)), size.y / 2);
	ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), ImGui::GetColorU32(color), label);
	ImGui::Dummy(size);
	if (ImGui::IsItemHovered())
	{
		if (saved && !responding)
			ImGui::SetTooltip("%s", T("Saved DCnet status. Waiting for a fresh activity report."));
		else if (live)
			ImGui::SetTooltip("%s", T("Server status reported by DCnet."));
		else if (check != serviceChecks.end())
		{
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(game.titleId() == "catalog:pba-bowling-2001"
					? T("PBA lobby query through the DCnet VPN.") : T("Sega Rally 2 initialization handshake with DCnet."));
			ImGui::TextUnformatted(T("Availability only. Player and match counts are not published."));
			if (checked)
				ImGui::Text(T("Checked %lld seconds ago. Refreshes every 2 minutes."),
						static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - check->checked).count()));
			if (!responding)
				ImGui::TextUnformatted(T("No response does not necessarily mean the server is offline."));
			ImGui::EndTooltip();
		}
		else
			ImGui::SetTooltip("%s", T("DCnet does not publish server status for this title. Online play may still be supported."));
	}
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
	refreshServices();
	refreshLibrary(scanner, boxart);
	if (pendingCover.valid() && pendingCover.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			covers[pendingCoverUrl].ready = !pendingCover.get().empty();
		} catch (const std::exception&) {
		}
		pendingCoverUrl.clear();
	}
	// Limit very wide windows to a readable dashboard while retaining narrow layouts.
	const float availableWidth = ImGui::GetContentRegionAvail().x;
	const float pageWidth = std::min(availableWidth, uiScaled(1280));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availableWidth - pageWidth) / 2);
	if (!ImGui::BeginChild("dcnetPage", ImVec2(pageWidth, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground))
	{
		ImGui::EndChild();
		return;
	}
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, uiScaled(10));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(6));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 6));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(10, 8));
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ScaledVec2(10, 10));
	ImVec4 panelColor = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
	panelColor.w = 0.30f;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, panelColor);
	std::string launchPath;
	std::string launchTitle;
	if (ImGui::BeginTable("dcnetHeader", pageWidth >= uiScaled(700) ? 2 : 1, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableNextColumn();
		ImGui::PushFont(largeFont, uiScaled(30));
		ImGui::TextUnformatted("DCnet");
		ImGui::PopFont();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", T("Find a game. Meet other players. Jump in."));
		ImGui::PopStyleColor();
		ImGui::TableNextColumn();
		{
			DisabledScope disabled(pending.valid());
			if (ImGui::Button(pending.valid() ? T("Refreshing...") : T("Refresh")))
			{
				nextRefresh = {};
				for (auto& check : serviceChecks)
					check.nextCheck = {};
				nextLibraryRefresh = {};
				for (auto& cover : covers)
					if (!cover.second.ready && cover.first != pendingCoverUrl)
						cover.second.requested = false;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("Network settings")))
			GuiMenu::openNetworkSettings();
		ImGui::EndTable();
	}
	const std::pair<const char *, std::string> metrics[] = {
		{ T("Players online"), hasStatus ? std::to_string(status.playerCount) : "-" },
		{ T("Active games"), hasStatus ? std::to_string(status.activeGames) : "-" },
		{ T("Lobbies / matches"), hasStatus ? std::to_string(status.matchCount) : "-" },
		{ T("Your games"), std::to_string(ownedGames.size()) + " / " + std::to_string(status.games.size()) },
	};
	if (ImGui::BeginTable("dcnetSummary", pageWidth >= uiScaled(600) ? 4 : 2, ImGuiTableFlags_SizingStretchSame))
	{
		for (const auto& metric : metrics)
		{
			ImGui::TableNextColumn();
			ImguiID id(metric.first);
			if (ImGui::BeginChild("metric", ImVec2(0, uiScaled(70)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding))
			{
				ImGui::TextDisabled("%s", metric.first);
				ImGui::PushFont(largeFont, uiScaled(24));
				ImGui::TextUnformatted(metric.second.c_str());
				ImGui::PopFont();
			}
			ImGui::EndChild();
		}
		ImGui::EndTable();
	}
	const bool inlineFilters = pageWidth >= uiScaled(900);
	const float controlsHeight = ImGui::GetFrameHeightWithSpacing() * (inlineFilters ? 1 : pageWidth >= uiScaled(400) ? 2 : 3)
			+ ImGui::GetStyle().WindowPadding.y * 2;
	if (ImGui::BeginChild("dcnetFilters", ImVec2(0, controlsHeight), ImGuiChildFlags_AlwaysUseWindowPadding))
	{
		ImGui::SetNextItemWidth(inlineFilters ? ImGui::GetContentRegionAvail().x * 0.46f : -1.f);
		if (ImGui::InputTextWithHint("##dcnetSearch", T("Search games or players..."), filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf)))
			filter.Build();
		if (inlineFilters)
			ImGui::SameLine();
		ImGui::Checkbox(T("Active games only"), &activeOnly);
		if (ImGui::GetContentRegionAvail().x > uiScaled(360))
			ImGui::SameLine();
		ImGui::Checkbox(T("Owned games only"), &ownedOnly);
	}
	ImGui::EndChild();
	if (!error.empty())
		ImGui::TextWrapped("%s", error.c_str());
	if (hasStatus)
	{
		const auto age = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - lastSuccess).count();
		ImGui::TextDisabled(hasFreshStatus() ? T("Updated %llds ago") : T("Saved activity from %llds ago"), static_cast<long long>(age));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T("Refreshes every 15 seconds while DCnet is open."));
	}
	else if (error.empty())
		ImGui::TextDisabled("%s", T("Connecting to DCnet..."));
	ImGui::SameLine();
	ImGui::TextDisabled("%s", T("Status guide"));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("A dash means no count is available. * Shared counts include both editions and count once in totals. No report does not mean online play is unsupported."));

	if (ImGui::BeginChild("dcnetActivity", ImVec2(0, 0),
			ImGuiChildFlags_NavFlattened | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_DragScrolling))
	{
		if (ImGui::BeginTabBar("dcnetViews"))
		{
			if (ImGui::BeginTabItem(T("Games")))
			{
				bool any = false;
				bool openDetails = false;
				const bool compact = ImGui::GetContentRegionAvail().x < uiScaled(680);
				if (ImGui::BeginTable("dcnetGames", compact ? 3 : 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("##cover", ImGuiTableColumnFlags_WidthFixed, uiScaled(48));
					ImGui::TableSetupColumn(T("Game"), ImGuiTableColumnFlags_WidthStretch);
					if (!compact)
					{
						ImGui::TableSetupColumn(T("Players"), ImGuiTableColumnFlags_WidthFixed, uiScaled(64));
						ImGui::TableSetupColumn(T("Matches"), ImGuiTableColumnFlags_WidthFixed, uiScaled(80));
					}
					ImGui::TableSetupColumn(T("Activity"), ImGuiTableColumnFlags_WidthFixed, uiScaled(110));
					ImGui::TableHeadersRow();
					for (const auto& game : status.games)
					{
						const auto owned = ownedGames.find(game.titleId());
						if ((activeOnly && !game.isActive())
								|| (ownedOnly && owned == ownedGames.end()) || !filter.PassFilter(game.name.c_str()))
							continue;
						any = true;
						ImguiID id(game.titleId().c_str());
						const ImVec2 coverSize = ScaledVec2(48, 60);
						ImGui::TableNextRow(ImGuiTableRowFlags_None, coverSize.y + ImGui::GetStyle().CellPadding.y * 2);
						ImGui::TableNextColumn();
						renderCover(game, owned == ownedGames.end() ? std::string() : owned->second.cover, coverSize);
						ImGui::TableNextColumn();
						std::string subtitle = owned == ownedGames.end() ? (game.id == "vf4" ? T("NAOMI") : T("Dreamcast")) : T("In your library");
						if (game.stage != dcnet::ReleaseStage::Unspecified)
							subtitle += std::string(" / ") + (game.stage == dcnet::ReleaseStage::Alpha ? T("Alpha") : T("Beta"));
						if (!game.sharedStatusId.empty())
							subtitle += std::string(" / ") + T("Shared service");
						const ImVec2 labelPos = ImGui::GetCursorPos();
						const float textWidth = ImGui::GetContentRegionAvail().x;
						const float rowHeight = std::max(coverSize.y,
								ImGui::CalcTextSize(game.name.c_str(), nullptr, false, textWidth).y
								+ ImGui::CalcTextSize(subtitle.c_str(), nullptr, false, textWidth).y + ImGui::GetStyle().ItemSpacing.y);
						if (ImGui::Selectable("##game", selectedGame == game.titleId(), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight)))
						{
							selectedGame = game.titleId();
							selectedCopy = 0;
							openDetails = true;
						}
						ImGui::SetCursorPos(labelPos);
						ImGui::TextWrapped("%s", game.name.c_str());
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
						ImGui::TextWrapped("%s", subtitle.c_str());
						ImGui::PopStyleColor();
						if (!compact)
						{
							ImGui::TableNextColumn();
							renderCount(game.players, !game.sharedStatusId.empty());
							ImGui::TableNextColumn();
							renderCount(game.matches, !game.sharedStatusId.empty());
						}
						ImGui::TableNextColumn();
						renderServerStatus(game);
						if (compact)
						{
							renderCount(game.players, !game.sharedStatusId.empty());
							ImGui::SameLine();
							ImGui::TextDisabled("%s", T("players"));
						}
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
						return game.titleId() == selectedGame;
					});
					const auto owned = selected == status.games.end() ? ownedGames.end() : ownedGames.find(selected->titleId());
					if (owned != ownedGames.end() && selectedCopy >= owned->second.copies.size())
						selectedCopy = 0;
					if (ImGui::BeginChild("gameDetails", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
							ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling))
					{
						if (selected != status.games.end())
						{
							if (ImGui::BeginTable("gameHeading", 2, ImGuiTableFlags_SizingStretchProp))
							{
								ImGui::TableSetupColumn("##art", ImGuiTableColumnFlags_WidthFixed, uiScaled(84));
								ImGui::TableSetupColumn("##title", ImGuiTableColumnFlags_WidthStretch);
								ImGui::TableNextColumn();
								renderCover(*selected, owned == ownedGames.end() ? std::string() : owned->second.cover, ScaledVec2(84, 108));
								ImGui::TableNextColumn();
								ImGui::PushFont(largeFont, uiScaled(22));
								ImGui::TextWrapped("%s", displayGameName(*selected).c_str());
								ImGui::PopFont();
								renderServerStatus(*selected);
								ImGui::EndTable();
							}
							if (!selected->sharedStatusId.empty())
								ImGui::TextWrapped("%s", T("Shared service: counts and the player list include both editions. Totals count this service once."));
							ImGui::Text("%s:", T("Players"));
							ImGui::SameLine();
							renderCount(selected->players, !selected->sharedStatusId.empty());
							ImGui::Text("%s:", T("Lobbies / matches"));
							ImGui::SameLine();
							renderCount(selected->matches, !selected->sharedStatusId.empty());
							if (owned != ownedGames.end() && !owned->second.copies.empty())
							{
								ImGui::SeparatorText(T("Your copy"));
								if (owned->second.copies.size() > 1)
								{
									ImGui::SetNextItemWidth(-1.f);
									if (ImGui::BeginCombo("##gameCopy", owned->second.copies[selectedCopy].fileName.c_str()))
									{
										for (size_t i = 0; i < owned->second.copies.size(); ++i)
										{
											ImguiID copyId(owned->second.copies[i].path);
											if (ImGui::Selectable(owned->second.copies[i].fileName.c_str(), selectedCopy == i))
												selectedCopy = i;
											if (ImGui::IsItemHovered())
												ImGui::SetTooltip("%s", owned->second.copies[i].path.c_str());
											if (selectedCopy == i)
												ImGui::SetItemDefaultFocus();
										}
										ImGui::EndCombo();
									}
								}
								else
									ImGui::TextWrapped("%s", owned->second.copies.front().fileName.c_str());
								ImGui::TextWrapped("%s", config::UseDCNet ? T("Launch your copy, then open the game's online mode.")
										: T("Enable DCnet in Network settings to use online play."));
							}
							if (!error.empty())
								ImGui::TextWrapped("%s", error.c_str());
							ImGui::SeparatorText(T("Players"));
							renderPlayers(selected->sharedStatusId.empty() ? selected->id : selected->sharedStatusId);
						}
						else
							ImGui::TextWrapped("%s", T("This game is no longer reported by DCnet."));
						scrollWhenDraggingOnVoid();
						windowDragScroll();
					}
					ImGui::EndChild();
					if (owned != ownedGames.end() && !owned->second.copies.empty())
					{
						const float launchWidth = std::max(uiScaled(100), ImGui::GetContentRegionAvail().x - uiScaled(80) - ImGui::GetStyle().ItemSpacing.x);
						const std::string label = std::string(ICON_FA_PLAY) + " " + T("Launch game");
						if (ImGui::Button(label.c_str(), ImVec2(launchWidth, 0)))
						{
							// Copy the path before leaving this frame; refreshing the scanner
							// or starting the loader can invalidate library-owned data.
							launchPath = owned->second.copies[selectedCopy].path;
							launchTitle = selected->name;
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
					}
					if (ImGui::Button(T("Close"), ImVec2(uiScaled(80), 0)))
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
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(5);
	ImGui::EndChild();
	// Launch only after all ImGui child windows and style scopes have closed.
	if (!launchPath.empty())
	{
		settings.content.title = std::move(launchTitle);
		gui_start_game(launchPath);
	}
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
	for (auto& check : serviceChecks)
	{
		if (check.pending.valid())
			check.pending.wait();
		check.pending = {};
		check.checked = {};
		check.nextCheck = {};
		check.responding = false;
	}
	covers.clear();
	pendingCoverUrl.clear();
	ownedGames.clear();
	nextLibraryRefresh = {};
	status = {};
	hasStatus = false;
	error.clear();
	selectedGame.clear();
	selectedCopy = 0;
	nextRefresh = {};
}
