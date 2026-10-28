# Portions Copyright 2026 The Hollycast Authors
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sdl = (root / 'core/sdl/sdl.cpp').read_text(encoding='utf-8')
gui = (root / 'core/ui/gui.cpp').read_text(encoding='utf-8')
native = (root / 'core/windows/vmu_window.cpp').read_text(encoding='utf-8')
mouse = (root / 'core/input/mouse.cpp').read_text(encoding='utf-8')
raw = (root / 'core/windows/rawinput.cpp').read_text(encoding='utf-8')

def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]

capture = between(sdl, 'static bool captureMouse(bool capture)', 'static void emuEventCallback(')
cursor = between(sdl, 'static bool keepVmuCursorVisible()', 'static void updateFullscreenCursorVisibility(')
transition = between(sdl, 'void input_sdl_handle()', '\tSDL_Event event;')
hotkey = between(sdl, '\t\t\t\t\t\t\tconst bool wasCaptured = mouseCaptured;', '\t\t\t\t\t\t\tbreak;')
active = between(native, 'bool detachedActive()', 'void publishScreen(')
reset = between(gui, 'void reset_vmus()', 'void gui_error(')
events = between(sdl[sdl.index('void input_sdl_handle()'):], '\tSDL_Event event;', '\t\tswitch (event.type)')
motion = between(sdl, '\t\t\tcase SDL_MOUSEMOTION:', '\t\t\tcase SDL_MOUSEBUTTONDOWN:')
position = between(mouse, 'static void screenToNative(', 'void SetRelativeMousePosition(')
raw_input = between(raw, 'void RawMouse::buttonInput(', 'static LRESULT CALLBACK rawWindowProc(')

