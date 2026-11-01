/* modules/media/health.c — the library's part of the System page.
 *
 * Checks each library folder (there, readable, writable for uploads, room on its disk), the
 * converter (built in for the server, and for browsers?), conversions that didn't fit or
 * failed, and adds "Add your films and series" to the getting-started list.
 *
 * The hook only reads cached facts. They're measured when first asked for and then from the
 * tick, every 30 s while someone looks (the System page, an admin's home dashboard) - and
 * whether uploads could be saved, by creating and deleting a tiny file in each folder, at most
 * every 10 minutes. Nobody looking: no disk is touched. */
#include "health.h"
#include "../../core/assets.h"
#include "../../core/plat.h"
#include "convert.h"
#include "room.h"
#include "drives.h"
#include "media.h"

#define FOLDERS_EVERY_MS 30000
#define PROBE_EVERY_MS (10 * 60000)
#define WANTED_FOR_MS 120000

typedef struct {
    char path[300];
    bool exists, readable;
    int writable; /* 1 yes, 0 no, -1 not known yet */
    int64_t free, total;
    bool data_disk; /* the disk Hemma's data folder is on too */
} RootFacts;

static struct {
    int64_t at, probed_at, wanted_at; /* monotonic ms; 0 = never */
    int n;
    RootFacts r[MEDIA_MAX_ROOTS];
} HS;

/* Can Hemma create a file here? (What an upload or a converted copy needs; a read-only mount -
   systemd's ProtectSystem, ProtectHome - says no.) */
static int write_probe(const char *root) {
    char p[400];
    snprintf(p, sizeof p, "%s/.hemma-write-check", root);
    FILE *f = file_open(p, "wb");
    if (!f) return 0;
    bool ok = fputc('h', f) != EOF;
    if (fclose(f) != 0) ok = false;
    file_remove(p);
    return ok ? 1 : 0;
}

static void refresh(int64_t now, bool probe) {
    char roots[MEDIA_MAX_ROOTS][300];
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    RootFacts was[MEDIA_MAX_ROOTS];
    int nwas = HS.n;
    memcpy(was, HS.r, sizeof was);
    int64_t data_free = -1, data_total = -1;
    if (!media_disk_space(app_config()->data_dir, &data_free, &data_total)) data_total = -1;
    for (int i = 0; i < n; i++) {
        RootFacts *r = &HS.r[i];
        memset(r, 0, sizeof *r);
        str_copy(r->path, sizeof r->path, roots[i]);
        r->exists = path_is_dir(r->path);
        DirIter *it = r->exists ? dir_open(r->path) : NULL;
        r->readable = it != NULL;
        if (it) dir_close(it);
        r->free = r->total = -1;
        if (r->exists && !media_disk_space(r->path, &r->free, &r->total)) r->free = r->total = -1;
        /* (the same disk: the same size, the same free space give or take what was just written) */
        r->data_disk = r->total > 0 && r->total == data_total && llabs(r->free - data_free) < (64LL << 20);
        r->writable = -1;
        for (int k = 0; k < nwas; k++)
            if (str_eq(was[k].path, r->path)) r->writable = was[k].writable;
        if (!r->exists) r->writable = -1;
        else if (probe) r->writable = write_probe(r->path);
    }
    HS.n = n;
    HS.at = now ? now : 1;
    if (probe) HS.probed_at = HS.at;
}

void media_health_tick(int64_t now) {
    if (!HS.wanted_at || now - HS.wanted_at > WANTED_FOR_MS) return; /* nobody looked lately */
    if (HS.at && now - HS.at < FOLDERS_EVERY_MS) return;
    refresh(now, !HS.probed_at || now - HS.probed_at >= PROBE_EVERY_MS);
}

void media_health_reset(void) { HS.at = HS.probed_at = 0; }

