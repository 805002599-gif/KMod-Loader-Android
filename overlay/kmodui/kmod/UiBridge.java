package kmod;

import android.app.Activity;
import android.app.Application;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.ArrayAdapter;
import android.widget.AdapterView;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Map;

/**
 * Native-mod UI: panels built from a JSON description with plain Android views (a Dialog on the game's activity).
 * Loaded by the core from an embedded dex (DexClassLoader); the core calls show/set/close and receives events through
 * nativeEvent (registered with RegisterNatives).
 *
 * spec: { "title": "..", "modal": true, "close": true, "items": [
 *   {"type":"text","id":"t","text":".."}, {"type":"button","id":"b","text":".."},
 *   {"type":"switch","id":"s","text":"..","value":true}, {"type":"slider","id":"v","text":"..","min":0,"max":1,"value":0.5},
 *   {"type":"edit","id":"e","text":"hint","value":".."}, {"type":"choice","id":"c","text":"..","options":["a","b"],"value":"a"} ] }
 * events (panel, id, value): click -> "click", switch -> "true"/"false", slider -> number, edit -> text, choice -> option;
 * closing the panel -> id "$close".
 */
public final class UiBridge {
    private static final String TAG = "KModUi";
    static native void nativeEvent(int panel, String id, String value);
    static native void nativeRun(long fn, long user);

    private static final Handler ui = new Handler(Looper.getMainLooper());
    private static final Map<Integer, Panel> panels = new HashMap<>();

    private static final class Panel {
        Dialog dlg;
        final Map<String, View> views = new HashMap<>();
        final Map<String, double[]> ranges = new HashMap<>();
        boolean quiet;                                   // programmatic change in progress: no event
        final Map<String, String> pendingChoice = new HashMap<>();   // spinner selections made by set(): their (asynchronous) event is dropped
    }

    private static Activity activity() {
        Activity found = null;
        try {                                                         // the engine keeps its activity in a static field
            Class<?> at = Class.forName("android.app.ActivityThread");
            Application app = (Application) at.getMethod("currentApplication").invoke(null);
            Class<?> ga = app.getClassLoader().loadClass("com.epicgames.unreal.GameActivity");
            found = (Activity) ga.getField("_activity").get(null);
        } catch (Throwable t) { Log.w(TAG, "GameActivity._activity: " + t); }
        if (found != null && !found.isFinishing()) return found;
        try {                                                         // otherwise: the activities the process currently has
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Field f = at.getDeclaredField("mActivities");
            f.setAccessible(true);
            Map<?, ?> m = (Map<?, ?>) f.get(thread);
            Activity any = null;
            for (Object rec : m.values()) {
                java.lang.reflect.Field af = rec.getClass().getDeclaredField("activity");
                af.setAccessible(true);
                Activity act = (Activity) af.get(rec);
                if (act == null || act.isFinishing()) continue;
                if (act.getClass().getName().endsWith("GameActivity")) return act;
                any = act;
            }
            return any;
        } catch (Throwable t) {
            Log.e(TAG, "no activity", t);
            return null;
        }
    }

    /** The game's current activity (null if there is none). */
    public static Activity currentActivity() { return activity(); }

    /** Runs a native function pointer on the Android UI thread. */
    public static void runOnUi(final long fn, final long user) {
        ui.post(new Runnable() { public void run() { nativeRun(fn, user); } });
    }

    public static void show(final int id, final String spec) {
        ui.post(new Runnable() { public void run() { try { build(id, spec); } catch (Throwable t) { Log.e(TAG, "show failed", t); nativeEvent(id, "$error", String.valueOf(t)); } } });
    }

    public static void set(final int id, final String key, final String value) {
        ui.post(new Runnable() { public void run() { try { apply(id, key, value); } catch (Throwable t) { Log.e(TAG, "set failed", t); } } });
    }

    public static void close(final int id) {
        ui.post(new Runnable() { public void run() { Panel p = panels.get(id); if (p != null && p.dlg != null) p.dlg.dismiss(); } });
    }

    private static int dp(Activity a, float v) { return Math.round(v * a.getResources().getDisplayMetrics().density); }

