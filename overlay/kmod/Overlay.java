package kmod;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * kmod dev overlay: a draggable floating button + Lua console panel drawn with plain Android views on top of the
 * game's activity (no UE rendering, no "draw over other apps" permission - it lives inside the game's own window).
 * Native side (libkmod.so) provides nativeEval(); the core calls appendBytes() to print output.
 */
public final class Overlay implements Application.ActivityLifecycleCallbacks {
    private static final String TAG = "KModOverlay";

    static native void nativeEval(String code);

    private static Overlay inst;
    private static final StringBuilder buf = new StringBuilder();

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ArrayList<String> history = new ArrayList<>();
    private int historyPos = 0;

    private Activity activity;
    private FrameLayout layer;
    private TextView fab;
    private LinearLayout panel;
    private TextView out;
    private ScrollView scroll;
    private TextView field;
    private String lastCode = "";
    private boolean open;

    // ------------------------------------------------------------------ entry points
    /** Called from JNI_OnLoad on the Java thread while the game starts. */
    public static void start() {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Application app = (Application) at.getMethod("currentApplication").invoke(null);
            inst = new Overlay();
            app.registerActivityLifecycleCallbacks(inst);
            append("[overlay] ready - tap the kmod button");
        } catch (Throwable t) {
            Log.e(TAG, "start failed", t);
        }
    }

    /** Called from native code (any thread) with UTF-8 bytes. */
    public static void appendBytes(byte[] utf8) {
        append(new String(utf8, StandardCharsets.UTF_8));
    }

    public static void append(final String line) {
        synchronized (buf) {
            buf.append(line).append('\n');
            if (buf.length() > 60000) buf.delete(0, buf.length() - 40000);
        }
        final Overlay o = inst;
        if (o != null) o.ui.post(new Runnable() { public void run() { o.refreshOut(); } });
    }

    // ------------------------------------------------------------------ lifecycle
    @Override public void onActivityResumed(Activity a) {
        if (a.getClass().getName().endsWith("GameActivity") && a != activity) attach(a);
    }
    @Override public void onActivityDestroyed(Activity a) {
        if (a == activity) { activity = null; layer = null; panel = null; fab = null; }
    }
    @Override public void onActivityCreated(Activity a, Bundle b) {}
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityPaused(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}

    // ------------------------------------------------------------------ UI
    private int dp(float v) { return Math.round(v * activity.getResources().getDisplayMetrics().density); }

    private GradientDrawable round(int color, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radiusDp));
        return g;
    }

    private TextView button(String text, View.OnClickListener l) {
        TextView t = new TextView(activity);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(13);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(10), dp(6), dp(10), dp(6));
        t.setBackground(round(0xFF3B4A5A, 6));
        t.setOnClickListener(l);
        return t;
    }

    private void attach(final Activity a) {
        activity = a;
        ui.post(new Runnable() { public void run() { try { build(a); } catch (Throwable t) { Log.e(TAG, "build failed", t); } } });
    }

    private void build(Activity a) {
        ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
        layer = new FrameLayout(a);                      // transparent, passes touches through when nothing is hit
        decor.addView(layer, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        layer.setElevation(dp(100));

        // ---- floating button (draggable; tap toggles the panel)
        fab = new TextView(a);
        fab.setText("kmod");
        fab.setTextColor(Color.WHITE);
        fab.setTextSize(12);
        fab.setTypeface(Typeface.DEFAULT_BOLD);
        fab.setGravity(Gravity.CENTER);
        fab.setBackground(round(0xCC2E7D32, 28));
        FrameLayout.LayoutParams fp = new FrameLayout.LayoutParams(dp(52), dp(52));
        fp.leftMargin = dp(150); fp.topMargin = dp(6);
        layer.addView(fab, fp);
        fab.setOnTouchListener(new View.OnTouchListener() {
            float dx, dy, sx, sy; boolean moved;
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN: sx = e.getRawX(); sy = e.getRawY(); dx = v.getX() - sx; dy = v.getY() - sy; moved = false; return true;
                    case MotionEvent.ACTION_MOVE:
                        if (Math.abs(e.getRawX() - sx) > dp(6) || Math.abs(e.getRawY() - sy) > dp(6)) moved = true;
                        if (moved) { v.setX(e.getRawX() + dx); v.setY(e.getRawY() + dy); }
                        return true;
                    case MotionEvent.ACTION_UP: if (!moved) toggle(); return true;
                }
                return false;
            }
        });

        // ---- console panel
        panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(round(0xEE0E1217, 10));
        panel.setPadding(dp(8), dp(8), dp(8), dp(8));
        panel.setClickable(true);                        // swallow touches inside the panel
        int w = Math.min(a.getResources().getDisplayMetrics().widthPixels * 62 / 100, dp(720));
        int h = a.getResources().getDisplayMetrics().heightPixels * 66 / 100;
        FrameLayout.LayoutParams pp = new FrameLayout.LayoutParams(w, h);
        pp.leftMargin = dp(8); pp.topMargin = dp(64);
        layer.addView(panel, pp);
        panel.setVisibility(View.GONE);

        LinearLayout bar = new LinearLayout(a);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        TextView title = new TextView(a);
        title.setText("kmod Lua console");
        title.setTextColor(0xFFB0BEC5);
        title.setTextSize(13);
        bar.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        bar.addView(button("Clear", new View.OnClickListener() { public void onClick(View v) { synchronized (buf) { buf.setLength(0); } refreshOut(); } }));
        space(bar);
        bar.addView(button("Reload mods", new View.OnClickListener() { public void onClick(View v) { run("kmod.reload_mods()"); } }));
        space(bar);
        bar.addView(button("X", new View.OnClickListener() { public void onClick(View v) { toggle(); } }));
        panel.addView(bar);

        scroll = new ScrollView(a);
        out = new TextView(a);
        out.setTextColor(0xFFC8E6C9);
        out.setTextSize(11);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextIsSelectable(true);
        out.setPadding(dp(4), dp(6), dp(4), dp(6));
        scroll.addView(out);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        sp.topMargin = dp(6); sp.bottomMargin = dp(6);
        panel.addView(scroll, sp);

        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        field = new TextView(a);
        field.setText("Tap to type Lua...");
        field.setTextColor(0xFF78909C);
        field.setTextSize(13);
        field.setTypeface(Typeface.MONOSPACE);
        field.setSingleLine(true);
        field.setBackground(round(0xFF1C2630, 6));
        field.setPadding(dp(8), dp(10), dp(8), dp(10));
        field.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { showInput(); } });
        row.addView(field, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        space(row);
        row.addView(button("Run last", new View.OnClickListener() { public void onClick(View v) { if (!lastCode.isEmpty()) run(lastCode); } }));
        panel.addView(row);

        refreshOut();
    }

    private void space(LinearLayout l) {
        View s = new View(activity);
        l.addView(s, new LinearLayout.LayoutParams(dp(6), 1));
    }

    private void toggle() {
        if (panel == null) return;
        open = !open;
        panel.setVisibility(open ? View.VISIBLE : View.GONE);
        if (open) refreshOut();
    }

    private void refreshOut() {
        if (out == null) return;
        String s;
        synchronized (buf) { s = buf.toString(); }
        out.setText(s);
        scroll.post(new Runnable() { public void run() { scroll.fullScroll(View.FOCUS_DOWN); } });
    }

    /** The game's main window can't take IME focus, so typing happens in a small dialog (same trick UE uses). */
    private void showInput() {
        if (activity == null) return;
        final EditText et = new EditText(activity);
        et.setSingleLine(true);
        et.setTypeface(Typeface.MONOSPACE);
        et.setTextSize(15);
        et.setHint("print(FindFirstOf(\"PlayerController\"))");
        et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        et.setImeOptions(EditorInfo.IME_ACTION_SEND | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        final AlertDialog d = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Lua").setView(et)
                .setPositiveButton("Run", null)
                .setNeutralButton("Last", null)
                .setNegativeButton("Cancel", null).create();
        d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        d.show();
        d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { submit(et.getText().toString()); d.dismiss(); }
        });
        d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(new View.OnClickListener() {   // recall previous command (does not close)
            public void onClick(View v) { et.setText(lastCode); et.setSelection(et.getText().length()); }
        });
        et.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            public boolean onEditorAction(TextView v, int actionId, android.view.KeyEvent ev) { submit(et.getText().toString()); d.dismiss(); return true; }
        });
        et.requestFocus();
    }

    private void submit(String code) {
        code = code.trim();
        if (code.isEmpty()) return;
        lastCode = code;
        field.setText("Tap to type Lua...");      // input bar is cleared after every run
        field.setTextColor(0xFF78909C);
        run(code);
    }

    private void run(String code) {
        append("> " + code);
        try { nativeEval(code); } catch (Throwable t) { append("[overlay] nativeEval failed: " + t); }
    }
}
