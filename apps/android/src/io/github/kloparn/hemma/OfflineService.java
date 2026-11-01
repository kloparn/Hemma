/* Saving for offline (2.3): a page's "Save for offline" (hemma-app:save?m=<manifest>) lands here. In the foreground
 * (dataSync: a notification with the progress), one file at a time, it fetches Hemma's manifest (a season's is a list
 * of them), then each thing's poster, subtitles and files into Offline's folder - with the app's own HTTP, not
 * Android's DownloadManager: that runs in another process, which Android 17 may not let reach the home network, and it
 * needs the sign-in's cookie. A file comes as <name>.part and goes on from there (Range) after a dropped connection
 * or a restart; the app looks again for unfinished ones whenever it reaches Hemma (resume()). */
package io.github.kloparn.hemma;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.text.format.Formatter;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Map;

public final class OfflineService extends Service {
    private static final String CHANNEL = "saving";
    private static final int ID = 2, DONE_ID = 3;
    private static final long MARGIN = 200L << 20; /* the phone keeps this much free besides */
    private static final ArrayDeque<String> queue = new ArrayDeque<>();
    private static volatile String busy; /* the manifest being saved now */
    static volatile String savingId;     /* the thing whose files come now (OfflineActivity's bar) */

    private final Handler main = new Handler(Looper.getMainLooper());
    private Thread worker;
    private int saved, failed;
    private String lastTitle = "";
    private long shown;

    /** A page's Save for offline: the manifest's path (Saved.savePath checked it) */
    static void save(Context c, String path) {
        if (add(path)) start(c);
    }

    /** Hemma reached again: whatever was left half-saved goes on (a failed one waits for its Try again) */
    static void resume(Context c) {
        boolean any = false;
        for (Saved.Item it : Offline.all(c))
            if (it.state.equals("saving") && !it.manifest.isEmpty()) any |= add(it.manifest);
        if (any) start(c);
    }

    private static boolean add(String path) {
        synchronized (queue) {
            if (path.equals(busy) || queue.contains(path)) return false;
            queue.add(path);
            return true;
        }
    }

    private static void start(Context c) {
        Intent i = new Intent(c, OfflineService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
        else c.startService(i);
    }

    static boolean waiting(String manifest) {
        synchronized (queue) {
            return queue.contains(manifest);
        }
    }

    @Override public void onCreate() {
        super.onCreate();
        NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.saving_channel), NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = note(getString(R.string.saving_start), -1, true);
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else startForeground(ID, n);
        if (worker == null || !worker.isAlive()) {
            worker = new Thread(this::work, "hemma-offline");
            worker.start();
        }
        return START_NOT_STICKY;
    }

    private void work() {
        saved = failed = 0;
        for (;;) {
            String path;
            synchronized (queue) {
                path = queue.poll();
                busy = path;
            }
            if (path == null) break;
            try {
                manifest(path);
            } catch (Exception e) { /* (never the whole queue for one thing) */
                failed++;
            }
        }
        busy = savingId = null;
        main.post(this::done);
    }

    private void done() {
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        else stopForeground(true);
        if (saved + failed > 0) { /* what came of it, where it stays seen */
            String say = failed > 0 ? getString(R.string.save_failed_note, lastTitle)
                : saved == 1 ? getString(R.string.saved_note, lastTitle) : getResources().getQuantityString(R.plurals.saved_n, saved, saved);
            getSystemService(NotificationManager.class).notify(DONE_ID, note(say, -1, false));
        }
        stopSelf();
    }

    private String base() { return Offline.base(this); }

    private String agent() { return Offline.agent(this); }

    private void toast(int res, Object... args) {
        String s = getString(res, args);
        main.post(() -> Toast.makeText(this, s, Toast.LENGTH_LONG).show());
    }

    private static int refusal(IOException e) {
        return e instanceof Offline.Refused ? ((Offline.Refused) e).code : 0;
    }

