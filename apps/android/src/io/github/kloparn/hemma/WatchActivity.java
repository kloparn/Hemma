/* A saved video, played offline (2.3): the phone's own player (VideoView + MediaController) full screen, turned
 * with the phone, from where it was left offline (else where Hemma had it when it was saved). Its subtitles are
 * VideoView's own WebVTT tracks (addSubtitleSource); the one Hemma had on is shown. The place is kept every 15 s, at
 * a pause and at the end (Offline.place) - and sent to Hemma when the app reaches it again. */
package io.github.kloparn.hemma;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.MediaController;
import android.widget.Toast;
import android.widget.VideoView;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public final class WatchActivity extends Activity {
    private final Handler main = new Handler(Looper.getMainLooper());
    private VideoView video;
    private Saved.Item it;
    private final List<InputStream> subs = new ArrayList<>();
    private int at; /* where to go on from (ms) when it comes back from the background */
    private final Runnable keep = new Runnable() {
        @Override public void run() {
            if (video.isPlaying()) save(false);
            main.postDelayed(this, 15000);
        }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        it = Offline.get(this, getIntent().getStringExtra("id"));
        File f = it != null && it.state.equals("ready") ? Offline.file(this, it, it.files.get(0).file) : null;
        if (f == null || !f.isFile()) {
            Toast.makeText(this, R.string.gone_here, Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        video = new VideoView(this);
        root.addView(video, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT,
            Gravity.CENTER));
        setContentView(root);
        bars();
        MediaController mc = new MediaController(this) { /* (its own window takes Back to hide itself: Back leaves) */
            @Override public boolean dispatchKeyEvent(KeyEvent e) {
                if (e.getKeyCode() != KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(e);
                if (e.getAction() == KeyEvent.ACTION_UP) finish();
                return true;
            }
        };
        mc.setAnchorView(video);
        video.setMediaController(mc);
        video.setVideoPath(f.getPath());
        int on = -1;
        for (Saved.Sub s : it.subs) { /* (VideoView keeps them until it's prepared) */
            File sf = Offline.file(this, it, s.file);
            if (!sf.isFile()) continue;
            try {
                InputStream in = new FileInputStream(sf);
                subs.add(in);
                video.addSubtitleSource(in, MediaFormat.createSubtitleFormat("text/vtt", s.lang.isEmpty() ? "und" : s.lang));
                if (s.on && on < 0) on = subs.size() - 1;
            } catch (IOException ignored) {
            }
        }
        final int show = on;
        at = (int) (it.start() * 1000);
        video.setOnPreparedListener(mp -> {
            subtitle(mp, show);
            if (at > 0) video.seekTo(at);
            video.start();
            main.removeCallbacks(keep);
            main.postDelayed(keep, 15000);
        });
        video.setOnCompletionListener(mp -> {
            save(true);
            finish();
        });
        video.setOnErrorListener((mp, what, extra) -> {
            Toast.makeText(this, R.string.wont_play, Toast.LENGTH_LONG).show();
            finish();
            return true;
        });
    }

    /* The subtitle Hemma had on: the n-th of the subtitle tracks VideoView added (they come after the file's own) */
    private void subtitle(MediaPlayer mp, int n) {
        if (n < 0) return;
        try {
            MediaPlayer.TrackInfo[] ti = mp.getTrackInfo();
            int seen = 0;
            for (int i = 0; i < ti.length; i++) {
                if (ti[i].getTrackType() != MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_SUBTITLE) continue;
                if (seen++ == n) {
                    mp.selectTrack(i);
                    return;
                }
            }
        } catch (RuntimeException ignored) { /* (no subtitles then: the picture still plays) */
        }
    }

    private void bars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private void save(boolean ended) {
        if (video == null || it == null) return;
        int dur = video.getDuration();
        double pos = ended ? dur / 1000.0 : video.getCurrentPosition() / 1000.0;
        Offline.place(this, it.id, pos, dur > 0 ? dur / 1000.0 : it.length(), ended);
    }

    @Override protected void onPause() {
        super.onPause();
        if (video == null) return;
        main.removeCallbacks(keep);
        if (video.getCurrentPosition() > 0) {
            at = video.getCurrentPosition();
            save(false);
        }
        video.pause();
    }

    @Override protected void onResume() {
        super.onResume();
        if (video != null && at > 0 && !isFinishing()) bars();
    }

    @Override protected void onDestroy() {
        main.removeCallbacks(keep);
        for (InputStream in : subs) {
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
        if (video != null) video.stopPlayback();
        super.onDestroy();
    }
}
