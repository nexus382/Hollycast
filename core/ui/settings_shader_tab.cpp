/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "settings_new.h"
#include "settings_new_internal.h"
#include "shader_preview_layout.h"
#include "gui_menu.h"
#include "app_update_package.h"
#ifdef USE_METAL
#include "rend/metal/metal_api.h"
#endif
#include "custom_theme.h"
#include "theme.h"
#include "theme_browser.h"
#include "gui_util.h"
#ifdef HOLLYCAST_TEST_HARNESS
#include "test_harness.h"
#include "test_harness_ui.h"
#endif
#include "imgui.h"
#include "imgui_stdlib.h"
#include "audio/audiostream.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "video_preset.h"
#include "network/ice.h"
#include "network/ggpo.h"
#include "widgets/widgets.h"
#include "version.h"
#include "wsi/context.h"
#include "input/dreamlink/dreampotato.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/dreamlink/maplelinkregistry.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/pvr/Renderer_if.h"
#ifdef USE_HIKARU
#include "hw/hikaru/frontend.h"
#include "hw/hikaru/roms.h"
#endif
#ifdef USE_DREAMLINK_DEVICES
#include "sdl/dreamlink/dreamlink_sdl_gamepad.h"
#endif
#include "log/LogManager.h"
#include "hw/mem/addrspace.h"
#include "settings.h"
#include "emulator.h"
#include "rewind.h"
#include "gui.h"
#include "boxart/boxart.h"
#include "boxart/screenscraper.h"
#include "boxart/gamesdb.h"
#include "boxart/artwork_adapter.h"
#include "IconsFontAwesome6.h"
#include "mainui.h"
#include "oslib/oslib.h"
#include "oslib/resources.h"
#include "oslib/storage.h"
#include "rend/postprocess/shader_chain.h"
#include "json.hpp"
#include "stdclass.h"
#include "achievements/achievements.h"
#include "vgamepad.h"
#include "rend/vmu_window.h"
#include "network/phone_vmu_backup.h"
#ifdef HAVE_PHONE_VMU
#include "deps/qrcodegen/qrcodegen.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cfloat>
#include <future>
#include <functional>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef __ANDROID__
#if HOST_CPU == CPU_ARM64 && USE_VULKAN
#include "rend/vulkan/adreno.h"
#endif
#endif

extern ImFont *largeFont;
extern ImFont *settingsTitleFont;
extern ImFont *settingsRightValueFont;
extern ImFont *settingsIconFont;
extern ImFont *aboutAsciiFont;

namespace SettingsNew {

struct ShaderBrowserEntry
{
	std::string path;
	std::string displayPath;
	std::string catalogKey;
	std::string searchText;
	postprocess::ShaderFormat format = postprocess::ShaderFormat::Unknown;
};

struct ShaderBrowserMetadata
{
	std::string name;
	std::string description;
	std::string author;
	std::string performance;
	bool mobileRecommended = false;
};

static std::vector<ShaderBrowserEntry> g_shaderBrowserEntries;
static std::unordered_map<std::string, ShaderBrowserMetadata> g_shaderBrowserMetadata;
static std::string g_shaderBrowserDirectory;
static std::string g_shaderBrowserError;
static std::string g_shaderCatalogWarning;
static std::string g_shaderFilter;
static bool g_shaderBrowserInitialized = false;
static std::vector<size_t> g_shaderVisibleEntries;
static std::string g_shaderAppliedFilter;
static uint64_t g_shaderBrowserGeneration = 0;
static uint64_t g_shaderVisibleGeneration = 0;

struct ShaderScanResult
{
	std::string directory;
	std::vector<ShaderBrowserEntry> entries;
	std::string error;
};

static std::future<ShaderScanResult> g_shaderScanFuture;
static bool g_shaderScanPending = false;
static std::string g_shaderPendingDirectory;

struct ShaderPreviewState
{
	bool initialized = false;
	std::vector<std::string> paths;
	std::string technique;
	std::string overrides;
	postprocess::ShaderChain chain;
	std::vector<postprocess::ShaderParameter> defaults;
	std::string error;
	uint64_t activationGeneration = 0;
};

struct ShaderSettingsSnapshot
{
	std::vector<std::string> paths = config::PostProcessShaders.get();
	std::string singlePath = config::PostProcessShader.get();
	std::string technique = config::PostProcessShaderTechnique.get();
	std::string overrides = config::PostProcessShaderParameters.get();

