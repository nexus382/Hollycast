# Portions Copyright 2026 The Hollycast Authors
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sdl = (root / 'core/sdl/sdl.cpp').read_text()
gui = (root / 'core/ui/gui.cpp').read_text()
native = (root / 'core/windows/vmu_window.cpp').read_text()

def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]

capture = between(sdl, 'static bool captureMouse(bool capture)', 'static void emuEventCallback(')
cursor = between(sdl, 'static bool keepVmuCursorVisible()', 'static void updateFullscreenCursorVisibility(')
transition = between(sdl, 'void input_sdl_handle()', '\tSDL_Event event;')
hotkey = between(sdl, '\t\t\t\t\t\t\tconst bool wasCaptured = mouseCaptured;', '\t\t\t\t\t\t\tbreak;')
active = between(native, 'bool detachedActive()', 'void publishScreen(')
reset = between(gui, 'void reset_vmus()', 'void gui_error(')

prefix = r'''
#include <cstdint>
#include <iterator>
#include <iostream>
#include <cstdlib>
#if defined(VMU_TEST_OTHER)
#undef _WIN32
#elif !defined(_WIN32)
#define _WIN32
#endif
using u32 = std::uint32_t;
using Uint64 = std::uint64_t;
Uint64 ticks = 0;
Uint64 SDL_GetTicks64() { return ticks; }
constexpr int SDL_FALSE=0, SDL_TRUE=1, SDL_DISABLE=0, SDL_ENABLE=1, SDL_QUERY=-1;
void* window = reinterpret_cast<void*>(1);
bool gameRunning=true, mouseCaptured=false, relativeMouse=false, captureSucceeds=true;
bool toggleRequested=false;
int cursorVisible=SDL_ENABLE, published=0, checks=0, captureAttempts=0;
bool vmu_lcd_status[8] {};
struct { struct { bool console=true; bool isConsole() const { return console; } } platform; } settings;
namespace config { bool UseRawInput=false, MovableVMUs=false, FloatVMUs=true; }
int SDL_SetRelativeMouseMode(int enabled) {
 if (enabled) ++captureAttempts;
 if (enabled && !captureSucceeds) return -1;
 relativeMouse=enabled; return 0;
}
int SDL_ShowCursor(int state) { if (state!=SDL_QUERY) cursorVisible=state; return cursorVisible; }
void SDL_SetWindowTitle(void*,const char*) {}
void setWindowTitleGame() {}
struct SDLGamepad { static void UpdateRumble() {} };
namespace vmu_window {
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
void update(bool) {}
void publishScreen(int,const u32*,bool) { ++published; }
'''
prefix += active + '#endif\n}\n' + capture + cursor + transition
prefix += ' if (toggleRequested) {\n' + hotkey + ' toggleRequested=false;\n }\n}\n' + reset

