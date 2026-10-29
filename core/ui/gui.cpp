/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

    Hollycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Hollycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "gui.h"
#include "custom_theme.h"
#include "rend/osd.h"
#include "cfg/cfg.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "network/net_handshake.h"
#include "network/ice.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "readiness.h"
#include "setup_checklist_popup.h"
#include "library.h"
#include "library_savestates.h"
#include "gui_util.h"
#include "gui_font.h"
#include "imgread/common.h"
#include "emulator.h"
#include "mainui.h"
#include "lua/lua.h"
#include "gui_chat.h"
#include "imgui_driver.h"
#if FC_PROFILER
#include "implot.h"
#endif
#include "boxart/boxart.h"
#include "boxart/vmu_icon.h"
#include "profiler/fc_profiler.h"
#include "hw/naomi/card_reader.h"
#include "oslib/resources.h"
#include "achievements/achievements.h"
#include "gui_achievements.h"
#include "IconsFontAwesome6.h"
#include <stb_image_write.h>
#include <stb_image.h>
#include "hw/pvr/Renderer_if.h"
#include "rend/CustomTexture.h"
#include "hw/mem/addrspace.h"
#include "hw/maple/maple_if.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif
#include "vgamepad.h"
#include "settings.h"
#include "settings_new.h"
#ifndef LIBRETRO
#include "rend/postprocess/shader_chain.h"
#endif
#include "oslib/i18n.h"
#include "gui_font.h"
using namespace i18n;
#include "gui_menu.h"
#include "performance_overlay.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <mutex>
#include <algorithm>
#include <array>
#include <cctype>
#include <utility>
#include <atomic>
#include <cmath>
#include <limits>

bool game_started;

int insetLeft, insetRight, insetTop, insetBottom;
std::unique_ptr<ImGuiDriver> imguiDriver;
static bool styleReloadRequested;

static bool inited = false;
static bool launchScreenPending = true;
static SetupChecklistLifecycle setupChecklist;
GuiState gui_state = GuiState::Main;
static bool commandLineStart;
static library::LaunchMode libraryLaunchMode = library::LaunchMode::Default;
static std::string libraryResumeStatePath;
// Android storage callbacks can invalidate this cache off the render thread.
static std::atomic<bool> librarySavestateCacheInvalidated{false};
std::string launchOnExitUri;
static u32 mouseButtons;
static int mouseX, mouseY;
static float mouseWheel;
static bool mouseTouchscreen;
static std::string error_msg;
static bool error_msg_shown;
static bool launch_error_help = false;
static std::string osd_message;
static u64 osd_message_end;
static std::mutex osd_message_mutex;
static void (*showOnScreenKeyboard)(bool show);
static bool keysUpNextFrame[512];
bool uiUserScaleUpdated;
static bool clearActiveIdNextFrame;
bool showExitSaveDialog = false;
static bool showAutoLoadDialog = false;
enum class ExitSaveDialogAction { ReturnToLibrary, ExitEmulator };
static ExitSaveDialogAction exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;

GameScanner scanner;
static BackgroundGameLoader gameLoader;
static LibrarySavestateCache librarySavestateCache;
static std::future<bool> networkStatus;
static Boxart boxart;
static Chat chat;
static std::recursive_mutex guiMutex;
using LockGuard = std::lock_guard<std::recursive_mutex>;

static Toast toast;
static ScheduledThreadRunner<std::chrono::steady_clock::time_point> uiThreadRunner;

static constexpr float GAME_INFO_LONG_PRESS_SECONDS = 3.f;
static constexpr double LIBRARY_GAME_INFO_HOVER_SECONDS = 1.0;
static bool resetLibraryGameInfoHoverState;
struct LibraryLongPressState
{
	std::string gameId;
	GameMedia game;
	float startTime = 0.f;
	bool tracking = false;
	bool opened = false;
};
static LibraryLongPressState libraryLongPress;
static GameMedia selectedGameForInfo;
static bool touchedLibraryItemThisFrame = false;

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Resume:
		performance_overlay::reset();
		boxart.resumePlaytime();
		game_started = true;
		vgamepad::startGame();
		break;
	case Event::Start:
		SettingsNew::stopTextureFilterLiveTune();
		SettingsNew::closeShaderLiveTune();
		performance_overlay::reset();
		boxart.startPlaytime(settings.platform.system == DC_PLATFORM_DREAMCAST ? settings.content.gameId : "", settings.content.path);
		markLibraryGameBooted(settings.content.gameId, settings.content.path);
		GamepadDevice::load_system_mappings();
		break;
	case Event::Terminate:
		SettingsNew::stopTextureFilterLiveTune();
		performance_overlay::reset();
		boxart.checkpointPlaytime(true);
		GamepadDevice::load_system_mappings();
		game_started = false;
		SettingsNew::closeShaderLiveTune();
		break;
	case Event::Pause:
		performance_overlay::reset();
		boxart.checkpointPlaytime(true);
		break;
	case Event::VBlank:
		// VBlank checkpoints playtime without restarting the OSD sample interval.
		boxart.checkpointPlaytime(false);
		break;
	default:
		break;
	}
}

static void clearThumbnailCache();

void gui_init()
{
	DEBUG_LOG(COMMON, "gui_init() called");
	if (inited)
		return;
	inited = true;
	launchScreenPending = true;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
#if FC_PROFILER
	ImPlot::CreateContext();
#endif
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

	io.IniFilename = nullptr;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls

    EventManager::listen(Event::Resume, emuEventCallback);
	EventManager::listen(Event::Pause, emuEventCallback);
    EventManager::listen(Event::Start, emuEventCallback);
	EventManager::listen(Event::Terminate, emuEventCallback);
	EventManager::listen(Event::VBlank, emuEventCallback);
    ggpo::receiveChatMessages([](int playerNum, const std::string& msg) { chat.receive(playerNum, msg); });

#ifdef TARGET_UWP
	{
		// Detect when the on-screen keyboard is hidden and clear the text input widget id to validate the edit.
		// Otherwise the user will cancel the edit if he presses B, and must press A in the case of multi-line inputs.
		using namespace Windows::UI::ViewManagement;
		InputPane^ inputPane = InputPane::GetForCurrentView();
		if (inputPane)
		{
			inputPane->Hiding += ref new Windows::Foundation::TypedEventHandler<InputPane^, InputPaneVisibilityEventArgs^>(
				[](InputPane^, InputPaneVisibilityEventArgs^)
				{
					clearActiveIdNextFrame = true;
				});
		}
	}
#endif
}

static ImGuiKey keycodeToImGuiKey(u8 keycode)
{
	switch (keycode)
	{
		case 0x2B: return ImGuiKey_Tab;
		case 0x50: return ImGuiKey_LeftArrow;
		case 0x4F: return ImGuiKey_RightArrow;
		case 0x52: return ImGuiKey_UpArrow;
		case 0x51: return ImGuiKey_DownArrow;
		case 0x4B: return ImGuiKey_PageUp;
		case 0x4E: return ImGuiKey_PageDown;
		case 0x4A: return ImGuiKey_Home;
		case 0x4D: return ImGuiKey_End;
		case 0x49: return ImGuiKey_Insert;
		case 0x4C: return ImGuiKey_Delete;
		case 0x2A: return ImGuiKey_Backspace;
		case 0x2C: return ImGuiKey_Space;
		case 0x28: return ImGuiKey_Enter;
		case 0x29: return ImGuiKey_Escape;
		case 0x04: return ImGuiKey_A;
		case 0x06: return ImGuiKey_C;
		case 0x19: return ImGuiKey_V;
		case 0x1B: return ImGuiKey_X;
		case 0x1C: return ImGuiKey_Y;
		case 0x1D: return ImGuiKey_Z;
		case 0xE0:
		case 0xE4:
			return ImGuiMod_Ctrl;
		case 0xE1:
		case 0xE5:
			return ImGuiMod_Shift;
		case 0xE3:
		case 0xE7:
			return ImGuiMod_Super;
		default: return ImGuiKey_None;
	}
}

void gui_updateStyle()
{
	static float uiScale;
	const bool forceReload = styleReloadRequested;
	styleReloadRequested = false;

	verify(inited);
	uiThreadRunner.init();

#if !defined(TARGET_UWP) && !defined(__SWITCH__)
	const float dpiScale = std::max(1.f, settings.display.dpi / 100.f * 0.75f);
#if defined(__APPLE__) && !defined(TARGET_IPHONE)
	if (settings.display.pointScale > 1.f)
		// Match macOS point scaling for HiDPI modes.
		settings.display.uiScale = std::max(settings.display.pointScale, dpiScale);
	else
		// Dense 1x modes get only a small physical-DPI boost.
		settings.display.uiScale = 1.f + (dpiScale - 1.f) * 0.15f;
#else
	settings.display.uiScale = dpiScale;
#endif
   	// Limit scaling on small low-res screens
    if (settings.display.width <= 640 || settings.display.height <= 480)
    	settings.display.uiScale = std::min(1.2f, settings.display.uiScale);
#endif
    settings.display.uiScale *= uiUserScale();
	if (!forceReload && settings.display.uiScale == uiScale && ImGui::GetIO().Fonts->IsBuilt())
		return;
	uiScale = settings.display.uiScale;

	// Setup Dear ImGui style
	ImGui::GetStyle() = ImGuiStyle{};

	// Apply the current theme
	applyCurrentTheme();
	ImguiFileTexture::clearThemeImageFailures();

	gui_loadFonts();

	NOTICE_LOG(RENDERER, "Screen DPI is %.0f, size %d x %d. Scaling by %.2f", settings.display.dpi, settings.display.width, settings.display.height, settings.display.uiScale);
	vgamepad::applyUiScale();
}

void gui_requestStyleReload() { styleReloadRequested = true; }

void gui_keyboard_input(u32 wc)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharacter(wc);
}

void gui_keyboard_inputUTF8(const std::string& s)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharactersUTF8(s.c_str());
}

void gui_keyboard_key(u8 keyCode, bool pressed)
{
#ifndef LIBRETRO
	postprocess::setShaderHostKey(keyCode, pressed);
#endif
	if (!inited)
		return;
	ImGuiKey key = keycodeToImGuiKey(keyCode);
	if (key == ImGuiKey_None)
		return;
	if (!pressed && ImGui::IsKeyDown(key))
	{
		keysUpNextFrame[keyCode] = true;
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(key, pressed);
}

bool gui_keyboard_captured() {
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureKeyboard;
}

bool gui_mouse_captured() {
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureMouse;
}

void gui_set_mouse_position(int x, int y, bool touchscreen)
{
	mouseX = std::round(x * settings.display.pointScale);
	mouseY = std::round(y * settings.display.pointScale);
	mouseTouchscreen = touchscreen;
#ifndef LIBRETRO
	postprocess::setShaderHostMouse(static_cast<float>(mouseX), static_cast<float>(mouseY), mouseButtons);
#endif
}

bool gui_is_menu_touch_target(float x, float y)
{
	// Reuse Android's gesture capture so virtual controls never consume panel taps.
	return SettingsNew::liveTuneActive()
			|| GuiMenu::isTouchTarget(std::round(x * settings.display.pointScale),
			std::round(y * settings.display.pointScale));
}

void gui_set_mouse_button(int button, bool pressed, bool touchscreen)
{
	if (pressed)
		mouseButtons |= 1 << button;
	else
		mouseButtons &= ~(1 << button);
	mouseTouchscreen = touchscreen;
#ifndef LIBRETRO
	postprocess::setShaderHostMouse(static_cast<float>(mouseX), static_cast<float>(mouseY), mouseButtons);
#endif
}

void gui_set_mouse_wheel(float delta) {
	mouseWheel += delta;
#ifndef LIBRETRO
	postprocess::setShaderHostMouse(static_cast<float>(mouseX), static_cast<float>(mouseY), mouseButtons, delta);
#endif
	mouseTouchscreen = false;
}

static void gui_newFrame()
{
	imguiDriver->newFrame();
	ImGui::GetIO().DisplaySize.x = settings.display.width;
	ImGui::GetIO().DisplaySize.y = settings.display.height;

	ImGuiIO& io = ImGui::GetIO();

	io.AddMouseSourceEvent(mouseTouchscreen ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
	if (mouseX < 0 || mouseX >= settings.display.width || mouseY < 0 || mouseY >= settings.display.height)
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	else
		io.AddMousePosEvent(mouseX, mouseY);
	if (io.WantCaptureMouse)
	{
		io.AddMouseWheelEvent(0, -mouseWheel / 16);
		mouseWheel = 0;
	}
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, (mouseButtons & (1 << 0)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Right, (mouseButtons & (1 << 1)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (mouseButtons & (1 << 2)) != 0);
	io.AddMouseButtonEvent(3, (mouseButtons & (1 << 3)) != 0);

	// shows a popup navigation window even in game because of the OSD
	//io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, ((kcode[0] & DC_BTN_X) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, ((kcode[0] & DC_BTN_B) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceUp, ((kcode[0] & DC_BTN_Y) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, ((kcode[0] & DC_BTN_A) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, ((kcode[0] & DC_DPAD_LEFT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, ((kcode[0] & DC_DPAD_RIGHT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, ((kcode[0] & DC_DPAD_UP) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, ((kcode[0] & DC_DPAD_DOWN) == 0));

	float analog;
	analog = joyx[0] < 0 ? -(float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, analog > 0.1f, analog);
	analog = joyx[0] > 0 ? (float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, analog > 0.1f, analog);
	analog = joyy[0] < 0 ? -(float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, analog > 0.1f, analog);
	analog = joyy[0] > 0 ? (float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, analog > 0.1f, analog);

	// Emergency quit: Shift+Esc or Ctrl+Shift+Esc to force quit (bypasses save dialog)
	// This is a safety measure in case of UI bugs
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
	    (ImGui::GetIO().KeyShift || ImGui::GetIO().KeySuper))
	{
		INFO_LOG(COMMON, "Emergency quit activated");
		showExitSaveDialog = false;  // Clear any pending dialog
		gui_stop_game();  // Stop the game immediately
		dc_exit();  // Exit application
		return;  // Skip the rest of this frame
	}

	if (showOnScreenKeyboard != nullptr)
		showOnScreenKeyboard(io.WantTextInput);
	if (clearActiveIdNextFrame && io.WantTextInput)
	{
		ImGui::ClearActiveID();
		clearActiveIdNextFrame = false;
	}
}

// SDL on-screen keyboard: Delay keys up by one frame to allow quick key presses.
static void delayedKeysUp()
{
	ImGuiIO& io = ImGui::GetIO();
	for (u32 i = 0; i < std::size(keysUpNextFrame); i++)
		if (keysUpNextFrame[i])
			io.AddKeyEvent(keycodeToImGuiKey(i), false);
	memset(keysUpNextFrame, 0, sizeof(keysUpNextFrame));
}

static void gui_endFrame(bool gui_open) {
    imguiDriver->renderDrawData(ImGui::GetDrawData(), gui_open);
    delayedKeysUp();
}

void gui_setOnScreenKeyboardCallback(void (*callback)(bool show)) {
	showOnScreenKeyboard = callback;
}

void gui_set_insets(int left, int right, int top, int bottom)
{
	insetLeft = left;
	insetRight = right;
	insetTop = top;
	insetBottom = bottom;
}

#if 0
#include "oslib/timeseries.h"
#include <vector>
TimeSeries renderTimes;
TimeSeries vblankTimes;

void gui_plot_render_time(int width, int height)
{
	std::vector<float> v = renderTimes.data();
	ImGui::PlotLines(T("Render Times"), v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text(T("StdDev: %.1f%%"), renderTimes.stddev() * 100.f / 0.01666666667f);
	v = vblankTimes.data();
	ImGui::PlotLines(T("VBlank"), v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text(T("StdDev: %.1f%%"), vblankTimes.stddev() * 100.f / 0.01666666667f);
}
#endif

bool gui_resume_game()
{
	if (custom_texture.needsRefresh())
	{
		gui_setState(GuiState::Loading);
		return false;
	}
	gui_setState(GuiState::Closed);
	return true;
}

void gui_open_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && !settings.naomi.slave)
	{
		if (!ggpo::active())
		{
			if (achievements::canPause())
			{
				vgamepad::hide();
				try {
					emu.stop();
					gui_setState(GuiState::Commands);
				} catch (const FlycastException& e) {
					gui_stop_game(e.what());
				}
			}
		}
		else
		{
			chat.toggle();
		}
	}
	else if (gui_state == GuiState::Loading)
	{
		gameLoader.cancel();
	}
	else if (gui_state == GuiState::Commands)
	{
		const bool resumeNow = gui_resume_game();
		GamepadDevice::load_system_mappings();
		if (resumeNow)
			emu.start();
	}
	else if (gui_state == GuiState::Pause)
	{
		gui_setState(GuiState::Commands);
	}
	else if (gui_state == GuiState::GameInfo)
	{
		gui_setState(GuiState::Main);
	}
}

void gui_open_shader_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Closed || !game_started || ggpo::active() || !achievements::canPause())
		return;

	try {
		SettingsNew::stopTextureFilterLiveTune();
		vgamepad::hide();
		emu.stop();
		SettingsNew::openTab(SettingsNew::SettingsTab::Shader);
		gui_setState(GuiState::Settings);
	} catch (const FlycastException& e) {
		gui_stop_game(e.what());
	}
}

void gui_open_texture_filter_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Closed || !game_started || ggpo::active() || !achievements::canPause())
		return;

	try {
		vgamepad::hide();
		emu.stop();
		SettingsNew::openTab(SettingsNew::SettingsTab::Video);
		gui_setState(GuiState::Settings);
	} catch (const FlycastException& e) {
		gui_stop_game(e.what());
	}
}

void gui_start_game(const std::string& path)
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Main && gui_state != GuiState::Closed && gui_state != GuiState::Commands
			&& gui_state != GuiState::Pause)
		return;
	libraryLaunchMode = library::LaunchMode::Default;
	libraryResumeStatePath.clear();
	librarySavestateCacheInvalidated = true;
	setupChecklist.gameLaunchStarted(config::loadBool("hollycast", "SetupChecklistPending", false), [](bool pending) {
		config::saveBool("hollycast", "SetupChecklistPending", pending);
	});
	showExitSaveDialog = false;
	showAutoLoadDialog = false;
	emu.unloadGame();
	reset_vmus();
    chat.reset();

	// Clear thumbnail cache when starting a new game
	clearThumbnailCache();

	scanner.stop();
	gui_setState(GuiState::Loading);
	gameLoader.load(path);
}

static void gui_start_library_game(const std::string& path, bool resume, const std::string& statePath)
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Main)
		return;
	gui_start_game(path);
	if (gui_state == GuiState::Loading)
	{
		libraryLaunchMode = resume ? library::LaunchMode::Resume : library::LaunchMode::Fresh;
		libraryResumeStatePath = statePath;
	}
}

void gui_stop_game(const std::string& message, bool allowAutoSave)
{
	const LockGuard lock(guiMutex);
	libraryLaunchMode = library::LaunchMode::Default;
	libraryResumeStatePath.clear();
	showExitSaveDialog = false;
	showAutoLoadDialog = false;
	if (!commandLineStart)
	{
		// Exit to main menu
		emu.unloadGame(allowAutoSave);
		gui_setState(GuiState::Main);
		reset_vmus();

		// Clear thumbnail cache when stopping game
		clearThumbnailCache();

		if (!message.empty())
			gui_error(Ts("Hollycast has stopped.") + "\n\n" + message);
	}
	else
	{
		if (!message.empty())
			ERROR_LOG(COMMON, "Flycast has stopped: %s", message.c_str());
		emu.unloadGame(allowAutoSave);
		// Exit emulator
		dc_exit();
	}
}

static void savestate();
static void savestate(int slot);

static void save_auto_state_before_exit()
{
	if (!dc_savestateAllowed())
		return;

	try {
		if (gui_state == GuiState::Closed)
			emu.stop();
		// Save here before exit so the chosen "Save and Exit" path uses the
		// hidden auto-save slot, then suppress unloadGame() from writing a second
		// auto-save for the same shutdown.
		savestate(dc_getAutoSaveSlot());
		dc_skipAutoSaveOnNextUnload();
	} catch (const FlycastException& e) {
		WARN_LOG(COMMON, "save_auto_state_before_exit: %s", e.what());
	}
}

static void finish_exit_request(bool exitEmulator, bool saveFirst)
{
	if (saveFirst)
		save_auto_state_before_exit();

	if (exitEmulator && !commandLineStart)
	{
		emu.unloadGame(false);
		mainui_stop();
		return;
	}

	gui_stop_game("", false);
}

