// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/custom_theme.h"
#include "ui/theme_font_validation.h"
#include "oslib/resources.h"
#include <filesystem>
#include <fstream>
#include <locale>
#include <random>
#include <map>
#include <sstream>
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "stdclass.h"
#include "ui/settings.h"
#ifdef _WIN32
#include <windows.h>
#endif

TEST(CustomThemeTest, ParsesBaseColorsAndStyle)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Light","colors":{"Text":"#12345678","Button":"#ABCDEF"},"style":{"FrameRounding":12,"ItemSpacing":[8,16],"ButtonTextAlign":[0.5,1]}})", theme, error)) << error;
	EXPECT_EQ(theme.base, 1);
	ASSERT_EQ(theme.colors.size(), 2u);
	for (const auto& color : theme.colors) {
		if (color.first == ImGuiCol_Text) {
			EXPECT_NEAR(color.second.x, 0x12 / 255.f, 0.0001f);
			EXPECT_NEAR(color.second.w, 0x78 / 255.f, 0.0001f);
		} else if (color.first == ImGuiCol_Button) {
			EXPECT_FLOAT_EQ(color.second.w, 1.f);
		} else FAIL() << "Unexpected color";
	}
	ImGuiStyle style;
	for (const auto& field : theme.scalars) style.*field.first = field.second;
	for (const auto& field : theme.vectors) style.*field.first = field.second;
	EXPECT_FLOAT_EQ(style.FrameRounding, 12);
	EXPECT_FLOAT_EQ(style.ItemSpacing.y, 16);
	EXPECT_FLOAT_EQ(style.ButtonTextAlign.y, 1);
}

TEST(CustomThemeTest, RejectsNearZeroGlobalAlpha)
{
	CustomTheme::Theme theme;
	std::string error;
	EXPECT_FALSE(CustomTheme::parse(R"({"style":{"Alpha":0.19}})", theme, error));
	EXPECT_NE(error.find("Alpha"), std::string::npos);
	EXPECT_TRUE(CustomTheme::parse(R"({"style":{"Alpha":0.2}})", theme, error)) << error;
}

TEST(CustomThemeTest, ParsesLayoutControlsWithStableDefaultsAndBounds)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"style":{"ItemSpacing":[13,17]},"layout":{"density":1.25,"settingsSidebarWidth":260,"libraryCoverSpacing":0,"artworkCornerRadius":12}})", theme, error)) << error;
	EXPECT_FLOAT_EQ(theme.layout.density, 1.25f);
	EXPECT_FLOAT_EQ(theme.layout.settingsSidebarWidth, 260.0f);
	EXPECT_FLOAT_EQ(theme.layout.libraryCoverSpacing, 0.0f);
	EXPECT_FLOAT_EQ(theme.layout.artworkCornerRadius, 12.0f);
	EXPECT_TRUE(theme.layout.hasDensity);
	EXPECT_TRUE(theme.layout.hasSettingsSidebarWidth);
	EXPECT_TRUE(theme.layout.hasLibraryCoverSpacing);
	EXPECT_TRUE(theme.layout.hasArtworkCornerRadius);
	ImGuiStyle style;
	CustomTheme::apply(theme, style);
	EXPECT_FLOAT_EQ(style.ItemSpacing.x, 13.0f);
	EXPECT_FLOAT_EQ(style.ItemSpacing.y, 17.0f);
	CustomTheme::setActiveLayout(theme.layout);
	EXPECT_FLOAT_EQ(CustomTheme::activeLayout().density, 1.25f);

	CustomTheme::Theme defaults;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"Text":"#FFFFFF"}})", defaults, error)) << error;
	EXPECT_FLOAT_EQ(defaults.layout.density, 1.0f);
	EXPECT_FLOAT_EQ(defaults.layout.settingsSidebarWidth, 0.0f);
	EXPECT_FLOAT_EQ(defaults.layout.libraryCoverSpacing, -1.0f);
	EXPECT_FLOAT_EQ(defaults.layout.artworkCornerRadius, -1.0f);
	EXPECT_FALSE(defaults.layout.hasDensity);
	CustomTheme::Theme explicitDefault;
	ASSERT_TRUE(CustomTheme::parse(R"({"layout":{"density":1.0}})", explicitDefault, error)) << error;
	EXPECT_TRUE(explicitDefault.layout.hasDensity);
	EXPECT_FLOAT_EQ(explicitDefault.layout.density, 1.0f);
}

TEST(CustomThemeTest, RejectsOutOfRangeLayoutControls)
{
	CustomTheme::Theme theme;
	std::string error;
	for (const char* input : {
		R"({"layout":{"density":0.5}})", R"({"layout":{"density":1.51}})",
		R"({"layout":{"settingsSidebarWidth":135}})", R"({"layout":{"settingsSidebarWidth":311}})",
		R"({"layout":{"libraryCoverSpacing":49}})", R"({"layout":{"artworkCornerRadius":-1}})",
		R"({"layout":{"unknown":1}})"})
		EXPECT_FALSE(CustomTheme::parse(input, theme, error)) << input;
}

TEST(CustomThemeTest, ParsesLocalFontAndImageAssetsAndRejectsTraversalPerAsset)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"fonts":{"body":"fonts/body.ttf","bold":"font-bold.otf"},"assets":{"background":"img/bg.png","backgroundFit":"contain","backgroundOpacity":0.4,"icons":{"settings":"img/settings.png"}}})", theme, error)) << error;
	EXPECT_EQ(theme.assets.bodyFont, "fonts/body.ttf");
	EXPECT_EQ(theme.assets.boldFont, "font-bold.otf");
	EXPECT_EQ(theme.assets.background, "img/bg.png");
	EXPECT_EQ(theme.assets.icons.at("settings"), "img/settings.png");
	EXPECT_EQ(theme.assets.backgroundFit, "contain");
	EXPECT_FLOAT_EQ(theme.assets.backgroundOpacity, 0.4f);
	ASSERT_TRUE(CustomTheme::parse(R"({"fonts":{"body":"../outside.ttf"},"assets":{"background":"/absolute.png","icons":{"settings":"icons/../../escape.png"}}})", theme, error)) << error;
	EXPECT_TRUE(theme.assets.bodyFont.empty());
	EXPECT_TRUE(theme.assets.background.empty());
	EXPECT_TRUE(theme.assets.icons.empty());
}

