/* An audiobook playing with the phone locked (2.0). A WebView plays the book (Hemma's own page and player, wire.js
 * "the listener"), but shows nothing of the Media Session API on the lock screen - and Android stops an app in the
 * background. So while a book is loaded this service runs in the foreground: a media notification and a
 * MediaSession (the lock screen, the quick settings' player, a headset's buttons) with the book's cover, the
 * chapter, play/pause, 15 s back, 30 s forward and the chapters. What plays comes from the page
 * (window.HemmaApp.media(json) - {"state": "playing" | "paused" | "none", title, artist, album, art, duration,
 * position, rate}); the buttons go back to it (MainActivity.action -> window.hemmaAction). "none" (another page
 * came) ends it.
 * OFFLINE (2.3): a book saved on the phone plays HERE - a MediaPlayer over its parts in order, the place always in the
 * WHOLE book (as wire.js's book() keeps it), chapters for next/previous, 15 s back and 30 s forward - with the same
 * session, notification and lock screen (playLocal; BookActivity shows it). The page's reports are left alone while
 * a saved book plays, and the place is kept (Offline.place) every 15 s, at a pause and at the end. */
package io.github.kloparn.hemma;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.MediaPlayer;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.webkit.CookieManager;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import org.json.JSONObject;

public final class PlayerService extends Service {
    static PlayerService instance;
    private static JSONObject pending;
    private static String pendingLocal; /* a saved book to play (playLocal) */
    private static final String CHANNEL = "listening";
    private static final int ID = 1;

