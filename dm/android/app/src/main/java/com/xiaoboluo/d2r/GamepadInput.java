package com.xiaoboluo.d2r;

import android.hardware.input.InputManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.Locale;

/** Android standard gamepad events -> the upstream controller path (never touch emulation). */
final class GamepadInput implements InputManager.InputDeviceListener {
    interface ModeListener { void changed(boolean pad); }
    private static native void nativeState(long buttons, int lx, int ly, int rx, int ry, int glyph, boolean pad);
    private static native void nativeClear();
    private final InputManager inputs;
    private final GamepadState state;
    private boolean enabled;
    GamepadInput(DmActivity activity, ModeListener listener) {
        inputs = activity.getSystemService(InputManager.class);
        state = new GamepadState((b,lx,ly,rx,ry,glyph,pad) -> {
            nativeState(b,lx,ly,rx,ry,glyph,pad); listener.changed(pad);
        });
        if (inputs != null) inputs.registerInputDeviceListener(this, null);
        for (int id : InputDevice.getDeviceIds()) onInputDeviceAdded(id);
    }
    private static boolean controller(InputDevice d) {
        // Some TV remotes advertise JOYSTICK for a consumer-control axis only.
        // They must retain Android Back/DPAD navigation rather than become pads.
        return d != null && (d.supportsSource(InputDevice.SOURCE_GAMEPAD) ||
            (d.supportsSource(InputDevice.SOURCE_JOYSTICK) && d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null &&
                d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null));
    }
    private static int glyph(InputDevice d) {
        String name = d == null ? "" : d.getName().toLowerCase(Locale.ROOT);
        if (d != null && d.getVendorId() == 0x054c || name.contains("dualsense") || name.contains("dualshock")) return GamepadState.PS;
        if (d != null && d.getVendorId() == 0x057e || name.contains("switch pro") || name.contains("joy-con")) return GamepadState.SWITCH;
        return GamepadState.XBOX;
    }
    boolean key(KeyEvent e) {
        if (!enabled || !(controller(e.getDevice()) || e.isFromSource(InputDevice.SOURCE_GAMEPAD))) return false;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return false;
        if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() > 0) return GamepadState.button(e.getKeyCode(), glyph(e.getDevice())) != 0;
        return state.key(e.getDeviceId(), glyph(e.getDevice()), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN && !e.isCanceled());
    }
    private static InputDevice.MotionRange range(InputDevice d, int axis, int source) {
        InputDevice.MotionRange r = d.getMotionRange(axis, source);
        return r == null ? d.getMotionRange(axis) : r;
    }
    private static float value(MotionEvent e, int axis, int history) {
        return history < 0 ? e.getAxisValue(axis) : e.getHistoricalAxisValue(axis, history);
    }
    private static float axis(MotionEvent e, int primary, int fallback, int h, boolean trigger) {
        InputDevice d = e.getDevice(); int a = primary;
        InputDevice.MotionRange r = range(d,a,e.getSource());
        if (fallback >= 0 && (r == null || (!trigger && r.getMin() >= 0))) { a = fallback; r = range(d,a,e.getSource()); }
        if (r == null) return 0;
        float v = value(e,a,h);
        return trigger ? GamepadState.trigger(v,r.getMin(),r.getMax()) : GamepadState.axis(v,r.getMin(),r.getMax(),r.getFlat());
    }
    boolean motion(MotionEvent e) {
        if (!enabled || !e.isFromSource(InputDevice.SOURCE_JOYSTICK) || e.getActionMasked() != MotionEvent.ACTION_MOVE || !controller(e.getDevice())) return false;
        for (int h = 0; h <= e.getHistorySize(); h++) {
            int at = h == e.getHistorySize() ? -1 : h;
            state.motion(e.getDeviceId(),glyph(e.getDevice()),
                axis(e,MotionEvent.AXIS_X,-1,at,false), axis(e,MotionEvent.AXIS_Y,-1,at,false),
                axis(e,MotionEvent.AXIS_Z,MotionEvent.AXIS_RX,at,false), axis(e,MotionEvent.AXIS_RZ,MotionEvent.AXIS_RY,at,false),
                axis(e,MotionEvent.AXIS_HAT_X,-1,at,false), axis(e,MotionEvent.AXIS_HAT_Y,-1,at,false),
                axis(e,MotionEvent.AXIS_LTRIGGER,MotionEvent.AXIS_BRAKE,at,true), axis(e,MotionEvent.AXIS_RTRIGGER,MotionEvent.AXIS_GAS,at,true));
        }
        return true;
    }
    boolean controllerMode() { return state.controllerMode(); }
    boolean touch() { boolean was = state.controllerMode(); if (was) state.touched(); return was; }
    void enable(boolean on) { enabled = on; if (!on) { state.reset(); nativeClear(); } }
    void close() { enable(false); if (inputs != null) inputs.unregisterInputDeviceListener(this); }
    @Override public void onInputDeviceAdded(int id) {
        InputDevice d = InputDevice.getDevice(id);
        if (controller(d)) android.util.Log.i("dm-gamepad", "Connected " + d.getName() + " vendor=" + d.getVendorId() + " product=" + d.getProductId() + " glyph=" + glyph(d));
    }
    @Override public void onInputDeviceChanged(int id) { state.remove(id); onInputDeviceAdded(id); }
    @Override public void onInputDeviceRemoved(int id) { state.remove(id); android.util.Log.i("dm-gamepad", "Disconnected id=" + id); }
}