	void restore()
	{
		config::PostProcessShaders.get() = std::move(paths);
		config::PostProcessShader.get() = std::move(singlePath);
		config::PostProcessShaderTechnique.get() = std::move(technique);
		config::PostProcessShaderParameters.get() = std::move(overrides);
	}
};

struct PendingShaderSettings
{
	ShaderSettingsSnapshot previous;
	uint64_t generation;
	bool perGame;
};

static ShaderPreviewState g_shaderPreview;
static std::atomic_bool g_shaderLiveTune { false };
static std::function<void()> g_shaderAction;
static uint64_t g_shaderActionGeneration = 0;
static bool g_shaderActionQueuedLive = false;
static std::optional<PendingShaderSettings> g_pendingShaderSettings;

static void deferShaderSettingsSave(bool defer)
{
	config::PostProcessShaders.deferSave(defer);
	config::PostProcessShader.deferSave(defer);
	config::PostProcessShaderTechnique.deferSave(defer);
	config::PostProcessShaderParameters.deferSave(defer);
}

static void finishPendingShaderSettings()
{
	if (!g_pendingShaderSettings)
		return;
	auto& pending = *g_pendingShaderSettings;
	if (pending.generation != postprocess::shaderChainReloadGeneration()
			|| pending.perGame != config::Settings::instance().hasPerGameConfig()) {
		// Scope changes have already loaded their own settings.
		deferShaderSettingsSave(false);
		g_pendingShaderSettings.reset();
		g_shaderPreview.initialized = false;
		return;
	}
	const auto status = postprocess::getShaderChainStatusSummary();
	const bool acknowledged = status.reloadGeneration == pending.generation;
	if (!acknowledged && g_shaderLiveTune && !gui_is_open())
		return;
	const bool rejected = acknowledged && !status.error.empty() && !status.frameError;
	if (!acknowledged || rejected) {
		pending.previous.restore();
		g_shaderPreview.initialized = false;
		// Reload even when the file path is unchanged. A rejected technique
		// must restore the working shader's controls as well as its settings.
		postprocess::requestShaderChainReload();
	}
	deferShaderSettingsSave(false);
	g_pendingShaderSettings.reset();
	if (rejected)
		gui_error(status.error);
	else if (acknowledged)
		SaveSettings();
}

static bool shaderRendererSupported()
{
	return isOpenGL(config::RendererType) || isVulkan(config::RendererType)
			|| isMetal(config::RendererType)
			|| config::RendererType == RenderType::DirectX11 || config::RendererType == RenderType::DirectX11_OIT;
}

static bool shaderPreviewBlockedByNetwork()
{
	return settings.network.online || ggpo::active();
}

static void queueShaderAction(std::function<void()> action)
{
	if (g_shaderAction || g_pendingShaderSettings)
		return;
	g_shaderAction = std::move(action);
	g_shaderActionGeneration = postprocess::shaderChainReloadGeneration();
	g_shaderActionQueuedLive = g_shaderLiveTune;
	postprocess::prepareShaderCompilation();
}

void processShaderLoading()
{
	// Called at the beginning of the next platform frame, after the previous
	// frame was presented. Never run expensive work merely after ImGui::Render.
	finishPendingShaderSettings();
	postprocess::advanceShaderCompilationFrame();
	if (g_shaderAction) {
		if (g_shaderActionGeneration != postprocess::shaderChainReloadGeneration()
				|| (g_shaderActionQueuedLive && !g_shaderLiveTune)) {
			g_shaderAction = {};
			postprocess::finishShaderCompilation();
			return;
		}
		if (!postprocess::prepareShaderCompilation()) return;
		const uint64_t previousReload = postprocess::shaderChainReloadGeneration();
		auto action = std::move(g_shaderAction);
		g_shaderAction = {};
		try {
			action();
		} catch (const std::exception& error) {
			gui_error(error.what());
		}
		// A live checkbox change is one loading operation through CPU preparation,
		// GPU compilation, and the first rendered preview. Do not reopen the popup
		// between stages or dismiss it as soon as the files have been parsed.
		if (!g_shaderLiveTune || !shaderRendererSupported()
				|| postprocess::shaderChainReloadGeneration() == previousReload)
			postprocess::finishShaderCompilation();
	}
	else if (gui_is_open()) {
		// Pausing can postpone renderer activation. Do not block the settings
		// screen waiting for a pipeline that will compile only after resuming.
		postprocess::finishShaderCompilation();
	}
}

void renderShaderLoadingOverlay()
{
	const bool loading = postprocess::shaderCompilationPending();
	if (!loading && !ImGui::IsPopupOpen("##ShaderCompilation")) return;
	if (loading) ImGui::OpenPopup("##ShaderCompilation");
	const char* title = T("Compiling shader pipeline...");
	const char* detail = T("This may take a few seconds.");
	const ImGuiStyle& style = ImGui::GetStyle();
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float width = std::max(1.f, std::min(display.x - 2.f * style.WindowPadding.x,
			std::max(ImGui::CalcTextSize(title).x, ImGui::CalcTextSize(detail).x) + 2.f * style.WindowPadding.x));
	const float wrapWidth = std::max(1.f, width - 2.f * style.WindowPadding.x);
	const float titleHeight = ImGui::CalcTextSize(title, nullptr, false, wrapWidth).y;
	const float height = titleHeight + ImGui::CalcTextSize(detail, nullptr, false, wrapWidth).y
			+ style.ItemSpacing.y + 2.f * style.WindowPadding.y;
	ImGui::SetNextWindowPos(display * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	// The foreground fallback below shares these dimensions with the modal so
	// its first visible frame does not jump when ImGui finishes measuring it.
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
	if (ImGui::BeginPopupModal("##ShaderCompilation", nullptr, ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		if (loading) {
			ImGui::TextWrapped("%s", title);
			ImGui::TextWrapped("%s", detail);
			if (ImGui::GetCurrentWindow()->Hidden) {
				// ImGui hides even explicitly sized popups on their opening frame.
				// Draw the same notice directly now; waiting for the modal's next
				// frame would let the checkbox appear to freeze before any message.
				ImDrawList* draw = ImGui::GetForegroundDrawList();
				const ImVec2 position = (display - ImVec2(width, height)) * 0.5f;
				draw->AddRectFilled(ImVec2(0, 0), display, ImGui::GetColorU32(ImGuiCol_ModalWindowDimBg));
				draw->AddRectFilled(position, position + ImVec2(width, height), ImGui::GetColorU32(ImGuiCol_PopupBg), style.PopupRounding);
				draw->AddRect(position, position + ImVec2(width, height), ImGui::GetColorU32(ImGuiCol_Border), style.PopupRounding, 0, style.PopupBorderSize);
				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position + style.WindowPadding,
						ImGui::GetColorU32(ImGuiCol_Text), title, nullptr, wrapWidth);
				draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position + style.WindowPadding + ImVec2(0, titleHeight + style.ItemSpacing.y),
						ImGui::GetColorU32(ImGuiCol_Text), detail, nullptr, wrapWidth);
			}
			postprocess::markShaderCompilationDrawn();
		}
		else ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

static std::string configuredShaderDirectory()
{
	return postprocess::shaderLibraryDirectory();
}

static std::string lowercaseAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

static std::string shaderCatalogKey(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	while (path.rfind("./", 0) == 0)
		path.erase(0, 2);
#ifdef _WIN32
	path = lowercaseAscii(std::move(path));
#endif
	return path;
}

static void loadShaderCatalog(const std::string& directory)
{
	constexpr size_t MaxCatalogBytes = 1024 * 1024;
	g_shaderBrowserMetadata.clear();
	g_shaderCatalogWarning.clear();
	const std::string catalogPath = hostfs::storage().getSubPath(directory, "shader-catalog.json");
	if (!hostfs::storage().exists(catalogPath))
		return;

	try {
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(catalogPath, "rb"));
		if (file == nullptr || file->size() < 0 || static_cast<uint64_t>(file->size()) > MaxCatalogBytes) {
			g_shaderCatalogWarning = T("shader-catalog.json is unavailable or exceeds the 1 MiB safety limit.");
			return;
		}
		std::string text(static_cast<size_t>(file->size()), '\0');
		if (!text.empty() && file->read(text.data(), 1, text.size()) != text.size()) {
			g_shaderCatalogWarning = T("shader-catalog.json could not be read.");
			return;
		}
		const nlohmann::json catalog = nlohmann::json::parse(text, nullptr, false);
		if (catalog.is_discarded() || !catalog.is_object() || !catalog.contains("shaders")
				|| !catalog["shaders"].is_object()) {
			g_shaderCatalogWarning = T("shader-catalog.json is malformed.");
			return;
		}
		for (auto entry = catalog["shaders"].begin(); entry != catalog["shaders"].end(); ++entry) {
			if (!entry.value().is_object())
				continue;
			ShaderBrowserMetadata metadata;
			metadata.name = entry.value().value("name", std::string());
			metadata.description = entry.value().value("description", std::string());
			metadata.author = entry.value().value("author", std::string());
			metadata.performance = entry.value().value("performance", std::string());
			metadata.mobileRecommended = entry.value().value("mobile_recommended", false);
			g_shaderBrowserMetadata.emplace(shaderCatalogKey(entry.key()), std::move(metadata));
		}
	}
	catch (const std::exception& exception) {
		g_shaderCatalogWarning = exception.what();
	}
	for (ShaderBrowserEntry& entry : g_shaderBrowserEntries) {
		entry.searchText = lowercaseAscii(entry.displayPath);
		if (const auto metadata = g_shaderBrowserMetadata.find(entry.catalogKey);
				metadata != g_shaderBrowserMetadata.end())
			entry.searchText += '\n' + lowercaseAscii(metadata->second.name) + '\n'
					+ lowercaseAscii(metadata->second.description) + '\n' + lowercaseAscii(metadata->second.author);
	}
}

static std::string shaderDisplayPath(const std::string& root, const std::string& path)
{
	std::string normalizedRoot = root;
	std::string normalizedPath = path;
	std::replace(normalizedRoot.begin(), normalizedRoot.end(), '\\', '/');
	std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
	while (!normalizedRoot.empty() && normalizedRoot.back() == '/')
		normalizedRoot.pop_back();
#ifdef _WIN32
	const bool insideRoot = lowercaseAscii(normalizedPath).compare(0, normalizedRoot.size(), lowercaseAscii(normalizedRoot)) == 0;
#else
	const bool insideRoot = normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) == 0;
#endif
	if (insideRoot && normalizedPath.size() > normalizedRoot.size()
			&& normalizedPath[normalizedRoot.size()] == '/')
		return normalizedPath.substr(normalizedRoot.size() + 1);
	return normalizedPath;
}

static ShaderScanResult scanShaderDirectory(std::string directory)
{
	constexpr size_t MaxDirectories = 4096;
	constexpr size_t MaxFiles = 16384;
	constexpr size_t MaxDepth = 64;
	ShaderScanResult result;
	result.directory = std::move(directory);
	try {
		std::vector<std::pair<std::string, size_t>> pendingDirectories {
			{ result.directory, 0 }
		};
		std::unordered_set<std::string> visitedDirectories;
		while (!pendingDirectories.empty() && result.entries.size() < MaxFiles
				&& visitedDirectories.size() < MaxDirectories) {
			auto [directory, depth] = std::move(pendingDirectories.back());
			pendingDirectories.pop_back();
			if (!visitedDirectories.emplace(directory).second)
				continue;
			for (const hostfs::FileInfo& entry : hostfs::storage().listContent(directory)) {
				if (entry.isDirectory) {
					if (depth < MaxDepth)
						pendingDirectories.emplace_back(entry.path, depth + 1);
					continue;
				}
				const postprocess::ShaderFormat format = postprocess::detectFormat(entry.path);
				if (format == postprocess::ShaderFormat::Unknown)
					continue;
				const std::string displayPath = shaderDisplayPath(result.directory, entry.path);
				result.entries.push_back({ entry.path, displayPath, shaderCatalogKey(displayPath),
						lowercaseAscii(displayPath), format });
				if (result.entries.size() == MaxFiles)
					break;
			}
		}
		if (!pendingDirectories.empty())
			result.error = "The shader scan reached its safety limit. Choose a smaller shader folder.";
		std::sort(result.entries.begin(), result.entries.end(), [](const ShaderBrowserEntry& left,
				const ShaderBrowserEntry& right) {
			return left.searchText < right.searchText;
		});
	}
	catch (const std::exception& exception) {
		result.error = exception.what();
	}
	return result;
}

static void startShaderBrowserScan(const std::string& directory)
{
	g_shaderBrowserDirectory = directory;
	g_shaderBrowserInitialized = true;
	g_shaderBrowserEntries.clear();
	g_shaderVisibleEntries.clear();
	g_shaderBrowserError.clear();
	g_shaderCatalogWarning.clear();
	if (config::PostProcessShaderDirectory.get().empty()
			&& !hostfs::storage().exists(directory) && !make_directory(directory)) {
		g_shaderBrowserError = T("The default Data/shaders folder could not be created.");
		return;
	}
	if (!hostfs::storage().exists(directory)) {
		g_shaderBrowserError = T("The selected shader folder does not exist or is unavailable.");
		return;
	}
	g_shaderScanFuture = std::async(std::launch::async, [directory] {
		return scanShaderDirectory(directory);
	});
}

static void refreshShaderBrowser()
{
	const std::string directory = configuredShaderDirectory();
	if (g_shaderScanFuture.valid()) {
		g_shaderScanPending = true;
		g_shaderPendingDirectory = directory;
		return;
	}
	startShaderBrowserScan(directory);
}

static void pollShaderBrowserScan()
{
	if (!g_shaderScanFuture.valid()
			|| g_shaderScanFuture.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
		return;
	ShaderScanResult result;
	try {
		result = g_shaderScanFuture.get();
	}
	catch (const std::exception& exception) {
		result.directory = g_shaderBrowserDirectory;
		result.error = exception.what();
	}
	if (g_shaderScanPending) {
		g_shaderScanPending = false;
		startShaderBrowserScan(g_shaderPendingDirectory);
		g_shaderPendingDirectory.clear();
		return;
	}
	if (result.directory != g_shaderBrowserDirectory)
		return;
	g_shaderBrowserEntries = std::move(result.entries);
	g_shaderBrowserError = std::move(result.error);
	loadShaderCatalog(g_shaderBrowserDirectory);
	++g_shaderBrowserGeneration;
}

static bool rebuildShaderPreview()
{
	g_shaderPreview.initialized = true;
	g_shaderPreview.paths = postprocess::configuredShaderPaths();
	g_shaderPreview.technique = config::PostProcessShaderTechnique.get();
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	g_shaderPreview.chain = {};
	g_shaderPreview.defaults.clear();
	g_shaderPreview.error.clear();
	if (g_shaderPreview.paths.empty())
		return true;

	postprocess::ShaderChain candidate;
	if (!postprocess::loadShaderChain(g_shaderPreview.paths, candidate, g_shaderPreview.error)
			|| !postprocess::preflightShaderAssets(candidate, g_shaderPreview.error))
		return false;
	g_shaderPreview.defaults = candidate.parameters;
	postprocess::applyShaderChainParameterValues(candidate, g_shaderPreview.overrides);
	g_shaderPreview.chain = std::move(candidate);
	return true;
}

static void ensureShaderPreview()
{
	const std::vector<std::string> paths = postprocess::configuredShaderPaths();
	if (!g_shaderPreview.initialized || paths != g_shaderPreview.paths
			|| config::PostProcessShaderTechnique.get() != g_shaderPreview.technique
			|| config::PostProcessShaderParameters.get() != g_shaderPreview.overrides) {
		if (paths.empty()) {
			rebuildShaderPreview();
			return;
		}
		queueShaderAction([] {
			rebuildShaderPreview();
			// Merely inspecting controls does not need CPU image copies afterward.
			for (auto& texture : g_shaderPreview.chain.textures) texture.decoded.reset();
		});
	}
}

static void requestShaderActivation()
{
	g_shaderPreview.activationGeneration =
			postprocess::requestShaderChainReload(g_shaderPreview.error.empty() ? &g_shaderPreview.chain : nullptr);
	for (auto& texture : g_shaderPreview.chain.textures) texture.decoded.reset();
}

static bool shaderActivationPending()
{
	auto& generation = g_shaderPreview.activationGeneration;
	if (generation != 0 && (postprocess::shaderChainReloadGeneration() != generation
			|| postprocess::getShaderChainStatusSummary().reloadGeneration == generation))
		generation = 0;
	return generation != 0;
}

static void applyShaderSettings(ShaderSettingsSnapshot previous)
{
	requestShaderActivation();
	if (g_shaderLiveTune && !g_shaderPreview.paths.empty()) {
		// A settings save from another panel must also skip the untested choice.
		deferShaderSettingsSave(true);
		g_pendingShaderSettings = PendingShaderSettings { std::move(previous), g_shaderPreview.activationGeneration,
				config::Settings::instance().hasPerGameConfig() };
	} else {
		// Clearing effects needs no GPU validation. Paused settings also work
		// before a title is running, so they retain their ordinary save behavior.
		SaveSettings();
	}
}

static bool previewMatchesActiveShader()
{
	if (shaderActivationPending() || g_shaderPreview.paths.empty())
		return false;
	const postprocess::ShaderChainStatusSummary status = postprocess::getShaderChainStatusSummary();
	return (status.error.empty() || status.frameError) && !status.activePath.empty()
			&& status.activePath == postprocess::shaderSelectionLabel(g_shaderPreview.paths)
			&& status.parameterCount == g_shaderPreview.chain.parameters.size();
}

static void shaderTextWrapped(const ImVec4& color, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextWrappedV(format, arguments);
	ImGui::PopStyleColor();
	va_end(arguments);
}

static bool setPreviewParameter(const std::string& name, const float *values, uint32_t componentCount)
{
	if (values == nullptr || componentCount == 0 || componentCount > 16)
		return false;
	auto parameter = std::find_if(g_shaderPreview.chain.parameters.begin(), g_shaderPreview.chain.parameters.end(),
			[&](const postprocess::ShaderParameter& candidate) {
				return postprocess::shaderParameterKey(candidate) == name;
			});
	if (parameter == g_shaderPreview.chain.parameters.end() || parameter->componentCount != componentCount)
		return false;
	for (uint32_t component = 0; component < componentCount; ++component) {
		const float minimum = componentCount == 1 ? parameter->minimum : parameter->minimumValues[component];
		const float maximum = componentCount == 1 ? parameter->maximum : parameter->maximumValues[component];
		if (!std::isfinite(values[component])
				|| (minimum < maximum && (values[component] < minimum || values[component] > maximum)))
			return false;
	}

	std::copy_n(values, componentCount, parameter->values.begin());
	parameter->value = parameter->values[0];
	config::PostProcessShaderParameters.get() =
			postprocess::serializeShaderChainParameterValues(g_shaderPreview.chain.parameters);
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	if (previewMatchesActiveShader()) {
		if (componentCount == 1)
			postprocess::setShaderChainParameter(name, values[0]);
		else
			postprocess::setShaderChainParameterComponents(name, values, componentCount);
	}
	return true;
}

static void resetShaderParameters()
{
	if (g_shaderPreview.defaults.size() != g_shaderPreview.chain.parameters.size())
		return;
	g_shaderPreview.chain.parameters = g_shaderPreview.defaults;
	config::PostProcessShaderParameters.get().clear();
	g_shaderPreview.overrides.clear();
	if (previewMatchesActiveShader()) {
		for (const postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
			if (parameter.componentCount == 1)
				postprocess::setShaderChainParameter(postprocess::shaderParameterKey(parameter), parameter.values[0]);
			else
				postprocess::setShaderChainParameterComponents(postprocess::shaderParameterKey(parameter),
						parameter.values.data(), parameter.componentCount);
		}
	}
	SaveSettings();
}

static void renderShaderActivationStatus(bool compact = false)
{
	if (!shaderRendererSupported()) {
		ImGui::TextWrapped("%s", T("Display shaders require OpenGL, Vulkan, DirectX 11, or Metal. Select a supported graphics API in Video settings."));
		return;
	}
	if (g_shaderPreview.paths.empty()) {
		ImGui::TextDisabled("%s", T("No display shader selected."));
		return;
	}
	if (!g_shaderPreview.error.empty()) {
		shaderTextWrapped(ImVec4(1.f, .35f, .35f, 1.f), T("Invalid: %s"), g_shaderPreview.error.c_str());
		return;
	}
	if (shaderActivationPending()) {
		shaderTextWrapped(ImVec4(1.f, .75f, .25f, 1.f), "%s",
				g_shaderLiveTune ? T("Loading on the current graphics API...")
						: T("Ready. Start Live Tune or return to the game to activate it."));
		return;
	}

	const postprocess::ShaderChainStatusSummary status = postprocess::getShaderChainStatusSummary();
	if (!status.error.empty())
		shaderTextWrapped(ImVec4(1.f, .35f, .35f, 1.f), status.frameError ? T("Frame unavailable: %s") : T("Rejected: %s"), status.error.c_str());
	else if (status.activePath == postprocess::shaderSelectionLabel(g_shaderPreview.paths)) {
		if (compact) {
			shaderTextWrapped(ImVec4(.35f, 1.f, .55f, 1.f), T("Active: %u layer(s), %u pass(es)"),
					static_cast<unsigned>(g_shaderPreview.paths.size()), static_cast<unsigned>(g_shaderPreview.chain.passes.size()));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", status.activePath.c_str());
		}
		else
			shaderTextWrapped(ImVec4(.35f, 1.f, .55f, 1.f), T("Active: %s"), status.activePath.c_str());
	}
	else
		shaderTextWrapped(ImVec4(1.f, .75f, .25f, 1.f), "%s", T("Parsed successfully; waiting for gameplay to activate."));
}

static void applyShaderTechniqueSelection(std::string technique)
{
	ShaderSettingsSnapshot previous;
	ShaderPreviewState previousPreview = std::move(g_shaderPreview);
	config::PostProcessShaderTechnique.get() = std::move(technique);
	if (!rebuildShaderPreview()) {
		// Keep the working technique and its controls when a consumer is chosen
		// without the producer layer it requires.
		const std::string error = g_shaderPreview.error;
		previous.restore();
		g_shaderPreview = std::move(previousPreview);
		gui_error(error);
		return;
	}
	applyShaderSettings(std::move(previous));
}

static void renderShaderParameterControls()
{
	for (postprocess::ShaderTechniqueGroup& group : g_shaderPreview.chain.techniqueGroups) {
		if (group.techniques.size() < 2)
			continue;
		const int selectedTechnique = group.selectedTechnique >= 0
				&& group.selectedTechnique < static_cast<int>(group.techniques.size())
				? group.selectedTechnique : 0;
		ImGui::PushID(static_cast<int>(group.layerIndex));
		std::string selection;
		if (ImGui::BeginCombo(group.label.c_str(), group.techniques[selectedTechnique].c_str())) {
			for (size_t index = 0; index < group.techniques.size(); ++index) {
				const bool selected = static_cast<int>(index) == selectedTechnique;
				if (ImGui::Selectable(group.techniques[index].c_str(), selected) && !selected) {
					const int previousIndex = group.selectedTechnique;
					group.selectedTechnique = static_cast<int>(index);
					selection = postprocess::serializeShaderTechniqueSelections(g_shaderPreview.chain.techniqueGroups);
					group.selectedTechnique = previousIndex;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		if (!selection.empty()) {
			queueShaderAction([selection = std::move(selection)] { applyShaderTechniqueSelection(selection); });
			break;
		}
	}
	if (g_shaderPreview.chain.techniqueGroups.empty() && g_shaderPreview.chain.techniques.size() > 1) {
		const int selectedTechnique = g_shaderPreview.chain.selectedTechnique >= 0
				&& g_shaderPreview.chain.selectedTechnique < static_cast<int>(g_shaderPreview.chain.techniques.size())
				? g_shaderPreview.chain.selectedTechnique : 0;
		std::string selection;
		if (ImGui::BeginCombo(T("Technique"), g_shaderPreview.chain.techniques[selectedTechnique].c_str())) {
			for (size_t index = 0; index < g_shaderPreview.chain.techniques.size(); ++index) {
				const bool selected = static_cast<int>(index) == selectedTechnique;
				if (ImGui::Selectable(g_shaderPreview.chain.techniques[index].c_str(), selected) && !selected)
					selection = g_shaderPreview.chain.techniques[index];
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (!selection.empty())
			queueShaderAction([selection = std::move(selection)] { applyShaderTechniqueSelection(selection); });
	}
	if (g_shaderPreview.chain.parameters.empty()) {
		ImGui::TextDisabled("%s", T("This shader stack exposes no adjustable parameters."));
		return;
	}
	if (ImGui::SmallButton(T("Reset All Parameters")))
		resetShaderParameters();
	ImGui::Separator();

	for (postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
		if (!parameter.hostSource.empty())
			continue;
		ImGui::PushID(postprocess::shaderParameterKey(parameter).c_str());
		if (!parameter.key.empty())
			ImGui::TextDisabled(T("Layer %u"), parameter.layerIndex + 1);
		bool changed = false;
		bool saveAfterEdit = false;
		if (parameter.componentCount <= 1) {
			float value = parameter.value;
			if (parameter.type == postprocess::SlangParameterType::Bool) {
				ImGui::TextWrapped("%s", parameter.label.c_str());
				bool enabled = value != 0.f;
				changed = ImGui::Checkbox(T("Enabled"), &enabled);
				value = enabled ? 1.f : 0.f;
				saveAfterEdit = changed;
			}
			else if (!parameter.choiceLabels.empty()) {
				const int selected = static_cast<int>(std::clamp(std::round(static_cast<double>(value)),
						0.0, static_cast<double>(parameter.choiceLabels.size() - 1)));
				ImGui::TextWrapped("%s", parameter.label.c_str());
				ImGui::SetNextItemWidth(-1.f);
				if (ImGui::BeginCombo("##choice", parameter.choiceLabels[selected].c_str())) {
					ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_DragScrolling;
					for (size_t index = 0; index < parameter.choiceLabels.size(); ++index) {
						const bool isSelected = static_cast<int>(index) == selected;
						if (ImGui::Selectable(parameter.choiceLabels[index].c_str(), isSelected) && !isSelected) {
							value = static_cast<float>(index);
							changed = true;
							saveAfterEdit = true;
						}
						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}
					scrollWhenDraggingOnVoid();
					windowDragScroll(false);
					ImGui::EndCombo();
				}
			}
			else if (parameter.minimum < parameter.maximum) {
				const double range = static_cast<double>(parameter.maximum) - parameter.minimum;
				if (parameter.type == postprocess::SlangParameterType::Int) {
					// Shader annotations are external data. Bound the slider before
					// converting to int so large ranges cannot overflow the UI.
					const double minimum = std::ceil(static_cast<double>(parameter.minimum));
					const double maximum = std::max(minimum, std::floor(static_cast<double>(parameter.maximum)));
					const double step = parameter.step > 0.f ? std::max(1.0, std::round(static_cast<double>(parameter.step)))
							: std::max(1.0, std::ceil((maximum - minimum) / 100.0));
					const int stepCount = static_cast<int>(std::clamp(std::ceil((maximum - minimum) / step), 1.0, 1000000.0));
					int stepIndex = static_cast<int>(std::clamp(std::round((value - minimum) / step), 0.0, static_cast<double>(stepCount)));
					ImGui::TextWrapped("%s: %.0f", parameter.label.c_str(), static_cast<double>(value));
					ImGui::SetNextItemWidth(-1.f);
					changed = allowSettingsTouchDrag(ImGui::SliderInt("##value", &stepIndex, 0, stepCount, ""));
					value = static_cast<float>(std::min(maximum, minimum + stepIndex * step));
				}
				else {
					const double step = parameter.step > 0.f ? parameter.step : range / 100.0;
					const int stepCount = static_cast<int>(std::clamp(std::ceil(range / step), 1.0, 1000000.0));
					int stepIndex = static_cast<int>(std::clamp(std::round((value - parameter.minimum) / step), 0.0, static_cast<double>(stepCount)));
					ImGui::TextWrapped("%s: %.3f", parameter.label.c_str(), value);
					ImGui::SetNextItemWidth(-1.f);
					changed = allowSettingsTouchDrag(ImGui::SliderInt("##value", &stepIndex, 0, stepCount, ""));
					value = static_cast<float>(std::clamp(parameter.minimum + stepIndex * step,
							static_cast<double>(parameter.minimum), static_cast<double>(parameter.maximum)));
				}
				saveAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
			}
			else
				ImGui::TextDisabled("%s: %.3f", parameter.label.c_str(), parameter.value);
			if (changed)
				setPreviewParameter(postprocess::shaderParameterKey(parameter), &value, 1);
		}
		else {
			ImGui::TextWrapped("%s", parameter.label.c_str());
			float values[16] {};
			std::copy(parameter.values.begin(), parameter.values.end(), values);
			for (uint32_t component = 0; component < parameter.componentCount; ++component) {
				ImGui::PushID(static_cast<int>(component));
				const float minimum = parameter.minimumValues[component];
				const float maximum = parameter.maximumValues[component];
				const float step = parameter.stepValues[component] > 0.f ? parameter.stepValues[component] : 0.01f;
				ImGui::TextDisabled("%u", component + 1);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.f);
				changed = allowSettingsTouchDrag(ImGui::DragFloat("##component", &values[component],
						step,
						minimum < maximum ? minimum : 0.f, minimum < maximum ? maximum : 0.f)) || changed;
				if (ImGui::IsItemEdited() && minimum < maximum)
					values[component] = std::clamp(minimum + std::round((values[component] - minimum) / step) * step,
							minimum, maximum);
				saveAfterEdit = ImGui::IsItemDeactivatedAfterEdit() || saveAfterEdit;
				ImGui::PopID();
			}
			if (changed)
				setPreviewParameter(postprocess::shaderParameterKey(parameter), values, parameter.componentCount);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::BeginTooltip();
			ImGui::Text("%s", parameter.label.c_str());
			ImGui::TextDisabled("%s", parameter.name.c_str());
			ImGui::EndTooltip();
		}
		if (saveAfterEdit)
			SaveSettings();
		ImGui::PopID();
	}
}

static void applyShaderSelection(const std::vector<std::string>& paths,
		std::vector<size_t> previousLayers = {})
{
	const std::vector<std::string> previousPaths = g_shaderPreview.paths;
	const std::vector<postprocess::ShaderParameter> previousParameters = g_shaderPreview.chain.parameters;
	// Explicit indices preserve identity when two layers use the same effect.
	// Browser changes match each old occurrence once; new layers start at defaults.
	if (previousLayers.size() != paths.size()) {
		previousLayers.assign(paths.size(), previousPaths.size());
		std::vector<bool> used(previousPaths.size(), false);
		for (size_t layer = 0; layer < paths.size(); ++layer)
			for (size_t previous = 0; previous < previousPaths.size(); ++previous)
				if (!used[previous] && previousPaths[previous] == paths[layer]) {
					previousLayers[layer] = previous;
					used[previous] = true;
					break;
				}
	}
	std::vector<postprocess::ShaderTechniqueGroup> previousTechniques = g_shaderPreview.chain.techniqueGroups;
	if (previousTechniques.empty() && previousPaths.size() == 1 && !g_shaderPreview.chain.techniques.empty())
		previousTechniques.push_back({ 0, {}, g_shaderPreview.chain.techniques, g_shaderPreview.chain.selectedTechnique });
	std::vector<postprocess::ShaderTechniqueGroup> techniques;
	for (size_t layer = 0; layer < paths.size(); ++layer)
		for (const auto& previous : previousTechniques)
			if (previous.layerIndex == previousLayers[layer]) {
				techniques.push_back(previous);
				techniques.back().layerIndex = layer;
				break;
			}
	ShaderSettingsSnapshot previousSettings;
	ShaderPreviewState previousPreview = std::move(g_shaderPreview);
	config::PostProcessShaders.get() = paths;
	// Keep old single-path configurations interoperable with older builds.
	config::PostProcessShader.get() = paths.size() == 1 ? paths.front() : std::string();
	if (paths.size() == 1 && !techniques.empty() && techniques.front().selectedTechnique >= 0
			&& techniques.front().selectedTechnique < static_cast<int>(techniques.front().techniques.size()))
		config::PostProcessShaderTechnique.get() = techniques.front().techniques[techniques.front().selectedTechnique];
	else
		config::PostProcessShaderTechnique.get() = postprocess::serializeShaderTechniqueSelections(techniques);
	config::PostProcessShaderParameters.get().clear();
	if (!rebuildShaderPreview()) {
		// An invalid ordering must not erase settings from the working stack.
		const std::string error = g_shaderPreview.error;
		previousSettings.restore();
		g_shaderPreview = std::move(previousPreview);
		gui_error(error);
		return;
	}
	for (postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
		if (!parameter.hostSource.empty())
			continue;
		if (parameter.layerIndex >= previousLayers.size())
			continue;
		const size_t previousLayer = previousLayers[parameter.layerIndex];
		const auto previous = std::find_if(previousParameters.begin(), previousParameters.end(), [&](const auto& value) {
			return value.layerIndex == previousLayer && value.name == parameter.name
					&& value.type == parameter.type && value.componentCount == parameter.componentCount;
		});
		if (previous == previousParameters.end())
			continue;
		for (uint32_t component = 0; component < parameter.componentCount; ++component) {
			const float minimum = parameter.componentCount == 1 ? parameter.minimum : parameter.minimumValues[component];
			const float maximum = parameter.componentCount == 1 ? parameter.maximum : parameter.maximumValues[component];
			parameter.values[component] = minimum < maximum
					? std::clamp(previous->values[component], minimum, maximum) : previous->values[component];
		}
		parameter.value = parameter.values[0];
	}
	config::PostProcessShaderParameters.get() = postprocess::serializeShaderChainParameterValues(g_shaderPreview.chain.parameters);
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	applyShaderSettings(std::move(previousSettings));
}

static void requestShaderSelection(std::vector<std::string> paths, std::vector<size_t> previousLayers = {})
{
	queueShaderAction([paths = std::move(paths), previousLayers = std::move(previousLayers)] {
		applyShaderSelection(paths, previousLayers);
	});
}

static std::string selectedShaderLabel(const std::string& path)
{
	const auto entry = std::find_if(g_shaderBrowserEntries.begin(), g_shaderBrowserEntries.end(),
			[&](const ShaderBrowserEntry& value) { return value.path == path; });
	if (entry == g_shaderBrowserEntries.end())
		return path;
	const auto metadata = g_shaderBrowserMetadata.find(entry->catalogKey);
	return metadata != g_shaderBrowserMetadata.end() && !metadata->second.name.empty()
			? metadata->second.name : entry->displayPath;
}

void renderShaderTab()
{
	ImGui::TextDisabled("%s", T("Shader Configuration"));
	ImGui::Separator();
	ensureShaderPreview();
	if (g_shaderAction) return;
	if (!shaderLiveTuneActive()) {
		const bool online = shaderPreviewBlockedByNetwork();
		const bool canLiveTune = !online && shaderRendererSupported() && game_started
				&& (!emu.running() || achievements::canPause());
		if (!canLiveTune)
			ImGui::BeginDisabled();
		if (ImGui::Button(T("Preview and Adjust"))) {
			applyPendingSettings();
			GamepadDevice::load_system_mappings();
			stopTextureFilterLiveTune();
			g_shaderLiveTune = true;
			gui_resume_game();
		}
		if (!canLiveTune)
			ImGui::EndDisabled();
		if (online)
			ImGui::TextWrapped("%s", T("Live shader preview is unavailable during online play."));
		else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", !shaderRendererSupported() ? T("Select OpenGL, Vulkan, DirectX 11, or Metal in Video settings to preview shaders.")
					: !game_started ? T("Start a game to preview shader changes.")
					: T("See shader changes beside the running game. Gameplay must be pausable."));
		ImGui::Separator();
	}

	const std::string directory = configuredShaderDirectory();
	if (!g_shaderBrowserInitialized || directory != g_shaderBrowserDirectory)
		refreshShaderBrowser();
	pollShaderBrowserScan();

	if (RenderCollapsingHeader("ShaderLibrarySection", ICON_FA_FOLDER_OPEN, T("Shader Library"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T("Hollycast scans this folder recursively for GLSL, GLSLP, Slang, Slang presets, and supported ReShade FX files."));
		ImGui::TextWrapped(T("Folder: %s"), directory.c_str());

		const char *folderPopup = T("Select Shader Folder");
		const auto folderSelected = [](bool cancelled, const std::string& selection) {
			if (!cancelled && !selection.empty()) {
				config::PostProcessShaderDirectory.get() = selection;
				refreshShaderBrowser();
			}
			return true;
		};
		const bool changeFolder = ImGui::Button(T("Change Folder"));
#ifdef __ANDROID__
		if (changeFolder && select_storage_popup(true, false, folderPopup, folderSelected)
				== StoragePopupResult::Unsupported)
			ImGui::OpenPopup(folderPopup);
#else
		if (changeFolder)
			ImGui::OpenPopup(folderPopup);
#endif
		ImGui::SameLine();
		if (ImGui::Button(T("Refresh List")))
			refreshShaderBrowser();
		if (!config::PostProcessShaderDirectory.get().empty()) {
			ImGui::SameLine();
			if (ImGui::Button(T("Use Default Folder"))) {
				config::PostProcessShaderDirectory.get().clear();
				refreshShaderBrowser();
			}
		}
		select_file_popup(folderPopup, folderSelected, false);

		std::vector<std::string> selectedPaths = postprocess::configuredShaderPaths();
		ImGui::Text(T("Recognized: %u    Selected layers: %u / %u"),
				static_cast<unsigned>(g_shaderBrowserEntries.size()), static_cast<unsigned>(selectedPaths.size()),
				static_cast<unsigned>(postprocess::MaxShaderLayers));
		if (!g_shaderBrowserError.empty())
			ImGui::TextColored(ImVec4(1.f, .55f, .25f, 1.f), "%s", g_shaderBrowserError.c_str());
		if (!g_shaderCatalogWarning.empty())
			ImGui::TextColored(ImVec4(1.f, .75f, .25f, 1.f), "%s", g_shaderCatalogWarning.c_str());
		if (g_shaderScanFuture.valid())
			ImGui::TextDisabled("%s", T("Scanning shader folder..."));

		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##ShaderFilter", T("Filter recognized shaders..."), &g_shaderFilter);
		if (g_shaderAppliedFilter != g_shaderFilter || g_shaderVisibleGeneration != g_shaderBrowserGeneration) {
			g_shaderAppliedFilter = g_shaderFilter;
			g_shaderVisibleGeneration = g_shaderBrowserGeneration;
			g_shaderVisibleEntries.clear();
			g_shaderVisibleEntries.reserve(g_shaderBrowserEntries.size());
			const std::string loweredFilter = lowercaseAscii(g_shaderFilter);
			for (size_t index = 0; index < g_shaderBrowserEntries.size(); ++index)
				if (loweredFilter.empty()
						|| g_shaderBrowserEntries[index].searchText.find(loweredFilter) != std::string::npos)
					g_shaderVisibleEntries.push_back(index);
		}

		ImGui::BeginChild("RecognizedShaderList", ImVec2(0.f, SettingsLayoutScaled(260.f)),
				ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_DragScrolling);
		if (g_shaderVisibleEntries.empty())
			ImGui::TextDisabled("%s", g_shaderBrowserEntries.empty()
					? T("No recognized shaders were found in this folder.") : T("No shaders match the current filter."));
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_shaderVisibleEntries.size()));
		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
				const ShaderBrowserEntry& entry = g_shaderBrowserEntries[g_shaderVisibleEntries[row]];
				ImGui::PushID(entry.path.c_str());
				const auto selected = std::find(selectedPaths.begin(), selectedPaths.end(), entry.path);
				bool enabled = selected != selectedPaths.end();
				const bool canEnable = enabled || selectedPaths.size() < postprocess::MaxShaderLayers;
				if (!canEnable)
					ImGui::BeginDisabled();
				if (ImGui::Checkbox("##Selected", &enabled)) {
					if (enabled)
						selectedPaths.push_back(entry.path);
					else
						selectedPaths.erase(std::remove(selectedPaths.begin(), selectedPaths.end(), entry.path), selectedPaths.end());
					requestShaderSelection(selectedPaths);
				}
				if (!canEnable)
					ImGui::EndDisabled();
				ImGui::SameLine();
				const auto metadata = g_shaderBrowserMetadata.find(entry.catalogKey);
				const bool hasDisplayName = metadata != g_shaderBrowserMetadata.end() && !metadata->second.name.empty();
				ImGui::TextUnformatted(hasDisplayName ? metadata->second.name.c_str() : entry.displayPath.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", postprocess::formatName(entry.format));
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					if (hasDisplayName)
						ImGui::TextUnformatted(metadata->second.name.c_str());
					ImGui::TextWrapped("%s", entry.displayPath.c_str());
					ImGui::TextDisabled(T("Format: %s"), postprocess::formatName(entry.format));
					if (metadata != g_shaderBrowserMetadata.end()) {
						if (!metadata->second.description.empty())
							ImGui::TextWrapped("%s", metadata->second.description.c_str());
						if (!metadata->second.author.empty())
							ImGui::TextDisabled(T("Author: %s"), metadata->second.author.c_str());
						if (!metadata->second.performance.empty())
							ImGui::TextDisabled(T("Performance: %s"), metadata->second.performance.c_str());
						if (metadata->second.mobileRecommended)
							ImGui::TextColored(ImVec4(.35f, 1.f, .55f, 1.f), "%s", T("Recommended for mobile GPUs"));
					}
					ImGui::Separator();
					ImGui::TextDisabled("%s", enabled
							? T("Selected. Stack compatibility is shown below.")
							: T("Graphics API compatibility is checked when selected and activated."));
					ImGui::EndTooltip();
				}
				ImGui::PopID();
			}
		}
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			scrollWhenDraggingOnVoid();
#if defined(__ANDROID__)
		windowDragScroll(false);
#else
		windowDragScroll();
#endif
		ImGui::EndChild();
	}

	std::vector<std::string> selectedPaths = postprocess::configuredShaderPaths();
	if (RenderCollapsingHeader("ShaderLayersSection", ICON_FA_WAND_MAGIC, T("Active Shader Layers"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T("Layers run from top to bottom. A preset remains one selectable layer even when it contains several internal passes."));
		if (selectedPaths.size() > 1)
			ImGui::TextDisabled("%s", T("Each layer keeps its own parameter settings."));
		if (selectedPaths.empty())
			ImGui::TextDisabled("%s", T("No shader layers selected."));
		else if (ImGui::BeginTable("SelectedShaderLayers", 4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, SettingsLayoutScaled(28.f));
			ImGui::TableSetupColumn(T("Shader"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("Order"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T("Remove"), ImGuiTableColumnFlags_WidthFixed);
			int moveFrom = -1;
			int moveTo = -1;
			int removeIndex = -1;
			int duplicateIndex = -1;
			for (size_t index = 0; index < selectedPaths.size(); ++index) {
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%u", static_cast<unsigned>(index + 1));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", selectedShaderLabel(selectedPaths[index]).c_str());
				ImGui::TableSetColumnIndex(2);
				if (index == 0)
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Up"))) {
					moveFrom = static_cast<int>(index);
					moveTo = static_cast<int>(index - 1);
				}
				if (index == 0)
					ImGui::EndDisabled();
				ImGui::SameLine();
				if (index + 1 == selectedPaths.size())
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Down"))) {
					moveFrom = static_cast<int>(index);
					moveTo = static_cast<int>(index + 1);
				}
				if (index + 1 == selectedPaths.size())
					ImGui::EndDisabled();
				ImGui::TableSetColumnIndex(3);
				if (selectedPaths.size() >= postprocess::MaxShaderLayers)
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Duplicate")))
					duplicateIndex = static_cast<int>(index);
				if (selectedPaths.size() >= postprocess::MaxShaderLayers)
					ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::SmallButton(T("Remove")))
					removeIndex = static_cast<int>(index);
				ImGui::PopID();
			}
			ImGui::EndTable();
			std::vector<size_t> previousLayers(selectedPaths.size());
			for (size_t layer = 0; layer < previousLayers.size(); ++layer)
				previousLayers[layer] = layer;
			if (removeIndex >= 0) {
				selectedPaths.erase(selectedPaths.begin() + removeIndex);
				previousLayers.erase(previousLayers.begin() + removeIndex);
				requestShaderSelection(selectedPaths, previousLayers);
			}
			else if (moveFrom >= 0 && moveTo >= 0) {
				std::swap(selectedPaths[moveFrom], selectedPaths[moveTo]);
				std::swap(previousLayers[moveFrom], previousLayers[moveTo]);
				requestShaderSelection(selectedPaths, previousLayers);
			}
			else if (duplicateIndex >= 0) {
				selectedPaths.insert(selectedPaths.begin() + duplicateIndex + 1, selectedPaths[duplicateIndex]);
				previousLayers.insert(previousLayers.begin() + duplicateIndex + 1, duplicateIndex);
				requestShaderSelection(selectedPaths, previousLayers);
			}
		}

		if (!selectedPaths.empty()) {
			if (ImGui::Button(T("Reload Active Stack"))) {
				queueShaderAction([] {
					rebuildShaderPreview();
					requestShaderActivation();
				});
			}
			ImGui::SameLine();
			if (ImGui::Button(T("Clear All")))
				requestShaderSelection({});
		}
	}

	if (RenderCollapsingHeader("ShaderStatusSection", ICON_FA_SLIDERS, T("Status and Parameters"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		renderShaderActivationStatus();
		renderShaderParameterControls();
	}
}

void renderShaderLiveTuneOverlay()
{
	if (!g_shaderLiveTune)
		return;
	if (!game_started || shaderPreviewBlockedByNetwork() || !shaderRendererSupported()) {
		closeShaderLiveTune();
		postprocess::finishShaderCompilation();
		return;
	}
	ensureShaderPreview();
	if (g_shaderAction) return;

	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const ShaderPreviewLayout layout = shaderPreviewLayout(static_cast<int>(displaySize.x),
			static_cast<int>(displaySize.y), static_cast<int>(GuiMenu::mainMenuBarHeight()));
	if (layout.controlsWidth <= 0 || layout.controlsHeight <= 0)
		return;
	ImGui::SetNextWindowPos(ImVec2(layout.controlsX, layout.controlsY), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(layout.controlsWidth, layout.controlsHeight), ImGuiCond_Always);
	bool open = true;
	bool returnToSettings = false;
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_DragScrolling;
	if (ImGui::Begin(T("Shader Preview###ShaderLiveTune"), &open, flags)) {
		if (ImGui::Button(T("Back to Settings")))
			returnToSettings = true;
		ImGui::SameLine();
		if (ImGui::Button(T("Done")))
			open = false;
		ImGui::TextWrapped("%s", T("Live game preview. Changes apply as you adjust the controls."));
		ImGui::TextWrapped("%s", T("Game inputs are held while this editor is open."));
		ImGui::Separator();
		if (ImGui::BeginTabBar("ShaderPreviewTabs")) {
			if (ImGui::BeginTabItem(T("Parameters"))) {
				renderShaderActivationStatus(true);
				ImGui::Separator();
				renderShaderParameterControls();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(T("Shader Stack"))) {
				renderShaderTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		// Use Settings touch gestures while nested lists and popups scroll independently.
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
			scrollWhenDraggingOnVoid();
#if defined(__ANDROID__)
		windowDragScroll(false);
#else
		windowDragScroll();
#endif
	}
	ImGui::End();

	if (returnToSettings) {
		g_shaderLiveTune = false;
		gui_open_shader_settings();
	}
	else if (!open)
		g_shaderLiveTune = false;
}

bool shaderLiveTuneActive()
{
	return g_shaderLiveTune.load(std::memory_order_relaxed)
			&& !settings.network.online.load(std::memory_order_relaxed);
}

bool liveTuneActive()
{
	return textureFilterLiveTuneActive() || shaderLiveTuneActive();
}

void closeShaderLiveTune()
{
	g_shaderLiveTune = false;
}

} // namespace SettingsNew
