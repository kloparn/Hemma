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

#define FOLDERS_EVERY_MS 30000
#define PROBE_EVERY_MS (10 * 60000)
#define WANTED_FOR_MS 120000

typedef struct {
    char path[300];
    bool exists, readable;
    int writable; /* 1 yes, 0 no, -1 not known yet */
    int64_t free, total;
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

static bool browser_converter(void) { /* av.wasm + its worker baked into this binary (the build needs zig) */
    bool wasm = false, worker = false;
    for (int i = 0; i < g_nassets; i++) {
        wasm |= g_assets[i].path && str_eq(g_assets[i].path, "/av.wasm");
        worker |= g_assets[i].path && str_eq(g_assets[i].path, "/conv.js");
    }
    return wasm && worker;
}

void media_health(Ctx *c, Health *h) {
    int64_t now = now_ms();
    HS.wanted_at = now ? now : 1;
    /* tests have no tick: they look at the folders (temporary ones) right away */
    if (!HS.at || app_testing()) refresh(now, app_testing());
    char d[320], t[240], sz[24];

    /* the library folders */
    if (!HS.n) {
        health_add(h, HEALTH_INFO, "Films and series", "No library folders yet.",
                   "Add the folders with your videos under Watch → Library settings.", "/media/settings");
    } else {
        int trouble = 0;
        int64_t least = -1;
        for (int i = 0; i < HS.n; i++) {
            const RootFacts *r = &HS.r[i];
            if (!r->exists) {
                snprintf(d, sizeof d, "Hemma can't find the library folder %s.", r->path);
                health_add(h, HEALTH_BAD, "Library folder missing", d,
                           "Is the disk plugged in and mounted? Then rescan under Watch → Library settings.",
                           "/media/settings");
                trouble++;
                continue;
            }
            if (!r->readable) {
                snprintf(d, sizeof d, "Hemma isn't allowed to look inside %s.", r->path);
                health_add(h, HEALTH_BAD, "Library folder locked", d,
                           "Let the user Hemma runs as read that folder (on a Pi: run the installer again with it).",
                           "/media/settings");
                trouble++;
                continue;
            }
            if (r->writable == 0) {
                snprintf(d, sizeof d, "Hemma may only read %s: uploads and converted copies can't be saved there.",
                         r->path);
                health_add(h, HEALTH_INFO, "Read-only library folder", d,
                           "Fine for watching. For uploads, run the installer again with this folder (docs/"
                           "raspberry-pi.md, step 7).",
                           NULL);
            }
            if (r->free >= 0) {
                int used = r->total > r->free ? (int)CLAMP((r->total - r->free) * 100 / r->total, 0, 100) : 0;
                if (r->free < (2LL << 30) || used >= 95) {
                    media_size_text(r->free, sz, sizeof sz);
                    snprintf(d, sizeof d, "The disk with %s is nearly full: %s free.", r->path, sz);
                    health_add(h, HEALTH_WARN, "Library disk nearly full", d,
                               "Delete or move some videos - or keep only the most compatible version of converted "
                               "ones (Library settings).",
                               "/media/settings");
                    trouble++;
                }
                if (least < 0 || r->free < least) least = r->free;
            }
        }
        if (!trouble) {
            const MediaIndex *ix = media_lib();
            int videos = ix->nmovies + ix->nothers + ix->nepisodes;
            int n = snprintf(d, sizeof d, "%d folder%s, %d video%s%s", HS.n, HS.n == 1 ? "" : "s", videos,
                             videos == 1 ? "" : "s", media_scanning() ? " so far (looking through them now)" : "");
            if (least >= 0 && n > 0 && (size_t)n < sizeof d) {
                media_size_text(least, sz, sizeof sz);
                snprintf(d + n, sizeof d - (size_t)n, "; %s free%s.", sz, HS.n > 1 ? " on the fullest disk" : "");
            } else if (n > 0 && (size_t)n + 1 < sizeof d) {
                snprintf(d + n, sizeof d - (size_t)n, ".");
            }
            health_add(h, HEALTH_OK, "Films and series", d, NULL, "/media/settings");
        }
    }

    /* making videos play everywhere */
    int noroom = 0, failed = 0;
    convert_trouble(&noroom, &failed);
    if (noroom) {
        snprintf(d, sizeof d, "%d video%s didn't fit on the drive, so %s converted.", noroom, noroom == 1 ? "" : "s",
                 noroom == 1 ? "it wasn't" : "they weren't");
        health_add(h, HEALTH_WARN, "Not enough room to convert", d,
                   "Make room on that drive, then convert again under Watch → Library settings.", "/media/settings");
    }
    if (failed) {
        snprintf(d, sizeof d, "%d conversion%s failed.", failed, failed == 1 ? "" : "s");
        health_add(h, HEALTH_WARN, "Conversions failed", d, "Library settings lists them, each with the reason.",
                   "/media/settings");
    }
    bool here = browser_converter();
    snprintf(t, sizeof t, "Built in: videos that not every phone or TV plays can get a copy that plays everywhere. %s",
             here ? "Uploads convert on the sending device first."
                  : "This build has no converter for browsers, so uploads send videos as they are.");
    health_add(h, here ? HEALTH_OK : HEALTH_INFO, "Play everywhere", t, NULL, NULL);

    health_step(h, HS.n > 0, "Add your films and series",
                "Point Hemma at the folders with your videos - or send them from a computer or phone.",
                "/media/settings");
}