TEST(CustomThemeTest, ParsesSemanticAndWidgetRolesWithHexAndRgba)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"accent":"#48B4AD","success":"#35C875","warning":[0.9,0.7,0.2,0.8]},"widgets":{"media.status":{"success":"#123456"}}})", theme, error)) << error;
	ASSERT_EQ(theme.roles.size(), 3u);
	EXPECT_NEAR(theme.roles.at("accent").x, 0x48 / 255.f, 0.0001f);
	EXPECT_FLOAT_EQ(theme.roles.at("warning").w, 0.8f);
	ASSERT_EQ(theme.widgetRoles.at("media.status").size(), 1u);
	CustomTheme::resetRoles();
	ImGuiStyle style;
	CustomTheme::apply(theme, style);
	const ImVec4 fallback(0.1f, 0.2f, 0.3f, 1.f);
	EXPECT_NEAR(CustomTheme::roleColor("success", fallback).x, 0x35 / 255.f, 0.0001f);
	EXPECT_NEAR(CustomTheme::roleColor("success", fallback, "media.status").x, 0x12 / 255.f, 0.0001f);
	EXPECT_FLOAT_EQ(CustomTheme::roleColor("warning", fallback).w, 0.8f);
	CustomTheme::resetRoles();
	EXPECT_EQ(CustomTheme::roleColor("success", fallback).x, fallback.x);
}

TEST(CustomThemeTest, AccentDerivesSharedControlsAndSameLayerImGuiColorsWin)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"accent":"#48B4AD","Button":"#ABCDEF"}})", theme, error)) << error;
	CustomTheme::resetRoles();
	ImGuiStyle style;
	style.Colors[ImGuiCol_FrameBg] = ImVec4(0.1f, 0.2f, 0.3f, 1.f);
	const float hoverBase = style.Colors[ImGuiCol_FrameBgHovered].x;
	CustomTheme::apply(theme, style);
	EXPECT_NEAR(style.Colors[ImGuiCol_Button].x, 0xAB / 255.f, 0.0001f);
	EXPECT_NEAR(style.Colors[ImGuiCol_ButtonHovered].x,
	 hoverBase + (0x48 / 255.f - hoverBase) * 0.68f, 0.0001f);
	EXPECT_NEAR(style.Colors[ImGuiCol_CheckMark].x, 0x48 / 255.f, 0.0001f);
	EXPECT_NEAR(style.Colors[ImGuiCol_SliderGrab].x, 0x48 / 255.f, 0.0001f);
	CustomTheme::resetRoles();
}

TEST(CustomThemeTest, RejectsInvalidThemesWithoutChangingOutput)
{
	const char* invalid[] = {
		"not json", R"({})", R"({"base":"Unknown","colors":{}})",
		R"({"colors":{"Bogus":"#123456"}})", R"({"colors":{"Text":"#12345G"}})",
		R"({"colors":{"Text":[1,1,1]}})", R"({"style":{"FrameRounding":-1}})",
		R"({"style":{"ButtonTextAlign":[0,2]}})", R"({"style":{"Unknown":1}})",
		R"({"base":42,"style":{}})", R"({"colors":{"success":"#12ZZ34"}})",
		R"({"widgets":{"media.status":{"warning":"red"}}})", R"({"colors":{"Text":[-0.1,0,0,1]}})",
		R"({"colors":{"Text":[0,0,1.1,1]}})", R"({"colors":{"Text":[0,0,null,1]}})",
		R"({"colors":{"Text":[0,0,true,1]}})", R"({"colors":{"Text":[0,0,1e999,1]}})",
		R"({"colors":{"Text":[0,0,0,1,1]}})", R"({"colors":{"Text":[0,0,"0",1]}})",
		R"({"assets":{"icons":{"":"icon.png"}}})", R"({"assets":{"icons":{"bad key":"icon.png"}}})"
	};
	for (auto input : invalid) {
		CustomTheme::Theme theme;
		theme.base = 4;
		theme.colors.emplace_back(ImGuiCol_Text, ImVec4(0.1f, 0.2f, 0.3f, 0.4f));
		std::string error;
		EXPECT_FALSE(CustomTheme::parse(input, theme, error)) << input;
		EXPECT_EQ(theme.base, 4) << input;
		ASSERT_EQ(theme.colors.size(), 1u);
		EXPECT_EQ(theme.colors[0].first, ImGuiCol_Text);
		EXPECT_EQ(theme.colors[0].second.w, 0.4f);
		EXPECT_TRUE(theme.roles.empty());
		EXPECT_TRUE(theme.widgetRoles.empty());
		EXPECT_FALSE(error.empty()) << input;
	}
}

TEST(CustomThemeTest, RejectsUnsafeFilenames)
{
	CustomTheme::Theme theme;
	std::string error;
	for (const char* name : {"../theme.json", "subdir/theme.json", "subdir\\theme.json", "C:Escape.json", "bad?.json", "CON.json", "theme.txt", ""})
		EXPECT_FALSE(CustomTheme::load(name, theme, error)) << name;
}

TEST(CustomThemeTest, RejectsMalformedFontDirectoriesWithoutOverflow)
{
	std::vector<std::uint8_t> ttc(16, 0);
	ttc[0] = 't'; ttc[1] = 't'; ttc[2] = 'c'; ttc[3] = 'f';
	ttc[12] = 0xff; ttc[13] = 0xff; ttc[14] = 0xff; ttc[15] = 0xf8;
	EXPECT_FALSE(validThemeFontDirectory(ttc.data(), ttc.size()));
	ttc[12] = 0; ttc[13] = 0; ttc[14] = 0; ttc[15] = 12;
	EXPECT_FALSE(validThemeFontDirectory(ttc.data(), ttc.size()));
	std::vector<std::uint8_t> sfnt(28, 0);
	sfnt[1] = 1; sfnt[5] = 1; // TrueType signature and one table
	EXPECT_TRUE(validThemeFontDirectory(sfnt.data(), sfnt.size()));
	sfnt[5] = 2; // Second table does not fit in the directory
	EXPECT_FALSE(validThemeFontDirectory(sfnt.data(), sfnt.size()));
	sfnt[5] = 1; sfnt[20] = 0xff; sfnt[21] = 0xff; sfnt[22] = 0xff; sfnt[23] = 0xf8;
	EXPECT_FALSE(validThemeFontDirectory(sfnt.data(), sfnt.size()));
}

TEST(CustomThemeTest, DiscoversAndLoadsFilesByStableFilename)
{
	const auto root = std::filesystem::temp_directory_path() / ("hollycast-theme-test-" + std::to_string(std::random_device{}()));
	ASSERT_TRUE(std::filesystem::create_directory(root));
	struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
	const std::string path = root.string() + "/";
	{ std::ofstream file(path + "z.json"); file << R"({"colors":{"Text":"#123456"}})"; }
	{ std::ofstream file(path + "a.json"); file << R"({"style":{"FrameRounding":4}})"; }
	{ std::ofstream file(path + "ignored.txt"); file << "not a theme"; }
	EXPECT_EQ(CustomTheme::list(path), (std::vector<std::string>{"a.json", "z.json"}));
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(path, "z.json", theme, error)) << error;
	ASSERT_EQ(theme.colors.size(), 1u);
	EXPECT_EQ(theme.colors[0].first, ImGuiCol_Text);
	std::filesystem::remove(root / "a.json");
	EXPECT_EQ(CustomTheme::list(path), (std::vector<std::string>{"z.json"}));
	EXPECT_TRUE(CustomTheme::loadFromDirectory(path, "z.json", theme, error));
	EXPECT_FALSE(CustomTheme::loadFromDirectory(path, "missing.json", theme, error));
}