void gui_request_exit_to_library()
{
	if (dc_savestateAllowed() && config::AutoSaveState)
	{
		finish_exit_request(false, true);
		return;
	}

	if (dc_savestateAllowed() && config::SaveProtection)
	{
		exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;
		showExitSaveDialog = true;
		return;
	}
	gui_stop_game("", false);
}

void gui_request_exit_emulator()
{
	if (!game_started)
	{
		mainui_stop();
		return;
	}

	if (dc_savestateAllowed() && config::AutoSaveState)
	{
		finish_exit_request(true, true);
		return;
	}

	if (!dc_savestateAllowed() || !config::SaveProtection)
	{
		finish_exit_request(true, false);
		return;
	}

	exitSaveDialogAction = ExitSaveDialogAction::ExitEmulator;
	showExitSaveDialog = true;
}

void gui_request_initial_auto_load()
{
	// A library action applies to this boot only, never the saved auto-load preference.
	const auto launchMode = std::exchange(libraryLaunchMode, library::LaunchMode::Default);
	const std::string resumePath = std::exchange(libraryResumeStatePath, std::string{});
	const bool stateAllowed = dc_savestateAllowed() && !settings.raHardcoreMode;
	const bool networkStartup = !game_started && NetworkHandshake::instance != nullptr;
	const int autoSaveSlot = dc_getAutoSaveSlot();
	const bool stateAvailable = launchMode == library::LaunchMode::Resume ? !resumePath.empty()
			: !networkStartup && stateAllowed && dc_getStateCreationDate(autoSaveSlot) != 0;
	const auto action = library::initialAutoLoadAction(launchMode,
			stateAvailable, stateAllowed, config::AutoLoadState, networkStartup);
	if (action == library::AutoLoadAction::StartNetwork)
	{
		networkStatus = NetworkHandshake::instance->start();
		gui_setState(GuiState::NetworkStart);
		return;
	}
	if (launchMode == library::LaunchMode::Resume)
	{
		if (action == library::AutoLoadAction::RejectResume || !dc_loadstateForPath(resumePath))
		{
			// Reject before a network handshake, and never autosave over a failed Resume.
			gui_stop_game("", false);
			gui_error(!stateAllowed || networkStartup
					? T("Save states cannot be resumed in the current mode. Choose Start Fresh to play.")
					: T("The selected save state could not be loaded. Choose Start Fresh to play."));
			return;
		}
		gui_setState(GuiState::Closed);
		return;
	}
	if (action == library::AutoLoadAction::StartFresh)
	{
		gui_setState(GuiState::Closed);
		return;
	}

	if (action == library::AutoLoadAction::Load)
	{
		dc_loadstate(autoSaveSlot);
		gui_setState(GuiState::Closed);
		return;
	}

	showAutoLoadDialog = true;
	gui_setState(GuiState::Pause);
}

static void appendVectorData(void *context, void *data, int size)
{
	std::vector<u8>& v = *(std::vector<u8> *)context;
	const u8 *bytes = (const u8 *)data;
	v.insert(v.end(), bytes, bytes + size);
}

static void getScreenshot(std::vector<u8>& data, int width = 0)
{
	data.clear();
	std::vector<u8> rawData;
	int height = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(rawData, width, height))
		return;
	stbi_flip_vertically_on_write(0);
	stbi_write_png_to_func(appendVectorData, &data, width, height, 3, &rawData[0], 0);
}

static void savestate(int slot)
{
	// Potential optimization: move screenshot/state compression/write off the UI thread.
	std::vector<u8> pngData;
	getScreenshot(pngData, 640);
	dc_savestate(slot, pngData.empty() ? nullptr : &pngData[0], pngData.size());
	ImguiStateTexture savestatePic(slot);
	savestatePic.invalidate();
}

static void savestate()
{
	savestate(config::SavestateSlot);
}

