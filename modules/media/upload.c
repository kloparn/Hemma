/* modules/media/upload.c — admins add videos to the library from any browser.
 *
 *   POST /media/upload/plan  {"root":"0","into":"Series","files":[["Show/Season 1/x.mkv",123],...]}
 *        -> {"batch":"<id>","todo":[[i,offset],[i,offset,end],...],"skipped":n,"total":bytes}  or  {"error":"...","stop":true}
 *        (with an end, only the bytes before it are wanted: a play-all's header)
 *   PUT  /media/upload/<batch>/<i>?at=<offset>   file i's bytes from offset, streamed to disk
 *   PUT  /media/upload/<batch>/<i>?at=<offset>&end=<e>   only the piece up to e -> {"ok":true,"at":e} (1.3: a copy
 *        kept in the browser's memory goes piece by piece; the last piece's answer is a whole file's)
 *   GET  /media/upload/<batch>/<i>               {"at":<bytes saved>}: where to resume
 *
 * With "conv":"1" (the browser can run the converter, and the box is ticked) a video
 * browsers can't all play comes as a copy the browser makes first: its todo entry is
 * {"i":<entry>,"f":<the browser's file>} and the browser, while converting and after,
 *   POST /media/upload/<batch>/<i>?pm=<0..1000>[&fed=<bytes>]  progress (fed: what the converter has taken in of the
 *        original so far - the card shows how fast, 1.5; 1000 = the copy is made) -> {"ok":true} | {"stop":true,...}
 *   POST /media/upload/<batch>/<i>?size=<bytes>      the copy's size -> {"at":0}: then PUT it
 *   POST /media/upload/<batch>/<i>?original=1&why=.. it couldn't   -> {"at":n} (PUT the original) | {"skip":true}
 *   POST /media/upload/<batch>/<i>?sub=1&lang=&name=&forced=&t=   (body: SRT) a text subtitle track of the
 *        file, saved next to the copy as "<name>.<language>.srt" -> {"ok":true} | {"skip":true} (one is there)
 *   PUT  /media/upload/<batch>/<i>?pic=pgs|vobsub&lang=&name=&forced=&t=[&idx=<bytes>]   a PICTURE subtitle
 *        track of the file (Blu-ray PGS, DVD VobSub - up to AV_CONV_PIC_BYTES, so streamed like a file):
 *        PGS: the body is the .sup, saved as "<name>.<language>.sup"; VobSub: the body is the .idx text
 *        (its first idx= bytes) and then the .sub, saved as "<name>.<language>.sub" and then ".idx" - both
 *        or neither. Written to "<file>.hemma-part" on a worker thread and renamed; never over a file
 *        (-> {"skip":true}); a try that broke off is sent again whole.
 *        t = the track's place among the file's subtitle tracks: the browser sends them in the file's
 *        order, so a second track in one language and flags is ".2" as the server's converter names it
 *        (text and pictures counted together, convert_save_sub), and a track sent again is known by it.
 * A video that stays behind (the library has it, or its copy) but whose subtitles the library lacks is
 * {"i":<entry>,"f":<file>,"subs":1} (1.3): the browser reads only its subtitle tracks, sends them as above -
 * they go next to the library's video - with progress (?pm=), and ?pm=1000 once it's done with it.
 *
 * Subtitle files the household drops in go as they are: .srt .vtt .sup .idx - and a VobSub's .sub, when
 * its .idx is in the same upload (alone, a .sub is MicroDVD text: refused). The pair is sorted together
 * and sent both or neither, the .sub first (the library takes a .sub only beside its .idx).
 *
 * AUDIOBOOKS (1.6): .m4b .m4a .mp3 go as they are - never converted, never a dupe or a companion of a video -
 * into the library folder's books folder (upload_sort.c); resuming, room and one upload at a time as for any file.
 * An .m4a or .mp3 in a show's or a film's own folder (a theme song, a soundtrack) is no audiobook: it goes
 * along with that folder.
 *
 * web/wire.js drives it (form[data-upload]): one plan, then one PUT per file.
 * Files are written as "<name>.hemma-part" (MEDIA_PART_SUFFIX) and renamed when complete, so the library
 * never sees half a file and a dropped upload resumes where it stopped. Bytes
 * go socket -> disk on a worker thread through one fixed buffer: memory stays
 * flat whatever the file size, and the event loop never waits for the disk.
 *
 * Admins only, same-origin only, every name checked part by part, and a plan
 * that doesn't fit on the disk is refused before a byte is sent (room.h: 1 GB stays
 * free, and running conversions keep the room they need; each PUT checks again). A copy
 * the server's own converter is making is never taken; one only queued there leaves the
 * converter's queue - the upload brings it. The converter asks here too (upload_claims).
 * The page is rendered here like any other, so progress shows on every screen.
 *
 * Words (issue #46): answers to the browser are in the language of the request (a worker thread
 * takes it along: Job.lang); the reasons kept for the cards (UpFile.why) are the English packed
 * with their values (i18n_pack), so every viewer reads them in their own language. */
#include <stdarg.h>
#include <stdatomic.h>

#include "../../core/assets.h"
#include "../../core/env.h"
#include "../../core/json.h"
#include "../../core/ui.h"
#include "convert.h"
#include "room.h"
#include "upload.h"
#include "media.h"

/* How much of the sending device's disk copies waiting to be sent may take, in GB (the form's slider,
   remembered per person; wire.js's converter keeps to it). More lets more slow conversions - DVD rips -
   run side by side. */
#define PREF_UPLOAD_ROOM "media.upload_room"
/* ... and whether they wait in its memory instead (1.3; owner, 2026-10-23: "the option of storing converted videos
   in RAM or on a drive ... as a big checkbox. If other people use it, the old style is nice"): off by default */
#define PREF_UPLOAD_RAM "media.upload_ram"
#define ROOM_DEFAULT 8
#define ROOM_MIN 2
#define ROOM_MAX 256 /* (2.7: "make the user be able to run it much higher" - it was 32) */
#define MAX_BATCHES 4
#define MAX_FILES 5000
#define MAX_WORKERS 2
#define CHUNK (512 * 1024)
#define STALL_MS 60000
#define KEEP_DONE_S (60 * 60)      /* finished uploads stay on screen this long */
#define FORGET_IDLE_S (24 * 3600)  /* an abandoned one is forgotten after a day */
#define ART_MAX (25LL << 20)       /* a poster, not a photo archive */
#define PIC_IDX_MAX (1 << 20)      /* a VobSub's .idx text (a line per subpicture: ~100 KB for a film) */
#define PLAYALL_HEAD (1LL << 20)   /* of a play-all, just this much: its header, with the duration */

enum { UP_QUEUED, UP_SKIPPED, UP_WRITING, UP_DONE, UP_FAILED };
/* a play-everywhere copy the browser makes first: size unknown yet / known / it couldn't, the original goes */
enum { CV_NONE, CV_WAIT, CV_READY, CV_ORIGINAL };

typedef struct {
    char rel[400]; /* below the root, '/' separators, checked */
    int64_t size, saved;
    int state;
    bool head;    /* only the first `size` bytes of the browser's file: a play-all's header */
    char why[160]; /* why it was skipped or failed (or, CV_ORIGINAL, why no copy was made): why_set */
    int src;      /* the browser's file this comes from: its own index, or the original's for an added copy */
    int conv, pm; /* CV_*; the browser's progress converting it, 0..1000 */
    bool extra;   /* a copy added next to its original ("also send originals"): dropped when it can't be made */
    bool plays;   /* CV_ORIGINAL because it plays everywhere as it is (2.5: an MP4 the converter needn't touch): not "sent as it is" */
    bool dupe;    /* skipped: the library has this episode or film under another name */
    bool on_server; /* skipped midway: the server's converter makes this copy (answered like a sent file) */
    bool out;     /* skipped before it had a place: a file the library doesn't take, a bad name ... (the preview) */
    int64_t orig; /* the original's size (copies) */
    char ext[8];  /* the original's extension, to fall back to its name */
    /* (2.6) a "here" batch's: the library file the browser converts (fetched as /media/file/<from>?src=1 - HEVC made
       H.264), the video it's for (whose page it is: the same, or the original whose HEVC copy is read), and whether its
       copy may take the place of the file there (Hemma's own HEVC .web.mp4) */
    uint64_t from, video;
    bool replace;
    /* (2.7) its copy is in place: upload_tick does what Hemma's own queue does after a conversion - with "Do that by
       itself after each conversion" ticked, the original goes (convert_replace_original's checks) */
    bool settle;
    /* (2.7) Full throttle on the device: a video made in parts side by side - its `parts` entries follow the videos,
       each part_of = 1 + its video's entry, cut = its stretch (ms after the first picture; to -1 = the end), saved as
       convert_piece_path(its copy, k); once they're all here, upload_tick has the server join them (joining) */
    int parts, part_of, part_k;
    int64_t cut_from, cut_to, dur_ms;
    bool joining;
    /* SUBTITLES ONLY (1.3): a video that stays behind - the library has it (dupe), or its copy goes instead -
       still gives its subtitles: the browser reads them out of the original (file src) and sends them as
       entry i, saved next to `target`. A dupe's own subtitle files in the batch go next to the library's
       video instead (rehome), and then nothing is read (sidecars). */
    bool subs, sidecars;
    int lib_subs;      /* a dupe: subtitle files the library's video has already (any = nothing read) */
    char target[720];  /* the video its subtitles go next to, root/rel in full (a dupe: the library's) */
    int64_t touched;   /* unix seconds: its bytes last arrived, or it was saved (the card, 1.5) */
    /* being made (or read for its subtitles) on the sending device: how much of the original its converter has
       taken in so far (its reports' fed=), when that was counted (monotonic ms) and how fast - the card's "23 MB/s"
       (1.5; owner: "no like indication on if it's actually doing something") */
    int64_t fed, fed_at;
    double pace;
    bool paced;
} UpFile;

typedef struct {
    uint64_t id;
    int person;
    uint64_t art; /* a picture for this series/film (one file, replaces the old one); 0 = videos */
    char root[300];
    UpFile *files;
    int nfiles;
    int64_t total, moved;         /* bytes to send in this batch / received so far */
    int64_t started, last, ended; /* unix seconds */
    int64_t busy; /* the browser's last sign of work on it (a PUT, a copy's progress): 0 = none yet */
    int64_t wrote; /* unix seconds: bytes last arrived (the speed is measured while they do) */
    int workers, current, done, skipped, failed;
    bool stopped;
    double rate;                 /* bytes per second, smoothed (main thread) */
    int64_t rate_at, rate_moved; /* last speed sample: monotonic ms, moved */
} Batch;

/* Processing speed: measured over at least this long (the converter reads ahead in runs of up to 32 MB, so a
   few seconds' worth evens out), and shown while reports keep coming (the browser's heartbeat sends one every
   10 s even when nothing moved - a stuck converter's speed then falls towards 0) */
#define PACE_WINDOW_MS 4000
#define PACE_SHOW_MS 30000
static int g_pace_window_ms = PACE_WINDOW_MS;

/* Workers update progress, so everything below is guarded by g_lock. Lock order: g_lock may
   be held while asking convert.c (which takes its own), never the other way round. */
static Lock g_lock = LOCK_INIT;
static Batch *g_batch[MAX_BATCHES];
static int g_workers;
/* files saved ever - a finished batch can be replaced before a tick sees it - so the tick
   rescans when these grow */
static int g_files_saved, g_pictures_saved;
static atomic_bool g_halt;
/* main thread only */
static int64_t g_rescan_at, g_rescan_first, g_since; /* g_since: when this server started */
static uint64_t g_shown;
static int g_done_seen, g_pictures_seen;

void upload_test_free_space(int64_t bytes) { media_test_free_space(bytes); }

/* ---------------------------------------------------------------- helpers */

/* A reason kept for the cards: the English (marked N_ where it's written) packed with its values,
   shown in each viewer's language (why_of). A name in it is cut first, so the whole fits. (No format
   attribute: the worker passes its English along as a variable.) */
static void why_set(UpFile *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    i18n_vpack(f->why, sizeof f->why, fmt, ap);
    va_end(ap);
}

static const char *why_of(const UpFile *f, char *out, size_t cap) {
    i18n_unpack_to(out, cap, f->why);
    return out;
}

static const Asset *asset_named(const char *path) { /* a file baked into this binary (tools/embed.c) */
    for (int i = 0; i < g_nassets; i++)
        if (g_assets[i].path && str_eq(g_assets[i].path, path)) return &g_assets[i];
    return NULL;
}

bool upload_can_convert(void) { return asset_named("/av.wasm") && asset_named("/conv.js"); }

bool upload_ram_chosen(int person) { return str_eq(pref(person, PREF_UPLOAD_RAM, ""), "1"); }

int upload_room_gb(int person, int *lo, int *hi) {
    if (lo) *lo = ROOM_MIN;
    if (hi) *hi = ROOM_MAX;
    return (int)CLAMP(str_int_or(pref(person, PREF_UPLOAD_ROOM, ""), ROOM_DEFAULT), ROOM_MIN, ROOM_MAX);
}

void upload_set_room(int person, int gb) {
    char v[16];
    snprintf(v, sizeof v, "%d", CLAMP(gb, ROOM_MIN, ROOM_MAX));
    pref_set(person, PREF_UPLOAD_ROOM, v);
}

void upload_converter_attrs(Ctx *c, Buf *b) {
    const Asset *wasm = asset_named("/av.wasm"), *worker = asset_named("/conv.js");
    if (!wasm || !worker) return;
    H(b, " data-convert=\"/static/conv.js?v=%s\" data-wasm=\"/static/av.wasm?v=%s\"", worker->etag, wasm->etag);
    int n = convert_device_workers(c); /* (2.7: this device at its person's power tier) */
    if (n > 0) H(b, " data-workers=\"%d\"", n);
}

static bool ieq3(const char *a, const char *b) {
    for (int i = 0; i < 3; i++)
        if ((a[i] | 0x20) != b[i]) return false;
    return true;
}

/* One path part as Windows, macOS and Linux will all store it. */
static bool part_ok(const char *s, size_t n) {
    if (!n || n > 200) return false;
    if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return false; /* Windows drops these silently */
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7F || strchr("\\:*?\"<>|", ch)) return false;
    }
    size_t stem = strcspn(s, ".");
    if (stem > n) stem = n;
    if (stem == 3 && (ieq3(s, "con") || ieq3(s, "prn") || ieq3(s, "aux") || ieq3(s, "nul"))) return false;
    if (stem == 4 && (ieq3(s, "com") || ieq3(s, "lpt")) && s[3] >= '1' && s[3] <= '9') return false;
    return utf8_valid(s, n);
}

bool upload_clean_rel(const char *in, char *out, size_t cap) {
    if (!in || !*in || *in == '/' || *in == '\\') return false; /* nothing absolute */
    Buf b = {0};
    bool ok = true;
    for (const char *p = in; ok && *p;) {
        size_t n = strcspn(p, "/\\");
        ok = part_ok(p, n); /* also refuses "", "." and ".." (they end in '.') */
        if (ok) {
            if (b.len) buf_putc(&b, '/');
            buf_put(&b, p, n);
        }
        p += n;
        if (*p) p++;
    }
    ok = ok && b.len && b.len < cap;
    if (ok) memcpy(out, b.p, b.len + 1);
    buf_free(&b);
    return ok;
}

static void join(char *out, size_t cap, const char *root, const char *rel) {
    size_t rl = strlen(root);
    bool slash = rl && (root[rl - 1] == '/' || root[rl - 1] == '\\');
    snprintf(out, cap, "%s%s%s", root, slash ? "" : "/", rel);
}

static const char *tail2(const char *rel) { /* "Show/Season 1/Disc - 1/x.mkv" -> "Disc - 1/x.mkv" */
    const char *last = strrchr(rel, '/');
    if (!last) return rel;
    for (const char *p = last - 1; p >= rel; p--)
        if (*p == '/') return p + 1;
    return rel;
}

static bool junk(const char *rel) { /* system clutter that comes along with a picked folder */
    const char *name = strrchr(rel, '/');
    name = name ? name + 1 : rel;
    return name[0] == '.' || str_ieq(name, "Thumbs.db") || str_ieq(name, "desktop.ini");
}

static int64_t part_size(const Batch *b, const UpFile *f, int64_t *mtime) {
    char path[1024];
    int64_t size = 0;
    join(path, sizeof path - sizeof MEDIA_PART_SUFFIX, b->root, f->rel);
    strcat(path, MEDIA_PART_SUFFIX);
    return path_stat(path, &size, mtime, NULL) ? size : 0;
}

static void drop_part(const Batch *b, const UpFile *f) {
    char path[1024];
    join(path, sizeof path - sizeof MEDIA_PART_SUFFIX, b->root, f->rel);
    strcat(path, MEDIA_PART_SUFFIX);
    file_remove(path);
}

/* Where file f continues: its half-sent MEDIA_PART_SUFFIX file, unless that can't be this file - longer than
   it (a leftover from another file of that name), a picture (always one name, poster.jpg,
   so a leftover is likely another picture), or a copy's from before this server started (the
   browser converts it again, maybe somewhere else: its bytes needn't match - a copy whose upload
   a restart cut off is sent again): then the leftover goes and it starts over. */
static int64_t resume_from(const Batch *b, const UpFile *f, bool dry) { /* dry: only say (the preview) */
    int64_t mtime = 0, have = part_size(b, f, &mtime);
    if (have && (have > f->size || b->art || (media_is_web_copy(f->rel) && mtime < g_since))) {
        if (!dry) drop_part(b, f);
        have = 0;
    }
    return have;
}

static int64_t resume_point(const Batch *b, const UpFile *f) { return resume_from(b, f, false); }

static Batch *find(uint64_t id) {
    for (int i = 0; i < MAX_BATCHES; i++)
        if (g_batch[i] && g_batch[i]->id == id) return g_batch[i];
    return NULL;
}

static void batch_free(Batch *b) {
    free(b->files);
    free(b);
}

static bool subs_due(const UpFile *f) { return f->subs && f->pm < 1000; } /* (the browser says 1000 when it's done) */

static bool batch_open(const Batch *b) {
    if (b->stopped) return false;
    for (int i = 0; i < b->nfiles; i++)
        if (b->files[i].state == UP_QUEUED || b->files[i].state == UP_WRITING || subs_due(&b->files[i])) return true;
    return false;
}

/* --------------------------------------------------------------- replies */

static void reply(Conn *conn, int status, const Buf *json) {
    conn_reply(conn, status, "application/json", json->p ? json->p : "{}", json->len, "Cache-Control: no-store\r\n");
}

static void reply_err(Conn *conn, int status, bool stop, const char *msg) {
    Buf j = {0};
    buf_puts(&j, "{\"error\":");
    json_str(&j, msg);
    buf_printf(&j, ",\"stop\":%s}", stop ? "true" : "false");
    reply(conn, status, &j);
    buf_free(&j);
}

/* Worker side: send on a socket that is no longer the event loop's (5 s at most). */
static void worker_send(sock_t s, const char *p, size_t n) {
    for (int waited = 0; n && waited < 5000;) {
        long k = sock_send(s, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (k < 0 && !sock_again(sock_errno())) break;
        pollfd_t pf = {.fd = s, .events = POLLOUT};
        if (plat_poll(&pf, 1, 500) == 0) waited += 500;
    }
}

/* After refusing a body midway: read (and drop) what the browser is still sending for a
   moment, so it gets to read the answer ("disk full") instead of a reset connection. */
static void drain(sock_t s, int64_t left) {
    char tmp[16384];
    left = MIN(left, 64LL << 20);
    for (int64_t end = now_ms() + 2000; left > 0 && now_ms() < end;) {
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) {
            left -= k;
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) break;
        pollfd_t pf = {.fd = s, .events = POLLIN};
        plat_poll(&pf, 1, 100);
    }
}

static void worker_reply(sock_t s, int status, const char *json) {
    Buf b = {0};
    http_reply(&b, status, "application/json", json, strlen(json), "Cache-Control: no-store\r\n", false);
    worker_send(s, b.p, b.len);
    buf_free(&b);
}

/* DVD rips carry a "play all" title per disc: every episode on the disc again, as one
   file (the owner's season 1 has four, 27 GB of duplicates). Sizes are all the plan
   knows, so: a video is a play-all when it is 2.5× bigger than anything else in its
   folder and at least three folder-mates of between a twelfth and half its size add
   up to about it (0.7-1.4×). A movie next to small trailers, a two-part film, a long
   special among episodes or an episode next to bonus clips never match. Folder-mates
   count whether they still need sending or are on the server already.
   The play-all's body stays behind, but an MKV's first MB goes as "<name>.playall.mkv":
   its header says how long the disc is, and that is how the library tells a 23-minute
   featurette from an episode once the play-all itself is missing (library.c). */
static bool same_dir(const char *a, const char *b) {
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;
    return la == lb && strncmp(a, b, la) == 0;
}

/* A subtitle or picture that goes with a video the library already has ("x.en.srt", "x.jpg",
   "x-poster.jpg" next to x.mkv) stays behind with it - unless a video that is sent shares the name. */
static bool goes_with(const char *rel, const char *video) {
    size_t sl = media_stem_len(video);
    return same_dir(rel, video) && strncmp(rel, video, sl) == 0 && (rel[sl] == '.' || rel[sl] == '-');
}

static bool sub_file(const char *rel) { return media_is_sub_ext(rel) || media_is_vobsub_data(rel); }

/* A subtitle file that goes with `video_rel` (as named in the batch) goes next to the library's video `lib`
   (root/rel in full) instead, named after it: "x.sv.srt" of x.mkv -> "<that video's name>.sv.srt" (1.3).
   False when it can't (in another library folder, a name too long); true and skipped when one of that
   name is there already. */
static bool rehome(Batch *b, UpFile *f, const char *video_rel, const char *lib, bool dry) {
    size_t rl = strlen(b->root);
    if (!lib[0] || strncmp(lib, b->root, rl) != 0 || lib[rl] != '/') return false;
    const char *lrel = lib + rl + 1, *name = strrchr(lrel, '/');
    char to[440], full[1024];
    int n = snprintf(to, sizeof to, "%.*s%s", (int)media_stem_len(lrel), lrel, f->rel + media_stem_len(video_rel));
    if (n < 0 || n > 380) return false;
    join(full, sizeof full, b->root, to);
    char shown[100]; /* (cut at a whole character) */
    str_copy(shown, sizeof shown, name ? name + 1 : lrel);
    if (path_stat(full, NULL, NULL, NULL)) {
        f->state = UP_SKIPPED;
        why_set(f, N_("%s has these subtitles already"), shown);
        return true;
    }
    str_copy(f->rel, sizeof f->rel, to);
    f->saved = resume_from(b, f, dry);
    why_set(f, N_("goes next to %s, which the library has"), shown);
    return true;
}

static void skip_companions(Batch *b, bool dry) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        /* (an audiobook's file is never a video's: it goes on its own) */
        if (f->state != UP_QUEUED || media_is_video_ext(f->rel) || media_is_audio_ext(f->rel)) continue;
        UpFile *with = NULL;
        bool kept = false;
        for (int k = 0; k < b->nfiles && !kept; k++) {
            UpFile *v = &b->files[k];
            if (k == i || !media_is_video_ext(v->rel) || !goes_with(f->rel, v->rel)) continue;
            if (v->state == UP_QUEUED) kept = true;
            else if (v->dupe && !with) with = v;
        }
        /* its subtitles the library's video lacks go next to that one (1.3: they stayed behind too) */
        if (with && !kept && sub_file(f->rel) && rehome(b, f, with->rel, with->target, dry)) {
            with->sidecars = true;
            continue;
        }
        if (with && !kept) {
            char name[100]; /* (cut at a whole character) */
            const char *n = strrchr(with->rel, '/');
            str_copy(name, sizeof name, n ? n + 1 : with->rel);
            why_set(f, N_("goes with %s, which the library has"), name);
            f->state = UP_SKIPPED;
        }
    }
}

