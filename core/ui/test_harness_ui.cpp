// Portions Copyright 2026 The Hollycast Authors
#include "test_harness_ui.h"
#include "gui.h"
#include "mainui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <vector>

namespace test_harness::ui {
using Clock = std::chrono::steady_clock;

struct Item {
	ImRect rect;
	std::string label, window;
	ImGuiID windowId = 0;
	bool visible = false, disabled = false;
	ImGuiItemStatusFlags flags = 0;
	Json metadata = Json::object();
};
static std::map<ImGuiID, Item> items;
struct WindowInfo {
	ImGuiID id = 0;
	std::string name;
	ImRect rect, innerRect;
	ImVec2 scroll, scrollMax;
};
static std::vector<WindowInfo> windows;
static bool requested, collecting, complete, ownsLog, truncated;
static std::string text, observation;
static Clock::time_point observationTime;
static int observedWidth, observedHeight;
static GuiState observedState;
static ImGuiID activeItemId, textInputId;
static bool textInputPassword;
static std::size_t metadataBytes;

static std::string boundedString(const std::string& value, std::size_t maximum)
{
	if (value.size() <= maximum)
		return value;
	std::size_t size = maximum;
	while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80)
		--size;
	truncated = true;
	return value.substr(0, size);
}

static void contextHook(ImGuiContext *context, ImGuiContextHook *hook)
{
	if (hook->Type == ImGuiContextHookType_NewFramePost && requested)
	{
		requested = false;
		collecting = true;
		complete = truncated = false;
		items.clear();
		windows.clear();
		text.clear();
		activeItemId = textInputId = 0;
		textInputPassword = false;
		metadataBytes = 0;
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
		activeItemId = context->ActiveId;
		if (context->InputTextState.ID != 0 && context->InputTextState.ID == context->ActiveId)
		{
			textInputId = context->InputTextState.ID;
			textInputPassword = (context->InputTextState.Flags & ImGuiInputTextFlags_Password) != 0;
		}
		for (const ImGuiWindow* window : context->Windows)
		{
			if (window == nullptr || !window->Active || window->Hidden)
				continue;
			if (windows.size() >= 128)
			{
				truncated = true;
				break;
			}
			windows.push_back({window->ID, boundedString(window->Name, 512), window->Rect(), window->InnerRect,
				window->Scroll, window->ScrollMax});
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

bool collectingInspection() { return collecting; }

void markTruncated()
{
	if (collecting)
		truncated = true;
}

std::uint32_t lastItemId()
{
	return ImGui::GetCurrentContext() != nullptr ? ImGui::GetItemID() : 0;
}

static bool validMetadataScalar(const Json& value)
{
	if (value.is_boolean() || value.is_number_integer() || value.is_number_unsigned())
		return true;
	return value.is_number_float() && std::isfinite(value.get<double>());
}

static Json boundedMetadataScalar(const Json& value, std::size_t stringLimit = 1024)
{
	if (value.is_string())
		return boundedString(value.get<std::string>(), stringLimit);
	return validMetadataScalar(value) ? value : Json();
}

void annotateItem(std::uint32_t rawId, const Json& metadata)
{
	if (!collecting || rawId == 0 || !metadata.is_object())
		return;
	const auto found = items.find(static_cast<ImGuiID>(rawId));
	if (found == items.end())
		return;
	Json candidate = found->second.metadata;
	for (auto field = metadata.begin(); field != metadata.end(); ++field)
	{
		const auto& key = field.key();
		const auto& value = field.value();
		if (key == "semantic_id" || key == "semantic_label" || key == "kind" || key == "value_label" || key == "value_type")
		{
			if (value.is_string())
			{
				const std::string stringValue = value.get<std::string>();
				const std::size_t maximum = key == "semantic_id" ? 128
					: key == "semantic_label" || key == "value_label" ? 512 : 256;
				if (key == "value_label" && stringValue.size() > maximum)
				{
					candidate.erase(key);
					truncated = true;
				}
				else
					candidate[key] = boundedString(stringValue, maximum);
			}
		}
		else if (key == "value" || key == "draft_value")
		{
			if (value.is_array())
			{
				if (value.size() <= 4 && std::all_of(value.begin(), value.end(), validMetadataScalar))
					candidate[key] = value;
				else
				{
					candidate.erase(key);
					truncated = true;
				}
			}
			else if (validMetadataScalar(value))
				candidate[key] = value;
			else if (value.is_string())
			{
				const std::string stringValue = value.get<std::string>();
				if (stringValue.size() <= 1024)
					candidate[key] = stringValue;
				else
				{
					candidate.erase(key);
					truncated = true;
				}
			}
			else
				candidate.erase(key);
		}
		else if (key == "options" && value.is_array())
		{
			Json options = Json::array();
			const auto count = std::min<std::size_t>(value.size(), 128);
			for (std::size_t i = 0; i < count; ++i)
			{
				const auto& option = value[i];
				if (!option.is_object() || !option.contains("value") || !option.contains("label")
					|| !option["label"].is_string()
					|| (!validMetadataScalar(option["value"]) && !option["value"].is_string()))
					continue;
				if (option["value"].is_string() && option["value"].get<std::string>().size() > 256)
				{
					truncated = true;
					continue;
				}
				Json entry = {{"value", boundedMetadataScalar(option["value"], 256)},
					{"label", boundedString(option["label"].get<std::string>(), 256)}};
				if (option.contains("selected") && option["selected"].is_boolean())
					entry["selected"] = option["selected"];
				if (option.contains("draft_selected") && option["draft_selected"].is_boolean())
					entry["draft_selected"] = option["draft_selected"];
				options.push_back(std::move(entry));
			}
			if (value.size() > count)
				truncated = true;
			candidate[key] = std::move(options);
		}
		else if (key == "range" && value.is_object()
				&& value.contains("min") && value.contains("max")
				&& validMetadataScalar(value["min"]) && validMetadataScalar(value["max"]))
			candidate[key] = {{"min", value["min"]}, {"max", value["max"]}};
		else if (key == "requires_apply" && value.is_boolean())
			candidate[key] = value;
		else if ((key == "password" || key == "read_only") && value.is_boolean())
			candidate[key] = value;
	}
	if (candidate.contains("password") && candidate["password"] == true)
	{
		candidate.erase("value");
		candidate.erase("value_label");
		candidate.erase("draft_value");
	}
	const std::size_t oldSize = found->second.metadata.empty() ? 0 : found->second.metadata.dump().size();
	const std::size_t newSize = candidate.empty() ? 0 : candidate.dump().size();
	if (metadataBytes - std::min(metadataBytes, oldSize) + newSize > 256 * 1024)
	{
		truncated = true;
		return;
	}
	metadataBytes = metadataBytes - std::min(metadataBytes, oldSize) + newSize;
	found->second.metadata = std::move(candidate);
}

Json inspection(const std::string& id)
{
	require(complete, "No completed UI inspection");
	Json result = Json::array();
	for (const auto& pair : items)
	{
		const auto& item = pair.second;
		Json entry = {{"id", pair.first}, {"label", item.label.substr(0, item.label.find("##"))},
			{"raw_label", item.label}, {"window", item.window}, {"window_id", item.windowId},
			{"visible", item.visible}, {"disabled", item.disabled},
			{"rect", {item.rect.Min.x, item.rect.Min.y, item.rect.Max.x, item.rect.Max.y}},
			{"checked", (item.flags & ImGuiItemStatusFlags_Checked) != 0},
			{"checkable", (item.flags & ImGuiItemStatusFlags_Checkable) != 0},
			{"inputable", (item.flags & ImGuiItemStatusFlags_Inputable) != 0}, {"active", pair.first == activeItemId}};
		for (auto field = item.metadata.begin(); field != item.metadata.end(); ++field)
			entry[field.key()] = field.value();
		if (pair.first == textInputId)
		{
			entry["text_input_focused"] = true;
			entry["password"] = textInputPassword;
			if (textInputPassword)
			{
				entry.erase("value");
				entry.erase("value_label");
				entry.erase("draft_value");
			}
		}
		result.push_back(std::move(entry));
	}
	Json windowResult = Json::array();
	for (const auto& window : windows)
		windowResult.push_back({{"id", window.id}, {"name", window.name},
			{"rect", {window.rect.Min.x, window.rect.Min.y, window.rect.Max.x, window.rect.Max.y}},
			{"inner_rect", {window.innerRect.Min.x, window.innerRect.Min.y, window.innerRect.Max.x, window.innerRect.Max.y}},
			{"scroll", {window.scroll.x, window.scroll.y}},
			{"scroll_max", {window.scrollMax.x, window.scrollMax.y}}});
	Json textInput = nullptr;
	if (textInputId != 0)
	{
		const auto item = items.find(textInputId);
		textInput = {{"active_item_id", textInputId}, {"password", textInputPassword}};
		if (item != items.end())
		{
			textInput["window_id"] = item->second.windowId;
			textInput["window"] = item->second.window;
		}
	}
	observation = id;
	observationTime = Clock::now();
	observedWidth = settings.display.width;
	observedHeight = settings.display.height;
	observedState = gui_state;
	return {{"items", result}, {"windows", windowResult}, {"active_item_id", activeItemId}, {"text_input", textInput},
		{"text", text}, {"text_scope", "submitted_imgui_text"},
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
	int mouseButton = ImGuiMouseButton_Left, clickCount = 1, clicksStarted = 0;
	std::vector<ImGuiKey> keys;
	Clock::time_point start, nextClickAt;
	u32 startFrame = 0, releaseFrame = 0;
	bool active = false, released = false, mouseDown = false, pointerPending = false;
	float wheelX = 0.f, wheelY = 0.f;
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
			if (args.contains("window"))
				require(args.at("window") == item->second.window, "UI item is in a different window; inspect again");
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
		if (args.contains("mouse_button"))
		{
			const auto button = args.at("mouse_button").get<std::string>();
			next.mouseButton = button == "right" ? ImGuiMouseButton_Right
				: button == "middle" ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
		}
		if (args.contains("click_count"))
			next.clickCount = args.at("click_count").get<int>();
		if (args.contains("modifiers"))
			for (const auto& modifier : args.at("modifiers"))
				next.keys.push_back(key(modifier.get<std::string>()));
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
		next.wheelX = -args.at("delta_x").get<float>();
		next.wheelY = -args.at("delta_y").get<float>();
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
	for (const auto k : next.keys)
		ImGui::GetIO().AddKeyEvent(k, true);
	if (next.kind == "click" || next.kind == "drag" || next.kind == "scroll")
	{
		gui_set_mouse_position(next.x, next.y, false, true);
		next.pointerPending = true;
	}
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
	if (action.mouseDown)
		gui_set_mouse_button(action.kind == "click" ? action.mouseButton : ImGuiMouseButton_Left, false, false, true);
	for (const auto k : action.keys)
		ImGui::GetIO().AddKeyEvent(k, false);
	action.active = false;
	action.mouseDown = false;
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
	const auto now = Clock::now();
	if (action.pointerPending)
	{
		// Overlapping widgets require a prior hovered frame. Move before
		// pressing or scrolling so the first action reaches its actual target.
		gui_set_mouse_position(action.x, action.y, false, true);
		if (MainFrameCount - action.startFrame < 2)
			return;
		action.pointerPending = false;
		action.start = now;
		action.startFrame = MainFrameCount;
		if (action.kind == "scroll")
			ImGui::GetIO().AddMouseWheelEvent(action.wheelX, action.wheelY);
		else
		{
			gui_set_mouse_button(action.kind == "click" ? action.mouseButton : ImGuiMouseButton_Left, true, false, true);
			action.mouseDown = true;
			action.clicksStarted = 1;
		}
		return;
	}
	if (action.kind == "click" && !action.mouseDown && action.clicksStarted < action.clickCount)
	{
		if (now < action.nextClickAt)
		{
			gui_set_mouse_position(action.x, action.y, false, true);
			return;
		}
		gui_set_mouse_position(action.x, action.y, false, true);
		gui_set_mouse_button(action.mouseButton, true, false, true);
		action.mouseDown = true;
		++action.clicksStarted;
		action.start = now;
		action.startFrame = MainFrameCount;
	}
	const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - action.start).count();
	const double fraction = std::min(1.0, elapsed / action.duration);
	if (action.kind == "drag")
		gui_set_mouse_position(int(action.x + (action.toX - action.x) * fraction), int(action.y + (action.toY - action.y) * fraction), false, true);
	else if (action.kind == "click" || action.kind == "scroll")
		gui_set_mouse_position(action.x, action.y, false, true);
	if (elapsed >= action.duration && MainFrameCount - action.startFrame >= 2)
	{
		if (action.mouseDown)
		{
			gui_set_mouse_button(action.kind == "click" ? action.mouseButton : ImGuiMouseButton_Left, false, false, true);
			action.mouseDown = false;
		}
		if (action.kind == "click" && action.clicksStarted < action.clickCount)
			action.nextClickAt = now + std::chrono::milliseconds(50);
		else
		{
			for (const auto k : action.keys)
				ImGui::GetIO().AddKeyEvent(k, false);
			action.released = true;
			action.releaseFrame = MainFrameCount;
		}
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
		item.window = boundedString(context->CurrentWindow->Name, 512);
		item.windowId = context->CurrentWindow->ID;
		item.rect.ClipWith(context->CurrentWindow->ClipRect);
	}
	item.visible = item.rect.GetWidth() > 0 && item.rect.GetHeight() > 0;
	item.disabled = data && (data->ItemFlags & ImGuiItemFlags_Disabled) != 0;
}

void itemInfo(ImGuiID id, const char *label, ImGuiItemStatusFlags flags)
{
	if (!collecting || id == 0)
		return;
	if (items.size() >= 4096 && items.find(id) == items.end())
	{
		truncated = true;
		return;
	}
	// Tabs report their label before ItemAdd supplies their geometry.
	auto& item = items[id];
	item.label = boundedString(label ? label : "", 4096);
	item.flags = flags;
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
