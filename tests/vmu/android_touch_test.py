# Portions Copyright 2026 The Hollycast Authors
"""Compile the production touch delegate against small Android/JNI boundary fakes."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1])
javac, java = sys.argv[2:4]
build.mkdir(parents=True, exist_ok=True)
sources = {
    'android/os/Looper.java': '''package android.os;
public class Looper { public static Looper getMainLooper() { return new Looper(); } }''',
    'android/os/Handler.java': '''package android.os;
public class Handler {
 public Handler(Looper looper) {} public void removeCallbacks(Runnable r) {}
 public void postDelayed(Runnable r, long ms) {}
}''',
    'android/view/MotionEvent.java': '''package android.view;
public class MotionEvent {
 public static final int ACTION_DOWN=0, ACTION_UP=1, ACTION_MOVE=2, ACTION_CANCEL=3,
 ACTION_POINTER_DOWN=5, ACTION_POINTER_UP=6, BUTTON_PRIMARY=1;
 private final int action, index; private final int[] ids; private final float[] xs, ys;
 public MotionEvent(int action, int index, int[] ids, float[] xs, float[] ys) {
  this.action=action; this.index=index; this.ids=ids; this.xs=xs; this.ys=ys;
 }
 public int getActionMasked(){return action;} public int getActionIndex(){return index;}
 public int getPointerId(int i){return ids[i];} public int getPointerCount(){return ids.length;}
 public float getX(int i){return xs[i];} public float getY(int i){return ys[i];}
 public int findPointerIndex(int id){for(int i=0;i<ids.length;i++)if(ids[i]==id)return i; return -1;}
}''',
    'com/hollycast/emulator/Emulator.java': '''package com.hollycast.emulator;
public class Emulator { public static int vibrationPower; }''',
    'com/hollycast/emulator/periph/InputDeviceManager.java': '''package com.hollycast.emulator.periph;
public class InputDeviceManager {
 private static final InputDeviceManager instance=new InputDeviceManager();
 public static int releases, gameEvents, mouseButtons;
 public static InputDeviceManager getInstance(){return instance;}
 public void virtualReleaseAll(){releases++;}
 public void virtualJoystick(float x,float y){gameEvents++;}
 public void virtualButtonInput(int c,boolean down){gameEvents++;}
 public void touchMouseEvent(int x,int y,int buttons){mouseButtons=buttons; if(buttons!=0)gameEvents++;}
}''',
    'com/hollycast/emulator/periph/VibratorThread.java': '''package com.hollycast.emulator.periph;
public class VibratorThread { public static VibratorThread getInstance(){return new VibratorThread();}
 public void click(){} public void stopThread(){} }''',
    'com/hollycast/emulator/emu/VGamepad.java': '''package com.hollycast.emulator.emu;
public class VGamepad {
 public static int hitTest(float x,float y){return -1;} public static float getControlWidth(int c){return 1;}
 public static boolean isCircularStickEnabled(){return true;}
 public static int getVibrationPower(){return 0;} public static int getIdleTimeoutSeconds(){return 0;}
 public static void show(){} public static void hide(){}
}''',
    'com/hollycast/emulator/emu/JNIdc.java': '''package com.hollycast.emulator.emu;
public class JNIdc {
 public static boolean hit=true, menu=false, gui=false, dragging=false;
 public static int downs, moves, ends; public static float x,y;
 public static boolean guiIsOpen(){return gui;}
 public static boolean guiShouldCaptureMenuTouch(float x,float y){return menu;}
 public static boolean vmuTouch(int action,float nx,float ny){
  if(action==0){downs++; dragging=hit;}
  if(action==2){moves++; x=nx; y=ny;}
  if(action==1 || action==3){ends++; dragging=false;}
  return action!=0 || hit;
 }
}''',
}
files = []
for name, source in sources.items():
    path = build / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('// Portions Copyright 2026 The Hollycast Authors\n' + source, encoding='utf-8')
    files.append(str(path))
production = root / 'shell/android-studio/flycast/src/main/java/com/hollycast/emulator/emu'
files += [str(production / name) for name in ('VirtualJoystickDelegate.java', 'TouchEventHandler.java')]
files.append(str(Path(__file__).with_name('AndroidTouchTest.java')))
subprocess.run([javac, '-encoding', 'UTF-8', '-d', str(build), *files], check=True)
subprocess.run([java, '-cp', str(build), 'com.hollycast.emulator.emu.AndroidTouchTest'], check=True)