prefix = r'''
#include <cstdint>
#include <iterator>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <cstring>
#if defined(VMU_TEST_OTHER)
#undef _WIN32
#elif !defined(_WIN32)
#define _WIN32
#endif
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
#define HAVE_MOVABLE_VMUS
#endif
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u16 = std::uint16_t;
using Uint64 = std::uint64_t;
Uint64 ticks = 0;
Uint64 SDL_GetTicks64() { return ticks; }
constexpr int SDL_FALSE=0, SDL_TRUE=1, SDL_DISABLE=0, SDL_ENABLE=1, SDL_QUERY=-1;
void* window = reinterpret_cast<void*>(1);
bool gameRunning=true, mouseCaptured=false, relativeMouse=false, captureSucceeds=true;
bool toggleRequested=false;
int cursorVisible=SDL_ENABLE, published=0, checks=0, captureAttempts=0;
bool vmu_lcd_status[8] {};
struct {
 struct { bool console=true; bool isConsole() const { return console; } } platform;
 struct { int width=640, height=480; } display;
} settings;
namespace config {
 bool UseRawInput=false, MovableVMUs=false, FloatVMUs=true, Rotate90=false, StretchToFill=false;
 int MouseSensitivity=100, ScreenStretching=100;
}
int SDL_SetRelativeMouseMode(int enabled) {
 if (enabled) ++captureAttempts;
 if (enabled && !captureSucceeds) return -1;
 relativeMouse=enabled; return 0;
}
int SDL_ShowCursor(int state) { if (state!=SDL_QUERY) cursorVisible=state; return cursorVisible; }
void SDL_SetWindowTitle(void*,const char*) {}
void setWindowTitleGame() {}
struct SDLGamepad { static void UpdateRumble() {} };
bool blockVmuMouse=false, pendingMouseReset=false;
namespace vmu_window {
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
void update(bool) {}
void publishScreen(int,const u32*,bool) { ++published; }
bool blocksMouseInput() {
 if (blockVmuMouse) pendingMouseReset=true;
 return blockVmuMouse;
}
bool consumeMousePositionReset() { bool pending=pendingMouseReset; pendingMouseReset=false; return pending; }
'''
prefix += active + '#endif\n}\n'
prefix += r'''
s32 mo_x_abs[4]{}, mo_y_abs[4]{};
s32 mo_x_prev[4]{-1,-1,-1,-1}, mo_y_prev[4]{-1,-1,-1,-1};
s32 mo_width=640, mo_height=480;
float mo_x_delta[4]{}, mo_y_delta[4]{};
std::mutex relPosMutex;
'''
prefix += position
prefix += r'''
class Mouse {
public:
 enum Button { LEFT_BUTTON=2, RIGHT_BUTTON=1, MIDDLE_BUTTON=3, BUTTON_4=4, BUTTON_5=5 };
 bool buttons[6]{};
 void setAbsPos(int x,int y,int width,int height) { SetMousePosition(x,y,width,height,0); }
 void setRelPos(float x,float y) { mo_x_delta[0]+=x; mo_y_delta[0]+=y; }
 void setButton(Button button,bool down) { buttons[button]=down; }
 void setWheel(int) {}
};
class SDLMouse : public Mouse {
public:
 void setAbsPos(int x,int y) { Mouse::setAbsPos(x,y,640,480); }
};
SDLMouse testMouse;
SDLMouse* getMouse(u32) { return &testMouse; }
constexpr u32 SDL_MOUSEMOTION=1, SDL_MOUSEBUTTONDOWN=2, SDL_MOUSEBUTTONUP=3, SDL_MOUSEWHEEL=4;
union SDL_Event {
 u32 type;
 struct { u32 type,which; int x,y,xrel,yrel; } motion;
 struct { u32 type,which; int x,y,button; } button;
 struct { u32 type; void* data1; } user;
};
std::deque<SDL_Event> eventQueue;
int SDL_PollEvent(SDL_Event* event) {
 if(eventQueue.empty()) return 0;
 *event=eventQueue.front(); eventQueue.pop_front(); return 1;
}
u32 openFileEventType() { return 1000; }
std::string openedFile;
void gui_start_game(const char* path) { openedFile=path; }
void SDL_free(void* pointer) { std::free(pointer); }
void gui_set_mouse_button(int,bool,bool) {}
void gui_set_mouse_position(int,int,bool) {}
void updateFullscreenCursorVisibility(int) {}
void checkRawInput() {}
void SDL_GetWindowSize(void*,int* width,int* height) { *width=640; *height=480; }
void SDL_WarpMouseInWindow(void*,int,int) {}
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
using HWND=void*;
struct POINT { long x,y; };
struct RAWMOUSE { u16 usFlags=0,usButtonFlags=0,usButtonData=0; long lLastX=0,lLastY=0; };
constexpr int MOUSE_MOVE_ABSOLUTE=1, MOUSE_VIRTUAL_DESKTOP=2;
constexpr int SM_CXVIRTUALSCREEN=0,SM_CYVIRTUALSCREEN=1,SM_CXSCREEN=2,SM_CYSCREEN=3;
constexpr u16 RI_MOUSE_LEFT_BUTTON_DOWN=1,RI_MOUSE_LEFT_BUTTON_UP=2,RI_MOUSE_RIGHT_BUTTON_DOWN=4,RI_MOUSE_RIGHT_BUTTON_UP=8;
constexpr u16 RI_MOUSE_MIDDLE_BUTTON_DOWN=16,RI_MOUSE_MIDDLE_BUTTON_UP=32,RI_MOUSE_BUTTON_4_DOWN=64,RI_MOUSE_BUTTON_4_UP=128;
constexpr u16 RI_MOUSE_BUTTON_5_DOWN=256,RI_MOUSE_BUTTON_5_UP=512,RI_MOUSE_WHEEL=1024;
constexpr int WHEEL_DELTA=120;
int GetSystemMetrics(int metric) { return metric%2==0 ? 640 : 480; }
HWND getNativeHwnd() { return window; }
void ScreenToClient(HWND,POINT*) {}
class RawMouse : public Mouse {
public:
 void buttonInput(Button button,u16 flags,u16 downFlag,u16 upFlag);
 void updateState(RAWMOUSE* state);
};
'''
prefix += raw_input + '#endif\n' + capture + cursor + transition
prefix += ' if (toggleRequested) {\n' + hotkey + ' toggleRequested=false;\n }\n'
# Include the real filtering and motion dispatch, not a duplicate of either path.
prefix += events + '\t\tswitch (event.type) {\n' + motion + '\t\tdefault: break;\n\t\t}\n\t}\n}\n' + reset

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
 SDL_Event file{}; file.user.type=openFileEventType();
 file.user.data1=std::malloc(9); std::memcpy(file.user.data1,"test.gdi",9);
 eventQueue.push_back(file); input_sdl_handle();
 require(openedFile=="test.gdi","dev file-opening events survive VMU event filtering");
 auto moveMouse=[](int x,int y) {
  SDL_Event event{}; event.motion.type=SDL_MOUSEMOTION; event.motion.x=x; event.motion.y=y;
  eventQueue.push_back(event); input_sdl_handle();
 };
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
 prepare(false); config::MovableVMUs=true; input_sdl_handle();
 moveMouse(100,100); moveMouse(110,105);
 require(mo_x_delta[0]==10 && mo_y_delta[0]==5,"normal SDL absolute motion retains its deltas");
 mo_x_delta[0]=mo_y_delta[0]=0;
 // Native completion publishes a pending reset even if no SDL events were queued.
 pendingMouseReset=true;
 moveMouse(400,300);
 require(mo_x_delta[0]==0 && mo_y_delta[0]==0,"first SDL event after a native-only drag rebases without a jump");
 moveMouse(405,303);
 require(mo_x_delta[0]==5 && mo_y_delta[0]==3,"SDL motion resumes after the first rebased event");
 mo_x_delta[0]=mo_y_delta[0]=0;
 testMouse.buttons[Mouse::LEFT_BUTTON]=true; blockVmuMouse=true;
 moveMouse(500,350);
 require(mo_x_delta[0]==0 && !testMouse.buttons[Mouse::LEFT_BUTTON],"suppressed SDL motion clears buttons without reaching the game");
 blockVmuMouse=false; moveMouse(550,370);
 require(mo_x_delta[0]==0 && mo_y_delta[0]==0,"resuming after suppressed SDL motion does not accumulate a jump");
 moveMouse(555,375);
 require(mo_x_delta[0]==5 && mo_y_delta[0]==5,"subsequent SDL motion is preserved after suppression");
 config::UseRawInput=true;
 RawMouse rawMouse; RAWMOUSE raw{}; raw.usFlags=MOUSE_MOVE_ABSOLUTE; raw.lLastX=raw.lLastY=10000;
 pendingMouseReset=true; rawMouse.updateState(&raw); mo_x_delta[0]=mo_y_delta[0]=0;
 blockVmuMouse=true; raw.lLastX=raw.lLastY=40000; rawMouse.updateState(&raw);
 blockVmuMouse=false; raw.lLastX=raw.lLastY=50000; rawMouse.updateState(&raw);
 require(mo_x_delta[0]==0 && mo_y_delta[0]==0,"absolute raw input resumes without replaying suppressed movement");
 raw.lLastX=raw.lLastY=51000; rawMouse.updateState(&raw);
 require(mo_x_delta[0]>0 && mo_y_delta[0]>0,"absolute raw input resumes normal deltas after rebasing");
 mo_x_delta[0]=mo_y_delta[0]=0; pendingMouseReset=true;
 raw.lLastX=raw.lLastY=60000; rawMouse.updateState(&raw);
 require(mo_x_delta[0]==0 && mo_y_delta[0]==0,"native-only drag also rebases the first absolute raw packet");
 raw.usFlags=0; raw.lLastX=7; raw.lLastY=-4; rawMouse.updateState(&raw);
 require(mo_x_delta[0]==7 && mo_y_delta[0]==-4,"ordinary relative raw packets retain their motion");
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