namespace {
class ThemeFilesTest : public ::testing::Test {
protected:
	void SetUp() override {
		root = std::filesystem::temp_directory_path() / ("hollycast-theme-test-" + std::to_string(std::random_device{}()));
		ASSERT_TRUE(std::filesystem::create_directory(root));
		path = root.string() + "/";
	}
	void TearDown() override {
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}
	void write(const std::string& file, const std::string& contents) {
		const auto target = std::filesystem::path(path + file);
		std::filesystem::create_directories(target.parent_path());
		// Preserve asset bytes and the exact bundled-preset comparison on Windows.
		std::ofstream output(target, std::ios::binary);
		output << contents;
		ASSERT_TRUE(output.good());
	}
	std::string read(const std::string& file) {
		std::ifstream input(path + file, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), {});
	}
	std::filesystem::path root;
	std::string path;
};

std::map<std::string, std::vector<float>> styleValues(const ImGuiStyle& style)
{
	std::map<std::string, std::vector<float>> values;
	for (int i = 0; i < ImGuiCol_COUNT; ++i) {
		const auto& color = style.Colors[i];
		values[ImGui::GetStyleColorName(i)] = {color.x, color.y, color.z, color.w};
	}
#define SCALAR(name) values["style." #name] = {style.name}
#define VECTOR(name) values["style." #name] = {style.name.x, style.name.y}
	SCALAR(Alpha);
	SCALAR(DisabledAlpha);
	SCALAR(WindowRounding);
	SCALAR(WindowBorderSize);
	SCALAR(ChildRounding);
	SCALAR(ChildBorderSize);
	SCALAR(PopupRounding);
	SCALAR(PopupBorderSize);
	SCALAR(FrameRounding);
	SCALAR(FrameBorderSize);
	SCALAR(IndentSpacing);
	SCALAR(ScrollbarSize);
	SCALAR(ScrollbarRounding);
	SCALAR(ScrollbarPadding);
	SCALAR(GrabMinSize);
	SCALAR(GrabRounding);
	SCALAR(TabRounding);
	SCALAR(TabBorderSize);
	SCALAR(TabMinWidthBase);
	SCALAR(TabMinWidthShrink);
	SCALAR(TabBarBorderSize);
	SCALAR(TabBarOverlineSize);
	SCALAR(SeparatorTextBorderSize);
	VECTOR(WindowPadding);
	VECTOR(WindowMinSize);
	VECTOR(WindowTitleAlign);
	VECTOR(FramePadding);
	VECTOR(ItemSpacing);
	VECTOR(ItemInnerSpacing);
	VECTOR(CellPadding);
	VECTOR(TouchExtraPadding);
	VECTOR(ButtonTextAlign);
	VECTOR(SelectableTextAlign);
	VECTOR(SeparatorTextAlign);
	VECTOR(SeparatorTextPadding);
#undef SCALAR
#undef VECTOR
	return values;
}
}

TEST(CustomThemeTest, PreservesNormalizedFloatColorPrecision)
{
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"Text":[0.179999992,0.349999994,0.580000043,0.861999989]}})", theme, error));
	ImGuiStyle style;
	CustomTheme::apply(theme, style);
	EXPECT_EQ(style.Colors[ImGuiCol_Text].x, 0.179999992f);
	EXPECT_EQ(style.Colors[ImGuiCol_Text].z, 0.580000043f);
	EXPECT_EQ(style.Colors[ImGuiCol_Text].w, 0.861999989f);
}

TEST(CustomThemeTest, KeepsLegacyIdsNamesAndAllCapturedStyleValues)
{
	const char* names[] = {"Dark", "Light", "Dreamcast", "High Contrast", "Nintendo", "Aqua Chill"};
	ASSERT_EQ(CustomTheme::CustomThemeId, 6);
	ASSERT_EQ(CustomTheme::builtins().size(), 6u);
	for (int id = 0; id < 6; ++id) {
		SCOPED_TRACE(names[id]);
		EXPECT_EQ(CustomTheme::builtins()[id].id, id);
		EXPECT_STREQ(CustomTheme::builtins()[id].name, names[id]);
		CustomTheme::Theme theme;
		std::string error;
		ASSERT_TRUE(CustomTheme::loadBundled(id, theme, error)) << error;
		ImGuiStyle style;
		CustomTheme::apply(theme, style);
		const auto actual = styleValues(style);
		std::ifstream baseline(std::string(FLYCAST_TEST_FILES) + "/ui-themes/" + std::to_string(id) + ".txt");
		ASSERT_TRUE(baseline.good());
		std::string line;
		size_t count = 0;
		while (std::getline(baseline, line)) {
			std::istringstream input(line);
			// Earlier suites set the process-wide locale to French; the checked-in
			// fixture always uses JSON/C-locale decimal points.
			input.imbue(std::locale::classic());
			std::string name;
			input >> name;
			auto value = actual.find(name);
			ASSERT_NE(value, actual.end()) << name;
			std::vector<float> expected;
			float component;
			while (input >> component) expected.push_back(component);
			EXPECT_EQ(value->second, expected) << name;
			++count;
		}
		EXPECT_EQ(count, actual.size());
		EXPECT_EQ(count, 95u);
	}
}

TEST_F(ThemeFilesTest, LegacyFilesAreLeftUntouchedByPresetInitialization)
{
	std::filesystem::create_directory(root / "builtin");
	const std::string edited = R"({"colors":{"Text":"#123456"}})";
	write("builtin/Dark.json", edited);
	ASSERT_TRUE(CustomTheme::seedBuiltins(path));
	EXPECT_EQ(read("builtin/Dark.json"), edited);
	write("builtin/Light.json", "broken user edit");
	ASSERT_TRUE(CustomTheme::seedBuiltins(path));
	EXPECT_EQ(read("builtin/Light.json"), "broken user edit");
	EXPECT_TRUE(CustomTheme::list(path).empty());
}

