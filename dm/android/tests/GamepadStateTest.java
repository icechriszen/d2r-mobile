package com.xiaoboluo.d2r;

/** javac GamepadState.java GamepadStateTest.java; java -ea com.xiaoboluo.d2r.GamepadStateTest */
public final class GamepadStateTest {
    private static final class Output implements GamepadState.Sink {
        long b; int lx, ly, rx, ry, glyph; boolean pad;
        public void publish(long b,int lx,int ly,int rx,int ry,int glyph,boolean pad) {
            this.b=b; this.lx=lx; this.ly=ly; this.rx=rx; this.ry=ry; this.glyph=glyph; this.pad=pad;
        }
    }
    private static void check(boolean v) { if (!v) throw new AssertionError(); }
    public static void main(String[] args) {
        Output o = new Output(); GamepadState s = new GamepadState(o);
        check(GamepadState.button(96,2)==1 && GamepadState.button(97,2)==2);
        check(GamepadState.button(99,2)==8 && GamepadState.button(100,2)==4);
        check(GamepadState.button(99,3)==4 && GamepadState.button(100,3)==8);
        check(GamepadState.button(96,0)==2 && GamepadState.button(97,0)==1);
        check(GamepadState.button(99,0)==8 && GamepadState.button(100,0)==4);
        check(!s.key(1,2,3,true)); // system Home is not a game button
        s.motion(1,2,.1f,0,0,0,0,0,0,0); check(!o.pad && o.lx==0);
        s.motion(1,2,.3f,-.5f,0,0,0,0,0,0); check(o.pad && o.lx>0 && o.ly>0);
        s.touched(); check(!o.pad && o.lx==0);
        s.motion(1,2,.8f,0,0,0,0,0,0,0); check(!o.pad && o.lx==0);
        s.motion(1,2,0,0,0,0,0,0,0,0);
        s.motion(1,2,.3f,0,0,0,0,0,0,0); check(o.pad && o.lx>0);
        s.motion(1,2,.18f,0,0,0,0,0,0,0); check(o.lx>0);
        s.motion(1,2,.1f,0,0,0,0,0,0,0); check(o.lx==0);
        s.key(1,2,96,true); s.key(2,3,96,true); s.remove(1); check(o.b==1 && o.pad);
        s.key(2,3,96,false); check(o.b==0);
        s.key(2,3,97,true); s.key(2,3,4,true); s.key(2,3,97,false); check(o.b==2);
        s.key(2,3,4,false); check(o.b==0);
        s.key(2,3,105,true); s.motion(2,3,0,0,0,0,0,0,0,1);
        s.key(2,3,105,false); check(o.b==512); // analog channel still holds trigger
        s.motion(2,3,0,0,0,0,0,0,0,.45f); check(o.b==512);
        s.motion(2,3,0,0,0,0,0,0,0,.3f); check(o.b==0);
        s.key(2,3,21,true); s.motion(2,3,0,0,0,0,-1,0,0,0);
        s.motion(2,3,0,0,0,0,0,0,0,0); check(o.b==4096);
        s.touched(); s.key(2,3,96,true); check(o.b==1); // previously held D-pad cannot leak across touch switch
        s.key(2,3,21,false); s.key(2,3,21,true); check(o.b==4097);
        s.motion(2,3,0,0,1,0,0,0,0,0);
        s.motion(3,2,-1,0,0,.5f,0,0,0,0); check(o.lx==-32767 && o.rx==32767 && o.ry==0);
        s.remove(2); check(o.b==0 && o.rx==0 && o.ry<0);
        s.reset(); check(o.b==0 && o.lx==0 && o.rx==0 && o.ry==0);
        s.remove(3); check(!o.pad);
        check(GamepadState.axis(Float.NaN,-1,1,0)==0);
        check(GamepadState.axis(.05f,-1,1,.1f)==0);
        check(GamepadState.axis(-2,-1,1,0)==-1);
        check(GamepadState.trigger(-1,-1,1)==0 && GamepadState.trigger(1,-1,1)==1);
        check(GamepadState.trigger(0,0,1)==0 && GamepadState.trigger(1,0,1)==1);
        System.out.println("GamepadState: mappings, drift, touch takeover, multiple devices, triggers, focus reset PASS");
    }
}