/* The library's biggest video in that folder (0 = none yet). */
static int64_t biggest_video(const char *root) {
    const MediaIndex *ix = media_lib();
    int k = -1;
    for (int r = 0; r < ix->nroots && k < 0; r++)
        if (str_eq(ix->roots[r], root)) k = r;
    int64_t big = 0;
    for (int i = 0; k >= 0 && i < ix->nfiles; i++)
        if (ix->files[i].root == k && ix->files[i].type == MF_VIDEO) big = MAX(big, ix->files[i].size);
    return big;
}

static bool browser_converter(void) { /* av.wasm + its worker baked into this binary (the build needs zig) */
    bool wasm = false, worker = false;
    for (int i = 0; i < g_nassets; i++) {
        wasm |= g_assets[i].path && str_eq(g_assets[i].path, "/av.wasm");
        worker |= g_assets[i].path && str_eq(g_assets[i].path, "/conv.js");
    }
    return wasm && worker;
}

void media_health(Ctx *c, Health *h) {
    media_drives_health(h); /* (issue #74: a movie disk missing, a new one plugged in) */
    int64_t now = now_ms();
    HS.wanted_at = now ? now : 1;
    /* tests have no tick: they look at the folders (temporary ones) right away */
    if (!HS.at || app_testing()) refresh(now, app_testing());
    char d[320], t[240], sz[24];

    /* the library folders */
    if (!HS.n) {
        health_add(h, HEALTH_INFO, T("Films and series"), T("No library folders yet."),
                   T("Add the folders with your videos under Watch → Library settings."), MEDIA_SETTINGS_FOLDERS);
    } else {
        int trouble = 0;
        int64_t least = -1;
        for (int i = 0; i < HS.n; i++) {
            const RootFacts *r = &HS.r[i];
            if (!r->exists) {
                snprintf(d, sizeof d, T("Hemma can't find the library folder %s."), r->path);
                health_add(h, HEALTH_BAD, T("Library folder missing"), d,
                           T("Is the disk plugged in and mounted? Then rescan under Watch → Library settings."),
                           MEDIA_SETTINGS_FOLDERS);
                trouble++;
                continue;
            }
            if (!r->readable) {
                snprintf(d, sizeof d, T("Hemma isn't allowed to look inside %s."), r->path);
                health_add(h, HEALTH_BAD, T("Library folder locked"), d,
                           T("Let the user Hemma runs as read that folder (on a Pi: run the installer again with it)."),
                           MEDIA_SETTINGS_FOLDERS);
                trouble++;
                continue;
            }
            if (r->writable == 0) {
                snprintf(d, sizeof d, T("Hemma may only read %s: uploads and converted copies can't be saved there."),
                         r->path);
                health_add(h, HEALTH_INFO, T("Read-only library folder"), d,
                           T("Fine for watching. For uploads, run the installer again with this folder (docs/"
                             "raspberry-pi.md, step 7)."),
                           NULL);
            }
            if (r->free >= 0) {
                /* Room for new videos counts, not a share of the disk (issue #29): what uploading or
                   converting a video as big as the library's biggest takes - itself and the reserve
                   (room.c holds jobs to the same), 2 GB at least, 20 at most. The System page makes
                   one row per disk of it (issue #95: this check and its own "Disk space" said "nearly
                   full" and "OK" about the same disk). */
                int64_t big = biggest_video(r->path), want = CLAMP(big + MEDIA_RESERVE, 2LL << 30, 20LL << 30);
                char why[200], bg[24];
                media_size_text(big, bg, sizeof bg);
                if (big > 0) snprintf(why, sizeof why, T("to upload or convert a video as big as its biggest (%s)"), bg);
                else snprintf(why, sizeof why, "%s", T("for new videos"));
                HealthDisk disk = {.what = T("the library"), .path = r->path, .free = r->free, .total = r->total,
                                   .data_disk = r->data_disk, .want = want, .why = why,
                                   .todo = T("Delete or move some videos - or keep only the most compatible version of "
                                             "converted ones (Library settings)."),
                                   .link = MEDIA_SETTINGS_MANAGE};
                health_disk(h, &disk);
                if (least < 0 || r->free < least) least = r->free;
            }
        }
        if (!trouble) {
            const MediaIndex *ix = media_lib();
            int videos = ix->nmovies + ix->nothers + ix->nepisodes;
            char nf[48], nv[48];
            snprintf(nf, sizeof nf, TN(HS.n, "%d folder", "%d folders"), HS.n);
            snprintf(nv, sizeof nv, TN(videos, "%d video", "%d videos"), videos);
            int n = snprintf(d, sizeof d, media_scanning() ? T("%s, %s so far (looking through them now)") : "%s, %s", nf, nv);
            if (least >= 0 && n > 0 && (size_t)n < sizeof d) {
                media_size_text(least, sz, sizeof sz);
                char room[80];
                snprintf(room, sizeof room, HS.n > 1 ? T("%s free on the fullest disk") : T("%s free"), sz);
                snprintf(d + n, sizeof d - (size_t)n, "; %s.", room);
            } else if (n > 0 && (size_t)n + 1 < sizeof d) {
                snprintf(d + n, sizeof d - (size_t)n, ".");
            }
            health_add(h, HEALTH_OK, T("Films and series"), d, NULL, MEDIA_SETTINGS);
        }
    }

    /* watch history that found no video after the last scan (issue #93: it stayed behind without a word) */
    char labels[3][100];
    bool away = false;
    int lost = media_history_lost(labels, countof(labels), &away);
    if (lost && !away) {
        int k = snprintf(d, sizeof d,
                         TN(lost, "The history of %d video couldn't be matched to one in the library",
                            "The history of %d videos couldn't be matched to one in the library"),
                         lost);
        int shown = 0;
        for (int i = 0; i < lost && i < countof(labels) && k > 0 && (size_t)k < sizeof d; i++) {
            if (!labels[i][0]) continue;
            k += snprintf(d + k, sizeof d - (size_t)k, "%s%s", shown++ ? ", " : ": ", labels[i]);
        }
        if (k > 0 && (size_t)k < sizeof d) /* ("..." only after names: "library...." once) */
            snprintf(d + k, sizeof d - (size_t)k, "%s.", shown && lost > shown ? "..." : "");
        health_add(h, HEALTH_INFO, T("Watch history"), d,
                   T("Deleted or replaced on purpose? Library settings can forget it."), MEDIA_SETTINGS_MANAGE);
    }

    /* making videos play everywhere */
    int noroom = 0, failed = 0;
    convert_trouble(&noroom, &failed);
    if (noroom) {
        snprintf(d, sizeof d,
                 TN(noroom, "%d video didn't fit on the drive, so it wasn't converted.",
                    "%d videos didn't fit on the drive, so they weren't converted."),
                 noroom);
        health_add(h, HEALTH_WARN, T("Not enough room to convert"), d,
                   T("Make room on that drive, then convert again under Watch → Library settings."), MEDIA_SETTINGS_CONVERTING);
    }
    if (failed) {
        snprintf(d, sizeof d, TN(failed, "%d conversion failed.", "%d conversions failed."), failed);
        health_add(h, HEALTH_WARN, T("Conversions failed"), d, T("Library settings lists them, each with the reason."),
                   MEDIA_SETTINGS_CONVERTING);
    }
    bool here = browser_converter();
    snprintf(t, sizeof t, "%s",
             here ? T("Built in: videos that not every phone or TV plays can get a copy that plays everywhere. Uploads "
                      "convert on the sending device first.")
                  : T("Built in: videos that not every phone or TV plays can get a copy that plays everywhere. This "
                      "build has no converter for browsers, so uploads send videos as they are."));
    health_add(h, here ? HEALTH_OK : HEALTH_INFO, T("Play everywhere"), t, NULL, NULL);

    health_step(h, HS.n > 0, T("Add your films and series"),
                T("Point Hemma at the folders with your videos - or send them from a computer or phone."),
                MEDIA_SETTINGS_FOLDERS);
}