TEST_F(ThemeFilesTest, AssetPathsKeepTheDirectoryOfTheirDefiningTheme)
{
	std::filesystem::create_directories(root / "theme-a");
	const std::string themeDir = (root / "theme-a").string() + "/";
	std::ofstream file(themeDir + "custom.json");
	file << R"({"assets":{"background":"art/bg.png"},"fonts":{"body":"fonts/body.ttf"}})";
	file.close();
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(themeDir, "custom.json", theme, error)) << error;
	EXPECT_EQ(theme.assets.directory, themeDir);
	CustomTheme::setActiveAssets(theme.assets);
	EXPECT_EQ(CustomTheme::assetPath("background"), themeDir + "art/bg.png");
	EXPECT_EQ(CustomTheme::fontPath(false), themeDir + "fonts/body.ttf");
	const std::string nextDir = (root / "theme-b").string() + "/";
	std::filesystem::create_directories(root / "theme-b");
	std::ofstream nextFile(nextDir + "next.json");
	nextFile << R"({"assets":{"background":"new.png"}})";
	nextFile.close();
	CustomTheme::Theme nextTheme;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(nextDir, "next.json", nextTheme, error)) << error;
	CustomTheme::setActiveAssets(nextTheme.assets);
	EXPECT_EQ(CustomTheme::assetPath("background"), nextDir + "new.png");
	EXPECT_TRUE(CustomTheme::fontPath(false).empty());
	CustomTheme::setActiveAssets({});
	EXPECT_TRUE(CustomTheme::assetPath("background").empty());
}

TEST_F(ThemeFilesTest, BuiltinPresetsIgnoreLegacyEditableFiles)
{
	for (const auto& entry : CustomTheme::builtins()) {
		SCOPED_TRACE(entry.name);
		CustomTheme::Theme original;
		std::string error;
		ASSERT_TRUE(CustomTheme::loadBundled(entry.id, original, error));
		ImGuiStyle expected;
		CustomTheme::apply(original, expected);
		const std::string file = std::string("builtin/") + entry.filename;
		write(file, R"({"style":{"FrameRounding":19},"colors":{"Text":[0.1,0.2,0.3,0.4]}})");
		CustomTheme::Theme theme;
		ASSERT_TRUE(CustomTheme::loadBuiltinFromDirectory(path, entry.id, theme, error));
		EXPECT_TRUE(error.empty());
		ImGuiStyle actual;
		CustomTheme::apply(theme, actual);
		EXPECT_EQ(styleValues(actual), styleValues(expected));
		EXPECT_EQ(read(file), R"({"style":{"FrameRounding":19},"colors":{"Text":[0.1,0.2,0.3,0.4]}})");
		std::filesystem::remove(root / file);
	}
}

TEST_F(ThemeFilesTest, MigratesSelectedLegacyBuiltinOnceAndKeepsSource)
{
	const std::string edited = R"({"colors":{"Text":"#123456"},"style":{"FrameRounding":19}})";
	write("builtin/Dark.json", edited);
	write("Dark (Customized).json", "existing user custom theme");
	std::string filename, error;
	ASSERT_TRUE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error)) << error;
	EXPECT_EQ(filename, "Dark (Customized) 2.json");
	EXPECT_EQ(read("builtin/Dark.json"), edited);
	CustomTheme::Theme theme;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(path, filename, theme, error)) << error;
	EXPECT_EQ(theme.base, 0);
	ImGuiStyle style;
	CustomTheme::apply(theme, style);
	EXPECT_EQ(style.FrameRounding, 19.f);
	EXPECT_NEAR(style.Colors[ImGuiCol_Text].x, 0x12 / 255.f, 0.0001f);
	std::string ignored;
	EXPECT_FALSE(CustomTheme::migrateLegacyBuiltin(path, 0, ignored, error));
	EXPECT_TRUE(error.empty());
	EXPECT_EQ(ignored, "");
}

TEST_F(ThemeFilesTest, MigrationSkipsPreexistingAssetDirectory)
{
	write("builtin/Dark.json", R"({"style":{"FrameRounding":19}})");
	const auto existingAssets = root / "assets" / "Dark (Customized)";
	std::filesystem::create_directories(existingAssets);
	{ std::ofstream existing(existingAssets / "keep.txt"); existing << "unrelated user data"; }
	std::string filename, error;
	ASSERT_TRUE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error)) << error;
	EXPECT_EQ(filename, "Dark (Customized) 2.json");
	std::ifstream existing(existingAssets / "keep.txt");
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(existing), {}), "unrelated user data");
}

TEST_F(ThemeFilesTest, DoesNotMigrateUneditedBundledPreset)
{
	const auto& entry = CustomTheme::builtins()[0];
	size_t size = 0;
	auto data = resource::load(entry.resource, size);
	ASSERT_NE(data, nullptr);
	write(std::string("builtin/") + entry.filename, std::string(reinterpret_cast<const char*>(data.get()), size));
	std::string error;
	std::string filename;
	EXPECT_FALSE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error));
	EXPECT_TRUE(error.empty());
}

TEST_F(ThemeFilesTest, FailedMigrationLeavesLegacyFileAndCanBeRetried)
{
	write("builtin/Dark.json", "invalid legacy JSON");
	std::string filename, error;
	EXPECT_FALSE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error));
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(read("builtin/Dark.json"), "invalid legacy JSON");
	EXPECT_FALSE(std::filesystem::exists(root / ".builtin-migrated-0"));
	write("builtin/Dark.json", R"({"style":{"FrameRounding":12}})");
	ASSERT_TRUE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error)) << error;
	EXPECT_TRUE(std::filesystem::exists(root / ".builtin-migrated-0"));
}

TEST_F(ThemeFilesTest, DeferredMigrationCanBeJournaledAndCompleted)
{
	write("builtin/Dark.json", R"({"style":{"FrameRounding":12}})");
	std::string filename, error;
	ASSERT_TRUE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error, false)) << error;
	EXPECT_FALSE(std::filesystem::exists(root / ".builtin-migrated-0"));
	const std::string firstFilename = filename;
	ASSERT_TRUE(CustomTheme::migrateLegacyBuiltin(path, 0, filename, error, false)) << error;
	EXPECT_EQ(filename, firstFilename);
	EXPECT_EQ(CustomTheme::list(path), (std::vector<std::string>{firstFilename}));
	ASSERT_TRUE(CustomTheme::writePendingBuiltinMigration(path, 0, filename, error)) << error;
	int pendingId = -1;
	std::string pendingFilename;
	ASSERT_TRUE(CustomTheme::readPendingBuiltinMigration(path, pendingId, pendingFilename, error)) << error;
	EXPECT_EQ(pendingId, 0);
	EXPECT_EQ(pendingFilename, filename);
	const auto markerPath = root / ".builtin-migrated-0";
	std::filesystem::create_directory(markerPath);
	EXPECT_FALSE(CustomTheme::markLegacyBuiltinMigrationComplete(path, 0, filename, error));
	EXPECT_FALSE(error.empty());
	error.clear();
	EXPECT_FALSE(CustomTheme::clearPendingBuiltinMigration(path, error));
	EXPECT_FALSE(error.empty());
	EXPECT_TRUE(std::filesystem::exists(root / ".builtin-migration-pending"));
	std::filesystem::remove(markerPath);
	ASSERT_TRUE(CustomTheme::markLegacyBuiltinMigrationComplete(path, 0, filename, error)) << error;
	EXPECT_TRUE(std::filesystem::is_regular_file(root / ".builtin-migrated-0"));
	ASSERT_TRUE(CustomTheme::clearPendingBuiltinMigration(path, error)) << error;
	EXPECT_FALSE(std::filesystem::exists(root / ".builtin-migration-pending"));
}

