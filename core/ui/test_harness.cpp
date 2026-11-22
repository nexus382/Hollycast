// Portions Copyright 2026 The Hollycast Authors
#include "test_harness.h"
#include "test_harness_protocol.h"
#include "test_harness_capture.h"
#include "test_harness_ui.h"
#include "test_harness_telemetry.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "rewind.h"
#include "input/gamepad_device.h"
#include "gui.h"
#include "imgui.h"
#include "settings_new.h"
#include "settings.h"
#include "mainui.h"
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_harness {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static fs::path root;
static std::string session;
static bool announced;
static std::atomic<bool> ownsControllerInput{false};
static Clock::time_point nextPoll;
struct Pending {
	std::string id, op;
	std::int64_t expiresMs = 0;
	bool screenshot = false;
};
static Pending pending;

static std::int64_t wallMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string utf8(const fs::path& path)
{
	const auto bytes = path.u8string();
	return std::string(bytes.begin(), bytes.end());
}

static void writeJson(const fs::path& path, const Json& value)
{
	const auto temporary = fs::path(path).concat(".tmp");
	{
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		stream.exceptions(std::ios::badbit | std::ios::failbit);
		stream << value.dump();
		stream.close();
	}
	// Every response has a unique name. Windows rename cannot atomically
	// replace an existing file, so neither endpoint reuses response names.
	fs::rename(temporary, path);
}

static void reply(const std::string& id, const Json& value, bool ok = true)
{
	Json response = {{"version", 1}, {"session", session}, {"id", id}, {"ok", ok}};
	response[ok ? "result" : "error"] = value;
	writeJson(root / ("response-" + id + ".json"), response);
}

bool controllerInputOwned() { return ownsControllerInput.load(std::memory_order_relaxed); }

class HarnessGamepad final : public GamepadDevice {
public:
	explicit HarnessGamepad(int port) : GamepadDevice(port, ControllerApiName, false)
	{
		input_mapper = std::make_shared<IdentityInputMapping>();
		input_mapper->dead_zone = 0;
		input_mapper->addTrigger(DC_AXIS_LT, false);
		input_mapper->addTrigger(DC_AXIS_RT, false);
		_name = "Hollycast Test Harness";
		_unique_id = "hollycast-test-harness-" + std::to_string(port);
	}

	void apply(const ControllerState& state)
	{
		const auto changed = buttons ^ state.buttons;
		for (std::size_t bit = 0; bit < ButtonNames.size(); ++bit)
			if ((changed & (1u << bit)) != 0)
				gamepad_btn_input(1u << bit, (state.buttons & (1u << bit)) != 0);
		buttons = state.buttons;
		constexpr std::array<DreamcastKey, 4> negative = { DC_AXIS_LEFT, DC_AXIS_UP, DC_AXIS2_LEFT, DC_AXIS2_UP };
		constexpr std::array<DreamcastKey, 4> positive = { DC_AXIS_RIGHT, DC_AXIS_DOWN, DC_AXIS2_RIGHT, DC_AXIS2_DOWN };
		for (std::size_t i = 0; i < state.axes.size(); ++i)
			gamepad_axis_input(state.axes[i] < 0 ? negative[i] : positive[i], std::abs(state.axes[i]));
		gamepad_axis_input(DC_AXIS_LT, state.triggers[0] - 32768);
		gamepad_axis_input(DC_AXIS_RT, state.triggers[1] - 32768);
		deadline = Clock::now() + std::chrono::milliseconds(state.leaseMs);
		active = true;
	}

	void release()
	{
		if (!active)
			return;
		apply(ControllerState{});
		active = false;
	}

	void expire(Clock::time_point now)
	{
		if (active && now >= deadline)
			release();
	}

private:
	std::uint32_t buttons = 0;
	bool active = false;
	Clock::time_point deadline;
};

class MappingTestDevice final : public GamepadDevice {
public:
	MappingTestDevice() : GamepadDevice(-1, ControllerApiName, true)
	{
		_name = "Hollycast Mapping Test Device";
		_unique_id = "hollycast-test-mapping-device";
		hasAnalogStick = true;
		input_mapper = std::make_shared<InputMapping>();
	}

	void set_maple_port(int) override {}

	const char *get_button_name(u32 code) override
	{
		return inputName("Button", code);
	}

	const char *get_axis_name(u32 code) override
	{
		return inputName("Axis", code);
	}

	bool buttonHeld(u32 code) const
	{
		return heldButtons.find(code) != heldButtons.end();
	}

	void setButtonHeld(u32 code, bool held)
	{
		if (held)
			heldButtons.insert(code);
		else
			heldButtons.erase(code);
	}

	bool sendButton(u32 code, bool pressed)
	{
		return gamepad_btn_input(code, pressed);
	}

	bool sendAxis(u32 code, int value)
	{
		return gamepad_axis_input(code, value);
	}

	void cancelDetection()
	{
		cancel_detect_input();
		heldButtons.clear();
	}

private:
	const char *inputName(const char *kind, u32 code)
	{
		nameBuffer = kind;
		nameBuffer += ' ';
		nameBuffer += std::to_string(code);
		return nameBuffer.c_str();
	}

	std::string nameBuffer;
	std::set<u32> heldButtons;
};

static std::shared_ptr<MappingTestDevice> mappingDevice;
static Clock::time_point mappingDetectionStarted;
static bool mappingDetectionObserved;
static Json mappingLastInput = nullptr;
// The adapters use the regular mapping path without registering persistent
// physical devices or changing the user's controller-to-port assignments.
static std::array<std::unique_ptr<HarnessGamepad>, 4> gamepads;
// Stable identity for this harness's rewind hold, refcounted in the core by
// source so releasing it never drops a physical device's hold.
static const char harnessRewindSource = 0;

void releaseInputs()
{
	rewind_state::releaseSource(&harnessRewindSource);
	ui::release();
	if (mappingDevice)
		mappingDevice->cancelDetection();
	mappingDetectionObserved = false;
	for (auto& pad : gamepads)
		if (pad)
		{
			pad->release();
			pad.reset();
		}
}

static bool restricted()
{
	return settings.raHardcoreMode || settings.network.online || settings.naomi.multiboard;
}

static bool recording;
static Json state();

static const char *settingsPageName()
{
	if (gui_state != GuiState::Settings)
		return nullptr;
	switch (SettingsNew::g_state.currentTab)
	{
	case SettingsNew::SettingsTab::General: return "general";
	case SettingsNew::SettingsTab::Library: return "library";
	case SettingsNew::SettingsTab::Video: return "video";
	case SettingsNew::SettingsTab::OnScreenDisplay: return "osd";
	case SettingsNew::SettingsTab::Audio: return "audio";
	case SettingsNew::SettingsTab::Controls: return "controls";
	case SettingsNew::SettingsTab::Network: return "network";
	case SettingsNew::SettingsTab::Advanced: return "advanced";
	case SettingsNew::SettingsTab::About: return "about";
	default: return nullptr;
	}
}

static bool hasOpenPopup()
{
	return ImGui::GetCurrentContext() != nullptr
		&& ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

static SettingsNew::SettingsTab settingsTabFromName(const std::string& page)
{
	if (page == "general") return SettingsNew::SettingsTab::General;
	if (page == "library") return SettingsNew::SettingsTab::Library;
	if (page == "video") return SettingsNew::SettingsTab::Video;
	if (page == "osd") return SettingsNew::SettingsTab::OnScreenDisplay;
	if (page == "audio") return SettingsNew::SettingsTab::Audio;
	if (page == "controls") return SettingsNew::SettingsTab::Controls;
	if (page == "network") return SettingsNew::SettingsTab::Network;
	if (page == "advanced") return SettingsNew::SettingsTab::Advanced;
	if (page == "about") return SettingsNew::SettingsTab::About;
	throw std::runtime_error("Unknown settings page");
}

static bool mappingInputReady()
{
	if (!mappingDevice || !mappingDevice->is_input_detecting())
	{
		mappingDetectionObserved = false;
		return false;
	}
	if (!mappingDetectionObserved)
	{
		mappingDetectionObserved = true;
		mappingDetectionStarted = Clock::now();
	}
	return Clock::now() - mappingDetectionStarted >= std::chrono::milliseconds(225);
}

static void sampleMappingDetection()
{
	(void)mappingInputReady();
}

static Json mappingStatus()
{
	const bool ready = mappingInputReady();
	const bool detecting = mappingDevice && mappingDevice->is_input_detecting();
	Json device = nullptr;
	Json mapState = nullptr;
	if (mappingDevice)
	{
		device = {{"api", mappingDevice->api_name()}, {"name", mappingDevice->name()},
			{"unique_id", mappingDevice->unique_id()}, {"role", "mapping_only"},
			{"maple_port", mappingDevice->maple_port()}};
		const auto mapping = mappingDevice->get_input_mapping();
		mapState = {{"dirty", mapping != nullptr && mapping->is_dirty()}};
	}
	return {{"device", device}, {"registered", mappingDevice != nullptr}, {"detecting", detecting},
		{"input_window_ready", ready}, {"last_input", mappingLastInput}, {"map_state", mapState}};
}

static Json mappingInputResult(bool delivered, bool accepted, const char *reason = nullptr)
{
	Json result = mappingStatus();
	result["delivered"] = delivered;
	result["accepted"] = accepted;
	if (reason != nullptr)
		result["reason"] = reason;
	return result;
}

static Json openSettingsPage(const Json& args)
{
	fields(args, {"page"});
	require(args.at("page").is_string(), "Settings page must be a string");
	const std::string page = args.at("page").get<std::string>();
	const SettingsNew::SettingsTab tab = settingsTabFromName(page);
	require(!recording, "Settings navigation is unavailable during performance capture");
	require(!settings.naomi.slave, "Settings navigation is unavailable on a Naomi slave");
	require(!hasOpenPopup(), "Close the current popup before changing settings pages");

	const bool allowedState = gui_state == GuiState::Main || gui_state == GuiState::Settings
		|| gui_state == GuiState::Commands || gui_state == GuiState::Pause || gui_state == GuiState::Closed;
	require(allowedState, "Settings navigation is unavailable from the current UI state");

	if (gui_state == GuiState::Closed && game_started)
	{
		releaseInputs();
		gui_open_settings();
		require(gui_state == GuiState::Commands || gui_state == GuiState::Settings,
			"Could not pause the running game for settings");
	}
	else if (gui_state == GuiState::Pause)
	{
		releaseInputs();
		gui_open_settings();
		require(gui_state == GuiState::Commands || gui_state == GuiState::Settings,
			"Could not open settings from the paused game");
	}
	else if (gui_state == GuiState::Closed)
	{
		gui_setState(GuiState::Main);
	}
	else
	{
		releaseInputs();
	}

	SettingsNew::openTab(tab);
	if (gui_state != GuiState::Settings)
		gui_setState(GuiState::Settings);
	return state();
}

static Json mappingOperation(const Json& args)
{
	require(args.is_object() && args.contains("action") && args.at("action").is_string(),
		"Mapping action must be a string");
	const std::string action = args.at("action").get<std::string>();
	if (action == "create")
	{
		fields(args, {"action"});
		require(gui_state == GuiState::Settings && SettingsNew::g_state.currentTab == SettingsNew::SettingsTab::Controls,
			"Create the mapping test device from Settings > Controls");
		require(!hasOpenPopup(), "Close the current popup before creating the mapping test device");
		bool created = false;
		if (!mappingDevice)
		{
			mappingDevice = std::make_shared<MappingTestDevice>();
			GamepadDevice::Register(mappingDevice);
			mappingLastInput = nullptr;
			created = true;
		}
		Json result = mappingStatus();
		result["created"] = created;
		return result;
	}
	if (action == "status")
	{
		fields(args, {"action"});
		return mappingStatus();
	}
	if (action == "cancel")
	{
		fields(args, {"action"});
		const bool cancelled = mappingDevice && mappingDevice->is_input_detecting();
		if (mappingDevice)
			mappingDevice->cancelDetection();
		mappingDetectionObserved = false;
		Json result = mappingStatus();
		result["cancelled"] = true;
		result["was_detecting"] = cancelled;
		return result;
	}

	if (action == "button")
	{
		fields(args, {"action", "code", "pressed"});
		const u32 code = static_cast<u32>(integer(args.at("code"), 0, 65535));
		require(args.at("pressed").is_boolean(), "Button pressed state must be boolean");
		const bool pressed = args.at("pressed").get<bool>();
		require(mappingDevice != nullptr, "Create the mapping test device first");
		require(gui_state == GuiState::Settings && SettingsNew::g_state.currentTab == SettingsNew::SettingsTab::Controls,
			"Mapping input requires Settings > Controls");
		if (pressed && mappingDevice->buttonHeld(code))
			return mappingInputResult(false, false, "button_already_pressed");
		if (!pressed && !mappingDevice->buttonHeld(code))
		{
			Json result = mappingInputResult(false, false);
			result["released"] = true;
			return result;
		}
		if (!mappingDevice->is_input_detecting())
		{
			if (!pressed)
			{
				mappingDevice->setButtonHeld(code, false);
				Json result = mappingInputResult(false, false);
				result["released"] = true;
				return result;
			}
			return mappingInputResult(false, false, "mapping_detection_inactive");
		}
		if (!mappingInputReady())
			return mappingInputResult(false, false, "input_window_not_ready");
		const bool accepted = mappingDevice->sendButton(code, pressed);
		mappingDevice->setButtonHeld(code, pressed);
		mappingLastInput = {{"type", "button"}, {"code", code}, {"pressed", pressed}};
		Json result = mappingInputResult(true, accepted);
		if (!pressed)
			result["released"] = true;
		return result;
	}
	if (action == "axis")
	{
		fields(args, {"action", "code", "value"});
		const u32 code = static_cast<u32>(integer(args.at("code"), 0, 65535));
		const int value = static_cast<int>(integer(args.at("value"), -32768, 32767));
		require(mappingDevice != nullptr, "Create the mapping test device first");
		require(gui_state == GuiState::Settings && SettingsNew::g_state.currentTab == SettingsNew::SettingsTab::Controls,
			"Mapping input requires Settings > Controls");
		if (!mappingDevice->is_input_detecting())
			return mappingInputResult(false, false, "mapping_detection_inactive");
		if (!mappingInputReady())
			return mappingInputResult(false, false, "input_window_not_ready");
		const bool accepted = mappingDevice->sendAxis(code, value);
		mappingLastInput = {{"type", "axis"}, {"code", code}, {"value", value}};
		return mappingInputResult(true, accepted);
	}
	fields(args, {"action"});
	throw std::runtime_error("Unknown mapping action");
}

static bool captureExists;
static bool previousPresent;
static Clock::time_point lastPresent;
static Clock::time_point captureStart;
static std::int64_t captureWallStart;
static int observationsDuringCapture;
static std::vector<double> intervals;
static std::set<std::string> captureProblems;
static std::string captureGame;
static int captureRenderer;
static std::uint64_t captureGuestStart;

void recordRewind()
{
	if (recording)
		captureProblems.insert("rewind_restore");
}

static Json rewindStatus()
{
	// Loading runs on a worker and may still be building the Maple device array.
	// Resume marks the completed game ready before host-side device inspection.
	const bool optionsReady = gui_state != GuiState::Loading || game_started;
	const char *reason = game_started ? rewind_state::unavailableReason()
			: gui_state == GuiState::Loading ? "Game is still loading."
			: "Start a game to record rewind history.";
	return {{"enabled", optionsReady ? Json(static_cast<bool>(config::RewindEnable)) : Json(nullptr)},
		{"available", reason == nullptr},
		{"unavailable_reason", reason == nullptr ? Json(nullptr) : Json(reason)},
		{"can_step", reason == nullptr && rewind_state::canStep()}, {"requested", rewind_state::requested()},
		{"held_by_harness", rewind_state::requested(&harnessRewindSource)},
		{"snapshots", rewind_state::snapshotCount()}, {"memory_bytes", rewind_state::memoryUsed()},
		{"interval_ms", optionsReady ? Json(config::RewindInterval.get()) : Json(nullptr)},
		{"duration_s", optionsReady ? Json(config::RewindDuration.get()) : Json(nullptr)},
		{"compression", optionsReady ? Json(config::RewindCompression.get()) : Json(nullptr)}};
}

static Json rewindOperation(const Json& args)
{
	fields(args, {"held"});
	require(args.at("held").is_boolean(), "Rewind held state must be boolean");
	const bool held = args.at("held").get<bool>();
	if (held)
	{
		require(game_started && emu.running(), "Rewind hold requires a running game");
		const char *reason = rewind_state::unavailableReason();
		require(reason == nullptr, reason == nullptr ? "" : reason);
		recordRewind();
		if (!rewind_state::requested(&harnessRewindSource))
			rewind_state::setRequested(&harnessRewindSource, true);
	}
	else
		rewind_state::releaseSource(&harnessRewindSource);
	return rewindStatus();
}

static Json state()
{
	Json osd = Json::object();
	for (const auto& key : config::getEntries("rend"))
		if (key.find("Overlay") != std::string::npos || key.find("ResourceMonitor") != std::string::npos
			|| key.find("Perf") != std::string::npos || key == "ShowFPS")
			osd[key] = config::loadStr("rend", key);
	Json pads = Json::array();
	for (int port = 0; port < 4; ++port)
		pads.push_back({{"port", port}, {"buttons_active_low", kcode[port]},
			{"axes", {joyx[port], joyy[port], joyrx[port], joyry[port]}}, {"triggers", {lt[port], rt[port]}}});
	const char *page = settingsPageName();
	return {{"wall_ms", wallMs()}, {"ui_frame_count", MainFrameCount}, {"gui_state", static_cast<int>(gui_state)},
		{"settings_page", page == nullptr ? Json(nullptr) : Json(page)},
		{"menu_open", ui::open()}, {"running", emu.running()}, {"input_restricted", restricted()},
		{"exclusive_controller_input", controllerInputOwned()},
		{"exclusive_ui_input", controllerInputOwned()},
		{"game_id", settings.content.gameId}, {"media", settings.content.path},
		{"platform", settings.platform.system},
		{"display", {{"width", settings.display.width}, {"height", settings.display.height}}},
		{"settings", {{"renderer", static_cast<int>(config::RendererType.get())},
			{"host_renderer", static_cast<int>(config::RendererType.get())},
			{"render_resolution", config::RenderResolution.get()}, {"vsync", config::VSync.get()},
			{"threaded_rendering", config::ThreadedRendering.get()}, {"frame_skip", config::SkipFrame.get()},
			{"auto_frame_skip", config::AutoSkipFrame.get()}, {"show_fps", config::ShowFPS.get()},
			{"dsp", config::DSPEnabled.get()}, {"osd", osd}}},
		{"controllers", pads}, {"rewind", rewindStatus()}, {"performance_recording", recording}};
}

static Json execute(const std::string& op, const Json& args)
{
	if (op == "observe")
	{
		fields(args, {});
		if (recording)
			++observationsDuringCapture;
		return state();
	}
	if (op == "release")
	{
		fields(args, {});
		releaseInputs();
		return Json::object();
	}
	if (op == "mapping" && args.is_object() && args.contains("action")
			&& args.at("action").is_string() && args.at("action").get<std::string>() == "cancel")
		return mappingOperation(args);
	// Releasing a rewind hold stays available if the session became restricted.
	if (op == "rewind" && args.is_object() && args.contains("held")
			&& args.at("held").is_boolean() && !args.at("held").get<bool>())
		return rewindOperation(args);
	require(!restricted() || op == "performance_end" || op == "quit",
		"Interactive control is unavailable during hardcore, netplay or multiboard sessions");
	if (op == "live")
	{
		fields(args, {});
		if (recording) ++observationsDuringCapture;
		return {{"state", state()}, {"telemetry", telemetry::live()}};
	}
	if (op == "watch_profile")
	{
		if (recording) captureProblems.insert("watch_profile_changed");
		telemetry::configure(args);
		return Json::object();
	}
	if (op == "quit")
	{
		fields(args, {});
		releaseInputs();
		dc_exit();
		return Json::object();
	}
	if (op == "controller")
	{
		const auto input = ControllerState::fromJson(args);
		auto& pad = gamepads[input.port];
		if (!pad)
			pad = std::make_unique<HarnessGamepad>(input.port);
		pad->apply(input);
		return {{"lease_ms", input.leaseMs}};
	}
	if (op == "settings")
		return openSettingsPage(args);
	if (op == "rewind")
		return rewindOperation(args);
	if (op == "mapping")
		return mappingOperation(args);
	if (op == "menu")
	{
		fields(args, {});
		releaseInputs();
		gui_open_settings();
		return state();
	}
	if (op == "memory")
	{
		if (recording) captureProblems.insert("manual_memory_observation");
		return telemetry::readMemory(args);
	}
	if (op == "performance_begin")
	{
		fields(args, {});
		require(!recording, "Performance capture is already active");
		require(emu.running() && !ui::open(), "Start performance capture during running gameplay");
		intervals.clear();
		intervals.reserve(36000);
		captureProblems.clear();
		observationsDuringCapture = 0;
		captureGuestStart = telemetry::completedGuestFrames();
		captureStart = Clock::now();
		captureWallStart = wallMs();
		captureGame = settings.content.gameId;
		captureRenderer = static_cast<int>(config::RendererType.get());
		previousPresent = false;
		captureExists = recording = true;
		return {{"wall_ms", captureWallStart}, {"domain", "host_present_interval_ms"}};
	}
	if (op == "performance_end")
	{
		fields(args, {});
		require(captureExists, "No performance capture has been started");
		recording = false;
		captureExists = false;
		if (intervals.size() < 2)
			captureProblems.insert("insufficient_samples");
		const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - captureStart).count();
		const auto guestEnd = telemetry::completedGuestFrames();
		if (guestEnd < captureGuestStart)
			captureProblems.insert("guest_frame_counter_regressed");
		const auto guestFrames = guestEnd >= captureGuestStart ? guestEnd - captureGuestStart : 0;
		// Count completed emulated boundaries over the same monotonic window.
		// Presentation rate alone can hide duplicated or skipped guest frames.
		return {{"domain", "host_present_interval_ms"}, {"start_wall_ms", captureWallStart},
			{"end_wall_ms", wallMs()}, {"elapsed_ms", elapsedMs},
			{"guest_frame_boundary_count", guestFrames}, {"guest_frame_boundary_hz", elapsedMs > 0 ? guestFrames * 1000.0 / elapsedMs : 0},
			{"guest_frame_boundary_source", "sh4_vblank"},
			{"samples_ms", intervals}, {"valid", captureProblems.empty()}, {"invalid_reasons", captureProblems},
			{"observations_during_capture", observationsDuringCapture}, {"game_id", captureGame}, {"renderer", captureRenderer},
			{"gpu_timing_measured", false}, {"physical_latency_measured", false}};
	}
	throw std::runtime_error("Unknown operation");
}

void prepareStorage()
{
	const char *directory = std::getenv("HOLLYCAST_HARNESS_DIR");
	if (!directory || !*directory)
		return;
	const char *token = std::getenv("HOLLYCAST_HARNESS_SESSION");
	require(token && isIdentifier(token), "Missing or invalid interactive harness session");
	const auto proposed = fs::u8path(directory);
	require(proposed.is_absolute() && fs::is_directory(proposed), "Harness root must be an existing absolute directory");
	require(!fs::is_symlink(fs::symlink_status(proposed)), "Harness root must not be a symlink");
	const auto canonical = fs::canonical(proposed);
	for (const auto& child : {canonical / "home", canonical / "home" / "data", canonical / "screenshots"})
		require(fs::is_directory(child) && !fs::is_symlink(fs::symlink_status(child)), "Missing or linked isolated storage");
	require(fs::create_directory(canonical / "emulator.lock"), "Harness directory is already owned; use a new run directory");
	root = canonical;
	session = token;
	// Raw-input devices may have their own polling threads. Ownership is
	// published atomically before they can affect this disposable session.
	ownsControllerInput.store(true, std::memory_order_relaxed);
	set_user_config_dir(utf8(root / "home"));
	set_user_data_dir(utf8(root / "home" / "data"));
	telemetry::initialize();
}

void poll()
{
	if (root.empty())
		return;
	const auto now = Clock::now();
	ui::poll();
	sampleMappingDetection();
	for (auto& pad : gamepads)
		if (pad)
			pad->expire(now);
	if (restricted())
		releaseInputs();
	if (recording)
	{
		if (restricted())
			captureProblems.insert("restricted_session");
		if (!emu.running() || ui::open())
			captureProblems.insert("gameplay_interrupted");
		if (settings.content.gameId != captureGame || static_cast<int>(config::RendererType.get()) != captureRenderer)
			captureProblems.insert("game_or_renderer_changed");
		if (settings.input.fastForwardMode)
			captureProblems.insert("fast_forward_enabled");
	}
	if (now < nextPoll)
		return;
	nextPoll = now + std::chrono::milliseconds(10);
	std::string id;
	try {
		if (!pending.id.empty())
		{
			if (wallMs() >= pending.expiresMs)
			{
				const auto expired = pending.id;
				pending = {};
				releaseInputs();
				reply(expired, "Deferred native UI command expired", false);
			}
			return;
		}
		if (!announced)
		{
			writeJson(root / "ready.json", {{"version", 1}, {"session", session}, {"pid", static_cast<int>(
#ifdef _WIN32
				_getpid()
#else
				getpid()
#endif
			)}, {"capabilities", {"controller", "exclusive_controller_input", "exclusive_ui_input", "native_ui", "settings_navigation", "mapping_test_device", "native_capture", "live_telemetry", "typed_memory_watches", "osd_metrics", "present_intervals", "benchmark_frame_counts", "rewind"}}});
			announced = true;
		}
		const auto path = root / "command.json";
		if (!fs::exists(path))
			return;
		require(fs::file_size(path) <= 65536, "Request exceeds 64 KiB");
		std::ifstream stream(path, std::ios::binary);
		stream.exceptions(std::ios::badbit);
		const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
		stream.close();
		fs::remove(path);
		const auto request = parse(bytes);
		if (request.contains("id") && request.at("id").is_string())
			id = request.at("id").get<std::string>();
		validateEnvelope(request, session, wallMs());
		const auto op = request.at("op").get<std::string>();
		if (op == "inspect" || op == "ui")
		{
			require(!restricted(), "Native UI inspection and control are unavailable in this session mode");
			if (recording) captureProblems.insert("native_ui_inspection_or_action");
			const auto& args = request.at("args");
			bool screenshot = false;
			if (op == "inspect")
			{
				fields(args, {"screenshot"});
				require(args.at("screenshot").is_boolean(), "Screenshot option must be boolean");
				screenshot = args.at("screenshot").get<bool>();
				ui::beginInspection();
			}
			else
				ui::startAction(args);
			pending = {id, op, request.at("expires_ms").get<std::int64_t>(), screenshot};
			return;
		}
		reply(id, execute(op, request.at("args")));
	} catch (const std::exception& error) {
		releaseInputs();
		if (recording)
			captureProblems.insert("bridge_error");
		// Consume malformed requests too, so a bad command cannot stall later
		// input release or flood the application log on every polling tick.
		std::error_code ignored;
		fs::remove(root / "command.json", ignored);
		if (isIdentifier(id))
		{
			try {
				reply(id, error.what(), false);
			} catch (const std::exception&) { }
		}
		WARN_LOG(COMMON, "Interactive test harness: %s", error.what());
	}
}

void beforePresent()
{
	if (root.empty() || pending.id.empty())
		return;
	if ((pending.op == "inspect" && !ui::inspectionReady()) || (pending.op == "ui" && !ui::actionReady()))
		return;
	const auto request = pending;
	pending = {};
	try {
		require(wallMs() < request.expiresMs, "Native observation expired before completion");
		require(!restricted(), "Session mode changed while native UI work was pending");
		if (request.op == "ui")
		{
			reply(request.id, {{"completed", true}, {"wall_ms", wallMs()}});
			return;
		}
		Json result = {{"observation_id", request.id}, {"wall_ms", wallMs()},
			{"state", state()}, {"ui", ui::inspection(request.id)}, {"screenshot", nullptr}};
		if (request.screenshot)
		{
			std::vector<std::uint8_t> rgba, png;
			int width = 0, height = 0;
			captureFrame(rgba, width, height);
			stbi_flip_vertically_on_write(0);
			const auto append = [](void *context, void *data, int size) {
				auto& output = *static_cast<std::vector<std::uint8_t> *>(context);
				const auto bytes = static_cast<const std::uint8_t *>(data);
				output.insert(output.end(), bytes, bytes + size);
			};
			require(stbi_write_png_to_func(append, &png, width, height, 4, rgba.data(), width * 4) != 0,
				"PNG encoding failed");
			const auto relative = "screenshots/" + request.id + ".png";
			const auto temporary = root / (relative + ".tmp");
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output.exceptions(std::ios::badbit | std::ios::failbit);
			output.write(reinterpret_cast<const char *>(png.data()), png.size());
			output.close();
			fs::rename(temporary, root / relative);
			result["screenshot"] = {{"file", relative}, {"width", width}, {"height", height},
				{"source", "composited_gpu_output"}, {"includes_ui_and_osd", true}};
		}
		reply(request.id, result);
	} catch (const std::exception& error) {
		releaseInputs();
		reply(request.id, error.what(), false);
	}
}

void presented()
{
	if (!recording)
		return;
	const auto now = Clock::now();
	if (!emu.running() || ui::open())
	{
		previousPresent = false;
		return;
	}
	if (previousPresent)
	{
		if (intervals.size() == 36000)
		{
			captureProblems.insert("sample_limit_reached");
			recording = false;
			return;
		}
		intervals.push_back(std::chrono::duration<double, std::milli>(now - lastPresent).count());
	}
	lastPresent = now;
	previousPresent = true;
}

} // namespace test_harness