/* A DVD VobSub is two files: "x.en.idx" (the index) and "x.en.sub" (the pictures). The other half of file
   i's pair in the batch (the same name but its extension, in either case), or -1. */
static int vobsub_mate(const Batch *b, int i) {
    const char *rel = b->files[i].rel;
    bool sub = media_is_vobsub_data(rel);
    if (!sub && !str_iends(rel, ".idx")) return -1;
    size_t n = strlen(rel);
    for (int k = 0; k < b->nfiles; k++) {
        const char *o = b->files[k].rel;
        if (k != i && strlen(o) == n && strncmp(o, rel, n - 4) == 0 &&
            (sub ? str_iends(o, ".idx") : media_is_vobsub_data(o)))
            return k;
    }
    return -1;
}

/* Both or neither: one half of a pair that stays behind keeps the other back too - unless that one is on
   the server already, the same size (then this one completes the pair; a different file of its name
   wouldn't be its other half). (One pass is enough: a skip made here is of a file whose other half was
   already staying behind.) */
static void vobsub_pairs(Batch *b) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        int k = f->state == UP_QUEUED ? vobsub_mate(b, i) : -1;
        if (k < 0 || b->files[k].state == UP_QUEUED) continue;
        char full[1024], name[100];
        int64_t have = -1;
        bool dir = false;
        join(full, sizeof full, b->root, b->files[k].rel);
        if (!b->files[k].out && path_stat(full, &have, NULL, &dir) && !dir && have == b->files[k].size) continue;
        const char *n = strrchr(b->files[k].rel, '/');
        str_copy(name, sizeof name, n ? n + 1 : b->files[k].rel); /* (cut at a whole character) */
        why_set(f, N_("goes with %s, which isn't sent"), name);
        f->state = UP_SKIPPED;
    }
}

static bool original_video(const UpFile *f) { return f->size > 0 && media_is_video_ext(f->rel) && !media_is_web_copy(f->rel); }

/* An MP4 or MOV (2.5: the converter reads them too - one with Dolby sound gets a copy, one that plays doesn't) */
static bool is_mp4_name(const char *rel) {
    return !media_is_web_copy(rel) && (str_iends(rel, ".mp4") || str_iends(rel, ".m4v") || str_iends(rel, ".mov"));
}
static bool ext_mp4(const char *ext) { return str_ieq(ext, "mp4") || str_ieq(ext, "m4v") || str_ieq(ext, "mov"); }

static void play_alls(Batch *b, bool dry) {
    int *parts = xcalloc((size_t)b->nfiles + 1, sizeof *parts); /* decided first: sizes change below */
    for (int i = 0; i < b->nfiles; i++) {
        const UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || !original_video(f) || f->size < (64LL << 20)) continue;
        int n = 0;
        int64_t sum = 0, biggest = 0;
        for (int k = 0; k < b->nfiles; k++) {
            const UpFile *o = &b->files[k];
            if (k == i || !original_video(o) || !same_dir(f->rel, o->rel)) continue;
            biggest = MAX(biggest, o->size);
            if (o->size * 12 >= f->size && o->size * 2 <= f->size) {
                n++;
                sum += o->size;
            }
        }
        /* ...and it dwarfs everything else in the folder (an episode next to a few bonus
           clips that happen to add up to it does not) */
        if (n >= 3 && sum * 10 >= f->size * 7 && sum * 10 <= f->size * 14 && f->size * 10 >= biggest * 25) parts[i] = n;
    }
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (!parts[i]) continue;
        char head[440], full[1024];
        size_t stem = strlen(f->rel) - 4;
        snprintf(head, sizeof head, "%.*s.playall.mkv", (int)stem, f->rel);
        if (!str_iends(f->rel, ".mkv") || strlen(head) > 380) {
            f->state = UP_SKIPPED;
            why_set(f, N_("looks like a \"play all\" copy of the %d episodes next to it"), parts[i]);
            continue;
        }
        str_copy(f->rel, sizeof f->rel, head);
        f->size = MIN(f->size, PLAYALL_HEAD);
        f->head = true;
        int64_t have = 0;
        bool dir = false;
        join(full, sizeof full, b->root, f->rel);
        if (path_stat(full, &have, NULL, &dir)) {
            f->state = UP_SKIPPED;
            if (dir || have != f->size) why_set(f, N_("a different file with that name is already there"));
            else why_set(f, N_("a \"play all\" copy of the %d episodes next to it - its header is there"), parts[i]);
            continue;
        }
        f->saved = resume_from(b, f, dry);
    }
    free(parts);
}

/* Is root/rel another upload's? Being written by any, or still to come in someone else's open
   upload (a person's own earlier upload is replaced by their new plan - unless it's sending,
   and then the new plan is refused anyway). */
#define ACTIVE_S 30 /* an upload whose browser did something this recently is running */

/* Running: sending now, or its browser worked on it in the last ACTIVE_S seconds (making
   copies, between files). A closed tab's upload stops counting after that, and picking the
   same files again resumes it. Pictures don't count. Lock held. */
static bool running(const Batch *o) {
    return o && !o->art && (o->workers || (batch_open(o) && o->busy && now_sec() - o->busy < ACTIVE_S));
}

/* Is root/rel another upload's? Being written, or still to come in a running upload. (A
   person's earlier upload that isn't running is replaced by their new plan.) */
static bool claimed(const char *root, int me, const char *rel) {
    (void)me;
    bool mine = false;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *o = g_batch[s];
        if (!o || o->stopped || !str_eq(o->root, root)) continue;
        bool live = running(o);
        for (int p = 0; p < o->nfiles && !mine; p++) {
            const UpFile *x = &o->files[p];
            mine = (x->state == UP_WRITING || (x->state == UP_QUEUED && live)) && str_eq(x->rel, rel);
        }
    }
    lock_give(&g_lock);
    return mine;
}

/* Running, or planned a moment ago and not yet heard from: what the converter must keep its
   hands off, and whose bytes still to come hold room on the disk (lock held). */
static bool live(const Batch *o) {
    return o && !o->art && !o->stopped && batch_open(o) && (running(o) || now_sec() - o->started < ACTIVE_S);
}

static int64_t batch_hold(const Batch *b);

/* What live uploads have left to send (lock held), except `skip` and `me`'s own uploads that
   aren't running (a new plan of theirs replaces those). */
static int holds_locked(MediaHold *out, int max, const Batch *skip, int me) {
    int n = 0;
    for (int s = 0; s < MAX_BATCHES && n < max; s++) {
        const Batch *o = g_batch[s];
        if (!live(o) || o == skip || (me && o->person == me && !running(o))) continue;
        str_copy(out[n].root, sizeof out[n].root, o->root);
        out[n].bytes = batch_hold(o);
        n++;
    }
    return n;
}

int upload_holds(MediaHold *out, int max) {
    lock_take(&g_lock);
    int n = holds_locked(out, max, NULL, 0);
    lock_give(&g_lock);
    return n;
}

/* Spare room for upload b's files below root: past the reserve, running conversions and other
   live uploads (`me`: leave out that person's uploads a new plan replaces). The holds are
   copied under the lock, the disk is asked outside it: a slow disk mustn't stall the uploads
   or the pages. Lock NOT held. */
static int64_t spare_for(const Batch *b, const char *root, int me, int64_t *held) {
    MediaHold holds[MEDIA_MAX_HOLDS];
    lock_take(&g_lock);
    int n = holds_locked(holds, MEDIA_MAX_HOLDS, b, me);
    lock_give(&g_lock);
    n += convert_holds(holds + n, MEDIA_MAX_HOLDS - n);
    return media_room(root, holds, n, held);
}

/* "No room for the copy: it needs 4 GB, and the drive has 2 GB to spare (Hemma keeps 1 GB free)" - in the
   request's language */
enum { ROOM_REST, ROOM_COPY, ROOM_ORIGINAL };
static void no_room(char *out, size_t cap, int what, int64_t need, int64_t spare, int64_t held) {
    char n[24], tail[200];
    media_size_text(need, n, sizeof n);
    media_room_text(tail, sizeof tail, spare, held);
    snprintf(out, cap,
             what == ROOM_REST   ? T("No room for the rest of this file: it needs %s, and %s")
             : what == ROOM_COPY ? T("No room for the copy: it needs %s, and %s")
                                 : T("No room for the original: it needs %s, and %s"),
             n, tail);
}

bool upload_claims(const char *path) {
    bool mine = false;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *o = g_batch[s];
        size_t rl = o ? strlen(o->root) : 0;
        if (!o || !str_istarts(path, o->root)) continue;
        const char *rel = path + rl; /* path = root/rel, as join() makes it */
        if (rl && o->root[rl - 1] != '/' && *rel++ != '/') continue;
        bool coming = live(o);
        for (int p = 0; p < o->nfiles && !mine; p++) {
            const UpFile *x = &o->files[p];
            mine = (x->state == UP_WRITING || (x->state == UP_QUEUED && coming)) && str_ieq(x->rel, rel);
        }
    }
    lock_give(&g_lock);
    return mine;
}

bool upload_claims_under(const char *dir) {
    bool mine = false;
    size_t dl = strlen(dir);
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *o = g_batch[s];
        if (!o) continue;
        bool coming = live(o);
        for (int p = 0; p < o->nfiles && !mine; p++) {
            const UpFile *x = &o->files[p];
            if (x->state != UP_WRITING && !(x->state == UP_QUEUED && coming)) continue;
            char path[1024];
            join(path, sizeof path, o->root, x->rel);
            mine = str_istarts(path, dir) && (path[dl] == '/' || path[dl] == '\0');
        }
    }
    lock_give(&g_lock);
    return mine;
}

static bool listed(const Batch *b, const char *rel) {
    for (int k = 0; k < b->nfiles; k++)
        if (str_eq(b->files[k].rel, rel)) return true;
    return false;
}

/* A folder converted by Hemma holds "x.mkv" next to "x.web.mp4": the copy is what plays,
   at about a third of the size, so unless asked for the originals only the copy goes - and the
   original's subtitles with it (subtitles_only), unless subtitle files of its own come along. */
static void skip_originals(Batch *b) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || !media_is_video_ext(f->rel) || media_is_web_copy(f->rel)) continue;
        char copy[420];
        convert_web_path(f->rel, copy, sizeof copy);
        if (listed(b, copy)) {
            f->state = UP_SKIPPED;
            why_set(f, N_("its play-everywhere copy (.web.mp4) goes instead"));
            join(f->target, sizeof f->target, b->root, copy);
            for (int k = 0; k < b->nfiles && !f->sidecars; k++)
                f->sidecars = k != i && b->files[k].state == UP_QUEUED && sub_file(b->files[k].rel) && goes_with(b->files[k].rel, f->rel);
        }
    }
}

/* A subtitle file with no video of the batch to go with: the video it's named for, if the library has it
   ("Friends S01E01.sv.srt" -> Friends S01E01, whatever its file is called) - it goes next to that one
   (1.3). Tried from the shortest name up ("Show.S01E01.720p.en.srt": "Show.S01E01.720p", then "...720p.en" -
   media_sub_for takes only names whose rest is tags, so the first that fits keeps every tag). */
static void lone_subtitles(Batch *b, bool dry) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || !sub_file(f->rel)) continue;
        bool with = false;
        for (int k = 0; k < b->nfiles && !with; k++)
            with = k != i && media_is_video_ext(b->files[k].rel) && goes_with(f->rel, b->files[k].rel);
        if (with) continue;
        char video[420], as[420], lib[720];
        str_copy(as, sizeof as, f->rel);
        if (media_is_vobsub_data(as)) memcpy(as + strlen(as) - 4, ".idx", 4); /* (a .sub is read as its .idx) */
        const char *name = strrchr(f->rel, '/'), *ext = strrchr(f->rel, '.');
        bool done = false;
        for (const char *dot = strchr(name ? name + 1 : f->rel, '.'); !done && dot && dot <= ext; dot = strchr(dot + 1, '.')) {
            snprintf(video, sizeof video, "%.*s.mkv", (int)(dot - f->rel), f->rel);
            if (media_sub_for(video, as, NULL, 0) && media_have_file(b->root, video, NULL, 0, lib, sizeof lib, NULL))
                done = rehome(b, f, video, lib, dry);
        }
    }
}

/* The originals that stay behind and give their subtitles (1.3: subtitles only): MKV/WebM ones - what the
   browser's converter reads - when the browser can run it, the video they go with lacks subtitles of its
   own (a dupe whose library video has some is left alone: no reading a whole file for nothing), and no
   subtitle files of the batch go along instead. */
static void subtitles_only(Batch *b, bool conv) {
    for (int i = 0; conv && i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (f->state != UP_SKIPPED || !f->target[0] || f->sidecars || f->lib_subs) continue;
        bool mkv = str_iends(f->rel, ".mkv") || str_iends(f->rel, ".webm") ||
                   (media_is_web_copy(f->rel) && (str_ieq(f->ext, "mkv") || str_ieq(f->ext, "webm"))); /* (convert_here's) */
        if (!mkv) continue;
        const char *name = strrchr(f->target, '/');
        char shown[100]; /* (cut at a whole character) */
        str_copy(shown, sizeof shown, name ? name + 1 : f->target);
        f->subs = true;
        if (f->dupe) why_set(f, N_("the library has it (%s): only the subtitles inside it go, next to that one"), shown);
        else if (media_is_web_copy(f->rel)) why_set(f, N_("its copy is there already: only the subtitles inside the original go"));
        else why_set(f, N_("its copy goes instead, and the subtitles inside it go along"));
    }
}

/* Without such a copy, an MKV (DVD rips, Dolby sound - what browsers can't all play) is
   converted by the uploading browser itself: web/conv.js runs av/conv.c as WebAssembly on
   that device, and only "x.web.mp4" travels - a third of the bytes over the network, and
   the original never takes room here. Its size is known once the copy exists (?size=);
   one the converter can't handle comes as it is (?original=1). With "also send originals"
   the copy is an added entry next to the original. */
static bool convertible(const Batch *b, const UpFile *f, char *copy, size_t cap) {
    if (f->state != UP_QUEUED || f->head || f->conv || !original_video(f)) return false;
    /* the demuxers we have: Matroska, and (2.5) MP4/MOV - an MP4 that plays as it is comes back as it is, quietly */
    if (!str_iends(f->rel, ".mkv") && !str_iends(f->rel, ".webm") && !is_mp4_name(f->rel)) return false;
    convert_web_path(f->rel, copy, cap);
    return strlen(copy) <= 380 && !listed(b, copy); /* a copy among the chosen files goes as it is */
}

/* Subtitle files next to a video on the disk (root/rel in full), named for it ("x.en.srt" of x.web.mp4) */
static int subs_beside(const char *full) {
    const char *slash = strrchr(full, '/');
    if (!slash) return 0;
    char dir[1024];
    str_ncopy(dir, sizeof dir, full, (size_t)(slash - full));
    DirIter *it = dir_open(dir);
    DirEntry e;
    int n = 0;
    while (it && dir_next(it, &e)) n += !e.is_dir && media_sub_for(slash + 1, e.name, NULL, 0);
    if (it) dir_close(it);
    return n;
}

static void convert_here(Batch *b, bool keep_originals, int me) {
    char copy[420], full[1024];
    int n = b->nfiles, add = 0;
    for (int i = 0; i < n; i++) add += convertible(b, &b->files[i], copy, sizeof copy);
    if (!add) return;
    if (keep_originals) {
        b->files = xrealloc(b->files, (size_t)(n + add) * sizeof *b->files);
        memset(b->files + n, 0, (size_t)add * sizeof *b->files);
    }
    for (int i = 0; i < n; i++) {
        if (!convertible(b, &b->files[i], copy, sizeof copy)) continue; /* (fewer than counted, never more) */
        UpFile *o = &b->files[i], *f = keep_originals ? &b->files[b->nfiles++] : o;
        f->orig = o->size;
        str_copy(f->ext, sizeof f->ext, strrchr(o->rel, '.') + 1);
        f->src = i;
        f->extra = f != o;
        str_copy(f->rel, sizeof f->rel, copy);
        f->size = f->saved = 0;
        f->pm = 0;
        join(full, sizeof full, b->root, copy);
        bool there = path_stat(full, NULL, NULL, NULL), server = !there && convert_making(full);
        if (there || server || claimed(b->root, me, copy)) {
            f->state = UP_SKIPPED;
            why_set(f, there    ? N_("its play-everywhere copy is already there")
                       : server ? N_("Hemma is making its play-everywhere copy on the server right now")
                                : N_("another upload is sending its copy right now"));
            if (there && f == o) { /* (the copy there may lack the original's subtitles: subtitles_only) */
                str_copy(f->target, sizeof f->target, full);
                f->lib_subs = subs_beside(full);
            }
            continue;
        }
        f->state = UP_QUEUED;
        f->conv = CV_WAIT;
    }
}

/* DVD copies come out at about a third: half is on the safe side; H.264 made of HEVC (2.6) is often a little bigger */
static int64_t copy_guess(const UpFile *f) { return f->from ? f->orig + f->orig / 4 : f->orig / 2; }

/* Bytes a queued file still has to send (a copy not made yet: a guess). */
static int64_t left_to_send(const UpFile *f) { return f->conv == CV_WAIT ? copy_guess(f) : MAX(f->size - f->saved, 0); }

/* The room a batch holds on its disk (lock held, or b not shared yet): what it has left to send - but a "here" batch's
   (2.6) only what's under way, at least its biggest still to come: the browser makes a few at a time, and each copy takes
   the place of one that's there (every guess of a whole series at once refused "All of them" on a disk with room) */
static int64_t batch_hold(const Batch *b) {
    if (!b->nfiles || !b->files[0].from) return MAX(b->total - b->moved, 0);
    int64_t sum = 0, biggest = 0;
    for (int i = 0; i < b->nfiles; i++) {
        const UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED && f->state != UP_WRITING) continue;
        int64_t left = left_to_send(f);
        biggest = MAX(biggest, left);
        if (f->state == UP_WRITING || f->conv == CV_READY || (f->conv == CV_WAIT && f->pm > 0)) sum += left;
    }
    return MAX(sum, biggest);
}

#define MAKING_IT N_("Hemma is making this copy on the server right now")
#define MADE_MEANWHILE N_("Hemma started making this copy on the server meanwhile")

/* The server's converter is making this copy, so it comes from there: a plan leaves it out, and
   an upload that went quiet and comes back hears "sent" for it and goes on with the rest. Its
   half-sent file from an earlier try goes. Lock held (or b not shared yet), and f has no worker.
   meanwhile: the converter started on it after the plan. */
static void leave_to_server(Batch *b, UpFile *f, bool meanwhile) {
    b->total -= left_to_send(f);
    drop_part(b, f);
    f->state = UP_SKIPPED;
    f->on_server = true;
    b->skipped++;
    why_set(f, meanwhile ? MADE_MEANWHILE : MAKING_IT);
}

/* Room on the disk for what b has to send (spare_for); false with the reason in err (in the request's
   language). Lock NOT held. */
static bool room_for(const Batch *b, int me, char *err, size_t cap) {
    int64_t held = 0, spare = spare_for(b, b->root, me, &held), hold = batch_hold(b);
    if (hold <= spare) return true;
    char need[24], tail[200];
    media_size_text(hold, need, sizeof need);
    media_room_text(tail, sizeof tail, spare, held);
    snprintf(err, cap, T("Not enough room: this needs %s, and %s"), need, tail);
    return false;
}

/* ------------------------------------------------------------------- plan */

/* {"art":"<series or film id>","files":[["My picture.jpeg",123]]}: one picture, saved as
   "<show folder>/poster.jpg" or "<film folder>/<film name>.jpg" over whatever was there. */
static bool art_plan(Json *j, int at, Batch **out, char *err, size_t cap) {
    char hex[40] = "", dir[400], stem[400], raw[1024] = "";
    uint64_t id = 0;
    int root = -1;
    json_string(j, at, hex, sizeof hex);
    if (!media_parse_id(hex, &id) || !media_art_target(id, &root, dir, sizeof dir, stem, sizeof stem) ||
        root < 0 || root >= media_lib()->nroots) {
        str_copy(err, cap, T("Pictures go with a series or a film in the library - this one isn't there any more"));
        return false;
    }
    int arr = json_get(j, 0, "files");
    int n = json_type(j, arr) == JSON_ARRAY ? j->tok[arr].count : 0;
    if (n != 1) {
        str_copy(err, cap, n ? T("Choose just one picture") : T("Choose a picture first"));
        return false;
    }
    long long size = -1;
    int k = json_child(j, arr);
    json_string(j, json_index(j, k, 0), raw, sizeof raw);
    json_integer(j, json_index(j, k, 1), &size);
    if (!media_is_image_ext(raw)) {
        str_copy(err, cap, T("Pictures can be JPEG, PNG or WebP"));
        return false;
    }
    if (size <= 0 || size > ART_MAX) {
        str_copy(err, cap, size > 0 ? T("That picture is too big - 25 MB at most") : T("That picture is empty"));
        return false;
    }
    char ext[8];
    str_copy(ext, sizeof ext, strrchr(raw, '.') + 1);
    for (char *p = ext; *p; p++) *p = (char)(*p | 0x20);
    if (str_eq(ext, "jpeg")) str_copy(ext, sizeof ext, "jpg");
    Batch *b = xcalloc(1, sizeof *b);
    b->files = xcalloc(1, sizeof *b->files);
    b->nfiles = 1;
    b->art = id;
    str_copy(b->root, sizeof b->root, media_lib()->roots[root]);
    UpFile *f = &b->files[0];
    f->size = size;
    if (*dir) snprintf(f->rel, sizeof f->rel, "%s/%s.%s", dir, stem, ext);
    else snprintf(f->rel, sizeof f->rel, "%s.%s", stem, ext);
    f->state = UP_QUEUED;
    f->saved = resume_point(b, f);
    *out = b;
    return true;
}

/* (2.6) {"here":"<set>"}: the library's HEVC videos of a set (convert.c convert_h264_set: one, a series or season, the
   films, all) made H.264 by the browser that asks - each read from /media/file/<id>?src=1 (the plan's "remote" list),
   its copy sent back as the video's .web.mp4 like any upload's copy, taking the place of Hemma's own HEVC copy there.
   One library folder a batch (the first video's): the rest wait for the next one. */
/* (2.7; owner: "Full throttle ... processes the videos in the most effective way on each tier") On Full throttle, fewer
   videos than the device makes at once: each long one in parts side by side - its share of the device's workers, each
   part at least CONVERT_PART_MIN_S long - every part an entry of its own after the videos (sent as any copy is, to its
   own file), the video's entry joined from them by the server once they're all here (upload_tick). */
static int64_t g_part_min_ms = CONVERT_PART_MIN_S * 1000LL; /* (tests: shorter - upload_test_part_min) */
void upload_test_part_min(int64_t ms) { g_part_min_ms = ms > 0 ? ms : CONVERT_PART_MIN_S * 1000LL; }