suffix = r'''
void require(bool value, const char* label) {
 if (!value) { std::cerr << "FAIL " << label << '\n'; std::exit(1); }
 ++checks; std::cout << "PASS " << label << '\n';
}
void prepare(bool captured) {
 config::MovableVMUs=false; config::FloatVMUs=true; settings.platform.console=true;
 gameRunning=true; captureSucceeds=true; config::UseRawInput=false;
 ticks += 1000;
 input_sdl_handle(); mouseCaptured=captured; relativeMouse=captured;
}
int main() {
 reset_vmus();
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
 require(published==8,"standalone Windows reset publishes all slots");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 require(!mouseCaptured && !relativeMouse && keepVmuCursorVisible(),"enable releases capture and exposes the free pointer");
 input_sdl_handle(); require(!mouseCaptured,"stable active VMUs leave capture unchanged");
 config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured && relativeMouse,"disable restores previously captured gameplay");
 prepare(false); config::MovableVMUs=true; input_sdl_handle();
 config::MovableVMUs=false; input_sdl_handle();
 require(!mouseCaptured,"disable preserves an originally free pointer");
 prepare(true); gameRunning=false; config::MovableVMUs=true; input_sdl_handle();
 require(!mouseCaptured,"enable while paused clears the resume capture preference");
 config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured,"disable while paused restores the resume preference");
 gameRunning=true; captureMouse(mouseCaptured); require(relativeMouse,"resume applies restored capture");
 prepare(true); config::MovableVMUs=true; input_sdl_handle(); gameRunning=false;
 toggleRequested=true; input_sdl_handle(); config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured,"ineffective paused CtrlAlt preserves restoration");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 toggleRequested=true; input_sdl_handle(); require(mouseCaptured,"explicit CtrlAlt captures while VMUs are active");
 toggleRequested=true; input_sdl_handle(); config::MovableVMUs=false; input_sdl_handle();
 require(!mouseCaptured,"explicit capture then release wins over automatic restoration");
 prepare(true); config::MovableVMUs=true; input_sdl_handle(); captureSucceeds=false;
 toggleRequested=true; input_sdl_handle(); require(!mouseCaptured,"failed SDL capture does not change capture state");
 captureSucceeds=true; config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured,"failed CtrlAlt preserves automatic restoration");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=false; config::MovableVMUs=false; input_sdl_handle();
 require(!mouseCaptured && !relativeMouse,"failed recapture when disabling leaves capture pending");
 int attempts=captureAttempts;
 for (int i=0; i<100; ++i) input_sdl_handle();
 require(captureAttempts==attempts,"failed automatic capture is throttled");
 ticks+=1000; input_sdl_handle();
 require(!mouseCaptured && captureAttempts==attempts+1,"automatic capture retries after its deadline");
 captureSucceeds=true; ticks+=1000; input_sdl_handle();
 require(mouseCaptured && relativeMouse,"later successful recapture restores the saved preference");
 attempts=captureAttempts; ticks+=1000; input_sdl_handle();
 require(captureAttempts==attempts,"successful recapture clears pending restoration");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 gameRunning=false; config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured,"paused disable retains the resume preference");
 gameRunning=true; captureSucceeds=false; captureMouse(mouseCaptured); input_sdl_handle();
 require(!mouseCaptured,"failed resume is not mistaken for a restored capture");
 captureSucceeds=true; ticks+=1000; input_sdl_handle();
 require(mouseCaptured && relativeMouse,"paused restoration survives a failed resume");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=false; config::MovableVMUs=false; input_sdl_handle();
 gameRunning=false; input_sdl_handle();
 gameRunning=true; captureMouse(mouseCaptured); input_sdl_handle();
 require(!mouseCaptured && !relativeMouse,"pause and failed resume clear stale capture state before the retry deadline");
 captureSucceeds=true; ticks+=1000; input_sdl_handle();
 require(mouseCaptured,"capture still recovers after failure then pause and resume");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=false; config::MovableVMUs=false; input_sdl_handle();
 config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=true; config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured,"re-enabling VMUs preserves an outstanding restore preference");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=false; config::MovableVMUs=false; input_sdl_handle();
 config::MovableVMUs=true; input_sdl_handle(); gameRunning=false;
 config::MovableVMUs=false; input_sdl_handle();
 gameRunning=true; captureMouse(mouseCaptured); input_sdl_handle();
 require(!mouseCaptured && !relativeMouse,"re-enable then paused disable does not retain a stale retry deadline on failed resume");
 captureSucceeds=true; ticks+=1000; input_sdl_handle();
 require(mouseCaptured,"re-enable and pause cycle retains restoration until success");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 captureSucceeds=false; config::MovableVMUs=false; input_sdl_handle();
 captureSucceeds=true; toggleRequested=true; input_sdl_handle();
 toggleRequested=true; input_sdl_handle(); ticks+=1000; input_sdl_handle();
 require(!mouseCaptured,"effective manual capture and release cancel a pending retry");
 prepare(true); config::MovableVMUs=true; input_sdl_handle(); settings.platform.console=false; input_sdl_handle();
 require(mouseCaptured && !keepVmuCursorVisible(),"arcade transition restores normal capture behavior");
 prepare(true); config::MovableVMUs=true; input_sdl_handle(); config::FloatVMUs=false; input_sdl_handle();
 require(mouseCaptured,"hiding VMUs restores prior capture");
 prepare(true); config::UseRawInput=true; config::MovableVMUs=true; input_sdl_handle();
 require(!mouseCaptured && cursorVisible==SDL_ENABLE,"raw-input enable releases capture");
 config::MovableVMUs=false; input_sdl_handle();
 require(mouseCaptured && cursorVisible==SDL_DISABLE,"raw-input disable restores capture and cursor state");
#else
 require(published==0,"unsupported native VMU reset is excluded");
 prepare(true); config::MovableVMUs=true; input_sdl_handle();
 require(mouseCaptured && !keepVmuCursorVisible(),"unsupported platforms retain normal capture behavior");
#endif
 std::cout << "Capture/guard checks complete: " << checks << '\n';
}
'''
# Compile the actual production functions against deterministic SDL stubs. Missing
# source boundaries intentionally fail generation rather than testing stale copies.
Path(sys.argv[1]).write_text(
    '// Portions Copyright 2026 The Hollycast Authors\n' + prefix + suffix,
    encoding='utf-8',
)
