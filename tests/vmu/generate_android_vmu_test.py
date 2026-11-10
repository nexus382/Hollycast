# Portions Copyright 2026 The Hollycast Authors
"""Run the production Android VMU backend against texture/UI boundary fakes."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
backend = (root / 'core/rend/vmu_android.cpp').read_text(encoding='utf-8')
# Keep all backend logic intact; only replace its application/platform dependencies.
backend = '\n'.join(line for line in backend.splitlines() if not line.startswith('#include '))
source = r'''
#define HAVE_MOVABLE_VMUS
#include "rend/vmu_window.h"
#include "rend/vmu_frame_store.h"
#include "rend/vmu_layout.h"
#include "cfg/option.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>

TestSettings settings;
namespace config {
bool MovableVMUs = true, VmuLayoutEnabled = false, FloatVMUs = true;
TestIntOption VmuWindowSize, VmuWindowTransparency {25};
RenderType RendererType = RenderType::OpenGL;
std::map<std::string, int> values;
int loadInt(const std::string& section, const std::string& key, int fallback) {
 auto it = values.find(section + "/" + key);
 return it == values.end() ? fallback : it->second;
}
void saveInt(const std::string& section, const std::string& key, int value) { values[section + "/" + key] = value; }
void setAutoSave(bool) {}
}
using ImTextureID = u64;
struct ImVec2 { float x, y; ImVec2(float x = 0, float y = 0) : x(x), y(y) {} };
struct DrawList {
 int draws = 0; u32 color = 0;
 void AddImage(ImTextureID, ImVec2, ImVec2, ImVec2, ImVec2, u32 nextColor) { ++draws; color = nextColor; }
};
namespace ImGui {
struct IO { ImVec2 DisplaySize {1000, 600}; } io;
DrawList drawList;
IO& GetIO() { return io; }
DrawList* GetForegroundDrawList() { return &drawList; }
}
namespace GuiMenu { float mainMenuBarHeight() { return 0; } }
int insetTop = 0, insetLeft = 0, insetRight = 0, insetBottom = 0;
struct TextureDriver {
 int uploads = 0;
 std::map<std::string, ImTextureID> textures;
 ImTextureID getTexture(const std::string& key) { return textures[key]; }
 ImTextureID updateTexture(const std::string& key, const u8*, int, int, bool) { ++uploads; return textures[key] = 1; }
} driver;
TextureDriver* imguiDriver = &driver;
'''
source += backend
source += r'''
int checks = 0;
void require(bool condition, const char* message) {
 ++checks; if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
 using namespace vmu_window;
 std::array<u32, 48 * 32> pixels {};
 pixels.fill(0xffffffff);
 publishScreen(0, pixels.data(), true);
 update(true); drawInApp();
 require(ownsScreen(0) && ImGui::drawList.color == 0xc0bfbfbf, "default Android alpha and tint are unchanged");
 const int uploads = driver.uploads;
 config::VmuWindowTransparency = 50;
 update(true); drawInApp();
 require(ImGui::drawList.color == 0x80bfbfbf && driver.uploads == uploads, "live alpha changes without texture republication");
 config::VmuLayoutEnabled = true; config::MovableVMUs = false;
 config::VmuWindowTransparency = 0;
 update(true); drawInApp();
 require(ImGui::drawList.color == 0xffbfbfbf && !touchDown(.02f, .03f), "locked LCD can become solid without becoming draggable");
 config::MovableVMUs = true;
 update(true);
 require(touchDown(.02f, .03f), "visible LCD starts a drag");
 touchMove(.5f, .5f);
 config::VmuWindowTransparency = 100;
 const int draws = ImGui::drawList.draws;
 update(true); drawInApp();
 require(ownsScreen(0) && ImGui::drawList.draws == draws && !touchDown(.5f, .5f), "hidden LCD suppresses drawing, fallback and new touches");
 const int savedX = config::values["vmu-touch/0.x"];
 touchMove(.8f, .8f); update(true);
 require(savedX > 0 && config::values["vmu-touch/0.x"] == savedX, "hiding ends the current drag and saves its final position");
 update(false);
 require(ownsScreen(0) && !touchDown(.5f, .5f), "opening the menu cannot revive the hidden fallback or capture touch");
 update(true); drawInApp();
 require(ownsScreen(0) && ImGui::drawList.draws == draws, "closing the menu keeps the hidden LCD hidden");
 config::VmuWindowTransparency = 25;
 update(true); drawInApp();
 require(ownsScreen(0) && ImGui::drawList.color == 0xc0bfbfbf && driver.uploads == uploads, "restoring visibility reuses the saved LCD texture");
 config::VmuWindowTransparency = 100; update(false);
 publishScreen(0, nullptr, false); update(false);
 require(!ownsScreen(0), "disconnect clears hidden menu ownership");
 publishScreen(0, pixels.data(), true); update(false);
 config::FloatVMUs = false; update(false);
 require(!ownsScreen(0), "disabling VMUs clears hidden ownership");
 shutdown();
 std::cout << checks << " checks passed\n";
}
'''
Path(sys.argv[1]).write_text('// Portions Copyright 2026 The Hollycast Authors\n' + source, encoding='utf-8')