static void here_parts(Ctx *c, Batch *b) {
    long told = strtol(env("HEMMA_TEST_PART_MS", ""), NULL, 10); /* (the browser tests' clips last a second: shorter) */
    if (told > 0) g_part_min_ms = told;
    int workers = convert_device_tier(c) == CONVERT_FULL ? convert_device_workers(c) : 0, queued = 0;
    for (int k = 0; k < b->nfiles; k++) queued += b->files[k].state == UP_QUEUED;
    int per = queued && workers > queued ? workers / queued : 1, extra = 0;
    if (per < 2) return;
    for (int k = 0; k < b->nfiles; k++) {
        UpFile *u = &b->files[k];
        int64_t p = MIN((int64_t)MIN(per, AV_CONV_MAX_PIECES), u->dur_ms / g_part_min_ms);
        u->parts = u->state == UP_QUEUED && p >= 2 && strlen(u->rel) + 20 < sizeof u->rel ? (int)p : 0;
        extra += u->parts;
    }
    if (!extra) return;
    int nvideos = b->nfiles;
    b->files = xrealloc(b->files, (size_t)(nvideos + extra) * sizeof *b->files);
    memset(b->files + nvideos, 0, (size_t)extra * sizeof *b->files);
    for (int k = 0; k < nvideos; k++) {
        UpFile *u = &b->files[k];
        for (int q = 0; q < u->parts; q++) {
            UpFile *p = &b->files[b->nfiles++];
            if (snprintf(p->rel, sizeof p->rel, "%s.p%d.conv.part", u->rel, q) >= (int)sizeof p->rel) /* (convert_piece_path's) */
                p->rel[0] = '\0'; /* (never: room was checked above) */
            str_copy(p->ext, sizeof p->ext, "mp4");
            p->orig = u->orig / u->parts, p->from = u->from, p->src = u->src;
            p->part_of = k + 1, p->part_k = q;
            p->cut_from = u->dur_ms * q / u->parts;
            p->cut_to = q == u->parts - 1 ? -1 : u->dur_ms * (q + 1) / u->parts;
            p->state = UP_QUEUED, p->conv = CV_WAIT;
        }
        /* (the video itself: being made while they are - its progress theirs, part_progress - joined, never sent) */
    }
}

/* (2.7) a part of a video a device is making in parts, at `path` (a live batch's, sent or not): not a leftover */
bool upload_has_part(const char *path) {
    bool mine = false;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *b = g_batch[s];
        for (int i = 0; b && i < b->nfiles && !mine; i++) {
            if (!b->files[i].part_of) continue;
            char full[1024];
            join(full, sizeof full, b->root, b->files[i].rel);
            mine = str_ieq(full, path);
        }
    }
    lock_give(&g_lock);
    return mine;
}

/* (2.7) a video made in parts: its progress is its parts' (a made one 1000), never going back - 999 at most, until joined */
static void part_progress(Batch *b, int v) {
    UpFile *video = &b->files[v];
    int64_t sum = 0;
    for (int q = 0; q < b->nfiles; q++) {
        const UpFile *p = &b->files[q];
        if (p->part_of == v + 1) sum += p->state == UP_DONE || p->conv == CV_READY ? 1000 : p->pm;
    }
    int pm = video->parts ? (int)CLAMP(sum / video->parts, 0, 999) : 0;
    if (pm > video->pm) video->pm = pm;
}

static bool here_plan(Ctx *c, Json *j, int at, Batch **out, char *err, size_t cap) {
    int me = c->me->id;
    char what[160] = "";
    json_string(j, at, what, sizeof what);
    enum { HERE_MAX = 4096 };
    const MediaFile **set = xcalloc(HERE_MAX, sizeof *set);
    int n = convert_h264_set(what, set, HERE_MAX);
    if (!n) {
        free(set);
        str_copy(err, cap, T("Nothing there needs converting any more"));
        return false;
    }
    const MediaIndex *ix = media_lib();
    Batch *b = xcalloc(1, sizeof *b);
    b->files = xcalloc((size_t)n, sizeof *b->files);
    str_copy(b->root, sizeof b->root, ix->roots[set[0]->root]);
    size_t rl = strlen(b->root);
    for (int k = 0; k < n; k++) {
        const MediaFile *f = set[k], *s = convert_h264_from(f);
        char src[1024], dst[1024];
        bool replace = false;
        if (f->root != set[0]->root || !s || !convert_h264_paths(f, src, sizeof src, dst, sizeof dst, &replace) ||
            strncmp(dst, b->root, rl) || dst[rl] != '/')
            continue;
        bool twice = false; /* (x.mkv and x.webm of different lengths would both make x.web.mp4: the first one does) */
        for (int p = 0; p < b->nfiles && !twice; p++) twice = str_ieq(b->files[p].rel, dst + rl + 1);
        if (twice) continue;
        UpFile *u = &b->files[b->nfiles];
        str_copy(u->rel, sizeof u->rel, dst + rl + 1);
        str_copy(u->ext, sizeof u->ext, "mp4");
        u->orig = s->size, u->from = s->id, u->video = f->id, u->replace = replace, u->src = b->nfiles;
        u->dur_ms = s->probe.duration_ms; /* (2.7: a long one may be made in parts) */
        if (convert_making(dst) || convert_queued(f->id) || claimed(b->root, me, u->rel)) {
            u->state = UP_SKIPPED;
            why_set(u, convert_queued(f->id) ? N_("Hemma is making its play-everywhere copy on the server right now")
                                             : N_("another upload is sending its copy right now"));
        } else {
            u->state = UP_QUEUED, u->conv = CV_WAIT;
        }
        b->nfiles++;
    }
    free(set);
    if (!b->nfiles) {
        batch_free(b);
        str_copy(err, cap, T("Nothing there needs converting any more"));
        return false;
    }
    here_parts(c, b); /* (2.7: Full throttle - long ones in parts side by side) */
    *out = b;
    return true;
}

/* What a plan is asked for - by the page's Start (the JSON plan) or by its preview (the live action's
   fields): read the same way from either, so both do the same. */
#define NAMES_MAX 24
#define YEARS_MAX 256 /* (a folder of films without years: one box each) */
#define FILMS_MAX 32
typedef struct {
    int root;        /* into media_roots(); -1 = none chosen */
    bool sort;       /* by the files' names (upload_sort: the page's way); else as chosen, under `into` */
    char into[256];  /* (as typed: plan_make checks it) */
    bool conv, originals, dupes, force;
    uint64_t series; /* /media/upload?series=: everything into that series' folder */
    int room;        /* the slider (GB), -1 = not said */
    bool ram;        /* copies wait in the browser's memory, not on its disk (only remembered here) */
    UpName names[NAMES_MAX]; /* series renamed on the preview ("to.<16 hex>" fields) */
    int nnames;
    UpYear years[YEARS_MAX]; /* films' years typed on the preview ("y.<16 hex>" fields: 2.4) */
    int nyears;
    uint64_t films[FILMS_MAX]; /* top folders whose videos are films ("films.<16 hex>" = 1: 2.4) */
    int nfilms;
    int n;           /* files chosen (over MAX_FILES: refused, and none are kept) */
    char **paths;
    int64_t *sizes;  /* -1: the browser didn't say */
} PlanIn;

static void plan_in_free(PlanIn *in) {
    for (int i = 0; in->paths && i < in->n; i++) free(in->paths[i]);
    free(in->paths);
    free(in->sizes);
    memset(in, 0, sizeof *in);
}

/* One option, from the JSON plan (j) or the preview's fields (f) */
typedef struct {
    const Json *j;
    const Fields *f;
} Ask;

static bool ask_str(const Ask *a, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    if (a->f) {
        const char *v = fields_get(a->f, key);
        if (v) str_copy(out, cap, v);
        return v != NULL;
    }
    int t = json_get(a->j, 0, key);
    long long v;
    if (json_type(a->j, t) == JSON_NUMBER && json_integer(a->j, t, &v)) {
        snprintf(out, cap, "%lld", v);
        return true;
    }
    return json_string(a->j, t, out, cap);
}

static bool ask_on(const Ask *a, const char *key) {
    char v[8];
    return ask_str(a, key, v, sizeof v) && str_eq(v, "1");
}

static void ask_name(PlanIn *in, const char *key, const char *val) { /* "to.<16 hex>" = "<a series' name>" */
    uint64_t k;
    if (in->nnames >= NAMES_MAX || !str_starts(key, "to.") || !media_parse_id(key + 3, &k)) return;
    UpName *nm = &in->names[in->nnames];
    str_copy(nm->name, sizeof nm->name, val);
    str_trim(nm->name);
    if (nm->name[0]) nm->key = k, in->nnames++;
}

/* A film's year that can be one: 1880 .. next year */
static int year_ok(const char *v) {
    char t[16];
    str_copy(t, sizeof t, v);
    str_trim(t);
    if (strlen(t) != 4 || t[strspn(t, "0123456789")]) return 0;
    int y = (int)str_int_or(t, 0);
    return y >= 1880 && y <= dt_local(now_sec()).year + 1 ? y : 0;
}

/* The preview's other boxes (2.4): "y.<16 hex>" = a film's year, "films.<16 hex>" = 1: that folder's videos are films */
static void ask_more(PlanIn *in, const char *key, const char *val) {
    uint64_t k;
    if (str_starts(key, "y.") && media_parse_id(key + 2, &k) && in->nyears < YEARS_MAX) {
        int y = year_ok(val);
        if (y) in->years[in->nyears++] = (UpYear){k, y};
    } else if (str_starts(key, "films.") && media_parse_id(key + 6, &k) && in->nfilms < FILMS_MAX && str_eq(val, "1")) {
        in->films[in->nfilms++] = k;
    }
}

static void plan_in(const Ask *a, PlanIn *in) {
    char tmp[300];
    in->root = ask_str(a, "root", tmp, sizeof tmp) ? (int)str_int_or(tmp, -1) : -1;
    in->sort = ask_on(a, "sort") && !ask_on(a, "keep");
    if (ask_str(a, "into", tmp, sizeof tmp)) {
        str_trim(tmp);
        str_copy(in->into, sizeof in->into, tmp);
    }
    in->conv = ask_on(a, "conv");
    in->originals = ask_on(a, "originals");
    in->dupes = ask_on(a, "dupes");
    in->force = ask_on(a, "force");
    in->room = ask_str(a, "room", tmp, sizeof tmp) ? (int)CLAMP(str_int_or(tmp, ROOM_DEFAULT), ROOM_MIN, ROOM_MAX) : -1;
    in->ram = ask_on(a, "ram");
    if (!ask_str(a, "series", tmp, sizeof tmp) || !media_parse_id(tmp, &in->series)) in->series = 0;
    if (a->f) {
        for (int i = 0; i < a->f->n; i++) {
            ask_name(in, a->f->k[i], a->f->v[i]);
            ask_more(in, a->f->k[i], a->f->v[i]);
        }
    } else {
        for (int k = json_child(a->j, 0); k >= 0; k = json_next(a->j, 0, k)) {
            char key[40], val[200];
            if (!json_string(a->j, k, key, sizeof key) || !json_string(a->j, k + 1, val, sizeof val)) continue;
            if (str_starts(key, "to.")) ask_name(in, key, val);
            else ask_more(in, key, val);
        }
    }
}

/* [[path, size], ...] */
static void files_in(const Json *j, int arr, PlanIn *in) {
    int n = json_type(j, arr) == JSON_ARRAY ? j->tok[arr].count : 0;
    in->n = n;
    if (n <= 0 || n > MAX_FILES) return; /* (plan_make says why) */
    in->paths = xcalloc((size_t)n, sizeof *in->paths);
    in->sizes = xcalloc((size_t)n, sizeof *in->sizes);
    int i = 0;
    for (int k = json_child(j, arr); k >= 0 && i < n; k = json_next(j, arr, k), i++) {
        char raw[1024] = "";
        long long size = -1;
        json_string(j, json_index(j, k, 0), raw, sizeof raw);
        json_integer(j, json_index(j, k, 1), &size);
        in->paths[i] = xstrdup(raw);
        in->sizes[i] = size;
    }
    in->n = i;
}

static void batch_count(Batch *b) { /* what's skipped, and the bytes to send */
    b->skipped = 0;
    b->total = 0;
    for (int k = 0; k < b->nfiles; k++) {
        const UpFile *f = &b->files[k];
        b->skipped += f->state == UP_SKIPPED;
        if (f->state == UP_QUEUED) b->total += left_to_send(f);
    }
}

/* What the plan does with in's files: a batch nobody shares yet. dry (the preview): nothing on the disk
   changes. sorted (may be NULL) gets upload_sort's word on each chosen file (free it). NULL, with the
   reason packed into err (i18n_pack: the plan says it in the request's words, the preview in the page's),
   when there's nothing to plan. */
static Batch *plan_make(Ctx *c, const PlanIn *in, bool dry, char *err, size_t cap, UpSorted **sorted) {
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS), ri = in->root;
    if (sorted) *sorted = NULL;
    if (in->series) { /* everything into that series' folder: in its library folder */
        const MediaIndex *ix = media_lib();
        const MediaSeries *ms = media_series(in->series);
        const MediaFile *e = ms && ms->count ? media_episode(ms, 0) : NULL;
        ri = -1;
        for (int i = 0; e && e->root < ix->nroots && i < nroots && ri < 0; i++)
            if (str_eq(ix->roots[e->root], roots[i])) ri = i;
        if (ri < 0) {
            i18n_pack(err, cap, N_("That series isn't in the library any more"));
            return NULL;
        }
    }
    if (ri < 0 || ri >= nroots) {
        i18n_pack(err, cap, N_("Choose which library folder to upload into"));
        return NULL;
    }
    if (!path_is_dir(roots[ri])) {
        i18n_pack(err, cap, N_("Can't reach the library folder %s right now"), roots[ri]);
        return NULL;
    }
    char into[256] = "";
    if (!in->sort && in->into[0] && !upload_clean_rel(in->into, into, 200)) {
        char shown[84]; /* (cut at a whole character) */
        str_copy(shown, sizeof shown, in->into);
        i18n_pack(err, cap, N_("The folder name \"%s\" has characters a disk can't store"), shown);
        return NULL;
    }
    if (in->n <= 0) {
        i18n_pack(err, cap, N_("No files were chosen"));
        return NULL;
    }
    if (in->n > MAX_FILES) {
        i18n_pack(err, cap, N_("That's more than %d files - upload a few folders at a time"), MAX_FILES);
        return NULL;
    }

    int n = in->n;
    Batch *b = xcalloc(1, sizeof *b);
    b->files = xcalloc((size_t)n, sizeof *b->files);
    b->nfiles = n;
    str_copy(b->root, sizeof b->root, roots[ri]);
    /* every name checked first, then sorted all at once: subtitles go with their video, a show's folder
       with its show (and the names say what each file is, for the preview, even when nothing's sorted) */
    char **clean = xcalloc((size_t)n, sizeof *clean);
    for (int i = 0; i < n; i++) {
        char tmp[400];
        if (in->sizes[i] >= 0 && upload_clean_rel(in->paths[i], tmp, sizeof tmp)) clean[i] = xstrdup(tmp);
    }
    UpSorted *srt = xcalloc((size_t)n, sizeof *srt);
    UpSortOpts so = {.root = b->root, .series = in->series, .names = in->names, .nnames = in->nnames,
                     .years = in->years, .nyears = in->nyears, .films = in->films, .nfilms = in->nfilms};
    upload_sort(&so, (const char *const *)clean, n, srt);
    /* episodes and films the library has under another name stay behind (before any converting) */
    for (int i = 0; i < n; i++) {
        UpFile *f = &b->files[i];
        char full[1024], check[400];
        f->state = UP_SKIPPED;
        f->src = i;
        f->out = true; /* (until it has a place) */
        str_copy(f->rel, sizeof f->rel, in->paths[i]);
        if (in->sizes[i] < 0) {
            why_set(f, N_("the browser didn't say how big it is"));
            continue;
        }
        f->size = in->sizes[i];
        if (!clean[i]) {
            why_set(f, N_("its name has characters a disk can't store"));
            continue;
        }
        if (in->sort) str_copy(f->rel, sizeof f->rel, srt[i].dest);
        else if (*into) snprintf(f->rel, sizeof f->rel, "%s/%s", into, clean[i]);
        else str_copy(f->rel, sizeof f->rel, clean[i]);
        if (strlen(f->rel) > 380) {
            why_set(f, N_("the path is too long"));
            continue;
        }
        if (in->sort && !upload_clean_rel(f->rel, check, sizeof check)) { /* (a folder named after a title) */
            why_set(f, N_("its name has characters a disk can't store"));
            continue;
        }
        if (junk(f->rel)) {
            why_set(f, N_("a system file"));
            continue;
        }
        /* (a VobSub's .sub only with its .idx in this upload - upload_sort says so; alone it's MicroDVD text) */
        bool vobsub = media_is_vobsub_data(f->rel) && srt[i].kind == UPK_SUB;
        if (!media_is_video_ext(f->rel) && !media_is_sub_ext(f->rel) && !media_is_image_ext(f->rel) && !vobsub &&
            !media_is_audio_ext(f->rel)) {
            why_set(f, N_("not a video, subtitle, poster or audiobook"));
            continue;
        }
        f->out = false;
        bool twice = false;
        for (int p = 0; p < i && !twice; p++) twice = b->files[p].state != UP_SKIPPED && str_eq(b->files[p].rel, f->rel);
        if (twice) {
            why_set(f, N_("it's on the list twice"));
            continue;
        }
        if (claimed(b->root, c->me->id, f->rel)) {
            why_set(f, N_("another upload is sending it right now"));
            continue;
        }
        int64_t have = 0;
        bool dir = false;
        join(full, sizeof full, b->root, f->rel);
        if (path_stat(full, &have, NULL, &dir)) {
            why_set(f, dir               ? N_("a folder on the server has that name")
                       : have == f->size ? N_("it's already there")
                                         : N_("a different file with that name is already there"));
            continue;
        }
        if (media_is_video_ext(f->rel) && convert_making(full)) { /* the server is making this copy: one writer */
            why_set(f, MAKING_IT);
            continue;
        }
        char found[400], have_it[120];
        if (!in->dupes && media_is_video_ext(f->rel) &&
            media_have_file(b->root, f->rel, found, sizeof found, f->target, sizeof f->target, &f->lib_subs)) {
            str_copy(have_it, sizeof have_it, tail2(found)); /* (cut at a whole character) */
            why_set(f, N_("the library has it: %s"), have_it);
            f->dupe = true;
            continue;
        }
        f->state = UP_QUEUED;
        f->saved = resume_from(b, f, dry);
    }
    for (int i = 0; i < n; i++) free(clean[i]);
    free(clean);
    skip_companions(b, dry);
    lone_subtitles(b, dry);
    vobsub_pairs(b);
    play_alls(b, dry);
    if (!in->originals) skip_originals(b);
    if (in->conv) convert_here(b, in->originals, c->me->id);
    subtitles_only(b, in->conv);
    batch_count(b);
    if (sorted) *sorted = srt;
    else free(srt);
    return b;
}

/* File i in the plan's answer: [i, from] / [i, from, end] / {"i": i, "f": the browser's file to convert} /
   {"i": i, "f": its file, "subs": 1}: only the subtitles inside it (subtitles_only) */
static void todo_entry(Buf *out, const Batch *b, int i, int *k) {
    const UpFile *f = &b->files[i];
    if (f->subs) {
        buf_printf(out, "%s{\"i\":%d,\"f\":%d,\"subs\":1}", (*k)++ ? "," : "", i, f->src);
        return;
    }
    if (f->conv == CV_WAIT && f->part_of) { /* (2.7) only part [from, to] of file f's picture (ms; to -1 = the end) */
        buf_printf(out, "%s{\"i\":%d,\"f\":%d,\"part\":[%lld,%lld],\"share\":%lld}", (*k)++ ? "," : "", i, f->src,
                   (long long)f->cut_from, (long long)f->cut_to, (long long)f->orig);
        return;
    }
    if (f->conv == CV_WAIT) { /* the browser converts its file f, then sends the copy as entry i */
        buf_printf(out, "%s{\"i\":%d,\"f\":%d}", (*k)++ ? "," : "", i, f->src);
        return;
    }
    buf_printf(out, "%s[%d,%lld", (*k)++ ? "," : "", i, (long long)f->saved);
    if (f->head) buf_printf(out, ",%lld", (long long)f->size); /* the browser sends bytes [saved, size) */
    buf_putc(out, ']');
}

