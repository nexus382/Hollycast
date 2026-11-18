// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_browser.h"
#include "theme_repository.h"
#include "theme.h"
#include "gui_util.h"
#include "imgui_driver.h"
#include "imgui_stdlib.h"
#include "cfg/option.h"
#include "oslib/i18n.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <future>
#include <map>

namespace themes
{
using i18n::T;
namespace
{
constexpr const char* catalogUrl = "https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/main/catalog.json";
struct Preview { std::string path, error; };
struct Result { std::vector<repository::Theme> themes; std::string installed; };
struct Browser
{
	bool requested = false, loaded = false, onlyNew = false;
	std::string search, error, message, installing;
	std::vector<repository::Theme> themes;
	std::future<Result> work;
	std::future<Preview> imageWork;
	std::string imageUrl;
	std::map<std::string, Preview> images;
	std::map<std::string, size_t> screenshot;
	std::filesystem::path cache;
	size_t imageSerial = 0;
	~Browser()
	{
		stop();
	}
	void stop()
	{
		if (work.valid()) work.wait();
		if (imageWork.valid()) imageWork.wait();
		work = {}; imageWork = {};
		std::error_code ec;
		if (!cache.empty()) std::filesystem::remove_all(cache, ec);
		cache.clear(); images.clear(); loaded = false;
	}
	void refresh()
	{
		error.clear(); message.clear(); installing.clear();
		images.clear();
		work = std::async(std::launch::async, [] { return Result{repository::fetch(catalogUrl), {}}; });
	}
	void poll()
	{
		if (work.valid() && work.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			try
			{
				Result result = work.get();
				if (!result.installed.empty())
				{
					themes::reload();
					message = T("Theme installed. Choose Use theme to apply it.");
				}
				else { themes = std::move(result.themes); loaded = true; }
			}
			catch (const std::exception& e) { error = e.what(); }
			installing.clear();
		}
		if (imageWork.valid() && imageWork.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			images[imageUrl] = imageWork.get();
	}
	const Preview* image(const std::string& url)
	{
		const auto it = images.find(url);
		if (it != images.end()) return &it->second;
		if (!imageWork.valid())
		{
			imageUrl = url;
			try
			{
				if (cache.empty())
				{
					const auto root = std::filesystem::u8path(themes::directory());
					std::filesystem::create_directories(root);
					cache = root / (".previews-"
						+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
					if (!std::filesystem::create_directory(cache)) throw std::runtime_error("Cannot create screenshot cache.");
				}
				const std::string path = (cache / (std::to_string(imageSerial++) + ".image")).u8string();
				imageWork = std::async(std::launch::async, [url, path] {
					try { repository::preview(url, path); return Preview{path, {}}; }
					catch (const std::exception& e) { return Preview{{}, e.what()}; }
				});
			}
			catch (const std::exception& e) { images[url] = Preview{{}, e.what()}; }
		}
		return nullptr;
	}
};
Browser browser;

bool matches(std::string value, std::string query)
{
	const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
	std::transform(value.begin(), value.end(), value.begin(), lower);
	std::transform(query.begin(), query.end(), query.begin(), lower);
	return value.find(query) != std::string::npos;
}

void screenshot(const repository::Theme& theme)
{
	size_t& index = browser.screenshot[theme.id];
	index = std::min(index, theme.screenshots.size() - 1);
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 size(width, width * 9.f / 16.f);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const Preview* preview = ImGui::IsRectVisible(size) ? browser.image(theme.screenshots[index]) : nullptr;
	ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
	ImGui::Dummy(size);
	if (preview && !preview->path.empty() && imguiDriver)
	{
		ImguiFileTexture texture(preview->path);
		const auto id = texture.getId();
		if (id != ImTextureID{})
		{
			const float aspect = std::max(.001f, imguiDriver->getAspectRatio(id));
			const float w = std::min(size.x, size.y * aspect), h = w / aspect;
			const ImVec2 start(pos.x + (size.x - w) / 2, pos.y + (size.y - h) / 2);
			ImGui::GetWindowDrawList()->AddImage(id, start, ImVec2(start.x + w, start.y + h));
		}
	}
	else
	{
		const char* label = preview && !preview->error.empty() ? T("Preview unavailable") : T("Loading preview...");
		ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + uiScaled(12), pos.y + size.y / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
		if (preview && !preview->error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preview->error.c_str());
	}
	if (theme.screenshots.size() > 1)
	{
		if (ImGui::SmallButton(T("Previous image"))) index = (index + theme.screenshots.size() - 1) % theme.screenshots.size();
		ImGui::SameLine();
		ImGui::Text("%zu / %zu", index + 1, theme.screenshots.size());
		if (ImGui::SmallButton(T("Next image"))) index = (index + 1) % theme.screenshots.size();
	}
}
}

void openBrowser() { browser.requested = true; }
void stopBrowser() { browser.stop(); }

std::optional<std::string> renderBrowser()
{
	browser.poll();
	const std::string title = std::string(T("Browse themes")) + "###ThemeBrowser";
	if (browser.requested)
	{
		browser.requested = false;
		ImGui::OpenPopup(title.c_str());
		if (!browser.loaded && !browser.work.valid()) browser.refresh();
	}
	std::optional<std::string> chosen;
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2(std::min(uiScaled(1120), display.x * .95f), display.y * .9f), ImGuiCond_Always);
	centerNextWindow();
	if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) return chosen;
	ImGui::TextWrapped("%s", T("Find a new look for your library. Download a theme, then choose when to use it."));
	const bool busy = browser.work.valid();
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##ThemeSearch", T("Search themes or creators"), &browser.search);
	ImGui::Checkbox(T("Not installed"), &browser.onlyNew);
	ImGui::SameLine();
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(T("Refresh"))) browser.refresh();
	ImGui::EndDisabled();
	if (busy) ImGui::TextWrapped("%s", browser.installing.empty() ? T("Loading themes...") : T("Downloading and installing theme..."));
	if (!browser.error.empty()) ImGui::TextWrapped("%s", browser.error.c_str());
	if (!browser.message.empty()) ImGui::TextWrapped("%s", browser.message.c_str());
	const float footer = ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("##ThemeResults", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
	const int columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / uiScaled(300)), 1, 3);
	size_t shown = 0;
	if (ImGui::BeginTable("##Themes", columns, ImGuiTableFlags_SizingStretchSame))
	{
		for (const auto& theme : browser.themes)
		{
			const auto& local = themes::catalog();
			const auto installed = std::find_if(local.begin(), local.end(), [&](const CatalogEntry& item) { return item.id == theme.id; });
			const bool present = installed != local.end();
			if ((browser.onlyNew && present) || !matches(theme.name + " " + theme.creator + " " + theme.description, browser.search)) continue;
			++shown;
			ImGui::TableNextColumn();
			ImGui::PushID(theme.id.c_str());
			ImGui::BeginChild("##ThemeCard", ImVec2(0, 0),
				ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
			screenshot(theme);
			ImGui::TextWrapped("%s", theme.name.c_str());
			ImGui::TextWrapped(T("By %s"), theme.creator.c_str());
			ImGui::TextDisabled(T("Version %s"), theme.version.c_str());
			if (present)
			{
				const bool active = config::CustomTheme.get() == theme.id;
				ImGui::TextUnformatted(active ? T("Current theme") : T("Installed"));
				ImGui::BeginDisabled(active || !installed->error.empty());
				if (ImGui::Button(T("Use theme"))) { chosen = theme.id; ImGui::CloseCurrentPopup(); }
				ImGui::EndDisabled();
				if (!installed->error.empty()) ImGui::TextWrapped("%s", installed->error.c_str());
			}
			else
			{
				ImGui::BeginDisabled(browser.work.valid());
				if (ImGui::Button(T("Download and install")))
				{
					browser.error.clear(); browser.message.clear(); browser.installing = theme.id;
					browser.work = std::async(std::launch::async, [theme] { return Result{{}, repository::install(theme)}; });
				}
				ImGui::EndDisabled();
			}
			ImGui::BeginChild("##Description", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 3), ImGuiChildFlags_NavFlattened);
			ImGui::TextWrapped("%s", theme.description.c_str());
			ImGui::EndChild();
			ImGui::EndChild();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (browser.loaded && shown == 0)
		ImGui::TextWrapped("%s", browser.themes.empty() ? T("No themes have been published yet. Choose Refresh to check again.") : T("No themes match your filters."));
	ImGui::EndChild();
	if (ImGui::Button(T("Close"), ImVec2(uiScaled(100), 0))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
	return chosen;
}
}