static void render_exit_save_dialog()
{
	static bool wasDialogShown = false;

	if (!showExitSaveDialog)
	{
		wasDialogShown = false;
		return;
	}

	// Center dialog
	ImGui::SetNextWindowPos(
		ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
		ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	// Only open popup if it's not already open and this is a new request
	if (showExitSaveDialog && !wasDialogShown)
	{
		ImGui::OpenPopup(Tnop("Exit Save Prompt"));
		wasDialogShown = true;
	}

	const bool exitEmulator = exitSaveDialogAction == ExitSaveDialogAction::ExitEmulator;
	const std::string exitGamePopup = std::string(exitEmulator ? T("Exit Emulator?") : T("Exit Game?")) + "###Exit Save Prompt";
	if (ImGui::BeginPopupModal(exitGamePopup.c_str(), NULL,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::TextUnformatted(exitEmulator
			? T("Do you want to save state before exiting the emulator?")
			: T("Do you want to save state before returning to the library?"));
		ImGui::NewLine();

		// Button styling (consistent with existing dialogs)
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
		const char *saveAndExit = T("Save & Exit");
		const char *exitWithoutSaving = T("Exit Without Saving");
		const char *cancelButton = T("Cancel");
		const float buttonWidth = std::max({
			ImGui::CalcTextSize(saveAndExit).x,
			ImGui::CalcTextSize(exitWithoutSaving).x,
			ImGui::CalcTextSize(cancelButton).x
		}) + ImGui::GetStyle().FramePadding.x * 2 + uiScaled(24.f);

		// Check if save is allowed
		bool canSave = dc_savestateAllowed();

		if (ImGui::Button(saveAndExit, ImVec2(buttonWidth, 0)))
		{
			if (canSave)
			{
				ImGui::CloseCurrentPopup();
				showExitSaveDialog = false;
				finish_exit_request(exitEmulator, true);
			}
			else
			{
				// Show error that save isn't allowed
				ImGui::OpenPopup(Tnop("Save Not Allowed"));
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(exitWithoutSaving, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showExitSaveDialog = false;
			finish_exit_request(exitEmulator, false);
		}
		ImGui::SameLine();
		if (ImGui::Button(cancelButton, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showExitSaveDialog = false;
			exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;
		}

		// Error popup for save not allowed
		const std::string saveNotAllowedPopup = std::string(T("Save Not Allowed")) + "###Save Not Allowed";
		if (ImGui::BeginPopupModal(saveNotAllowedPopup.c_str(), NULL,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::TextUnformatted(T("Cannot save state in current mode."));
			ImGui::TextUnformatted(T("Possible reasons:"));
			ImGui::BulletText("%s", T("No game loaded"));
			ImGui::BulletText("%s", T("Network play active"));
			ImGui::BulletText("%s", T("Multi-board arcade mode"));
			ImGui::NewLine();
			if (ImGui::Button(T("OK"), ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(2);
		ImGui::EndPopup();
	}
}

static void render_auto_load_dialog()
{
	static bool wasDialogShown = false;

	if (!showAutoLoadDialog)
	{
		wasDialogShown = false;
		return;
	}

	ImGui::SetNextWindowPos(
		ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
		ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (!wasDialogShown)
	{
		ImGui::OpenPopup(Tnop("Auto-Load Prompt"));
		wasDialogShown = true;
	}

	const std::string popupTitle = std::string(T("Load Auto-Save?")) + "###Auto-Load Prompt";
	if (ImGui::BeginPopupModal(popupTitle.c_str(), nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::TextUnformatted(T("A hidden Auto-Save was found for this game."));
		ImGui::TextUnformatted(T("Do you want to load it?"));
		const int autoSaveSlot = dc_getAutoSaveSlot();
		const time_t savestateDate = dc_getStateCreationDate(autoSaveSlot);
		if (savestateDate != 0)
			ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", timeToShortDateTimeString(savestateDate).c_str());

		ImGui::Spacing();
		ImguiStateTexture savestatePic(autoSaveSlot);
		savestatePic.draw(ScaledVec2(320.f, 0.f));
		ImGui::Spacing();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
		const char *yesButton = T("Yes");
		const char *noButton = T("No");
		const float buttonWidth = std::max(ImGui::CalcTextSize(yesButton).x, ImGui::CalcTextSize(noButton).x)
			+ ImGui::GetStyle().FramePadding.x * 2 + uiScaled(60.f);

		if (ImGui::Button(yesButton, ImVec2(buttonWidth, 0)))
		{
			dc_loadstate(autoSaveSlot);
			ImGui::CloseCurrentPopup();
			showAutoLoadDialog = false;
			gui_setState(GuiState::Closed);
		}
		ImGui::SameLine();
		if (ImGui::Button(noButton, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showAutoLoadDialog = false;
			gui_setState(GuiState::Closed);
		}

		ImGui::PopStyleVar(2);
		ImGui::EndPopup();
	}
}

static void gui_display_commands()
{
	fullScreenWindow(false);
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _{ImGuiStyleVar_WindowBorderSize, 0};

	ImGui::Begin("##commands", nullptr, ImGuiWindowFlags_NoDecoration);
	{
		ImguiStyleVar _{ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)};	// left aligned

		float columnWidth = std::min(200.f,
				(ImGui::GetContentRegionAvail().x - uiScaled(100 + 150) - ImGui::GetStyle().FramePadding.x * 2)
				/ 2 / uiScaled(1));
		float buttonWidth = 150.f;	// not scaled
		bool lowWidth = ImGui::GetContentRegionAvail().x < uiScaled(100 + buttonWidth * 3)
				+ ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x * 2;
		if (lowWidth)
			buttonWidth = std::min(150.f,
					(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x * 2)
					/ 3 / uiScaled(1));
		bool lowHeight = ImGui::GetContentRegionAvail().y < uiScaled(100 + 50 * 2 + buttonWidth * 3 / 4) + ImGui::GetTextLineHeightWithSpacing() * 2
				+ ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y;

		GameMedia game;
		game.path = settings.content.path;
		game.fileName = settings.content.fileName;
		game.arcade = settings.platform.isArcade();
		GameBoxart art = boxart.getBoxart(game);
		ImguiFileTexture tex(art.boxartPath);
		// Use a fallback image when artwork is unavailable
		tex.draw(ScaledVec2(100, 100));

		ImGui::SameLine();
		if (!lowHeight)
		{
			ImGui::BeginChild("game_info", ScaledVec2(0, 100.f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::Text("%s", art.name.c_str());
			ImGui::PopFont();
			{
				ImguiStyleColor _(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
				ImGui::TextWrapped("%s", art.fileName.c_str());
				if (!art.arcade && !art.uniqueId.empty())
					ImGui::Text(T("UID: %s"), art.uniqueId.c_str());
			}
			ImGui::EndChild();
		}

		if (lowWidth) {
			ImGui::Columns(3, "buttons", false);
		}
		else
		{
			ImGui::Columns(4, "buttons", false);
			ImGui::SetColumnWidth(0, uiScaled(100.f)  + ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetColumnWidth(1, uiScaled(columnWidth));
			ImGui::SetColumnWidth(2, uiScaled(columnWidth));
			const ImVec2 vmuPos = ImGui::GetStyle().WindowPadding + ScaledVec2(0.f, 100.f)
					+ ImVec2(insetLeft, ImGui::GetStyle().ItemSpacing.y + getScaledTopInset());
			ImguiVmuTexture::displayVmus(vmuPos);
			ImGui::NextColumn();
		}
		ImguiStyleVar _1{ImGuiStyleVar_FramePadding, ScaledVec2(12.f, 3.f)};

		// Resume
		if (IconButton(ICON_FA_PLAY, T("Resume"), ScaledVec2(buttonWidth, 50)).realize())
		{
			GamepadDevice::load_system_mappings();
			gui_resume_game();
		}
		// Cheats
		{
			DisabledScope _{settings.network.online || settings.raHardcoreMode};

			if (IconButton(ICON_FA_MASK, T("Cheats"), ScaledVec2(buttonWidth, 50)).realize() && !settings.network.online)
				gui_setState(GuiState::Cheats);
		}
		// Achievements
		{
			DisabledScope _{!achievements::isActive()};

			if (IconButton(ICON_FA_TROPHY, T("Achievements"), ScaledVec2(buttonWidth, 50)).realize() && achievements::isActive())
				gui_setState(GuiState::Achievements);
		}
		// Barcode
		if (card_reader::barcodeAvailable())
		{
			ImGui::Text("%s", T("Barcode Card"));
			char cardBuf[64] {};
			strncpy(cardBuf, card_reader::barcodeGetCard().c_str(), sizeof(cardBuf) - 1);
			ImGui::SetNextItemWidth(uiScaled(buttonWidth));
			if (InputText("##barcode", cardBuf, sizeof(cardBuf), ImGuiInputTextFlags_None))
				card_reader::barcodeSetCard(cardBuf);
		}

		ImGui::NextColumn();

		// Insert/Eject Disk
		std::string disk_label = gdr::isOpen() ? T("Insert Disk") : T("Eject Disk");
		if (IconButton(ICON_FA_COMPACT_DISC, disk_label, ScaledVec2(buttonWidth, 50)).realize())
		{
			if (gdr::isOpen()) {
				gui_setState(GuiState::SelectDisk);
			}
			else {
				emu.openGdrom();
				gui_resume_game();
			}
		}
			// Settings
			if (ImGui::Button((std::string(ICON_FA_GEAR "  ") + T("Settings")).c_str(), ScaledVec2(buttonWidth, 50)))
			{
				gui_setState(GuiState::Settings);
			}

		// Exit
		if (IconButton(ICON_FA_POWER_OFF, commandLineStart ?  T("Exit") : T("Close Game"), ScaledVec2(buttonWidth, 50)).realize())
			gui_request_exit_to_library();

		ImGui::NextColumn();
		{
			DisabledScope _{!dc_savestateAllowed()};
			ImguiStateTexture savestatePic;
			time_t savestateDate = dc_getStateCreationDate(config::SavestateSlot);

			// Load State
			{
				DisabledScope _{settings.raHardcoreMode || savestateDate == 0};
				if (IconButton(ICON_FA_CLOCK_ROTATE_LEFT, T("Load State"), ScaledVec2(buttonWidth, 50)).realize() && dc_savestateAllowed())
				{
					gui_resume_game();
					dc_loadstate(config::SavestateSlot);
				}
			}

			// Save State
			if (IconButton(ICON_FA_DOWNLOAD, T("Save State"), ScaledVec2(buttonWidth, 50)).realize() && dc_savestateAllowed())
			{
				gui_resume_game();
				savestate();
			}

			// Slot #
			if (ImGui::ArrowButton("##prev-slot", ImGuiDir_Left))
				gui_cycleSaveStateSlot(-1);
			std::string slot = strprintf(T("Slot %d"), (int)config::SavestateSlot + 1);
			float spacingW = (uiScaled(buttonWidth) - ImGui::GetFrameHeight() * 2 - ImGui::CalcTextSize(slot.c_str()).x) / 2;
			ImGui::SameLine(0, spacingW);
			ImGui::Text("%s", slot.c_str());
			ImGui::SameLine(0, spacingW);
			if (ImGui::ArrowButton("##next-slot", ImGuiDir_Right))
				gui_cycleSaveStateSlot(1);
			{
				if (savestateDate == 0)
					ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", T("Empty"));
				else
					ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", timeToShortDateTimeString(savestateDate).c_str());
			}
			savestatePic.draw(ScaledVec2(buttonWidth, 0.f));
		}

		ImGui::Columns(1, nullptr, false);
	}
	ImGui::End();
}

void error_popup()
{
	if (!error_msg_shown && !error_msg.empty())
	{
		ImVec2 padding = ScaledVec2(20, 20);
		ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
		ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
		ImGui::OpenPopup(Tnop("Error"));
		const std::string errorPopup = std::string(T("Error")) + "###Error";
		if (ImGui::BeginPopupModal(errorPopup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
			ImGui::TextWrapped("%s", error_msg.c_str());
			if (launch_error_help)
			{
				ImGui::Spacing();
				ImGui::TextWrapped("%s", T("Check the game image and any accompanying track files. Review firmware and controls in the setup checklist."));
				if (ImGui::Button(T("Open Setup Checklist")))
				{
					error_msg.clear();
					launch_error_help = false;
					ImGui::CloseCurrentPopup();
					gui_open_setup_checklist();
				}
			}
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(80.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button(T("OK"), ScaledVec2(80.f, 0)))
				{
					error_msg.clear();
					launch_error_help = false;
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SetItemDefaultFocus();
			ImGui::PopTextWrapPos();
			ImGui::EndPopup();
		}
		error_msg_shown = true;
	}
}

static void render_modal_dialogs()
{
	error_popup();
	render_exit_save_dialog();
	render_auto_load_dialog();
}

static void contentpath_warning_popup()
{
    static bool show_contentpath_selection;

    if (scanner.content_path_looks_incorrect)
    {
        ImGui::OpenPopup(Tnop("Incorrect Content Location?"));
        const std::string incorrectContentPopup = std::string(T("Incorrect Content Location?")) + "###Incorrect Content Location?";
        if (ImGui::BeginPopupModal(incorrectContentPopup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
            ImGui::TextWrapped((std::string("  ") + T("Scanned %d folders but no game can be found!") + std::string("  ")).c_str(), scanner.empty_folders_scanned);
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x - uiScaled(55.f));
				if (ImGui::Button(T("Reselect"), ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					show_contentpath_selection = true;
				}

				ImGui::SameLine();
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x + uiScaled(55.f));
				if (ImGui::Button(T("Cancel"), ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					scanner.stop();
					config::ContentPath.get().clear();
				}
			}
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
    }
    if (show_contentpath_selection)
    {
        scanner.stop();
        const char *title = T("Select a Content Folder");
        ImGui::OpenPopup(title);
        select_file_popup(title, [](bool cancelled, std::string selection)
        {
            show_contentpath_selection = false;
            if (!cancelled)
            {
            	config::ContentPath.get().clear();
                config::ContentPath.get().push_back(selection);
            }
            gui_refresh_files();
            return true;
        });
    }
}

void os_notify(const char *msg, int durationMs, const char *details)
{
	if (gui_state != GuiState::Closed && gui_state != GuiState::Pause)
	{
		std::lock_guard<std::mutex> _{osd_message_mutex};
		osd_message = msg;
		osd_message_end = getTimeMs() + durationMs;
	}
	else {
		toast.show(msg, details != nullptr ? details : "", durationMs);
	}
}

static std::string get_notification()
{
	std::lock_guard<std::mutex> lock(osd_message_mutex);
	if (!osd_message.empty() && getTimeMs() >= osd_message_end)
		osd_message.clear();
	return osd_message;
}

static void drawPauseIcon()
{
	const char *icon = ICON_FA_PAUSE;
	ImFont *font = ImGui::GetFont();
	const float fontSize = uiScaled(52.f);
	const ScaledVec2 padding(14.f, 6.f);
	const ScaledVec2 margin(12.f, 12.f);
	const ImVec2 size =  ImVec2(fontSize * 0.7, fontSize) + padding * 2;
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImVec2 pos(displaySize.x - insetRight - margin.x - size.x, insetTop + margin.y);
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImU32 bgCol = alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.45f);
	const ImU32 shadowCol = alphaOverride(0, 0.65f);
	const ImU32 textCol = alphaOverride(ImGui::GetColorU32(ImGuiCol_Text), 0.95f);

	dl->AddRectFilled(pos, pos + size, bgCol, uiScaled(6.f));

	ImVec2 iconPos = pos + padding + ScaledVec2(2.5f, 2.5f);
	dl->AddText(font, fontSize, iconPos + ScaledVec2(2.5f, 2.5f), shadowCol, icon);
	dl->AddText(font, fontSize, iconPos, textCol, icon);
}

inline static void gui_display_demo() {
	ImGui::ShowDemoWindow();
}

static void gameTooltip(const std::string& tip)
{
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(tip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static GameBoxart getLibraryDisplayArtwork(const GameMedia& game, double animationClock);

static ImVec4 blendThemeColor(ImGuiCol base, ImGuiCol accent, float accentWeight, float alpha = 1.0f)
{
	const ImVec4 a = ImGui::GetStyle().Colors[base];
	const ImVec4 b = ImGui::GetStyle().Colors[accent];
	return ImVec4(a.x + (b.x - a.x) * accentWeight,
			a.y + (b.y - a.y) * accentWeight,
			a.z + (b.z - a.z) * accentWeight, alpha);
}

static ImVec4 themeStatusColor(const char *role, const ImVec4& semantic, const char *widget = "media.status")
{
	const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
	constexpr float semanticWeight = 0.62f;
	const ImVec4 fallback(accent.x + (semantic.x - accent.x) * semanticWeight,
			accent.y + (semantic.y - accent.y) * semanticWeight,
			accent.z + (semantic.z - accent.z) * semanticWeight, 1.0f);
	return CustomTheme::roleColor(role, fallback, widget);
}

static const char *game_info_value(const std::string& value)
{
	return value.empty() ? T("Unknown") : value.c_str();
}

static ImFont *game_info_title_font()
{
	return settingsRightValueFont != nullptr ? settingsRightValueFont : (largeFont != nullptr ? largeFont : ImGui::GetFont());
}

static ImFont *game_info_body_font()
{
	return settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
}

static std::string format_game_info_release_date(const std::string& value)
{
	if (value.size() < 8 || !std::all_of(value.begin(), value.begin() + 8, [](unsigned char c) { return std::isdigit(c); }))
		return value;
	return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);
}

static void draw_game_info_stat(const char *label, const std::string& value)
{
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.35f), "%s", T(label));
	ImGui::TextWrapped("%s", game_info_value(value));
	ImGui::PopFont();
}

struct GameInfoMediaEntry
{
	const char *label;
	std::string path;
	bool selected = false;
};

struct HoverGameInfoCache
{
	std::string gameId;
	GameBoxart art;
	std::string mixImagePath;
	std::string titleImagePath;
	std::string coverPath;
	std::string casePath;
	std::string screenshotPath;
	std::string fanArtPath;
	std::string titleScreenPath;
	std::string physicalPath;
	std::string manualPath;
	std::string primaryImagePath;
	std::string title;
	int selectedMediaIndex = 0;
	bool ready = false;
};

static void draw_game_info_media_label(const char *label, bool present)
{
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(present ? themeStatusColor("success", ImVec4(0.20f, 0.76f, 0.38f, 1.0f))
			: CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "media.status"), "%s", label);
	const bool hasRoomForNextLabel = ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize("Title Screen").x + uiScaled(18.0f);
	ImGui::PopFont();
	if (hasRoomForNextLabel)
		ImGui::SameLine(0.0f, uiScaled(10.0f));
}

static bool draw_game_info_image(const std::string& path, const ImVec2& size)
{
	if (path.empty())
		return false;
	ImguiFileTexture tex(path);
	tex.draw(size);
	return true;
}

static ImVec2 fit_game_info_image(float aspectRatio, ImVec2 boxSize, ImVec2 maxSize)
{
	if (aspectRatio <= 0.0f || boxSize.x <= 0.0f || boxSize.y <= 0.0f)
		return ImVec2(0.0f, 0.0f);

	ImVec2 drawSize(std::min(boxSize.x, maxSize.x), std::min(boxSize.y, maxSize.y));
	if (drawSize.x / drawSize.y > aspectRatio)
		drawSize.x = drawSize.y * aspectRatio;
	else
		drawSize.y = drawSize.x / aspectRatio;
	return drawSize;
}

static std::string game_info_path_filename(const std::string& path)
{
	const size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

static ImTextureID get_game_info_texture_id(const std::string& path)
{
	if (path.empty())
		return {};
	ImguiFileTexture tex(path);
	return tex.getId();
}

static bool draw_game_info_image_contained(const std::string& path, const ImVec2& boxSize, const ImVec2& maxDrawSize, ImVec2 *actualDrawSize = nullptr)
{
	const ImVec2 start = ImGui::GetCursorScreenPos();
	ImGui::Dummy(boxSize);

	ImTextureID id = get_game_info_texture_id(path);
	if (id == ImTextureID{})
		return false;

	const ImVec2 drawSize = fit_game_info_image(imguiDriver->getAspectRatio(id), boxSize, maxDrawSize);
	if (drawSize.x <= 0.0f || drawSize.y <= 0.0f)
		return false;

	if (actualDrawSize != nullptr)
		*actualDrawSize = drawSize;
	const ImVec2 drawPos(start.x + (boxSize.x - drawSize.x) * 0.5f, start.y + (boxSize.y - drawSize.y) * 0.5f);
	ImGui::GetWindowDrawList()->AddImage(id, drawPos, drawPos + drawSize);
	return true;
}

static bool draw_game_info_image_cover_cropped(const std::string& path, ImDrawList *drawList, const ImVec2& pMin, const ImVec2& pMax, ImU32 tint)
{
	ImTextureID id = get_game_info_texture_id(path);
	if (id == ImTextureID{})
		return false;

	const float boxW = pMax.x - pMin.x;
	const float boxH = pMax.y - pMin.y;
	const float imageAspect = imguiDriver->getAspectRatio(id);
	if (boxW <= 0.0f || boxH <= 0.0f || imageAspect <= 0.0f)
		return false;

	const float boxAspect = boxW / boxH;
	ImVec2 uv0(0.0f, 0.0f);
	ImVec2 uv1(1.0f, 1.0f);
	if (imageAspect > boxAspect)
	{
		const float visibleWidth = boxAspect / imageAspect;
		const float crop = (1.0f - visibleWidth) * 0.5f;
		uv0.x = crop;
		uv1.x = 1.0f - crop;
	}
	else
	{
		const float visibleHeight = imageAspect / boxAspect;
		const float crop = (1.0f - visibleHeight) * 0.5f;
		uv0.y = crop;
		uv1.y = 1.0f - crop;
	}
	drawList->AddImage(id, pMin, pMax, uv0, uv1, tint);
	return true;
}

static std::vector<GameInfoMediaEntry> make_game_info_media_entries(const HoverGameInfoCache& cache)
{
	std::vector<GameInfoMediaEntry> entries {
		{ "Mix", cache.mixImagePath, false },
		{ "Cover", !cache.coverPath.empty() ? cache.coverPath : cache.art.boxartPath, false },
		{ "Case", cache.casePath, false },
		{ "Screenshot", cache.screenshotPath, false },
		{ "Marquee", cache.titleImagePath, false },
		{ "Title Screen", cache.titleScreenPath, false },
		{ "Fan Art", cache.fanArtPath, false },
		{ "Physical", cache.physicalPath, false },
		{ "Manual", cache.manualPath, false },
	};
	if (!entries.empty())
		entries[std::clamp(cache.selectedMediaIndex, 0, (int)entries.size() - 1)].selected = true;
	return entries;
}

static std::string selected_game_info_media_path(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (media.selected)
			return media.path;
	return {};
}

static const GameInfoMediaEntry *selected_game_info_media(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (media.selected)
			return &media;
	return mediaEntries.empty() ? nullptr : &mediaEntries[0];
}

static int default_game_info_media_index(const HoverGameInfoCache& cache)
{
	std::vector<GameInfoMediaEntry> mediaEntries = make_game_info_media_entries(cache);
	for (int i = 0; i < (int)mediaEntries.size(); i++)
		if (!mediaEntries[i].path.empty())
			return i;
	return 0;
}

static std::string best_game_info_hero_background_path(const HoverGameInfoCache& cache)
{
	if (!cache.fanArtPath.empty())
		return cache.fanArtPath;
	if (!cache.screenshotPath.empty())
		return cache.screenshotPath;
	if (!cache.titleScreenPath.empty())
		return cache.titleScreenPath;
	if (!cache.mixImagePath.empty())
		return cache.mixImagePath;
	if (!cache.coverPath.empty())
		return cache.coverPath;
	if (!cache.casePath.empty())
		return cache.casePath;
	return {};
}

static void draw_game_info_chip(const std::string& text)
{
	if (text.empty())
		return;
	ImguiStyleVar rounding(ImGuiStyleVar_FrameRounding, uiScaled(10.0f));
	ImguiStyleVar padding(ImGuiStyleVar_FramePadding, ScaledVec2(10.0f, 4.0f));
	ImguiStyleColor button(ImGuiCol_Button, blendThemeColor(ImGuiCol_FrameBg, ImGuiCol_ButtonActive, 0.24f, 0.92f));
	ImguiStyleColor buttonHovered(ImGuiCol_ButtonHovered, blendThemeColor(ImGuiCol_FrameBgHovered, ImGuiCol_ButtonActive, 0.38f, 0.95f));
	ImguiStyleColor buttonActive(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
	ImGui::Button(text.c_str());
}

static void draw_game_info_chip_row(const GameMedia& game)
{
	std::vector<std::string> chips;
	if (!game.players.empty())
		chips.push_back(game.players);
	if (!game.releaseDate.empty())
		chips.push_back(format_game_info_release_date(game.releaseDate).substr(0, 4));
	if (!game.genre.empty())
		chips.push_back(game.genre);
	if (!game.developer.empty())
		chips.push_back(game.developer);

	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
	const float avail = ImGui::GetContentRegionAvail().x;
	while (!chips.empty())
	{
		float totalWidth = spacing * static_cast<float>(chips.size() - 1);
		for (const std::string& chip : chips)
			totalWidth += ImGui::CalcTextSize(chip.c_str()).x + padding;
		if (totalWidth <= avail || chips.size() <= 2)
		{
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - totalWidth) * 0.5f));
			for (size_t i = 0; i < chips.size(); i++)
			{
				if (i != 0)
					ImGui::SameLine();
				draw_game_info_chip(chips[i]);
			}
			break;
		}
		chips.pop_back();
	}
}

static bool draw_game_info_badge(const GameInfoMediaEntry& media, int index)
{
	const bool found = !media.path.empty();
	const ImVec4 bg = media.selected ? blendThemeColor(ImGuiCol_Button, ImGuiCol_ButtonActive, 0.85f)
			: found ? blendThemeColor(ImGuiCol_FrameBg, ImGuiCol_ButtonActive, 0.28f, 0.88f)
			: ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
	const ImVec4 text = media.selected ? ImGui::GetStyle().Colors[ImGuiCol_Text]
			: found ? themeStatusColor("success", ImVec4(0.20f, 0.76f, 0.38f, 1.0f))
			: CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "media.status");

	ImguiStyleVar rounding(ImGuiStyleVar_FrameRounding, uiScaled(11.0f));
	ImguiStyleVar padding(ImGuiStyleVar_FramePadding, ScaledVec2(9.0f, 4.0f));
	ImguiStyleColor button(ImGuiCol_Button, bg);
	ImguiStyleColor buttonHovered(ImGuiCol_ButtonHovered, bg);
	ImguiStyleColor buttonActive(ImGuiCol_ButtonActive, bg);
	ImguiStyleColor textColor(ImGuiCol_Text, text);
	const std::string label = std::string(found ? ICON_FA_CIRCLE " " : ICON_FA_CIRCLE_DOT " ") + media.label
			+ "##library_media_badge_" + std::to_string(index);
	const bool clicked = ImGui::Button(label.c_str());
	if (!found && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s media missing", media.label);
	return clicked;
}

static int draw_game_info_badge_wrap(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	int clickedIndex = -1;
	for (size_t i = 0; i < mediaEntries.size(); i++)
	{
		const float nextWidth = ImGui::CalcTextSize(mediaEntries[i].label).x + ImGui::GetStyle().FramePadding.x * 2.0f + uiScaled(26.0f);
		if (i != 0 && ImGui::GetContentRegionAvail().x > nextWidth + uiScaled(12.0f))
			ImGui::SameLine();
		if (draw_game_info_badge(mediaEntries[i], (int)i))
			clickedIndex = (int)i;
	}
	return clickedIndex;
}

static void draw_game_info_detail_row(const char *label, const std::string& value)
{
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.35f), "%s", T(label));
	ImGui::SameLine(uiScaled(110.0f));
	ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_Text], "%s", game_info_value(value));
	ImGui::PopFont();
}

static void draw_library_hover_hero(const HoverGameInfoCache& cache, const GameMedia& game, float width)
{
	const ImVec2 heroSize(width, uiScaled(144.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p + ScaledVec2(0.0f, 4.0f), p + heroSize + ScaledVec2(0.0f, 4.0f),
			alphaOverride(ImGui::GetColorU32(ImGuiCol_BorderShadow), 0.45f), uiScaled(14.0f));
	dl->AddRectFilled(p, p + heroSize, alphaOverride(ImGui::GetColorU32(ImGuiCol_ChildBg), 0.95f), uiScaled(14.0f));
	if (draw_game_info_image_cover_cropped(best_game_info_hero_background_path(cache), dl, p, p + heroSize, IM_COL32(255, 255, 255, 62)))
		dl->AddRectFilled(p, p + heroSize, alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.58f), uiScaled(14.0f));
	dl->AddRectFilled(ImVec2(p.x, p.y + heroSize.y - uiScaled(48.0f)), p + heroSize,
			alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.44f), uiScaled(14.0f));
	dl->AddRectFilled(p + ScaledVec2(24.0f, 16.0f), ImVec2(p.x + heroSize.x - uiScaled(24.0f), p.y + uiScaled(84.0f)),
			alphaOverride(ImGui::GetColorU32(ImGuiCol_HeaderActive), 0.12f), uiScaled(18.0f));
	dl->AddRect(p, p + heroSize, alphaOverride(ImGui::GetColorU32(ImGuiCol_Border), 0.82f), uiScaled(14.0f), 0, uiScaled(1.0f));

	if (ImGui::BeginChild("##libraryHoverHero", heroSize, false,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::SetCursorPosY(uiScaled(14.0f));
		ImVec2 titleDrawSize;
		const bool drewLogo = draw_game_info_image_contained(cache.titleImagePath,
				ImVec2(ImGui::GetContentRegionAvail().x, uiScaled(78.0f)),
				ImVec2(std::min(width * 0.68f, uiScaled(680.0f)), uiScaled(78.0f)),
				&titleDrawSize);
		if (!drewLogo)
		{
			ImGui::PushFont(game_info_title_font());
			const ImVec2 textSize = ImGui::CalcTextSize(cache.title.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x);
			ImGui::SetCursorPosX(std::max(uiScaled(10.0f), (ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f));
			ImGui::TextWrapped("%s", cache.title.c_str());
			ImGui::PopFont();
		}
		ImGui::SetCursorPosY(uiScaled(106.0f));
		draw_game_info_chip_row(game);
	}
	ImGui::EndChild();
}

static void draw_library_hover_media_card(const HoverGameInfoCache& cache, const std::vector<GameInfoMediaEntry>& mediaEntries, float width, float height)
{
	if (!ImGui::BeginChild("##libraryHoverMedia", ImVec2(width, height), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::EndChild();
		return;
	}

	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.4f), "%s", T("Media Preview"));
	ImGui::PopFont();
	const GameInfoMediaEntry *selected = selected_game_info_media(mediaEntries);
	const std::string selectedPath = selected != nullptr ? selected->path : selected_game_info_media_path(mediaEntries);
	const char *selectedLabel = selected != nullptr ? selected->label : T("Media");
	const bool found = !selectedPath.empty();
	const float previewHeight = std::max(uiScaled(180.0f), height - uiScaled(158.0f));
	const ImVec2 previewSize(ImGui::GetContentRegionAvail().x, previewHeight);
	const ImVec2 previewPos = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(previewPos, previewPos + previewSize, alphaOverride(ImGui::GetColorU32(ImGuiCol_ChildBg), 0.72f), uiScaled(9.0f));
	dl->AddRect(previewPos, previewPos + previewSize, alphaOverride(ImGui::GetColorU32(ImGuiCol_Border), 0.55f), uiScaled(9.0f));
	if (!draw_game_info_image_contained(selectedPath, previewSize, previewSize - ScaledVec2(14.0f, 14.0f)))
	{
		const std::string missingText = std::string(selectedLabel) + " missing";
		const char *missing = missingText.c_str();
		const ImVec2 textSize = ImGui::CalcTextSize(missing);
		dl->AddCircle(previewPos + previewSize * 0.5f - ScaledVec2(0.0f, 22.0f), uiScaled(14.0f),
				alphaOverride(ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.86f), 32, uiScaled(2.0f));
		dl->AddText(previewPos + (previewSize - textSize) * 0.5f + ScaledVec2(0.0f, 18.0f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled), missing);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.35f), "%s", T("Current Media"));
	ImGui::SameLine(uiScaled(128.0f));
	ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_Text], "%s", selectedLabel);
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.35f), "%s", T("Status"));
	ImGui::SameLine(uiScaled(128.0f));
	ImGui::TextColored(found ? themeStatusColor("success", ImVec4(0.20f, 0.76f, 0.38f, 1.0f))
			: CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "media.status"),
			"%s", found ? T("Found") : T("Missing"));
	if (found)
	{
		ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.35f), "%s", T("File"));
		ImGui::SameLine(uiScaled(128.0f));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
		ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_Text], "%s", game_info_path_filename(selectedPath).c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::PopFont();
	ImGui::EndChild();
}

static void draw_library_hover_description_card(const GameMedia& game, float height)
{
	if (!ImGui::BeginChild("##libraryHoverDetails", ImVec2(0.0f, height), true))
	{
		ImGui::EndChild();
		return;
	}

	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.4f), "%s", T("Details"));
	ImGui::Spacing();
	draw_game_info_detail_row("Players", game.players);
	draw_game_info_detail_row("Released", format_game_info_release_date(game.releaseDate));
	draw_game_info_detail_row("Genre", game.genre);
	draw_game_info_detail_row("Developer", game.developer);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.4f), "%s", T("Description"));
	ImGui::Spacing();
	const float wrapWidth = std::min(ImGui::GetContentRegionAvail().x, uiScaled(760.0f));
	if (ImGui::BeginChild("##libraryHoverDescriptionText", ImVec2(0.0f, 0.0f), false))
	{
		ImGui::PushFont(game_info_body_font());
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
		if (game.desc.empty())
			ImGui::TextDisabled("%s", T("No description available."));
		else
			ImGui::TextUnformatted(game.desc.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	}
	ImGui::EndChild();
	ImGui::EndChild();
}

static int draw_library_hover_status_strip(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	int foundCount = 0;
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (!media.path.empty())
			foundCount++;

	if (!ImGui::BeginChild("##libraryHoverMediaStatus", ImVec2(0.0f, uiScaled(76.0f)), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::EndChild();
		return -1;
	}

	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(blendThemeColor(ImGuiCol_TextDisabled, ImGuiCol_HeaderActive, 0.4f), "%s", T("Media"));
	ImGui::SameLine();
	ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_Text], "%d / %d found", foundCount, (int)mediaEntries.size());
	ImGui::PopFont();
	int clickedIndex = -1;
	if (ImGui::BeginChild("##libraryHoverMediaBadges", ImVec2(0.0f, uiScaled(34.0f)), false,
			ImGuiWindowFlags_HorizontalScrollbar))
		clickedIndex = draw_game_info_badge_wrap(mediaEntries);
	ImGui::EndChild();
	ImGui::EndChild();
	return clickedIndex;
}