static void plan(Ctx *c, Conn *conn, HttpReq *r) {
    int maxtok = 64 + MAX_FILES * 3;
    JsonTok *tok = xmalloc((size_t)maxtok * sizeof *tok);
    Json j;
    Batch *b = NULL;
    PlanIn in = {0};
    char err[400] = "", packed[400] = ""; /* (err in the request's language: the browser shows it) */
    if (!r->body || !json_parse(&j, r->body, r->body_len, tok, maxtok) || json_type(&j, 0) != JSON_OBJECT) {
        snprintf(err, sizeof err, T("That upload list didn't make sense (more than %d files?)"), MAX_FILES);
        goto out;
    }
    int art = json_get(&j, 0, "art"), here = json_get(&j, 0, "here");
    if (art >= 0) {
        if (!art_plan(&j, art, &b, err, sizeof err)) goto out;
        batch_count(b);
    } else if (here >= 0) {
        if (!here_plan(c, &j, here, &b, err, sizeof err)) goto out;
        batch_count(b);
    } else {
        Ask a = {.j = &j};
        plan_in(&a, &in);
        files_in(&j, json_get(&j, 0, "files"), &in);
        if (!(b = plan_make(c, &in, false, packed, sizeof packed, NULL))) {
            i18n_unpack_to(err, sizeof err, packed);
            goto out;
        }
        if (in.room >= 0) { /* the slider's choice, for next time */
            char v[12];
            snprintf(v, sizeof v, "%d", in.room);
            pref_set(c->me->id, PREF_UPLOAD_ROOM, v);
        }
        if (in.conv) pref_set(c->me->id, PREF_UPLOAD_RAM, in.ram ? "1" : "0"); /* (the box too, when converting) */
    }
    if (!room_for(b, c->me->id, err, sizeof err)) goto out;

    /* One upload at a time (owner's rule): while one is running - anyone's, in any tab - a new
       one is refused, unless "upload anyway" was ticked; then both run, and the files the running
       one still has to send were skipped above (claimed). A person's earlier upload that isn't
       running is replaced (picking the same files resumes it). A picture only replaces their
       previous picture, so one can be set while videos go up. */
    bool force = in.force;
    lock_take(&g_lock);
    int slot = -1;
    const Batch *other = NULL;
    for (int s = 0; s < MAX_BATCHES && !b->art; s++)
        if (running(g_batch[s])) other = g_batch[s];
    if (other && !force) {
        const Person *p = person_get(other->person);
        int todo = other->nfiles - other->skipped;
        if (other->person == c->me->id)
            snprintf(err, sizeof err,
                     TN(todo,
                        "Your upload is still running (%d of %d file, in another tab?) - let it finish, stop it, or "
                        "tick \"Upload anyway\"",
                        "Your upload is still running (%d of %d files, in another tab?) - let it finish, stop it, or "
                        "tick \"Upload anyway\""),
                     other->done, todo);
        else if (p)
            snprintf(err, sizeof err,
                     TN(todo,
                        "%s's upload is running (%d of %d file) - let it finish, stop it, or tick \"Upload anyway\"",
                        "%s's upload is running (%d of %d files) - let it finish, stop it, or tick \"Upload anyway\""),
                     p->name, other->done, todo);
        else
            snprintf(err, sizeof err,
                     TN(todo,
                        "Someone's upload is running (%d of %d file) - let it finish, stop it, or tick \"Upload anyway\"",
                        "Someone's upload is running (%d of %d files) - let it finish, stop it, or tick \"Upload "
                        "anyway\""),
                     other->done, todo);
    }
    for (int s = 0; s < MAX_BATCHES && !err[0]; s++) {
        Batch *o = g_batch[s];
        if (o && o->person == c->me->id && !o->art == !b->art && !running(o) && !o->workers) {
            batch_free(o);
            g_batch[s] = NULL;
        }
    }
    for (int s = 0; s < MAX_BATCHES && slot < 0 && !err[0]; s++)
        if (!g_batch[s]) slot = s;
    for (int s = 0; s < MAX_BATCHES && slot < 0 && !err[0]; s++) /* full: drop a finished one */
        if (g_batch[s] && !g_batch[s]->workers && !batch_open(g_batch[s])) {
            batch_free(g_batch[s]);
            g_batch[s] = NULL;
            slot = s;
        }
    if (slot < 0 && !err[0]) str_copy(err, sizeof err, T("Too many uploads at once - try again in a while"));
    if (!err[0]) {
        rand_bytes(&b->id, sizeof b->id);
        b->person = c->me->id;
        b->started = b->last = now_sec();
        b->current = -1;
        g_batch[slot] = b;
    }
    lock_give(&g_lock);
    if (err[0]) goto out;

    /* Registered, so now a claim that conversions see. The room once more, with all that holds
       room now: a conversion that started since the first look can't count on the same room
       (convert.c checks again after marking one running, too). */
    if (!b->art && !room_for(b, c->me->id, err, sizeof err)) {
        lock_take(&g_lock);
        g_batch[slot] = NULL; /* (nobody has its id yet) */
        lock_give(&g_lock);
        goto out;
    }
    /* Conversions still waiting to make a copy this brings leave the queue: the upload's copy
       does instead. One that started meanwhile keeps it. */
    lock_take(&g_lock);
    for (int k = 0; k < b->nfiles && !b->art; k++) {
        UpFile *f = &b->files[k];
        char full[1024];
        if (f->state != UP_QUEUED || !media_is_video_ext(f->rel)) continue;
        join(full, sizeof full, b->root, f->rel);
        if (convert_give_way(full)) leave_to_server(b, f, false);
    }
    lock_give(&g_lock);

    Buf out = {0};
    char hex[17];
    media_id_hex(b->id, hex);
    buf_printf(&out, "{\"batch\":\"%s\",\"todo\":[", hex);
    int k = 0;
    bool *listed_yet = xcalloc((size_t)b->nfiles + 1, sizeof *listed_yet);
    for (int i = 0; i < b->nfiles; i++) {
        if ((b->files[i].state != UP_QUEUED && !b->files[i].subs) || listed_yet[i] || b->files[i].parts) continue; /* (2.7: a video in parts: they go instead) */
        int m = vobsub_mate(b, i); /* a VobSub's .sub goes before its .idx (the library takes it only beside one) */
        if (m >= 0 && !listed_yet[m] && b->files[m].state == UP_QUEUED && media_is_vobsub_data(b->files[m].rel)) {
            todo_entry(&out, b, m, &k);
            listed_yet[m] = true;
        }
        todo_entry(&out, b, i, &k);
        listed_yet[i] = true;
    }
    free(listed_yet);
    buf_printf(&out, "],\"skipped\":%d,\"total\":%lld", b->skipped, (long long)b->total);
    /* (2.7; owner: "It should be the limit you've set, like make the user be able to run it much higher") the room the
       device may hold for copies under way: what its person set (this plan's slider, else the one kept), and at least
       what its power tier's workers need at once - their biggest job each - so the tier is never held back by it */
    int64_t room = (int64_t)(in.room > 0 ? in.room : upload_room_gb(c->me->id, NULL, NULL)) << 30, biggest = 0;
    for (int i = 0; i < b->nfiles; i++) {
        const UpFile *f = &b->files[i];
        if (f->state == UP_QUEUED && f->conv == CV_WAIT && !f->parts) biggest = MAX(biggest, f->orig);
    }
    room = MAX(room, (int64_t)convert_device_workers(c) * biggest);
    buf_printf(&out, ",\"room\":%lld", (long long)room);
    if (here >= 0) { /* (2.6) where each "file" of the todo comes from: the library's, read by the browser */
        buf_puts(&out, ",\"remote\":[");
        for (int i = 0; i < b->nfiles && !b->files[i].part_of; i++) { /* (2.7: the videos - their parts read them) */
            char fx[17];
            media_id_hex(b->files[i].from, fx);
            buf_printf(&out, "%s{\"u\":\"/media/file/%s?src=1\",\"s\":%lld,\"h264\":true}", i ? "," : "", fx,
                       (long long)b->files[i].orig);
        }
        buf_putc(&out, ']');
    }
    buf_putc(&out, '}');
    reply(conn, 200, &out);
    buf_free(&out);
    if (!k) { /* nothing to send: done already */
        lock_take(&g_lock);
        b->ended = now_sec();
        lock_give(&g_lock);
    }
    int nparts = 0; /* (2.7: parts of videos made side by side - not files of their own) */
    for (int i = 0; i < b->nfiles; i++) nparts += b->files[i].part_of != 0;
    if (b->art) log_info("upload: %s sets the picture %s/%s", c->me->name, b->root, b->files[0].rel);
    else if (nparts)
        log_info("upload: %s plans %d files (%d skipped) into %s, in %d parts side by side", c->me->name,
                 b->nfiles - b->skipped - nparts, b->skipped, b->root, nparts);
    else
        log_info("upload: %s plans %d files (%d skipped) into %s", c->me->name, b->nfiles - b->skipped, b->skipped,
                 b->root);
    b = NULL; /* owned by g_batch now */
    app_changed_where("/media");

out:
    if (err[0]) reply_err(conn, 422, true, err);
    if (b) batch_free(b);
    plan_in_free(&in);
    free(tok);
}

/* ----------------------------------------------------------------- worker */

typedef struct {
    Batch *b;
    int i;
    sock_t s;
    Buf head, body; /* unsent output to flush first / body bytes already received */
    int64_t at, len;
    bool piece; /* ends before the file does (?end=): the rest comes in the next PUT */
    char full[1024], part[1040];
    Lang lang; /* the request's: the answer's words (the thread itself starts in English) */
} Job;

static void progress(Job *j, int64_t n) {
    lock_take(&g_lock);
    j->b->moved += n;
    j->b->files[j->i].saved += n;
    j->b->last = j->b->busy = j->b->wrote = j->b->files[j->i].touched = now_sec();
    lock_give(&g_lock);
}

static bool halted(const Batch *b) { /* the module stops, or someone pressed Stop on this upload */
    if (atomic_load(&g_halt)) return true;
    lock_take(&g_lock);
    bool s = b->stopped;
    lock_give(&g_lock);
    return s;
}

/* A picture has to start like a JPEG, PNG or WebP file (whatever its name says). */
static bool picture_bytes(const char *path) {
    unsigned char h[12] = {0};
    FILE *f = file_open(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return true;
    if (n >= 8 && memcmp(h, "\x89PNG\r\n\x1a\n", 8) == 0) return true;
    return n >= 12 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WEBP", 4) == 0;
}

static void worker(void *arg) {
    Job *j = arg;
    Batch *b = j->b;
    const char *fail = NULL;
    int status = 200;
    bool stop = false;
    sock_nonblock(j->s, true);
    if (j->head.len) worker_send(j->s, j->head.p, j->head.len); /* rare: answers to earlier pipelined requests */

    char dir[1024];
    str_copy(dir, sizeof dir, j->full);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    FILE *f = NULL; /* (fail: English, marked N_ - the card shows it in each viewer's language, the answer in
                       the request's) */
    if (!dir_make(dir)) fail = N_("Couldn't create the folder on the server"), status = 500, stop = true;
    else if (!(f = file_open(j->part, j->at ? "r+b" : "wb")))
        fail = N_("Couldn't create the file on the server"), status = 500, stop = true;
    else if (j->at && !file_seek(f, j->at)) fail = N_("Couldn't continue the file"), status = 409;

    int64_t got = 0;
    if (!fail && j->body.len) {
        size_t n = (size_t)MIN((int64_t)j->body.len, j->len);
        if (fwrite(j->body.p, 1, n, f) != n) fail = N_("The server's disk is full"), status = 507, stop = true;
        else {
            got += (int64_t)n;
            progress(j, (int64_t)n);
        }
    }
    char *buf = fail ? NULL : xmalloc(CHUNK);
    for (int waited = 0; !fail && got < j->len;) {
        if (halted(b)) {
            fail = N_("The upload was stopped"), status = 410, stop = true;
            break;
        }
        long k = sock_recv(j->s, buf, (size_t)MIN((int64_t)CHUNK, j->len - got));
        if (k > 0) {
            if (fwrite(buf, 1, (size_t)k, f) != (size_t)k) {
                fail = N_("The server's disk is full"), status = 507, stop = true;
                break;
            }
            got += k;
            waited = 0;
            progress(j, k);
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) {
            fail = N_("The connection dropped"), status = 0; /* the browser resumes from what we saved */
            break;
        }
        pollfd_t pf = {.fd = j->s, .events = POLLIN};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0) fail = N_("The connection dropped"), status = 0;
        else if (rc == 0 && (waited += 1000) >= STALL_MS) fail = N_("Nothing arrived for a minute"), status = 408;
    }
    free(buf);
    if (f) {
        if (!file_sync(f) && !fail) fail = N_("The server couldn't save the file"), status = 507, stop = true;
        fclose(f);
    }
    bool complete = !fail && got == j->len, piece = complete && j->piece; /* (a piece: saved, the file goes on) */
    complete &= !piece;
    if (complete && b->art && !picture_bytes(j->part)) {
        fail = N_("That file isn't a JPEG, PNG or WebP picture"), status = 415, stop = true;
        file_remove(j->part);
        complete = false;
    }
    if (complete) {
        bool over = b->files[j->i].replace && str_ends(j->full, ".web.mp4"); /* (2.6: Hemma's HEVC copy - only a copy) */
        if (!b->art && !over && path_stat(j->full, NULL, NULL, NULL))
            fail = N_("A file with that name appeared meanwhile"), status = 409, stop = true;
        else if (!file_replace(j->part, j->full)) fail = N_("Couldn't finish the file"), status = 500, stop = true;
        complete = !fail;
    }
    if (complete && b->art) { /* the new picture wins: drop "poster.png" when "poster.jpg" came, and the like */
        char dir[1024], stem[512];
        str_copy(dir, sizeof dir, j->full);
        char *name = strrchr(dir, '/');
        if (name) {
            *name++ = '\0';
            str_copy(stem, sizeof stem, name);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            media_art_prune(dir, stem, name);
        }
    }

    /* counted before the answer goes: a browser told "ok" may ask about the upload at once, and must
       find this file done (a busy machine once answered "0 of 2 files" right after an "ok") */
    lock_take(&g_lock);
    UpFile *uf = &b->files[j->i];
    uf->saved = j->at + got;
    if (complete) {
        uf->state = UP_DONE;
        uf->touched = now_sec();
        uf->settle = uf->from && uf->video; /* (2.7: a "here" copy - its original, on the loop: upload_tick) */
        b->done += !uf->part_of; /* (2.7: a part of a video isn't a file of the upload - its video, joined, is) */
        g_files_saved += b->art == 0 && !uf->part_of;
        g_pictures_saved += b->art != 0;
    } else if (stop && status != 410) {
        uf->state = UP_FAILED;
        why_set(uf, fail);
        b->failed++;
    } else {
        uf->state = UP_QUEUED; /* resumable */
    }
    if (b->current == j->i) b->current = -1;
    b->workers--;
    g_workers--;
    b->last = b->busy = now_sec();
    lock_give(&g_lock);
    if (status) {
        Buf out = {0};
        if (complete) buf_puts(&out, "{\"ok\":true}");
        else if (piece) buf_printf(&out, "{\"ok\":true,\"at\":%lld}", (long long)(j->at + got));
        else {
            buf_puts(&out, "{\"error\":");
            json_str(&out, T_in(j->lang, fail)); /* (in the request's language: the browser shows it) */
            buf_printf(&out, ",\"stop\":%s}", stop ? "true" : "false");
        }
        worker_reply(j->s, complete || piece ? 200 : status, buf_cstr(&out));
        buf_free(&out);
    }
    sock_shutdown_write(j->s);
    if (status && !complete && !piece) drain(j->s, j->len - got);
    sock_close(j->s);
    if (complete) log_info("upload: saved %s", j->full);
    else if (!piece) log_warn("upload: %s - %s", j->full, fail);
    app_tick_soon(); /* the tick shows it now (a new picture: at once), not at the next idle tick */
    buf_free_on_loop(&j->head); /* (the job and its bytes are the event loop's: base.h free_on_loop) */
    buf_free_on_loop(&j->body);
    free_on_loop(j);
}

/* -------------------------------------------------------------- endpoints */

/* (every error below is shown by the browser: in the request's language. "busy" is a word wire.js
   knows - it waits and tries again - so it stays as it is.) */

static void reply_str(Conn *conn, const char *json) {
    Buf b = {0};
    buf_puts(&b, json);
    reply(conn, 200, &b);
    buf_free(&b);
}

static void reply_ok(Conn *conn) { reply_str(conn, "{\"ok\":true}"); }

static void put(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i) {
    lock_take(&g_lock);
    Batch *b = find(id);
    const char *err = NULL;
    char failed[300];
    int status = 409;
    bool stop = true;
    if (!b || b->person != c->me->id) err = T("That upload is over - start it again to continue"), status = 404;
    else if (b->stopped) err = T("The upload was stopped"), status = 410;
    else if (i < 0 || i >= b->nfiles || (b->files[i].state == UP_SKIPPED && !b->files[i].on_server))
        err = T("That file isn't part of the upload"), status = 404;
    else if (b->files[i].state == UP_FAILED) /* (disk full and the like): say so, don't take it again */
        err = why_of(&b->files[i], failed, sizeof failed);
    else if (b->files[i].state == UP_DONE || b->files[i].on_server) err = NULL; /* answered below */
    else if (b->files[i].conv == CV_WAIT) err = T("That copy isn't made yet"), status = 409;
    else if (b->files[i].state == UP_WRITING || g_workers >= MAX_WORKERS) err = "busy", status = 503, stop = false;
    if (err || !b) {
        lock_give(&g_lock);
        reply_err(conn, status, stop, err ? err : "?");
        return;
    }
    UpFile *f = &b->files[i];
    char full[1024];
    join(full, sizeof full, b->root, f->rel);
    /* (an upload back from a quiet spell: a conversion queued meanwhile for this copy leaves the
       queue; one already running keeps it, and this file is answered as sent) */
    if (f->state == UP_DONE || f->on_server || (!b->art && media_is_video_ext(f->rel) && convert_give_way(full))) {
        if (f->state != UP_DONE && !f->on_server) leave_to_server(b, f, true);
        lock_give(&g_lock);
        reply_ok(conn);
        app_changed_where("/media");
        return;
    }
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    int64_t at = fields_int(&q, "at", -1), end = fields_int(&q, "end", f->size); /* (a piece ends before the file) */
    fields_free(&q);
    int64_t disk = resume_point(b, f);
    if (at != disk || at > f->size) { /* tell the browser where we really are */
        lock_give(&g_lock);
        Buf out = {0};
        char msg[100];
        snprintf(msg, sizeof msg, T("resume from %lld"), (long long)disk);
        buf_puts(&out, "{\"error\":");
        json_str(&out, msg);
        buf_printf(&out, ",\"at\":%lld}", (long long)disk);
        reply(conn, 409, &out);
        buf_free(&out);
        return;
    }
    if (end <= at || end > f->size || r->content_length != end - at) {
        lock_give(&g_lock);
        reply_err(conn, 400, true,
                  f->head ? T("Hemma was updated meanwhile - reload the page and start the upload again")
                          : T("The file changed size since the upload started - start it again"));
        return;
    }
    /* Room for the rest: an upload holds room only while it's live, so after a quiet spell (a
       laptop asleep) conversions may have used it. (The disk is asked outside the lock; nothing
       else touches this file meanwhile - claims and PUTs are all on the event loop.) */
    char root[300];
    str_copy(root, sizeof root, b->root);
    lock_give(&g_lock);
    int64_t held = 0, spare = spare_for(b, root, 0, &held);
    lock_take(&g_lock);
    if (f->size - at > spare) {
        char msg[400];
        no_room(msg, sizeof msg, ROOM_REST, f->size - at, spare, held);
        f->state = UP_FAILED;
        why_set(f, N_("not enough room on the drive (Hemma keeps 1 GB free)"));
        b->failed++;
        lock_give(&g_lock);
        reply_err(conn, 507, true, msg);
        app_changed_where("/media");
        return;
    }
    Job *j = xcalloc(1, sizeof *j);
    j->b = b;
    j->i = i;
    j->at = at;
    j->len = end - at;
    j->piece = end < f->size;
    j->lang = i18n_lang();
    join(j->full, sizeof j->full, b->root, f->rel);
    snprintf(j->part, sizeof j->part, "%s%s", j->full, MEDIA_PART_SUFFIX);
    f->state = UP_WRITING;
    f->saved = at;
    b->current = i;
    b->workers++;
    g_workers++;
    b->last = b->busy = now_sec();
    lock_give(&g_lock);

    Detached d;
    if (!r->streaming && r->body_len) buf_put(&j->body, r->body, r->body_len); /* small file: already here */
    conn_detach(conn, r, &d);
    j->s = d.fd;
    buf_put(&j->body, d.body.p, d.body.len);
    buf_free(&d.body);
    j->head = d.unsent;
    if (!thread_spawn(worker, j)) {
        lock_take(&g_lock);
        f->state = UP_QUEUED;
        b->workers--;
        g_workers--;
        lock_give(&g_lock);
        worker_reply(j->s, 503, "{\"error\":\"busy\",\"stop\":false}");
        sock_close(j->s);
        buf_free(&j->head);
        buf_free(&j->body);
        free(j);
    }
}

static void where(Ctx *c, Conn *conn, uint64_t id, int i) {
    lock_take(&g_lock);
    Batch *b = find(id);
    Buf out = {0};
    int status = 200;
    const char *err = NULL;
    char failed[300];
    if (!b || b->person != c->me->id) status = 404, err = T("That upload is over - start it again to continue");
    else if (b->stopped) status = 410, err = T("The upload was stopped");
    else if (i < 0 || i >= b->nfiles || (b->files[i].state == UP_SKIPPED && !b->files[i].on_server))
        status = 404, err = T("That file isn't part of the upload");
    else if (b->files[i].state == UP_FAILED) status = 409, err = why_of(&b->files[i], failed, sizeof failed);
    if (err) {
        buf_puts(&out, "{\"error\":");
        json_str(&out, err);
        buf_puts(&out, ",\"stop\":true}");
    } else {
        const UpFile *f = &b->files[i];
        int64_t at = f->state == UP_DONE || f->on_server ? f->size
                     : f->state == UP_WRITING           ? f->saved
                                                        : resume_point(b, f);
        buf_printf(&out, "{\"at\":%lld%s}", (long long)at, f->state == UP_WRITING ? ",\"busy\":true" : "");
    }
    lock_give(&g_lock);
    reply(conn, status, &out);
    buf_free(&out);
}

/* ------------------------------------------- subtitle tracks the browser's converter took out */

/* A file's tracks come one request each, in the file's order: what came before for that file, so a
   second track in one language and flags is named ".2" as the server's converter names it
   (convert_save_sub - text and picture tracks counted together). A request sent again (its answer was
   lost, its connection broke) is known by its words and its place in the file (t=) and gets the same
   answer, uncounted - a picture track whose bytes broke off is written again, under the same name.
   (g_lock: picture tracks are written on worker threads.) */
#define SUB_FILES 16
enum { TRACK_WRITING = 1, TRACK_SAVED, TRACK_SKIPPED, TRACK_BROKE };
static struct {
    uint64_t batch;
    int i;
    int64_t used;
    ConvertSubs seen;
    uint64_t sum[AV_CONV_MAX_SUBS];
    char state[AV_CONV_MAX_SUBS]; /* TRACK_* */
} g_subfiles[SUB_FILES];

typedef struct {
    int k;              /* its place among the file's tracks (-1: past AV_CONV_MAX_SUBS - counted, not kept) */
    int state;          /* as it was: 0 = new, else TRACK_* */
    ConvertSubs before; /* the file's tracks before it: what numbers it */
} Track;

/* Track `sum` of upload id's file i (lock held): one that came before, else a new one. Either way it's
   TRACK_WRITING now - unless it was finished (saved or skipped) or is being written. */
static Track track_of(uint64_t id, int i, uint64_t sum, const char *key) {
    int e = 0; /* this file's place in g_subfiles: its own, else the one unused longest */
    for (int k = 0; k < SUB_FILES; k++) {
        if (g_subfiles[k].batch == id && g_subfiles[k].i == i) {
            e = k;
            break;
        }
        if (g_subfiles[k].used < g_subfiles[e].used) e = k;
    }
    if (g_subfiles[e].batch != id || g_subfiles[e].i != i) {
        memset(&g_subfiles[e], 0, sizeof g_subfiles[e]);
        g_subfiles[e].batch = id;
        g_subfiles[e].i = i;
    }
    g_subfiles[e].used = now_ms();
    ConvertSubs *seen = &g_subfiles[e].seen;
    Track t = {.k = -1};
    for (int k = 0; k < seen->n && t.k < 0; k++)
        if (g_subfiles[e].sum[k] == sum) t.k = k, t.state = g_subfiles[e].state[k];
    t.before.n = t.k >= 0 ? t.k : seen->n;
    memcpy(t.before.keys, seen->keys, (size_t)t.before.n * sizeof seen->keys[0]);
    if (t.k < 0 && seen->n < AV_CONV_MAX_SUBS) {
        t.k = seen->n++;
        str_copy(seen->keys[t.k], sizeof seen->keys[0], key);
        g_subfiles[e].sum[t.k] = sum;
    }
    if (t.k >= 0 && (!t.state || t.state == TRACK_BROKE)) g_subfiles[e].state[t.k] = TRACK_WRITING;
    return t;
}

/* How track k (sum) of upload id's file i was left: TRACK_SAVED / _SKIPPED / _BROKE (lock held) */
static void track_left(uint64_t id, int i, int k, uint64_t sum, int state) {
    for (int e = 0; e < SUB_FILES; e++)
        if (g_subfiles[e].batch == id && g_subfiles[e].i == i && k >= 0 && k < g_subfiles[e].seen.n &&
            g_subfiles[e].sum[k] == sum)
            g_subfiles[e].state[k] = (char)state;
}

/* The copy file i of upload id became, when the browser made it (as root/rel in full); else the reason
   (in the request's words), its status in *status and whether the upload stops. */
static const char *copy_of(Ctx *c, uint64_t id, int i, char *full, size_t cap, Batch **out, int *status, bool *stop) {
    lock_take(&g_lock);
    Batch *b = find(id);
    const UpFile *f = b && i >= 0 && i < b->nfiles ? &b->files[i] : NULL;
    const char *err = NULL;
    *status = 404;
    *stop = false;
    if (!b || b->person != c->me->id) err = T("That upload is over");
    else if (b->stopped) err = T("The upload was stopped"), *status = 410, *stop = true;
    else if (f && f->subs) { /* only its subtitles: next to the video they go with */
        str_copy(full, cap, f->target);
        b->last = b->busy = now_sec();
    } else if (!f || f->conv == CV_NONE || f->conv == CV_ORIGINAL) err = T("That file has no copy made here");
    else {
        join(full, cap, b->root, f->rel);
        b->last = b->busy = now_sec(); /* (a sign of work: the upload is running) */
    }
    *out = b;
    lock_give(&g_lock);
    return err;
}

/* A text subtitle track the browser's converter took out of a file it made a copy of (?sub=1&lang=
   &name=&forced=&t=, the SRT as the body): saved next to the copy, named by media_sub_sidecar - never
   over a file that is there ({"skip":true} then). */
