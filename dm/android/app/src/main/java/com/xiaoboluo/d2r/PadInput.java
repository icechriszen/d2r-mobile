package com.xiaoboluo.d2r;

import android.view.KeyEvent;
import android.view.MotionEvent;

/** Single upstream activity entry point, backed by the tested device-axis and
 * held-input state machine. Never dispatch the same event to two controllers. */
final class PadInput {
    private static GamepadInput input;
    private PadInput() {}
    static void init(DmActivity activity) {
        close();
        input = new GamepadInput(activity, DmActivity::onPadMode);
    }
    static boolean isPadMode() { return input != null && input.controllerMode(); }
    static boolean onKey(KeyEvent e) { return input != null && input.key(e); }
    static boolean onMotion(MotionEvent e) { return input != null && input.motion(e); }
    static void onTouch() { if (input != null) input.touch(); }
    static void enable(boolean enabled) { if (input != null) input.enable(enabled); }
    static void reset() { enable(false); }
    static void close() { if (input != null) { input.close(); input = null; } }
}