    /* A manifest: a thing, or a list of them (a season) */
    private void manifest(String path) {
        String base = base(), json;
        try {
            json = Offline.text(base, path, agent());
        } catch (IOException e) {
            int code = refusal(e);
            toast(code == 403 ? R.string.not_for_you : code == 404 ? R.string.gone : code == 401 ? R.string.sign_in : R.string.network_failed);
            failed++;
            return;
        }
        Object o;
        try {
            o = Saved.parse(json);
        } catch (IllegalArgumentException e) {
            failed++;
            return;
        }
        if (!(o instanceof Map)) return;
        Map<?, ?> m = (Map<?, ?>) o;
        if ("list".equals(Saved.str(m, "kind"))) {
            List<?> items = Saved.list(m, "items");
            toast(R.string.saving_for_offline, Saved.str(m, "title"));
            for (Object p : items) /* (each its own manifest: a child's list holds only what's theirs, Hemma says) */
                if (p instanceof String && Saved.savePath("save?m=" + p) != null) {
                    try {
                        Saved.Item it = Saved.Item.fromManifest((Map<?, ?>) Saved.parse(Offline.text(base, (String) p, agent())));
                        if (it != null) item(it, (String) p, false);
                    } catch (IOException | IllegalArgumentException | ClassCastException e) {
                        failed++;
                    }
                }
            return;
        }
        Saved.Item it = Saved.Item.fromManifest(m);
        if (it == null) {
            failed++;
            return;
        }
        item(it, path, true);
    }

    /* One thing: everything it has, then ready */
    private void item(Saved.Item fresh, String path, boolean say) {
        Offline.deleted.remove(fresh.id);
        Saved.Item had = Offline.get(this, fresh.id), it = fresh;
        if (had != null && had.state.equals("ready")) {
            if (say) toast(R.string.already_saved, had.title);
            return;
        }
        if (had != null) { /* half-saved before: go on with what came, Hemma's newest words for the rest */
            for (int i = 0; i < it.files.size() && i < had.files.size(); i++)
                if (it.files.get(i).bytes == had.files.get(i).bytes) it.files.get(i).done = had.files.get(i).done;
            it.pos = had.pos;
            it.dur = had.dur;
            it.ended = had.ended;
            it.unsent = had.unsent;
            it.placeAt = had.placeAt;
            it.savedAt = had.savedAt;
        }
        if (it.savedAt == 0) it.savedAt = System.currentTimeMillis();
        it.manifest = path;
        it.state = "saving";
        it.why = "";
        lastTitle = it.title;
        savingId = it.id;
        long need = it.bytes() - it.done();
        if (Offline.free(this) < need + MARGIN) {
            fail(it, "room");
            toast(R.string.no_room, Formatter.formatShortFileSize(this, need + MARGIN - Offline.free(this)));
            return;
        }
        Offline.put(this, it);
        if (say) toast(R.string.saving_for_offline, it.title);
        String base = base();
        try {
            if (!it.poster.isEmpty() && it.posterFile.isEmpty()) poster(base, it);
            for (Saved.Sub s : it.subs) small(base, s.url, Offline.file(this, it, s.file));
            for (Saved.Part p : it.files) {
                part(base, it, p);
                Offline.put(this, it);
            }
        } catch (IOException e) {
            int code = refusal(e);
            if (code == 410 || Offline.deleted.contains(it.id)) return; /* (deleted while it saved: nothing to say) */
            fail(it, code == 403 ? "forbidden" : code == 404 ? "gone" : code == 401 ? "signin" : "network");
            return;
        }
        if (it.book()) measure(it);
        it.state = "ready";
        Offline.put(this, it);
        saved++;
    }

    private void fail(Saved.Item it, String why) {
        it.state = "failed";
        it.why = why;
        Offline.put(this, it);
        failed++;
    }

    /* A poster, no bigger than 512 px, as a JPEG */
    private void poster(String base, Saved.Item it) {
        File tmp = Offline.file(this, it, "poster.part");
        try {
            small(base, it.poster, tmp);
            Bitmap bm = BitmapFactory.decodeFile(tmp.getPath());
            if (bm == null) return;
            if (Math.max(bm.getWidth(), bm.getHeight()) > 512) {
                float k = 512f / Math.max(bm.getWidth(), bm.getHeight());
                bm = Bitmap.createScaledBitmap(bm, Math.round(bm.getWidth() * k), Math.round(bm.getHeight() * k), true);
            }
            try (OutputStream o = new FileOutputStream(Offline.file(this, it, "poster.jpg"))) {
                bm.compress(Bitmap.CompressFormat.JPEG, 85, o);
            }
            it.posterFile = "poster.jpg";
        } catch (IOException ignored) { /* (no poster: the list draws its title) */
        } finally {
            tmp.delete();
        }
    }

