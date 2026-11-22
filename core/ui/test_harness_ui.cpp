// Portions Copyright 2026 The Hollycast Authors
#include "test_harness_ui.h"
#include "gui.h"
#include "mainui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <chrono>
#include <map>

namespace test_harness::ui {
using Clock = std::chrono::steady_clock;

struct Item {
	ImRect rect;
	std::string label, window;
	bool visible = false, disabled = false;
	ImGuiItemStatusFlags flags = 0;
};
static std::map<ImGuiID, Item> items;
static bool requested, collecting, complete, ownsLog, truncated;
static std::string text, observation;
static Clock::time_point observationTime;
static int observedWidth, observedHeight;
static GuiState observedState;

static void contextHook(ImGuiContext *context, ImGuiContextHook *hook)
{
	if (hook->Type == ImGuiContextHookType_NewFramePost && requested)
	{
		requested = false;
		collecting = true;
		complete = truncated = false;
		items.clear();
		text.clear();
		context->TestEngineHookItems = true;
		ownsLog = !context->LogEnabled;
		if (ownsLog)
		{
			ImGui::LogToBuffer(0);
			// Do not expand collapsed nodes or unclip offscreen widgets just to
			// inspect them. The captured text describes submitted ImGui text.
			context->ItemUnclipByLog = false;
		}
	}
	else if (hook->Type == ImGuiContextHookType_EndFramePre && collecting)
	{
		if (ownsLog)
		{
			auto size = std::min<std::size_t>(context->LogBuffer.size(), 256 * 1024);
			// A byte limit can bisect a translated UTF-8 character. Preserve a
			// complete prefix so the bounded inspection remains valid JSON.
			while (size > 0 && size < std::size_t(context->LogBuffer.size())
					&& (static_cast<unsigned char>(context->LogBuffer[size]) & 0xc0) == 0x80)
				--size;
			text.assign(context->LogBuffer.begin(), size);
			truncated = truncated || size != std::size_t(context->LogBuffer.size());
			ImGui::LogFinish();
		}
		context->TestEngineHookItems = false;
		collecting = false;
		complete = true;
	}
}

void beginInspection()
{
	const auto context = ImGui::GetCurrentContext();
	require(context != nullptr, "UI context is not initialized");
	// Renderer recreation can allocate a new context at the same address.
	// Check its actual hooks rather than caching a context pointer.
	for (const auto type : {ImGuiContextHookType_NewFramePost, ImGuiContextHookType_EndFramePre})
	{
		const bool installed = std::any_of(context->Hooks.begin(), context->Hooks.end(), [type](const ImGuiContextHook& hook) {
			return hook.Type == type && hook.Callback == contextHook;
		});
		if (!installed)
		{
			ImGuiContextHook hook;
			hook.Type = type;
			hook.Callback = contextHook;
			ImGui::AddContextHook(context, &hook);
		}
	}
	complete = false;
	requested = true;
}

bool inspectionReady() { return complete; }

Json inspection(const std::string& id)
{
	require(complete, "No completed UI inspection");
	Json result = Json::array();
	for (const auto& pair : items)
	{
		const auto& item = pair.second;
		result.push_back({{"id", pair.first}, {"label", item.label.substr(0, item.label.find("##"))},
			{"raw_label", item.label}, {"window", item.window}, {"visible", item.visible}, {"disabled", item.disabled},
			{"rect", {item.rect.Min.x, item.rect.Min.y, item.rect.Max.x, item.rect.Max.y}},
			{"checked", (item.flags & ImGuiItemStatusFlags_Checked) != 0},
			{"checkable", (item.flags & ImGuiItemStatusFlags_Checkable) != 0},
			{"inputable", (item.flags & ImGuiItemStatusFlags_Inputable) != 0}});
	}
	observation = id;
	observationTime = Clock::now();
	observedWidth = settings.display.width;
	observedHeight = settings.display.height;
	observedState = gui_state;
	return {{"items", result}, {"text", text}, {"text_scope", "submitted_imgui_text"},
		{"truncated", truncated}, {"frame", MainFrameCount}, {"want_text_input", ImGui::GetIO().WantTextInput}};
}

bool open()
{
	return gui_is_open() || (ImGui::GetCurrentContext() && ImGui::IsPopupOpen(nullptr,
		ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
}

static void point(int x, int y)
{
	require(x >= 0 && y >= 0 && x < settings.display.width && y < settings.display.height,
		"UI coordinates are outside the current display");
}

struct Action {
	std::string kind;
	int x = 0, y = 0, toX = 0, toY = 0, duration = 80;
	std::vector<ImGuiKey> keys;
	Clock::time_point start;
	u32 startFrame = 0, releaseFrame = 0;
	bool active = false, released = false;
};
static Action action;
static bool finished = true;

static ImGuiKey key(const std::string& name)
{
	if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
		return ImGuiKey(ImGuiKey_A + name[0] - 'A');
	if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
		return ImGuiKey(ImGuiKey_0 + name[0] - '0');
	const std::pair<const char *, ImGuiKey> keys[] = {
		{"Enter", ImGuiKey_Enter}, {"Escape", ImGuiKey_Escape}, {"Tab", ImGuiKey_Tab},
		{"Up", ImGuiKey_UpArrow}, {"Down", ImGuiKey_DownArrow}, {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow},
		{"Space", ImGuiKey_Space}, {"Backspace", ImGuiKey_Backspace}, {"Delete", ImGuiKey_Delete},
		{"Home", ImGuiKey_Home}, {"End", ImGuiKey_End}, {"PageUp", ImGuiKey_PageUp}, {"PageDown", ImGuiKey_PageDown},
		{"Ctrl", ImGuiKey(ImGuiMod_Ctrl)}, {"Shift", ImGuiKey(ImGuiMod_Shift)}, {"Alt", ImGuiKey(ImGuiMod_Alt)}
	};
	for (const auto& pair : keys)
		if (name == pair.first)
			return pair.second;
	throw std::runtime_error("Unknown native UI key");
}

void startAction(const Json& args)
{
	validateUiAction(args);
	require(args.at("observation_id") == observation && Clock::now() - observationTime < std::chrono::seconds(10),
		"UI observation is stale; inspect again");
	require(observedWidth == settings.display.width && observedHeight == settings.display.height && observedState == gui_state,
		"UI layout changed; inspect again");
	require(!action.active, "A native UI action is still active");
	Action next;
	next.kind = args.at("action").get<std::string>();
	if (next.kind == "click")
	{
		if (args.contains("item_id"))
		{
			const auto item = items.find(args.at("item_id").get<ImGuiID>());
			require(item != items.end() && item->second.visible && !item->second.disabled, "UI item is unavailable or disabled");
			const auto center = item->second.rect.GetCenter();
			next.x = int(center.x);
			next.y = int(center.y);
		}
		else
		{
			next.x = args.at("x").get<int>();
			next.y = args.at("y").get<int>();
		}
		point(next.x, next.y);
	}
	else if (next.kind == "key")
	{
		const auto chord = args.at("key").get<std::string>();
		std::size_t begin = 0;
		do {
			const auto end = chord.find('+', begin);
			next.keys.push_back(key(chord.substr(begin, end - begin)));
			require(next.keys.size() <= 4, "UI key chord is too long");
			if (end == std::string::npos)
				break;
			begin = end + 1;
		} while (true);
	}
	else if (next.kind == "text")
	{
		require(ImGui::GetIO().WantTextInput, "No native UI text field has input focus");
		ImGui::GetIO().AddInputCharactersUTF8(args.at("text").get<std::string>().c_str());
	}
	else if (next.kind == "scroll")
	{
		next.x = args.at("x").get<int>();
		next.y = args.at("y").get<int>();
		point(next.x, next.y);
		ImGui::GetIO().AddMouseWheelEvent(-args.at("delta_x").get<float>(), -args.at("delta_y").get<float>());
	}
	else if (next.kind == "drag")
	{
		next.x = args.at("from_x").get<int>();
		next.y = args.at("from_y").get<int>();
		next.toX = args.at("to_x").get<int>();
		next.toY = args.at("to_y").get<int>();
		next.duration = args.at("duration_ms").get<int>();
		point(next.x, next.y);
		point(next.toX, next.toY);
	}
	if (next.kind == "click" || next.kind == "drag" || next.kind == "scroll")
		gui_set_mouse_position(next.x, next.y, false, true);
	if (next.kind == "click" || next.kind == "drag")
		gui_set_mouse_button(0, true, false, true);
	for (const auto k : next.keys)
		ImGui::GetIO().AddKeyEvent(k, true);
	next.start = Clock::now();
	next.startFrame = MainFrameCount;
	next.active = true;
	action = std::move(next);
	finished = false;
	observation.clear();
}

void release()
{
	if (!action.active)
		return;
	if (action.kind == "click" || action.kind == "drag")
		gui_set_mouse_button(0, false, false, true);
	for (const auto k : action.keys)
		ImGui::GetIO().AddKeyEvent(k, false);
	action.active = false;
	finished = true;
}

void poll()
{
	if (!action.active)
		return;
	if (action.released)
	{
		if (MainFrameCount > action.releaseFrame)
		{
			action.active = false;
			finished = true;
		}
		return;
	}
	const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - action.start).count();
	const double fraction = std::min(1.0, elapsed / action.duration);
	if (action.kind == "drag")
		gui_set_mouse_position(int(action.x + (action.toX - action.x) * fraction), int(action.y + (action.toY - action.y) * fraction), false, true);
	else if (action.kind == "click" || action.kind == "scroll")
		gui_set_mouse_position(action.x, action.y, false, true);
	if (elapsed >= action.duration && MainFrameCount - action.startFrame >= 2)
	{
		if (action.kind == "click" || action.kind == "drag")
			gui_set_mouse_button(0, false, false, true);
		for (const auto k : action.keys)
			ImGui::GetIO().AddKeyEvent(k, false);
		action.released = true;
		action.releaseFrame = MainFrameCount;
	}
}

bool actionReady() { return finished; }

void itemAdd(ImGuiContext *context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData *data)
{
	if (!collecting || id == 0)
		return;
	if (items.size() >= 4096 && items.find(id) == items.end())
	{
		truncated = true;
		return;
	}
	auto& item = items[id];
	item.rect = rect;
	if (context->CurrentWindow)
	{
		item.window = context->CurrentWindow->Name;
		item.rect.ClipWith(context->CurrentWindow->ClipRect);
	}
	item.visible = item.rect.GetWidth() > 0 && item.rect.GetHeight() > 0;
	item.disabled = data && (data->ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

void itemInfo(ImGuiID id, const char *label, ImGuiItemStatusFlags flags)
{
	const auto found = items.find(id);
	if (collecting && found != items.end())
	{
		found->second.label = label ? label : "";
		found->second.flags = flags;
	}
}

const char *debugLabel(ImGuiID id)
{
	const auto found = items.find(id);
	return found == items.end() ? nullptr : found->second.label.c_str();
}

} // namespace test_harness::ui

// ImGui's optional hooks provide widget semantics without patching vendored
// ImGui or linking a desktop automation driver. Enabled only in harness builds.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id, const ImRect& rect, const ImGuiLastItemData *data)
{ test_harness::ui::itemAdd(context, id, rect, data); }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label, ImGuiItemStatusFlags flags)
{ test_harness::ui::itemInfo(id, label, flags); }
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id)
{ return test_harness::ui::debugLabel(id); }
