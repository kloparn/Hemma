/* A saved audiobook, offline (2.3): its cover, title, author, the chapter playing and where in the book, play/pause,
 * 15 s back, 30 s forward and its chapters to jump to. PlayerService plays it (the lock screen and the notification
 * as for Hemma's own page); this screen only shows it and sends it the buttons. */
package io.github.kloparn.hemma;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.format.DateUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;

public final class BookActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private Saved.Item it;
    private TextView chapter, place;
    private Button toggle;
    private LinearLayout chapters;
    private int lit = -2;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            show();
            main.postDelayed(this, 1000);
        }
    };

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private PlayerService player() {
        PlayerService p = PlayerService.instance;
        return p != null && it != null && it.id.equals(p.localId()) ? p : null;
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        it = Offline.get(this, getIntent().getStringExtra("id"));
        if (it == null || !it.state.equals("ready") || !it.book()) {
            Toast.makeText(this, R.string.gone_here, Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        boolean night = (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        box.setPadding(dp(24), dp(28), dp(24), dp(24));
        ImageView cover = new ImageView(this);
        cover.setScaleType(ImageView.ScaleType.FIT_CENTER);
        File cf = it.posterFile.isEmpty() ? null : Offline.file(this, it, it.posterFile);
        if (cf != null && cf.isFile()) cover.setImageBitmap(BitmapFactory.decodeFile(cf.getPath()));
        else cover.setBackgroundColor(Color.parseColor("#2f7362"));
        cover.setContentDescription(it.title);
        box.addView(cover, new LinearLayout.LayoutParams(dp(220), dp(220)));
        TextView title = new TextView(this);
        title.setText(it.title);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(16), 0, 0);
        box.addView(title);
        if (!it.author.isEmpty()) {
            TextView a = new TextView(this);
            a.setText(it.author);
            a.setGravity(Gravity.CENTER);
            box.addView(a);
        }
        chapter = new TextView(this);
        chapter.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        chapter.setGravity(Gravity.CENTER);
        chapter.setPadding(0, dp(14), 0, 0);
        box.addView(chapter);
        place = new TextView(this);
        place.setGravity(Gravity.CENTER);
        place.setAlpha(0.8f);
        box.addView(place);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, dp(12), 0, dp(12));
        row.addView(button(getString(R.string.back15_short), () -> act("seekbackward")));
        toggle = button(getString(R.string.play), () -> {
            PlayerService p = player();
            if (p != null) p.localToggle();
            else PlayerService.playLocal(this, it.id);
            main.postDelayed(this::show, 150);
        });
        row.addView(toggle);
        row.addView(button(getString(R.string.fwd30_short), () -> act("seekforward")));
        box.addView(row);
        if (!it.chapters.isEmpty()) {
            TextView h = new TextView(this);
            h.setText(R.string.chapters);
            h.setTextSize(TypedValue.COMPLEX_UNIT_SP, 19);
            h.setTypeface(h.getTypeface(), android.graphics.Typeface.BOLD);
            h.setPadding(0, dp(12), 0, dp(4));
            box.addView(h, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            chapters = new LinearLayout(this);
            chapters.setOrientation(LinearLayout.VERTICAL);
            for (int i = 0; i < it.chapters.size(); i++) {
                Saved.Chapter c = it.chapters.get(i);
                Button b = new Button(this);
                b.setAllCaps(false);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setText((c.title.isEmpty() ? getString(R.string.chapter_n, i + 1) : c.title) + "  ·  " + clock(c.t));
                b.setOnClickListener(v -> {
                    PlayerService p = player();
                    if (p != null) p.localSeek(c.t);
                });
                chapters.addView(b, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            }
            box.addView(chapters, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);
        scroll.setBackgroundColor(night ? Color.parseColor("#121316") : Color.parseColor("#f4f1ea"));
        setContentView(scroll);
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            scroll.setOnApplyWindowInsetsListener((v, ins) -> {
                Insets s = ins.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                v.setPadding(s.left, s.top, s.right, s.bottom);
                return WindowInsets.CONSUMED;
            });
        }
        if (player() == null && saved == null) PlayerService.playLocal(this, it.id); /* (Play on the Offline screen) */
    }

    private Button button(String label, Runnable r) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(v -> r.run());
        return b;
    }

    private void act(String what) {
        PlayerService p = player();
        if (p != null) p.local(what, Double.NaN);
        main.postDelayed(this::show, 150);
    }

    private static String clock(double s) { return DateUtils.formatElapsedTime((long) Math.max(0, s)); }

    private void show() {
        PlayerService p = player();
        double at = p != null ? p.localPlace() : it.start();
        boolean playing = p != null && p.playingLocal();
        int c = Saved.chapterAt(it.chapters, at);
        chapter.setText(c >= 0 ? (it.chapters.get(c).title.isEmpty() ? getString(R.string.chapter_n, c + 1) : it.chapters.get(c).title) : "");
        place.setText(clock(at) + " / " + clock(it.length()));
        toggle.setText(playing ? R.string.pause : R.string.play);
        if (chapters != null && c != lit) { /* the chapter playing stands out */
            for (int i = 0; i < chapters.getChildCount(); i++) chapters.getChildAt(i).setAlpha(i == c ? 1f : 0.7f);
            lit = c;
        }
    }

    @Override protected void onResume() {
        super.onResume();
        if (it == null) return;
        show();
        main.postDelayed(tick, 1000);
    }

    @Override protected void onPause() {
        super.onPause();
        main.removeCallbacks(tick);
    }
}