static void draw_library_game_info_hover(const GameMedia& game, const GameBoxart* displayArt = nullptr)
{
#if defined(__ANDROID__)
	(void)game;
	(void)displayArt;
#else
	static std::string hoveredGameId;
	static double hoverStartTime = 0.0;
	static ImVec2 hoverStartMousePos;
	static HoverGameInfoCache hoverCache;
	static ImVec2 hoverPanelPos;
	static ImVec2 hoverPanelMin;
	static ImVec2 hoverPanelMax;
	static bool hasHoverPanelPos = false;

	if (resetLibraryGameInfoHoverState)
	{
		hoveredGameId.clear();
		hoverStartTime = 0.0;
		hoverStartMousePos = ImVec2();
		hoverCache = {};
		hasHoverPanelPos = false;
		hoverPanelMin = ImVec2();
		hoverPanelMax = ImVec2();
		resetLibraryGameInfoHoverState = false;
	}

	const std::string gameId = !game.path.empty() ? game.path : (!game.fileName.empty() ? game.fileName : game.name);
	const ImVec2 mousePos = ImGui::GetMousePos();
	const ImGuiHoveredFlags hoverFlags = ImGuiHoveredFlags_RectOnly | ImGuiHoveredFlags_NoNavOverride;
	const bool itemHovered = ImGui::IsItemHovered(hoverFlags) && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
	const bool panelHovered = hoveredGameId == gameId
			&& mousePos.x >= hoverPanelMin.x && mousePos.x <= hoverPanelMax.x
			&& mousePos.y >= hoverPanelMin.y && mousePos.y <= hoverPanelMax.y;
	if (!itemHovered && !panelHovered)
		return;

	const float mouseDeltaX = mousePos.x - hoverStartMousePos.x;
	const float mouseDeltaY = mousePos.y - hoverStartMousePos.y;
	const float movementThreshold = uiScaled(6.0f);
	if (itemHovered && (hoveredGameId != gameId || mouseDeltaX * mouseDeltaX + mouseDeltaY * mouseDeltaY > movementThreshold * movementThreshold))
	{
		hoveredGameId = gameId;
		hoverStartTime = ImGui::GetTime();
		hoverStartMousePos = mousePos;
		hoverCache = {};
		hasHoverPanelPos = false;
	}
	const double hoverElapsed = ImGui::GetTime() - hoverStartTime;
	if (hoverElapsed >= 1.0 && (!hoverCache.ready || hoverCache.gameId != gameId))
	{
		hoverCache.gameId = gameId;
		hoverCache.art = displayArt != nullptr ? *displayArt : getLibraryDisplayArtwork(game, 0.0);
		hoverCache.mixImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::MixImage);
		hoverCache.titleImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Title);
		hoverCache.coverPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Cover);
		hoverCache.casePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Case);
		hoverCache.screenshotPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Screenshot);
		hoverCache.fanArtPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::FanArt);
		hoverCache.titleScreenPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::TitleScreen);
		hoverCache.physicalPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Physical);
		hoverCache.manualPath = !game.manualPath.empty()
				? game.manualPath : boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Manual);
		hoverCache.primaryImagePath = !hoverCache.mixImagePath.empty() ? hoverCache.mixImagePath : hoverCache.art.boxartPath;
		hoverCache.title = game.name;
		if (hoverCache.title.empty() || hoverCache.title == game.fileName)
			hoverCache.title = hoverCache.art.name;
		if (hoverCache.title.empty())
			hoverCache.title = game.fileName;
		hoverCache.selectedMediaIndex = default_game_info_media_index(hoverCache);
		hoverCache.ready = true;
	}
	if (hoverElapsed < LIBRARY_GAME_INFO_HOVER_SECONDS || !hoverCache.ready)
		return;

	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	const ImVec2 displaySize = viewport->Size;
	const float hoverWidth = std::clamp(displaySize.x * 0.72f, uiScaled(820.0f), uiScaled(1180.0f));
	const float hoverHeight = std::clamp(displaySize.y * 0.74f, uiScaled(560.0f), uiScaled(720.0f));
	if (itemHovered || !hasHoverPanelPos)
	{
		hoverPanelPos = ImGui::GetMousePos() + ScaledVec2(18.0f, 18.0f);
		if (hoverPanelPos.x + hoverWidth > viewport->Pos.x + displaySize.x - uiScaled(12.0f))
			hoverPanelPos.x = ImGui::GetMousePos().x - hoverWidth - uiScaled(18.0f);
		if (hoverPanelPos.y + hoverHeight > viewport->Pos.y + displaySize.y - uiScaled(12.0f))
			hoverPanelPos.y = viewport->Pos.y + displaySize.y - hoverHeight - uiScaled(12.0f);
		hoverPanelPos.x = std::max(viewport->Pos.x + uiScaled(12.0f), hoverPanelPos.x);
		hoverPanelPos.y = std::max(viewport->Pos.y + uiScaled(12.0f), hoverPanelPos.y);
		hasHoverPanelPos = true;
	}
	hoverPanelMin = hoverPanelPos;
	hoverPanelMax = hoverPanelPos + ImVec2(hoverWidth, hoverHeight);

	ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, viewport->Pos + viewport->Size,
			alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.58f));
	ImGui::SetNextWindowPos(hoverPanelPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(hoverWidth, hoverHeight), ImGuiCond_Always);
	ImguiStyleVar windowPadding(ImGuiStyleVar_WindowPadding, ScaledVec2(18.0f, 16.0f));
	ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(10.0f, 6.0f));
	ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ScaledVec2(12.0f, 8.0f));
	ImguiStyleVar windowRounding(ImGuiStyleVar_WindowRounding, uiScaled(12.0f));
	ImguiStyleVar childRounding(ImGuiStyleVar_ChildRounding, uiScaled(10.0f));
	ImguiStyleVar frameRounding(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
	ImguiStyleVar borderSize(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImguiStyleColor borderColor(ImGuiCol_Border, blendThemeColor(ImGuiCol_Border, ImGuiCol_HeaderActive, 0.35f, 0.65f));
	ImguiStyleColor childBgColor(ImGuiCol_ChildBg, blendThemeColor(ImGuiCol_WindowBg, ImGuiCol_ChildBg, 0.45f, 0.96f));
	ImguiStyleColor windowBgColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
	const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration
			| ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoNav
			| ImGuiWindowFlags_NoMove;
	if (!ImGui::Begin("##libraryGameInfoHover", nullptr, windowFlags))
	{
		ImGui::End();
		return;
	}

	const float width = ImGui::GetContentRegionAvail().x;
	const std::vector<GameInfoMediaEntry> mediaEntries = make_game_info_media_entries(hoverCache);
	draw_library_hover_hero(hoverCache, game, width);

	ImGui::Spacing();
	const float mediaWidth = std::clamp(width * 0.31f, uiScaled(300.0f), uiScaled(390.0f));
	const float statusHeight = uiScaled(76.0f);
	const float mainHeight = std::max(uiScaled(300.0f), ImGui::GetContentRegionAvail().y - statusHeight - uiScaled(10.0f));
	draw_library_hover_media_card(hoverCache, mediaEntries, mediaWidth, mainHeight);

	ImGui::SameLine(0.0f, uiScaled(14.0f));
	draw_library_hover_description_card(game, mainHeight);

	ImGui::Spacing();
	const int clickedMediaIndex = draw_library_hover_status_strip(mediaEntries);
	if (clickedMediaIndex >= 0)
		hoverCache.selectedMediaIndex = clickedMediaIndex;
	ImGui::End();
#endif
}

static void gui_display_game_info()
{
	const GameMedia game = selectedGameForInfo;
	const GameBoxart art = getLibraryDisplayArtwork(game, 0.0);
	const std::string mixImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::MixImage);
	const std::string titleImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Title);
	const std::string coverPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Cover);
	const std::string casePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Case);
	const std::string screenshotPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Screenshot);
	const std::string fanArtPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::FanArt);
	const std::string titleScreenPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::TitleScreen);
	const std::string physicalPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Physical);
	const std::string manualPath = !game.manualPath.empty()
			? game.manualPath : boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Manual);
	const std::string primaryImagePath = !mixImagePath.empty() ? mixImagePath : art.boxartPath;
	std::string title = game.name;
	if (title.empty() || title == game.fileName)
		title = art.name;
	if (title.empty())
		title = game.fileName;

	ImVec2 windowPos(static_cast<float>(insetLeft), static_cast<float>(insetTop));
	ImVec2 windowSize(std::max(0, settings.display.width - insetLeft - insetRight),
			std::max(0, settings.display.height - insetTop - insetBottom));
	ImGui::SetNextWindowPos(windowPos);
	ImGui::SetNextWindowSize(windowSize);
	ImguiStyleVar _1(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _2(ImGuiStyleVar_WindowBorderSize, 0);

	if (ImGui::Begin("##gameInfo", nullptr, ImGuiWindowFlags_NoDecoration))
	{
		ImguiStyleVar _3(ImGuiStyleVar_FramePadding, ScaledVec2(18, 12));
		ImguiStyleVar _4(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 10));

		if (ImGui::Button(T("Close")))
			gui_setState(GuiState::Main);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
			gui_setState(GuiState::Main);

		ImGui::Dummy(ScaledVec2(0, 4));
		if (!titleImagePath.empty())
		{
			const float headerWidth = std::min(windowSize.x - ImGui::GetStyle().WindowPadding.x * 2.0f, uiScaled(520.0f));
			draw_game_info_image(titleImagePath, ImVec2(headerWidth, uiScaled(92.0f)));
		}
		else
		{
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::TextWrapped("%s", title.c_str());
			ImGui::PopFont();
		}
		ImGui::Separator();

		const float contentWidth = ImGui::GetContentRegionAvail().x;
		const float leftWidth = std::min(contentWidth * 0.42f, uiScaled(430.0f));
		const float rightWidth = std::max(uiScaled(260.0f), contentWidth - leftWidth - ImGui::GetStyle().ItemSpacing.x);
		const float panelHeight = ImGui::GetContentRegionAvail().y - uiScaled(8.0f);

		if (ImGui::BeginChild("##gameInfoMedia", ImVec2(leftWidth, panelHeight), false))
		{
			const float previewSize = std::min(leftWidth, panelHeight * 0.78f);
			if (!draw_game_info_image(primaryImagePath, ImVec2(previewSize, previewSize)))
			{
				ImGui::Dummy(ImVec2(previewSize, previewSize * 0.6f));
				ImGui::TextDisabled("%s", T("No media preview"));
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("##gameInfoDetails", ImVec2(rightWidth, panelHeight), false))
		{
			if (ImGui::BeginTable("##gameInfoStats", 2, ImGuiTableFlags_SizingStretchSame))
			{
				ImGui::TableNextColumn();
				draw_game_info_stat("Players", game.players);
				ImGui::TableNextColumn();
				draw_game_info_stat("Released", format_game_info_release_date(game.releaseDate));
				ImGui::TableNextColumn();
				draw_game_info_stat("Genre", game.genre);
				ImGui::TableNextColumn();
				draw_game_info_stat("Developer", game.developer);
				ImGui::EndTable();
			}

			ImGui::Separator();
			ImGui::TextDisabled("%s", T("Description"));
			if (game.desc.empty())
				ImGui::TextDisabled("%s", T("No description available."));
			else
				ImGui::TextWrapped("%s", game.desc.c_str());

			ImGui::Separator();
			ImGui::TextDisabled("%s", T("MEDIA:"));
			ImGui::SameLine(0.0f, uiScaled(10.0f));
			draw_game_info_media_label("Mix", !mixImagePath.empty());
			draw_game_info_media_label("Cover", !coverPath.empty());
			draw_game_info_media_label("Case", !casePath.empty());
			draw_game_info_media_label("Screenshot", !screenshotPath.empty());
			draw_game_info_media_label("Marquee", !titleImagePath.empty());
			draw_game_info_media_label("Title Screen", !titleScreenPath.empty());
			draw_game_info_media_label("Fan Art", !fanArtPath.empty());
			draw_game_info_media_label("Physical", !physicalPath.empty());
			draw_game_info_media_label("Manual", !manualPath.empty());
			ImGui::NewLine();

			if (!manualPath.empty())
			{
				ImGui::TextDisabled("%s", T("Manual"));
				ImGui::TextWrapped("%s", manualPath.c_str());
			}
		}
		ImGui::EndChild();
	}
	ImGui::End();
}

static void gui_set_library_game_info(const GameMedia& game)
{
	selectedGameForInfo = game;
	gui_setState(GuiState::GameInfo);
}

static void resetLibraryLongPress()
{
	libraryLongPress = {};
	touchedLibraryItemThisFrame = false;
}

static void resetLibraryGameInfoHover()
{
	resetLibraryGameInfoHoverState = true;
}

static void updateLibraryLongPress(const GameMedia& game, const std::string& gameId, bool itemActive)
{
#if defined(__ANDROID__)
	if (!mouseTouchscreen || gui_state != GuiState::Main)
		return;
#else
	(void)game;
	(void)gameId;
	(void)itemActive;
#endif

#if defined(__ANDROID__)
	if (!itemActive)
		return;

	touchedLibraryItemThisFrame = true;
	if (!libraryLongPress.tracking || libraryLongPress.gameId != gameId)
	{
		libraryLongPress = {};
		libraryLongPress.tracking = true;
		libraryLongPress.startTime = ImGui::GetTime();
		libraryLongPress.gameId = gameId;
		libraryLongPress.game = game;
		return;
	}

	if (!libraryLongPress.opened && (ImGui::GetTime() - libraryLongPress.startTime >= GAME_INFO_LONG_PRESS_SECONDS))
	{
		libraryLongPress.opened = true;
		libraryLongPress.tracking = false;
		gui_set_library_game_info(game);
	}
#endif
}

static bool gameImageButton(ImguiTexture& texture, const std::string& tooltip, ImVec2 size,
		const std::string& gameName, float fallbackTitleSize = 0.0f)
{
	(void)tooltip;
	// Library artwork keeps a two-pixel highlight border regardless of the
	// general UI scale. ImageButton otherwise inherits scaled FramePadding,
	// which makes the artwork shrink inside an unchanged library card.
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
	const float radius = CustomTheme::activeLayout().artworkCornerRadius;
	bool pressed = texture.button("##imagebutton", size, gameName, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1),
			fallbackTitleSize, radius < 0.0f ? 0.0f : uiScaled(radius));
	ImGui::PopStyleVar();

    return pressed;
}

static double getLibraryIconAnimationClock()
{
	static double animationStart = 0.0;
	static bool libraryWasOpen = false;
	const bool libraryOpen = gui_state == GuiState::Main || gui_state == GuiState::SelectDisk;

	if (libraryOpen && !libraryWasOpen)
		animationStart = ImGui::GetTime();
	libraryWasOpen = libraryOpen;
	return std::max(0.0, ImGui::GetTime() - animationStart);
}

static GameBoxart getLibraryDisplayArtwork(const GameMedia& game, double animationClock)
{
	GameBoxart art;
	if (game.device)
		return art;

	art = boxart.getBoxartAndLoad(game);
	const std::string localCoverOverride = boxart.getLibraryCoverMediaPath(game);
	if (!localCoverOverride.empty())
		art.boxartPath = localCoverOverride;
	const auto source = static_cast<config::LibraryImageSourceMode>(config::LibraryImageSource.get());
	switch (source)
	{
	case config::LibraryImageSourceMode::CurrentArtwork:
		return art;

	case config::LibraryImageSourceMode::VmuSaveIcon:
	case config::LibraryImageSourceMode::VmuThenCurrentArtwork: {
		const bool animate = config::VmuIconMode.get() == static_cast<int>(config::VmuIconPlaybackMode::Active);
		const std::string vmuIconPath = getCachedVmuIconPath(game, art.uniqueId, animate, animationClock);
		if (!vmuIconPath.empty())
			art.boxartPath = vmuIconPath;
		return art;
	}

	case config::LibraryImageSourceMode::CurrentArtworkThenVmu:
		if (art.boxartPath.empty())
		{
			const bool animate = config::VmuIconMode.get() == static_cast<int>(config::VmuIconPlaybackMode::Active);
			const std::string vmuIconPath = getCachedVmuIconPath(game, art.uniqueId, animate, animationClock);
			if (!vmuIconPath.empty())
				art.boxartPath = vmuIconPath;
		}
		return art;
	}

	return art;
}

static std::string formatLibraryRegion(u32 region)
{
	if (region == 0)
		return "Unknown";

	std::string value;
	if (region & GameBoxart::JAPAN)
		value += "JP";
	if (region & GameBoxart::USA)
	{
		if (!value.empty())
			value += "/";
		value += "US";
	}
	if (region & GameBoxart::EUROPE)
	{
		if (!value.empty())
			value += "/";
		value += "EU";
	}
	return value.empty() ? "Unknown" : value;
}

static std::string formatLibrarySize(size_t size)
{
	if (size == 0)
		return "";

	constexpr size_t KiB = 1024;
	constexpr size_t MiB = KiB * 1024;
	if (size < MiB)
		return std::to_string((size + KiB - 1) / KiB) + " KB";
	return std::to_string((size + MiB - 1) / MiB) + " MB";
}

static std::string formatLibraryPlaytime(u64 seconds)
{
	constexpr u64 secondsPerMinute = 60;
	constexpr u64 secondsPerHour = secondsPerMinute * 60;
	constexpr u64 secondsPerDay = secondsPerHour * 24;
	const auto value = static_cast<unsigned long long>(seconds);
	if (seconds >= secondsPerDay)
		return strprintf(T("%llud %lluh"), value / secondsPerDay, (seconds % secondsPerDay) / secondsPerHour);
	if (seconds >= secondsPerHour)
		return strprintf(T("%lluh %llum"), value / secondsPerHour, (seconds % secondsPerHour) / secondsPerMinute);
	if (seconds >= secondsPerMinute)
		return strprintf(T("%llum"), value / secondsPerMinute);
	return strprintf(T("%llus"), value);
}

static bool drawLibraryFavouriteButton(const GameMedia& game, bool coverOverlay = false, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
	if (gui_state == GuiState::SelectDisk || game.device || game.path.empty())
		return false;
	const bool favourite = library::isFavourite(game.path, config::LibraryFavourites.get());
	const char *label = favourite ? T("Remove from favourites") : T("Add to favourites");
	const ImVec4 favouriteColor = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
	if (coverOverlay)
	{
		const float scale = size.x / uiScaled(34.0f);
		ImVec4 buttonColor = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
		ImVec4 hoveredColor = ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered];
		ImVec4 activeColor = ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive];
		ImVec4 borderColor = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
		buttonColor.w = 0.92f;
		hoveredColor.w = 0.98f;
		activeColor.w = 1.0f;
		borderColor.w = 0.48f;
		ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoveredColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
		ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(5.0f) * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, scale);
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
	}
	ImGui::PushStyleColor(ImGuiCol_Text, favourite ? favouriteColor : ImGui::GetStyle().Colors[ImGuiCol_Text]);
	const bool pressed = ImGui::Button((std::string(ICON_FA_STAR) + "##favourite_" + game.path).c_str(), size);
	ImGui::PopStyleColor();
	if (coverOverlay)
		ImGui::PopFont();
	gameTooltip(label);
	if (coverOverlay)
	{
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(4);
	}
	if (!pressed)
		return false;
	std::vector<std::string> favourites = config::LibraryFavourites.get();
	library::setFavourite(game.path, !favourite, favourites);
	config::LibraryFavourites.set(favourites);
	config::LibraryFavourites.save();
	return true;
}

static bool drawLibraryCoverButton(ImguiTexture& texture, const std::string& tooltip, const ImVec2& size,
		const std::string& title, float fallbackTitleSize, const GameMedia& game, const GameBoxart *art,
		float overlayScale, bool& favouritePressed)
{
	if (gui_state == GuiState::SelectDisk || game.device || game.path.empty())
	{
		const bool pressed = gameImageButton(texture, tooltip, size, title, fallbackTitleSize);
		updateLibraryLongPress(game, game.path.empty() ? "bios" : game.path, ImGui::IsItemActive());
		if (ImGui::IsItemHovered())
			draw_library_game_info_hover(game, art);
		return pressed;
	}

	const ImVec2 coverMin = ImGui::GetCursorScreenPos();
	const ImVec2 buttonSize(uiScaled(34.0f) * overlayScale, uiScaled(34.0f) * overlayScale);
	const float inset = uiScaled(6.0f) * overlayScale;
	const ImVec2 starMin(coverMin.x + size.x - buttonSize.x - inset, coverMin.y + inset);
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	ImDrawListSplitter splitter;
	splitter.Split(drawList, 2);
	splitter.SetCurrentChannel(drawList, 1);
	ImGui::SetCursorScreenPos(starMin);
	favouritePressed = drawLibraryFavouriteButton(game, true, buttonSize);
	splitter.SetCurrentChannel(drawList, 0);
	ImGui::SetCursorScreenPos(coverMin);
	const bool pressed = gameImageButton(texture, tooltip, size, title, fallbackTitleSize);
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool mouseOverStar = mouse.x >= starMin.x && mouse.x <= starMin.x + buttonSize.x
			&& mouse.y >= starMin.y && mouse.y <= starMin.y + buttonSize.y;
	splitter.Merge(drawList);
	updateLibraryLongPress(game, game.path, ImGui::IsItemActive() && !mouseOverStar);
	if (!mouseOverStar)
		draw_library_game_info_hover(game, art);
	return pressed;
}

static void startLibraryGame(const GameMedia& game, const GameBoxart& art, bool resume)
{
	librarySavestateCacheInvalidated = true;
	// Recheck on activation: another save may have been written since the card was rendered.
	const SavestateInfo state = resume ? dc_getLatestStateForGame(game.path) : SavestateInfo{};
	if (resume && state.path.empty())
	{
		gui_error(T("No save state is available for this game. Choose Start Fresh to play."));
		return;
	}
	settings.content.title = art.name;
	if (settings.content.title.empty() || settings.content.title == game.fileName)
		settings.content.title = get_file_basename(game.fileName);
	const std::string path = game.path;
	scanner.get_mutex().unlock();
	gui_start_library_game(path, resume, state.path);
	scanner.get_mutex().lock();
}

static bool libraryStateThumbnailCacheInvalidated = false;

class LibraryStateThumbnail : public ImguiTexture
{
public:
	LibraryStateThumbnail(const SavestateInfo& state) : path(state.path), modified(state.creationDate) {}