static void save_sub(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i, const Fields *q) {
    char lang[24], name[128], full[1024];
    str_copy(lang, sizeof lang, fields_str(q, "lang"));
    str_copy(name, sizeof name, fields_str(q, "name"));
    bool forced = str_eq(fields_str(q, "forced"), "1"), stop;
    int status;
    Batch *b;
    const char *err = copy_of(c, id, i, full, sizeof full, &b, &status, &stop);
    if (!err && (!r->body || !r->body_len)) err = T("No subtitles came"), status = 400, stop = false;
    if (err) {
        reply_err(conn, status, stop, err);
        return;
    }
    AvConvSub s = {.forced = forced, .kind = AV_SUB_TEXT, .srt = (char *)r->body, .len = r->body_len}; /* (only read) */
    str_copy(s.lang, sizeof s.lang, lang);
    str_copy(s.name, sizeof s.name, name);
    MediaSubTags tags;
    char key[40], said[300];
    media_sub_track_tags(lang, name, forced, &tags);
    media_sub_key(&tags, key, sizeof key);
    snprintf(said, sizeof said, "text\n%s\n%s\n%d\n%s\n", lang, name, forced, fields_str(q, "t"));
    uint64_t sum = hash64(r->body, r->body_len) ^ hash64_str(said);
    lock_take(&g_lock);
    Track t = track_of(id, i, sum, key);
    lock_give(&g_lock);
    bool saved = t.state == TRACK_SAVED; /* (written then - or not, and it isn't now either) */
    if (!t.state || t.state == TRACK_BROKE) { /* (the event loop writes it: never "being written" here) */
        saved = convert_save_sub(full, &s, &t.before);
        lock_take(&g_lock);
        track_left(id, i, t.k, sum, saved ? TRACK_SAVED : TRACK_SKIPPED);
        g_files_saved += saved; /* (the library looks again once the upload settles) */
        lock_give(&g_lock);
    }
    Buf out = {0};
    buf_puts(&out, saved ? "{\"ok\":true}" : "{\"skip\":true}");
    if (saved) app_changed_where("/media");
    reply(conn, 200, &out);
    buf_free(&out);
}

/* A picture subtitle track (see the top): its body is written on a worker thread, as file PUTs are. */
typedef struct {
    sock_t s;
    Buf head, body;             /* unsent output to flush first / body bytes already received */
    int64_t len, idx_len;       /* the body's bytes; its first idx_len are a VobSub's .idx text */
    char path[1100], idx[1100]; /* the .sup, or the VobSub's .sub and .idx ("" for PGS) */
    uint64_t batch, sum;
    int i, k;
    Lang lang; /* the request's: the answer's words */
} PicJob;

/* Bytes of the body from `at` on: the .idx text first, the rest into the file */
static bool pic_take(const PicJob *j, int64_t at, FILE *f, Buf *idx, const char *p, size_t n) {
    if (at < j->idx_len) {
        size_t k = (size_t)MIN((int64_t)n, j->idx_len - at);
        buf_put(idx, p, k);
        p += k;
        n -= k;
    }
    return !n || fwrite(p, 1, n, f) == n;
}

/* A .sup starts with its first segment ("PG"), a VobSub's .sub with an MPEG-PS pack header */
static bool pic_bytes(const char *path, bool vob) {
    unsigned char h[4] = {0};
    FILE *f = file_open(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return vob ? n == 4 && memcmp(h, "\0\0\1\xBA", 4) == 0 : n >= 2 && h[0] == 'P' && h[1] == 'G';
}

static void pic_worker(void *arg) {
    PicJob *j = arg;
    bool vob = j->idx[0] != '\0';
    sock_nonblock(j->s, true);
    if (j->head.len) worker_send(j->s, j->head.p, j->head.len); /* rare: answers to earlier pipelined requests */
    char part[1120];
    snprintf(part, sizeof part, "%s%s", j->path, MEDIA_PART_SUFFIX);
    const char *fail = NULL; /* (English, marked N_: the answer says it in the request's words) */
    int status = 200, left = TRACK_BROKE;
    Buf idx = {0};
    FILE *f = file_open(part, "wb"); /* (never resumed: a track is sent whole again) */
    if (!f) fail = N_("Couldn't create the file on the server"), status = 500;
    int64_t got = 0;
    if (!fail && j->body.len) {
        size_t n = (size_t)MIN((int64_t)j->body.len, j->len);
        if (!pic_take(j, 0, f, &idx, j->body.p, n)) fail = N_("The server's disk is full"), status = 507;
        else got = (int64_t)n;
    }
    char *buf = fail ? NULL : xmalloc(CHUNK);
    for (int waited = 0; !fail && got < j->len;) {
        if (atomic_load(&g_halt)) {
            fail = N_("The upload was stopped"), status = 410;
            break;
        }
        long k = sock_recv(j->s, buf, (size_t)MIN((int64_t)CHUNK, j->len - got));
        if (k > 0) {
            if (!pic_take(j, got, f, &idx, buf, (size_t)k)) {
                fail = N_("The server's disk is full"), status = 507;
                break;
            }
            got += k;
            waited = 0;
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) {
            fail = N_("The connection dropped"), status = 0; /* the browser sends it again */
            break;
        }
        pollfd_t pf = {.fd = j->s, .events = POLLIN};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0) fail = N_("The connection dropped"), status = 0;
        else if (rc == 0 && (waited += 1000) >= STALL_MS) fail = N_("Nothing arrived for a minute"), status = 408;
    }
    free(buf);
    if (f) {
        if (!file_sync(f) && !fail) fail = N_("The server couldn't save the file"), status = 507;
        fclose(f);
    }
    if (!fail) {
        if (!pic_bytes(part, vob)) fail = N_("Those aren't subtitle pictures"), status = 415, left = TRACK_SKIPPED;
        else if (path_stat(j->path, NULL, NULL, NULL) || (vob && path_stat(j->idx, NULL, NULL, NULL)))
            left = TRACK_SKIPPED; /* one came meanwhile: it stays */
        else if (!file_replace(part, j->path)) fail = N_("Couldn't finish the file"), status = 500;
        else if (vob && !file_write_atomic(j->idx, idx.p ? idx.p : "", idx.len)) { /* both or neither */
            file_remove(j->path);
            fail = N_("Couldn't finish the file"), status = 500;
        } else left = TRACK_SAVED;
    }
    if (left != TRACK_SAVED) file_remove(part);
    buf_free(&idx);

    lock_take(&g_lock);
    g_workers--;
    track_left(j->batch, j->i, j->k, j->sum, left);
    g_files_saved += left == TRACK_SAVED; /* (the library looks again once the upload settles) */
    lock_give(&g_lock);
    if (status) {
        Buf out = {0};
        if (left == TRACK_SAVED) buf_puts(&out, "{\"ok\":true}");
        else if (!fail) buf_puts(&out, "{\"skip\":true}");
        else {
            buf_puts(&out, "{\"error\":");
            json_str(&out, T_in(j->lang, fail));
            buf_printf(&out, ",\"stop\":%s}", status == 410 ? "true" : "false"); /* (subtitles never stop an upload) */
        }
        worker_reply(j->s, fail ? status : 200, buf_cstr(&out));
        buf_free(&out);
    }
    sock_shutdown_write(j->s);
    if (status && fail) drain(j->s, j->len - got);
    sock_close(j->s);
    if (left == TRACK_SAVED) log_info("upload: saved %s%s", j->path, vob ? " and its .idx" : "");
    else if (fail) log_warn("upload: %s - %s", j->path, fail);
    app_tick_soon();
    buf_free_on_loop(&j->head); /* (the event loop's: base.h free_on_loop) */
    buf_free_on_loop(&j->body);
    free_on_loop(j);
}

/* PUT ...?pic=pgs|vobsub: a picture subtitle track for the copy file i is (see the top) */
static void pic_put(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i, const Fields *q) {
    const char *kind = fields_str(q, "pic");
    bool vob = str_eq(kind, "vobsub"), forced = str_eq(fields_str(q, "forced"), "1"), stop;
    char lang[24], name[128], full[1024], msg[400];
    str_copy(lang, sizeof lang, fields_str(q, "lang"));
    str_copy(name, sizeof name, fields_str(q, "name"));
    long long idx_len = vob ? fields_int(q, "idx", -1) : 0, len = r->content_length;
    int status;
    Batch *b;
    const char *err = copy_of(c, id, i, full, sizeof full, &b, &status, &stop);
    if (!err && !vob && !str_eq(kind, "pgs")) err = "pic=pgs or pic=vobsub", status = 400; /* (only a broken page) */
    else if (!err && (idx_len < 0 || (vob && !idx_len) || len <= idx_len)) err = T("No subtitles came"), status = 400;
    else if (!err && (idx_len > PIC_IDX_MAX || len - idx_len > (long long)AV_CONV_PIC_BYTES))
        err = T("Those subtitles are too big"), status = 413;
    if (err) {
        reply_err(conn, status, stop, err);
        return;
    }
    char root[300];
    lock_take(&g_lock);
    str_copy(root, sizeof root, b->root); /* (copy_of found it: it's there until a tick, on this thread) */
    lock_give(&g_lock);

    MediaSubTags tags;
    char key[40], said[400];
    media_sub_track_tags(lang, name, forced, &tags);
    media_sub_key(&tags, key, sizeof key);
    snprintf(said, sizeof said, "%s\n%s\n%s\n%d\n%s\n%lld\n%lld\n", kind, lang, name, forced, fields_str(q, "t"), len,
             idx_len);
    uint64_t sum = hash64_str(said);
    lock_take(&g_lock);
    Track t = track_of(id, i, sum, key);
    bool mine = false; /* a worker taken for it */
    if (!t.state || t.state == TRACK_BROKE) {
        if (g_workers < MAX_WORKERS) g_workers++, mine = true;
        else track_left(id, i, t.k, sum, TRACK_BROKE); /* (not now: when it comes again) */
    }
    lock_give(&g_lock);
    if (t.state == TRACK_SAVED || t.state == TRACK_SKIPPED) { /* sent again: the same answer */
        reply_str(conn, t.state == TRACK_SAVED ? "{\"ok\":true}" : "{\"skip\":true}");
        return;
    }
    if (!mine) { /* being written still (its connection ends soon), or the uploads are writing: wait a turn */
        reply_err(conn, 503, false, "busy");
        return;
    }

    /* its name: by its place among the file's tracks ("x.en.2.sup"); a VobSub's .sub beside its .idx */
    char path[1100], idx[1100] = "";
    int n = 1, left = 0;
    for (int k = 0; k < t.before.n; k++) n += str_eq(t.before.keys[k], key);
    bool named = media_sub_sidecar(full, lang, name, forced, vob ? "idx" : "sup", n, path, sizeof path);
    if (named && vob) {
        str_copy(idx, sizeof idx, path);
        memcpy(path + strlen(path) - 3, "sub", 3);
    }
    if (!named || path_stat(path, NULL, NULL, NULL) || (vob && path_stat(idx, NULL, NULL, NULL))) {
        left = TRACK_SKIPPED; /* (one there stays: the household's own, or this track's from before) */
    } else {
        int64_t held = 0, spare = spare_for(b, root, 0, &held);
        if (len - idx_len > spare) {
            no_room(msg, sizeof msg, ROOM_REST, len - idx_len, spare, held);
            left = TRACK_BROKE;
        }
    }
    if (left) {
        lock_take(&g_lock);
        g_workers--;
        track_left(id, i, t.k, sum, left);
        lock_give(&g_lock);
        if (left == TRACK_SKIPPED) reply_str(conn, "{\"skip\":true}");
        else reply_err(conn, 507, false, msg);
        return;
    }
    PicJob *j = xcalloc(1, sizeof *j);
    j->len = len;
    j->idx_len = idx_len;
    str_copy(j->path, sizeof j->path, path);
    str_copy(j->idx, sizeof j->idx, idx);
    j->batch = id;
    j->sum = sum;
    j->i = i;
    j->k = t.k;
    j->lang = i18n_lang();
    Detached d;
    if (!r->streaming && r->body_len) buf_put(&j->body, r->body, r->body_len); /* small: already here */
    conn_detach(conn, r, &d);
    j->s = d.fd;
    buf_put(&j->body, d.body.p, d.body.len);
    buf_free(&d.body);
    j->head = d.unsent;
    if (!thread_spawn(pic_worker, j)) {
        lock_take(&g_lock);
        g_workers--;
        track_left(id, i, t.k, sum, TRACK_BROKE);
        lock_give(&g_lock);
        worker_reply(j->s, 503, "{\"error\":\"busy\",\"stop\":false}");
        sock_close(j->s);
        buf_free(&j->head);
        buf_free(&j->body);
        free(j);
    }
}

void upload_test_pace_window(int ms) { g_pace_window_ms = ms < 0 ? PACE_WINDOW_MS : ms; }

/* How fast the sending device's converter takes in the original (fed = its bytes so far; lock held) */
static void pace_note(UpFile *f, int64_t fed, int64_t now) {
    if (fed < 0) return;
    if (!f->fed_at || fed < f->fed) { /* the first report, or the job started over (a read that failed: {again}) */
        f->fed = fed;
        f->fed_at = now;
        f->paced = false;
        return;
    }
    if (now - f->fed_at < g_pace_window_ms) return; /* (keep counting from the older report) */
    double inst = (double)(fed - f->fed) * 1000.0 / (double)MAX(now - f->fed_at, 1);
    /* (nothing taken in for a whole window is a converter standing still: 0 at once, not halving towards it) */
    f->pace = f->paced && inst > 0 ? f->pace * 0.5 + inst * 0.5 : inst;
    f->paced = true;
    f->fed = fed;
    f->fed_at = now;
}

/* The browser's side of a copy it makes (see the top): progress, the finished size, or "it
   couldn't" - then the original goes under its own name, unless it's on the list anyway. */
static void converted(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i) {
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    if (str_eq(fields_str(&q, "sub"), "1")) {
        save_sub(c, conn, r, id, i, &q);
        fields_free(&q);
        return;
    }
    long long pm = fields_int(&q, "pm", -1), size = fields_int(&q, "size", -1), fed = fields_int(&q, "fed", -1);
    bool original = str_eq(fields_str(&q, "original"), "1"), plays = str_eq(fields_str(&q, "plays"), "1"); /* (2.5) */
    char why[96], msg[400]; /* (why: the browser's converter's words - shown as they came) */
    str_copy(why, sizeof why, fields_str(&q, "why"));
    fields_free(&q);

    int64_t spare = INT64_MAX / 4, held = 0; /* for a copy or an original to come: asked before the lock */
    if (size >= 0 || original) {
        char root[300] = "";
        lock_take(&g_lock);
        const Batch *sb = find(id);
        if (sb) str_copy(root, sizeof root, sb->root);
        lock_give(&g_lock);
        if (sb) spare = spare_for(sb, root, 0, &held);
    }
    /* (2.6) a "here" copy whose place went meanwhile - its video removed or renamed (Manage): the HEVC copy it replaces
       is gone, or the original it goes beside. Sent anyway it would bring the episode back, or one under its old name */
    bool gone = false;
    if (size >= 0) {
        char tgt[1024] = "", src[1024] = "";
        uint64_t from = 0;
        bool rep = false;
        lock_take(&g_lock);
        const Batch *sb = find(id);
        if (sb && i >= 0 && i < sb->nfiles && sb->files[i].from) {
            from = sb->files[i].from, rep = sb->files[i].replace;
            join(tgt, sizeof tgt, sb->root, sb->files[i].rel);
        }
        lock_give(&g_lock);
        const MediaFile *mf = from && !rep ? media_file(from) : NULL;
        if (from) gone = rep ? !path_stat(tgt, NULL, NULL, NULL)
                             : !mf || !media_full_path(mf, src, sizeof src) || !path_stat(src, NULL, NULL, NULL);
    }

    Buf out = {0};
    int status = 200;
    const char *err = NULL;
    lock_take(&g_lock);
    Batch *b = find(id);
    UpFile *f = b && i >= 0 && i < b->nfiles ? &b->files[i] : NULL;
    if (!b || b->person != c->me->id) err = T("That upload is over - start it again to continue"), status = 404;
    else if (b->stopped) err = T("The upload was stopped"), status = 410;
    else if (pm >= 0 && size < 0 && !original) { /* progress - also a late one, after the copy went: fine */
        /* 1000 = made, waiting its turn to go (1.5: the card says "Ready"); a report sent before it and arriving
           after it doesn't take that back - a made copy is never made again */
        if (f && f->conv == CV_WAIT && f->state == UP_QUEUED && (f->pm < 1000 || pm >= 1000)) {
            f->pm = (int)CLAMP(pm, 0, 1000);
            pace_note(f, fed, now_ms());
            if (f->part_of) part_progress(b, f->part_of - 1);
        } else if (f && f->subs && f->pm < 1000) { /* (only its subtitles: 1000 = all sent) */
            f->pm = (int)CLAMP(pm, 0, 1000);
            pace_note(f, fed, now_ms());
            if (f->pm == 1000) f->touched = now_sec();
        }
        b->last = b->busy = now_sec();
        buf_puts(&out, "{\"ok\":true}");
    } else if (!f || !f->conv || f->state != UP_QUEUED) err = T("That file isn't waiting for a copy"), status = 404;
    else if (size >= 0) { /* the copy is ready: size it, then the browser sends it */
        char full[1024], part[1100];
        join(full, sizeof full, b->root, f->rel);
        snprintf(part, sizeof part, "%s%s", full, MEDIA_PART_SUFFIX);
        if (f->conv == CV_READY && size == f->size) { /* asked again (a lost answer): carry on */
            buf_printf(&out, "{\"at\":%lld}", (long long)resume_point(b, f));
        } else if (f->conv != CV_WAIT) {
            err = T("That file is going as it is"), status = 409;
        } else if (size <= 0 || size > (1LL << 40)) {
            err = T("The browser's copy came out empty"), status = 422;
        } else if (gone) {
            b->total -= left_to_send(f);
            drop_part(b, f);
            f->state = UP_SKIPPED;
            b->skipped++;
            why_set(f, N_("its video was removed or renamed meanwhile"));
            buf_puts(&out, "{\"skip\":true}");
        } else if (convert_give_way(full)) { /* the server's converter started on it meanwhile */
            leave_to_server(b, f, true);
            buf_puts(&out, "{\"skip\":true}");
        } else if (size > spare) {
            no_room(msg, sizeof msg, ROOM_COPY, size, spare, held);
            err = msg, status = 507;
        } else {
            b->total += size - copy_guess(f);
            f->size = size;
            f->saved = 0;
            f->conv = CV_READY;
            f->pm = 1000;
            if (f->part_of) part_progress(b, f->part_of - 1);
            file_remove(part); /* a copy from an earlier try: this one starts over */
            b->last = b->busy = now_sec();
            buf_puts(&out, "{\"at\":0}");
        }
    } else if (original && f->conv == CV_WAIT && f->from) { /* (2.6) a "here" one: its original is the library's own */
        b->total -= copy_guess(f);
        f->state = UP_SKIPPED;
        b->skipped++;
        if (why[0]) why_set(f, "%s", why); /* (the browser's words: data) */
        else why_set(f, N_("the converter couldn't read it"));
        buf_puts(&out, "{\"skip\":true}");
    } else if (original && f->conv == CV_WAIT) { /* couldn't convert: the original goes as it is */
        char rel[420], full[1024];
        snprintf(rel, sizeof rel, "%.*s.%s", (int)(strlen(f->rel) - 8), f->rel, f->ext); /* - ".web.mp4" */
        join(full, sizeof full, b->root, rel);
        int64_t have = 0;
        bool dir = false, taken = f->extra || path_stat(full, &have, NULL, &dir);
        if (!taken && f->orig > spare) {
            no_room(msg, sizeof msg, ROOM_ORIGINAL, f->orig, spare, held);
            err = msg, status = 507;
        } else if (taken) {
            b->total -= copy_guess(f);
            f->state = UP_SKIPPED;
            b->skipped++;
            why_set(f, f->extra                  ? N_("couldn't be converted here - its original is on the list")
                       : dir || have != f->orig ? N_("couldn't be converted here, and a different file has its name")
                                                : N_("couldn't be converted here, and its original is already there"));
            buf_puts(&out, "{\"skip\":true}");
        } else {
            b->total -= copy_guess(f);
            str_copy(f->rel, sizeof f->rel, rel);
            f->size = f->orig;
            f->conv = CV_ORIGINAL;
            f->saved = resume_point(b, f);
            b->total += f->size - f->saved;
            f->plays = plays && is_mp4_name(rel); /* (it plays everywhere as it is: nothing to say) */
            if (f->plays) f->why[0] = '\0';
            else if (why[0]) why_set(f, "%s", why); /* (the browser's words: data) */
            else why_set(f, N_("the converter couldn't read it"));
            buf_printf(&out, "{\"at\":%lld}", (long long)f->saved);
        }
        if (!err) b->last = b->busy = now_sec();
    } else if (original && f->conv == CV_ORIGINAL) { /* asked again */
        buf_printf(&out, "{\"at\":%lld}", (long long)resume_point(b, f));
    } else {
        err = "pm, size or original", status = 400; /* (only a broken page asks: no words for people) */
    }
    lock_give(&g_lock);
    if (!err) app_changed_where("/media"); /* progress bars, copies ready */
    if (err) reply_err(conn, status, true, err);
    else reply(conn, 200, &out);
    buf_free(&out);
}

bool upload_http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_starts(r->path, "/media/upload/")) return false;
    const char *rest = r->path + 14;
    if (!is_admin(c)) {
        reply_err(conn, 403, true, T("Only an admin can add videos"));
        return true;
    }
    if (!http_same_origin(r)) { /* (this and the method answers: only another site's page or a broken one gets them) */
        reply_err(conn, 403, true, "cross-site request refused");
        return true;
    }
    if (str_eq(rest, "plan")) {
        if (!str_eq(r->method, "POST")) reply_err(conn, 405, true, "POST only");
        else plan(c, conn, r);
        return true;
    }
    uint64_t id = 0;
    char hex[17];
    size_t hl = strcspn(rest, "/");
    str_ncopy(hex, sizeof hex, rest, hl);
    long long i = rest[hl] == '/' ? str_int_or(rest + hl + 1, -1) : -1;
    if (hl != 16 || !media_parse_id(hex, &id) || i < 0 || i >= 2 * MAX_FILES) {
        reply_err(conn, 404, true, T("No such upload"));
        return true;
    }
    if (str_eq(r->method, "PUT")) {
        Fields q = {0};
        fields_parse(&q, r->query, strlen(r->query));
        if (fields_get(&q, "pic")) pic_put(c, conn, r, id, (int)i, &q); /* a picture subtitle track */
        else put(c, conn, r, id, (int)i);
        fields_free(&q);
    } else if (str_eq(r->method, "GET")) where(c, conn, id, (int)i);
    else if (str_eq(r->method, "POST")) converted(c, conn, r, id, (int)i);
    else reply_err(conn, 405, true, "PUT, GET or POST");
    return true;
}

/* ------------------------------------------------------------- the preview */

/* What Start would do with the files chosen on the page (owner, 2026-10-21: the upload page must be
   "easy enough to understand"): the plan, made dry, grouped by the folder each file goes to - kept for
   the screen that asked (its live state "media.up" holds the id), and drawn by upload_page in the
   reader's words. Only data is kept: names, counts, the plan's reasons packed in English. */
#define PV_SLOTS 8
#define PV_ROWS 240       /* rows drawn in all ... */
#define PV_GROUP_ROWS 40  /* ... and in one folder */
#define PV_GROUPS 60
#define PV_NAMES 16
#define PV_LIVE "media.up"

enum { FATE_SEND = 1, FATE_CONVERT, FATE_RESUME, FATE_HEAD, FATE_SKIP, FATE_COPY, FATE_SUBS, FATE_MAYBE };

