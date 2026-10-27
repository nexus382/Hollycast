// Portions Copyright 2026 The Hollycast Authors
#include "custom_theme.h"
#include "json.hpp"
#include "nowide/fstream.hpp"
#include "nowide/cstdio.hpp"
#include "oslib/resources.h"
#include "oslib/i18n.h"
#include "oslib/directory.h"
#include "oslib/storage.h"
#include "stdclass.h"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <iterator>
#include <map>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace CustomTheme {
namespace {
constexpr size_t MaxThemeSize = 256 * 1024;
const std::array<Builtin, CustomThemeId> Builtins = {{
	{0, Tnop("Dark"), "Dark.json", "themes/Dark.json"},
	{1, Tnop("Light"), "Light.json", "themes/Light.json"},
	{2, Tnop("Dreamcast"), "Dreamcast.json", "themes/Dreamcast.json"},
	{3, Tnop("High Contrast"), "High Contrast.json", "themes/High Contrast.json"},
	{4, Tnop("Nintendo"), "Nintendo.json", "themes/Nintendo.json"},
	{5, Tnop("Aqua Chill"), "Aqua Chill.json", "themes/Aqua Chill.json"}
}};

std::map<std::string, ImVec4> ActiveRoles;
std::map<std::string, std::map<std::string, ImVec4>> ActiveWidgetRoles;
Theme::Assets ActiveAssets;
Theme::Layout ActiveLayout;

bool validAssetPath(const std::string& path)
{
	if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
		return false;
	std::string part;
	for (char c : path) {
		if (static_cast<unsigned char>(c) < 0x20) return false;
		if (c == '/' || c == '\\') {
			if (part.empty() || part == "." || part == "..") return false;
			part.clear();
		} else part.push_back(c);
	}
	return !part.empty() && part != "." && part != "..";
}

bool validRole(const std::string& role)
{
	return role == "accent" || role == "success" || role == "warning" || role == "error";
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float amount)
{
	return ImVec4(a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount,
			a.z + (b.z - a.z) * amount, a.w + (b.w - a.w) * amount);
}

const Builtin& builtin(int id)
{
	for (const auto& entry : Builtins)
		if (entry.id == id) return entry;
	return Builtins.front();
}

struct ScalarField { const char* name; float ImGuiStyle::*member; float max; };
struct VectorField { const char* name; ImVec2 ImGuiStyle::*member; float max; };
#define SCALAR(name, max) {#name, &ImGuiStyle::name, max}
#define VECTOR(name, max) {#name, &ImGuiStyle::name, max}
const ScalarField ScalarFields[] = {
	SCALAR(Alpha, 1), SCALAR(DisabledAlpha, 1),
	SCALAR(WindowRounding, 200), SCALAR(WindowBorderSize, 20),
	SCALAR(ChildRounding, 200), SCALAR(ChildBorderSize, 20),
	SCALAR(PopupRounding, 200), SCALAR(PopupBorderSize, 20),
	SCALAR(FrameRounding, 200), SCALAR(FrameBorderSize, 20),
	SCALAR(IndentSpacing, 200), SCALAR(ScrollbarSize, 200), SCALAR(ScrollbarRounding, 200),
	SCALAR(ScrollbarPadding, 200), SCALAR(GrabMinSize, 200), SCALAR(GrabRounding, 200),
	SCALAR(TabRounding, 200), SCALAR(TabBorderSize, 20), SCALAR(TabMinWidthBase, 200),
	SCALAR(TabMinWidthShrink, 200), SCALAR(TabBarBorderSize, 20), SCALAR(TabBarOverlineSize, 20),
	SCALAR(SeparatorTextBorderSize, 20)
};
const VectorField VectorFields[] = {
	VECTOR(WindowPadding, 200), VECTOR(WindowMinSize, 200), VECTOR(WindowTitleAlign, 1),
	VECTOR(FramePadding, 200), VECTOR(ItemSpacing, 200), VECTOR(ItemInnerSpacing, 200),
	VECTOR(CellPadding, 200), VECTOR(TouchExtraPadding, 20), VECTOR(ButtonTextAlign, 1),
	VECTOR(SelectableTextAlign, 1), VECTOR(SeparatorTextAlign, 1), VECTOR(SeparatorTextPadding, 200)
};
#undef SCALAR
#undef VECTOR

bool number(const nlohmann::json& value, float max, float& result)
{
	if (!value.is_number()) return false;
	double n = value.get<double>();
	if (!std::isfinite(n) || n < 0 || n > max) return false;
	result = static_cast<float>(n);
	return true;
}

bool validFilename(const std::string& filename)
{
	return filename.size() > 5 && filename.substr(filename.size() - 5) == ".json"
			&& filename.find_first_of("/\\") == std::string::npos
			&& filename != "." && filename != "..";
}

// Public path strings in the theme API are UTF-8. Keep conversion at the
// std::filesystem boundary explicit; narrow path constructors use the active
// locale on some standard library implementations.
std::filesystem::path utf8Path(const std::string& path)
{
	return std::filesystem::u8path(path);
}

int hexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool parseColor(const nlohmann::json& value, ImVec4& color)
{
	if (value.is_array() && value.size() == 4) {
		float components[4];
		for (size_t i = 0; i < 4; ++i)
			if (!number(value[i], 1, components[i])) return false;
		color = ImVec4(components[0], components[1], components[2], components[3]);
		return true;
	}
	if (!value.is_string()) return false;
	const std::string text = value.get<std::string>();
	if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return false;
	float components[] = {0, 0, 0, 1};
	for (size_t i = 0; i < (text.size() - 1) / 2; ++i) {
		int high = hexDigit(text[1 + i * 2]);
		int low = hexDigit(text[2 + i * 2]);
		if (high < 0 || low < 0) return false;
		components[i] = (high * 16 + low) / 255.f;
	}
	color = ImVec4(components[0], components[1], components[2], components[3]);
	return true;
}

bool parseContents(const std::string& contents, Theme& theme, std::string& error, bool allowBase)
{
	try {
		auto json = nlohmann::json::parse(contents);
		if (!json.is_object() || (!json.contains("colors") && !json.contains("style")
				&& !json.contains("widgets") && !json.contains("fonts") && !json.contains("assets") && !json.contains("layout"))
				|| (json.contains("colors") && !json["colors"].is_object())
				|| (json.contains("style") && !json["style"].is_object())
				|| (json.contains("widgets") && !json["widgets"].is_object())
				|| (json.contains("fonts") && !json["fonts"].is_object())
				|| (json.contains("assets") && !json["assets"].is_object())
				|| (json.contains("layout") && !json["layout"].is_object())) {
			error = "Expected a colors, style, widgets, fonts, assets, or layout object";
			return false;
		}
		Theme parsed;
		if (!allowBase && json.contains("base")) {
			error = "Built-in overrides cannot specify a base theme";
			return false;
		}
		const std::string base = json.value("base", std::string(Builtins.front().name));
		auto it = std::find_if(Builtins.begin(), Builtins.end(), [&](const Builtin& entry) { return base == entry.name; });
		if (it == Builtins.end()) {
			error = "Unknown base theme: " + base;
			return false;
		}
		parsed.base = it->id;
		auto parseAsset = [&](const nlohmann::json& value, const std::string& key, std::string& output) {
			(void)key;
			if (!value.is_string() || !validAssetPath(value.get<std::string>())) {
				// Reject just this asset. The rest of the selected theme remains usable.
				output.clear();
				return true;
			}
			output = value.get<std::string>();
			return true;
		};
		if (json.contains("fonts")) {
			const auto& fonts = json["fonts"];
			for (auto field = fonts.begin(); field != fonts.end(); ++field) {
				if (field.key() == "body") { if (!parseAsset(field.value(), "fonts.body", parsed.assets.bodyFont)) return false; }
				else if (field.key() == "bold") { if (!parseAsset(field.value(), "fonts.bold", parsed.assets.boldFont)) return false; }
				else { error = "Unknown font role: " + field.key(); return false; }
			}
		}
		if (json.contains("assets")) {
			const auto& assets = json["assets"];
			for (auto field = assets.begin(); field != assets.end(); ++field) {
				if (field.key() == "background") { if (!parseAsset(field.value(), "assets.background", parsed.assets.background)) return false; }
				else if (field.key() == "backgroundOpacity") {
					if (!field.value().is_number() || !std::isfinite(field.value().get<double>()) || field.value().get<double>() < 0 || field.value().get<double>() > 1) { error = "Invalid backgroundOpacity"; return false; }
					parsed.assets.backgroundOpacity = field.value().get<float>();
				}
				else if (field.key() == "backgroundFit") {
					if (!field.value().is_string() || (field.value() != "cover" && field.value() != "contain" && field.value() != "stretch")) { error = "Invalid backgroundFit"; return false; }
					parsed.assets.backgroundFit = field.value().get<std::string>();
				}
				else if (field.key() == "icons") {
					if (!field.value().is_object()) { error = "Invalid assets.icons"; return false; }
					for (auto icon = field.value().begin(); icon != field.value().end(); ++icon) {
						std::string iconPath;
						if (icon.key().empty() || icon.key().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos || !parseAsset(icon.value(), "assets.icons." + icon.key(), iconPath)) return false;
						if (!iconPath.empty()) parsed.assets.icons.emplace(icon.key(), std::move(iconPath));
					}
				}
				else { error = "Unknown asset property: " + field.key(); return false; }
			}
		}
		if (json.contains("layout")) {
			const auto& layout = json["layout"];
			for (auto field = layout.begin(); field != layout.end(); ++field) {
				if (field.key() == "density") {
					if (!number(field.value(), 1.5f, parsed.layout.density) || parsed.layout.density < 0.75f) { error = "Invalid layout density (expected 0.75–1.5)"; return false; }
					parsed.layout.hasDensity = true;
				}
				else if (field.key() == "settingsSidebarWidth") {
					if (!number(field.value(), 310, parsed.layout.settingsSidebarWidth) || parsed.layout.settingsSidebarWidth < 136) { error = "Invalid layout settingsSidebarWidth (expected 136–310)"; return false; }
					parsed.layout.hasSettingsSidebarWidth = true;
				}
				else if (field.key() == "libraryCoverSpacing") {
					if (!number(field.value(), 48, parsed.layout.libraryCoverSpacing)) { error = "Invalid layout libraryCoverSpacing (expected 0–48)"; return false; }
					parsed.layout.hasLibraryCoverSpacing = true;
				}
				else if (field.key() == "artworkCornerRadius") {
					if (!number(field.value(), 32, parsed.layout.artworkCornerRadius)) { error = "Invalid layout artworkCornerRadius (expected 0–32)"; return false; }
					parsed.layout.hasArtworkCornerRadius = true;
				}
				else { error = "Unknown layout property: " + field.key(); return false; }
			}
		}
		if (json.contains("colors"))
		for (auto color = json["colors"].begin(); color != json["colors"].end(); ++color) {
			int index = 0;
			for (; index < ImGuiCol_COUNT; ++index)
				if (color.key() == ImGui::GetStyleColorName(index)) break;
			ImVec4 value;
			if (index == ImGuiCol_COUNT && validRole(color.key())) {
				if (!parseColor(color.value(), value)) { error = "Invalid semantic color: " + color.key(); return false; }
				parsed.roles[color.key()] = value;
				continue;
			}
			if (index == ImGuiCol_COUNT || !parseColor(color.value(), value)) {
				error = "Invalid color: " + color.key();
				return false;
			}
			parsed.colors.emplace_back(index, value);
		}
		if (json.contains("colors"))
		for (auto role = json["colors"].begin(); role != json["colors"].end(); ++role) {
			// ImGui names are handled above; semantic names share this object.
			if (!validRole(role.key())) continue;
			ImVec4 value;
			if (!parseColor(role.value(), value)) { error = "Invalid semantic color: " + role.key(); return false; }
			parsed.roles[role.key()] = value;
		}
		if (json.contains("widgets"))
		for (auto widget = json["widgets"].begin(); widget != json["widgets"].end(); ++widget) {
			if (widget.key().empty() || !widget.value().is_object()) {
				error = "Invalid widget roles: " + widget.key(); return false;
			}
			for (auto role = widget.value().begin(); role != widget.value().end(); ++role) {
				ImVec4 value;
				if (!validRole(role.key()) || !parseColor(role.value(), value)) {
					error = "Invalid widget semantic color: " + widget.key() + "." + role.key(); return false;
				}
				parsed.widgetRoles[widget.key()][role.key()] = value;
			}
		}
		if (json.contains("style"))
		for (auto field = json["style"].begin(); field != json["style"].end(); ++field) {
			bool found = false;
			for (const auto& spec : ScalarFields) {
				if (field.key() != spec.name) continue;
				float value;
				if (!number(field.value(), spec.max, value)) {
					error = "Invalid style value: " + field.key(); return false;
				}
				if (field.key() == "Alpha" && value < 0.2f) {
					error = "Style Alpha must be at least 0.2"; return false;
				}
				parsed.scalars.emplace_back(spec.member, value);
				found = true;
				break;
			}
			if (!found) for (const auto& spec : VectorFields) {
				if (field.key() != spec.name) continue;
				float x, y;
				if (!field.value().is_array() || field.value().size() != 2
						|| !number(field.value()[0], spec.max, x) || !number(field.value()[1], spec.max, y)) {
					error = "Invalid style value: " + field.key(); return false;
				}
				parsed.vectors.emplace_back(spec.member, ImVec2(x, y));
				found = true;
				break;
			}
			if (!found) { error = "Unknown style property: " + field.key(); return false; }
		}
		nlohmann::json extensions = nlohmann::json::object();
		for (auto field = json.begin(); field != json.end(); ++field) {
			if (field.key() != "base" && field.key() != "colors" && field.key() != "style"
					&& field.key() != "widgets" && field.key() != "fonts" && field.key() != "assets" && field.key() != "layout")
				extensions[field.key()] = field.value();
		}
		if (!extensions.empty()) parsed.preservedFieldsJson = extensions.dump();
		theme = std::move(parsed);
		error.clear();
		return true;
	} catch (const nlohmann::json::exception& e) {
		error = e.what();
		return false;
	}
}

bool loadFile(const std::string& path, const std::string& filename, Theme& theme, std::string& error, bool allowBase)
{
	if (!validFilename(filename)) {
		error = "Invalid theme filename";
		return false;
	}
	nowide::ifstream file(path + filename, std::ios::binary | std::ios::ate);
	if (!file) { error = "Cannot open theme file"; return false; }
	auto size = file.tellg();
	if (size < 0 || size > static_cast<std::streamoff>(MaxThemeSize)) {
		error = "Theme file is too large";
		return false;
	}
	file.seekg(0);
	std::string contents(static_cast<size_t>(size), '\0');
	if (size != 0 && !file.read(&contents[0], size)) { error = "Cannot read theme file"; return false; }
	if (!parseContents(contents, theme, error, allowBase)) return false;
	theme.assets.directory = path;
	return true;
}

std::string pathString(const std::filesystem::path& path)
{
	return path.u8string();
}

bool finishOutput(nowide::ofstream& output)
{
	output.flush();
	const bool flushed = static_cast<bool>(output);
	output.close();
	return flushed && !output.fail();
}

bool atomicReplaceFile(const std::filesystem::path& source, const std::filesystem::path& target,
		std::error_code& error)
{
#ifdef _WIN32
	if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error.clear();
		return true;
	}
	error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	return false;
#else
	std::filesystem::rename(source, target, error);
	return !error;
#endif
}