	ImTextureID getId() override
	{
		if (path.empty() || modified == 0 || imguiDriver == nullptr)
			return {};
		const std::string textureName = "library_state_thumbnail:" + path;
		static std::vector<Entry> cache;
		if (libraryStateThumbnailCacheInvalidated)
		{
			for (const Entry& entry : cache)
				imguiDriver->deleteTexture("library_state_thumbnail:" + entry.path);
			cache.clear();
			libraryStateThumbnailCacheInvalidated = false;
		}
		auto found = std::find_if(cache.begin(), cache.end(), [this](const Entry& entry) { return entry.path == path; });
		if (found != cache.end())
		{
			if (found->modified == modified)
			{
				// Keep failed previews cached; retry only when a previously uploaded texture was lost.
				if (found->id == ImTextureID{})
					return {};
				found->id = imguiDriver->getTexture(textureName);
				if (found->id != ImTextureID{})
					return found->id;
				cache.erase(found);
			}
			else
			{
				imguiDriver->deleteTexture(textureName);
				cache.erase(found);
			}
		}
		if (cache.size() >= 16)
		{
			imguiDriver->deleteTexture("library_state_thumbnail:" + cache.front().path);
			cache.erase(cache.begin());
		}

		Entry entry { path, modified, {} };
		std::vector<u8> pngData;
		dc_getStateScreenshotForPath(path, pngData);
		int width = 0, height = 0, channels = 0;
		if (!pngData.empty() && pngData.size() <= 64 * 1024 * 1024
				&& stbi_info_from_memory(pngData.data(), static_cast<int>(pngData.size()), &width, &height, &channels)
				&& width > 0 && height > 0 && width <= 2048 && height <= 2048
				&& static_cast<u64>(width) * height <= 4'000'000)
		{
			stbi_set_flip_vertically_on_load_thread(0);
			u8 *pixels = stbi_load_from_memory(pngData.data(), static_cast<int>(pngData.size()), &width, &height, &channels, STBI_rgb_alpha);
			if (pixels != nullptr)
			{
				try {
					entry.id = imguiDriver->updateTextureAndAspectRatio(textureName, pixels, width, height, false);
				} catch (...) {
					// Keep artwork visible if the renderer cannot upload the preview.
				}
				stbi_image_free(pixels);
			}
		}
		cache.push_back(entry);
		return entry.id;
	}

private:
	struct Entry { std::string path; time_t modified; ImTextureID id; };
	std::string path;
	time_t modified;
};

static void drawLibraryStatePreview(const SavestateInfo& state)
{
	if (!ImGui::IsItemHovered())
		return;
	LibraryStateThumbnail texture(state);
	if (texture.getId() != ImTextureID{})
	{
		ImGui::BeginTooltip();
		texture.draw(ImVec2(uiScaled(240.0f), uiScaled(135.0f)));
		ImGui::EndTooltip();
	}
}

static void centerTableCellCursor(const ImVec2& contentSize, float rowContentHeight, bool centerX)
{
	ImVec2 pos = ImGui::GetCursorScreenPos();
	pos.y += std::max(0.0f, (rowContentHeight - contentSize.y) * 0.5f);
	if (centerX)
		pos.x += std::max(0.0f, (ImGui::GetContentRegionAvail().x - contentSize.x) * 0.5f);
	ImGui::SetCursorScreenPos(pos);
}

static void textTableCellCentered(const std::string& text, float rowContentHeight, bool centerX = true)
{
	if (text.empty())
		return;
	centerTableCellCursor(ImGui::CalcTextSize(text.c_str()), rowContentHeight, centerX);
	ImGui::TextUnformatted(text.c_str());
}

#ifdef TARGET_UWP
void gui_load_game()
{
	using namespace Windows::Storage;
	using namespace Concurrency;

	auto picker = ref new Pickers::FileOpenPicker();
	picker->ViewMode = Pickers::PickerViewMode::List;

	picker->FileTypeFilter->Append(".chd");
	picker->FileTypeFilter->Append(".gdi");
	picker->FileTypeFilter->Append(".cue");
	picker->FileTypeFilter->Append(".cdi");
	picker->FileTypeFilter->Append(".zip");
	picker->FileTypeFilter->Append(".7z");
	picker->FileTypeFilter->Append(".elf");
	if (!config::HideLegacyNaomiRoms)
	{
		picker->FileTypeFilter->Append(".bin");
		picker->FileTypeFilter->Append(".lst");
		picker->FileTypeFilter->Append(".dat");
	}
	picker->SuggestedStartLocation = Pickers::PickerLocationId::DocumentsLibrary;

	create_task(picker->PickSingleFileAsync()).then([](StorageFile ^file) {
		if (file)
		{
			NOTICE_LOG(COMMON, "Picked file: %S", file->Path->Data());
			nowide::stackstring path;
			if (path.convert(file->Path->Data()))
				gui_start_game(path.get());
		}
	});
}
#endif

static void gui_display_content()
{
#if defined(__ANDROID__)
	touchedLibraryItemThisFrame = false;
#endif
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration);
	// Draw theme artwork first so all library controls remain above it. A dark
	// wash preserves contrast even when users choose a bright background.
	const std::string themeBackground = CustomTheme::assetPath("background");
	bool libraryThemeBackgroundVisible = false;
	if (!themeBackground.empty()) {
		ImguiFileTexture background(themeBackground, true, true);
		ImTextureID id = background.getId();
		const bool hasThemeBackground = id != ImTextureID();
		libraryThemeBackgroundVisible = hasThemeBackground;
		if (hasThemeBackground) {
			const ImVec2 min = ImGui::GetWindowPos();
			const ImVec2 max(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
			ImVec2 imageMin = min, imageMax = max;
			const float aspect = imguiDriver->getAspectRatio(id);
			const float windowAspect = (max.x - min.x) / std::max(1.f, max.y - min.y);
			const std::string fit = CustomTheme::backgroundFit();
			if (fit != "stretch" && aspect > 0.f) {
				const bool contain = fit == "contain";
				if ((aspect > windowAspect) == contain) {
					const float height = (max.x - min.x) / aspect;
					imageMin.y += ((max.y - min.y) - height) * 0.5f;
					imageMax.y = imageMin.y + height;
				} else {
					const float width = (max.y - min.y) * aspect;
					imageMin.x += ((max.x - min.x) - width) * 0.5f;
					imageMax.x = imageMin.x + width;
				}
			}
			ImGui::GetWindowDrawList()->AddImage(id, imageMin, imageMax,
					ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(CustomTheme::backgroundOpacity() * 255)));
			ImGui::GetWindowDrawList()->AddRectFilled(min, max, IM_COL32(0, 0, 0, 72));
		}
	}

    // Keep the library toolbar cohesive at a larger size without coupling it to
    // the user-configured global UI Scaling value.
    constexpr float libraryToolbarScale = 4.0f / 3.0f;
    const ImVec2 itemSpacing = ImGui::GetStyle().ItemSpacing;
    const ImVec2 itemInnerSpacing = ImGui::GetStyle().ItemInnerSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(20.0f * libraryToolbarScale, 8.0f * libraryToolbarScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(itemSpacing.x * libraryToolbarScale, itemSpacing.y * libraryToolbarScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
            ImVec2(itemInnerSpacing.x * libraryToolbarScale, itemInnerSpacing.y * libraryToolbarScale));
	ImGui::PushFont(largeFont, 18.5f * libraryToolbarScale);
    ImGui::AlignTextToFramePadding();
    // Position "GAMES" text and search bar below the menu bar (window is already positioned below menu bar)
    ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
    ImGui::Indent(10.0f * libraryToolbarScale);
    ImGui::Text("%s", T("GAMES"));
    ImGui::Unindent(10.0f * libraryToolbarScale);

	static library::View libraryView = library::View::All;
	if (gui_state != GuiState::SelectDisk)
	{
		const char *viewLabels[] = { T("All Games"), T("Continue Playing"), T("Favourites") };
		const library::View viewValues[] = { library::View::All, library::View::ContinuePlaying, library::View::Favourites };
		for (int i = 0; i < 3; i++)
		{
			ImGui::SameLine(0.0f, uiScaled(8.0f));
			const float width = ImGui::CalcTextSize(viewLabels[i]).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			if (ImGui::GetCursorPosX() + width > ImGui::GetWindowContentRegionMax().x)
			{
				ImGui::NewLine();
				ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + uiScaled(10.0f));
			}
			const bool selected = libraryView == viewValues[i];
			if (selected)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
			if (ImGui::Button(viewLabels[i]))
			{
				libraryView = viewValues[i];
				librarySavestateCacheInvalidated = true;
			}
			if (selected)
				ImGui::PopStyleColor();
		}
		ImGui::NewLine();
	}

    static SettingsUI::TextFilter filter;
	int libraryIconScale = std::clamp(config::LibraryIconScale.get(), 50, 200);
	static bool libraryHoverSettingsInitialized = false;
	static int lastLibraryIconScale = libraryIconScale;
	static int lastLibraryDisplayStyle = config::LibraryDisplayStyle.get();
	static int lastLibraryImageSource = config::LibraryImageSource.get();
	static int lastLibraryCoverMedia = config::LibraryCoverMedia.get();
	static bool lastBoxartDisplayMode = config::BoxartDisplayMode.get();
	const int currentLibraryDisplayStyle = config::LibraryDisplayStyle.get();
	const int currentLibraryImageSource = config::LibraryImageSource.get();
	const int currentLibraryCoverMedia = config::LibraryCoverMedia.get();
	const bool currentBoxartDisplayMode = config::BoxartDisplayMode.get();
	if (!libraryHoverSettingsInitialized)
	{
		libraryHoverSettingsInitialized = true;
		lastLibraryIconScale = libraryIconScale;
		lastLibraryDisplayStyle = currentLibraryDisplayStyle;
		lastLibraryImageSource = currentLibraryImageSource;
		lastLibraryCoverMedia = currentLibraryCoverMedia;
		lastBoxartDisplayMode = currentBoxartDisplayMode;
	}
	else if (lastLibraryIconScale != libraryIconScale
			|| lastLibraryDisplayStyle != currentLibraryDisplayStyle
			|| lastLibraryImageSource != currentLibraryImageSource
			|| lastLibraryCoverMedia != currentLibraryCoverMedia
			|| lastBoxartDisplayMode != currentBoxartDisplayMode)
	{
		resetLibraryGameInfoHover();
		lastLibraryIconScale = libraryIconScale;
		lastLibraryDisplayStyle = currentLibraryDisplayStyle;
		lastLibraryImageSource = currentLibraryImageSource;
		lastLibraryCoverMedia = currentLibraryCoverMedia;
		lastBoxartDisplayMode = currentBoxartDisplayMode;
	}
    IconButton settingsBtn(ICON_FA_GEAR, T("Settings"));
	ImGui::SameLine(0, 32.0f * libraryToolbarScale);
	const float iconScaleSliderWidth = 135.0f * libraryToolbarScale;
	const char* iconScaleLabel = T("Icon Size");
	float filterWidth = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x
			- settingsBtn.width() - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize(T("Filter")).x
			- ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize(iconScaleLabel).x
			- ImGui::GetStyle().ItemSpacing.x - iconScaleSliderWidth;
#ifdef TARGET_UWP
	filterWidth -= ImGui::CalcTextSize(T("Load...")).x + ImGui::GetStyle().FramePadding.x * 2
			+ ImGui::GetStyle().ItemSpacing.x;
#elif defined(__SWITCH__)
	IconButton exitBtn(ICON_FA_POWER_OFF, T("Exit"));
	filterWidth -= exitBtn.width() + ImGui::GetStyle().ItemSpacing.x;
