/* Saved.java (2.3, offline): the manifest and item.json, what a page may ask the app to save, a book's place in its
 * parts, chapters, resuming a download, the places to send back - checked with a JDK alone (apps/android/build.sh and
 * build.ps1 run it before building the app):
 *   javac -d out src/io/github/kloparn/hemma/Saved.java test/SavedTest.java && java -cp out SavedTest */
import io.github.kloparn.hemma.Saved;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

public class SavedTest {
    static int failed, checks;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failed++;
            System.out.println("FAILED: " + what);
        }
    }

    static void eq(Object got, Object want, String what) {
        check(want == null ? got == null : want.equals(got), what + ": got " + got + ", want " + want);
    }

    static boolean throwsOn(String json) {
        try {
            Saved.parse(json);
            return false;
        } catch (IllegalArgumentException e) {
            return true;
        }
    }

    static Saved.Item manifest(String json) { return Saved.Item.fromManifest((Map<?, ?>) Saved.parse(json)); }

    static final String VIDEO = "{\"v\":1,\"kind\":\"video\",\"id\":\"0123456789abcdef\",\"title\":\"Dark Side\","
        + "\"series\":\"Family Guy\",\"code\":\"S08E20\",\"poster\":\"/media/img/00000000000000aa\",\"duration_ms\":1320000,"
        + "\"resume_s\":600,\"files\":[{\"url\":\"/media/file/0123456789abcdef?a=1\",\"bytes\":400000000}],"
        + "\"subs\":[{\"url\":\"/media/sub/00000000000000bb.vtt\",\"lang\":\"sv\",\"label\":\"Svenska\",\"on\":true},"
        + "{\"url\":\"http://evil.example/x.vtt\",\"lang\":\"en\",\"label\":\"English\",\"on\":false}],"
        + "\"chapters\":[],\"author\":\"\",\"narrator\":\"\"}";

    static final String BOOK = "{\"v\":1,\"kind\":\"book\",\"id\":\"fedcba9876543210\",\"title\":\"Oz 1\","
        + "\"author\":\"L. Frank Baum\",\"narrator\":\"Phil\",\"duration_ms\":0,\"resume_s\":0,"
        + "\"files\":[{\"url\":\"/media/file/0000000000000001\",\"bytes\":1000,\"ms\":600000},"
        + "{\"url\":\"/media/file/0000000000000002\",\"bytes\":2000,\"ms\":900000},"
        + "{\"url\":\"/media/file/0000000000000003\",\"bytes\":3000,\"ms\":300000}],"
        + "\"chapters\":[{\"t\":0,\"title\":\"One\"},{\"t\":550,\"title\":\"Two\"},{\"t\":1400,\"title\":\"Three\"}]}";

    public static void main(String[] args) {
        /* JSON: what Hemma's manifests hold, back and forth */
        Object o = Saved.parse("{\"a\":[1,2.5,-3e2,true,false,null,\"x\\\"y\\u00e5\\n\"],\"b\":{}}");
        check(o instanceof Map, "an object");
        eq(Saved.write(o), "{\"a\":[1,2.5,-300,true,false,null,\"x\\\"yå\\n\"],\"b\":{}}", "written back");
        check(throwsOn("{\"a\":1,}") && throwsOn("[1 2]") && throwsOn("{\"a\"") && throwsOn("\"\u0001\"") && throwsOn("nul")
            && throwsOn("1 2") && throwsOn(""), "not JSON");
        StringBuilder deep = new StringBuilder();
        for (int i = 0; i < 40; i++) deep.append('[');
        check(throwsOn(deep.toString()), "too deep");

        /* a page's Save for offline: only Hemma's manifests */
        eq(Saved.savePath("save?m=%2Fmedia%2Foffline%2F0123456789abcdef.json"), "/media/offline/0123456789abcdef.json", "a video");
        eq(Saved.savePath("save?m=/media/offline/season/0123456789abcdef-2.json"), "/media/offline/season/0123456789abcdef-2.json",
            "a season, unencoded");
        eq(Saved.savePath("save?m=http%3A%2F%2Fevil%2Fmedia%2Foffline%2Fx.json"), null, "another host");
        eq(Saved.savePath("save?m=%2Fmedia%2Foffline%2F..%2F..%2Fa%2Fpeople.delete"), null, "climbing out");
        eq(Saved.savePath("save?m=%2Fmedia%2Fofflinex"), null, "not the folder");
        eq(Saved.savePath("save?m=%2Fmedia%2Foffline%2Fa.json%3Fx%3D1"), null, "a query");
        eq(Saved.savePath("offline"), null, "not a save");
        eq(Saved.savePath("save?m=%ZZ"), null, "bad escapes");
        check(Saved.hemmaPath("/media/file/0123456789abcdef?a=1&k=1-2-ab") && !Saved.hemmaPath("//evil/x")
            && !Saved.hemmaPath("http://x/y") && !Saved.hemmaPath("/a b") && !Saved.hemmaPath("/a\\b") && !Saved.hemmaPath(""),
            "file addresses");

        /* a video's manifest */
        Saved.Item v = manifest(VIDEO);
        check(v != null, "a video manifest");
        if (v != null) {
            eq(v.files.size(), 1, "one file");
            eq(v.files.get(0).file, "video.media", "its name on the phone");
            eq(v.subs.size(), 1, "the subtitle on another host left out");
            eq(v.subs.get(0).file, "sub-1.vtt", "the subtitle's name");
            check(v.subs.get(0).on, "the subtitle on");
            eq(v.sub(), "Family Guy · S08E20", "the second line");
            eq(v.start(), 600.0, "starts where Hemma had it");
            v.pos = 1318;
            eq(v.start(), 0.0, "from the top in its last seconds");
            v.pos = 700;
            v.dur = 1320;
            v.unsent = true;
            v.files.get(0).done = 1234;
            v.state = "ready";
            v.posterFile = "poster.jpg";
            v.manifest = "/media/offline/0123456789abcdef.json";
            Saved.Item back = Saved.Item.fromJson(v.json());
            check(back != null, "item.json read back");
            if (back != null) {
                eq(back.pos, 700.0, "its place");
                check(back.unsent && !back.ended, "unsent");
                eq(back.files.get(0).done, 1234L, "how far the file came");
                eq(back.state, "ready", "state");
                eq(back.posterFile, "poster.jpg", "poster");
                eq(back.manifest, "/media/offline/0123456789abcdef.json", "where its manifest is");
                eq(back.subs.get(0).file, "sub-1.vtt", "its subtitle file");
                eq(back.json(), v.json(), "the same again");
            }
            eq(v.progressForm(), "id=0123456789abcdef&pos=700&dur=1320&ended=", "media.progress");
        }
        check(manifest("{\"v\":2,\"kind\":\"video\",\"id\":\"0123456789abcdef\",\"files\":[{\"url\":\"/m\",\"bytes\":1}]}") == null,
            "a newer manifest");
        check(manifest("{\"v\":1,\"kind\":\"video\",\"id\":\"0123\",\"files\":[{\"url\":\"/m\",\"bytes\":1}]}") == null, "a bad id");
        check(manifest("{\"v\":1,\"kind\":\"video\",\"id\":\"0123456789abcdef\",\"files\":[{\"url\":\"http://x/m\",\"bytes\":1}]}") == null,
            "a file elsewhere");
        check(manifest("{\"v\":1,\"kind\":\"video\",\"id\":\"0123456789abcdef\",\"files\":[]}") == null, "no file");
        check(manifest("{\"v\":1,\"kind\":\"film\",\"id\":\"0123456789abcdef\",\"files\":[{\"url\":\"/m\",\"bytes\":1}]}") == null,
            "a kind it doesn't know");

        /* a book: its parts, its place in them, its chapters */
        Saved.Item b = manifest(BOOK);
        check(b != null, "a book manifest");
        if (b != null) {
            eq(b.files.get(2).file, "part-3.media", "a part's name");
            eq(b.length(), 1800.0, "its length: the parts'");
            eq(b.sub(), "L. Frank Baum", "the author under the title");
            double[] l = b.lengths();
            eq(Saved.partAt(l, 0), 0, "the start");
            eq(Saved.partAt(l, 599.9), 0, "the first part's end");
            eq(Saved.partAt(l, 600), 1, "the second's start");
            eq(Saved.partAt(l, 1600), 2, "the third");
            eq(Saved.partAt(l, 99999), 2, "past the end: the last");
            eq(Saved.partStart(l, 2), 1500.0, "where the third starts");
            eq(Saved.chapterAt(b.chapters, 0), 0, "chapter one");
            eq(Saved.chapterAt(b.chapters, 549), 0, "still one");
            eq(Saved.chapterAt(b.chapters, 550), 1, "two");
            eq(Saved.chapterAt(b.chapters, 5000), 2, "three, to the end");
            eq(Saved.chapterAt(new ArrayList<>(), 10), -1, "no chapters");
            b.pos = 1650;
            b.dur = 1800;
            b.ended = false;
            b.unsent = true;
            eq(b.progressForm(), "id=fedcba9876543210&pos=1650&dur=1800&ended=", "a book's place in the whole book");
        }

        /* the places Hemma hasn't heard */
        Saved.Item heard = manifest(VIDEO), never = manifest(BOOK);
        heard.pos = 5;
        if (v != null && b != null) eq(Saved.toSend(Arrays.asList(v, heard, never, b)).size(), 2, "the unsent ones");

        /* 2.7.1: the place both ways - the newer wins, the phone's or Hemma's (skew = Hemma's clock less the phone's) */
        Saved.Places ps = Saved.places("{\"v\":1,\"now\":5000,\"places\":[{\"id\":\"0123456789abcdef\",\"pos\":900,\"dur\":1320,"
            + "\"done\":false,\"at\":4000},{\"id\":\"nope\",\"pos\":1},{\"id\":\"fedcba9876543210\",\"pos\":1700,\"dur\":1800,"
            + "\"done\":true,\"at\":4500}]}");
        check(ps != null && ps.now == 5000 && ps.by.size() == 2, "Hemma's places, a bad id left out");
        check(Saved.places("{\"v\":2,\"now\":5000,\"places\":[]}") == null && Saved.places("<html>") == null
            && Saved.places("{\"v\":1,\"places\":[]}") == null, "not that answer");
        Saved.Place tv = ps != null ? ps.by.get("0123456789abcdef") : new Saved.Place();
        Saved.Item s = manifest(VIDEO); /* (saved at 600, never played here) */
        eq(Saved.settle(s, tv, 0), Saved.TAKE, "watched on the TV since it was saved: Hemma's");
        s.take(tv, 100);
        check(s.pos == 900 && !s.unsent && !s.ended && s.placeAt == 3900, "taken, its time on the phone's clock");
        eq(s.start(), 900.0, "and played from there");
        eq(Saved.settle(s, tv, 100), Saved.KEEP, "taken once");
        eq(Saved.settle(s, tv, 99), Saved.KEEP, "the clocks a second apart: still the same");
        s.pos = 1000; /* (played on here, offline) */
        s.unsent = true;
        s.placeAt = 4200;
        eq(Saved.settle(s, tv, 0), Saved.SEND, "played here later: ours to Hemma");
        eq(Saved.settle(s, tv, -300), Saved.TAKE, "the phone's clock ahead: the TV was later after all");
        eq(Saved.settle(s, null, 0), Saved.SEND, "Hemma has none: ours");
        s.placeAt = 3500;
        eq(Saved.settle(s, tv, 0), Saved.TAKE, "watched here, then on the TV: the TV's, ours never sent");
        s.unsent = false;
        s.placeAt = 4100; /* (sent; Hemma's own record of it is older: a report it had before) */
        eq(Saved.settle(s, tv, 0), Saved.KEEP, "ours is newer and Hemma has it");
        s.placeAt = 0; /* (a place from before 2.7.1) */
        eq(Saved.settle(s, tv, 0), Saved.TAKE, "a sent place with no time: Hemma's");
        s.unsent = true;
        eq(Saved.settle(s, tv, 0), Saved.SEND, "an unsent place with no time: sent, as before");
        eq(Saved.settle(manifest(VIDEO), null, 0), Saved.KEEP, "nothing anywhere");
        Saved.Item done = manifest(BOOK);
        done.pos = 1700;
        done.dur = 1800;
        done.placeAt = 4400;
        Saved.Place fin = ps != null ? ps.by.get("fedcba9876543210") : new Saved.Place();
        eq(Saved.settle(done, fin, 0), Saved.TAKE, "the same place, finished on Hemma's side");
        done.take(fin, 0);
        check(done.ended && done.start() == 0, "finished: from the top, as Hemma's pages start it");
        Saved.Item back2 = Saved.Item.fromJson(done.json());
        eq(back2 != null ? back2.placeAt : -1, 4500L, "its time in item.json");
        List<Saved.Item> many = new ArrayList<>();
        for (int i = 0; i < 61; i++) {
            Saved.Item it = manifest(VIDEO);
            it.id = String.format("%016x", i + 1);
            many.add(it);
        }
        List<String> asks = Saved.placesPaths(many);
        eq(asks.size(), 3, "61 things: three asks");
        eq(asks.get(2), "/media/offline/places.json?ids=000000000000003d", "the last on its own");
        check(asks.get(0).startsWith("/media/offline/places.json?ids=0000000000000001,0000000000000002,") && Saved.hemmaPath(asks.get(0)),
            "thirty ids, a path of Hemma's");
        eq(Saved.placesPaths(new ArrayList<>()).size(), 0, "nothing saved: nothing to ask");

        /* a download that goes on */
        eq(Saved.resumeAt(0, 100), 0L, "nothing yet");
        eq(Saved.resumeAt(40, 100), 40L, "from what's there");
        eq(Saved.resumeAt(140, 100), 0L, "more than the whole: start over");
        eq(Saved.resumeAt(40, 0), 40L, "the size unknown");
        check(Saved.goesOn(206, "bytes 40-99/100", 40), "206 from there");
        check(!Saved.goesOn(206, "bytes 0-99/100", 40), "206 from elsewhere");
        check(!Saved.goesOn(200, null, 40), "the whole file again");
        check(!Saved.goesOn(206, "bytes 0-99/100", 0), "from the start anyway");
        /* the size Hemma's answer gives - a one-sound view is smaller than the manifest's "bytes" */
        eq(Saved.totalOf(200, null, 9000), 9000L, "a 200's length");
        eq(Saved.totalOf(206, "bytes 4000-8999/9000", 5000), 9000L, "a 206's total");
        eq(Saved.totalOf(206, "bytes 4000-8999/*", 5000), -1L, "a total not said");
        eq(Saved.totalOf(200, null, -1), -1L, "no length");
        eq(Saved.totalOf(416, "bytes */9000", 0), -1L, "no file");

        System.out.println(failed == 0 ? "SavedTest: all " + checks + " checks passed" : "SavedTest: " + failed + " of " + checks + " FAILED");
        System.exit(failed == 0 ? 0 : 1);
    }
}
