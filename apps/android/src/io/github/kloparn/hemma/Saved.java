/* Saved for offline (2.3): what the app keeps of each video and audiobook it saved - Hemma's manifest for it
 * (GET /media/offline/<id>.json), the files' local names and how far they came, and where the viewer got to while
 * offline - and the little JSON that takes. Pure Java, no Android: test/SavedTest.java runs it with a JDK alone,
 * like Net. Offline.java keeps these on the phone; OfflineService fetches them; the players read and move them.
 * What may be saved, by whom, in which language: Hemma's (media/offline.c) - the app only follows its manifest. */
package io.github.kloparn.hemma;

import java.io.UnsupportedEncodingException;
import java.net.URLDecoder;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class Saved {
    private Saved() {}

    /* ------------------------------------------------------------------ JSON */

    /** JSON text -> Map (objects, in order), List, String, Double, Boolean or null. IllegalArgumentException when it
     *  isn't JSON (or nests past 32 levels: nothing of Hemma's does). */
    public static Object parse(String s) {
        Reader r = new Reader(s);
        r.ws();
        Object v = r.value(0);
        r.ws();
        if (r.i != s.length()) throw new IllegalArgumentException("text after the JSON");
        return v;
    }

    private static final class Reader {
        final String s;
        int i;

        Reader(String s) { this.s = s; }

        void ws() {
            while (i < s.length() && " \t\r\n".indexOf(s.charAt(i)) >= 0) i++;
        }

        IllegalArgumentException bad() { return new IllegalArgumentException("not JSON at " + i); }

        Object value(int depth) {
            if (depth > 32 || i >= s.length()) throw bad();
            char c = s.charAt(i);
            if (c == '{') {
                Map<String, Object> m = new LinkedHashMap<>();
                i++;
                ws();
                if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
                for (;;) {
                    ws();
                    if (i >= s.length() || s.charAt(i) != '"') throw bad();
                    String k = string();
                    ws();
                    if (i >= s.length() || s.charAt(i) != ':') throw bad();
                    i++;
                    ws();
                    m.put(k, value(depth + 1));
                    ws();
                    if (i < s.length() && s.charAt(i) == ',') { i++; continue; }
                    if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
                    throw bad();
                }
            }
            if (c == '[') {
                List<Object> l = new ArrayList<>();
                i++;
                ws();
                if (i < s.length() && s.charAt(i) == ']') { i++; return l; }
                for (;;) {
                    ws();
                    l.add(value(depth + 1));
                    ws();
                    if (i < s.length() && s.charAt(i) == ',') { i++; continue; }
                    if (i < s.length() && s.charAt(i) == ']') { i++; return l; }
                    throw bad();
                }
            }
            if (c == '"') return string();
            if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
            if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            int from = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
            if (from == i) throw bad();
            try {
                return Double.parseDouble(s.substring(from, i));
            } catch (NumberFormatException e) {
                throw bad();
            }
        }

        String string() {
            StringBuilder b = new StringBuilder();
            for (i++; i < s.length(); i++) {
                char c = s.charAt(i);
                if (c == '"') { i++; return b.toString(); }
                if (c < 0x20) throw bad();
                if (c != '\\') { b.append(c); continue; }
                if (++i >= s.length()) throw bad();
                char e = s.charAt(i);
                switch (e) {
                    case 'n': b.append('\n'); break;
                    case 't': b.append('\t'); break;
                    case 'r': b.append('\r'); break;
                    case 'b': b.append('\b'); break;
                    case 'f': b.append('\f'); break;
                    case 'u':
                        if (i + 4 >= s.length()) throw bad();
                        try {
                            b.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16));
                        } catch (NumberFormatException x) {
                            throw bad();
                        }
                        i += 4;
                        break;
                    default: b.append(e);
                }
            }
            throw bad();
        }
    }

    /** Map / List / String / Number / Boolean / null -> JSON text */
    public static String write(Object o) {
        StringBuilder b = new StringBuilder();
        write(b, o);
        return b.toString();
    }

    private static void write(StringBuilder b, Object o) {
        if (o == null) b.append("null");
        else if (o instanceof String) quote(b, (String) o);
        else if (o instanceof Boolean) b.append(o);
        else if (o instanceof Number) {
            double d = ((Number) o).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) b.append('0');
            else if (d == Math.rint(d) && Math.abs(d) < 1e15) b.append((long) d);
            else b.append(d);
        } else if (o instanceof Map) {
            b.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> e : ((Map<?, ?>) o).entrySet()) {
                if (!first) b.append(',');
                first = false;
                quote(b, String.valueOf(e.getKey()));
                b.append(':');
                write(b, e.getValue());
            }
            b.append('}');
        } else if (o instanceof List) {
            b.append('[');
            boolean first = true;
            for (Object v : (List<?>) o) {
                if (!first) b.append(',');
                first = false;
                write(b, v);
            }
            b.append(']');
        } else quote(b, o.toString());
    }

    private static void quote(StringBuilder b, String s) {
        b.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c == '\n') b.append("\\n");
            else if (c == '\r') b.append("\\r");
            else if (c == '\t') b.append("\\t");
            else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        b.append('"');
    }

    /* a field of a parsed object, of the type wanted - else the fallback */
    static String str(Map<?, ?> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof String ? (String) v : "";
    }

    static double num(Map<?, ?> m, String k, double or) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Double ? (Double) v : or;
    }

    static boolean bool(Map<?, ?> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof Boolean && (Boolean) v;
    }

    static List<?> list(Map<?, ?> m, String k) {
        Object v = m == null ? null : m.get(k);
        return v instanceof List ? (List<?>) v : new ArrayList<>();
    }

    /* ------------------------------------------------------------------ what's safe to ask for */

    /** A Hemma file's address from a manifest: a path on the Hemma the app knows ("/media/file/..."), never an address
     *  of another host ("//x", "http://") or anything a path can't be */
    public static boolean hemmaPath(String u) {
        if (u == null || u.length() < 2 || u.length() > 600 || u.charAt(0) != '/' || u.charAt(1) == '/') return false;
        for (int i = 0; i < u.length(); i++) {
            char c = u.charAt(i);
            if (c <= ' ' || c == '\\' || c > '~') return false;
        }
        return !u.contains("://");
    }

    /** hemma-app:save?m=<manifest path> (a page's Save for offline): the path, or null - only Hemma's manifests
     *  ("/media/offline/...", letters, digits, '-', '_', '.', '/'), nothing a page could point elsewhere */
    public static String savePath(String encodedSchemePart) {
        if (encodedSchemePart == null || !encodedSchemePart.startsWith("save?")) return null;
        String m = null;
        for (String kv : encodedSchemePart.substring(5).split("&")) {
            if (!kv.startsWith("m=")) continue;
            try {
                m = URLDecoder.decode(kv.substring(2), "UTF-8");
            } catch (UnsupportedEncodingException | IllegalArgumentException e) {
                return null;
            }
        }
        if (m == null || !m.startsWith("/media/offline/") || m.contains("..") || m.contains("//") || m.length() > 200) return null;
        for (int i = 0; i < m.length(); i++) {
            char c = m.charAt(i);
            if (!(Character.isLetterOrDigit(c) && c < 128) && "/-_.".indexOf(c) < 0) return null;
        }
        return m;
    }

    public static boolean validId(String id) { return id != null && id.matches("[0-9a-f]{16}"); }

    /* ------------------------------------------------------------------ one saved thing */

    public static final class Part {
        public String url = "", file = "";
        public long bytes, done; /* its size (Hemma's word for it), and what's on the phone so far */
        public double seconds;   /* its length - a book's parts: from the manifest, else measured once saved */
    }

    public static final class Sub {
        public String url = "", file = "", lang = "", label = "";
        public boolean on;
    }

    public static final class Chapter {
        public double t; /* seconds into the whole book */
        public String title = "";
    }

    public static final class Item {
        public String id = "", kind = "video", title = "", series = "", code = "", poster = "", posterFile = "";
        public String author = "", narrator = "", manifest = "";
        public long durationMs, savedAt;
        public double resumeS;
        public final List<Part> files = new ArrayList<>();
        public final List<Sub> subs = new ArrayList<>();
        public final List<Chapter> chapters = new ArrayList<>();
        public String state = "saving"; /* saving | ready | failed */
        public String why = "";         /* failed: room | forbidden | gone | signin | network | bad */
        public double pos = -1, dur;    /* the place reached offline (-1 = none yet) - or Hemma's, when newer (2.7.1) */
        public boolean ended, unsent;   /* unsent: Hemma hasn't heard of that place yet */
        public long placeAt;            /* when pos was reached, by the PHONE's clock (unix s; 0 = not known) */

        public boolean book() { return "book".equals(kind); }

        public long bytes() {
            long n = 0;
            for (Part p : files) n += p.bytes;
            return n;
        }

        public long done() {
            long n = 0;
            for (Part p : files) n += Math.min(p.done, p.bytes > 0 ? p.bytes : p.done);
            return n;
        }

        public int percent() {
            long all = bytes();
            return all <= 0 ? 0 : (int) Math.min(100, done() * 100 / all);
        }

        /** Where playing starts: the place reached offline (or Hemma's newer one), else Hemma's when it was saved; from the
         *  top once it was seen to its end, or in its last 5 s - as Hemma's pages do */
        public double start() {
            if (ended) return 0;
            double at = pos >= 0 ? pos : resumeS, len = length();
            return at < 0 || (len > 0 && at >= len - 5) ? 0 : at;
        }

        /** Its length in seconds: the parts' (a book), else Hemma's word */
        public double length() {
            double s = 0;
            for (Part p : files) s += p.seconds;
            return s > 0 ? s : durationMs / 1000.0;
        }

        public double[] lengths() {
            double[] l = new double[files.size()];
            for (int i = 0; i < l.length; i++) l[i] = files.get(i).seconds;
            return l;
        }

        /** "Family Guy · S08E20", the author, or "" (the second line under its title) */
        public String sub() {
            if (!series.isEmpty()) return code.isEmpty() ? series : series + " · " + code;
            return book() ? author : "";
        }

        /** From Hemma's manifest; null when it isn't one the app can take (a version it doesn't know, a bad id, an
         *  address that isn't a path on Hemma) */
        public static Item fromManifest(Map<?, ?> m) {
            if (m == null || num(m, "v", 0) != 1) return null;
            Item it = new Item();
            it.id = str(m, "id");
            it.kind = str(m, "kind");
            if (!validId(it.id) || !(it.kind.equals("video") || it.kind.equals("book"))) return null;
            it.title = str(m, "title");
            it.series = str(m, "series");
            it.code = str(m, "code");
            it.poster = str(m, "poster");
            if (!it.poster.isEmpty() && !hemmaPath(it.poster)) it.poster = "";
            it.author = str(m, "author");
            it.narrator = str(m, "narrator");
            it.durationMs = (long) num(m, "duration_ms", 0);
            it.resumeS = num(m, "resume_s", 0);
            for (Object o : list(m, "files")) {
                if (!(o instanceof Map)) return null;
                Part p = new Part();
                p.url = str((Map<?, ?>) o, "url");
                p.bytes = (long) num((Map<?, ?>) o, "bytes", 0);
                p.seconds = num((Map<?, ?>) o, "ms", 0) / 1000.0;
                if (!hemmaPath(p.url) || p.bytes < 0) return null;
                p.file = (it.book() ? "part-" + (it.files.size() + 1) : "video") + ".media";
                it.files.add(p);
            }
            if (it.files.isEmpty() || (!it.book() && it.files.size() != 1)) return null;
            for (Object o : list(m, "subs")) {
                if (!(o instanceof Map)) continue;
                Sub s = new Sub();
                s.url = str((Map<?, ?>) o, "url");
                s.lang = str((Map<?, ?>) o, "lang");
                s.label = str((Map<?, ?>) o, "label");
                s.on = bool((Map<?, ?>) o, "on");
                if (!hemmaPath(s.url)) continue;
                s.file = "sub-" + (it.subs.size() + 1) + ".vtt";
                it.subs.add(s);
            }
            for (Object o : list(m, "chapters")) {
                if (!(o instanceof Map)) continue;
                Chapter c = new Chapter();
                c.t = Math.max(0, num((Map<?, ?>) o, "t", 0));
                c.title = str((Map<?, ?>) o, "title");
                it.chapters.add(c);
            }
            return it;
        }

        /** item.json: all of it, the files' progress and the offline place too */
        public String json() {
            Map<String, Object> m = new LinkedHashMap<>();
            m.put("v", 1);
            m.put("id", id);
            m.put("kind", kind);
            m.put("title", title);
            m.put("series", series);
            m.put("code", code);
            m.put("poster", poster);
            m.put("poster_file", posterFile);
            m.put("author", author);
            m.put("narrator", narrator);
            m.put("manifest", manifest);
            m.put("duration_ms", durationMs);
            m.put("resume_s", resumeS);
            m.put("saved_at", savedAt);
            List<Object> fl = new ArrayList<>();
            for (Part p : files) {
                Map<String, Object> f = new LinkedHashMap<>();
                f.put("url", p.url);
                f.put("file", p.file);
                f.put("bytes", p.bytes);
                f.put("done", p.done);
                f.put("ms", Math.round(p.seconds * 1000));
                fl.add(f);
            }
            m.put("files", fl);
            List<Object> sl = new ArrayList<>();
            for (Sub s : subs) {
                Map<String, Object> f = new LinkedHashMap<>();
                f.put("url", s.url);
                f.put("file", s.file);
                f.put("lang", s.lang);
                f.put("label", s.label);
                f.put("on", s.on);
                sl.add(f);
            }
            m.put("subs", sl);
            List<Object> cl = new ArrayList<>();
            for (Chapter c : chapters) {
                Map<String, Object> f = new LinkedHashMap<>();
                f.put("t", c.t);
                f.put("title", c.title);
                cl.add(f);
            }
            m.put("chapters", cl);
            m.put("state", state);
            m.put("why", why);
            m.put("pos", pos);
            m.put("dur", dur);
            m.put("ended", ended);
            m.put("unsent", unsent);
            m.put("place_at", placeAt);
            return write(m);
        }

        public static Item fromJson(String text) {
            Object o;
            try {
                o = parse(text);
            } catch (IllegalArgumentException e) {
                return null;
            }
            if (!(o instanceof Map)) return null;
            Map<?, ?> m = (Map<?, ?>) o;
            Item it = fromManifest(m);
            if (it == null) return null;
            it.posterFile = str(m, "poster_file");
            it.manifest = str(m, "manifest");
            it.savedAt = (long) num(m, "saved_at", 0);
            List<?> fl = list(m, "files");
            for (int i = 0; i < it.files.size() && i < fl.size(); i++) {
                Map<?, ?> f = (Map<?, ?>) fl.get(i);
                if (!str(f, "file").isEmpty()) it.files.get(i).file = str(f, "file");
                it.files.get(i).done = (long) num(f, "done", 0);
            }
            List<?> sl = list(m, "subs");
            for (int i = 0, k = 0; i < sl.size() && k < it.subs.size(); i++) {
                if (!(sl.get(i) instanceof Map) || !hemmaPath(str((Map<?, ?>) sl.get(i), "url"))) continue;
                String f = str((Map<?, ?>) sl.get(i), "file");
                if (!f.isEmpty()) it.subs.get(k).file = f;
                k++;
            }
            it.state = str(m, "state").isEmpty() ? "saving" : str(m, "state");
            it.why = str(m, "why");
            it.pos = num(m, "pos", -1);
            it.dur = num(m, "dur", 0);
            it.ended = bool(m, "ended");
            it.unsent = bool(m, "unsent");
            it.placeAt = (long) num(m, "place_at", 0);
            return it;
        }

        /** Hemma's place, newer than this one, taken (skew = Hemma's clock less the phone's, seconds) */
        public void take(Place p, long skew) {
            pos = p.pos;
            if (p.dur > 0) dur = p.dur;
            ended = p.done;
            unsent = false;
            placeAt = p.at - skew;
        }

        /** What Hemma's media.progress takes for the place reached offline (a book's in the whole book) */
        public String progressForm() {
            return "id=" + id + "&pos=" + (long) Math.max(0, pos) + "&dur=" + (long) Math.max(0, dur) + "&ended=" + (ended ? "1" : "");
        }
    }

    /** The saved things whose offline place Hemma hasn't heard yet */
    public static List<Item> toSend(List<Item> all) {
        List<Item> out = new ArrayList<>();
        for (Item it : all)
            if (it.unsent && it.pos >= 0) out.add(it);
        return out;
    }

    /* ------------------------------------------------------------------ the place, both ways (2.7.1) */

    /** Where Hemma says the viewer is in one saved thing (GET /media/offline/places.json) */
    public static final class Place {
        public double pos, dur;
        public boolean done;
        public long at; /* when they got there, by HEMMA's clock (unix s) */
    }

    /** Hemma's answer: its clock ("now") and its places by id; null = not that answer */
    public static final class Places {
        public long now;
        public final Map<String, Place> by = new LinkedHashMap<>();
    }

    public static Places places(String json) {
        Object o;
        try {
            o = parse(json);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if (!(o instanceof Map) || num((Map<?, ?>) o, "v", 0) != 1) return null;
        Map<?, ?> m = (Map<?, ?>) o;
        Places out = new Places();
        out.now = (long) num(m, "now", 0);
        if (out.now <= 0) return null;
        for (Object e : list(m, "places")) {
            if (!(e instanceof Map)) continue;
            Map<?, ?> f = (Map<?, ?>) e;
            Place p = new Place();
            p.pos = Math.max(0, num(f, "pos", 0));
            p.dur = Math.max(0, num(f, "dur", 0));
            p.done = bool(f, "done");
            p.at = (long) num(f, "at", 0);
            if (validId(str(f, "id"))) out.by.put(str(f, "id"), p);
        }
        return out;
    }

    /** Hemma's addresses for the places of these saved things: a few dozen ids each (an address stays under hemmaPath's
     *  600 characters) */
    public static List<String> placesPaths(List<Item> all) {
        List<String> out = new ArrayList<>();
        StringBuilder b = null;
        int n = 0;
        for (Item it : all) {
            if (!validId(it.id)) continue;
            if (b == null || n == PLACES_EACH) {
                if (b != null) out.add(b.toString());
                b = new StringBuilder("/media/offline/places.json?ids=");
                n = 0;
            }
            b.append(n++ > 0 ? "," : "").append(it.id);
        }
        if (b != null) out.add(b.toString());
        return out;
    }

    static final int PLACES_EACH = 30;

    public static final int KEEP = 0, SEND = 1, TAKE = 2;

    /** One saved thing's place against Hemma's (p: null = Hemma has none; skew = Hemma's clock less the phone's): the
     *  newer one wins. SEND = ours, reached here later, goes to Hemma; TAKE = Hemma's (watched on the TV, in a browser,
     *  in the app at home since) becomes ours; KEEP = nothing to do. A place reached here before 2.7.1 has no time: it's
     *  sent, as it was then. */
    public static int settle(Item it, Place p, long skew) {
        boolean ours = it.pos >= 0 && it.placeAt > 0;
        if (it.unsent && it.pos >= 0 && (p == null || it.placeAt <= 0 || it.placeAt + skew >= p.at)) return SEND;
        if (p == null) return KEEP;
        if (!it.unsent && ours && it.placeAt + skew >= p.at) return KEEP;
        if (!it.unsent && Math.abs(p.pos - it.pos) < 1 && p.done == it.ended) return KEEP; /* (the same: nothing to write) */
        return TAKE;
    }

    /* ------------------------------------------------------------------ a book's place */

    /** Which part plays at second t of the whole book (lens = each part's length; the last takes what's past the end) */
    public static int partAt(double[] lens, double t) {
        double acc = 0;
        for (int i = 0; i < lens.length; i++) {
            if (t < acc + lens[i] || i == lens.length - 1) return i;
            acc += lens[i];
        }
        return 0;
    }

    /** Where part i starts in the whole book */
    public static double partStart(double[] lens, int i) {
        double acc = 0;
        for (int k = 0; k < i && k < lens.length; k++) acc += lens[k];
        return acc;
    }

    /** The chapter playing at t (the last starting at or before it); -1 = none (no chapters, or before the first) */
    public static int chapterAt(List<Chapter> ch, double t) {
        int at = -1;
        for (int i = 0; i < ch.size(); i++)
            if (ch.get(i).t <= t + 0.25) at = i;
        return at;
    }

    /* ------------------------------------------------------------------ saving a file in pieces */

    /** Where a file's download goes on from: what's on the phone - unless that's more than the whole (a file Hemma
     *  made again, smaller): then from the start */
    public static long resumeAt(long have, long total) {
        if (have <= 0) return 0;
        return total > 0 && have > total ? 0 : have;
    }

    /** Hemma's answer to "bytes=<from>-": true = it sends from there (206, Content-Range "bytes <from>-..."), so the
     *  bytes go after what's there; false = the whole file again (200, or a range that isn't the one asked) */
    public static boolean goesOn(int code, String contentRange, long from) {
        if (from <= 0 || code != 206 || contentRange == null) return false;
        return contentRange.trim().startsWith("bytes " + from + "-");
    }

    /** The file's whole size as Hemma's answer says it - a 200's length, a 206's ".../<total>" - or -1. It, not the
     *  manifest's "bytes", is what a saved file must come to: a video in several languages is sent with the one listened
     *  to alone, a little smaller than the file the manifest measured. */
    public static long totalOf(int code, String contentRange, long contentLength) {
        if (code == 200) return contentLength > 0 ? contentLength : -1;
        if (code != 206 || contentRange == null) return -1;
        int slash = contentRange.lastIndexOf('/');
        if (slash < 0) return -1;
        try {
            long t = Long.parseLong(contentRange.substring(slash + 1).trim());
            return t > 0 ? t : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }
}
