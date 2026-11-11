package com.xiaoboluo.d2r;

import java.util.LinkedHashMap;
import java.util.HashSet;

/** UI-thread controller state; independent of Android so disconnect/drift rules are testable. */
final class GamepadState {
    static final int SWITCH = 0, XBOX = 2, PS = 3;
    interface Sink { void publish(long buttons, int lx, int ly, int rx, int ry, int glyph, boolean pad); }
    private static final class Pad {
        long keys, motion, blocked;
        final HashSet<Integer> heldKeys = new HashSet<>();
        final int[] axes = new int[4];
        final boolean[] on = new boolean[2], centered = {true, true};
        int glyph;
    }
    private final LinkedHashMap<Integer, Pad> pads = new LinkedHashMap<>();
    private final Sink sink;
    private boolean mode;
    private int glyph = XBOX;
    GamepadState(Sink sink) { this.sink = sink; }
    boolean controllerMode() { return mode; }

    // Android reports face buttons by position, including Nintendo (AOSP .kl).
    // Keep the upstream Xbox/PS/Switch glyph-to-function correspondence.
    static long button(int key, int glyph) {
        switch (key) {
            case 96: return 1L << (glyph == SWITCH ? 1 : 0); // south
            case 97: return 1L << (glyph == SWITCH ? 0 : 1); // east
            case 99: return 1L << (glyph == PS ? 2 : 3);   // west
            case 100: return 1L << (glyph == PS ? 3 : 2);  // north
            case 102: return 1L << 6; case 103: return 1L << 7;
            case 104: return 1L << 8; case 105: return 1L << 9;
            case 106: return 1L << 4; case 107: return 1L << 5;
            case 108: return 1L << 10; case 109: return 1L << 11;
            case 21: return 1L << 12; case 19: return 1L << 13;
            case 22: return 1L << 14; case 20: return 1L << 15;
            case 23: return 1L; // D-pad center, controller devices only
            case 4: return 2L; // controller BACK, never the system/keyboard Back
            default: return 0;
        }
    }
    private Pad pad(int id, int style) {
        Pad p = pads.computeIfAbsent(id, ignored -> new Pad()); p.glyph = style; return p;
    }
    boolean key(int id, int style, int key, boolean down) {
        long bit = button(key, style); if (bit == 0) return false;
        Pad p = pad(id, style); long before = p.keys | p.motion;
        if (down) p.heldKeys.add(key); else p.heldKeys.remove(key);
        p.keys = 0; for (int held : p.heldKeys) p.keys |= button(held, style);
        update(p, before); return true;
    }
    static float axis(float value, float min, float max, float flat) {
        if (!Float.isFinite(value) || !Float.isFinite(min) || !Float.isFinite(max) || min >= 0 || max <= 0) return 0;
        if (Math.abs(value) <= Math.max(0, flat)) return 0;
        return Math.max(-1, Math.min(1, value / (value < 0 ? -min : max)));
    }
    static float trigger(float value, float min, float max) {
        if (!Float.isFinite(value) || !Float.isFinite(min) || !Float.isFinite(max) || max <= min) return 0;
        return Math.max(0, Math.min(1, (value - min) / (max - min)));
    }
    private static int stick(float v) { return Float.isFinite(v) ? Math.round(Math.max(-1, Math.min(1, v)) * 32767) : 0; }
    void motion(int id, int style, float lx, float ly, float rx, float ry, float hatX, float hatY, float lt, float rt) {
        Pad p = pad(id, style); long before = p.keys | p.motion;
        long b = hatX < -.5f ? 1L << 12 : hatX > .5f ? 1L << 14 : 0;
        b |= hatY < -.5f ? 1L << 13 : hatY > .5f ? 1L << 15 : 0;
        // Trigger hysteresis; key and analog channels remain independent.
        if (lt >= ((p.motion & 256) != 0 ? .4f : .55f)) b |= 256;
        if (rt >= ((p.motion & 512) != 0 ? .4f : .55f)) b |= 512;
        p.motion = b;
        p.axes[0] = stick(lx); p.axes[1] = stick(-ly); // Android Y grows downward
        p.axes[2] = stick(rx); p.axes[3] = stick(-ry);
        update(p, before);
    }
    private void update(Pad p, long before) {
        long b = p.keys | p.motion;
        p.blocked &= b;
        boolean explicit = (b & ~before & ~p.blocked) != 0;
        for (int k = 0; k < 2; k++) {
            double m = Math.hypot(p.axes[k * 2], p.axes[k * 2 + 1]) / 32767;
            p.on[k] = m >= (p.on[k] ? .15 : .25);
            if (!p.on[k]) p.centered[k] = true;
            if (p.on[k] && p.centered[k]) explicit = true;
        }
        if (explicit) { mode = true; glyph = p.glyph; }
        publish();
    }
    void touched() {
        mode = false;
        for (Pad p : pads.values()) {
            p.blocked = p.keys | p.motion;
            for (int k = 0; k < 2; k++) p.centered[k] = !p.on[k];
        }
        publish();
    }
    void remove(int id) { pads.remove(id); if (pads.isEmpty()) mode = false; publish(); }
    void reset() { pads.clear(); publish(); } // Keep layout on focus loss; clear every held input.
    private void publish() {
        long b = 0; int[] a = new int[4]; long[] best = new long[2];
        if (mode) for (Pad p : pads.values()) {
            b |= (p.keys | p.motion) & ~p.blocked;
            for (int k = 0; k < 2; k++) if (p.on[k] && p.centered[k]) {
                int x = p.axes[2*k], y = p.axes[2*k+1]; long m = (long)x*x + (long)y*y;
                if (m > best[k]) { best[k] = m; a[2*k] = x; a[2*k+1] = y; }
            }
        }
        sink.publish(b, a[0], a[1], a[2], a[3], glyph, mode);
    }
}