bool publishNewFile(const std::filesystem::path& source, const std::filesystem::path& target,
		std::error_code& error)
{
#ifdef _WIN32
	if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
		error.clear();
		return true;
	}
	error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	return false;
#else
	// Prefer atomic same-directory hard-link publication. Some filesystems do
	// not support hard links, so use O_EXCL only for those unsupported cases.
	std::filesystem::create_hard_link(source, target, error);
	if (!error) {
		std::error_code cleanupError;
		std::filesystem::remove(source, cleanupError);
		return true;
	}
	const auto condition = error.default_error_condition();
	if (condition != std::errc::operation_not_supported
			&& condition != std::errc::function_not_supported
			&& condition != std::errc::operation_not_permitted
			&& condition != std::errc::cross_device_link) return false;
	error.clear();
	// O_EXCL is supported on FAT/exFAT and network filesystems where hard links
	// are unavailable, and guarantees that fallback publication never replaces
	// an existing destination. Partial files are removed on every write failure.
	const int sourceFd = ::open(source.c_str(), O_RDONLY);
	if (sourceFd < 0) {
		error = std::error_code(errno, std::generic_category());
		return false;
	}
	const int targetFd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (targetFd < 0) {
		const int openError = errno;
		::close(sourceFd);
		error = std::error_code(openError, std::generic_category());
		return false;
	}
	int failure = 0;
	char buffer[32768];
	for (;;) {
		ssize_t count;
		do { count = ::read(sourceFd, buffer, sizeof(buffer)); } while (count < 0 && errno == EINTR);
		if (count < 0) { failure = errno; break; }
		if (count == 0) break;
		ssize_t written = 0;
		while (written < count) {
			ssize_t result;
			do { result = ::write(targetFd, buffer + written, static_cast<size_t>(count - written)); }
			while (result < 0 && errno == EINTR);
			if (result <= 0) { failure = result < 0 ? errno : EIO; break; }
			written += result;
		}
		if (failure) break;
	}
	if (::close(sourceFd) != 0 && !failure) failure = errno;
	if (::close(targetFd) != 0 && !failure) failure = errno;
	if (failure) {
		::unlink(target.c_str());
		error = std::error_code(failure, std::generic_category());
		return false;
	}
	::unlink(source.c_str());
	error.clear();
	return true;
