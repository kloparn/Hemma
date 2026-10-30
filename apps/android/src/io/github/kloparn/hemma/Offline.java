/* Saved for offline (2.3) on this phone: each saved video or book is a folder <app's files>/offline/<id>/ holding its
 * files, poster, subtitles and item.json (Saved.Item: Hemma's manifest + how far each file came + the place reached
 * offline). The app's own storage on the phone (getExternalFilesDir: no permission asked; it goes with the app).
 * Also the one way the app talks to Hemma outside the WebView: GET/POST with the sign-in's cookie, only to the Hemma
 * it found (a private address - Net's rules), and sending the places reached offline back home (media.progress).
 * OfflineService fetches; OfflineActivity lists; WatchActivity and PlayerService play. */
package io.github.kloparn.hemma;

import android.content.Context;
import android.os.StatFs;
import android.webkit.CookieManager;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class Offline {
    private Offline() {}

    static File root(Context c) {
        File ext = c.getExternalFilesDir(null);
        File d = new File(ext != null ? ext : c.getFilesDir(), "offline");
        if (!d.isDirectory()) d.mkdirs();
        return d;
    }

    static File dir(Context c, String id) { return new File(root(c), id); }

    static File file(Context c, Saved.Item it, String name) { return new File(dir(c, it.id), name); }

    /** Every saved thing, the newest first */
    static synchronized List<Saved.Item> all(Context c) {
        List<Saved.Item> out = new ArrayList<>();
        File[] ds = root(c).listFiles();
        for (int i = 0; ds != null && i < ds.length; i++) {
            if (!Saved.validId(ds[i].getName())) continue;
            Saved.Item it = read(new File(ds[i], "item.json"));
            if (it != null) out.add(it);
        }
        Collections.sort(out, (a, b) -> Long.compare(b.savedAt, a.savedAt));
        return out;
    }

    static int count(Context c) { return all(c).size(); }

    static synchronized Saved.Item get(Context c, String id) {
        return Saved.validId(id) ? read(new File(dir(c, id), "item.json")) : null;
    }

    private static Saved.Item read(File f) {
        try {
            return Saved.Item.fromJson(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
        } catch (IOException e) {
            return null;
        }
    }

    /* deleted while it was being saved: the saving mustn't write it back (OfflineService.item clears it when saved
       again) */
    static final java.util.Set<String> deleted = java.util.Collections.synchronizedSet(new java.util.HashSet<>());

    /** item.json written whole (a new file renamed over the old: a phone switched off half-way keeps the old one) */
    static synchronized void put(Context c, Saved.Item it) {
        if (deleted.contains(it.id)) return;
        File d = dir(c, it.id);
        if (!d.isDirectory() && !d.mkdirs()) return;
        File tmp = new File(d, "item.json.new");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(it.json().getBytes(StandardCharsets.UTF_8));
            o.flush();
            ((FileOutputStream) o).getFD().sync();
        } catch (IOException e) {
            return;
        }
        tmp.renameTo(new File(d, "item.json"));
    }

    /** The place reached offline (a player): kept, and marked for Hemma */
    static synchronized void place(Context c, String id, double pos, double dur, boolean ended) {
        Saved.Item it = get(c, id);
        if (it == null || pos < 0) return;
        if (Math.abs(it.pos - pos) < 0.5 && it.ended == ended && it.unsent) return;
        it.pos = pos;
        if (dur > 0) it.dur = dur;
        it.ended = ended;
        it.unsent = true;
        put(c, it);
    }

    static synchronized void delete(Context c, String id) {
        if (!Saved.validId(id)) return;
        if (id.equals(OfflineService.savingId)) deleted.add(id);
        File d = dir(c, id);
        File[] fs = d.listFiles();
        for (int i = 0; fs != null && i < fs.length; i++) fs[i].delete();
        d.delete();
    }

    static long free(Context c) {
        try {
            return new StatFs(root(c).getPath()).getAvailableBytes();
        } catch (IllegalArgumentException e) {
            return 0;
        }
    }

    static long used(Context c, Saved.Item it) {
        long n = 0;
        File[] fs = dir(c, it.id).listFiles();
        for (int i = 0; fs != null && i < fs.length; i++) n += fs[i].length();
        return n;
    }

    /* ------------------------------------------------------------------ Hemma */

    /** Hemma answers a signed-out request for its media with its sign-in page - a 200 of HTML, never a 401 (the
     *  core's rule for every module's address): no manifest, file, poster or subtitles is ever HTML */
    static boolean signInPage(HttpURLConnection h) {
        String t = h.getContentType();
        return t != null && t.toLowerCase(java.util.Locale.ROOT).startsWith("text/html");
    }

    static final class Refused extends IOException {
        final int code;

        Refused(int code) {
            super("HTTP " + code);
            this.code = code;
        }
    }

    /** A connection to path on the Hemma at base - only a private address (resolved here), only a path of Hemma's;
     *  the sign-in's cookie and the app's name go along, no redirects followed */
    static HttpURLConnection open(String base, String path, String method, String agent) throws IOException {
        if (base == null || !Saved.hemmaPath(path)) throw new IOException("not Hemma's");
        URL u = new URL(base + path);
        if (!"http".equals(u.getProtocol()) || !Net.isPrivate(InetAddress.getByName(u.getHost()).getAddress()))
            throw new IOException("not at home");
        HttpURLConnection h = (HttpURLConnection) u.openConnection();
        h.setInstanceFollowRedirects(false);
        h.setConnectTimeout(6000);
        h.setReadTimeout(20000);
        h.setRequestMethod(method);
        String cookie = CookieManager.getInstance().getCookie(base + "/");
        if (cookie != null) h.setRequestProperty("Cookie", cookie);
        if (agent != null) h.setRequestProperty("User-Agent", agent);
        return h;
    }

    /** GET a small text (a manifest): Refused(code) for anything but 200 */
    static String text(String base, String path, String agent) throws IOException {
        HttpURLConnection h = open(base, path, "GET", agent);
        try {
            int code = h.getResponseCode();
            if (code != 200) throw new Refused(code);
            if (signInPage(h)) throw new Refused(401);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            try (InputStream in = h.getInputStream()) {
                for (int n; (n = in.read(buf)) > 0;) {
                    out.write(buf, 0, n);
                    if (out.size() > 4 << 20) throw new IOException("too big for a manifest");
                }
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            h.disconnect();
        }
    }

    /** The places reached offline, to Hemma (media.progress, as its own pages report them) - on a thread; each one
     *  Hemma took is marked sent. A redirect to the sign-in page isn't taking it. */
    static void sync(Context c, String base, String agent) {
        if (!syncing.compareAndSet(false, true)) return; /* (one at a time: two finds of Hemma sent each place twice) */
        try {
            send(c, base, agent);
        } finally {
            syncing.set(false);
        }
    }

    private static final java.util.concurrent.atomic.AtomicBoolean syncing = new java.util.concurrent.atomic.AtomicBoolean();

    private static void send(Context c, String base, String agent) {
        for (Saved.Item it : Saved.toSend(all(c))) {
            try {
                HttpURLConnection h = open(base, "/a/media.progress", "POST", agent);
                h.setDoOutput(true);
                h.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                h.setRequestProperty("Origin", base);
                h.setRequestProperty("Referer", base + "/");
                byte[] body = it.progressForm().getBytes(StandardCharsets.UTF_8);
                h.setFixedLengthStreamingMode(body.length);
                try (OutputStream o = h.getOutputStream()) {
                    o.write(body);
                }
                int code = h.getResponseCode();
                String to = h.getHeaderField("Location");
                h.disconnect();
                boolean took = (code >= 200 && code < 300) || (code >= 300 && code < 400 && (to == null || !to.contains("/login")));
                if (!took) continue;
                synchronized (Offline.class) {
                    Saved.Item now = get(c, it.id);
                    if (now != null && Math.abs(now.pos - it.pos) < 0.5) { /* (not moved on meanwhile) */
                        now.unsent = false;
                        put(c, now);
                    }
                }
            } catch (IOException ignored) { /* (not at home after all: next time) */
            }
        }
    }
}
