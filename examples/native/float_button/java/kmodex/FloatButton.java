package kmodex;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/** A draggable button on top of the game's window. A tap calls nativeClick(id, false), a long press nativeClick(id, true). */
public final class FloatButton {
    static native void nativeClick(int id, boolean longPress);

    private static final Handler ui = new Handler(Looper.getMainLooper());
    private static final Map<Integer, TextView> buttons = new HashMap<>();

    public static void create(final Activity a, final int id, final String text, final int x, final int y) {
        ui.post(new Runnable() { public void run() { build(a, id, text, x, y); } });
    }

    public static void setText(final int id, final String text) {
        ui.post(new Runnable() { public void run() { TextView t = buttons.get(id); if (t != null) t.setText(text); } });
    }

    public static void destroy(final int id) {
        ui.post(new Runnable() { public void run() {
            TextView t = buttons.remove(id);
            if (t != null && t.getParent() != null) ((ViewGroup) t.getParent()).removeView(t);
        } });
    }

    private static void build(Activity a, final int id, String text, int x, int y) {
        if (buttons.containsKey(id)) return;
        final float density = a.getResources().getDisplayMetrics().density;
        final TextView b = new TextView(a);
        b.setText(text);
        b.setTextColor(Color.WHITE);
        b.setTextSize(13);
        b.setGravity(Gravity.CENTER);
        int pad = Math.round(10 * density);
        b.setPadding(pad * 2, pad, pad * 2, pad);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xCC27323F);
        bg.setCornerRadius(24 * density);
        bg.setStroke(Math.round(density), 0x88FFFFFF);
        b.setBackground(bg);
        b.setElevation(100 * density);

        final int slop = ViewConfiguration.get(a).getScaledTouchSlop();
        final long longPress = ViewConfiguration.getLongPressTimeout();
        b.setOnTouchListener(new View.OnTouchListener() {
            float downX, downY, startX, startY;
            boolean dragging, fired;
            Runnable pending;
            public boolean onTouch(final View v, MotionEvent e) {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) v.getLayoutParams();
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = e.getRawX(); downY = e.getRawY(); startX = lp.leftMargin; startY = lp.topMargin;
                        dragging = false; fired = false;
                        pending = new Runnable() { public void run() { if (!dragging) { fired = true; nativeClick(id, true); } } };
                        v.postDelayed(pending, longPress);
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx = e.getRawX() - downX, dy = e.getRawY() - downY;
                        if (!dragging && (Math.abs(dx) > slop || Math.abs(dy) > slop)) dragging = true;
                        if (dragging) {
                            lp.leftMargin = Math.max(0, Math.round(startX + dx));
                            lp.topMargin = Math.max(0, Math.round(startY + dy));
                            v.setLayoutParams(lp);
                        }
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        v.removeCallbacks(pending);
                        if (e.getActionMasked() == MotionEvent.ACTION_UP && !dragging && !fired) nativeClick(id, false);
                        return true;
                }
                return false;
            }
        });
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.leftMargin = x; lp.topMargin = y;
        ((ViewGroup) a.getWindow().getDecorView()).addView(b, lp);     // only the button's own area takes touches; the rest of the game is untouched
        buttons.put(id, b);
    }
}