#endif
}

bool copyReferencedAsset(const std::string& sourceRoot, const std::string& relative,
		const std::filesystem::path& destination, std::string& error)
{
	if (relative.empty()) return true;
	if (!validAssetPath(relative)) { error = "Unsafe asset path: " + relative; return false; }
	const std::filesystem::path source = utf8Path(sourceRoot) / utf8Path(relative);
	std::error_code ec;
	std::filesystem::create_directories(destination.parent_path(), ec);
	if (ec) { error = "Cannot create asset folder: " + ec.message(); return false; }
	if (std::filesystem::exists(destination, ec)) {
		error = "Export would overwrite an existing asset: " + pathString(destination);
		return false;
	}
	if (std::filesystem::is_regular_file(source, ec) && !ec) {
		const auto canonicalRoot = std::filesystem::weakly_canonical(utf8Path(sourceRoot), ec);
		if (ec) { error = "Cannot resolve theme asset folder: " + sourceRoot; return false; }
		const auto canonicalSource = std::filesystem::canonical(source, ec);
		if (ec) { error = "Cannot resolve referenced asset: " + pathString(source); return false; }
		const auto relativeToRoot = std::filesystem::relative(canonicalSource, canonicalRoot, ec);
		if (ec || relativeToRoot.empty() || *relativeToRoot.begin() == "..") {
			error = "Referenced asset resolves outside the theme folder: " + pathString(source);
			return false;
		}
		if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec) || ec) {
			error = "Cannot copy referenced asset to " + pathString(destination) + ": " + ec.message();
			return false;
		}
		return true;
	}
	// Import sources may come from a platform storage provider (for example an
	// Android document picker URI) rather than a native filesystem path.
	std::unique_ptr<hostfs::File> input(hostfs::storage().openFile(hostfs::storage().getSubPath(sourceRoot, relative), "rb"));
	if (!input) { error = "Referenced asset is missing or unreadable: " + pathString(source); return false; }
	nowide::ofstream output(destination, std::ios::binary | std::ios::out);
	if (!output) { error = "Cannot create referenced asset: " + pathString(destination); return false; }
	auto failWrite = [&]() {
		output.close();
		std::error_code removeError;
		std::filesystem::remove(destination, removeError);
		return false;
	};
	char buffer[32768];
	for (;;) {
		const size_t count = input->read(buffer, 1, sizeof(buffer));
		if (count && !output.write(buffer, static_cast<std::streamsize>(count))) {
			error = "Cannot write referenced asset: " + pathString(destination); return failWrite();
		}
		if (count < sizeof(buffer)) {
			if (input->error()) { error = "Cannot read referenced asset: " + pathString(source); return failWrite(); }
			break;
		}
	}
	if (!finishOutput(output)) {
		error = "Cannot write referenced asset: " + pathString(destination);
		std::error_code removeError;
		std::filesystem::remove(destination, removeError);
		return false;
	}
	return true;
}

template<typename Paths>
bool copyThemeAssets(const Theme& theme, const std::string& root,
		const std::filesystem::path& destination, Paths&& rewrite, std::string& error,
		std::vector<std::filesystem::path>* copiedFiles = nullptr)
{
	std::set<std::string> copied;
	std::vector<std::filesystem::path> createdFiles;
	auto rollback = [&]() { std::error_code ec; for (const auto& file : createdFiles) std::filesystem::remove(file, ec); };
	auto copyOne = [&](std::string& reference) {
		if (reference.empty()) return true;
		if (!validAssetPath(reference)) { error = "Unsafe asset path: " + reference; return false; }
		const std::filesystem::path source = utf8Path(root) / utf8Path(reference);
		const std::string relative = rewrite(reference);
		if (!copied.insert(relative).second) return true;
		const auto outputPath = destination / utf8Path(relative);
		if (!copyReferencedAsset(root, reference, outputPath, error)) return false;
		createdFiles.push_back(outputPath);
		reference = relative;
		return true;
	};
	Theme copy = theme;
	if (!copyOne(copy.assets.bodyFont) || !copyOne(copy.assets.boldFont) || !copyOne(copy.assets.background)) { rollback(); return false; }
	for (auto& icon : copy.assets.icons) if (!copyOne(icon.second)) { rollback(); return false; }
	if (copiedFiles) *copiedFiles = std::move(createdFiles);
	return true;
}

} // namespace