    /* A small file whole (a subtitle, a poster) */
    private void small(String base, String path, File to) throws IOException {
        HttpURLConnection h = Offline.open(base, path, "GET", agent());
        try {
            int code = h.getResponseCode();
            if (code != 200) throw new Offline.Refused(code);
            if (Offline.signInPage(h)) throw new Offline.Refused(401);
            try (InputStream in = h.getInputStream(); OutputStream o = new FileOutputStream(to)) {
                byte[] buf = new byte[32768];
                long n = 0;
                for (int k; (k = in.read(buf)) > 0;) {
                    if ((n += k) > 64L << 20) throw new IOException("too big");
                    o.write(buf, 0, k);
                }
            }
        } finally {
            h.disconnect();
        }
    }

    /* A file in pieces: <name>.part grows (Range from what's there), renamed when whole */
    private void part(String base, Saved.Item it, Saved.Part p) throws IOException {
        File whole = Offline.file(this, it, p.file), part = Offline.file(this, it, p.file + ".part");
        if (whole.isFile() && (p.bytes == 0 || whole.length() == p.bytes)) {
            p.done = whole.length();
            return;
        }
        for (int tries = 0;; tries++) {
            long from = Saved.resumeAt(part.length(), p.bytes);
            if (from == 0 && part.length() > 0) part.delete();
            HttpURLConnection h = Offline.open(base, p.url, "GET", agent());
            try {
                if (from > 0) h.setRequestProperty("Range", "bytes=" + from + "-");
                int code = h.getResponseCode();
                if (code != 200 && code != 206) throw new Offline.Refused(code);
                if (Offline.signInPage(h)) throw new Offline.Refused(401); /* (signed out: Hemma's sign-in page) */
                boolean on = Saved.goesOn(code, h.getHeaderField("Content-Range"), from);
                if (!on) from = 0;
                long total = Saved.totalOf(code, h.getHeaderField("Content-Range"), h.getContentLengthLong());
                if (total > 0) p.bytes = total; /* (Hemma's own size, not the manifest's estimate) */
                try (InputStream in = h.getInputStream(); OutputStream o = new FileOutputStream(part, on)) {
                    byte[] buf = new byte[1 << 17];
                    long got = from;
                    for (int k; (k = in.read(buf)) > 0;) {
                        if (Offline.deleted.contains(it.id)) throw new Offline.Refused(410); /* (deleted meanwhile) */
                        o.write(buf, 0, k);
                        got += k;
                        p.done = got;
                        progress(it);
                    }
                }
                break;
            } catch (Offline.Refused r) {
                throw r;
            } catch (IOException e) { /* a dropped connection: again from where it got to, a few times */
                if (tries >= 4) throw e;
                SystemClock.sleep(3000L << tries);
            } finally {
                h.disconnect();
                Offline.put(this, it);
            }
        }
        if (p.bytes > 0 && part.length() != p.bytes) throw new IOException("short");
        if (!part.renameTo(whole)) throw new IOException("rename");
        p.done = whole.length();
    }

    /* A book's parts' lengths, when Hemma's manifest didn't say: read from the saved files */
    private void measure(Saved.Item it) {
        for (Saved.Part p : it.files) {
            if (p.seconds > 0) continue;
            MediaMetadataRetriever r = new MediaMetadataRetriever();
            try {
                r.setDataSource(Offline.file(this, it, p.file).getPath());
                String ms = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
                p.seconds = ms != null ? Long.parseLong(ms) / 1000.0 : 0;
            } catch (RuntimeException ignored) {
            } finally {
                try {
                    r.release();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private void progress(Saved.Item it) {
        long now = SystemClock.elapsedRealtime();
        if (now - shown < 700) return;
        shown = now;
        getSystemService(NotificationManager.class).notify(ID, note(getString(R.string.saving_note, it.title, it.percent()), it.percent(), true));
        Offline.put(this, it);
    }

    private Notification note(String text, int percent, boolean ongoing) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        Intent i = new Intent(this, OfflineActivity.class);
        b.setSmallIcon(R.drawable.ic_note)
            .setContentTitle(text)
            .setOngoing(ongoing)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setContentIntent(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        if (percent >= 0) b.setProgress(100, percent, false);
        else if (ongoing) b.setProgress(0, 0, true);
        return b.build();
    }
}