typedef struct {
    char name[200]; /* the file as chosen (a copy made here: the copy's name) */
    char kind, fate; /* UpKind, FATE_* */
    char title[128], ep[96];
    int year, season, episode, episode_end, disc, pct;
    bool kept;      /* sorting, but nothing said where it goes: as it was chosen */
    bool part;      /* an audiobook's file among others of its book (UpSorted.part) */
    char why[160];  /* FATE_SKIP: the reason (packed English); FATE_COPY: the original's name */
} PvRow;

typedef struct {
    char dir[400];   /* where its files go, below the library folder ("" = the library folder itself) */
    bool out, fresh, kept; /* the files with no place / a folder that isn't there yet / some go as chosen */
    int files, eps, films, subs, pics, others, books, audio, skipped;
    int64_t bytes;
    int row0, nrows, more;
} PvGroup;

typedef struct {
    uint64_t key;    /* upload_show_key(found): the "to.<hex>" field renames it */
    char found[128]; /* as the files name it ("Unknown series": none) */
    char name[128];  /* what it's called now (the library's spelling, or the new name) */
    int eps;
    bool known;
} PvName;

/* (2.4) A film without a year in its name - its year box - and a folder whose videos could be films - its toggle */
#define PV_FILMS 200
#define PV_MARKS 16
typedef struct {
    uint64_t key;     /* UpSorted.film: "y.<hex>" */
    char title[128];
    int year;         /* typed (0 = not yet) */
} PvFilm;

typedef struct {
    uint64_t key;      /* UpSorted.group: "films.<hex>" */
    char folder[128];  /* the top folder as chosen ("" = files on their own) */
    int videos;
    bool on;
} PvMark;

typedef struct {
    uint64_t id;
    int person;
    char root[300];
    bool sorted;
    int chosen, all;  /* files sent to the preview / chosen in the browser (a long list comes cut) */
    int files, skipped, conv, subs; /* (subs: videos that stay behind while their subtitles go) */
    int64_t bytes;
    char err[400];    /* the plan's refusal (packed) */
    bool no_room;
    int64_t need, spare, held;
    PvGroup *groups;
    int ngroups, more_groups;
    PvRow *rows;
    int nrows;
    PvName names[PV_NAMES];
    int nnames;
    PvFilm films[PV_FILMS]; /* films without a year in their names (2.4) */
    int nfilms, no_year;    /* ... and how many still lack one */
    PvMark marks[PV_MARKS]; /* "These are films" (2.4) */
    int nmarks;
} Preview;

static Preview *g_pv[PV_SLOTS]; /* (main thread only) */
static uint64_t g_test_pv;      /* upload_test_preview's */
static int g_test_pv_person;

static void pv_free(Preview *p) {
    if (!p) return;
    free(p->groups);
    free(p->rows);
    free(p);
}

static Preview *pv_find(uint64_t id, int person) {
    for (int s = 0; s < PV_SLOTS; s++)
        if (g_pv[s] && g_pv[s]->id == id && g_pv[s]->person == person) return g_pv[s];
    return NULL;
}

static void pv_drop(uint64_t id, int person) {
    for (int s = 0; s < PV_SLOTS; s++)
        if (g_pv[s] && g_pv[s]->id == id && g_pv[s]->person == person) {
            pv_free(g_pv[s]);
            g_pv[s] = NULL;
        }
}

static void pv_keep(Preview *p) { /* in a free slot, else the oldest's (ids are random: the slot order says) */
    static unsigned turn;
    int s = 0;
    while (s < PV_SLOTS && g_pv[s]) s++;
    if (s == PV_SLOTS) s = (int)(turn++ % PV_SLOTS);
    pv_free(g_pv[s]);
    g_pv[s] = p;
}

static void pv_clear(void) {
    for (int s = 0; s < PV_SLOTS; s++) {
        pv_free(g_pv[s]);
        g_pv[s] = NULL;
    }
    g_test_pv = 0;
    g_test_pv_person = 0;
}

static const char *base_name(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

static void dir_of(const UpFile *f, char *out, size_t cap) {
    const char *s = f->out ? NULL : strrchr(f->rel, '/');
    str_ncopy(out, cap, f->rel, s ? (size_t)(s - f->rel) : 0);
}

static const Batch *g_order_b; /* (qsort has no context: main thread only) */
static int by_place(const void *x, const void *y) {
    const UpFile *a = &g_order_b->files[*(const int *)x], *b = &g_order_b->files[*(const int *)y];
    if (a->out != b->out) return a->out ? 1 : -1;
    char da[400], db[400];
    dir_of(a, da, sizeof da);
    dir_of(b, db, sizeof db);
    int d = strcmp(da, db);
    if (d) return d;
    d = strcmp(base_name(a->rel), base_name(b->rel));
    return d ? d : *(const int *)x - *(const int *)y;
}

static void pv_row(Preview *p, PvGroup *g, const PlanIn *in, const UpSorted *s, const UpFile *f) {
    PvRow *r = &p->rows[p->nrows++];
    g->nrows++;
    str_copy(r->name, sizeof r->name, base_name(f->extra ? f->rel : in->paths[f->src]));
    r->kind = s->kind;
    str_copy(r->title, sizeof r->title, s->title);
    str_copy(r->ep, sizeof r->ep, s->ep_title);
    r->year = s->year;
    r->season = s->season;
    r->episode = s->episode;
    r->episode_end = s->episode_end;
    r->disc = s->disc;
    r->kept = p->sorted && s->kept && !in->series;
    r->part = s->part;
    if (f->subs) { /* stays behind, its subtitles go (subtitles_only) */
        r->fate = FATE_SUBS;
        str_copy(r->why, sizeof r->why, f->why);
    } else if (f->state == UP_SKIPPED) {
        r->fate = FATE_SKIP;
        str_copy(r->why, sizeof r->why, f->why);
    } else if (f->extra) {
        r->fate = FATE_COPY;
        str_copy(r->why, sizeof r->why, base_name(in->paths[f->src]));
    } else if (f->conv == CV_WAIT) { /* (an MP4: only the browser knows, once it has read it, whether its sound needs it) */
        r->fate = ext_mp4(f->ext) ? FATE_MAYBE : FATE_CONVERT;
    } else if (f->head) {
        r->fate = FATE_HEAD;
    } else if (f->saved > 0 && f->size > 0) {
        r->fate = FATE_RESUME;
        r->pct = (int)MIN(f->saved * 100 / f->size, 99);
    } else {
        r->fate = FATE_SEND;
        str_copy(r->why, sizeof r->why, f->why); /* (a subtitle file going next to the library's video: rehome) */
    }
}

/* The series the files name (sorting by names): each with a field to rename it on the page */
static void pv_names(Preview *p, const PlanIn *in, const UpSorted *srt) {
    for (int i = 0; i < in->n; i++) {
        const UpSorted *s = &srt[i];
        if (s->kind != UPK_EPISODE && s->kind != UPK_DISC && s->kind != UPK_ASK) continue;
        PvName *nm = NULL;
        for (int k = 0; k < p->nnames && !nm; k++)
            if (p->names[k].key == s->show) nm = &p->names[k];
        if (!nm && p->nnames == PV_NAMES) continue;
        if (!nm) {
            nm = &p->names[p->nnames++];
            nm->key = s->show;
            str_copy(nm->found, sizeof nm->found, s->found);
            str_copy(nm->name, sizeof nm->name, s->kind == UPK_ASK ? "" : s->title);
            nm->known = s->known;
        }
        nm->eps += s->kind == UPK_EPISODE ? MAX(s->episode_end - s->episode, 0) + 1 : 1;
    }
}

/* (2.4) The films without a year (one box per title) and the folders whose videos could be films (one toggle each) */
static void pv_films(Preview *p, const PlanIn *in, const UpSorted *srt) {
    for (int i = 0; i < in->n; i++) {
        const UpSorted *s = &srt[i];
        if (s->kind == UPK_FILM && s->no_year) {
            PvFilm *f = NULL;
            for (int k = 0; k < p->nfilms && !f; k++)
                if (p->films[k].key == s->film) f = &p->films[k];
            if (!f && p->nfilms < PV_FILMS) {
                f = &p->films[p->nfilms++];
                f->key = s->film;
                str_copy(f->title, sizeof f->title, s->title);
                f->year = s->year;
            }
        }
        if (!s->group || (s->kind != UPK_OTHER && !s->as_film)) continue;
        PvMark *m = NULL;
        for (int k = 0; k < p->nmarks && !m; k++)
            if (p->marks[k].key == s->group) m = &p->marks[k];
        if (!m && p->nmarks == PV_MARKS) continue;
        if (!m) {
            m = &p->marks[p->nmarks++];
            m->key = s->group;
            const char *path = in->paths[i], *slash = strchr(path, '/');
            str_ncopy(m->folder, sizeof m->folder, path, slash ? (size_t)(slash - path) : 0);
            for (int k = 0; k < in->nfilms; k++) m->on |= in->films[k] == s->group;
        }
        m->videos++;
    }
    for (int k = 0; k < p->nfilms; k++) p->no_year += !p->films[k].year;
}

static Preview *pv_build(Ctx *c, const PlanIn *in, int all) {
    Preview *p = xcalloc(1, sizeof *p);
    rand_bytes(&p->id, sizeof p->id);
    p->id |= 1; /* (never 0) */
    p->person = c->me->id;
    p->sorted = in->sort;
    p->chosen = in->n;
    p->all = MAX(all, in->n);
    UpSorted *srt = NULL;
    Batch *b = plan_make(c, in, true, p->err, sizeof p->err, &srt);
    if (!b) return p;
    str_copy(p->root, sizeof p->root, b->root);
    int64_t held = 0, spare = spare_for(b, b->root, c->me->id, &held); /* as the plan will ask */
    if (b->total > spare) p->no_room = true, p->need = b->total, p->spare = spare, p->held = held;
    if (in->sort && !in->series) pv_names(p, in, srt);
    if (in->sort && !in->series) pv_films(p, in, srt);

    int n = b->nfiles, *ord = xmalloc((size_t)n * sizeof *ord);
    for (int i = 0; i < n; i++) ord[i] = i;
    g_order_b = b;
    qsort(ord, (size_t)n, sizeof *ord, by_place);
    p->groups = xcalloc((size_t)MIN(n, PV_GROUPS), sizeof *p->groups);
    p->rows = xcalloc((size_t)MIN(n, PV_ROWS), sizeof *p->rows);
    PvGroup *g = NULL, spill; /* (spill: the folders past PV_GROUPS, counted only) */
    char dir[400], last[400] = "";
    bool last_out = false;
    for (int k = 0; k < n; k++) {
        const UpFile *f = &b->files[ord[k]];
        const UpSorted *s = &srt[f->src];
        dir_of(f, dir, sizeof dir);
        if (!g || f->out != last_out || !str_eq(dir, last)) {
            if (p->ngroups < PV_GROUPS) g = &p->groups[p->ngroups++];
            else g = &spill, p->more_groups++;
            memset(g, 0, sizeof *g);
            str_copy(g->dir, sizeof g->dir, dir);
            g->out = f->out;
            g->row0 = p->nrows;
            if (!f->out) {
                char full[1024];
                join(full, sizeof full, b->root, dir);
                g->fresh = !path_is_dir(full);
            }
            str_copy(last, sizeof last, dir);
            last_out = f->out;
        }
        bool skip = f->state == UP_SKIPPED && !f->subs;
        int64_t size = f->conv == CV_WAIT ? f->orig : f->size;
        g->files++;
        if (skip) g->skipped++, p->skipped++;
        else if (f->subs) p->subs++;
        else g->bytes += size, p->bytes += size, p->files++;
        p->conv += !skip && f->conv == CV_WAIT && !f->extra && !ext_mp4(f->ext); /* (an MP4: converted only if it needs it) */
        if (!f->extra) {
            g->eps += s->kind == UPK_EPISODE || s->kind == UPK_DISC || s->kind == UPK_ASK;
            g->films += s->kind == UPK_FILM;
            g->subs += s->kind == UPK_SUB;
            g->pics += s->kind == UPK_PIC;
            g->others += s->kind == UPK_OTHER || s->kind == UPK_EXTRA;
            g->books += s->kind == UPK_BOOK;
            g->audio += s->kind == UPK_AUDIO;
            g->kept |= p->sorted && !in->series && s->kept && s->kind != UPK_JUNK && !f->out;
        }
        if (g != &spill && g->nrows < PV_GROUP_ROWS && p->nrows < PV_ROWS) pv_row(p, g, in, s, f);
        else if (g != &spill) g->more++;
    }
    free(ord);
    free(srt);
    batch_free(b);
    return p;
}

/* The live action's fields -> a preview (NULL: no files - the screen's preview goes) */
static Preview *pv_from(Ctx *c, const Fields *f) {
    const char *files = fields_str(f, "files");
    if (!*files) return NULL;
    int maxtok = 16 + MAX_FILES * 3;
    JsonTok *tok = xmalloc((size_t)maxtok * sizeof *tok);
    Json j;
    PlanIn in = {0};
    Ask a = {.f = f};
    plan_in(&a, &in);
    Preview *p;
    if (json_parse(&j, files, strlen(files), tok, maxtok) && json_type(&j, 0) == JSON_ARRAY) {
        files_in(&j, 0, &in);
        p = pv_build(c, &in, (int)fields_int(f, "all", in.n));
    } else {
        p = xcalloc(1, sizeof *p);
        rand_bytes(&p->id, sizeof p->id);
        p->id |= 1;
        p->person = c->me->id;
        i18n_pack(p->err, sizeof p->err, N_("That upload list didn't make sense (more than %d files?)"), MAX_FILES);
    }
    plan_in_free(&in);
    free(tok);
    return p;
}

void upload_act_preview(Ctx *c, Fields *f) {
    if (!is_admin(c)) return; /* (a live action: nothing to say) */
    uint64_t old = 0;
    if (media_parse_id(ctx_live(c, PV_LIVE), &old)) pv_drop(old, c->me->id);
    ctx_set_live(c, PV_LIVE, "");
    Preview *p = pv_from(c, f);
    if (!p) return;
    pv_keep(p);
    char hex[17];
    media_id_hex(p->id, hex);
    ctx_set_live(c, PV_LIVE, hex);
}

void upload_test_preview(int person, const char *form) {
    pv_drop(g_test_pv, g_test_pv_person);
    g_test_pv = 0;
    g_test_pv_person = 0;
    if (!form) return;
    Ctx c;
    app_ctx(&c, person, "/media/upload");
    Fields f = {0};
    fields_parse(&f, form, strlen(form));
    Preview *p = c.me ? pv_from(&c, &f) : NULL;
    if (p) {
        pv_keep(p);
        g_test_pv = p->id;
        g_test_pv_person = person;
    }
    fields_free(&f);
    app_ctx_free(&c);
}

static const Preview *pv_for(const Ctx *c) {
    uint64_t id = 0;
    if (media_parse_id(ctx_live(c, PV_LIVE), &id)) return pv_find(id, c->me->id);
    if (!c->client && app_testing() && g_test_pv_person == c->me->id) return pv_find(g_test_pv, c->me->id);
    return NULL;
}

/* "70's Show · S01E03" / "Inception (2010)" / "Friends" - what a subtitle or picture goes with */
static void pv_label(Buf *b, const PvRow *r) {
    if (r->episode > 0 && r->episode_end > r->episode)
        H(b, "%s · S%02dE%02d–E%02d", r->title, r->season, r->episode, r->episode_end);
    else if (r->episode > 0) H(b, "%s · S%02dE%02d", r->title, r->season, r->episode);
    else if (r->year) H(b, "%s (%d)", r->title, r->year);
    else H(b, "%s", r->title);
}

static void pv_what(Buf *b, const PvRow *r) { /* what Hemma thinks the file is */
    Buf l = {0};
    switch (r->kind) {
    case UPK_EPISODE:
        pv_label(b, r);
        if (r->ep[0]) H(b, " · %s", r->ep);
        break;
    case UPK_DISC:
        if (r->disc) H(b, T("%s · season %d, disc %d"), r->title, r->season, r->disc);
        else H(b, T("%s · season %d"), r->title, r->season);
        break;
    case UPK_ASK: H(b, "%s", T("An episode - of which series? Name it above")); break;
    case UPK_FILM: pv_label(b, r); break;
    case UPK_EXTRA:
        if (r->title[0]) H(b, T("An extra for %s"), r->title);
        else H(b, "%s", T("An extra (a trailer, sample or bonus clip)"));
        break;
    case UPK_OTHER: H(b, "%s", T("A video that isn't an episode or a film")); break;
    case UPK_SUB:
    case UPK_PIC:
        pv_label(&l, r);
        if (r->title[0]) H(b, r->kind == UPK_SUB ? T("Subtitles for %s") : T("A picture for %s"), buf_cstr(&l));
        else H(b, "%s", r->kind == UPK_SUB ? T("Subtitles") : T("A picture"));
        break;
    case UPK_BOOK: H(b, "%s", r->part ? T("Part of an audiobook") : T("An audiobook")); break;
    case UPK_AUDIO: /* (a theme song, a soundtrack: its show's or film's) */
        pv_label(&l, r);
        if (r->title[0]) H(b, T("Goes with %s"), buf_cstr(&l));
        break;
    default: break;
    }
    buf_free(&l);
}

static void pv_fate(Buf *b, const PvRow *r) {
    switch (r->fate) {
    case FATE_CONVERT: H(b, "<span class=\"badge ok\">%s</span>", T("Converted here first")); break;
    case FATE_MAYBE: H(b, "<span class=\"badge ok\">%s</span>", T("Converted here if needed")); break;
    case FATE_RESUME:
        buf_puts(b, "<span class=\"badge\">");
        H(b, T("Continues: %d%% sent"), r->pct);
        buf_puts(b, "</span>");
        break;
    case FATE_HEAD: H(b, "<span class=\"badge\">%s</span>", T("Just its first MB")); break;
    case FATE_SKIP: H(b, "<span class=\"badge warn\">%s</span>", T("Skipped")); break;
    case FATE_COPY: H(b, "<span class=\"badge ok\">%s</span>", T("Made here")); break;
    case FATE_SUBS: H(b, "<span class=\"badge ok\">%s</span>", T("Only its subtitles")); break;
    default: H(b, "<span class=\"badge\">%s</span>", T("Sent as it is")); break;
    }
}

static void pv_part(Buf *b, int *parts, int n, const char *fmt) { /* one count of a list ("3 episodes") */
    if (!n) return;
    if ((*parts)++) buf_puts(b, ", ");
    H(b, fmt, n);
}

static void pv_group_sum(Buf *b, const PvGroup *g) { /* "3 episodes, 1 subtitle file · 2.1 GB" */
    int parts = 0;
    pv_part(b, &parts, g->eps, TN(g->eps, "%d episode", "%d episodes"));
    pv_part(b, &parts, g->films, TN(g->films, "%d movie", "%d movies"));
    pv_part(b, &parts, g->subs, TN(g->subs, "%d subtitle file", "%d subtitle files"));
    pv_part(b, &parts, g->pics, TN(g->pics, "%d picture", "%d pictures"));
    pv_part(b, &parts, g->others, TN(g->others, "%d other video", "%d other videos"));
    pv_part(b, &parts, g->books, TN(g->books, "%d audiobook file", "%d audiobook files"));
    pv_part(b, &parts, g->audio, TN(g->audio, "%d audio file", "%d audio files"));
    pv_part(b, &parts, g->skipped, T("%d skipped"));
    if (g->bytes) {
        char size[24];
        media_size_text(g->bytes, size, sizeof size);
        H(b, "%r%s", parts ? " · " : "", size);
    }
}

static void pv_draw(Ctx *c, Buf *b, const Preview *p) {
    char why[400], size[24];
    if (p->err[0]) {
        i18n_unpack_to(why, sizeof why, p->err);
        H(b, "<p class=\"notice bad\">%s</p>", why);
        return;
    }
    /* the totals, read out when they change (a screen reader hears what the drop did) */
    media_size_text(p->bytes, size, sizeof size);
    buf_puts(b, "<p class=\"media-up-sum\" data-say>");
    H(b, TN(p->files, "%d file to send", "%d files to send"), p->files);
    if (p->files) H(b, " · %s", size);
    if (p->conv) {
        buf_puts(b, " · ");
        H(b, T("%d to convert here first"), p->conv);
    }
    if (p->subs) {
        buf_puts(b, " · ");
        H(b, TN(p->subs, "only the subtitles of %d video", "only the subtitles of %d videos"), p->subs);
    }
    if (p->skipped) {
        buf_puts(b, " · ");
        H(b, T("%d skipped"), p->skipped);
    }
    buf_puts(b, "</p>");
    if (p->all > p->chosen) {
        buf_puts(b, "<p class=\"muted media-small\">");
        H(b, T("This shows the first %d of %d files - Start sorts them all the same way."), p->chosen, p->all);
        buf_puts(b, "</p>");
    }
    if (p->no_room) {
        char need[24], tail[200], msg[400];
        media_size_text(p->need, need, sizeof need);
        media_room_text(tail, sizeof tail, p->spare, p->held);
        snprintf(msg, sizeof msg, T("Not enough room: this needs %s, and %s"), need, tail);
        H(b, "<p class=\"notice bad\">%s</p>", msg);
    }
    /* the series it found: renamed here when Hemma got one wrong (or knows none) */
    int ask = 0;
    for (int i = 0; i < p->nnames; i++) ask += !p->names[i].name[0];
    if (p->nnames) {
        H(b, "<details class=\"media-up-names\" id=\"media-up-names\"%r><summary>%s</summary><p class=\"muted media-small\">%s</p>",
          ask ? " open" : "", ask ? T("Which series are these episodes from?") : T("Not the right series? Change a name"),
          T("Type the series' name - one the library already has puts the episodes next to its own."));
        for (int i = 0; i < p->nnames; i++) {
            const PvName *nm = &p->names[i];
            bool none = str_eq(nm->found, "Unknown series");
            H(b, "<label class=\"field\" id=\"media-up-n-%X\"><span>", nm->key);
            if (none) H(b, TN(nm->eps, "%d episode Hemma can't place", "%d episodes Hemma can't place"), nm->eps);
            else {
                H(b, "%s · ", nm->found);
                H(b, TN(nm->eps, "%d episode", "%d episodes"), nm->eps);
            }
            H(b,
              "</span><input name=\"to.%X\" value=\"%s\" placeholder=\"%s\" list=\"media-up-shows\" maxlength=\"120\" "
              "autocomplete=\"off\" spellcheck=\"false\"></label>",
              nm->key, nm->name, T("The series' name"));
        }
        const MediaIndex *ix = media_lib();
        buf_puts(b, "<datalist id=\"media-up-shows\">");
        for (int i = 0; i < ix->nseries && i < 400; i++) H(b, "<option value=\"%s\">", ix->series[i].title);
        buf_puts(b, "</datalist></details>");
    }
    /* (2.4) folders of videos that could be films: one tick each, and they go into Movies */
    if (p->nmarks) {
        H(b, "<fieldset class=\"media-up-marks\" id=\"media-up-marks\"><legend>%s</legend><p class=\"muted media-small\">%s</p>",
          T("Films?"),
          T("Hemma reads these as videos that aren't films or episodes. Ticked, they go into Movies, each into a folder of "
            "its own with its year."));
        for (int i = 0; i < p->nmarks; i++) {
            const PvMark *m = &p->marks[i];
            H(b, "<label class=\"check\" id=\"media-up-m-%X\"><input type=\"checkbox\" name=\"films.%X\" value=\"1\"%r><span>",
              m->key, m->key, m->on ? " checked" : "");
            if (m->folder[0]) H(b, T("The videos in %s are films"), m->folder);
            else H(b, "%s", T("The videos on their own are films"));
            buf_puts(b, " <small class=\"muted\">");
            H(b, TN(m->videos, "%d video", "%d videos"), m->videos);
            buf_puts(b, "</small></span></label>");
        }
        buf_puts(b, "</fieldset>");
    }
    /* (2.4; owner: "the year of a movie to be almost mandatory") a box for each film without a year in its name */
    if (p->nfilms) {
        H(b, "<details class=\"media-up-years\" id=\"media-up-years\"%r><summary>", p->no_year ? " open" : "");
        if (p->no_year) H(b, TN(p->no_year, "%d film has no year", "%d films have no year"), p->no_year);
        else H(b, "%s", T("The films' years"));
        H(b, "</summary><p class=\"muted media-small\">%s</p>",
          T("The year tells films apart - The Lion King (1994) from The Lion King (2019) - and finds their categories. "
            "Without one, a film goes in all the same."));
        for (int i = 0; i < p->nfilms; i++) {
            const PvFilm *f = &p->films[i];
            char year[8] = "";
            if (f->year) snprintf(year, sizeof year, "%d", f->year);
            H(b, "<label class=\"field media-up-year\" id=\"media-up-y-%X\"><span>%s", f->key, f->title);
            if (!f->year) H(b, " <small class=\"badge warn\">%s</small>", T("needs its year"));
            H(b,
              "</span><input name=\"y.%X\" value=\"%s\" inputmode=\"numeric\" maxlength=\"4\" pattern=\"[0-9]{4}\" "
              "placeholder=\"%s\" autocomplete=\"off\"></label>",
              f->key, year, T("Year"));
        }
        buf_puts(b, "</details>");
    }
    /* where it all goes, folder by folder */
    for (int gi = 0; gi < p->ngroups; gi++) {
        const PvGroup *g = &p->groups[gi];
        H(b, "<section class=\"media-up-group\" id=\"media-up-g-%X\"><h3 class=\"media-up-dir\">",
          hash64(g->dir, strlen(g->dir)) ^ (uint64_t)g->out);
        ui_icon(b, g->out ? "x" : "folder");
        if (g->out) H(b, "<span>%s</span>", T("Left out"));
        else if (!g->dir[0]) H(b, "<span>%s</span>", T("The library folder itself"));
        else {
            buf_puts(b, "<span>");
            for (const char *s = g->dir; *s;) { /* "Series / 70's Show / Season 1" (breaks between parts) */
                size_t k = strcspn(s, "/");
                char part[256];
                str_ncopy(part, sizeof part, s, k);
                H(b, "%r%s", s == g->dir ? "" : " <i>/</i> ", part);
                s += k;
                if (*s) s++;
            }
            buf_puts(b, "</span>");
        }
        if (!g->out && g->fresh && g->files > g->skipped) H(b, "<small class=\"badge\">%s</small>", T("New folder"));
        buf_puts(b, "</h3><p class=\"muted media-small\">");
        pv_group_sum(b, g);
        if (g->kept) {
            buf_puts(b, " · ");
            H(b, "%s", T("Hemma can't tell where these belong, so they go in as they are"));
        }
        buf_puts(b, "</p><ul class=\"rows media-up-files\">");
        for (int ri = g->row0; ri < g->row0 + g->nrows; ri++) {
            const PvRow *r = &p->rows[ri];
            Buf w = {0}; /* what it is, and why it stays behind */
            pv_what(&w, r);
            const char *sep = w.len ? " · " : "";
            if (r->fate == FATE_SKIP || r->fate == FATE_SUBS || (r->fate == FATE_SEND && r->why[0])) {
                i18n_unpack_to(why, sizeof why, r->why);
                H(&w, "%r%s", sep, why);
            } else if (r->fate == FATE_COPY) {
                buf_puts(&w, sep);
                H(&w, T("the copy of %s, made on this device"), r->why);
            } else if (r->fate == FATE_HEAD) {
                H(&w, "%r%r", sep,
                  T("only its first MB: it looks like a \"play all\" copy of the episodes next to it (if it isn't, "
                    "upload it on its own)"));
            }
            H(b, "<li class=\"row\"><div class=\"row-main\"><span class=\"media-up-name\">%s</span>", r->name);
            if (w.len) H(b, "<small>%r</small>", buf_cstr(&w));
            buf_puts(b, "</div>");
            pv_fate(b, r);
            buf_puts(b, "</li>");
            buf_free(&w);
        }
        buf_puts(b, "</ul>");
        if (g->more) {
            buf_puts(b, "<p class=\"muted media-small\">");
            H(b, T("+%d more"), g->more);
            buf_puts(b, "</p>");
        }
        buf_puts(b, "</section>");
    }
    if (p->more_groups) {
        buf_puts(b, "<p class=\"muted media-small\">");
        H(b, TN(p->more_groups, "…and %d more folder", "…and %d more folders"), p->more_groups);
        buf_puts(b, "</p>");
    }
    (void)c;
}

/* --------------------------------------------------------------- the page */

static Batch *batch_of(int person) { /* the latest of this person's video uploads (lock held) */
    Batch *best = NULL;
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && !g_batch[s]->art && g_batch[s]->person == person &&
            (!best || g_batch[s]->started > best->started))
            best = g_batch[s];
    return best;
}