#endif
	filter.Draw(T("Filter"), filterWidth);
	ImGui::SameLine(0, 24.0f * libraryToolbarScale);
	ImGui::SetNextItemWidth(iconScaleSliderWidth);
	if (ImGui::SliderInt(iconScaleLabel, &libraryIconScale, 50, 200, "%d%%")) {
		config::LibraryIconScale.set(libraryIconScale);
	}
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        config::LibraryIconScale.save();
    }
    if (gui_state != GuiState::SelectDisk)
    {
#ifdef TARGET_UWP
    	ImGui::SameLine();
		if (ImGui::Button(T("Load...")))
			gui_load_game();
		ImGui::SameLine();
#elif defined(__SWITCH__)
		ImGui::SameLine();
		if (exitBtn.realize())
			dc_exit();
		ImGui::SameLine();
#else
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtn.width());
#endif
			const std::string settingsIcon = CustomTheme::assetPath("settings");
			ImTextureID settingsIconId{};
			if (!settingsIcon.empty()) {
				ImguiFileTexture icon(settingsIcon, true, true);
				settingsIconId = icon.getId();
			}
			const bool hasSettingsIcon = settingsIconId != ImTextureID();
			const bool settingsPressed = hasSettingsIcon
					? ImGui::Button("##LibrarySettings", ImVec2(settingsBtn.width(), 0))
					: settingsBtn.realize();
			if (hasSettingsIcon) {
			const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
			const float iconSize = std::min(max.y - min.y - 12.f, 24.f * libraryToolbarScale);
			const ImVec2 iconMin(min.x + 8.f, min.y + ((max.y - min.y) - iconSize) * 0.5f);
			ImGui::GetWindowDrawList()->AddImage(settingsIconId, iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize));
			ImGui::GetWindowDrawList()->AddText(ImVec2(iconMin.x + iconSize + 8.f, min.y + ImGui::GetStyle().FramePadding.y),
					ImGui::GetColorU32(ImGuiCol_Text), T("Settings"));
			}
			if (settingsPressed)
			{
				gui_setState(GuiState::Settings);
			}
    }
    else
    {
    	IconButton cancelBtn(T("Cancel"));
		ImGui::SameLine(ImGui::GetContentRegionMax().x - cancelBtn.width());
		if (cancelBtn.realize())
			gui_setState(GuiState::Commands);
    }
	ImGui::PopFont();
    ImGui::PopStyleVar(3);

    boxart.refreshCustomBoxartIndex(false);
    scanner.fetch_game_list();

	// Only if Filter and Settings aren't focused... ImGui::SetNextWindowFocus();
	if (libraryThemeBackgroundVisible) ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::BeginChild(ImGui::GetID("library"), ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
	if (libraryThemeBackgroundVisible) ImGui::PopStyleColor();
    {
		const bool useListStyle = config::LibraryDisplayStyle.get() == static_cast<int>(config::LibraryDisplayStyleMode::List);
		const float totalWidth = ImGui::GetContentRegionMax().x - (!ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0);
#if defined(__ANDROID__)
		constexpr float libraryPlatformFactor = 0.85f;
#else
		constexpr float libraryPlatformFactor = 1.0f;
#endif
		// 100% icon scale corresponds to 1.18 inches based on display DPI.
		constexpr float libraryIconBaseInches = 1.18f * libraryPlatformFactor;
		constexpr float libraryListIconBaseInches = 0.34f * libraryPlatformFactor;
		const float iconScaleMultiplier = libraryIconScale / 100.0f;
		const float gridBoxBaseSize = libraryIconBaseInches * settings.display.dpi * iconScaleMultiplier;
		const float libraryTextScaleFactor = iconScaleMultiplier;
		const float listIconPixels = libraryListIconBaseInches * settings.display.dpi * iconScaleMultiplier;
		const ImVec2 iconSize(listIconPixels, listIconPixels);
		const double iconAnimationClock = getLibraryIconAnimationClock();

		if (useListStyle)
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5, 4));
		else {
			const auto& layout = CustomTheme::activeLayout();
			const float density = layout.density;
			const float currentSpacingX = ImGui::GetStyle().ItemSpacing.x;
			const float currentSpacingY = ImGui::GetStyle().ItemSpacing.y;
			const float spacingX = layout.libraryCoverSpacing >= 0.0f
					? uiScaled(layout.libraryCoverSpacing) : (config::BoxartDisplayMode ? currentSpacingX : 8.0f);
			const float spacingY = config::BoxartDisplayMode ? currentSpacingY : 20.0f;
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacingX * density, spacingY * density));
			if (config::BoxartDisplayMode)
				ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
		}
		const float tableTextSize = std::clamp(18.5f * libraryTextScaleFactor, 18.5f, 46.0f);
		const bool showContinueActions = gui_state != GuiState::SelectDisk && libraryView == library::View::ContinuePlaying;
		const float tableRowContentHeight = std::max(std::max(iconSize.y, tableTextSize),
				showContinueActions ? ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f : 0.0f);
		const float tableRowHeight = tableRowContentHeight + ImGui::GetStyle().CellPadding.y * 2.0f;
		auto calcLibraryTextWidth = [&](const char *text) {
			return largeFont != nullptr ? largeFont->CalcTextSizeA(tableTextSize, FLT_MAX, -1.0f, text).x
					: ImGui::CalcTextSize(text).x;
		};
		const float iconColumnWidth = std::max(iconSize.x, calcLibraryTextWidth("Icon"))
				+ ImGui::GetStyle().CellPadding.x * 2.0f;
		auto calcLibraryColumnWidth = [&](const char *header, const char *sample) {
			return std::max(calcLibraryTextWidth(header), calcLibraryTextWidth(sample))
					+ ImGui::GetStyle().CellPadding.x * 2.0f;
		};
		const char* iconLabel = T("Icon");
		const char* productIdLabel = T("Product ID");
		const float productIdColumnWidth = calcLibraryColumnWidth(productIdLabel, "T-12345M-50");
		const char* titleLabel = T("Title");
		const char* regionLabel = T("Region");
		const float regionColumnWidth = calcLibraryColumnWidth(regionLabel, "JP/US/EU");
		const char* formatLabel = T("Format");
		const char* timePlayedLabel = T("Time Played");
		const char* dateLabel = showContinueActions ? T("Latest Save") : T("Last Booted");
		const float formatColumnWidth = calcLibraryColumnWidth(formatLabel, "Unknown");
		const float dateColumnWidth = calcLibraryTextWidth("12/31/2026 12:59:59 PM")
				+ ImGui::GetStyle().CellPadding.x * 2.0f;
		const float timePlayedColumnWidth = std::max(calcLibraryTextWidth(T("Time Played")), calcLibraryTextWidth("999h 59m"))
				+ ImGui::GetStyle().CellPadding.x * 2.0f;
		const char* sizeLabel = T("Size");
		const float sizeColumnWidth = calcLibraryColumnWidth(sizeLabel, "99999 MB");

		int counter = 0;
		bool gameListEmpty = false;
		bool libraryViewEmpty = false;
		bool noFilteredResults = false;
		{
			scanner.get_mutex().lock();
			const auto& gameList = scanner.get_game_list();
			gameListEmpty = gameList.empty();
			const library::View visibleView = gui_state == GuiState::SelectDisk ? library::View::All : libraryView;
			if (visibleView == library::View::ContinuePlaying)
			{
				static hostfs::SavestateFolders cachedFolders;
				const auto folders = hostfs::getSavestateFolders();
				if (librarySavestateCacheInvalidated.exchange(false)
						|| folders.custom != cachedFolders.custom || folders.data != cachedFolders.data)
				{
					cachedFolders = folders;
					librarySavestateCache.reset([folders](const std::string& path, const std::string& fileName) {
						return dc_getLatestStateForGame(path, fileName, folders);
					});
					libraryStateThumbnailCacheInvalidated = true;
				}
			}
			std::vector<library::Entry> libraryEntries;
			libraryEntries.reserve(gameList.size());
			for (const GameMedia& game : gameList)
				libraryEntries.push_back({ game.path, game.name,
						visibleView == library::View::ContinuePlaying && !game.device
								? librarySavestateCache.get(game.path, game.fileName).creationDate : 0, game.device });
			const std::vector<size_t> visibleGames = library::selectGames(libraryEntries, visibleView,
					config::LibraryFavourites.get());
			libraryViewEmpty = visibleGames.empty();
			int displayedGameCount = 0;
			if (useListStyle)
			{
				ImGui::PushFont(largeFont, tableTextSize);
				const int columnCount = gui_state == GuiState::SelectDisk ? 8 : showContinueActions ? 11 : 9;
				if (ImGui::BeginTable("libraryTable", columnCount, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders
						| ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit
						| ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
				{
					ImGui::TableSetupColumn(iconLabel, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, iconColumnWidth);
					ImGui::TableSetupColumn(productIdLabel, ImGuiTableColumnFlags_WidthFixed, productIdColumnWidth);
					ImGui::TableSetupColumn(titleLabel, ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn(regionLabel, ImGuiTableColumnFlags_WidthFixed, regionColumnWidth);
					ImGui::TableSetupColumn(formatLabel, ImGuiTableColumnFlags_WidthFixed, formatColumnWidth);
					ImGui::TableSetupColumn(timePlayedLabel, ImGuiTableColumnFlags_WidthFixed, timePlayedColumnWidth);
					ImGui::TableSetupColumn(dateLabel, ImGuiTableColumnFlags_WidthFixed, dateColumnWidth);
					ImGui::TableSetupColumn(sizeLabel, ImGuiTableColumnFlags_WidthFixed, sizeColumnWidth);
					if (gui_state != GuiState::SelectDisk)
						ImGui::TableSetupColumn(T("Favourite"), ImGuiTableColumnFlags_WidthFixed, uiScaled(48.0f));
					if (showContinueActions)
					{
						ImGui::TableSetupColumn(T("Resume"), ImGuiTableColumnFlags_WidthFixed, calcLibraryTextWidth(T("Resume")) + ImGui::GetStyle().CellPadding.x * 2.0f);
						ImGui::TableSetupColumn(T("Start Fresh"), ImGuiTableColumnFlags_WidthFixed, calcLibraryTextWidth(T("Start Fresh")) + ImGui::GetStyle().CellPadding.x * 2.0f);
					}
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
					const char *const libraryColumnHeaders[] = {
						iconLabel, productIdLabel, titleLabel, regionLabel, formatLabel, timePlayedLabel, dateLabel, sizeLabel,
						T("Favourite"), T("Resume"), T("Start Fresh")
					};
					for (int column = 0; column < columnCount; ++column)
					{
						ImGui::TableSetColumnIndex(column);
						textTableCellCentered(libraryColumnHeaders[column], tableTextSize);
					}

					auto drawTableGame = [&](const GameMedia& game, int rowIndex) -> bool
					{
						if (gui_state == GuiState::SelectDisk)
						{
							std::string extension = get_file_extension(game.path);
							if (!game.device && extension != "gdi" && extension != "chd"
									&& extension != "cdi" && extension != "cue")
								return false;
							if (game.path.empty())
								return false;
						}

						std::string gameName = game.name;
						bool passFilter = filter.PassFilter(gameName.c_str());
						GameBoxart art;
						if (!game.device)
						{
							art = getLibraryDisplayArtwork(game, iconAnimationClock);
							if (!art.name.empty())
								gameName = art.name;
							passFilter = passFilter || filter.PassFilter(gameName.c_str());
						}

						if (!passFilter)
							return false;
						displayedGameCount++;

						std::string productId;
						std::string region = "Unknown";
						if (!art.uniqueId.empty())
							productId = art.uniqueId;
						if (art.region != 0)
							region = formatLibraryRegion(art.region);

						std::string format = "Unknown";
						if (game.path.empty())
							format = "BIOS";
						else if (game.device)
							format = "Device";
						else
						{
							const std::string extension = get_file_extension(game.path);
							if (!extension.empty())
								format = extension;
						}

						ImguiID _(game.path.empty() ? "bios" : (game.path + "_row"));
						ImGui::TableNextRow(ImGuiTableRowFlags_None, tableRowHeight);
						ImGui::TableSetColumnIndex(0);
						const ImVec2 iconCellPos = ImGui::GetCursorScreenPos();
						const bool rowPressed = ImGui::Selectable(("##row_" + std::to_string(rowIndex)).c_str(),
								false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, tableRowContentHeight));
						ImGui::SetCursorScreenPos(iconCellPos);
						if (!game.device && !art.boxartPath.empty())
						{
							ImguiFileTexture tex(art.boxartPath);
							centerTableCellCursor(iconSize, tableRowContentHeight, true);
							tex.draw(iconSize);
						}

						ImGui::TableSetColumnIndex(1);
						textTableCellCentered(productId, tableRowContentHeight);
						ImGui::TableSetColumnIndex(2);
						textTableCellCentered(gameName, tableRowContentHeight, false);
						ImGui::TableSetColumnIndex(3);
						textTableCellCentered(region, tableRowContentHeight);
						ImGui::TableSetColumnIndex(4);
						textTableCellCentered(format, tableRowContentHeight);
						ImGui::TableSetColumnIndex(5);
						const std::string timePlayed = art.playTimeSeconds.has_value()
								? formatLibraryPlaytime(*art.playTimeSeconds) : std::string();
						textTableCellCentered(timePlayed, tableRowContentHeight);
						ImGui::TableSetColumnIndex(6);
						const time_t date = showContinueActions ? librarySavestateCache.get(game.path, game.fileName).creationDate
								: getLibraryGameLastBooted(game, art.uniqueId);
						const std::string dateText = date == 0 ? std::string() : formatShortDateTime(date);
						textTableCellCentered(dateText, tableRowContentHeight);
						ImGui::TableSetColumnIndex(7);
						const std::string size = formatLibrarySize(game.size);
						textTableCellCentered(size, tableRowContentHeight);
						bool favouritePressed = false;
						if (gui_state != GuiState::SelectDisk)
						{
							ImGui::TableSetColumnIndex(8);
							favouritePressed = drawLibraryFavouriteButton(game);
						}
						if (showContinueActions && !game.device && !game.path.empty())
						{
							const SavestateInfo& stateInfo = librarySavestateCache.get(game.path, game.fileName);
							ImGui::TableSetColumnIndex(9);
							const bool resumePressed = ImGui::Button((std::string(T("Resume")) + "##resume_" + game.path).c_str());
							drawLibraryStatePreview(stateInfo);
							if (resumePressed)
							{
								startLibraryGame(game, art, true);
								return true;
							}
							ImGui::TableSetColumnIndex(10);
							if (ImGui::Button((std::string(T("Start Fresh")) + "##fresh_" + game.path).c_str()))
							{
								startLibraryGame(game, art, false);
								return true;
							}
						}

						if (rowPressed && !favouritePressed)
						{
							if (gui_state == GuiState::SelectDisk)
							{
								try {
									emu.insertGdrom(game.path);
									gui_resume_game();
								} catch (const FlycastException& e) {
									gui_error(e.what());
								}
							}
							else
							{
								librarySavestateCacheInvalidated = true;
								settings.content.title = art.name;
								if (settings.content.title.empty() || settings.content.title == game.fileName)
									settings.content.title = get_file_basename(game.fileName);
								std::string gamePath(game.path);
								scanner.get_mutex().unlock();
								gui_start_game(gamePath);
								scanner.get_mutex().lock();
								return true;
							}
						}
						return false;
					};

					if (!filter.IsActive() && gui_state != GuiState::SelectDisk)
					{
						ImGuiListClipper clipper;
						clipper.Begin(static_cast<int>(visibleGames.size()), tableRowHeight);
						bool gameStartedFromRow = false;
						while (clipper.Step() && !gameStartedFromRow)
							for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
								if (drawTableGame(gameList[visibleGames[i]], visibleGames[i]))
								{
									gameStartedFromRow = true;
									break;
								}
					}
					else
					{
						for (size_t i : visibleGames)
							if (drawTableGame(gameList[i], static_cast<int>(i)))
								break;
					}
					ImGui::EndTable();
				}
				ImGui::PopFont();
			}
			else
			{
				const int itemsPerLine = std::max<int>(totalWidth / (gridBoxBaseSize + ImGui::GetStyle().ItemSpacing.x), 1);
				const float responsiveBoxSize = totalWidth / itemsPerLine - ImGui::GetStyle().FramePadding.x * 2;
				const ImVec2 responsiveBoxVec2 = ImVec2(responsiveBoxSize, responsiveBoxSize);
				// Keep the current star size at 200%, then follow the actual cover size as columns change.
				const float maxGridBoxBaseSize = libraryIconBaseInches * settings.display.dpi * 2.0f;
				const int maxScaleItemsPerLine = std::max<int>(totalWidth / (maxGridBoxBaseSize + ImGui::GetStyle().ItemSpacing.x), 1);
				const float maxScaleBoxSize = std::max(1.0f, totalWidth / maxScaleItemsPerLine - ImGui::GetStyle().FramePadding.x * 2);
				const float overlayScale = std::max(1.0f, responsiveBoxSize) / maxScaleBoxSize;
				const float gridTextSize = std::clamp(13.0f * libraryTextScaleFactor, 13.0f, 38.0f);

				for (size_t visibleIndex : visibleGames)
				{
					const GameMedia& game = gameList[visibleIndex];
					if (gui_state == GuiState::SelectDisk)
					{
						std::string extension = get_file_extension(game.path);
						if (!game.device && extension != "gdi" && extension != "chd"
								&& extension != "cdi" && extension != "cue")
							// Only dreamcast disks
							continue;
						if (game.path.empty())
							// Dreamcast BIOS isn't a disk
							continue;
					}
					std::string gameName = game.name;
					bool passFilter = filter.PassFilter(gameName.c_str());
					GameBoxart art;
					if ((config::BoxartDisplayMode || visibleView == library::View::ContinuePlaying) && !game.device)
					{
						art = getLibraryDisplayArtwork(game, iconAnimationClock);
						if (visibleView == library::View::ContinuePlaying)
						{
							if (!art.name.empty())
								gameName = art.name;
							if (!art.searchName.empty() && (gameName.empty() || gameName == game.fileName
									|| gameName == get_file_basename(game.fileName)))
								gameName = art.searchName;
						}
						else
							gameName = art.name;
						passFilter = passFilter || filter.PassFilter(gameName.c_str());
					}
					if (passFilter)
					{
						displayedGameCount++;
						ImguiID _(game.path.empty() ? "bios" : game.path);
						bool pressed = false;
						bool favouritePressed = false;
						bool libraryActionPressed = false;
						bool resumeRequested = false;
						if (visibleView == library::View::ContinuePlaying)
						{
							if (counter % itemsPerLine != 0)
								ImGui::SameLine();
							counter++;
							ImGui::BeginGroup();
							ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
							if (ImGui::BeginChild("libraryCard", ImVec2(responsiveBoxSize, 0.0f), ImGuiChildFlags_AutoResizeY,
									ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
							{
								// Preview the same newest state used to include and order this card.
								const SavestateInfo& stateInfo = librarySavestateCache.get(game.path, game.fileName);
								ImguiFileTexture artTexture(art.boxartPath);
								pressed = drawLibraryCoverButton(artTexture, game.name, responsiveBoxVec2, gameName, gridTextSize,
										game, &art, overlayScale, favouritePressed);
								ImguiStyleVar compactSpacing(ImGuiStyleVar_ItemSpacing, ScaledVec2(6.0f, 6.0f));
								ImguiStyleVar compactPadding(ImGuiStyleVar_FramePadding, ScaledVec2(8.0f, 4.0f));
								ImGui::PushFont(largeFont, uiScaled(14.0f));
								ImGui::PushFont(largeFont, std::clamp(uiScaled(18.0f * libraryTextScaleFactor), uiScaled(18.0f), uiScaled(28.0f)));
								ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + responsiveBoxSize);
								ImGui::TextWrapped("%s", gameName.c_str());
								ImGui::PopTextWrapPos();
								ImGui::PopFont();
								const time_t savedAt = stateInfo.creationDate;
								if (savedAt != 0)
								{
									const std::string savedAtLabel = formatShortDateTime(savedAt);
									ImGui::PushFont(nullptr, std::clamp(uiScaled(12.0f * libraryTextScaleFactor), uiScaled(12.0f), uiScaled(18.0f)));
									ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + responsiveBoxSize);
									ImGui::TextDisabled("%s: %s", T("Saved"), savedAtLabel.c_str());
									ImGui::PopTextWrapPos();
									ImGui::PopFont();
								}

								const float actionGap = ImGui::GetStyle().ItemSpacing.x;
								const float framePad = ImGui::GetStyle().FramePadding.x * 2.0f;
								const float requiredWidth = ImGui::CalcTextSize(T("Resume")).x
										+ ImGui::CalcTextSize(T("Start Fresh")).x + framePad * 2.0f + actionGap;
								const bool sideBySide = responsiveBoxSize >= requiredWidth;
								const float actionWidth = sideBySide ? (responsiveBoxSize - actionGap) / 2.0f : responsiveBoxSize;
								if (ImGui::Button((std::string(T("Resume")) + "##resume_" + game.path).c_str(), ImVec2(actionWidth, 0.0f)))
								{
									resumeRequested = true;
									libraryActionPressed = true;
								}
								drawLibraryStatePreview(stateInfo);
								if (sideBySide)
									ImGui::SameLine(0.0f, actionGap);
								ImguiStyleColor secondaryAction(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
								if (ImGui::Button((std::string(T("Start Fresh")) + "##fresh_" + game.path).c_str(), ImVec2(actionWidth, 0.0f)))
								{
									resumeRequested = false;
									libraryActionPressed = true;
								}
								ImGui::PopFont();
							}
							ImGui::EndChild();
							ImGui::PopStyleVar();
							ImGui::EndGroup();
						}
						else
						{
							if (config::BoxartDisplayMode)
							{
								if (counter % itemsPerLine != 0)
									ImGui::SameLine();
								counter++;
							}
							ImGui::BeginGroup();
							if (config::BoxartDisplayMode)
							{
								// Put the image inside a child window so we can detect when it's fully clipped and doesn't need to be loaded
								if (ImGui::BeginChild("img", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened))
								{
									ImguiFileTexture artTexture(art.boxartPath);
									pressed = drawLibraryCoverButton(artTexture, game.name, responsiveBoxVec2, gameName, gridTextSize,
											game, &art, overlayScale, favouritePressed);
								}
								ImGui::EndChild();
							}
							else
							{
								ImGui::PushFont(largeFont, gridTextSize);
								pressed = ImGui::Selectable(gameName.c_str());
								ImGui::PopFont();
								updateLibraryLongPress(game, game.path.empty() ? "bios" : game.path, ImGui::IsItemActive());
								draw_library_game_info_hover(game);
							}
							if (!config::BoxartDisplayMode)
								favouritePressed = drawLibraryFavouriteButton(game);
							ImGui::EndGroup();
						}
						if (libraryActionPressed)
						{
							startLibraryGame(game, art, resumeRequested);
							break;
						}
						if (pressed && !favouritePressed)
						{
							if (!config::BoxartDisplayMode)
								art = boxart.getBoxart(game);
							settings.content.title = art.name;
							if (settings.content.title.empty() || settings.content.title == game.fileName)
								settings.content.title = get_file_basename(game.fileName);
							if (gui_state == GuiState::SelectDisk)
							{
								try {
									emu.insertGdrom(game.path);
									gui_resume_game();
								} catch (const FlycastException& e) {
									gui_error(e.what());
								}
							}
							else
							{
								librarySavestateCacheInvalidated = true;
								std::string gamePath(game.path);
								scanner.get_mutex().unlock();
								gui_start_game(gamePath);
								scanner.get_mutex().lock();
								break;
							}
						}
					}
				}
			}
			noFilteredResults = filter.IsActive() && !visibleGames.empty() && displayedGameCount == 0;
			scanner.get_mutex().unlock();
		}

#if defined(__ANDROID__)
		if (gui_state == GuiState::Main && (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] || !touchedLibraryItemThisFrame))
			resetLibraryLongPress();
#endif
		bool addContent = false;
#if !defined(TARGET_IPHONE)
		if (gameListEmpty && gui_state != GuiState::SelectDisk)
		{
			const char *label = T("Your game list is empty");
			// center horizontally
			const float w = ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, -1.f, label).x + ImGui::GetStyle().FramePadding.x * 2;
			ImGui::SameLine((ImGui::GetContentRegionMax().x - w) / 2);
			if (ImGui::BeginChild("empty", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened))
			{
				ImGui::PushFont(nullptr, uiLargeFontSize());
				ImGui::NewLine();
				ImGui::Text("%s", label);
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
				addContent = ImGui::Button(T("Add Game Folder"));
				ImGui::PopFont();
			}
			ImGui::EndChild();
		}
		if (!gameListEmpty && (libraryViewEmpty || noFilteredResults) && gui_state != GuiState::SelectDisk)
		{
			const bool scanningStates = libraryView == library::View::ContinuePlaying && librarySavestateCache.busy();
			const char *label = scanningStates ? T("Checking saved games...")
					: filter.IsActive() ? T("No games match this filter") : libraryView == library::View::ContinuePlaying
					? T("No saved games yet") : libraryView == library::View::Favourites
					? T("No favourites yet") : T("No games match this filter");
			const char *detail = scanningStates || filter.IsActive() ? "" : libraryView == library::View::ContinuePlaying
					? T("Games with a save state or autosave will appear here.") : libraryView == library::View::Favourites
					? T("Use the star on a game to add it to Favourites.") : "";
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + uiScaled(24.0f));
			ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label).x) * 0.5f));
			ImGui::TextWrapped("%s", label);
			if (detail[0] != '\0')
				ImGui::TextWrapped("%s", detail);
			ImGui::PopFont();
		}
#endif
		ImGui::PopStyleVar(useListStyle || !config::BoxartDisplayMode ? 1 : 2);
        addContentPath(addContent);
    }
    scrollWhenDraggingOnVoid();
    windowDragScroll();
	ImGui::EndChild();
	ImGui::End();

    contentpath_warning_popup();
}

static bool systemdir_selected_callback(bool cancelled, std::string selection)
{
	if (cancelled)
	{
		gui_setState(GuiState::Main);
		return true;
	}
	selection += "/";

	std::string data_path = selection + "data/";
	if (!file_exists(data_path))
	{
		if (!make_directory(data_path))
		{
			WARN_LOG(BOOT, "Cannot create 'data' directory: %s", data_path.c_str());
			gui_error(Ts("Invalid selection:") + '\n' + Ts("Hollycast cannot write to this folder."));
			return false;
		}
	}
	// We might be able to create a directory but not a file. Because ... android
	// So let's test to be sure.
	std::string testPath = data_path + "writetest.txt";
	FILE *file = fopen(testPath.c_str(), "w");
	if (file == nullptr)
	{
		WARN_LOG(BOOT, "Cannot write in the 'data' directory");
		gui_error(Ts("Invalid selection:") + '\n' + Ts("Hollycast cannot write to this folder."));
		return false;
	}
	fclose(file);
	unlink(testPath.c_str());

	set_user_config_dir(selection);
	add_system_data_dir(selection);
	set_user_data_dir(data_path);

	if (config::open())
	{
		config::Settings::instance().load(false);
		// Make sure the renderer type doesn't change mid-flight
		config::RendererType = RenderType::OpenGL;
		gui_setState(GuiState::Main);
		if (config::ContentPath.get().empty())
		{
			scanner.stop();
			config::ContentPath.get().push_back(selection);
		}
		SaveSettings();
	}
	return true;
}

static void gui_display_onboarding()
{
	const char *title = T("Select Hollycast Home Folder");
	ImGui::OpenPopup(title);
	select_file_popup(title, &systemdir_selected_callback);
}

void gui_open_setup_checklist()
{
	setupChecklist.requestManual();
}

static void gui_display_setup_checklist()
{
	const char* title = T("Ready to Play Checklist");
	static bool biosChecked = false;
	static bool bios = false;
	static std::vector<std::string> checkedContentPaths, checkedBiosPaths;
	if (openSetupChecklistIfUnblocked(setupChecklist, title))
		biosChecked = false;
	ImguiStyleVar windowPadding(ImGuiStyleVar_WindowPadding, ScaledVec2(22.f, 18.f));
	ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ScaledVec2(10.f, 8.f));
	ImguiStyleVar cellPadding(ImGuiStyleVar_CellPadding, ScaledVec2(8.f, 6.f));
	ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(12.f, 7.f));
	centerNextWindow();
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowSize(ImVec2(std::min(uiScaled(620.f), display.x * 0.95f),
		std::min(uiScaled(680.f), display.y * 0.90f)), ImGuiCond_Appearing);
	if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar))
	{
		if (setupChecklist.takeAutomaticCloseRequest())
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		// Use the regular Roboto face for readable body copy; section labels use
		// the existing bold face instead of inheriting the surrounding UI font.
		ImGui::PushFont(largeFont, uiScaled(18.f));
		const float footerHeight = uiScaled(42.f) + ImGui::GetStyle().ItemSpacing.y * 2.f + 1.f;
		ImGui::BeginChild("##setup-checklist-content", ImVec2(0, -footerHeight),
			false, ImGuiWindowFlags_DragScrolling);
		auto sectionHeading = [](const char* label) {
			ImGui::Dummy(ScaledVec2(0.f, 10.f));
			ImGui::PushFont(boldFont, uiScaled(20.f));
			ImGui::TextWrapped("%s", label);
			ImGui::PopFont();
			ImGui::Separator();
		};
		auto firmwareRow = [](const char* system, const char* filename) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushFont(boldFont, uiScaled(18.f));
			ImGui::TextWrapped("%s", system);
			ImGui::PopFont();
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", filename);
		};
		bool controller = false, keyboard = false;
		for (int i = 0; i < GamepadDevice::GetGamepadCount(); ++i)
		{
			auto device = GamepadDevice::GetGamepad(i);
			if (!device) continue;
			if (dynamic_cast<KeyboardDevice*>(device.get())) keyboard = true;
			else if (!dynamic_cast<Mouse*>(device.get()) && !device->is_virtual_gamepad()) controller = true;
		}
