/* An audiobook playing with the phone locked (2.0). A WebView plays the book (Hemma's own page and player, wire.js
 * "the listener"), but shows nothing of the Media Session API on the lock screen - and Android stops an app in the
 * background. So while a book is loaded this service runs in the foreground: a media notification and a
 * MediaSession (the lock screen, the quick settings' player, a headset's buttons) with the book's cover, the
 * chapter, play/pause, 15 s back, 30 s forward and the chapters. What plays comes from the page
 * (window.HemmaApp.media(json) - {"state": "playing" | "paused" | "none", title, artist, album, art, duration,
 * position, rate}); the buttons go back to it (MainActivity.action -> window.hemmaAction). "none" (another page
 * came) ends it. */
package io.github.kloparn.hemma;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.webkit.CookieManager;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import org.json.JSONObject;

public final class PlayerService extends Service {
    static PlayerService instance;
    private static JSONObject pending;
    private static final String CHANNEL = "listening";
    private static final int ID = 1;

    private final Handler main = new Handler(Looper.getMainLooper());
    private MediaSession session;
    private PowerManager.WakeLock wake;
    private JSONObject now;
    private String artUrl;
    private Bitmap art;

    /** From the page, on the main thread: start, update or end */
    static void update(MainActivity a, String json) {
        JSONObject j;
        try {
            j = new JSONObject(json);
        } catch (Exception e) {
            return;
        }
        String state = j.optString("state");
        a.listening = state.equals("playing");
        if (state.equals("none")) {
            if (instance != null) instance.finish();
            pending = null;
            return;
        }
        if (instance != null) {
            instance.apply(j);
            return;
        }
        if (!state.equals("playing")) return; /* (it starts with playing: a paused book isn't worth a notification) */
        pending = j;
        Intent i = new Intent(a, PlayerService.class);
        if (Build.VERSION.SDK_INT >= 26) a.startForegroundService(i);
        else a.startService(i);
    }