TEST_F(ThemeFilesTest, MatchingPendingJournalRepairsPartialRegularMarker)
{
	const std::string filename = "Dark (Customized).json";
	std::string error;
	ASSERT_TRUE(CustomTheme::writePendingBuiltinMigration(path, 0, filename, error)) << error;
	write(".builtin-migrated-0", "Dark (Custom");

	ASSERT_TRUE(CustomTheme::markLegacyBuiltinMigrationComplete(path, 0, filename, error)) << error;
	EXPECT_EQ(read(".builtin-migrated-0"), filename);
	ASSERT_TRUE(CustomTheme::clearPendingBuiltinMigration(path, error)) << error;
	EXPECT_FALSE(std::filesystem::exists(root / ".builtin-migration-pending"));
}

TEST_F(ThemeFilesTest, MismatchedMarkerWithoutMatchingPendingJournalIsPreserved)
{
	const std::string markerContents = "unrelated marker data";
	write(".builtin-migrated-0", markerContents);
	std::string error;
	EXPECT_FALSE(CustomTheme::markLegacyBuiltinMigrationComplete(
			path, 0, "Dark (Customized).json", error));
	EXPECT_NE(error.find("does not match"), std::string::npos);
	EXPECT_EQ(read(".builtin-migrated-0"), markerContents);
}

TEST_F(ThemeFilesTest, KeepsBundledCustomBasesIndependentOfBuiltinEdits)
{
	ASSERT_TRUE(CustomTheme::seedBuiltins(path));
	for (const auto& entry : CustomTheme::builtins()) {
		write(std::string("builtin/") + entry.filename, R"({"colors":{"Text":"#000000"},"style":{"FrameRounding":19}})");
		CustomTheme::Theme custom, base;
		std::string error;
		ASSERT_TRUE(CustomTheme::parse(std::string("{\"base\":\"") + entry.name + "\",\"style\":{\"FrameBorderSize\":2}}", custom, error));
		EXPECT_EQ(custom.base, entry.id);
		ASSERT_TRUE(CustomTheme::loadBundled(custom.base, base, error));
		ImGuiStyle style;
		CustomTheme::apply(base, style);
		CustomTheme::apply(custom, style);
		EXPECT_EQ(style.FrameRounding, 3.f);
		ImGuiStyle expected;
		CustomTheme::apply(base, expected);
		EXPECT_EQ(style.Colors[ImGuiCol_Text].x, expected.Colors[ImGuiCol_Text].x);
		EXPECT_EQ(style.FrameBorderSize, 2.f);
	}
}

TEST_F(ThemeFilesTest, UsesDefaultsWhenStorageIsUnusable)
{
	write("blocked", "keep");
	const std::string blocked = path + "blocked/";
	EXPECT_TRUE(CustomTheme::seedBuiltins(blocked));
	EXPECT_EQ(read("blocked"), "keep");
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::loadBuiltinFromDirectory(blocked, 1, theme, error));
	EXPECT_TRUE(error.empty());
	ImGuiStyle style;
	CustomTheme::apply(theme, style);
	EXPECT_EQ(style.Colors[ImGuiCol_Text].x, 0.f);
	std::filesystem::create_directory(root / "builtin");
	std::filesystem::create_directory(root / "builtin/Dark.json");
	ASSERT_TRUE(CustomTheme::loadBuiltinFromDirectory(path, 0, theme, error));
	EXPECT_TRUE(error.empty());
}

TEST_F(ThemeFilesTest, RuntimeReloadResetsStyleBeforeScalingAndPreservesCustomInheritance)
{
	struct Restore {
		std::string configPath = get_writable_config_path("");
		int theme = config::UITheme.get();
		std::string custom = config::UICustomTheme.get();
		float scale = settings.display.uiScale;
		ImGuiContext* context = ImGui::GetCurrentContext();
		~Restore() {
			ImGui::DestroyContext();
			ImGui::SetCurrentContext(context);
			set_user_config_dir(configPath.empty() ? "." : configPath);
			config::open();
			config::UITheme = theme;
			config::UICustomTheme = custom;
			settings.display.uiScale = scale;
		}
	} restore;
	ImGui::CreateContext();
	set_user_config_dir(path);
	ASSERT_TRUE(config::open());
	settings.display.uiScale = 2.f;
	config::UITheme = 0;
	config::UICustomTheme = "";
	write("themes/builtin/Dark.json", R"({"colors":{"success":"#35C875"}})");
	write("themes/builtin/Light.json", R"({"style":{"FrameRounding":9}})");
	write("themes/.builtin-migration-pending", "malformed migration journal");
	const std::string configBeforeMigration = read("emu.cfg");
	applyCurrentTheme();
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 6.f);
	EXPECT_EQ(config::UITheme.get(), 0);
	EXPECT_TRUE(config::UICustomTheme.get().empty());
	EXPECT_TRUE(std::filesystem::exists(root / "themes/Dark (Customized).json"));
	EXPECT_TRUE(std::filesystem::exists(root / "themes/Light (Customized).json"));
	EXPECT_TRUE(std::filesystem::exists(root / "themes/.builtin-migrated-1"));
	EXPECT_FALSE(std::filesystem::exists(root / "themes/.builtin-migrated-0"));
	EXPECT_EQ(read("themes/.builtin-migration-pending"), "malformed migration journal");
	EXPECT_EQ(read("emu.cfg"), configBeforeMigration);
	// Remove the deliberately corrupted fixture so the remainder can exercise
	// later preset selection and preview behavior independently.
	std::filesystem::remove(root / "themes/.builtin-migration-pending");

	// Selecting a preset later always restores the immutable bundled appearance,
	// even though the preserved legacy edit is still present on disk.
	int pendingId = -1;
	std::string pendingFilename, migrationError;
	ASSERT_TRUE(CustomTheme::writePendingBuiltinMigration(CustomTheme::directory(), 0,
			"Dark (Customized).json", migrationError)) << migrationError;
	ASSERT_TRUE(CustomTheme::readPendingBuiltinMigration(CustomTheme::directory(), pendingId, pendingFilename, migrationError)) << migrationError;
	EXPECT_EQ(pendingId, 0);
	EXPECT_EQ(pendingFilename, "Dark (Customized).json");
	ASSERT_TRUE(CustomTheme::markLegacyBuiltinMigrationComplete(CustomTheme::directory(), pendingId, pendingFilename, migrationError)) << migrationError;
	ASSERT_TRUE(CustomTheme::clearPendingBuiltinMigration(CustomTheme::directory(), migrationError)) << migrationError;
	config::UITheme = 1;
	applyCurrentTheme();
	EXPECT_EQ(config::UITheme.get(), 1);
	config::UITheme = 0;
	applyCurrentTheme();
	EXPECT_EQ(config::UITheme.get(), 0);
	const ImVec4 fallbackRole(0.12f, 0.23f, 0.34f, 1.f);
	EXPECT_FLOAT_EQ(CustomTheme::roleColor("success", fallbackRole).x, fallbackRole.x);
	CustomTheme::Theme preview;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Dark","style":{"FrameRounding":11}})", preview, error));
	setThemePreview(&preview);
	applyCurrentTheme();
	EXPECT_EQ(config::UITheme.get(), 0);
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 22.f);
	EXPECT_FALSE(std::filesystem::exists(root / "themes/.builtin-migration-pending"));
	setThemePreview(nullptr);
	applyCurrentTheme();
	EXPECT_FLOAT_EQ(CustomTheme::roleColor("success", fallbackRole).x, fallbackRole.x);
	EXPECT_EQ(config::UITheme.get(), 0);
	EXPECT_FALSE(std::filesystem::exists(root / "themes/.builtin-migration-pending"));
	ImGui::GetStyle().TouchExtraPadding = ImVec2(20, 20);
	applyCurrentTheme();
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 6.f);
#if !defined(__ANDROID__) && !defined(TARGET_IPHONE) && !defined(__SWITCH__)
	EXPECT_EQ(ImGui::GetStyle().TouchExtraPadding.x, 0.f);