#if defined(__ANDROID__) || defined(TARGET_IPHONE)
		const bool touch = true;
#else
		const bool touch = false;
#endif
		if (!biosChecked || checkedContentPaths != config::ContentPath.get() || checkedBiosPaths != config::BiosPath.get())
		{
			checkedContentPaths = config::ContentPath.get();
			checkedBiosPaths = config::BiosPath.get();
			bios = !hostfs::findFlash("dc_", "%bios.bin;%boot.bin").empty();
			biosChecked = true;
		}
		const auto checklist = readiness::evaluate({ !config::ContentPath.get().empty(), bios, controller, keyboard, touch });
		ImGui::PushFont(boldFont, uiScaled(25.f));
		ImGui::TextWrapped("%s", T("Ready to Play"));
		ImGui::PopFont();
		ImGui::TextWrapped("%s", T("Optional setup for games, firmware and controls. Reopen this checklist from General settings."));
		sectionHeading(T("1. Game folder"));
		ImGui::TextWrapped("%s", checklist.games == readiness::Status::Ready
			? T("A folder is configured. If your games are elsewhere, choose their folder below.")
			: T("Choose a folder containing your games to add them to the library."));
		bool addFolder = ImGui::Button(T("Choose Game Folder"));
		addContentPath(addFolder);
		sectionHeading(T("2. BIOS and firmware"));
		ImGui::PushFont(boldFont, uiScaled(18.f));
		ImGui::TextUnformatted(T("Dreamcast"));
		ImGui::PopFont();
		ImGui::TextWrapped("%s", T("Place dc_boot.bin and dc_flash.bin in Hollycast's data folder, unless you have specified a custom BIOS folder."));
		ImGui::TextWrapped("%s", bios
			? T("A BIOS filename was found. Its contents have not been verified.")
			: T("No original BIOS was found. Built-in HLE supports many games; some need an original BIOS."));
		ImGui::Spacing();
		ImGui::PushFont(boldFont, uiScaled(18.f));
		ImGui::TextUnformatted(T("Arcade systems"));
		ImGui::PopFont();
		const ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingStretchSame
			| ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings;
		if (ImGui::BeginTable("##setup-arcade-bios", 2, tableFlags))
		{
			firmwareRow(T("NAOMI / GD-ROM"), "naomi.zip");
			firmwareRow(T("NAOMI 2"), "naomi2.zip");
			firmwareRow(T("Atomiswave"), "awbios.zip");
			firmwareRow(T("System SP"), "segasp.zip");
			ImGui::EndTable();
		}
		// Archive names and title-specific ROM selections follow naomi_roms.cpp
		// and loadBios(); the naomi-jp-d and naomi-ko-h sets both use naomi.zip.
		ImGui::PushFont(boldFont, uiScaled(18.f));
		const bool showTitleBios = ImGui::TreeNodeEx(T("Title-specific BIOS requirements"), ImGuiTreeNodeFlags_SpanAvailWidth);
		ImGui::PopFont();
		if (showTitleBios)
		{
			if (ImGui::BeginTable("##setup-title-bios", 2, tableFlags))
			{
				firmwareRow(T("Airline Pilots"), "airlbios.zip");
				firmwareRow(T("The House of the Dead 2"), "hod2bios.zip");
				firmwareRow(T("F355 Challenge (twin) / Challenge 2"), "f355bios.zip");
				firmwareRow(T("F355 Challenge (deluxe)"), "f355dlx.zip");
				firmwareRow(T("Kasei Channel Mars TV"), T("naomi.zip containing epr-21576d.ic27"));
				firmwareRow(T("Mushiking II / III / III+ (Korea)"), T("naomi.zip containing epr-21576h_multi.ic27"));
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}
		ImGui::TextWrapped("%s", T("Use the archive for your system or game. Keep ZIP or 7z archives packed; the matching BIOS data may already be included in your game or parent ROM set."));
		ImGui::Spacing();
		ImGui::PushFont(boldFont, uiScaled(18.f));
		ImGui::TextUnformatted(T("BIOS location"));
		ImGui::PopFont();
		ImGui::TextWrapped("%s", T("Default location: Hollycast's data folder."));
		ImGui::TextWrapped("%s", T("To use a custom folder instead, add it in General > Custom Paths > BIOS Folders."));
		sectionHeading(T("3. Controls"));
		ImGui::TextWrapped("%s", controller ? T("A controller is detected. Configure its mapping in Controls settings.")
			: touch ? T("No controller detected. Touch controls are available; you can also connect a controller.")
			: keyboard ? T("No controller detected. A keyboard is available; you can also connect a controller.")
			: T("No controller detected. Connect a controller and configure it in Controls settings."));
		ImGui::Spacing();
		ImGui::TextWrapped("%s", checklist.games == readiness::Status::Ready
			? T("Next: return to the library and select a game. If none appear, check your game folder.")
			: T("Next: choose a game folder, then select a game in the library."));
		ImGui::EndChild();
		ImGui::Separator();
		ImGui::PushFont(boldFont, uiScaled(18.f));
		if (ImGui::Button(T("Done"), ImVec2(-FLT_MIN, uiScaled(42.f))))
		{
			setupChecklist.dismissed([](bool pending) {
				config::saveBool("hollycast", "SetupChecklistPending", pending);
			});
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopFont();
		ImGui::PopFont();
		ImGui::EndPopup();
	}
}

static void drawBoxartBackground()
{
	GameMedia game;
	game.path = settings.content.path;
	game.fileName = settings.content.fileName;
	game.arcade = settings.platform.isArcade();
	GameBoxart art = boxart.getBoxart(game);
	ImguiFileTexture tex(art.boxartPath);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	tex.draw(dl, ImVec2(0, 0), ImVec2(settings.display.width, settings.display.height), 1.f);
}