    @Override public void onCreate() {
        super.onCreate();
        instance = this;
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.playing), NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
        session = new MediaSession(this, "Hemma");
        session.setSessionActivity(openApp());
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { page("play", Double.NaN); }
            @Override public void onPause() { page("pause", Double.NaN); }
            @Override public void onStop() { page("pause", Double.NaN); }
            @Override public void onSkipToNext() { page("nexttrack", Double.NaN); }
            @Override public void onSkipToPrevious() { page("previoustrack", Double.NaN); }
            @Override public void onFastForward() { page("seekforward", Double.NaN); }
            @Override public void onRewind() { page("seekbackward", Double.NaN); }
            @Override public void onSeekTo(long ms) { page("seekto", ms / 1000.0); }
            @Override public void onCustomAction(String action, android.os.Bundle extras) {
                page(action.equals("back15") ? "seekbackward" : "seekforward", Double.NaN);
            }
        });
        session.setActive(true);
        wake = ((PowerManager) getSystemService(POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "hemma:listening");
        wake.setReferenceCounted(false);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        String act = intent == null ? null : intent.getAction();
        if (act != null) { /* the notification's own buttons */
            if (act.equals("toggle")) page(now != null && "playing".equals(now.optString("state")) ? "pause" : "play", Double.NaN);
            else if (act.equals("back")) page("seekbackward", Double.NaN);
            else if (act.equals("fwd")) page("seekforward", Double.NaN);
        }
        if (pending != null) {
            apply(pending);
            pending = null;
        } else if (now == null) { /* (nothing to show: started again by the system) */
            finish();
        }
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onTaskRemoved(Intent root) { /* the app swiped away: its page - and the book - are gone */
        finish();
    }

    @Override public void onDestroy() {
        if (instance == this) instance = null;
        if (wake.isHeld()) wake.release();
        session.release();
        super.onDestroy();
    }

    void finish() {
        if (instance == this) instance = null;
        now = null;
        if (wake.isHeld()) wake.release();
        session.setActive(false);
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        else stopForeground(true);
        stopSelf();
    }

    private void page(String what, double arg) {
        MainActivity a = MainActivity.current.get();
        if (a != null) a.action(what, arg);
    }

    private PendingIntent openApp() {
        Intent i = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private PendingIntent button(String act, int code) {
        Intent i = new Intent(this, PlayerService.class).setAction(act);
        return PendingIntent.getService(this, code, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    /* What plays now: the session's state and metadata, the notification, the wake lock */
    void apply(JSONObject j) {
        now = j;
        boolean playing = "playing".equals(j.optString("state"));
        long pos = Math.round(j.optDouble("position", 0) * 1000), dur = Math.round(j.optDouble("duration", 0) * 1000);
        float rate = (float) j.optDouble("rate", 1);
        PlaybackState.Builder st = new PlaybackState.Builder()
            .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
                | PlaybackState.ACTION_SEEK_TO | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                | PlaybackState.ACTION_REWIND | PlaybackState.ACTION_FAST_FORWARD | PlaybackState.ACTION_STOP)
            .addCustomAction(new PlaybackState.CustomAction.Builder("back15", getString(R.string.back15),
                android.R.drawable.ic_media_rew).build())
            .addCustomAction(new PlaybackState.CustomAction.Builder("fwd30", getString(R.string.fwd30),
                android.R.drawable.ic_media_ff).build())
            .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED, pos, playing ? rate : 0f,
                SystemClock.elapsedRealtime());
        session.setPlaybackState(st.build());
        String url = j.optString("art", "");
        if (!url.equals(artUrl)) {
            artUrl = url;
            art = null;
            if (!url.isEmpty()) loadArt(url);
        }
        meta(j, dur);
        if (playing) wake.acquire(6 * 3600 * 1000L); /* (let go at a pause; a bound in case the page never says) */
        else if (wake.isHeld()) wake.release();
        Notification n = notification(j, playing);
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        else startForeground(ID, n);
    }

    private void meta(JSONObject j, long dur) {
        MediaMetadata.Builder m = new MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, j.optString("title"))
            .putString(MediaMetadata.METADATA_KEY_ARTIST, j.optString("artist"))
            .putString(MediaMetadata.METADATA_KEY_ALBUM, j.optString("album"))
            .putLong(MediaMetadata.METADATA_KEY_DURATION, dur);
        if (art != null) m.putBitmap(MediaMetadata.METADATA_KEY_ART, art);
        session.setMetadata(m.build());
    }

    private Notification notification(JSONObject j, boolean playing) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        b.setSmallIcon(R.drawable.ic_note)
            .setContentTitle(j.optString("title"))
            .setContentText(j.optString("album"))
            .setSubText(j.optString("artist"))
            .setContentIntent(openApp())
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(playing)
            .setShowWhen(false)
            .addAction(new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_rew),
                getString(R.string.back15), button("back", 1)).build())
            .addAction(new Notification.Action.Builder(Icon.createWithResource(this,
                playing ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play),
                getString(playing ? R.string.pause : R.string.play), button("toggle", 2)).build())
            .addAction(new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_ff),
                getString(R.string.fwd30), button("fwd", 3)).build())
            .setStyle(new Notification.MediaStyle().setMediaSession(session.getSessionToken()).setShowActionsInCompactView(0, 1, 2));
        if (art != null) b.setLargeIcon(art);
        return b.build();
    }

    /* The cover, from Hemma (with the sign-in's cookie), no bigger than 512 px */
    private void loadArt(String url) {
        final String want = url;
        new Thread(() -> {
            Bitmap bm = null;
            try {
                URL u = new URL(url);
                if (!Net.isPrivate(InetAddress.getByName(u.getHost()).getAddress())) return;
                HttpURLConnection c = (HttpURLConnection) u.openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(8000);
                String cookie = CookieManager.getInstance().getCookie(url);
                if (cookie != null) c.setRequestProperty("Cookie", cookie);
                try (InputStream in = c.getInputStream()) {
                    bm = BitmapFactory.decodeStream(in);
                } finally {
                    c.disconnect();
                }
                if (bm != null && Math.max(bm.getWidth(), bm.getHeight()) > 512) {
                    float k = 512f / Math.max(bm.getWidth(), bm.getHeight());
                    bm = Bitmap.createScaledBitmap(bm, Math.round(bm.getWidth() * k), Math.round(bm.getHeight() * k), true);
                }
            } catch (Exception ignored) {
            }
            final Bitmap got = bm;
            main.post(() -> {
                if (got == null || !want.equals(artUrl) || now == null || instance != this) return;
                art = got;
                apply(now);
            });
        }).start();
    }
}