    private static GradientDrawable round(Activity a, int color, float radius) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(a, radius));
        return g;
    }

    private static TextView label(Activity a, String text, float size, boolean bold) {
        TextView t = new TextView(a);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(size);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private static void build(final int id, String specText) throws Exception {
        final Activity a = activity();
        if (a == null) throw new IllegalStateException("no activity");
        JSONObject spec = new JSONObject(specText);
        final Panel p = new Panel();
        panels.put(id, p);

        LinearLayout col = new LinearLayout(a);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(a, 16), dp(a, 14), dp(a, 16), dp(a, 14));
        String title = spec.optString("title", "");
        if (!title.isEmpty()) {
            TextView t = label(a, title, 18, true);
            t.setPadding(0, 0, 0, dp(a, 8));
            col.addView(t);
        }
        JSONArray items = spec.optJSONArray("items");
        for (int i = 0; items != null && i < items.length(); i++) {
            JSONObject it = items.getJSONObject(i);
            final String type = it.optString("type", "text"), key = it.optString("id", "");
            String text = it.optString("text", "");
            View v;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(a, 8);
            if (type.equals("button")) {
                TextView b = label(a, text, 15, false);
                b.setGravity(Gravity.CENTER);
                b.setPadding(dp(a, 12), dp(a, 10), dp(a, 12), dp(a, 10));
                b.setBackground(round(a, 0xFF3B4A5A, 8));
                final String k = key;
                b.setOnClickListener(new View.OnClickListener() { public void onClick(View x) { nativeEvent(id, k, "click"); } });
                v = b;
            } else if (type.equals("switch")) {
                Switch s = new Switch(a);
                s.setText(text);
                s.setTextColor(Color.WHITE);
                s.setChecked(it.optBoolean("value", false));
                final String k = key;
                s.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
                    public void onCheckedChanged(CompoundButton c, boolean on) { if (!p.quiet) nativeEvent(id, k, on ? "true" : "false"); } });
                v = s;
            } else if (type.equals("slider")) {
                LinearLayout box = new LinearLayout(a);
                box.setOrientation(LinearLayout.VERTICAL);
                final TextView cap = label(a, text, 14, false);
                final double min = it.optDouble("min", 0), max = it.optDouble("max", 1);
                final SeekBar sb = new SeekBar(a);
                sb.setMax(1000);
                double val = it.optDouble("value", min);
                sb.setProgress((int) Math.round((val - min) / (max - min) * 1000));
                p.ranges.put(key, new double[] { min, max });
                final String k = key, base = text;
                cap.setText(base + "  " + trim(val));
                sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                    public void onProgressChanged(SeekBar s, int pr, boolean user) { cap.setText(base + "  " + trim(min + (max - min) * pr / 1000.0)); }
                    public void onStartTrackingTouch(SeekBar s) {}
                    public void onStopTrackingTouch(SeekBar s) { if (!p.quiet) nativeEvent(id, k, trim(min + (max - min) * s.getProgress() / 1000.0)); }
                });
                box.addView(cap); box.addView(sb);
                p.views.put(key + "#cap", cap);
                v = box;
                p.views.put(key, sb);
                box.setTag(key);
                col.addView(v, lp);
                continue;
            } else if (type.equals("edit")) {
                final EditText e = new EditText(a);
                e.setHint(text);
                e.setText(it.optString("value", ""));
                e.setTextColor(Color.WHITE);
                e.setHintTextColor(0xFF8A96A3);
                e.setSingleLine(true);
                e.setInputType(InputType.TYPE_CLASS_TEXT);
                e.setImeOptions(EditorInfo.IME_ACTION_DONE);
                final String k = key;
                e.setOnEditorActionListener(new TextView.OnEditorActionListener() {
                    public boolean onEditorAction(TextView t, int act, android.view.KeyEvent ev) { if (!p.quiet) nativeEvent(id, k, t.getText().toString()); return false; } });
                e.setOnFocusChangeListener(new View.OnFocusChangeListener() {
                    public void onFocusChange(View x, boolean has) { if (!has && !p.quiet) nativeEvent(id, k, e.getText().toString()); } });
                v = e;
            } else if (type.equals("choice")) {
                LinearLayout box = new LinearLayout(a);
                box.setOrientation(LinearLayout.VERTICAL);
                box.addView(label(a, text, 14, false));
                JSONArray opts = it.optJSONArray("options");
                final String[] names = new String[opts == null ? 0 : opts.length()];
                for (int j = 0; j < names.length; j++) names[j] = opts.getString(j);
                Spinner sp = new Spinner(a);
                sp.setAdapter(new ArrayAdapter<String>(a, android.R.layout.simple_spinner_dropdown_item, names));
                String cur = it.optString("value", "");
                for (int j = 0; j < names.length; j++) if (names[j].equals(cur)) sp.setSelection(j);
                final String k = key;
                final boolean[] first = { true };
                sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                    public void onItemSelected(AdapterView<?> par, View x, int pos, long rid) {
                        if (first[0]) { first[0] = false; return; }
                        if (names[pos].equals(p.pendingChoice.remove(k))) return;
                        if (!p.quiet) nativeEvent(id, k, names[pos]);
                    }
                    public void onNothingSelected(AdapterView<?> par) {}
                });
                box.addView(sp);
                p.views.put(key, sp);
                col.addView(box, lp);
                continue;
            } else {
                v = label(a, text, 14, false);
            }
            if (!key.isEmpty()) p.views.put(key, v);
            col.addView(v, lp);
        }
        if (spec.optBoolean("close", true)) {
            TextView c = label(a, spec.optString("closeText", "Close"), 15, false);
            c.setGravity(Gravity.CENTER);
            c.setPadding(dp(a, 12), dp(a, 10), dp(a, 12), dp(a, 10));
            c.setBackground(round(a, 0xFF4A3B3B, 8));
            final Panel pp = p;
            c.setOnClickListener(new View.OnClickListener() { public void onClick(View x) { pp.dlg.dismiss(); } });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(a, 14);
            col.addView(c, lp);
        }

        final int maxH = (int) (a.getResources().getDisplayMetrics().heightPixels * 0.9);
        ScrollView sv = new ScrollView(a) {                // never taller than the screen: the content scrolls
            @Override protected void onMeasure(int w, int h) { super.onMeasure(w, View.MeasureSpec.makeMeasureSpec(maxH, View.MeasureSpec.AT_MOST)); }
        };
        sv.setBackground(round(a, 0xF21E2530, 14));
        sv.addView(col);
        Dialog d = new Dialog(a);
        d.requestWindowFeature(Window.FEATURE_NO_TITLE);
        d.setContentView(sv);
        Window w = d.getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        int width = Math.min(dp(a, 520), (int) (a.getResources().getDisplayMetrics().widthPixels * 0.92));
        w.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
        if (!spec.optBoolean("modal", true)) {
            w.addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL);
            w.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        d.setOnDismissListener(new android.content.DialogInterface.OnDismissListener() {
            public void onDismiss(android.content.DialogInterface x) { panels.remove(id); nativeEvent(id, "$close", ""); } });
        p.dlg = d;
        d.show();
    }

    private static String trim(double v) {
        String s = String.format(java.util.Locale.US, "%.3f", v);
        if (s.indexOf('.') >= 0) { s = s.replaceAll("0+$", ""); if (s.endsWith(".")) s = s.substring(0, s.length() - 1); }
        return s;
    }

    private static void apply(int id, String key, String value) {
        Panel p = panels.get(id);
        if (p == null) return;
        View v = p.views.get(key);
        if (v == null) return;
        p.quiet = true;
        try {
            if (v instanceof Switch) ((Switch) v).setChecked(value.equals("true"));
            else if (v instanceof SeekBar) {
                double[] r = p.ranges.get(key);
                double val = Double.parseDouble(value);
                ((SeekBar) v).setProgress((int) Math.round((val - r[0]) / (r[1] - r[0]) * 1000));
            }
            else if (v instanceof Spinner) {
                Spinner sp = (Spinner) v;
                for (int i = 0; i < sp.getAdapter().getCount(); i++)
                    if (String.valueOf(sp.getAdapter().getItem(i)).equals(value)) {
                        if (sp.getSelectedItemPosition() != i) { p.pendingChoice.put(key, value); sp.setSelection(i); }
                        break;
                    }
            }
            else if (v instanceof TextView) ((TextView) v).setText(value);
        } finally { p.quiet = false; }
    }
}