static void gui_network_start()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(360, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _1(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

	if (ImGui::Begin("##network", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
		ImGui::AlignTextToFramePadding();
		ImGui::SetCursorPosX(uiScaled(20.f));

		if (networkStatus.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
		{
			ImGui::Text("%s", T("Starting..."));
			try {
				if (networkStatus.get())
					gui_resume_game();
				else
					gui_stop_game();
			} catch (const FlycastException& e) {
				gui_stop_game(e.what());
			}
		}
		else
		{
			ImGui::Text("%s", T("Starting Network..."));
			if (NetworkHandshake::instance->canStartNow())
				ImGui::TextWrapped("%s", T("Press Start to start the game now."));
		}
		ImGui::Text("%s", get_notification().c_str());

		float currentwidth = ImGui::GetContentRegionAvail().x;
		float buttonWidth = ImGui::CalcTextSize(T("Cancel")).x + ImGui::GetStyle().FramePadding.x * 2;
		if (NetworkHandshake::instance != nullptr && NetworkHandshake::instance->canStartNow() && gui_state != GuiState::Closed)
		{
			float startWidth = ImGui::CalcTextSize(T("Start Now")).x + ImGui::GetStyle().FramePadding.x * 2;
			buttonWidth = std::max(buttonWidth, startWidth);
			ImGui::SetCursorPosX((currentwidth - buttonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) / 2.f + ImGui::GetStyle().WindowPadding.x);
			if (ImGui::Button(T("Start Now"), ScaledVec2(buttonWidth, 0)) && NetworkHandshake::instance != nullptr)
				NetworkHandshake::instance->startNow();
			ImGui::SameLine();
		}
		else {
			ImGui::SetCursorPosX((currentwidth - buttonWidth) / 2.f + ImGui::GetStyle().WindowPadding.x);
		}
		if (ImGui::Button(T("Cancel"), ScaledVec2(buttonWidth, 0)) && NetworkHandshake::instance != nullptr)
		{
			NetworkHandshake::instance->stop();
			try {
				networkStatus.get();
			}
			catch (const FlycastException&) {
			}
			gui_stop_game();
		}
	}
	ImGui::End();

	if ((kcode[0] & DC_BTN_START) == 0 && NetworkHandshake::instance != nullptr)
		NetworkHandshake::instance->startNow();
}

#ifdef TARGET_UWP
#include "oslib/http_client.h"

static bool checkUWPProtocolActivation()
{
	// Check for UWP protocol-activated ROM path
	static int checkCount = 90; // Try many times - OnAppActivated may not be called yet
	if (checkCount == 0)
		return false;
	checkCount--;
	char* activationUri = SDL_WinRTGetProtocolActivationURI();
	if (activationUri == nullptr)
		return false;

	std::string uri(activationUri);
	SDL_free(activationUri);
	INFO_LOG(BOOT, "Protocol activation URI: %s", uri.c_str());
	size_t qpos = uri.find('?');
	if (qpos != std::string::npos)
	{
		uri = uri.substr(qpos + 1);
		// Parse launchOnExit parameter
		size_t exitPos = uri.find("launchOnExit=");
		if (exitPos != std::string::npos) {
			exitPos += 13; // Skip "launchOnExit="
			size_t exitEnd = uri.find('&', exitPos);
			if (exitEnd == std::string::npos)
				exitEnd = uri.size();
			std::string exitUri = uri.substr(exitPos, exitEnd - exitPos);
			launchOnExitUri = http::urlDecode(exitUri);
			INFO_LOG(BOOT, "LaunchOnExit URI: %s", launchOnExitUri.c_str());
			// SDL WinRT will automatically handle launchOnExit from the protocol URI
		}

		uri = http::urlDecode(uri);

		// Parse ROM path (first quoted string)
		size_t s = uri.find('"');
		if (s != std::string::npos)
		{
			size_t e = uri.find('"', s + 1);
			if (e != std::string::npos)
			{
				std::string romPath = uri.substr(s + 1, e - (s + 1));
				commandLineStart = true;
				gui_start_game(romPath);
				return true;
			}
		}
	}
	return false;
}
#endif

static bool gui_display_launch_screen()
{
	static std::chrono::steady_clock::time_point started;
	static bool startedAnimation = false;
	static constexpr const char *textureName = "picture/hollycast-launch.png";
#ifdef TEST_AUTOMATION
	launchScreenPending = false;
#endif
	if (!launchScreenPending)
		return false;

	// Launching a game directly should not be delayed by the library intro.
	if (!settings.content.path.empty() || settings.naomi.slave || !error_msg.empty()
			|| (gui_state != GuiState::Main && gui_state != GuiState::Onboarding))
	{
		if (startedAnimation)
			imguiDriver->deleteTexture(textureName);
		launchScreenPending = false;
		startedAnimation = false;
		return false;
	}

	if (!startedAnimation)
	{
		started = std::chrono::steady_clock::now();
		startedAnimation = true;
	}
	const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
	constexpr float fadeStart = 2.05f;
	constexpr float duration = 2.55f;
	if (elapsed >= duration)
	{
		launchScreenPending = false;
		startedAnimation = false;
		imguiDriver->deleteTexture(textureName);
		return false;
	}

	ImTextureID logo = imguiDriver->getTexture(textureName);
	if (logo == ImTextureID())
	{
		size_t size = 0;
		std::unique_ptr<u8[]> data = resource::load(textureName, size);
		int width, height, channels;
		stbi_set_flip_vertically_on_load_thread(0);
		u8 *pixels = data && size <= static_cast<size_t>(std::numeric_limits<int>::max())
				? stbi_load_from_memory(data.get(), static_cast<int>(size), &width, &height, &channels, STBI_rgb_alpha)
				: nullptr;
		if (pixels == nullptr)
		{
			launchScreenPending = false;
			startedAnimation = false;
			return false;
		}
		try {
			logo = imguiDriver->updateTexture(textureName, pixels, width, height, false);
		} catch (...) {
			stbi_image_free(pixels);
			throw;
		}
		stbi_image_free(pixels);
		if (logo == ImTextureID())
		{
			// Vulkan defers uploads until its first frame has finished. Retry while
			// the intro is pending; the duration above also bounds persistent failures.
			return true;
		}
	}

	auto smooth = [](float value) {
		value = std::clamp(value, 0.f, 1.f);
		return value * value * (3.f - 2.f * value);
	};
	const float emblemReveal = smooth(elapsed / .8f);
	const float wordmarkReveal = smooth((elapsed - .38f) / .75f);
	const float fade = 1.f - smooth((elapsed - fadeStart) / (duration - fadeStart));
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImDrawList *draw = ImGui::GetForegroundDrawList();
	const int bgAlpha = static_cast<int>(255.f * fade);
	draw->AddRectFilledMultiColor(ImVec2(0, 0), display, IM_COL32(9, 20, 30, bgAlpha),
			IM_COL32(14, 26, 39, bgAlpha), IM_COL32(7, 13, 24, bgAlpha), IM_COL32(6, 19, 25, bgAlpha));

	// The green spiral in the emblem becomes the centre of the whole sequence.
	const float logoWidth = std::min(display.x * .82f, std::min(1000.f, display.y * 2.5f));
	const float logoHeight = logoWidth * 226.f / 800.f;
	const ImVec2 topLeft((display.x - logoWidth) * .5f, (display.y - logoHeight) * .5f);
	const ImVec2 orbit(topLeft.x + logoWidth * .145f, topLeft.y + logoHeight * .43f);
	const float unit = logoWidth / 800.f;
	const float energy = smooth(elapsed / .55f) * fade;

	// Layered transparent discs make a quiet pool of light, without an image asset.
	for (int i = 12; i > 0; --i)
	{
		const float radius = unit * (72.f + i * 23.f);
		draw->AddCircleFilled(orbit, radius, IM_COL32(28, 145, 116, static_cast<int>(2.f * energy)), 64);
	}
	constexpr float pi = 3.14159265f;
	const float orbitAngle = elapsed * 1.15f - pi * .8f;
	const float arcReveal = smooth(elapsed / .9f);
	for (int i = 0; i < 3; ++i)
	{
		const float radius = unit * (112.f + 17.f * i);
		const float start = orbitAngle + i * 1.65f;
		draw->PathArcTo(orbit, radius, start, start + (1.25f - i * .22f) * arcReveal, 32);
		draw->PathStroke(IM_COL32(85, 213, 166, static_cast<int>((70 - i * 16) * energy)), 0, std::max(1.f, unit));
	}
	const float sparkRadius = unit * 112.f;
	const float sparkAngle = orbitAngle + 1.25f * arcReveal;
	const ImVec2 spark(orbit.x + std::cos(sparkAngle) * sparkRadius, orbit.y + std::sin(sparkAngle) * sparkRadius);
	for (int i = 5; i > 0; --i)
	{
		const float angle = sparkAngle - i * .065f;
		const ImVec2 trail(orbit.x + std::cos(angle) * sparkRadius, orbit.y + std::sin(angle) * sparkRadius);
		draw->AddCircleFilled(trail, unit * (2.2f - i * .25f),
				IM_COL32(118, 255, 194, static_cast<int>((55 - i * 8) * energy)), 12);
	}
	draw->AddCircleFilled(spark, 4.f * unit, IM_COL32(118, 255, 194, static_cast<int>(95.f * energy)), 16);
	draw->AddCircleFilled(spark, 1.8f * unit, IM_COL32(224, 255, 235, static_cast<int>(220.f * energy)), 12);

	// Four colored lights spiral into the matching buttons in the emblem.
	const int colors[4][3] = {{30, 235, 186}, {255, 197, 61}, {48, 145, 255}, {255, 78, 64}};
	const ImVec2 buttonCenters[4] = {
		ImVec2(topLeft.x + 213.f * unit, topLeft.y + 136.f * unit),
		ImVec2(topLeft.x + 183.f * unit, topLeft.y + 161.f * unit),
		ImVec2(topLeft.x + 237.f * unit, topLeft.y + 166.f * unit),
		ImVec2(topLeft.x + 207.f * unit, topLeft.y + 187.f * unit)
	};
	auto particlePosition = [&](int i, float time) {
		const float converge = smooth((time - .08f - i * .06f) / 1.f);
		const float angle = i * pi * .5f - pi * .5f + time * 2.2f;
		const float radius = unit * (210.f - 40.f * converge);
		const ImVec2 start(orbit.x + std::cos(angle) * radius, orbit.y + std::sin(angle) * radius);
		return ImVec2(start.x + (buttonCenters[i].x - start.x) * converge,
				start.y + (buttonCenters[i].y - start.y) * converge);
	};
	for (int i = 0; i < 4; ++i)
	{
		const float particlesAlpha = smooth(elapsed / .22f)
				* (1.f - smooth((elapsed - .92f - i * .06f) / .3f)) * fade;
		for (int tail = 4; tail >= 0; --tail)
		{
			const ImVec2 tailPoint = particlePosition(i, std::max(0.f, elapsed - tail * .035f));
			const int opacity = static_cast<int>((tail == 0 ? 220.f : 55.f - tail * 10.f) * particlesAlpha);
			draw->AddCircleFilled(tailPoint, unit * (tail == 0 ? 3.5f : 2.f),
					IM_COL32(colors[i][0], colors[i][1], colors[i][2], opacity), 12);
		}
	}

	// Reveal the emblem first, then let the wordmark settle alongside it.
	const float split = .375f;
	const float emblemScale = .88f + .12f * emblemReveal;
	const ImVec2 emblemCenter(topLeft.x + logoWidth * split * .5f, topLeft.y + logoHeight * .5f);
	const ImVec2 emblemHalf(logoWidth * split * .5f * emblemScale, logoHeight * .5f * emblemScale);
	draw->AddImage(logo, ImVec2(emblemCenter.x - emblemHalf.x, emblemCenter.y - emblemHalf.y),
			ImVec2(emblemCenter.x + emblemHalf.x, emblemCenter.y + emblemHalf.y), ImVec2(0, 0), ImVec2(split, 1),
			IM_COL32(255, 255, 255, static_cast<int>(255.f * emblemReveal * fade)));
	const float wordOffset = (1.f - wordmarkReveal) * 28.f * unit;
	draw->AddImage(logo, ImVec2(topLeft.x + logoWidth * split - wordOffset, topLeft.y),
			ImVec2(topLeft.x + logoWidth - wordOffset, topLeft.y + logoHeight), ImVec2(split, 0), ImVec2(1, 1),
			IM_COL32(255, 255, 255, static_cast<int>(255.f * wordmarkReveal * fade)));
	for (int i = 0; i < 4; ++i)
	{
		const float impact = smooth((elapsed - .94f - i * .06f) / .12f)
				* (1.f - smooth((elapsed - 1.06f - i * .06f) / .28f)) * fade;
		draw->AddCircleFilled(buttonCenters[i], 13.f * unit,
				IM_COL32(colors[i][0], colors[i][1], colors[i][2], static_cast<int>(45.f * impact)), 24);
		draw->AddCircleFilled(buttonCenters[i], 3.f * unit,
				IM_COL32(255, 255, 255, static_cast<int>(150.f * impact)), 16);
	}

	// In the last half-second, render the real UI underneath the fading artwork.
	return elapsed < fadeStart;
}

static void gui_display_loadscreen()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

    if (ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
    {
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
		ImGui::AlignTextToFramePadding();
		ImGui::SetCursorPosX(uiScaled(20.f));
		try {
			const bool gameReady = gameLoader.ready();
			if (gameReady)
			{
				if (custom_texture.needsRefresh())
				{
					custom_texture.refresh();
				}
				else
					custom_texture.init();
			}
			const char *label = gameLoader.getProgress().label;
			if (label == nullptr)
			{
				if (gameReady)
					label = T("Starting...");
				else
					label = T("Loading...");
			}

			const bool customTexPreloading = custom_texture.isPreloading();
			if (gameReady && !customTexPreloading)
			{
				gui_request_initial_auto_load();
				ImGui::Text("%s", label);
			}
			else
			{
				int texLoaded = 0;
				int texTotal = 0;
				size_t loaded_size_b = 0;
				custom_texture.getPreloadProgress(texLoaded, texTotal, loaded_size_b);

				ImGui::Text("%s", label);
				float progress = 0;
				char overlay[64] = "";

				if (!gameReady)
				{
					progress = gameLoader.getProgress().progress;
				}
				else if (customTexPreloading)
				{
					ImGui::Spacing();
					ImGui::Text("%s", T("Preloading custom textures"));
					progress = (texTotal == -1 || texTotal == 0) ? 0.f : (float)texLoaded / (float)texTotal;
					if (texTotal == -1)
						snprintf(overlay, sizeof(overlay), "%s", T("Preparing..."));
					else
					{
						float loaded_size_mb = (float)loaded_size_b / (1024 * 1024);
						snprintf(overlay, sizeof(overlay), "%d / %d (%.1f MB)", texLoaded, texTotal, loaded_size_mb);
					}
				}

				ImguiStyleColor _(ImGuiCol_PlotHistogram, ImGui::GetStyle().Colors[ImGuiCol_HeaderActive]);
				ImGui::ProgressBar(progress, ImVec2(-1, uiScaled(20.f)), overlay);

				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button(T("Cancel"), ScaledVec2(100.f, 0)))
					gameLoader.cancel();
			}
		} catch (const FlycastException& ex) {
			ERROR_LOG(BOOT, "%s", ex.what());
#ifdef TEST_AUTOMATION
			die("Game load failed");
#endif
			gui_stop_game(ex.what());
			if (!commandLineStart && !error_msg.empty())
			{
				gui_error(Ts("Unable to start game.") + "\n\n" + ex.what());
				launch_error_help = true;
			}
		}
    }
    ImGui::End();
}

void gui_display_ui()
{
	FC_PROFILE_SCOPE;
	const LockGuard lock(guiMutex);

	if (gui_state == GuiState::Closed)
		return;

	// Initialize ImGui frame BEFORE any early returns
	// This ensures the menu bar is always visible, even during auto-start
	if (styleReloadRequested)
		gui_updateStyle();
	gui_newFrame();
	if (gui_state == GuiState::Settings)
		SettingsNew::prepareControllerNavigation();
	ImGui::NewFrame();
	error_msg_shown = false;
	bool gui_open = gui_is_open();
	auto finishFrame = [&]() {
		ImGui::Render();
		gui_endFrame(gui_open);
		uiThreadRunner.execTasks(std::chrono::steady_clock::now());
		ImguiFileTexture::resetLoadCount();
	};

#ifdef TARGET_UWP
	// Resolve protocol-launched games before the intro can defer activation.
	const bool protocolActivated = gui_state == GuiState::Main && checkUWPProtocolActivation();
#endif

	if (gui_display_launch_screen())
	{
		finishFrame();
		return;
	}

	// Render menu bar BEFORE any early returns.
	// Hide it for the full-screen GameInfo screen on Android.
	if (gui_state != GuiState::GameInfo)
		GuiMenu::renderMainMenuBar();

#ifdef TARGET_UWP
	if (protocolActivated)
	{
		finishFrame();
		return;
	}
#endif

	// Check for auto-start after menu bar is rendered
	if (gui_state == GuiState::Main)
	{
		if (!settings.content.path.empty() || settings.naomi.slave)
		{
#ifndef __ANDROID__
			commandLineStart = true;
#endif
			if (settings.content.path.substr(0, 7) == "dc_bios")
				gui_start_game("");
			else
				gui_start_game(settings.content.path);
			finishFrame();
			return;
		}
	}

	// Render modal dialogs BEFORE window management
	// This ensures modals are not affected by window stack operations
	render_modal_dialogs();

	switch (gui_state)
	{
	case GuiState::Settings:
		// Settings now always use the new UI through the compatibility facade.
		gui_display_settings();
		break;
	case GuiState::Commands:
		gui_display_commands();
		break;
	case GuiState::Pause:
		toast.draw();
		drawPauseIcon();
		break;
	case GuiState::Main:
		//gui_display_demo();
		gui_display_content();
		break;
	case GuiState::GameInfo:
		gui_display_game_info();
		break;
	case GuiState::Closed:
		break;
	case GuiState::Onboarding:
		gui_display_onboarding();
		break;
	case GuiState::VJoyEditCommands:
		vgamepad::displayCommands();
		break;
	case GuiState::SelectDisk:
		gui_display_content();
		break;
	case GuiState::Loading:
		gui_display_loadscreen();
		rend_process_custom_texture_preloads();
		break;
	case GuiState::NetworkStart:
		gui_network_start();
		break;
	case GuiState::Cheats:
		gui_cheats();
		break;
	case GuiState::Achievements:
#ifdef USE_RACHIEVEMENTS
		achievements::achievementList();
		break;
#endif
	default:
		die("Unknown UI state");
		break;
	}

	if (gui_state == GuiState::Main)
		setupChecklist.libraryReady(config::loadBool("hollycast", "SetupChecklistPending", false));
	if (gui_state == GuiState::Main || gui_state == GuiState::Settings)
		gui_display_setup_checklist();

	finishFrame();

	if (gui_state == GuiState::Closed)
		emu.start();
}

static u64 LastFPSTime;
static int lastFrameCount = 0;
static float fps = -1;

static std::string getFPSNotification()
{
	// Minimal and Full own the frame-rate presentation while active. Keeping
	// the lightweight counter exclusive to Off avoids overlapping OSDs; the
	// settings screen exposes the corresponding FPS control for each mode.
	const bool showLightweightFps = config::ShowFPS && performance_overlay::mode() == 0;
	if (showLightweightFps)
	{
		u64 now = getTimeMs();
		if (now - LastFPSTime >= 1000) {
			fps = ((float)MainFrameCount - lastFrameCount) * 1000.f / (now - LastFPSTime);
			LastFPSTime = now;
			lastFrameCount = MainFrameCount;
		}
		if (fps >= 0.f && fps < 9999.f) {
			char text[32];
			snprintf(text, sizeof(text), "F:%4.1f%s", fps, settings.input.fastForwardMode ? " >>" : "");

			return std::string(text);
		}
	}
	return std::string(settings.input.fastForwardMode ? ">>" : "");
}

void gui_draw_osd()
{
	gui_newFrame();
	ImGui::NewFrame();

#ifdef USE_RACHIEVEMENTS
	if (!achievements::notifier.draw())
#endif
		if (!toast.draw())
		{
			std::string message = getFPSNotification();
			if (!message.empty())
			{
				const float maxW = uiScaled(640.f);
				ImDrawList *dl = ImGui::GetForegroundDrawList();
				const ScaledVec2 padding(5.f, 5.f);
				const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, maxW, &message.front(), &message.back() + 1)
						+ padding * 2.f;
				ImVec2 pos(insetLeft, ImGui::GetIO().DisplaySize.y - size.y);
				constexpr float alpha = 0.7f;
				const ImU32 bg_col = alphaOverride(0x00202020, alpha / 2.f);
				dl->AddRectFilled(pos, pos + size, bg_col, 0.f);
				pos += padding;
				const ImU32 col = alphaOverride(0x0000FFFF, alpha);
				dl->AddText(nullptr, uiLargeFontSize(), pos, col, &message.front(), &message.back() + 1, maxW);
			}
		}

	if (ggpo::active())
	{
		if (config::NetworkStats)
			ggpo::displayStats();
		chat.display();
	}
	else if (config::NetworkStats) {
		ice::displayStats();
	}
	if (!settings.raHardcoreMode)
		lua::overlay();
	SettingsNew::renderTextureFilterLiveTuneOverlay();
	if (!SettingsNew::liveTuneActive())
		vgamepad::draw();
	SettingsNew::renderShaderLiveTuneOverlay();
	// Render menu bar on top during gameplay
	GuiMenu::renderMainMenuBar();
	// Use this frame's effective bar height, including fullscreen auto-hide.
	performance_overlay::draw();
#if defined(__ANDROID__) && defined(HAVE_MOVABLE_VMUS)
	vmu_window::drawInApp();
#endif

	// Render modal dialogs (exit dialog, error popups)
	render_modal_dialogs();

	ImGui::Render();
	uiThreadRunner.execTasks(std::chrono::steady_clock::now());
}

void gui_display_osd() {
	gui_draw_osd();
	gui_endFrame(gui_is_open());
}

void gui_display_profiler()
{
#if FC_PROFILER
	gui_newFrame();
	ImGui::NewFrame();

	const std::string profilerWindowTitle = std::string(T("Profiler")) + "###Profiler";
	ImGui::Begin(profilerWindowTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);

	{
		ImguiStyleColor _(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

		std::unique_lock<std::recursive_mutex> lock(fc_profiler::ProfileThread::s_allThreadsLock);

		for(const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
		{
			char text[256];
			std::snprintf(text, 256, "%.3f : Thread %s", (float)profileThread->cachedTime, profileThread->threadName.c_str());
			ImGui::TreeNode(text);

			ImGui::Indent();
			fc_profiler::drawGUI(profileThread->cachedResultTree);
			ImGui::Unindent();
		}
	}

	for (const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
	{
		fc_profiler::drawGraph(*profileThread);
	}

	ImGui::End();
    ImGui::Render();
	gui_endFrame(true);
#endif
}

void gui_open_onboarding() {
	gui_setState(GuiState::Onboarding);
}

void gui_cancel_load() {
	gameLoader.cancel();
}

void gui_term()
{
	if (inited)
	{
		inited = false;
		scanner.stop();
		librarySavestateCache.stop();
		clearThumbnailCache();
		ImGui::DestroyContext();
	    EventManager::unlisten(Event::Resume, emuEventCallback);
		EventManager::unlisten(Event::Pause, emuEventCallback);
	    EventManager::unlisten(Event::Start, emuEventCallback);
	    EventManager::unlisten(Event::Terminate, emuEventCallback);
	    EventManager::unlisten(Event::VBlank, emuEventCallback);
	    clearVmuIconLookups();
	    boxart.term();
	}
}

void fatal_error(const char* text, ...)
{
    va_list args;

    char temp[2048];
    va_start(args, text);
    vsnprintf(temp, sizeof(temp), text, args);
    va_end(args);
    ERROR_LOG(COMMON, "%s", temp);

    os_notify(T("Fatal Error"), 20000, temp);
}

extern bool subfolders_read;

void gui_refresh_files(bool invalidateArtwork) {
	librarySavestateCacheInvalidated = true;
	if (invalidateArtwork)
		boxart.invalidateOfflineArtwork();
	scanner.refresh();
	subfolders_read = false;
}

void reset_vmus() {
	for (u32 i = 0; i < std::size(vmu_lcd_status); i++)
	{
		vmu_lcd_status[i] = false;
#ifdef HAVE_MOVABLE_VMUS
		vmu_window::publishScreen(i, nullptr, false);
#endif
	}
}

void gui_error(const std::string& what) {
	launch_error_help = false;
	error_msg = what;
}

void gui_loadState(int slot)
{
	const LockGuard lock(guiMutex);

	if (dc_savestateAllowed())
	{
		try {
			// Close UI if it's open (menu selection scenario)
			if (gui_state != GuiState::Closed)
			{
				gui_setState(GuiState::Closed);
			}

			emu.stop();
			dc_loadstate(slot);
			emu.start();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
	else
	{
		// User feedback when load not allowed
		WARN_LOG(COMMON, "Load state not allowed: network=%d, multiboard=%d",
				 settings.network.online, settings.naomi.multiboard);
		os_notify(T("Cannot load state during online play"), 3000);
	}
}

void gui_loadState(bool inRam)
{
	gui_loadState(inRam ? -2 : config::SavestateSlot);
}

void gui_saveState(int slot, bool stopRestart)
{
	const LockGuard lock(guiMutex);
	if ((gui_state == GuiState::Closed || !stopRestart) && dc_savestateAllowed())
	{
		try {
			if (stopRestart)
				emu.stop();

			savestate(slot);

			if (stopRestart)
				emu.start();
		} catch (const FlycastException& e) {
			if (stopRestart)
				gui_stop_game(e.what());
			else
				WARN_LOG(COMMON, "gui_saveState: %s", e.what());
		}
	}
}

void gui_saveState(bool stopRestart, bool inRam)
{
	if (inRam)
	{
		const LockGuard lock(guiMutex);
		if ((gui_state == GuiState::Closed || !stopRestart) && dc_savestateAllowed())
		{
			try {
				if (stopRestart)
					emu.stop();
				dc_savestate(-2);
				if (stopRestart)
					emu.start();
			} catch (const FlycastException& e) {
				if (stopRestart)
					gui_stop_game(e.what());
				else
					WARN_LOG(COMMON, "gui_saveState: %s", e.what());
			}
		}
		return;
	}
	gui_saveState(config::SavestateSlot, stopRestart);
}

void gui_cycleSaveStateSlot(int step)
{
	config::SavestateSlot = (config::SavestateSlot + (step % NUM_SAVE_SLOTS) + NUM_SAVE_SLOTS) % NUM_SAVE_SLOTS;
	SaveSettings();
	os_notify(strprintf(T("Save state slot %d"), config::SavestateSlot + 1).c_str(), 2000);
}

void gui_togglePause()
{
	const LockGuard lock(guiMutex);
	if (settings.network.online || settings.naomi.multiboard)
		return;

	try {
		if (gui_state == GuiState::Closed)
		{
			if (!achievements::canPause())
				return;
			vgamepad::hide();
			emu.stop();
			gui_setState(GuiState::Pause);
		}
		else if (gui_state == GuiState::Pause)
		{
			GamepadDevice::load_system_mappings();
			if (gui_resume_game())
				emu.start();
		}
	} catch (const FlycastException& e) {
		gui_stop_game(e.what());
	}
}

void gui_setState(GuiState newState)
{
	if (gui_state != newState)
	{
		if (gui_state == GuiState::VJoyEditCommands)
			// Menu navigation cancels unsaved edits; Done has already ended the session.
			vgamepad::stopEditing(true);
		if (newState == GuiState::Main && gui_state == GuiState::Closed)
			clearVmuIconLookups();
		resetLibraryLongPress();
		resetLibraryGameInfoHover();
	}
	gui_state = newState;
	if (newState != GuiState::Closed && newState != GuiState::Loading)
	{
		// Loading may refresh replacement textures before resuming a tuning session.
		SettingsNew::stopTextureFilterLiveTune();
		SettingsNew::closeShaderLiveTune();
	}
	if (newState == GuiState::Closed)
	{
		clearThumbnailCache();
		// If the game isn't rendering any frame, these flags won't be updated and keyboard/mouse input will be ignored.
		// So we force them false here. They will be set in the next ImGUI::NewFrame() anyway
		ImGuiIO& io = ImGui::GetIO();
		io.WantCaptureKeyboard = false;
		io.WantCaptureMouse = false;
	}
}

std::string gui_getCurGameBoxartUrl()
{
	GameMedia game;
	game.fileName = settings.content.fileName;
	game.path = settings.content.path;
	game.arcade = settings.platform.isArcade();
	GameBoxart art = boxart.getBoxart(game);
	return art.boxartUrl;
}

void gui_refresh_custom_boxart(bool force)
{
	boxart.invalidateOfflineArtwork();
	boxart.refreshLibraryPlaytimeDatabase();
	boxart.refreshCustomBoxartIndex(force);
}

void gui_refresh_boxart_cache()
{
	clearVmuIconLookups();
	boxart.refreshCache();
	scanner.fetch_game_list_sync();
	std::vector<GameMedia> games;
	{
		std::lock_guard<std::mutex> guard(scanner.get_mutex());
		games = scanner.get_game_list();
	}
	for (const GameMedia& game : games) {
		if (!game.device)
			boxart.queueBoxart(game);
	}
	boxart.startFetch();
}

void gui_runOnUiThread(std::function<void()> function) {
	uiThreadRunner.runOnThread(function);
}

void gui_runOnUiThread(const std::chrono::steady_clock::time_point& tp, const std::function<void()>& function) {
	uiThreadRunner.runOnThread(tp, function);
}

void gui_takeScreenshot()
{
	if (!game_started)
		return;
	gui_runOnUiThread([]() {
		std::string date = timeToISO8601(time(nullptr));
		std::replace(date.begin(), date.end(), '/', '-');
		std::replace(date.begin(), date.end(), ':', '-');
		std::string name = "Hollycast-" + date + ".png";

		std::vector<u8> data;
		getScreenshot(data);
		if (data.empty()) {
			os_notify(T("No screenshot available"), 2000);
		}
		else
		{
			try {
				hostfs::saveScreenshot(name, data);
				os_notify(T("Screenshot saved"), 2000, name.c_str());
			} catch (const FlycastException& e) {
				os_notify(T("Error saving screenshot"), 5000, e.what());
			}
		}
	});
}

// Cache for save state thumbnails
struct ThumbnailEntry {
    ImTextureID id;
};
static std::map<int, ThumbnailEntry> thumbnailCache;


static ImTextureID loadSaveStateThumbnail(int slot)
{
	const int key = slot;
	// Check cache first
	auto cached = thumbnailCache.find(key);
	if (cached != thumbnailCache.end())
		return cached->second.id;

	// Load screenshot from save state
	std::vector<u8> pngData;
	dc_getStateScreenshot(slot, pngData);
	if (pngData.empty())
		return ImTextureID_Invalid;

	// Decode PNG using stb_image
	int width, height, channels;
	stbi_set_flip_vertically_on_load(0);
	u8* imgData = stbi_load_from_memory(
		pngData.data(), pngData.size(),
		&width, &height, &channels, 4);

	if (!imgData)
		return ImTextureID_Invalid;

	// Create texture using imguiDriver abstraction (works with all renderers)
	std::string texName = "savestate_" + std::to_string(key);
	ImTextureID textureId{};
	try {
		textureId = imguiDriver->updateTextureAndAspectRatio(texName, imgData, width, height, false);
	} catch (...) {
		// Renderer might throw during resize
		free(imgData);
		return ImTextureID_Invalid;
	}

	free(imgData);

	// Cache texture
	thumbnailCache[key] = {textureId};

	return textureId;
}

static void draw_save_state_menu_thumbnail(int slot, float size)
{
	ImTextureID thumbnail = loadSaveStateThumbnail(slot);
	if (thumbnail != ImTextureID_Invalid)
		ImGui::Image(thumbnail, ImVec2(size, size));
	else
		ImGui::Dummy(ImVec2(size, size));
}

static void clearThumbnailCache()
{
	// Delete textures before clearing cache
	for (auto& entry : thumbnailCache)
	{
		std::string texName = "savestate_" + std::to_string(entry.first);
		try {
			imguiDriver->deleteTexture(texName);
		} catch (...) {
			// Ignore errors during shutdown/cleanup
		}
	}

	thumbnailCache.clear();
}

static std::string format_save_state_menu_time(time_t timestamp)
{
	if (timestamp <= 0)
		return T("Unknown");

	struct tm tmInfo {};
#ifdef _WIN32
	if (localtime_s(&tmInfo, &timestamp) != 0)
		return T("Unknown");
#else
	if (localtime_r(&timestamp, &tmInfo) == nullptr)
		return T("Unknown");
#endif

	char timeStr[64];
	if (strftime(timeStr, sizeof(timeStr), "%m/%d/%Y %I:%M %p", &tmInfo) == 0)
		return T("Unknown");

	return timeStr;
}

void render_save_state_slots(bool isSaving)
{
    const float thumbnailSize = uiScaled(18.0f);
    bool hasEntries = false;

    for (int slot = 0; slot < NUM_SAVE_SLOTS; slot++)
    {
        const time_t timestamp = dc_getStateCreationDate(slot);
        const bool isEmpty = (timestamp <= 0);

        // If loading, we only care about occupied slots
        if (!isSaving && isEmpty) {
            continue;
        }

        hasEntries = true;

        std::string dateStr = isEmpty ? T("Empty") : format_save_state_menu_time(timestamp);
        std::string label = isSaving
                          ? strprintf(T("Save Slot %d (%s)"), slot + 1, dateStr.c_str())
                          : strprintf(T("Load Slot %d (%s)"), slot + 1, dateStr.c_str());

        draw_save_state_menu_thumbnail(slot, thumbnailSize);
        ImGui::SameLine(0, uiScaled(6.0f));

        if (ImGui::MenuItem(label.c_str())) {
            config::SavestateSlot = slot;
            if (isSaving) {
                gui_saveState();
            } else {
                gui_loadState();
            }
        }
    }

    if (!isSaving && !hasEntries) {
        ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", T("No Save States"));
    }
}

#ifdef TARGET_UWP
// Ugly but a good workaround for MS stupidity
// UWP doesn't allow the UI thread to wait on a thread/task. When an std::future is ready, it is possible
// that the task has not yet completed. Calling std::future::get() at this point will throw an exception
// AND destroy the std::future at the same time, rendering it invalid and discarding the future result.
bool __cdecl Concurrency::details::_Task_impl_base::_IsNonBlockingThread() {
	return false;
}
#endif
