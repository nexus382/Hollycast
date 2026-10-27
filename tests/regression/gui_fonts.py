#!/usr/bin/env python3
# Portions Copyright 2026 The Hollycast Authors
"""Check production font loading using objects from a macOS Debug app build.

Usage: python3 tests/regression/gui_fonts.py <xcode-build-dir> <output-dir>
The app must have been rebuilt after any font-loading changes.
"""

import ast
from pathlib import Path
import re
import subprocess
import sys
import unicodedata
import zipfile


ROOT = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
fonts = out / "fonts"
fonts.mkdir(exist_ok=True)
for archive in (ROOT / "fonts").glob("*.tt?.zip"):
    with zipfile.ZipFile(archive) as zipped:
        name = archive.name.removesuffix(".zip")
        (fonts / name).write_bytes(zipped.read(name))

# Exercise every language offered by the settings UI, including native names
# displayed in the picker even when the active UI language is English.
settings = (ROOT / "core/ui/settings_new.cpp").read_text()
options = settings.split("const UILanguageOption uiLanguageOptions[] = {", 1)[1].split("};", 1)[0]
locales = re.findall(r'\{ Tnop\("([^"]+)"\), "([^"]+)" \}', options)
native_names = "".join(name for name, _ in locales)
for _, locale in locales:
    po = ROOT / "resources/i18n" / (locale + ".po")
    translations = [native_names]
    in_translation = False
    if po.exists():
        for line in po.read_text().splitlines():
            if line.startswith("msgstr"):
                in_translation = True
                translations.append(ast.literal_eval(line.split(" ", 1)[1]))
            elif line.startswith('"') and in_translation:
                translations.append(ast.literal_eval(line))
            else:
                in_translation = False
    chars = sorted({ord(c) for text in translations for c in text
                    if not unicodedata.category(c).startswith("C")})
    (out / (locale + ".chars")).write_text("\n".join(map(str, chars)))

source = out / "gui_fonts.cpp"
source.write_text(r'''
#include "types.h"
#include "oslib/resources.h"
#include "ui/gui_font.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

settings_t settings;
static std::string resourceDir, currentLocale;
namespace i18n { std::string getCurrentLocale() { return currentLocale; } }
void fatal_error(const char*, ...) { std::abort(); }
[[noreturn]] void os_DebugBreak() { std::abort(); }
namespace resource {
std::unique_ptr<u8[]> load(const std::string& path, size_t& size) {
    std::ifstream file(resourceDir + "/" + path, std::ios::binary | std::ios::ate);
    if (!file) { size = 0; return nullptr; }
    size = static_cast<size_t>(file.tellg());
    auto data = std::make_unique<u8[]>(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.get()), size);
    return data;
}
}

int main(int argc, char** argv) {
    resourceDir = argv[1];
    int failures = 0;
    ImGui::CreateContext();
    for (int arg = 2; arg < argc; ++arg) {
        currentLocale = argv[arg];
        auto& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        gui_loadFonts();
        io.Fonts->Build();
        size_t cjkSize = 0;
        auto cjk = resource::load("fonts/NotoSansCJK-Regular.ttc", cjkSize);
        if (!cjk) return 2;
        ImFont* textFonts[] = {io.Fonts->Fonts[0], boldFont, largeFont,
                              settingsTitleFont, settingsRightValueFont, settingsValueFont};
        const char* names[] = {"regular", "bold", "large", "title", "right-value", "popup-value"};
        std::string charsLocale = currentLocale;
        if (currentLocale == "zh-CN" || currentLocale == "zh-Hans-CN" || currentLocale == "zh_CN.UTF-8") charsLocale = "zh_CN";
        if (currentLocale == "zh-Hant-TW" || currentLocale == "zh-Hant-HK" || currentLocale == "zh_TW.UTF-8" || currentLocale == "zh_HK.UTF-8") charsLocale = "zh_TW";
        std::ifstream chars(resourceDir + "/" + charsLocale + ".chars");
        std::vector<unsigned> codepoints{std::istream_iterator<unsigned>(chars), {}};
        if (codepoints.empty()) return 2;
        for (int i = 0; i < 6; ++i) {
            int missing = 0;
            ImFontConfig* primary = textFonts[i]->Sources[0];
            ImFontConfig* bundled = nullptr;
            for (ImFontConfig* src : textFonts[i]->Sources)
                if (src->FontDataSize == static_cast<int>(cjkSize) &&
                    !memcmp(src->FontData, cjk.get(), cjkSize)) bundled = src;
            if (!bundled) { std::cerr << "Missing bundled source for " << names[i] << "\n"; return 2; }
            const int expectedFace = currentLocale == "zh_CN" || currentLocale == "zh-CN" || currentLocale == "zh-Hans-CN" || currentLocale == "zh_CN.UTF-8" ? 2 :
                currentLocale == "zh_TW" || currentLocale == "zh-Hant-TW" || currentLocale == "zh_TW.UTF-8" ? 3 : currentLocale == "zh-Hant-HK" || currentLocale == "zh_HK.UTF-8" ? 4 : 0;
            if (bundled->FontNo != expectedFace) { std::cerr << "Wrong regional face\n"; return 2; }
            for (unsigned c : codepoints) {
                // Restrict coverage to the built-in primary and bundled sources,
                // even on a machine that has other CJK fonts installed.
                auto contains = [&](ImFontConfig* src) {
                    auto loader = src->FontLoader ? src->FontLoader : io.Fonts->FontLoader;
                    return loader->FontSrcContainsGlyph(io.Fonts, src, static_cast<ImWchar>(c));
                };
                if ((!contains(primary) && !contains(bundled)) ||
                    !textFonts[i]->GetFontBaked(textFonts[i]->LegacySize)->FindGlyphNoFallback(c)) {
                    if (missing++ < 5) std::cout << currentLocale << " " << names[i]
                        << " missing U+" << std::hex << c << std::dec << "\n";
                }
            }
            std::cout << currentLocale << " " << names[i] << ": " << missing
                      << " missing / " << codepoints.size() << " characters\n";
            failures += missing;
        }
    }
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
''')
objects = build / "build/Hollycast.build/Debug/Objects-normal/arm64"
units = ["gui_font", "imgui", "imgui_draw", "imgui_tables", "imgui_widgets", "imgui_cjk", "imgui_freetype"]
subprocess.run([
    "clang++", "-std=c++17", "-DNOWIDE_NO_LIB", "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG",
    "-I" + str(ROOT / "core"), "-I" + str(ROOT / "core/deps"),
    "-I" + str(ROOT / "core/deps/nowide/include"), str(source),
    *[str(objects / (unit + ".o")) for unit in units],
    str(build / "core/deps/freetype/Debug/libfreetyped.a"),
    "-L/opt/homebrew/lib", "-lpng", "-lz", "-o", str(out / "gui_fonts"),
], check=True)
subprocess.run([str(out / "gui_fonts"), str(out), *[locale for _, locale in locales], "zh-CN", "zh-Hans-CN", "zh-Hant-TW", "zh-Hant-HK", "zh_CN.UTF-8", "zh_TW.UTF-8", "zh_HK.UTF-8"], check=True)