void upload_art_status(Ctx *c, Buf *b, uint64_t id) {
    if (!c->me || !id) return;
    lock_take(&g_lock);
    const Batch *u = NULL;
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && g_batch[s]->art == id && g_batch[s]->person == c->me->id &&
            (!u || g_batch[s]->started > u->started))
            u = g_batch[s];
    const UpFile *f = u ? &u->files[0] : NULL;
    char why[300];
    if (!f) {
    } else if (f->state == UP_FAILED) {
        H(b, "<p class=\"notice bad\" id=\"media-pic-st\">%s</p>", why_of(f, why, sizeof why));
    } else if (f->state == UP_DONE) {
        if (!u->ended || c->now - u->ended < 120)
            H(b, "<p class=\"notice\" id=\"media-pic-st\">%s</p>",
              T("Saved - the new picture shows everywhere in a moment."));
    } else if (!u->stopped) {
        int pct = f->size ? (int)(f->saved * 100 / f->size) : 0;
        buf_puts(b, "<p class=\"notice\" id=\"media-pic-st\">");
        H(b, T("Sending the picture… %d%%"), pct);
        buf_puts(b, "</p>");
    }
    lock_give(&g_lock);
}

/* How far a batch is, 0..1000. With copies being made it counts work, not bytes: making a
   copy is most of it (weighed by its original's size), sending it the rest. */
static int batch_permille(const Batch *u) {
    double all = 0, done = 0;
    for (int i = 0; i < u->nfiles; i++) {
        const UpFile *f = &u->files[i];
        if (f->state == UP_SKIPPED) continue;
        bool copy = f->conv == CV_WAIT || f->conv == CV_READY;
        double w = (double)MAX(copy ? f->orig : f->size, 1), sent = f->size ? (double)f->saved / (double)f->size : 0;
        all += w;
        done += w * (f->state == UP_DONE || f->state == UP_FAILED ? 1
                     : f->conv == CV_WAIT                         ? 0.85 * f->pm / 1000.0
                     : f->conv == CV_READY                        ? 0.85 + 0.15 * sent
                                                                  : sent);
    }
    return all > 0 ? (int)(done * 1000 / all) : 1000;
}

/* THE CARD (1.5; owner, 2026-10-27: "it sort of spams between texts and is quite un representatry of what is
   happening, make it more modern" - "a list of the most processed items going up, and make it say if it's
   processing / uploading"): the upload's state, its files and one bar; three numbers that keep their places (sent,
   speed, time left - a dash while there's none); then what's under way, the furthest along on top: just saved,
   uploading, ready (made on the sending device, waiting its turn), processing (being made there - or read for its
   subtitles). A copy kept in memory goes up in 16 MB pieces, each its own PUT: a file stays "uploading" until UP_GAP_S
   after its last byte, so the card no longer flips between pieces ("Now: ..." came and went every second or two). */
#define UP_GAP_S 30
#define UP_SPEED_KEEP_S 60 /* the speed shows this long after the last byte (between files: the last one's) */
#define UP_PACE 8          /* time left with copies still to make: the pace of the last this many saved */
enum { ROW_SAVED, ROW_UP, ROW_READY, ROW_PROC, ROW_SUBS };
static const int ROW_CAP[] = {2, 4, 4, 8}; /* rows shown of each (subtitles count with processing) */

typedef struct {
    int i, kind;
    int64_t key; /* within its kind, higher first */
} UpRow;

static int row_kind(const UpFile *f, int64_t now) {
    if (f->subs) return f->pm >= 1000 ? ROW_SAVED : f->pm > 0 ? ROW_SUBS : -1;
    if (f->state == UP_DONE) return ROW_SAVED;
    if (f->state == UP_WRITING) return ROW_UP;
    if (f->state != UP_QUEUED) return -1;
    if (f->saved > 0 && f->conv != CV_WAIT && now - f->touched <= UP_GAP_S) return ROW_UP;
    if (f->conv == CV_READY || (f->conv == CV_WAIT && f->pm >= 1000)) return ROW_READY;
    return f->conv == CV_WAIT && f->pm > 0 ? ROW_PROC : -1;
}

static int row_rank(int kind) { return kind == ROW_SUBS ? ROW_PROC : kind; }

static int row_cmp(const void *x, const void *y) {
    const UpRow *a = x, *b = y;
    if (row_rank(a->kind) != row_rank(b->kind)) return row_rank(a->kind) - row_rank(b->kind);
    return a->key < b->key ? 1 : a->key > b->key ? -1 : 0;
}

/* The file as the sending device names it ("x.mkv", not the copy's "x.web.mp4"), below its folders or not */
static void sent_name(const UpFile *f, char *out, size_t cap, bool whole) {
    const char *name = whole ? f->rel : base_name(f->rel);
    if (media_is_web_copy(name) && f->ext[0]) snprintf(out, cap, "%.*s.%s", (int)media_stem_len(name), name, f->ext);
    else str_copy(out, cap, name);
}

/* What it is: "Family Guy · S04E12 · Breaking Out Is Hard to Do", "Kill Bill (2003) · part 2", else its name (an
   audiobook's file always: "01.mp3", with its folder below) */
static void row_label(Buf *b, const UpFile *f) {
    char rel[420];
    sent_name(f, rel, sizeof rel, true);
    if (media_is_audio_ext(rel)) {
        H(b, "%s", base_name(rel));
        return;
    }
    MediaName n;
    media_parse_name(rel, &n);
    if (n.kind == MEDIA_EPISODE && n.episode > 0 && n.title[0]) {
        if (n.episode_end > n.episode) H(b, "%s · S%02dE%02d–E%02d", n.title, n.season, n.episode, n.episode_end);
        else H(b, "%s · S%02dE%02d", n.title, n.season, n.episode);
        if (n.ep_title[0]) H(b, " · %s", n.ep_title);
    } else if (n.kind == MEDIA_MOVIE && n.title[0]) {
        if (n.year) H(b, "%s (%d)", n.title, n.year);
        else H(b, "%s", n.title);
        int part = media_name_part(rel);
        if (part) {
            buf_puts(b, " · ");
            H(b, T("part %d"), part);
        }
    } else {
        H(b, "%s", base_name(rel));
    }
}

static void render_row(Buf *b, const Batch *u, int i, int kind) {
    static const char *const TAG[] = {N_("Saved"), N_("Uploading"), N_("Ready"), N_("Processing"), N_("Subtitles")};
    static const char *const CLS[] = {"saved", "up", "ready", "proc", "subs"};
    const UpFile *f = &u->files[i];
    int pm = kind == ROW_PROC || kind == ROW_SUBS ? CLAMP(f->pm, 0, 999)
             : kind == ROW_SAVED                  ? 1000
             : f->size > 0                        ? (int)(MIN(f->saved, f->size) * 1000 / f->size)
                                                  : 0;
    char name[300], dir[400] = "";
    sent_name(f, name, sizeof name, false);
    Buf label = {0}, shown = {0}; /* (both as on the page: escaped) */
    row_label(&label, f);
    H(&shown, "%s", name);
    const char *slash = strrchr(f->rel, '/');
    bool same = str_eq(buf_cstr(&label), buf_cstr(&shown)); /* (its name said nothing more: where it goes, then) */
    if (same && slash) str_ncopy(dir, sizeof dir, f->rel, (size_t)(slash - f->rel));
    H(b, "<li class=\"media-upc-row media-upc-r-%s\" id=\"media-upc-%X-%d\"><span class=\"media-upc-tag\">%s</span>"
         "<span class=\"media-upc-what\"><b>%r</b><small>%s</small></span><span class=\"media-upc-num\">",
      CLS[kind], u->id, i, T(TAG[kind]), buf_cstr(&label), same ? dir : name);
    buf_free(&label);
    buf_free(&shown);
    if (kind == ROW_SAVED) {
        if (f->touched) ui_hm(b, f->touched);
    } else if (kind != ROW_READY || pm > 0) {
        H(b, "%d%%", pm / 10);
        if ((kind == ROW_PROC || kind == ROW_SUBS) && f->paced && now_ms() - f->fed_at < PACE_SHOW_MS) {
            char pace[24]; /* (how fast it takes in the original: is it doing something?) */
            media_size_text((int64_t)f->pace, pace, sizeof pace);
            H(b, " · %s/s", pace);
        }
    }
    /* (a ready copy's bar is full: the making is done - its own colour says it waits) */
    H(b, "</span><span class=\"media-upc-rbar\" aria-hidden=\"true\"><i style=\"width:%d.%d%%\"></i></span></li>",
      kind == ROW_READY && !pm ? 100 : pm / 10, kind == ROW_READY && !pm ? 0 : pm % 10);
}

/* What's left, in seconds (-1: can't tell yet). While copies are still being made, the pace of the last files
   saved - their sizes are guesses until made, and the making sets the pace; else the bytes at the speed. */
static int64_t time_left(const Batch *u, bool making, int files_left, int64_t now) {
    if (files_left <= 0) return -1;
    if (!making) return u->rate > 1 && u->total > u->moved ? (int64_t)((double)(u->total - u->moved) / u->rate) : -1;
    int64_t t[UP_PACE];
    int n = 0;
    for (int i = 0; i < u->nfiles; i++) { /* the newest UP_PACE saves, newest first */
        const UpFile *f = &u->files[i];
        if (f->state != UP_DONE || !f->touched) continue;
        int k = n < UP_PACE ? n++ : UP_PACE;
        while (k > 0 && t[k - 1] < f->touched) {
            if (k < UP_PACE) t[k] = t[k - 1];
            k--;
        }
        if (k < UP_PACE) t[k] = f->touched;
    }
    if (n < 3 || t[0] <= t[n - 1]) return -1;
    double each = (double)(t[0] - t[n - 1]) / (n - 1);
    return MAX((int64_t)(each * files_left) - MIN(now - t[0], (int64_t)each), 0);
}

static void render_left(Buf *b, int64_t s) { /* "about 3 h 20 min": coarser the longer it is, so it doesn't twitch */
    int64_t step = s > 2 * 3600 ? 600 : s > 3600 ? 300 : 60;
    Buf d = {0};
    ui_duration(&d, MAX((s + step / 2) / step * step, 60));
    H(b, T("about %s"), buf_cstr(&d));
    buf_free(&d);
}

static void render_batch(Ctx *c, Buf *b, const Batch *u) {
    char moved[24], total[24], speed[24], why[300];
    media_size_text(u->moved, moved, sizeof moved);
    media_size_text(u->total, total, sizeof total);
    int todo = u->nfiles - u->skipped, copies = 0, pending = 0, as_is = 0, heads = 0, done = u->done, videos_left = 0;
    int ups = 0, procs = 0, rest = 0, nrows = 0;
    UpRow *rows = xcalloc((size_t)MAX(u->nfiles, 1), sizeof *rows);
    for (int i = 0; i < u->nfiles; i++) {
        const UpFile *f = &u->files[i];
        if (f->part_of) { /* (2.7) a part of a video: not a file of the upload - its video's row says how far */
            todo -= f->state != UP_SKIPPED;
            ups += row_kind(f, c->now) == ROW_UP;
            continue;
        }
        copies += f->conv != CV_NONE;
        pending += f->conv == CV_WAIT && f->state == UP_QUEUED;
        as_is += f->conv == CV_ORIGINAL && !f->plays;
        heads += f->head && f->state != UP_SKIPPED;
        todo += f->subs; /* (only its subtitles: a file of the upload too, done once they're sent) */
        done += f->subs && !subs_due(f);
        bool going = f->state == UP_QUEUED || f->state == UP_WRITING;
        videos_left += going;
        rest += going || subs_due(f);
        int k = row_kind(f, c->now);
        if (k < 0 || (k == ROW_SAVED && !f->touched)) continue;
        ups += k == ROW_UP;
        procs += k == ROW_PROC || k == ROW_SUBS;
        int64_t key = k == ROW_SAVED   ? f->touched * 8192 - i
                      : k == ROW_READY ? -i
                      : k == ROW_UP    ? (f->size > 0 ? MIN(f->saved, f->size) * 1000 / f->size : 0) * 8192 - i
                                       : (int64_t)f->pm * 8192 - i;
        rows[nrows++] = (UpRow){i, k, key};
    }
    bool open = batch_open(u);
    bool waiting = open && !u->workers && c->now - u->last > 30;
    const char *state = u->stopped ? N_("Stopped")
                        : !open    ? N_("Finished")
                        : waiting  ? N_("Waiting for the browser")
                        : !ups && procs ? N_("Processing")
                                        : N_("Uploading");
    const char *cls = u->stopped ? "stop" : !open ? "done" : waiting ? "wait" : !ups && procs ? "proc" : "up";
    int per_mille = !open && !u->stopped ? 1000
                    : copies ? batch_permille(u)
                    : u->total ? (int)(u->moved * 1000 / u->total) : 1000;
    per_mille = CLAMP(per_mille, 0, 1000);

    /* the state, the files, how far (the heading reads "Uploading · 3 of 9 files" as a sentence) */
    H(b, "<section class=\"media-up media-upc\" id=\"media-up-%X\"><div class=\"media-upc-top\"><h3 class=\"media-upc-head\">"
         "<span class=\"media-upc-state media-upc-s-%s\"><i aria-hidden=\"true\"></i>%s</span>"
         "<span class=\"sr-only\"> · </span><span class=\"media-upc-count\">",
      u->id, cls, T(state));
    if (!open && !u->stopped) H(b, TN(done, "%d file sent", "%d files sent"), done);
    else if (open && !waiting) H(b, TN(todo, "%d of %d file", "%d of %d files"), done, todo);
    else H(b, TN(todo, "%d of %d file sent", "%d of %d files sent"), done, todo);
    H(b, "</span></h3><span class=\"media-upc-pct\">%d%%</span></div>", per_mille / 10);
    H(b, "<div class=\"media-upc-bar\" role=\"progressbar\" aria-valuemin=\"0\" aria-valuemax=\"100\" aria-valuenow=\"%d\" "
         "aria-label=\"%s\"><i style=\"width:%d.%d%%\"></i></div>",
      per_mille / 10, T("How far the upload is"), per_mille / 10, per_mille % 10);

    /* three numbers in their places */
    H(b, "<dl class=\"media-upc-stats%r\"><div><dt>%s</dt><dd>", open ? "" : " media-upc-two", T("Sent"));
    if (!open && !u->stopped) H(b, "%s", moved);
    else H(b, pending ? T("%s of about %s") : T("%s of %s"), moved, total);
    buf_puts(b, "</dd></div>");
    if (open) {
        H(b, "<div><dt>%s</dt><dd>", T("Speed"));
        if (!waiting && u->rate > 1 && c->now - u->wrote <= UP_SPEED_KEEP_S) {
            media_size_text((int64_t)u->rate, speed, sizeof speed);
            H(b, "%s/s", speed);
        } else {
            buf_puts(b, "–");
        }
        H(b, "</dd></div><div><dt>%s</dt><dd>", T("Time left"));
        int64_t left = waiting ? -1 : time_left(u, pending > 0, videos_left, c->now);
        if (left >= 0) render_left(b, left);
        else buf_puts(b, "–");
        buf_puts(b, "</dd></div>");
    } else if (u->ended >= u->started && u->done) {
        int64_t took = MAX(u->ended - u->started, 1);
        H(b, "<div><dt>%s</dt><dd>", T("Took"));
        if (took < 60) buf_puts(b, T("less than a minute"));
        else {
            Buf d = {0};
            ui_duration(&d, took);
            H(b, "%s", buf_cstr(&d));
            buf_free(&d);
        }
        buf_puts(b, "</dd></div>");
    }
    buf_puts(b, "</dl>");

    /* what's under way, the furthest along on top */
    if (open && nrows) {
        qsort(rows, (size_t)nrows, sizeof *rows, row_cmp);
        int used[4] = {0};
        H(b, "<ul class=\"media-upc-list\" aria-label=\"%s\">", T("Under way"));
        for (int r = 0; r < nrows; r++) {
            int rank = row_rank(rows[r].kind);
            if (used[rank] >= ROW_CAP[rank]) continue;
            used[rank]++;
            if (rank != ROW_SAVED) rest--;
            render_row(b, u, rows[r].i, rows[r].kind);
        }
        buf_puts(b, "</ul>");
    }
    free(rows);
    if (open && rest > 0) {
        buf_puts(b, "<p class=\"media-upc-rest\">");
        H(b, TN(rest, "%d more waiting", "%d more waiting"), rest);
        buf_puts(b, "</p>");
    }

    if (waiting)
        H(b, "<p class=\"muted media-small\">%s</p>",
          T("If the browser that started it was closed, choose the same files again and press Start - what's already "
            "on the server is skipped, and half-sent files continue where they stopped."));
    else if (open)
        H(b, "<p class=\"muted media-small\">%s</p>",
          T("Keep the tab that started it open until it's done - you can use other pages meanwhile."));
    if (as_is) { /* said out loud, not only in the list below: such a video may play silent or not at all */
        const UpFile *first = NULL;
        for (int i = 0; i < u->nfiles && !first; i++)
            if (u->files[i].conv == CV_ORIGINAL && !u->files[i].plays) first = &u->files[i];
        H(b, "<p class=\"notice media-up-asis\" id=\"media-up-asis-%X\">", u->id);
        H(b,
          TN(as_is,
             "%d video couldn't be converted on this device and went up as it was - it may not play everywhere. Why: %s.",
             "%d videos couldn't be converted on this device and went up as they were - they may not play everywhere. "
             "Why: %s (each one's reason is in the list below)."),
          as_is, why_of(first, why, sizeof why));
        buf_puts(b, "</p>");
    }
    if (u->failed || u->skipped || as_is || heads) {
        buf_puts(b, "<details class=\"media-up-list\"><summary>");
        H(b, T("%d skipped"), u->skipped);
        if (heads) {
            buf_puts(b, ", ");
            H(b, TN(heads, "%d play-all header", "%d play-all headers"), heads);
        }
        if (as_is) {
            buf_puts(b, ", ");
            H(b, T("%d sent unconverted"), as_is);
        }
        if (u->failed) {
            buf_puts(b, ", ");
            H(b, T("%d failed"), u->failed);
        }
        buf_puts(b, "</summary><ul>");
        int shown = 0;
        for (int i = 0; i < u->nfiles && shown < 200; i++) {
            const UpFile *f = &u->files[i];
            if (f->state == UP_SKIPPED || f->state == UP_FAILED)
                H(b, "<li%r><code class=\"media-code\">%s</code> — %s</li>", f->state == UP_FAILED ? " class=\"bad\"" : "",
                  f->rel, why_of(f, why, sizeof why));
            else if (f->conv == CV_ORIGINAL && !f->plays) {
                H(b, "<li><code class=\"media-code\">%s</code> — ", f->rel);
                H(b, T("sent as it is: %s"), why_of(f, why, sizeof why));
                buf_puts(b, "</li>");
            } else if (f->head) /* a long special can look like one: this is where you'd notice */
                H(b, "<li><code class=\"media-code\">%s</code> — %r</li>", f->rel,
                  T("only its first MB: it looks like a \"play all\" copy of the episodes next to it (if it isn't, "
                    "upload it on its own)"));
            else continue;
            shown++;
        }
        buf_puts(b, "</ul></details>");
    }
    if (open) {
        ui_form_confirm(b, "media.upload_stop", "inline",
                        T("Stop the upload? Everything already sent stays on the server."));
        H(b, "<input type=\"hidden\" name=\"batch\" value=\"%X\"><button class=\"btn\">%s</button>", u->id, T("Stop"));
        ui_form_end(b);
    } else if (!u->stopped && u->done) { /* (the library finds them by itself a moment after) */
        H(b, "<p><a class=\"btn sm\" href=\"/media\">%s</a></p>", T("See what's new"));
    }
    buf_puts(b, "</section>");
}

