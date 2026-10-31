// Portions Copyright 2026 The Hollycast Authors
#include "gui_dcnet.h"
#include "dcnet_status.h"
#include "gui_util.h"
#include "gui_font.h"
#include "gui_menu.h"
#include "oslib/http_client.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

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

void gui_display_dcnet()
{
	if (status.games.empty())
		status = dcnet::parseStatus("[]", "[]");
	refreshStatus();
	ImGui::PushFont(largeFont, uiLargeFontSize());
	ImGui::TextUnformatted("DCnet");
	ImGui::PopFont();
	ImGui::TextWrapped("%s", T("Find Dreamcast players and see which games are active."));
	{
		DisabledScope disabled(pending.valid());
		if (ImGui::Button(pending.valid() ? T("Refreshing...") : T("Refresh")))
			nextRefresh = Clock::time_point();
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
	ImGui::Text(T("Games in catalog: %zu"), status.games.size());
	const float searchWidth = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(T("Search")).x
			- ImGui::GetStyle().ItemInnerSpacing.x;
	filter.Draw(T("Search"), std::clamp(searchWidth, uiScaled(60), uiScaled(320)));
	ImGui::Checkbox(T("Active games only"), &activeOnly);
	ImGui::TextWrapped("%s", T("DCnet reports lobby / match counts only. A dash means the count is unavailable."));

	if (ImGui::BeginChild("dcnetActivity", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling))
	{
		if (ImGui::BeginTabBar("dcnetViews"))
		{
			if (ImGui::BeginTabItem(T("Games")))
			{
				bool any = false;
				bool openDetails = false;
				if (ImGui::BeginTable("dcnetGames", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn(T("Game"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
					ImGui::TableSetupColumn(T("Players"));
					ImGui::TableSetupColumn(T("Lobbies / matches"));
					ImGui::TableSetupColumn(T("Server"));
					ImGui::TableHeadersRow();
					for (const auto& game : status.games)
					{
						if ((activeOnly && game.players <= 0 && game.matches <= 0) || !filter.PassFilter(game.name.c_str()))
							continue;
						any = true;
						ImguiID id(game.id.c_str());
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						const std::string title = displayGameName(game);
						const ImVec2 labelPos = ImGui::GetCursorPos();
						const float rowHeight = std::max(ImGui::GetTextLineHeight(),
								ImGui::CalcTextSize(title.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x).y);
						if (ImGui::Selectable("##game", selectedGame == game.id, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, rowHeight)))
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
						ImGui::TextWrapped("%s", !game.hasLiveStatus ? T("Not reported")
								: game.online ? T("Online") : T("Offline"));
					}
					ImGui::EndTable();
				}
				if (!any)
					ImGui::TextWrapped("%s", activeOnly ? T("No active games reported right now.") : T("No games match your search."));
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
							ImGui::TextUnformatted(!selected->hasLiveStatus ? T("Live activity is not reported for this title.")
									: selected->online ? T("Server online") : T("Server offline"));
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
	status = {};
	hasStatus = false;
	error.clear();
	selectedGame.clear();
	nextRefresh = {};
}