#endif
	write("themes/builtin/Dark.json", R"({"style":{"FrameRounding":9}})");
	applyCurrentTheme();
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 6.f);
	write("themes/custom.json", R"({"base":"Dark","style":{"FrameBorderSize":2}})");
	config::UITheme = CustomTheme::CustomThemeId;
	config::UICustomTheme = "custom.json";
	applyCurrentTheme();
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 6.f);
	EXPECT_EQ(ImGui::GetStyle().FrameBorderSize, 2.f);
	EXPECT_EQ(ImGui::GetStyle().ItemSpacing.y, 16.f);
	EXPECT_FLOAT_EQ(CustomTheme::roleColor("success", fallbackRole).x, fallbackRole.x);
	config::UICustomTheme = "missing.json";
	applyCurrentTheme();
	EXPECT_EQ(ImGui::GetStyle().FrameRounding, 6.f);
	EXPECT_EQ(config::UITheme.get(), CustomTheme::CustomThemeId);
	EXPECT_EQ(config::UICustomTheme.get(), "missing.json");
}

TEST_F(ThemeFilesTest, SerializesEverySupportedFieldAndRoundTripsWithoutLosingOverrides)
{
	CustomTheme::Theme original;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Aqua Chill","colors":{"Text":"#E0F0FFFF","accent":"#48B4AD","warning":[0.8,0.6,0.2,0.9]},"widgets":{"media.status":{"success":"#123456"},"settings.feedback":{"error":"#AB1234"}},"style":{"FrameRounding":8,"ItemSpacing":[9,14],"WindowTitleAlign":[0.5,0.5]},"layout":{"density":1.1,"settingsSidebarWidth":230,"libraryCoverSpacing":4,"artworkCornerRadius":12},"fonts":{"body":"fonts/body.ttf","bold":"fonts/bold.otf"},"assets":{"background":"images/bg.png","backgroundOpacity":0.35,"backgroundFit":"contain","icons":{"settings":"images/settings.png","other":"images/other.png"}},"vendorExtension":{"futureField":{"kept":true}}})", original, error)) << error;
	const std::string encoded = CustomTheme::serialize(original, error);
	ASSERT_TRUE(error.empty()) << error;
	CustomTheme::Theme decoded;
	ASSERT_TRUE(CustomTheme::parse(encoded, decoded, error)) << error;
	EXPECT_EQ(decoded.base, original.base);
	EXPECT_EQ(decoded.colors.size(), original.colors.size());
	EXPECT_EQ(decoded.roles.size(), original.roles.size());
	EXPECT_EQ(decoded.widgetRoles, original.widgetRoles);
	EXPECT_EQ(decoded.scalars.size(), original.scalars.size());
	EXPECT_EQ(decoded.vectors.size(), original.vectors.size());
	EXPECT_EQ(decoded.assets.bodyFont, original.assets.bodyFont);
	EXPECT_EQ(decoded.assets.boldFont, original.assets.boldFont);
	EXPECT_EQ(decoded.assets.background, original.assets.background);
	EXPECT_EQ(decoded.assets.icons, original.assets.icons);
	EXPECT_NE(encoded.find("vendorExtension"), std::string::npos);
	EXPECT_EQ(decoded.assets.backgroundFit, original.assets.backgroundFit);
	EXPECT_FLOAT_EQ(decoded.assets.backgroundOpacity, original.assets.backgroundOpacity);
	EXPECT_TRUE(decoded.layout.hasDensity);
	EXPECT_TRUE(decoded.layout.hasSettingsSidebarWidth);
	EXPECT_TRUE(decoded.layout.hasLibraryCoverSpacing);
	EXPECT_TRUE(decoded.layout.hasArtworkCornerRadius);
	ImGuiStyle a, b;
	CustomTheme::applyStyle(original, a);
	CustomTheme::applyStyle(decoded, b);
	EXPECT_EQ(styleValues(a), styleValues(b));
}