/* The drop area's picture (core/ui.c has no icon for sending something up) */
#define UPLOAD_ICON                                                                                                    \
    "<svg class=\"i media-up-ico\" viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"M12 15.5V4M7.5 8.5 12 4l4.5 "  \
    "4.5\"/><path d=\"M4 14.5v4A1.5 1.5 0 0 0 5.5 20h13a1.5 1.5 0 0 0 1.5-1.5v-4\"/></svg>"

/* The upload page (owner, 2026-10-21: "easy enough to understand"): one big place to drop videos or a
   folder (or choose them), then the server's preview of what Start will do - where each file goes, what
   gets converted here, what's skipped and why - then Start. The rest is folded under "More options".
   The form comes first and keeps its place (it holds the chosen files, wire.js keeps them on it):
   progress cards appear after it, so live updates never rebuild it. */
void upload_page(Ctx *c, Buf *b) {
    /* ?series=<id> (a series page's "Add episodes"): everything goes into that series' folder */
    uint64_t sid = 0;
    const MediaSeries *ser = media_parse_id(ctx_q(c, "series"), &sid) ? media_series(sid) : NULL;
    char shex[17] = "";
    if (ser) media_id_hex(ser->id, shex);
    ctx_title(c, "%s", ser ? T("Add episodes") : T("Add videos"));
    if (ser) H(b, "<a class=\"media-back\" href=\"/media/series/%s\">", shex);
    else buf_puts(b, "<a class=\"media-back\" href=\"/media\">"); /* (Add videos sits in Watch's header) */
    ui_icon(b, "left");
    H(b, "%s</a>", ser ? ser->title : T("Watch"));
    ui_head(b, ser ? T("Add episodes") : T("Add videos"),
            T("Send movies, series and audiobooks from this device to the library"));
    ui_head_end(b);
    if (!is_admin(c)) {
        ui_empty(b, "🔒", T("Admins only"), NULL);
        Buf who = {0};
        ui_admins(&who);
        buf_puts(b, "<p class=\"center muted\">");
        H(b, T("Ask %r to add videos to the library."), buf_cstr(&who));
        buf_puts(b, "</p>");
        buf_free(&who);
        return;
    }
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS);
    if (!nroots) {
        ui_empty(b, "📁", T("Choose a library folder first"),
                 T("Uploads go into one of the library's folders. Add one in the library settings."));
        H(b, "<p class=\"center\"><a class=\"btn primary\" href=\"/media/settings\">%s</a></p>", T("Library settings"));
        return;
    }
    const Asset *wasm = asset_named("/av.wasm"), *worker = asset_named("/conv.js");
    bool here = wasm && worker; /* the browser converter is built in (it needs zig to build) */
    bool busy_now = false;      /* someone's upload is running: a new one needs "upload anyway" */
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) busy_now |= running(g_batch[s]);
    lock_give(&g_lock);
    const Preview *pv = pv_for(c);

    buf_puts(b, "<div class=\"grid2\">");
    ui_card(b, NULL, NULL, NULL);
    H(b,
      "<form class=\"stack media-up-form\" id=\"media-up-form\" data-upload=\"/media/upload\" "
      "data-preview=\"media.upload_preview\" data-empty=\"%s\"",
      T("Choose videos or a folder first"));
    if (here) upload_converter_attrs(c, b); /* (2.7: with this device's data-workers) */
    if (pv && pv->no_year) { /* (2.4: "almost mandatory" - Start asks first while films lack their years) */
        char ask[200];
        snprintf(ask, sizeof ask, TN(pv->no_year, "%d film has no year. Upload it anyway?", "%d films have no year. Upload them anyway?"),
                 pv->no_year);
        H(b, " data-confirm=\"%s\"", ask);
    }
    buf_puts(b, "><input type=\"hidden\" name=\"sort\" value=\"1\">");
    if (ser) {
        H(b, "<input type=\"hidden\" name=\"series\" value=\"%s\"><p class=\"notice\">", shex);
        H(b, T("Everything goes into the folder of <b>%s</b> - each episode into its season, whatever the file names say "
               "about the show."),
          ser->title);
        buf_puts(b, "</p>");
    }

    /* 1. the drop area (a phone gets the picker: nothing to drop there) */
    H(b,
      "<div class=\"media-up-drop\" id=\"media-up-drop\" data-drop>%r<p class=\"media-up-drop-t\">%s</p>"
      "<p class=\"media-up-drop-tap\">%s</p><p class=\"muted media-small\">%s</p><div class=\"media-up-pick\">"
      "<label class=\"btn primary\">%s<input type=\"file\" name=\"files\" multiple class=\"sr-only\" "
      "accept=\"video/*,.mkv,.avi,.m2ts,.ts,.srt,.vtt,.sup,.idx,.sub,.jpg,.jpeg,.png,.webp,.m4b,.m4a,.mp3,audio/*\">"
      "</label><label class=\"btn media-up-folder\">%s<input type=\"file\" name=\"folder\" webkitdirectory multiple "
      "class=\"sr-only\"></label></div></div>",
      UPLOAD_ICON, T("Drop videos, audiobooks or a whole folder here"), T("Choose videos from this device"),
      ser ? T("Hemma reads each file's name first, and nothing is sent before you press Start.")
          : T("Hemma sorts them by their names - series into Series, films into Movies, audiobooks into Audiobooks - "
              "and shows you where each one goes before anything is sent."),
      T("Choose videos"), T("Choose a folder"));

    /* 2. what Start will do with them */
    buf_puts(b, "<div class=\"media-up-pv\" id=\"media-up-pv\">");
    if (pv) pv_draw(c, b, pv);
    buf_puts(b, "</div>");

    /* 3. where the copies made here wait to be sent: memory or this device's disk - in sight, not folded */
    if (here)
        H(b,
          "<label class=\"media-up-ram\" id=\"media-up-ram\"><input type=\"checkbox\" name=\"ram\" value=\"1\"%r>"
          "<span><b>%s</b><small class=\"muted\">%s</small></span></label>",
          str_eq(pref(c->me->id, PREF_UPLOAD_RAM, ""), "1") ? " checked" : "",
          T("Keep converted videos in memory until they're sent"),
          T("The copies wait in this device's memory (RAM) instead of on its disk, so nothing piles up there and the "
            "disk only reads the originals - up to 8 GB of them, the rest on the disk. Leave it off on a phone or a "
            "computer with little memory."));

    /* 4. the rest, folded away */
    H(b, "<details class=\"media-up-more\" id=\"media-up-more\"><summary>%s</summary><div class=\"stack\">",
      T("More options"));
    if (!ser) {
        if (nroots > 1) {
            H(b, "<label class=\"field\"><span>%s</span><select name=\"root\">", T("Save into"));
            for (int i = 0; i < nroots; i++) H(b, "<option value=\"%d\">%s</option>", i, roots[i]);
            buf_puts(b, "</select></label>");
        } else {
            buf_puts(b, "<input type=\"hidden\" name=\"root\" value=\"0\">");
        }
        H(b,
          "<label class=\"check\"><input type=\"checkbox\" name=\"keep\" value=\"1\"> %s</label>"
          "<label class=\"field media-up-into\"><span>%s</span><input name=\"into\" maxlength=\"120\" placeholder=\"%s\" "
          "spellcheck=\"false\"></label>",
          T("Keep the folders' own layout instead of sorting by names"), T("Inside the folder (optional)"),
          T("e.g. Series"));
    }
    if (here) {
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"conv\" value=\"1\" checked> %s</label>",
          T("Make videos play everywhere on this device first (recommended)"));
        /* (2.7; owner: "Yes, off by default") HEVC made H.264 while uploading: one pass on the sending computer instead
           of up, back down and up again with "On this device" later - slow, and not needed where every device plays HEVC */
        char cant[200];
        int n = media_devices_no_hevc(c, cant, sizeof cant);
        H(b,
          "<label class=\"check media-up-h264\"><input type=\"checkbox\" name=\"h264\" value=\"1\"><span><b>%s</b>"
          "<small class=\"muted\">%s ",
          T("Make HEVC videos play everywhere too (H.264)"),
          T("HEVC (x265) videos are re-encoded to H.264 before they're sent, instead of copied as they are: every device "
            "plays them then, but it's slow - an episode takes about as long as it lasts, or longer, on each core."));
        if (n < 0) H(b, "%s", T("No device of yours has opened Watch yet."));
        else if (!n) H(b, "%s", T("Every device of yours that has opened Watch plays HEVC already."));
        else H(b, T("These can't play HEVC: %s."), cant);
        buf_puts(b, "</small></span></label>");
        int room = (int)CLAMP(str_int_or(pref(c->me->id, PREF_UPLOAD_ROOM, ""), ROOM_DEFAULT), ROOM_MIN, ROOM_MAX);
        buf_puts(b, "<label class=\"field\"><span>"); /* (wire.js [data-show] keeps the number with the slider) */
        H(b, T("Room on this device for copies waiting to be sent: <b id=\"media-room-n\">%d</b> GB"), room);
        H(b,
          "</span><input type=\"range\" name=\"room\" min=\"%d\" max=\"%d\" step=\"1\" value=\"%d\" "
          "data-show=\"media-room-n\" aria-describedby=\"media-room-h\"><small class=\"muted\" "
          "id=\"media-room-h\">%r</small></label>",
          ROOM_MIN, ROOM_MAX, room,
          T("More room lets more videos convert side by side - DVD rips take minutes each. A copy is let go as soon "
            "as it's sent."));
    }
    H(b,
      "<label class=\"check\"><input type=\"checkbox\" name=\"originals\" value=\"1\"> %s</label>"
      "<label class=\"check\"><input type=\"checkbox\" name=\"dupes\" value=\"1\"> %s</label>"
      "<p class=\"muted media-small\">%s</p><p class=\"muted media-small\">%r</p></div></details>",
      T("Also send the originals"), T("Also send episodes and films the library already has under another name"),
      T("What's already on the server is skipped, and so is what the library has under another name - before "
        "anything is converted. If the connection drops, Start again carries on where it stopped."),
      here ? T("Converting makes a copy that plays everywhere, about a third of the size, and only the copy is sent. "
               "It keeps this device busy meanwhile - a computer does it much faster than a phone.")
           : T("This Hemma was built without the video converter, so videos are sent as they are (convert them under "
               "Library settings afterwards)."));

    H(b, "<noscript><p class=\"notice\">%s</p></noscript>",
      T("Adding videos needs JavaScript - turn it on for this page, or use another browser."));
    /* always there, hidden while nothing runs: live updates then never rebuild the form (and lose the files) */
    H(b, "<label class=\"check\" id=\"media-up-force\"%r><input type=\"checkbox\" name=\"force\" value=\"1\"> %r</label>",
      busy_now ? "" : " hidden",
      T("Upload anyway, alongside the upload that's running (its files are left to it)"));
    buf_puts(b, "<div class=\"modal-f\">");
    if (pv)
        H(b, "<button type=\"button\" class=\"btn ghost\" id=\"media-up-clear\" data-drop-clear>%s</button>",
          T("Clear the list"));
    H(b, "<button class=\"btn primary\" id=\"media-up-start\">%s</button></div></form>", T("Start"));
    ui_card_end(b);
    lock_take(&g_lock);
    const Batch *mine = batch_of(c->me->id);
    if (mine) {
        ui_card(b, T("Your upload"), NULL, NULL);
        render_batch(c, b, mine);
        ui_card_end(b);
    }
    for (int s = 0; s < MAX_BATCHES; s++) { /* other admins' uploads, so nobody is surprised */
        const Batch *o = g_batch[s];
        if (!o || o == mine || o->art || !batch_open(o)) continue;
        const Person *p = person_get(o->person);
        ui_card(b, p ? p->name : T("Someone"), NULL, NULL);
        render_batch(c, b, o);
        ui_card_end(b);
    }
    lock_give(&g_lock);
    buf_puts(b, "</div>");
}

/* (2.6) the person's own "On this device" batch (HEVC made H.264 by their browser) - its card where they started it: the
   HEVC card, Manage, a video's page (video: only a batch making that one) - while it runs and 10 minutes after; false =
   none. (Re-drawn with every /media page: the upload's changes re-render them all.) */
bool upload_here_card(Ctx *c, Buf *b, uint64_t video) {
    if (!c->me || !is_admin(c)) return false;
    lock_take(&g_lock);
    const Batch *u = batch_of(c->me->id);
    bool show = u && u->nfiles && u->files[0].from && (batch_open(u) || !u->ended || now_sec() - u->ended < 600);
    for (int i = 0; show && video && i < u->nfiles && u->files[i].video != video; i++)
        if (i == u->nfiles - 1) show = false; /* (a video's page: only a batch with it) */
    if (show) {
        H(b, "<div class=\"media-h264-run\" id=\"media-h264-run\"><p><b>%s</b></p>", T("Converting on this device"));
        render_batch(c, b, u);
        buf_puts(b, "</div>");
    }
    lock_give(&g_lock);
    return show;
}

void upload_summary(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    lock_take(&g_lock);
    int open = 0, files = 0, done = 0;
    for (int s = 0; s < MAX_BATCHES; s++) {
        const Batch *o = g_batch[s];
        if (!o || o->art || !batch_open(o)) continue;
        open++;
        files += o->nfiles - o->skipped;
        done += o->done;
    }
    lock_give(&g_lock);
    if (!open) return;
    buf_puts(b, "<p class=\"media-small\">");
    H(b, TN(files, "Uploading now: %d of %d file sent.", "Uploading now: %d of %d files sent."), done, files);
    buf_puts(b, "</p>");
}

void upload_act_stop(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    /* the upload whose Stop was pressed (another admin's card has its own); without one, your own */
    uint64_t which = 0;
    bool one = media_parse_id(fields_str(f, "batch"), &which);
    int stopped = 0;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) {
        Batch *o = g_batch[s];
        if (o && batch_open(o) && (one ? o->id == which : o->person == c->me->id)) {
            o->stopped = true;
            o->ended = now_sec();
            stopped++;
        }
    }
    lock_give(&g_lock);
    if (stopped) ctx_ok(c, "%s", T("Upload stopped - what was sent stays"));
    else ctx_info(c, "%s", T("Nothing is uploading"));
}

/* -------------------------------------------------------------- lifecycle */

void upload_start(void) {
    atomic_store(&g_halt, false);
    g_since = now_sec();
    g_pace_window_ms = PACE_WINDOW_MS; /* (a test's setting doesn't outlive it) */
}

/* (2.7) A video made in parts on a device: the server joins them with its sound and subtitles (convert_file's join -
   a remux of the picture, the sound as any conversion's), on a thread of its own, then it's done as a sent copy is. */
typedef struct {
    uint64_t batch;
    int i, parts;
    bool replace;
    char src[1024], dst[1024];
} Join;

static void join_worker(void *arg) {
    Join *jn = arg;
    thread_background();
    ConvertJob *cj = xcalloc(1, sizeof *cj); /* (its own: the thread's) */
    str_copy(cj->src, sizeof cj->src, jn->src);
    str_copy(cj->dst, sizeof cj->dst, jn->dst);
    cj->h264 = true, cj->replace = jn->replace, cj->npieces = jn->parts;
    bool ok = convert_file(cj);
    if (ok) log_info("upload: joined %s (%s)", cj->dst, cj->what);
    else log_warn("upload: couldn't join the parts of %s - %s", cj->dst, cj->err);
    lock_take(&g_lock);
    Batch *b = find(jn->batch);
    if (b && jn->i < b->nfiles) {
        UpFile *f = &b->files[jn->i];
        f->joining = false;
        f->touched = now_sec();
        if (ok) {
            f->state = UP_DONE;
            f->settle = f->from && f->video;
            b->done++;
            g_files_saved++;
        } else {
            f->state = UP_FAILED;
            why_set(f, cj->err);
            b->failed++;
        }
        b->workers--;
        b->last = b->busy = now_sec();
    }
    lock_give(&g_lock);
    free(cj);
    free_on_loop(jn); /* (the loop's) */
    app_tick_soon();
}

void upload_tick(int64_t now) {
    uint64_t sig = 0; /* (unsigned: it wraps, by design) */
    int saved = 0, pictures = 0;
    bool active = false;
    int finished_person = 0, finished_files = 0;
    struct {
        uint64_t video;
        char dst[1024];
    } settle[8]; /* (2.7) "here" copies just in place (a few a tick: the rest the next) */
    int nsettle = 0;
    Join *joins[4]; /* (2.7) videos whose parts are all here: joined on threads */
    uint64_t join_from[4];
    int njoin = 0;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) {
        Batch *b = g_batch[s];
        if (!b) continue;
        for (int i = 0; i < b->nfiles && njoin < countof(joins); i++) {
            UpFile *f = &b->files[i];
            if (!f->parts || f->state != UP_QUEUED || f->joining) continue;
            int done = 0, bad = 0;
            for (int q = 0; q < b->nfiles; q++) {
                const UpFile *p = &b->files[q];
                if (p->part_of != i + 1) continue;
                done += p->state == UP_DONE;
                bad += p->state == UP_FAILED || p->state == UP_SKIPPED;
            }
            if (bad && done + bad == f->parts) { /* (one couldn't be made, or its video went: the rest are no use) */
                f->state = UP_FAILED;
                why_set(f, N_("a part of its copy couldn't be made"));
                b->failed++;
                char dst[1024];
                join(dst, sizeof dst, b->root, f->rel);
                for (int q = 0; q < f->parts; q++) {
                    char piece[1100];
                    convert_piece_path(dst, q, piece, sizeof piece);
                    file_remove(piece);
                }
                continue;
            }
            if (done < f->parts) continue;
            Join *jn = xcalloc(1, sizeof *jn);
            jn->batch = b->id, jn->i = i, jn->parts = f->parts, jn->replace = f->replace;
            join(jn->dst, sizeof jn->dst, b->root, f->rel);
            join_from[njoin] = f->from;
            joins[njoin++] = jn;
            f->joining = true;
            b->workers++; /* (the batch stays while it's joined) */
        }
        for (int i = 0; i < b->nfiles && nsettle < countof(settle); i++) {
            UpFile *f = &b->files[i];
            if (!f->settle) continue;
            f->settle = false;
            settle[nsettle].video = f->video;
            join(settle[nsettle].dst, sizeof settle[nsettle].dst, b->root, f->rel);
            nsettle++;
        }
        /* speed: smoothed over ~5 s, measured while bytes come - also across the pauses between a copy's pieces
           (each its own PUT, 1.3), which used to restart it every 16 MB */
        if (b->workers || (b->wrote && now_sec() - b->wrote <= 2)) {
            active = true;
            if (b->rate_at && now - b->rate_at >= 900) {
                double inst = (double)(b->moved - b->rate_moved) * 1000.0 / (double)(now - b->rate_at);
                b->rate = b->rate > 0 ? b->rate * 0.75 + inst * 0.25 : inst;
            }
            if (!b->rate_at || now - b->rate_at >= 900) {
                b->rate_at = now;
                b->rate_moved = b->moved;
            }
        } else {
            b->rate_at = 0;
        }
        bool open = batch_open(b);
        if (!open && !b->ended && !b->workers) {
            b->ended = now_sec();
            if (!b->art) { /* a picture shows itself; no toast */
                finished_person = b->person;
                finished_files = b->done;
            }
        }
        int64_t idle = now_sec() - (b->ended ? b->ended : b->last);
        if (!b->workers && ((b->ended && idle > KEEP_DONE_S) || idle > FORGET_IDLE_S)) {
            batch_free(b);
            g_batch[s] = NULL;
            sig++;
            continue;
        }
        sig = sig * 31 + (uint64_t)b->moved + (uint64_t)b->done * 7 + (uint64_t)b->failed * 13 +
              (uint64_t)b->workers * 17 + b->stopped + (b->ended != 0) * 3u;
        for (int i = 0; i < b->nfiles; i++) /* copies being made and subtitles being read move the card too */
            if (b->files[i].pm > 0 && b->files[i].pm < 1000)
                sig = sig * 31 + (uint64_t)b->files[i].pm + (uint64_t)i + (uint64_t)(b->files[i].pace / 65536.0) * 7;
    }
    saved = g_files_saved;
    pictures = g_pictures_saved;
    lock_give(&g_lock);
    /* (2.7) a copy made on someone's device follows "Do that by itself after each conversion" as Hemma's own queue's do
       (owner, 2026-11-01: "How do we make sure that does not happen" - the original stayed beside it): it takes its
       original's place, with the same checks (as long as it, as compatible; never a .web.mp4) */
    for (int k = 0; k < njoin; k++) { /* (2.7) the joins: the video read from where it is now */
        const MediaFile *o = media_file(join_from[k]);
        if (!o || !media_full_path(o, joins[k]->src, sizeof joins[k]->src)) joins[k]->src[0] = '\0';
        if (app_testing() || !thread_spawn(join_worker, joins[k])) join_worker(joins[k]); /* (tests: at once) */
    }
    bool drop = nsettle && setting_int("media.drop_originals", 0) == 1;
    for (int k = 0; k < nsettle && drop; k++) {
        const MediaFile *o = media_file(settle[k].video);
        char src[1024];
        if (o && media_full_path(o, src, sizeof src)) convert_replace_original(o->id, src, settle[k].dst);
    }
    if (pictures > g_pictures_seen) media_scan_start(); /* a new picture: show it now, not in 20 s */
    g_pictures_seen = pictures;

    if (sig != g_shown) { /* progress moved: refresh the screens showing it */
        g_shown = sig;
        app_changed_where("/media");
    }
    if (active) app_wake_in(1000); /* keep the numbers moving even with no other reason to tick */
    if (finished_person) /* (the core says it in their language) */
        app_notify_person(finished_person, 0,
                          finished_files == 1 ? N_("Upload finished: %d file added") : N_("Upload finished: %d files added"),
                          finished_files);
    /* new files: rescan once things settle (and at least every few minutes during a long upload) */
    if (saved > g_done_seen) {
        g_rescan_at = now + 20000;
        if (!g_rescan_first) g_rescan_first = now;
    }
    g_done_seen = saved;
    if (g_rescan_at && (now >= g_rescan_at || now - g_rescan_first > 180000) && !media_scanning()) {
        g_rescan_at = g_rescan_first = 0;
        media_scan_start();
    }
}

void upload_stop(void) {
    atomic_store(&g_halt, true);
    for (int waited = 0; waited < 3000; waited += 50) { /* let workers close their files */
        lock_take(&g_lock);
        int n = g_workers;
        lock_give(&g_lock);
        if (!n) break;
        plat_sleep_ms(50);
    }
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && !g_batch[s]->workers) {
            batch_free(g_batch[s]);
            g_batch[s] = NULL;
        }
    g_files_saved = g_pictures_saved = 0;
    lock_give(&g_lock);
    pv_clear();
    g_rescan_at = g_rescan_first = 0;
    g_shown = 0;
    g_done_seen = g_pictures_seen = 0;
}