const std::array<Builtin, CustomThemeId>& builtins() { return Builtins; }
std::string directory() { return get_writable_config_path("themes/"); }
std::string builtinDirectory() { return directory() + "builtin/"; }

bool parse(const std::string& contents, Theme& theme, std::string& error)
{
	return parseContents(contents, theme, error, true);
}

std::string serialize(const Theme& theme, std::string& error)
{
	try {
		nlohmann::json json;
		json["base"] = builtin(theme.base).name;
		auto& colors = json["colors"] = nlohmann::json::object();
		for (const auto& color : theme.colors) {
			if (color.first < 0 || color.first >= ImGuiCol_COUNT) { error = "Theme has an invalid color index"; return {}; }
			const ImVec4& c = color.second;
			colors[ImGui::GetStyleColorName(color.first)] = {c.x, c.y, c.z, c.w};
		}
		for (const auto& role : theme.roles) colors[role.first] = {role.second.x, role.second.y, role.second.z, role.second.w};
		if (!theme.widgetRoles.empty()) {
			auto& widgets = json["widgets"] = nlohmann::json::object();
			for (const auto& widget : theme.widgetRoles)
				for (const auto& role : widget.second)
					widgets[widget.first][role.first] = {role.second.x, role.second.y, role.second.z, role.second.w};
		}
		if (!theme.scalars.empty() || !theme.vectors.empty()) {
			auto& style = json["style"] = nlohmann::json::object();
			for (const auto& value : theme.scalars) {
				bool found = false;
				for (const auto& field : ScalarFields) if (field.member == value.first) { style[field.name] = value.second; found = true; break; }
				if (!found) { error = "Theme has an unsupported style field"; return {}; }
			}
			for (const auto& value : theme.vectors) {
				bool found = false;
				for (const auto& field : VectorFields) if (field.member == value.first) { style[field.name] = {value.second.x, value.second.y}; found = true; break; }
				if (!found) { error = "Theme has an unsupported style field"; return {}; }
			}
		}
		if (!theme.assets.bodyFont.empty() || !theme.assets.boldFont.empty()) {
			auto& fonts = json["fonts"] = nlohmann::json::object();
			if (!theme.assets.bodyFont.empty()) fonts["body"] = theme.assets.bodyFont;
			if (!theme.assets.boldFont.empty()) fonts["bold"] = theme.assets.boldFont;
		}
		if (!theme.assets.background.empty() || !theme.assets.icons.empty()
				|| theme.assets.backgroundOpacity != 0.22f || theme.assets.backgroundFit != "cover") {
			auto& assets = json["assets"] = nlohmann::json::object();
			if (!theme.assets.background.empty()) assets["background"] = theme.assets.background;
			if (!theme.assets.icons.empty()) assets["icons"] = theme.assets.icons;
			assets["backgroundOpacity"] = theme.assets.backgroundOpacity;
			assets["backgroundFit"] = theme.assets.backgroundFit;
		}
		if (theme.layout.hasDensity || theme.layout.hasSettingsSidebarWidth || theme.layout.hasLibraryCoverSpacing || theme.layout.hasArtworkCornerRadius) {
			auto& layout = json["layout"] = nlohmann::json::object();
			if (theme.layout.hasDensity) layout["density"] = theme.layout.density;
			if (theme.layout.hasSettingsSidebarWidth) layout["settingsSidebarWidth"] = theme.layout.settingsSidebarWidth;
			if (theme.layout.hasLibraryCoverSpacing) layout["libraryCoverSpacing"] = theme.layout.libraryCoverSpacing;
			if (theme.layout.hasArtworkCornerRadius) layout["artworkCornerRadius"] = theme.layout.artworkCornerRadius;
		}
		if (!theme.preservedFieldsJson.empty()) {
			const auto extensions = nlohmann::json::parse(theme.preservedFieldsJson);
			if (!extensions.is_object()) { error = "Theme extension fields are invalid"; return {}; }
			for (auto field = extensions.begin(); field != extensions.end(); ++field)
				if (!json.contains(field.key())) json[field.key()] = field.value();
		}
		error.clear();
		return json.dump(2) + "\n";
	} catch (const nlohmann::json::exception& e) { error = e.what(); return {}; }
}

void setActiveAssets(const Theme::Assets& assets) { ActiveAssets = assets; }
std::string assetPath(const std::string& name)
{
	std::string relative;
	if (name == "background") relative = ActiveAssets.background;
	else { auto it = ActiveAssets.icons.find(name); if (it != ActiveAssets.icons.end()) relative = it->second; }
	return relative.empty() ? std::string() : ActiveAssets.directory + relative;
}
std::vector<std::string> assetPaths()
{
	std::vector<std::string> paths;
	if (!ActiveAssets.background.empty()) paths.push_back(ActiveAssets.directory + ActiveAssets.background);
	for (const auto& icon : ActiveAssets.icons) paths.push_back(ActiveAssets.directory + icon.second);
	return paths;
}
std::string fontPath(bool bold)
{
	const std::string& relative = bold ? ActiveAssets.boldFont : ActiveAssets.bodyFont;
	return relative.empty() ? std::string() : ActiveAssets.directory + relative;
}
float backgroundOpacity() { return ActiveAssets.backgroundOpacity; }
std::string backgroundFit() { return ActiveAssets.backgroundFit; }
void setActiveLayout(const Theme::Layout& layout) { ActiveLayout = layout; }
const Theme::Layout& activeLayout() { return ActiveLayout; }

bool loadFromDirectory(const std::string& path, const std::string& filename, Theme& theme, std::string& error)
{
	return loadFile(path, filename, theme, error, true);
}

bool loadBundled(int id, Theme& theme, std::string& error)
{
	const auto& entry = builtin(id);
	size_t size = 0;
	auto data = resource::load(entry.resource, size);
	if (!data) {
		error = "Cannot load bundled theme: " + std::string(entry.resource);
		return false;
	}
	return parseContents(std::string(reinterpret_cast<const char*>(data.get()), size), theme, error, false);
}

bool seedBuiltins(const std::string& path)
{
	(void)path;
	return true;
}

bool loadBuiltinFromDirectory(const std::string& path, int id, Theme& theme, std::string& error)
{
	(void)path;
	return loadBundled(id, theme, error);
}

namespace {
bool inspectMarkerStatus(const std::filesystem::path& markerPath,
		std::filesystem::file_status& status, std::string& error)
{
	std::error_code ec;
	status = std::filesystem::symlink_status(markerPath, ec);
	if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
		status = std::filesystem::file_status(std::filesystem::file_type::not_found);
		return true;
	}
	if (ec) {
		error = "Cannot inspect legacy theme migration marker: " + ec.message();
		return false;
	}
	return true;
}

