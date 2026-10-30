/* The Offline screen (2.3): what's saved on this phone, the newest first - each with its poster, title, size and
 * state (a bar while it saves, Failed + Try again), Play and Delete - and the phone's free room. Opened from Hemma's
 * Watch (hemma-app:offline) and from the finder's panel when Hemma can't be reached: away from home is when it's for.
 * Built in code like MainActivity's panel; Back goes back where it came from. */
package io.github.kloparn.hemma;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.format.Formatter;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.List;

public final class OfflineActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private LinearLayout list;
    private TextView room;
    private String shown = ""; /* what the list was drawn from: drawn again only when that changes */
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            draw(false);
            main.postDelayed(this, 1000);
        }
    };

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private boolean night() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(28), dp(20), dp(24));
        TextView title = new TextView(this);
        title.setText(R.string.offline);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        box.addView(title);
        room = new TextView(this);
        room.setPadding(0, dp(4), 0, dp(2));
        box.addView(room);
        TextView stays = new TextView(this);
        stays.setText(R.string.offline_stays);
        stays.setAlpha(0.75f);
        stays.setPadding(0, 0, 0, dp(12));
        box.addView(stays);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        scroll.setBackgroundColor(night() ? Color.parseColor("#121316") : Color.parseColor("#f4f1ea")); /* Hemma's --bg */
        setContentView(scroll);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            scroll.setOnApplyWindowInsetsListener((v, ins) -> {
                Insets s = ins.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                v.setPadding(s.left, s.top, s.right, s.bottom);
                return WindowInsets.CONSUMED;
            });
        }
    }

    @Override protected void onResume() {
        super.onResume();
        draw(true);
        main.postDelayed(tick, 1000);
    }

    @Override protected void onPause() {
        super.onPause();
        main.removeCallbacks(tick);
    }

    /* The list again - whole when what's saved changed (something new, gone, ready, failed), else only the bars */
    private void draw(boolean always) {
        List<Saved.Item> all = Offline.all(this);
        StringBuilder sig = new StringBuilder();
        for (Saved.Item it : all) sig.append(it.id).append(it.state).append(it.posterFile).append(';');
        room.setText(getString(R.string.free_space, Formatter.formatShortFileSize(this, Offline.free(this))));
        if (!always && sig.toString().equals(shown)) {
            for (Saved.Item it : all) {
                View row = list.findViewWithTag(it.id);
                ProgressBar bar = row != null ? row.findViewWithTag("bar") : null;
                TextView st = row != null ? row.findViewWithTag("state") : null;
                if (bar != null) bar.setProgress(it.percent());
                if (st != null && it.state.equals("saving")) st.setText(saving(it));
            }
            return;
        }
        shown = sig.toString();
        list.removeAllViews();
        if (all.isEmpty()) {
            TextView none = new TextView(this);
            none.setText(R.string.offline_empty);
            none.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
            none.setPadding(0, dp(24), 0, 0);
            list.addView(none);
            return;
        }
        for (Saved.Item it : all) list.addView(row(it));
    }

    private String saving(Saved.Item it) {
        boolean now = it.id.equals(OfflineService.savingId);
        return now ? getString(R.string.saving_state, it.percent()) : getString(R.string.waiting);
    }

    private View row(Saved.Item it) {
        LinearLayout row = new LinearLayout(this);
        row.setTag(it.id);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(10), 0, dp(10));
        ImageView pic = new ImageView(this);
        pic.setScaleType(ImageView.ScaleType.CENTER_CROP);
        pic.setBackgroundColor(Color.parseColor("#2f7362")); /* (no poster: Hemma's green) */
        File pf = it.posterFile.isEmpty() ? null : Offline.file(this, it, it.posterFile);
        if (pf != null && pf.isFile()) {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inSampleSize = 2;
            Bitmap bm = BitmapFactory.decodeFile(pf.getPath(), o);
            if (bm != null) pic.setImageBitmap(bm);
        }
        pic.setContentDescription(it.title);
        row.addView(pic, new LinearLayout.LayoutParams(dp(64), dp(96)));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.setPadding(dp(14), 0, 0, 0);
        TextView t = new TextView(this);
        t.setText(it.title);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        text.addView(t);
        if (!it.sub().isEmpty()) {
            TextView s = new TextView(this);
            s.setText(it.sub());
            text.addView(s);
        }
        TextView st = new TextView(this);
        st.setTag("state");
        st.setAlpha(0.8f);
        if (it.state.equals("ready")) st.setText(Formatter.formatShortFileSize(this, Offline.used(this, it)));
        else if (it.state.equals("failed")) st.setText(getString(R.string.failed, why(it.why)));
        else st.setText(saving(it));
        text.addView(st);
        if (it.state.equals("saving")) {
            ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            bar.setTag("bar");
            bar.setMax(100);
            bar.setProgress(it.percent());
            text.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        LinearLayout buttons = new LinearLayout(this);
        buttons.setGravity(Gravity.CENTER_VERTICAL);
        if (it.state.equals("ready")) buttons.addView(button(R.string.play, v -> play(it)));
        if (it.state.equals("failed")) buttons.addView(button(R.string.try_again, v -> {
            OfflineService.save(this, it.manifest);
            draw(true);
        }));
        buttons.addView(button(R.string.delete, v -> delete(it)));
        text.addView(buttons);
        row.addView(text, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    private Button button(int res, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(res);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    private String why(String code) {
        switch (code) {
            case "room": return getString(R.string.why_room);
            case "forbidden": return getString(R.string.not_for_you);
            case "gone": return getString(R.string.gone);
            case "signin": return getString(R.string.sign_in);
            default: return getString(R.string.network_failed);
        }
    }

    private void play(Saved.Item it) {
        startActivity(new Intent(this, it.book() ? BookActivity.class : WatchActivity.class).putExtra("id", it.id));
    }

    private void delete(Saved.Item it) {
        new AlertDialog.Builder(this)
            .setMessage(getString(R.string.delete_q, it.title))
            .setPositiveButton(R.string.delete, (d, w) -> {
                if (PlayerService.instance != null && it.id.equals(PlayerService.instance.localId())) PlayerService.instance.finish();
                Offline.delete(this, it.id);
                draw(true);
            })
            .setNegativeButton(R.string.cancel, null)
            .show();
    }
}