TEST_F(ThemeFilesTest, ExportAndImportCopyReferencedAssetsAndRewriteRelativePaths)
{
	std::filesystem::create_directories(root / "source/fonts");
	std::filesystem::create_directories(root / "source/images");
	{ std::ofstream file(root / "source/fonts/body.ttf", std::ios::binary); file << "font"; }
	{ std::ofstream file(root / "source/images/bg.png", std::ios::binary); file << "image"; }
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Light","fonts":{"body":"fonts/body.ttf","bold":"fonts/body.ttf"},"assets":{"background":"images/bg.png","backgroundOpacity":0.4,"icons":{"settings":"images/bg.png"}}})", theme, error)) << error;
	theme.assets.directory = (root / "source").string();
	const auto exportDir = root / "export";
	ASSERT_TRUE(CustomTheme::exportToDirectory(theme, "Shared.json", exportDir.string(), error)) << error;
	EXPECT_TRUE(std::filesystem::exists(exportDir / "Shared.json"));
	EXPECT_TRUE(std::filesystem::exists(exportDir / "fonts/body.ttf"));
	EXPECT_TRUE(std::filesystem::exists(exportDir / "images/bg.png"));
	EXPECT_EQ(std::distance(std::filesystem::recursive_directory_iterator(exportDir), std::filesystem::recursive_directory_iterator()), 5);
	EXPECT_FALSE(CustomTheme::exportToDirectory(theme, "Shared.json", exportDir.string(), error));
	EXPECT_NE(error.find("overwrite"), std::string::npos);

	std::string imported;
	ASSERT_TRUE(CustomTheme::importFromFile((exportDir / "Shared.json").string(), (root / "imported").string(), imported, error)) << error;
	EXPECT_EQ(imported, "Shared.json");
	CustomTheme::Theme restored;
	ASSERT_TRUE(CustomTheme::loadFromDirectory((root / "imported").string() + "/", imported, restored, error)) << error;
	EXPECT_EQ(restored.assets.bodyFont, "assets/Shared/fonts/body.ttf");
	EXPECT_EQ(restored.assets.background, "assets/Shared/images/bg.png");
	EXPECT_TRUE(std::filesystem::exists(root / "imported/assets/Shared/fonts/body.ttf"));
	EXPECT_TRUE(std::filesystem::exists(root / "imported/assets/Shared/images/bg.png"));
	EXPECT_EQ(restored.assets.boldFont, "assets/Shared/fonts/body.ttf");
	EXPECT_EQ(restored.assets.icons.at("settings"), "assets/Shared/images/bg.png");
	ASSERT_TRUE(CustomTheme::importFromFile((exportDir / "Shared.json").string(), (root / "imported").string(), imported, error)) << error;
	EXPECT_EQ(imported, "Shared 2.json");
}

TEST_F(ThemeFilesTest, ExportSerializationFailureRollsBackOnlyNewlyCopiedAssets)
{
	std::filesystem::create_directories(root / "source/fonts");
	{ std::ofstream file(root / "source/fonts/body.ttf", std::ios::binary); file << "font"; }
	const auto exportDir = root / "export";
	std::filesystem::create_directories(exportDir / "fonts");
	{ std::ofstream file(exportDir / "fonts/keep.txt", std::ios::binary); file << "unrelated user data"; }

	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"fonts":{"body":"fonts/body.ttf"}})", theme, error)) << error;
	theme.assets.directory = (root / "source").string();
	theme.colors.emplace_back(ImGuiCol_COUNT, ImVec4(1, 1, 1, 1));

	EXPECT_FALSE(CustomTheme::exportToDirectory(theme, "Broken.json", exportDir.string(), error));
	EXPECT_NE(error.find("invalid color index"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(exportDir / "fonts/body.ttf"));
	EXPECT_TRUE(std::filesystem::exists(exportDir / "fonts/keep.txt"));
	std::ifstream preserved(exportDir / "fonts/keep.txt", std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(preserved), {}), "unrelated user data");
	EXPECT_FALSE(std::filesystem::exists(exportDir / "Broken.json"));
}

TEST_F(ThemeFilesTest, ImportAndExportReportMissingOrUnsafeAssetsWithoutWritingThemeJson)
{
	write("missing.json", R"({"fonts":{"body":"fonts/missing.ttf"}})");
	std::string imported, error;
	EXPECT_FALSE(CustomTheme::importFromFile(path + "missing.json", (root / "imported").string(), imported, error));
	EXPECT_NE(error.find("missing"), std::string::npos);
	EXPECT_TRUE(CustomTheme::list((root / "imported").string() + "/").empty());
	write("unsafe.json", R"({"assets":{"background":"../outside.png"}})");
	EXPECT_FALSE(CustomTheme::importFromFile(path + "unsafe.json", (root / "imported").string(), imported, error));
	EXPECT_NE(error.find("unsafe"), std::string::npos);
}

TEST_F(ThemeFilesTest, ExportRejectsAssetSymlinksThatEscapeThemeFolder)
{
	// Keep the outside asset within the fixture so TearDown also cleans it on a skip.
	write("outside.png", "outside");
	const auto source = root / "source";
	std::filesystem::create_directory(source);
	std::error_code ec;
	std::filesystem::create_symlink(root / "outside.png", source / "escape.png", ec);
#ifdef _WIN32
	if (ec == std::error_code(ERROR_PRIVILEGE_NOT_HELD, std::system_category()))
		GTEST_SKIP() << "Windows requires symbolic-link privilege or Developer Mode";
#endif
	ASSERT_FALSE(ec) << ec.message();
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"assets":{"background":"escape.png"}})", theme, error)) << error;
	theme.assets.directory = source.string() + "/";
	EXPECT_FALSE(CustomTheme::exportToDirectory(theme, "Escape.json", (root / "export").string(), error));
	EXPECT_NE(error.find("outside"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(root / "export/Escape.json"));
}

TEST_F(ThemeFilesTest, SaveRefusesAccidentalReplacementAndReplacesOnlyOnExplicitRequest)
{
	struct RestorePath { std::string path = get_writable_config_path(""); ~RestorePath() { set_user_config_dir(path.empty() ? "." : path); } } restore;
	set_user_config_dir(path);
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Dark","colors":{"Text":"#123456"}})", theme, error)) << error;
	ASSERT_TRUE(CustomTheme::save("saved.json", theme, false, error)) << error;
	const auto themePath = std::filesystem::path(CustomTheme::directory()) / "saved.json";
	std::ifstream beforeFile(themePath, std::ios::binary);
	const std::string before(std::istreambuf_iterator<char>(beforeFile), {});
	// Windows read handles must be closed before testing atomic replacement.
	beforeFile.close();
	EXPECT_FALSE(CustomTheme::save("saved.json", theme, false, error));
	EXPECT_NE(error.find("already exists"), std::string::npos);
	std::ifstream afterFile(themePath, std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(afterFile), {}), before);
	afterFile.close();
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Light","colors":{"Text":"#ABCDEF"}})", theme, error)) << error;
	ASSERT_TRUE(CustomTheme::save("saved.json", theme, true, error)) << error;
	CustomTheme::Theme loaded;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(CustomTheme::directory(), "saved.json", loaded, error)) << error;
	EXPECT_EQ(loaded.base, 1);
	EXPECT_NEAR(loaded.colors.front().second.x, 0xAB / 255.f, 0.0001f);
}