bool completionMarkerMatches(const std::string& path, int id, const std::string& filename,
		std::string& error)
{
	const auto markerPath = utf8Path(path) / utf8Path(".builtin-migrated-" + std::to_string(id));
	std::filesystem::file_status status;
	if (!inspectMarkerStatus(markerPath, status, error)) return false;
	if (status.type() != std::filesystem::file_type::regular) {
		error = status.type() == std::filesystem::file_type::not_found
				? "Legacy theme migration marker is missing"
				: "Legacy theme migration marker is not a regular file";
		return false;
	}
	nowide::ifstream marker(markerPath, std::ios::binary);
	if (!marker) { error = "Cannot open legacy theme migration marker"; return false; }
	const std::string contents((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
	if (marker.bad()) { error = "Cannot read legacy theme migration marker"; return false; }
	if (contents != filename) {
		error = "Legacy theme migration marker does not match the pending theme";
		return false;
	}
	error.clear();
	return true;
}
} // namespace

bool markLegacyBuiltinMigrationComplete(const std::string& path, int id,
		const std::string& filename, std::string& error)
{
	if (id < 0 || id >= CustomThemeId || !validFilename(filename)) {
		error = "Invalid legacy theme migration marker values";
		return false;
	}
	std::error_code ec;
	const auto root = utf8Path(path);
	const auto markerPath = root / utf8Path(".builtin-migrated-" + std::to_string(id));
	std::filesystem::file_status markerStatus;
	if (!inspectMarkerStatus(markerPath, markerStatus, error)) return false;
	if (markerStatus.type() == std::filesystem::file_type::regular) {
		std::string markerError;
		if (completionMarkerMatches(path, id, filename, markerError)) { error.clear(); return true; }
		int pendingId = -1;
		std::string pendingFilename, pendingError;
		if (!readPendingBuiltinMigration(path, pendingId, pendingFilename, pendingError)
				|| pendingId != id || pendingFilename != filename) {
			error = pendingError.empty() ? markerError : pendingError;
			return false;
		}
		// A matching durable pending record proves this app-owned marker belongs
		// to the interrupted operation, so it can be atomically repaired below.
	} else if (markerStatus.type() != std::filesystem::file_type::not_found) {
		error = "Legacy theme migration marker is not a regular file";
		return false;
	}
	auto markerTemp = markerPath.parent_path() / utf8Path(pathString(markerPath.filename()) + ".tmp");
	for (int suffix = 2; std::filesystem::exists(markerTemp, ec); ++suffix)
		markerTemp = markerPath.parent_path() / utf8Path(pathString(markerPath.filename()) + ".tmp" + std::to_string(suffix));
	if (ec) { error = "Cannot reserve legacy theme migration marker temp file: " + ec.message(); return false; }
	nowide::ofstream marker(markerTemp, std::ios::binary | std::ios::out | std::ios::trunc);
	bool markerWritten = static_cast<bool>(marker);
	if (markerWritten) markerWritten = static_cast<bool>(marker << filename);
	const bool markerClosed = marker.is_open() && finishOutput(marker);
	if (!markerWritten || !markerClosed) {
		std::filesystem::remove(markerTemp, ec);
		error = "Cannot write legacy theme migration marker";
		return false;
	}
	if (!inspectMarkerStatus(markerPath, markerStatus, error)) {
		std::error_code cleanupError;
		std::filesystem::remove(markerTemp, cleanupError);
		return false;
	}
	if (markerStatus.type() == std::filesystem::file_type::regular) {
		std::string markerError;
		if (completionMarkerMatches(path, id, filename, markerError)) {
			std::filesystem::remove(markerTemp, ec);
			error.clear();
			return true;
		}
		int pendingId = -1;
		std::string pendingFilename, pendingError;
		if (readPendingBuiltinMigration(path, pendingId, pendingFilename, pendingError)
				&& pendingId == id && pendingFilename == filename) {
			if (atomicReplaceFile(markerTemp, markerPath, ec)) { error.clear(); return true; }
			const std::string replaceError = ec.message();
			std::error_code cleanupError;
			std::filesystem::remove(markerTemp, cleanupError);
			error = "Cannot repair interrupted legacy theme migration marker: " + replaceError;
			return false;
		}
		std::error_code cleanupError;
		std::filesystem::remove(markerTemp, cleanupError);
		error = pendingError.empty() ? markerError : pendingError;
		return false;
	}
	if (markerStatus.type() != std::filesystem::file_type::not_found) {
		std::error_code cleanupError;
		std::filesystem::remove(markerTemp, cleanupError);
		error = "Legacy theme migration marker is not a regular file";
		return false;
	}
	if (!publishNewFile(markerTemp, markerPath, ec)) {
		const std::string publishError = ec.message();
		std::error_code cleanupError;
		std::filesystem::remove(markerTemp, cleanupError);
		error = "Cannot record legacy theme migration: " + publishError;
		return false;
	}
	error.clear();
	return true;
}

bool readPendingBuiltinMigration(const std::string& path, int& id,
		std::string& filename, std::string& error)
{
	const auto pendingPath = utf8Path(path) / ".builtin-migration-pending";
	nowide::ifstream pending(pendingPath, std::ios::binary);
	if (!pending) {
		std::error_code ec;
		if (!std::filesystem::exists(pendingPath, ec) && !ec) { error.clear(); return false; }
		error = "Cannot open pending legacy theme migration";
		return false;
	}
	std::string contents((std::istreambuf_iterator<char>(pending)), std::istreambuf_iterator<char>());
	try {
		const auto record = nlohmann::json::parse(contents);
		if (!record.is_object() || !record.contains("id") || !record["id"].is_number_integer()
				|| !record.contains("filename") || !record["filename"].is_string()) {
			error = "Invalid pending legacy theme migration record";
			return false;
		}
		const int savedId = record["id"].get<int>();
		const std::string savedFilename = record["filename"].get<std::string>();
		if (savedId < 0 || savedId >= CustomThemeId || !validFilename(savedFilename)) {
			error = "Invalid pending legacy theme migration values";
			return false;
		}
		id = savedId;
		filename = savedFilename;
		error.clear();
		return true;
	} catch (const nlohmann::json::exception& e) {
		error = std::string("Cannot read pending legacy theme migration: ") + e.what();
		return false;
	}
}

bool writePendingBuiltinMigration(const std::string& path, int id,
		const std::string& filename, std::string& error)
{
	if (id < 0 || id >= CustomThemeId || !validFilename(filename)) {
		error = "Invalid pending legacy theme migration values";
		return false;
	}
	const auto root = utf8Path(path);
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	if (ec) { error = "Cannot create pending migration folder: " + ec.message(); return false; }
	const auto pendingPath = root / ".builtin-migration-pending";
	if (std::filesystem::exists(pendingPath, ec)) {
		int existingId = -1;
		std::string existingFilename, readError;
		if (readPendingBuiltinMigration(path, existingId, existingFilename, readError)
				&& existingId == id && existingFilename == filename) { error.clear(); return true; }
		error = readError.empty() ? "A different legacy theme migration is already pending" : readError;
		return false;
	}
	auto temporary = root / ".builtin-migration-pending.tmp";
	for (int suffix = 2; std::filesystem::exists(temporary, ec); ++suffix)
		temporary = root / utf8Path(".builtin-migration-pending.tmp" + std::to_string(suffix));
	if (ec) { error = "Cannot reserve pending migration temp file: " + ec.message(); return false; }
	nowide::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
	const std::string contents = nlohmann::json{{"id", id}, {"filename", filename}}.dump(2) + "\n";
	bool outputWritten = static_cast<bool>(output);
	if (outputWritten) outputWritten = static_cast<bool>(output << contents);
	const bool outputClosed = output.is_open() && finishOutput(output);
	if (!outputWritten || !outputClosed) {
		std::filesystem::remove(temporary, ec);
		error = "Cannot write pending legacy theme migration";
		return false;
	}
	if (std::filesystem::exists(pendingPath, ec)) {
		std::filesystem::remove(temporary, ec);
		int existingId = -1;
		std::string existingFilename, readError;
		if (readPendingBuiltinMigration(path, existingId, existingFilename, readError)
				&& existingId == id && existingFilename == filename) {
			error.clear();
			return true;
		}
		error = readError.empty() ? "A different legacy theme migration is already pending" : readError;
		return false;
	}
	std::filesystem::rename(temporary, pendingPath, ec);
	if (ec) {
		const std::string renameError = ec.message();
		std::error_code cleanupError;
		std::filesystem::remove(temporary, cleanupError);
		error = "Cannot record pending legacy theme migration: " + renameError;
		return false;
	}
	std::filesystem::remove(temporary, ec);
	error.clear();
	return true;
}

bool clearPendingBuiltinMigration(const std::string& path, std::string& error)
{
	int id = -1;
	std::string filename;
	if (!readPendingBuiltinMigration(path, id, filename, error)) {
		if (error.empty()) return true;
		return false;
	}
	if (!completionMarkerMatches(path, id, filename, error)) return false;
	std::error_code ec;
	std::filesystem::remove(utf8Path(path) / ".builtin-migration-pending", ec);
	if (ec) { error = "Cannot clear pending legacy theme migration: " + ec.message(); return false; }
	error.clear();
	return true;
}

bool migrateLegacyBuiltin(const std::string& path, int id, std::string& filename,
		std::string& error, bool recordCompletion)
{
	if (id < 0 || id >= CustomThemeId) { error = "Invalid built-in theme id"; return false; }
	const auto& entry = builtin(id);
	const auto root = utf8Path(path);
	const auto markerPath = root / utf8Path(".builtin-migrated-" + std::to_string(id));
	std::error_code ec;
	if (std::filesystem::exists(markerPath, ec)) { error.clear(); return false; }
	if (ec) { error = "Cannot inspect legacy theme migration marker: " + ec.message(); return false; }
	const auto sourcePath = root / "builtin" / utf8Path(entry.filename);
	nowide::ifstream source(sourcePath, std::ios::binary);
	if (!source) { error.clear(); return false; }
	std::string contents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
	if (source.bad()) { error = "Cannot read legacy built-in theme"; return false; }
	size_t bundledSize = 0;
	auto bundledData = resource::load(entry.resource, bundledSize);
	if (!bundledData) { error = "Cannot load bundled theme for migration"; return false; }
	if (contents == std::string(reinterpret_cast<const char*>(bundledData.get()), bundledSize)) { error.clear(); return false; }
	Theme theme;
	if (!parseContents(contents, theme, error, false)) return false;
	theme.base = id;
	theme.assets.directory = pathString(root / "builtin");
	const std::string stem = std::string(entry.name) + " (Customized)";
	std::filesystem::create_directories(root, ec);
	if (ec) { error = "Cannot create custom theme folder: " + ec.message(); return false; }
	const auto assetsRoot = root / "assets";
	std::filesystem::create_directories(assetsRoot, ec);
	if (ec) { error = "Cannot create custom theme asset folder: " + ec.message(); return false; }
	std::string candidate = stem;
	auto candidateAssets = assetsRoot / utf8Path(candidate);
	std::string serialized;
	for (int suffix = 2; ; ++suffix) {
		Theme candidateTheme = theme;
		const std::string prefix = "assets/" + candidate + "/";
		auto rewriteCandidate = [&](std::string& asset) { if (!asset.empty()) asset = prefix + asset; };
		rewriteCandidate(candidateTheme.assets.bodyFont); rewriteCandidate(candidateTheme.assets.boldFont);
		rewriteCandidate(candidateTheme.assets.background);
		for (auto& icon : candidateTheme.assets.icons) rewriteCandidate(icon.second);
		candidateTheme.assets.directory = path;
		serialized = serialize(candidateTheme, error);
		if (!error.empty()) return false;
		const auto target = root / utf8Path(candidate + ".json");
		if (std::filesystem::exists(target, ec)) {
			if (ec) { error = "Cannot inspect custom theme folder: " + ec.message(); return false; }
			nowide::ifstream existing(target, std::ios::binary);
			const std::string existingContents((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
			auto copiedAssetsMatch = [&]() {
				std::vector<std::string> refs;
				if (!theme.assets.bodyFont.empty()) refs.push_back(theme.assets.bodyFont);
				if (!theme.assets.boldFont.empty()) refs.push_back(theme.assets.boldFont);
				if (!theme.assets.background.empty()) refs.push_back(theme.assets.background);
				for (const auto& icon : theme.assets.icons) refs.push_back(icon.second);
				for (const auto& ref : refs) {
					const auto original = utf8Path(theme.assets.directory) / utf8Path(ref);
					const auto copied = candidateAssets / utf8Path(ref);
					std::error_code assetError;
					if (!std::filesystem::is_regular_file(original, assetError) || assetError
							|| !std::filesystem::is_regular_file(copied, assetError) || assetError)
						return false;
					const auto originalSize = std::filesystem::file_size(original, assetError);
					if (assetError) return false;
					const auto copiedSize = std::filesystem::file_size(copied, assetError);
					if (assetError || originalSize != copiedSize) return false;
					nowide::ifstream originalFile(original, std::ios::binary), copiedFile(copied, std::ios::binary);
					const std::string originalBytes((std::istreambuf_iterator<char>(originalFile)), std::istreambuf_iterator<char>());
					const std::string copiedBytes((std::istreambuf_iterator<char>(copiedFile)), std::istreambuf_iterator<char>());
					if (originalFile.bad() || copiedFile.bad() || originalBytes != copiedBytes) return false;
				}
				return true;
			};
			if (!existing.bad() && existingContents == serialized && copiedAssetsMatch()) {
				filename = candidate + ".json";
				return !recordCompletion || markLegacyBuiltinMigrationComplete(path, id, filename, error);
			}
			candidate = stem + " " + std::to_string(suffix);
			candidateAssets = assetsRoot / utf8Path(candidate);
			continue;
		}
		if (std::filesystem::create_directory(candidateAssets, ec)) break;
		if (ec) { error = "Cannot reserve custom theme asset folder: " + ec.message(); return false; }
		candidate = stem + " " + std::to_string(suffix);
		candidateAssets = assetsRoot / utf8Path(candidate);
	}
	if (!copyThemeAssets(theme, theme.assets.directory, root,
			[&](const std::string& asset) { return "assets/" + candidate + "/" + asset; }, error)) {
		std::filesystem::remove_all(candidateAssets, ec);
		return false;
	}
	const auto target = root / utf8Path(candidate + ".json");
	FILE* output = nowide::fopen(pathString(target).c_str(), "wbx");
	if (!output && errno == EEXIST) {
		std::filesystem::remove_all(candidateAssets, ec);
		error = "A custom theme appeared during legacy migration; retry the migration";
		return false;
	}
	const bool createdTarget = output != nullptr;
	bool themeWritten = output && std::fwrite(serialized.data(), 1, serialized.size(), output) == serialized.size();
	if (output && std::fclose(output) != 0) themeWritten = false;
	if (!themeWritten) {
		if (createdTarget) std::filesystem::remove(target, ec);
		std::filesystem::remove_all(candidateAssets, ec);
		error = "Cannot write migrated custom theme";
		return false;
	}
	const std::string migratedFilename = candidate + ".json";
	filename = migratedFilename;
	error.clear();
	return !recordCompletion || markLegacyBuiltinMigrationComplete(path, id, filename, error);
}

static void applyImpl(const Theme& theme, ImGuiStyle& style, bool updateRoles)
{
	auto applyPaletteLayer = [&](bool hasAccent, const ImVec4& active,
			const std::vector<std::pair<int, ImVec4>>& colors,
			const std::map<std::string, ImVec4>& roles,
			const std::map<std::string, std::map<std::string, ImVec4>>& widgetRoles) {
		// Accent-derived controls are applied first; explicitly authored ImGui colors
		// in this layer retain their precedence over them.
		if (hasAccent) {
			const ImVec4 base = style.Colors[ImGuiCol_FrameBg];
			const ImVec4 hoverBase = style.Colors[ImGuiCol_FrameBgHovered];
			style.Colors[ImGuiCol_Button] = mix(base, active, 0.36f);
			style.Colors[ImGuiCol_ButtonHovered] = mix(hoverBase, active, 0.68f);
			style.Colors[ImGuiCol_ButtonActive] = active;
			style.Colors[ImGuiCol_Header] = mix(base, active, 0.42f);
			style.Colors[ImGuiCol_HeaderHovered] = mix(hoverBase, active, 0.72f);
			style.Colors[ImGuiCol_HeaderActive] = active;
			style.Colors[ImGuiCol_CheckMark] = active;
			style.Colors[ImGuiCol_SliderGrab] = active;
			const float luminance = active.x * 0.2126f + active.y * 0.7152f + active.z * 0.0722f;
			style.Colors[ImGuiCol_SliderGrabActive] = mix(active,
					luminance > 0.55f ? ImVec4(0, 0, 0, active.w) : ImVec4(1, 1, 1, active.w), 0.24f);
		}
		if (updateRoles) {
			for (const auto& role : roles) ActiveRoles[role.first] = role.second;
			for (const auto& widget : widgetRoles)
				for (const auto& role : widget.second) ActiveWidgetRoles[widget.first][role.first] = role.second;
		}
		for (const auto& color : colors) style.Colors[color.first] = color.second;
	};
	if (theme.paletteLayers.empty()) {
		auto accent = theme.roles.find("accent");
		applyPaletteLayer(accent != theme.roles.end(), accent == theme.roles.end() ? ImVec4() : accent->second,
				theme.colors, theme.roles, theme.widgetRoles);
	} else {
		for (const auto& layer : theme.paletteLayers)
			applyPaletteLayer(layer.hasAccent, layer.accent, layer.colors, layer.roles, layer.widgetRoles);
	}
	for (const auto& field : theme.scalars) style.*field.first = field.second;
	for (const auto& field : theme.vectors) style.*field.first = field.second;
}

void apply(const Theme& theme, ImGuiStyle& style) { applyImpl(theme, style, true); }
void applyStyle(const Theme& theme, ImGuiStyle& style) { applyImpl(theme, style, false); }

void resetRoles()
{
	ActiveRoles.clear();
	ActiveWidgetRoles.clear();
}

ImVec4 roleColor(const std::string& role, const ImVec4& fallback, const std::string& widget)
{
	if (!widget.empty()) {
		auto widgetIt = ActiveWidgetRoles.find(widget);
		if (widgetIt != ActiveWidgetRoles.end()) {
			auto roleIt = widgetIt->second.find(role);
			if (roleIt != widgetIt->second.end()) return roleIt->second;
		}
	}
	auto it = ActiveRoles.find(role);
	return it == ActiveRoles.end() ? fallback : it->second;
}

bool load(const std::string& filename, Theme& theme, std::string& error)
{
	return loadFromDirectory(directory(), filename, theme, error);
}

bool save(const std::string& filename, const Theme& theme, bool replaceExisting, std::string& error)
{
	if (!validFilename(filename)) { error = "Theme name must be a .json filename without folder separators"; return false; }
	for (const std::string* asset : {&theme.assets.bodyFont, &theme.assets.boldFont, &theme.assets.background}) {
		if (asset->empty()) continue;
		if (!validAssetPath(*asset)) { error = "Unsafe asset path: " + *asset; return false; }
		std::error_code assetError;
		if (!std::filesystem::is_regular_file(utf8Path(theme.assets.directory) / utf8Path(*asset), assetError) || assetError) {
			error = "Referenced asset is missing or unreadable: " + pathString(utf8Path(theme.assets.directory) / utf8Path(*asset));
			return false;
		}
	}
	for (const auto& icon : theme.assets.icons) {
		if (!validAssetPath(icon.second)) { error = "Unsafe asset path: " + icon.second; return false; }
		std::error_code assetError;
		if (!std::filesystem::is_regular_file(utf8Path(theme.assets.directory) / utf8Path(icon.second), assetError) || assetError) {
			error = "Referenced asset is missing or unreadable: " + pathString(utf8Path(theme.assets.directory) / utf8Path(icon.second));
			return false;
		}
	}
	const std::string output = serialize(theme, error);
	if (!error.empty()) return false;
	std::error_code ec;
	const auto themeDirectory = utf8Path(directory());
	std::filesystem::create_directories(themeDirectory, ec);
	if (ec) { error = "Cannot create custom theme folder: " + ec.message(); return false; }
	const auto target = themeDirectory / utf8Path(filename);
	const bool targetExists = std::filesystem::exists(target, ec);
	if (ec) { error = "Cannot inspect theme file: " + ec.message(); return false; }
	if (!replaceExisting && targetExists) { error = "A theme with that name already exists"; return false; }
	auto temporary = target;
	temporary += ".tmp";
	for (int suffix = 2; std::filesystem::exists(temporary, ec); ++suffix)
		temporary = target.parent_path() / utf8Path(target.filename().u8string() + ".tmp" + std::to_string(suffix));
	if (ec) { error = "Cannot inspect temporary theme file: " + ec.message(); return false; }
	{
		nowide::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		const bool wrote = file && static_cast<bool>(file << output);
		const bool closed = finishOutput(file);
		if (!wrote || !closed) {
			std::filesystem::remove(temporary, ec);
			error = "Cannot write theme file: " + pathString(temporary);
			return false;
		}
	}
	const bool saved = targetExists ? atomicReplaceFile(temporary, target, ec)
			: publishNewFile(temporary, target, ec);
	if (!saved) {
		const std::string saveError = ec.message();
		std::filesystem::remove(temporary, ec);
		error = "Cannot save theme: " + saveError;
		return false;
	}
	error.clear();
	return true;
}

bool exportToDirectory(const Theme& original, const std::string& filename,
		const std::string& destination, std::string& error)
{
	if (!validFilename(filename)) { error = "Export name must be a .json filename without folder separators"; return false; }
	Theme theme = original;
	const auto root = utf8Path(destination);
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	if (ec) { error = "Cannot create export folder: " + ec.message(); return false; }
	const auto jsonPath = root / utf8Path(filename);
	const bool exportExists = std::filesystem::exists(jsonPath, ec);
	if (ec) { error = "Cannot inspect export destination: " + ec.message(); return false; }
	if (exportExists) { error = "Export would overwrite an existing file: " + pathString(jsonPath); return false; }
	std::vector<std::filesystem::path> copiedAssets;
	if (!copyThemeAssets(theme, theme.assets.directory, root,
			[](const std::string& path) { return path; }, error, &copiedAssets)) return false;
	auto rollbackAssets = [&]() {
		for (const auto& asset : copiedAssets) std::filesystem::remove(asset, ec);
	};
	const std::string contents = serialize(theme, error);
	if (!error.empty()) { rollbackAssets(); return false; }
	auto temporary = jsonPath;
	temporary += ".tmp";
	for (int suffix = 2; std::filesystem::exists(temporary, ec); ++suffix)
		temporary = root / utf8Path(filename + ".tmp" + std::to_string(suffix));
	if (ec) { rollbackAssets(); error = "Cannot inspect temporary export file: " + ec.message(); return false; }
	nowide::ofstream file(temporary, std::ios::binary | std::ios::out);
	const bool wrote = file && static_cast<bool>(file << contents);
	const bool closed = finishOutput(file);
	if (!wrote || !closed) {
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = "Cannot write exported theme: " + pathString(temporary);
		return false;
	}
	const bool destinationExists = std::filesystem::exists(jsonPath, ec);
	if (ec || destinationExists) {
		const std::error_code inspectError = ec;
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = inspectError ? "Cannot inspect export destination: " + inspectError.message()
				: "Export would overwrite an existing file: " + pathString(jsonPath);
		return false;
	}
	if (!publishNewFile(temporary, jsonPath, ec)) {
		const std::string publishError = ec.message();
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = "Cannot publish exported theme: " + publishError;
		return false;
	}
	error.clear();
	return true;
}

bool importFromFile(const std::string& source, const std::string& destination,
		std::string& importedFilename, std::string& error)
{
	std::unique_ptr<hostfs::File> input(hostfs::storage().openFile(source, "rb"));
	if (!input) { error = "Cannot open import file"; return false; }
	const s64 size = input->size();
	if (size < 0 || size > static_cast<s64>(MaxThemeSize)) { error = "Theme file is too large"; return false; }
	std::string contents(static_cast<size_t>(size), '\0');
	if (size && input->read(&contents[0], 1, static_cast<size_t>(size)) != static_cast<size_t>(size)) { error = "Cannot read import file"; return false; }
	try {
		const auto raw = nlohmann::json::parse(contents);
		auto check = [&](const nlohmann::json& object, const char* key) {
			if (object.contains(key) && (!object[key].is_string() || !validAssetPath(object[key].get<std::string>()))) {
				error = std::string("Import contains an unsafe asset reference: ") + key;
				return false;
			}
			return true;
		};
		if (raw.contains("fonts") && raw["fonts"].is_object()
				&& (!check(raw["fonts"], "body") || !check(raw["fonts"], "bold"))) return false;
		if (raw.contains("assets") && raw["assets"].is_object()) {
			if (!check(raw["assets"], "background")) return false;
			if (raw["assets"].contains("icons") && raw["assets"]["icons"].is_object())
				for (auto it = raw["assets"]["icons"].begin(); it != raw["assets"]["icons"].end(); ++it)
					if (!it.value().is_string() || !validAssetPath(it.value().get<std::string>())) {
						error = "Import contains an unsafe asset reference: assets.icons." + it.key();
						return false;
					}
		}
	} catch (const nlohmann::json::exception& e) { error = e.what(); return false; }
	Theme theme;
	if (!parse(contents, theme, error)) return false;
	const std::string sourceRoot = hostfs::storage().getParentPath(source);
	std::string sourceLeaf = source.substr(sourceRoot.size());
	while (!sourceLeaf.empty() && (sourceLeaf.front() == '/' || sourceLeaf.front() == '\\')) sourceLeaf.erase(sourceLeaf.begin());
	const auto extension = sourceLeaf.find_last_of('.');
	const std::string stem = extension == std::string::npos ? sourceLeaf : sourceLeaf.substr(0, extension);
	if (stem.empty()) { error = "Import filename has no theme name"; return false; }
	std::string candidate = stem;
	std::error_code ec;
	const auto destinationPath = utf8Path(destination);
	std::filesystem::create_directories(destinationPath, ec);
	if (ec) { error = "Cannot create custom theme folder: " + ec.message(); return false; }
	for (int suffix = 2; ; ++suffix) {
		const bool candidateExists = std::filesystem::exists(destinationPath / utf8Path(candidate + ".json"), ec);
		if (ec) { error = "Cannot inspect imported theme destination: " + ec.message(); return false; }
		if (!candidateExists) break;
		candidate = stem + " " + std::to_string(suffix);
	}
	if (!validFilename(candidate + ".json")) { error = "Import filename contains unsupported characters"; return false; }
	theme.assets.directory = sourceRoot;
	if (theme.assets.directory.empty()) theme.assets.directory = ".";
	const std::string assetPrefix = "assets/" + candidate;
	std::vector<std::filesystem::path> copiedAssets;
	if (!copyThemeAssets(theme, theme.assets.directory, destinationPath,
			[&](const std::string& path) { return assetPrefix + "/" + path; }, error, &copiedAssets)) return false;
	auto rollbackAssets = [&]() {
		for (const auto& asset : copiedAssets) std::filesystem::remove(asset, ec);
	};
	if (!theme.assets.bodyFont.empty()) theme.assets.bodyFont = assetPrefix + "/" + theme.assets.bodyFont;
	if (!theme.assets.boldFont.empty()) theme.assets.boldFont = assetPrefix + "/" + theme.assets.boldFont;
	if (!theme.assets.background.empty()) theme.assets.background = assetPrefix + "/" + theme.assets.background;
	for (auto& icon : theme.assets.icons) icon.second = assetPrefix + "/" + icon.second;
	theme.assets.directory = pathString(destinationPath);
	importedFilename = candidate + ".json";
	const std::string serialized = serialize(theme, error);
	if (!error.empty()) { rollbackAssets(); return false; }
	const auto importedPath = destinationPath / utf8Path(importedFilename);
	if (std::filesystem::exists(importedPath, ec) || ec) {
		rollbackAssets();
		error = ec ? "Cannot inspect imported theme destination: " + ec.message()
				: "An imported theme with that name already exists";
		return false;
	}
	auto temporary = importedPath;
	temporary += ".tmp";
	for (int suffix = 2; std::filesystem::exists(temporary, ec); ++suffix)
		temporary = destinationPath / utf8Path(importedFilename + ".tmp" + std::to_string(suffix));
	if (ec) { rollbackAssets(); error = "Cannot inspect temporary import file: " + ec.message(); return false; }
	nowide::ofstream output(temporary, std::ios::binary | std::ios::out);
	const bool wrote = output && static_cast<bool>(output << serialized);
	const bool closed = finishOutput(output);
	if (!wrote || !closed) {
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = "Cannot write imported theme file: " + pathString(temporary);
		return false;
	}
	const bool destinationExists = std::filesystem::exists(importedPath, ec);
	if (ec || destinationExists) {
		const std::error_code inspectError = ec;
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = inspectError ? "Cannot inspect imported theme destination: " + inspectError.message()
				: "An imported theme with that name already exists";
		return false;
	}
	if (!publishNewFile(temporary, importedPath, ec)) {
		std::filesystem::remove(temporary, ec);
		rollbackAssets();
		error = "Cannot publish imported theme: " + pathString(importedPath);
		return false;
	}
	error.clear();
	return true;
}

bool importFile(const std::string& source, std::string& importedFilename, std::string& error)
{
	return importFromFile(source, directory(), importedFilename, error);
}

std::vector<std::string> list(const std::string& path)
{
	std::vector<std::string> files;
	DIR* dir = flycast::opendir(path.c_str());
	if (!dir) return files;
	while (dirent* entry = flycast::readdir(dir)) {
		std::string filename(entry->d_name);
		if (validFilename(filename)) files.push_back(filename);
	}
	flycast::closedir(dir);
	std::sort(files.begin(), files.end());
	return files;
}

std::vector<std::string> list() { return list(directory()); }
}