    private final Handler main = new Handler(Looper.getMainLooper());
    private MediaSession session;
    private PowerManager.WakeLock wake;
    private JSONObject now;
    private String artUrl;
    private Bitmap art;
    /* a saved book playing here (2.3) */
    private Saved.Item book;
    private double[] lens;
    private MediaPlayer mp;
    private int part = -1;
    private long kept, told;
    private AudioFocusRequest focus;
    private final BroadcastReceiver noisy = new BroadcastReceiver() { /* headphones out: pause, as players do */
        @Override public void onReceive(Context c, Intent i) {
            if (book != null) localPause();
        }
    };
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (book == null) return;
            long t = SystemClock.elapsedRealtime();
            if (playingLocal() && t - kept >= 15000) keep(false);
            if (t - told >= 4000) publish();
            main.postDelayed(this, 1000);
        }
    };

    /** From the page, on the main thread: start, update or end */
    static void update(MainActivity a, String json) {
        if (instance != null && instance.book != null) return; /* (a saved book plays: the page's is paused) */
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
        if (pendingLocal != null) {
            String id = pendingLocal;
            pendingLocal = null;
            pending = null;
            startLocal(id);
        } else if (pending != null) {
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
        stopLocal();
        if (wake.isHeld()) wake.release();
        session.release();
        super.onDestroy();
    }

    void finish() {
        if (instance == this) instance = null;
        stopLocal();
        now = null;
        if (wake.isHeld()) wake.release();
        session.setActive(false);
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        else stopForeground(true);
        stopSelf();
    }

    private void page(String what, double arg) {
        if (book != null) { /* a saved book: it plays here */
            local(what, arg);
            return;
        }
        MainActivity a = MainActivity.current.get();
        if (a != null) a.action(what, arg);
    }

    private PendingIntent openApp() {
        Intent i = book != null ? new Intent(this, BookActivity.class).putExtra("id", book.id)
            : new Intent(this, MainActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, book != null ? 1 : 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    /* ------------------------------------------------------------------ a saved book (2.3) */

    /** BookActivity: play the saved book `id` here (from where it was left) */
    static void playLocal(Context c, String id) {
        if (instance != null && id.equals(instance.localId())) {
            instance.localPlay();
            return;
        }
        pendingLocal = id;
        Intent i = new Intent(c, PlayerService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
        else c.startService(i);
    }

    String localId() { return book != null ? book.id : null; }

    boolean playingLocal() { return mp != null && mp.isPlaying(); }

    /** Where the saved book is, in seconds of the whole book */
    double localPlace() {
        if (book == null || part < 0) return 0;
        return Saved.partStart(lens, part) + (mp != null ? mp.getCurrentPosition() / 1000.0 : 0);
    }

    Saved.Item localBook() { return book; }

    private void startLocal(String id) {
        stopLocal();
        Saved.Item it = Offline.get(this, id);
        if (it == null || !it.state.equals("ready") || !it.book()) {
            if (now == null) { /* (started in the foreground: it has to say so once before it goes) */
                apply(new JSONObject());
                finish();
            }
            return;
        }
        MainActivity a = MainActivity.current.get();
        if (a != null && now != null) a.action("pause", Double.NaN); /* (the page's book, if one played) */
        now = null;
        book = it;
        lens = it.lengths();
        File cover = it.posterFile.isEmpty() ? null : Offline.file(this, it, it.posterFile);
        art = cover != null && cover.isFile() ? BitmapFactory.decodeFile(cover.getPath()) : null;
        artUrl = ""; /* (publish() says "": apply() then keeps this cover) */
        registerReceiver(noisy, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
        publish(); /* (in the foreground at once, paused, whatever comes next) */
        seekLocal(it.start(), true);
        main.removeCallbacks(tick);
        main.post(tick);
    }

    private void stopLocal() {
        if (book == null) return;
        keep(false);
        main.removeCallbacks(tick);
        try {
            unregisterReceiver(noisy);
        } catch (IllegalArgumentException ignored) {
        }
        if (mp != null) mp.release();
        mp = null;
        part = -1;
        book = null;
        letFocusGo();
    }

    /* The whole book's second t: its part opened (when another), the place in it, playing or not */
    private void seekLocal(double t, boolean play) {
        if (book == null) return;
        double len = book.length();
        t = Math.max(0, len > 0 ? Math.min(t, len - 0.5) : t);
        int p = Saved.partAt(lens, t);
        if (p != part || mp == null) {
            boolean was = play || playingLocal();
            if (!openPart(p)) return;
            play = was;
        }
        mp.seekTo((int) ((t - Saved.partStart(lens, p)) * 1000));
        if (play) localPlay();
        else publish();
    }

    private boolean openPart(int p) {
        if (mp != null) mp.release();
        mp = null;
        try {
            MediaPlayer m = new MediaPlayer();
            m.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build());
            m.setWakeMode(this, PowerManager.PARTIAL_WAKE_LOCK);
            m.setDataSource(Offline.file(this, book, book.files.get(p).file).getPath());
            m.prepare();
            m.setOnCompletionListener(x -> {
                if (part + 1 < book.files.size()) seekLocal(Saved.partStart(lens, part + 1), true);
                else { /* the end of the book */
                    localPause();
                    keep(true);
                }
            });
            mp = m;
            part = p;
            return true;
        } catch (IOException | RuntimeException e) {
            mp = null;
            part = -1;
            return false;
        }
    }

    private void localPlay() {
        if (mp == null) return;
        if (!holdFocus()) return;
        mp.start();
        publish();
    }

    private void localPause() {
        if (mp == null) return;
        if (mp.isPlaying()) mp.pause();
        keep(false);
        letFocusGo();
        publish();
    }

    /* The page's actions (wire.js lockActs' names) on the saved book */
    void local(String what, double arg) {
        if (book == null) return;
        double at = localPlace();
        switch (what) {
            case "play": localResume(); break;
            case "pause": case "stop": localPause(); break;
            case "seekbackward": seekLocal(at - 15, playingLocal()); break;
            case "seekforward": seekLocal(at + 30, playingLocal()); break;
            case "seekto": if (!Double.isNaN(arg)) seekLocal(arg, playingLocal()); break;
            case "nexttrack": {
                int c = Saved.chapterAt(book.chapters, at);
                if (c + 1 < book.chapters.size()) seekLocal(book.chapters.get(c + 1).t, playingLocal());
                break;
            }
            case "previoustrack": {
                int c = Saved.chapterAt(book.chapters, at);
                if (c < 0) seekLocal(0, playingLocal());
                else if (at - book.chapters.get(c).t > 3 || c == 0) seekLocal(book.chapters.get(c).t, playingLocal());
                else seekLocal(book.chapters.get(c - 1).t, playingLocal());
                break;
            }
            default:
        }
    }

    /** BookActivity's buttons: toggle, back, fwd, or a chapter's second */
    void localToggle() {
        if (playingLocal()) localPause();
        else localResume();
    }

    /* Play - from the top again once the book was heard to its end */
    private void localResume() {
        if (book != null && book.length() > 0 && localPlace() >= book.length() - 1) seekLocal(0, true);
        else localPlay();
    }

    void localSeek(double t) { seekLocal(t, playingLocal()); }

    private void keep(boolean ended) {
        if (book == null) return;
        kept = SystemClock.elapsedRealtime();
        double len = book.length();
        Offline.place(this, book.id, ended ? len : localPlace(), len, ended);
    }

    /* What plays, to the session and the notification - as the page's reports are */
    private void publish() {
        if (book == null) return;
        told = SystemClock.elapsedRealtime();
        double at = localPlace();
        int c = Saved.chapterAt(book.chapters, at);
        JSONObject j = new JSONObject();
        try {
            j.put("state", playingLocal() ? "playing" : "paused");
            j.put("title", c >= 0 && !book.chapters.get(c).title.isEmpty() ? book.chapters.get(c).title : book.title);
            j.put("artist", book.author);
            j.put("album", book.title);
            j.put("art", "");
            j.put("duration", book.length());
            j.put("position", at);
            j.put("rate", 1);
        } catch (Exception ignored) {
        }
        apply(j);
    }

    private boolean holdFocus() {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (Build.VERSION.SDK_INT < 26) return true;
        if (focus == null)
            focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setOnAudioFocusChangeListener(change -> {
                    if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) localPause();
                }, main)
                .build();
        return am.requestAudioFocus(focus) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    private void letFocusGo() {
        if (focus != null && Build.VERSION.SDK_INT >= 26) ((AudioManager) getSystemService(AUDIO_SERVICE)).abandonAudioFocusRequest(focus);
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