TEST_F(ThemeFilesTest, InvalidDraftCannotReplaceOrExportAnExistingTheme)
{
	struct RestorePath { std::string path = get_writable_config_path(""); ~RestorePath() { set_user_config_dir(path.empty() ? "." : path); } } restore;
	set_user_config_dir(path);
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"Text":"#123456"}})", theme, error)) << error;
	ASSERT_TRUE(CustomTheme::save("saved.json", theme, false, error)) << error;
	const auto saved = std::filesystem::path(CustomTheme::directory()) / "saved.json";
	std::ifstream input(saved, std::ios::binary);
	const std::string before(std::istreambuf_iterator<char>(input), {});
	input.close();
	const CustomTheme::Theme validTheme = theme;
	theme.scalars.emplace_back(&ImGuiStyle::Alpha, 0.f);
	EXPECT_FALSE(CustomTheme::save("saved.json", theme, true, error));
	EXPECT_NE(error.find("Alpha"), std::string::npos);
	std::ifstream unchanged(saved, std::ios::binary);
	EXPECT_EQ(std::string(std::istreambuf_iterator<char>(unchanged), {}), before);
	EXPECT_FALSE(CustomTheme::exportToDirectory(theme, "invalid.json", (root / "export").string(), error));
	EXPECT_FALSE(std::filesystem::exists(root / "export/invalid.json"));
	unchanged.close();

	write("icon.png", "icon data");
	for (const char* name : {"", "bad key"}) {
		CustomTheme::Theme invalid = validTheme;
		invalid.assets.directory = path;
		invalid.assets.icons[name] = "icon.png";
		error.clear();
		EXPECT_TRUE(CustomTheme::serialize(invalid, error).empty());
		EXPECT_NE(error.find("icon"), std::string::npos);
		error.clear();
		EXPECT_FALSE(CustomTheme::save("saved.json", invalid, true, error));
		EXPECT_NE(error.find("icon"), std::string::npos);
		{
			std::ifstream preserved(saved, std::ios::binary);
			EXPECT_EQ(std::string(std::istreambuf_iterator<char>(preserved), {}), before);
		}
		error.clear();
		EXPECT_FALSE(CustomTheme::exportToDirectory(invalid, "invalid-icons.json", (root / "export").string(), error));
		EXPECT_NE(error.find("icon"), std::string::npos);
		EXPECT_FALSE(std::filesystem::exists(root / "export/invalid-icons.json"));
	}
}

TEST_F(ThemeFilesTest, RejectsNonportableNamesBeforeCreatingFiles)
{
	struct RestorePath { std::string path = get_writable_config_path(""); ~RestorePath() { set_user_config_dir(path.empty() ? "." : path); } } restore;
	set_user_config_dir(path);
	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"Text":"#123456"}})", theme, error)) << error;
	for (const char* name : {"C:Escape.json", "bad?.json", "CON.json", "subdir/theme.json"}) {
		EXPECT_FALSE(CustomTheme::save(name, theme, false, error)) << name;
		EXPECT_FALSE(CustomTheme::exportToDirectory(theme, name, (root / "export").string(), error)) << name;
	}
	EXPECT_FALSE(std::filesystem::exists(root / "export"));
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(CustomTheme::directory()) / "C:Escape.json"));
}

TEST_F(ThemeFilesTest, WindowsAssetSeparatorsSurviveImportAndExport)
{
	write("source/fonts/body.ttf", "font");
	write("source/Theme.json", R"({"fonts":{"body":"fonts\\body.ttf"}})");
	std::string imported, error;
	ASSERT_TRUE(CustomTheme::importFromFile(path + "source/Theme.json", path + "imported", imported, error)) << error;
	CustomTheme::Theme theme;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(path + "imported/", imported, theme, error)) << error;
	EXPECT_EQ(theme.assets.bodyFont, "assets/Theme/fonts/body.ttf");
	ASSERT_TRUE(std::filesystem::exists(root / "imported/assets/Theme/fonts/body.ttf"));
	ASSERT_TRUE(CustomTheme::exportToDirectory(theme, "Portable.json", path + "export", error)) << error;
	EXPECT_TRUE(std::filesystem::exists(root / "export/assets/Theme/fonts/body.ttf"));
	CustomTheme::Theme exported;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(path + "export/", "Portable.json", exported, error)) << error;
	EXPECT_EQ(exported.assets.bodyFont, "assets/Theme/fonts/body.ttf");
}

TEST_F(ThemeFilesTest, Utf8PathsRoundTripThroughSaveExportAndImport)
{
	struct RestorePath { std::string path = get_writable_config_path(""); ~RestorePath() { set_user_config_dir(path.empty() ? "." : path); } } restore;
	const auto configDir = root / std::filesystem::u8path("用户 – Café");
	const std::string configPath = configDir.u8string();
	set_user_config_dir(configPath);

	CustomTheme::Theme theme;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"base":"Dark","colors":{"Text":"#123456"}})", theme, error)) << error;
	ASSERT_TRUE(CustomTheme::save("主题.json", theme, false, error)) << error;
	const auto savedTheme = configDir / std::filesystem::u8path("themes/主题.json");
	ASSERT_TRUE(std::filesystem::exists(savedTheme));
	CustomTheme::Theme loaded;
	ASSERT_TRUE(CustomTheme::loadFromDirectory((configDir / "themes").u8string() + "/", "主题.json", loaded, error)) << error;
	ASSERT_EQ(loaded.colors.size(), 1u);

	const auto exportDir = root / std::filesystem::u8path("出口 – résumé");
	ASSERT_TRUE(CustomTheme::exportToDirectory(loaded, "Thème partagé.json", exportDir.u8string(), error)) << error;
	const auto exportedTheme = exportDir / std::filesystem::u8path("Thème partagé.json");
	ASSERT_TRUE(std::filesystem::exists(exportedTheme));
	const auto importDir = root / std::filesystem::u8path("进口");
	std::string importedFilename;
	ASSERT_TRUE(CustomTheme::importFromFile(exportedTheme.u8string(), importDir.u8string(), importedFilename, error)) << error;
	EXPECT_EQ(importedFilename, "Thème partagé.json");
	EXPECT_TRUE(std::filesystem::exists(importDir / std::filesystem::u8path(importedFilename)));
	CustomTheme::Theme imported;
	ASSERT_TRUE(CustomTheme::loadFromDirectory(importDir.u8string() + "/", importedFilename, imported, error)) << error;
	ASSERT_EQ(imported.colors.size(), 1u);
	EXPECT_EQ(imported.base, loaded.base);
}

TEST_F(ThemeFilesTest, StyleOnlyPreviewDoesNotChangeActiveSemanticRoles)
{
	CustomTheme::Theme active, draft;
	std::string error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"success":"#123456"}})", active, error)) << error;
	ASSERT_TRUE(CustomTheme::parse(R"({"colors":{"success":"#ABCDEF"}})", draft, error)) << error;
	CustomTheme::resetRoles();
	ImGuiStyle style;
	CustomTheme::apply(active, style);
	CustomTheme::applyStyle(draft, style);
	const ImVec4 fallback;
	EXPECT_NEAR(CustomTheme::roleColor("success", fallback).x, 0x12 / 255.f, 0.0001f);
	CustomTheme::resetRoles();
}
