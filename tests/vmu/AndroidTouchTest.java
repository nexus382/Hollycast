// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator.emu;

import android.view.MotionEvent;
import com.hollycast.emulator.periph.InputDeviceManager;

public class AndroidTouchTest {
    private static int checks;
    private static void require(boolean condition, String message) {
        checks++;
        if (!condition) throw new AssertionError(message);
    }
    private static MotionEvent event(int action, int index, int... ids) {
        float[] x = new float[ids.length], y = new float[ids.length];
        for (int i = 0; i < ids.length; i++) { x[i] = 100 + i * 200; y[i] = 80 + i * 100; }
        return new MotionEvent(action, index, ids, x, y);
    }
    private static void send(VirtualJoystickDelegate delegate, int action, int index, int... ids) {
        delegate.onTouchEvent(event(action, index, ids), 1000, 500);
    }
    public static void main(String[] args) {
        VirtualJoystickDelegate delegate = new VirtualJoystickDelegate();
        send(delegate, MotionEvent.ACTION_DOWN, 0, 4);
        require(JNIdc.dragging && InputDeviceManager.releases == 1, "VMU starts drag and releases game controls");
        require(InputDeviceManager.gameEvents == 0 && InputDeviceManager.mouseButtons == 0, "VMU down is not a game click");
        send(delegate, MotionEvent.ACTION_POINTER_DOWN, 1, 4, 9);
        send(delegate, MotionEvent.ACTION_MOVE, 0, 9, 4);
        require(Math.abs(JNIdc.x - .3f) < .0001f && Math.abs(JNIdc.y - .36f) < .0001f,
                "pointer identity survives index reordering and coordinates use the Android view");
        send(delegate, MotionEvent.ACTION_POINTER_UP, 1, 9, 4);
        require(!JNIdc.dragging, "lifting drag pointer ends native drag");
        int moves = JNIdc.moves, downs = JNIdc.downs;
        send(delegate, MotionEvent.ACTION_MOVE, 0, 9);
        send(delegate, MotionEvent.ACTION_POINTER_DOWN, 1, 9, 11);
        require(JNIdc.moves == moves && JNIdc.downs == downs && InputDeviceManager.gameEvents == 0,
                "remaining fingers stay quarantined until the gesture ends");
        send(delegate, MotionEvent.ACTION_UP, 0, 9);
        JNIdc.hit = false;
        send(delegate, MotionEvent.ACTION_DOWN, 0, 3);
        require(InputDeviceManager.gameEvents == 1, "new non-VMU gesture reaches gameplay");
        JNIdc.hit = true;
        send(delegate, MotionEvent.ACTION_POINTER_DOWN, 1, 3, 6);
        require(JNIdc.dragging && InputDeviceManager.mouseButtons == 0, "VMU takeover releases a held game mouse");
        send(delegate, MotionEvent.ACTION_CANCEL, 0, 3, 6);
        require(!JNIdc.dragging, "Android cancellation ends native drag");
        downs = JNIdc.downs;
        JNIdc.menu = true;
        send(delegate, MotionEvent.ACTION_DOWN, 0, 1);
        require(JNIdc.downs == downs, "menu bar has priority over a new VMU gesture");
        send(delegate, MotionEvent.ACTION_UP, 0, 1);
        JNIdc.menu = false;
        JNIdc.gui = true;
        send(delegate, MotionEvent.ACTION_DOWN, 0, 2);
        require(JNIdc.downs == downs, "open settings cannot start a VMU drag");
        send(delegate, MotionEvent.ACTION_UP, 0, 2);
        JNIdc.gui = false;
        send(delegate, MotionEvent.ACTION_DOWN, 0, 7);
        send(delegate, MotionEvent.ACTION_MOVE, 0, 8);
        require(!JNIdc.dragging, "missing pointer cancels native capture");
        send(delegate, MotionEvent.ACTION_UP, 0, 8);
        send(delegate, MotionEvent.ACTION_DOWN, 0, 7);
        delegate.stop();
        require(!JNIdc.dragging, "view teardown releases native capture");
        delegate = new VirtualJoystickDelegate();
        send(delegate, MotionEvent.ACTION_DOWN, 0, 7);
        delegate.onTouchEvent(event(MotionEvent.ACTION_MOVE, 0, 7), 0, 0);
        require(!JNIdc.dragging, "zero-size surface cancels capture without invalid coordinates");
        send(delegate, MotionEvent.ACTION_DOWN, 0, 8);
        require(JNIdc.dragging, "fresh down recovers even if the previous gesture lost its final event");
        delegate.stop();
        System.out.println(checks + " checks passed");
    }
}
