/* modules/media/drives.c — movie disks plugged into the machine (issue #74; see drives.h).
 *
 * The looking is Hemma's own and needs no rights: sysfs says which disks there are and how big, udev's
 * database what's on them (file system, UUID, label, model, USB), mountinfo where they're mounted. The
 * doing is root's: a request line for deploy/hemma-disks, which trusts none of it and looks again.
 * (Issue #46: the helper's own words in its result are English data - shown as they came, with
 * everything around them in the viewer's language.) */
#include "drives.h"
#include "../../core/plat.h"
#include "../../core/ui.h"
#include "media.h"

#define ANSWERS "/var/lib/hemma-disks"
#define SET_ASKED "media.drive_asked" /* the last request: "<unix> <person> <what>" */
#define SET_DONE "media.drive_done"   /* the result whose library folder was added/removed: its unix */
#define SET_SEEN "media.drive_seen"   /* the result an admin put away ("Got it"): its unix */
#define SCAN_MS 5000
#define SILENT_S 900       /* asked this long ago and no answer: the helper isn't there */
#define REMOUNT_S 300      /* one of Hemma's disks back but not mounted: asked again at most this often */
#define MINE_MAX 16

typedef struct {
    char uuid[64], path[300], fstype[24], label[80];
} Mine;

static Drive g_drives[DRIVES_MAX];
static int g_ndrives;
static uint64_t g_sig;
static int64_t g_scan_at, g_read_at; /* monotonic ms */
static char g_sys[300], g_udev[300], g_minfo[300];
static bool g_test;
static char g_ask[700], g_ans[700];
static bool g_present;
static Buf g_result_raw, g_list_raw;
static int64_t g_result_mt, g_list_mt;
static char g_state[16], g_res_uuid[64], g_res_path[300], g_res_words[320];
static int64_t g_res_at;
static Mine g_mine[MINE_MAX];
static int g_nmine;
static int64_t g_asked_at;
static char g_asked[120];
static struct {
    char uuid[64];
    int64_t at;
} g_remount[MINE_MAX];

/* ---------------------------------------------------------------- looking */

static bool skip_dev(const char *n) {
    static const char *const no[] = {"loop", "ram", "zram", "md", "dm-", "sr", "fd", "nbd", "mtdblock"};
    for (size_t i = 0; i < countof(no); i++)
        if (str_starts(n, no[i])) return true;
    return n[0] == '.';
}

static bool read_small(const char *path, char *out, size_t cap) {
    Buf b = {0};
    bool ok = file_read_all(path, &b, 8192);
    str_copy(out, cap, ok ? buf_cstr(&b) : "");
    str_trim(out);
    buf_free(&b);
    return ok;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A udev "E:KEY=value" line's value ("" when absent); *_ENC values have \xNN escapes - decoded. */
static void udev_get(const char *text, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        if (p[0] == 'E' && p[1] == ':' && strncmp(p + 2, key, kl) == 0 && p[2 + kl] == '=') {
            const char *v = p + 3 + kl;
            size_t o = 0;
            for (; *v && *v != '\n' && o + 1 < cap; v++) {
                int h1, h2;
                if (v[0] == '\\' && v[1] == 'x' && (h1 = hexval(v[2])) >= 0 && (h2 = hexval(v[3])) >= 0) {
                    out[o++] = (char)(h1 * 16 + h2);
                    v += 3;
                } else out[o++] = *v;
            }
            out[o] = '\0';
            str_trim(out);
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

static bool system_place(const char *mp) {
    static const char *const sys[] = {"/", "/boot", "/boot/firmware", "/boot/efi", "/usr", "/var", "/home"};
    for (size_t i = 0; i < countof(sys); i++)
        if (str_eq(mp, sys[i])) return true;
    return false;
}

/* Where dev ("8:1") is mounted - the mount of its own top first - and whether it holds the system. */
static void mount_of(const char *text, const char *dev, char *out, size_t cap, bool *sys) {
    out[0] = '\0';
    char other[300] = "";
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        char row[1200];
        str_ncopy(row, sizeof row, line, end ? (size_t)(end - line) : strlen(line));
        line = end ? end + 1 : NULL;
        char id[16], parent[16], majmin[32], root[300], mp[300];
        if (sscanf(row, "%15s %15s %31s %299s %299s", id, parent, majmin, root, mp) != 5 || !str_eq(majmin, dev))
            continue;
        char path[300]; /* "\040" = a space, as the kernel writes it */
        size_t o = 0;
        for (const char *p = mp; *p && o + 1 < sizeof path; p++) {
            if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
                path[o++] = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
                p += 3;
            } else path[o++] = *p;
        }
        path[o] = '\0';
        if (system_place(path)) *sys = true;
        if (str_eq(root, "/") && !out[0]) str_copy(out, cap, path);
        else if (!other[0]) str_copy(other, sizeof other, path);
    }
    if (!out[0]) str_copy(out, cap, other);
}

static void add_vol(Drive *d, const char *kpath, const char *kname, const char *udev, const char *minfo, bool whole) {
    if (d->nvol == DRIVE_VOLS) return;
    char p[800], dev[32];
    snprintf(p, sizeof p, "%s/dev", kpath);
    if (!read_small(p, dev, sizeof dev) || !dev[0]) return;
    Buf u = {0};
    snprintf(p, sizeof p, "%s/b%s", udev, dev);
    file_read_all(p, &u, 65536);
    DriveVol v = {0};
    str_copy(v.kname, sizeof v.kname, kname);
    udev_get(buf_cstr(&u), "ID_FS_TYPE", v.fstype, sizeof v.fstype);
    udev_get(buf_cstr(&u), "ID_FS_UUID", v.uuid, sizeof v.uuid);
    udev_get(buf_cstr(&u), "ID_FS_LABEL_ENC", v.label, sizeof v.label);
    if (!v.label[0]) udev_get(buf_cstr(&u), "ID_FS_LABEL", v.label, sizeof v.label);
    buf_free(&u);
    bool sysmount = false;
    mount_of(minfo, dev, v.mounted, sizeof v.mounted, &sysmount);
    if (sysmount || str_eq(v.fstype, "swap")) d->system = true;
    if (whole && !v.fstype[0] && !v.mounted[0]) return; /* (a disk with a partition table: its partitions say) */
    d->vol[d->nvol++] = v;
}

static int knamecmp(const char *a, const char *b) { /* sda2 before sda10 */
    size_t la = strlen(a), lb = strlen(b);
    return la != lb ? (la < lb ? -1 : 1) : strcmp(a, b);
}

int media_drives_scan(const char *sys, const char *udev, const char *minfo, Drive *out, int max) {
    DirIter *it = dir_open(sys);
    if (!it) return 0;
    int n = 0;
    DirEntry e;
    while (n < max && dir_next(it, &e)) {
        if (skip_dev(e.name)) continue;
        char kpath[600], p[800], v[64], dev[32];
        snprintf(kpath, sizeof kpath, "%.400s/%.64s", sys, e.name); /* (device names are short) */
        snprintf(p, sizeof p, "%s/dev", kpath);
        if (!read_small(p, dev, sizeof dev) || !dev[0]) continue;
        snprintf(p, sizeof p, "%s/size", kpath);
        read_small(p, v, sizeof v);
        long long sectors = str_int_or(v, 0);
        if (sectors <= 0) continue; /* (an empty card reader) */
        Drive *d = &out[n];
        memset(d, 0, sizeof *d);
        str_copy(d->kname, sizeof d->kname, e.name);
        d->bytes = sectors * 512;
        Buf u = {0};
        snprintf(p, sizeof p, "%s/b%s", udev, dev);
        file_read_all(p, &u, 65536);
        char t[100];
        udev_get(buf_cstr(&u), "ID_BUS", t, sizeof t);
        d->usb = str_eq(t, "usb");
        udev_get(buf_cstr(&u), "ID_USB_DRIVER", t, sizeof t);
        d->usb |= t[0] != '\0'; /* (a USB-SATA bridge says ID_BUS=ata, but it's on USB) */
        udev_get(buf_cstr(&u), "ID_MODEL_ENC", d->name, sizeof d->name);
        if (!d->name[0]) {
            udev_get(buf_cstr(&u), "ID_MODEL", d->name, sizeof d->name);
            for (char *c = d->name; *c; c++)
                if (*c == '_') *c = ' ';
        }
        buf_free(&u);
        if (!d->name[0]) {
            snprintf(p, sizeof p, "%s/device/model", kpath);
            read_small(p, d->name, sizeof d->name);
        }
        if (!d->name[0]) str_copy(d->name, sizeof d->name, d->usb ? N_("USB disk") : N_("Disk")); /* (drive_name) */
        add_vol(d, kpath, e.name, udev, minfo, true);
        DirIter *pi = dir_open(kpath);
        DirEntry pe;
        while (pi && dir_next(pi, &pe)) {
            if (!str_starts(pe.name, e.name) || str_eq(pe.name, e.name)) continue;
            char pp[900], part[16];
            snprintf(pp, sizeof pp, "%.600s/%.64s/partition", kpath, pe.name);
            if (!read_small(pp, part, sizeof part)) continue;
            snprintf(pp, sizeof pp, "%.600s/%.64s", kpath, pe.name);
            add_vol(d, pp, pe.name, udev, minfo, false);
        }
        if (pi) dir_close(pi);
        for (int a = 1; a < d->nvol; a++) /* (the directory's order is anyone's) */
            for (int b = a; b > 0 && knamecmp(d->vol[b - 1].kname, d->vol[b].kname) > 0; b--) {
                DriveVol x = d->vol[b];
                d->vol[b] = d->vol[b - 1];
                d->vol[b - 1] = x;
            }
        n++;
    }
    dir_close(it);
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && knamecmp(out[b - 1].kname, out[b].kname) > 0; b--) {
            Drive x = out[b];
            out[b] = out[b - 1];
            out[b - 1] = x;
        }
    return n;
}

bool media_drive_fs_ok(const char *fs) {
    static const char *const ok[] = {"ext4", "ext3", "ext2", "xfs", "btrfs", "exfat", "vfat", "ntfs"};
    for (size_t i = 0; i < countof(ok); i++)
        if (str_eq(fs, ok[i])) return true;
    return false;
}

void media_drive_size(int64_t n, char *out, size_t cap) { /* decimal, as the box says: "2.0 TB", not "1.8" */
    if (n >= 1000000000000LL) snprintf(out, cap, "%.1f TB", (double)n / 1e12);
    else if (n >= 1000000000LL) snprintf(out, cap, "%.0f GB", (double)n / 1e9);
    else snprintf(out, cap, "%.0f MB", (double)n / 1e6);
    char *dot = strchr(out, '.');
    if (dot) *dot = i18n_decimal(); /* ("2,0 TB" in Swedish and Spanish) */
}

/* in the language in use (what isn't a word stays as it is) */
static const char *fs_name(const char *fs) {
    static const char *const names[][2] = {
        {"ntfs", "NTFS"},
        {"exfat", "exFAT"},
        {"vfat", "FAT"},
        {"apfs", N_("APFS (a Mac's)")},
        {"hfsplus", N_("HFS+ (a Mac's)")},
        {"crypto_LUKS", N_("encrypted")},
        {"linux_raid_member", N_("part of a RAID")},
        {"LVM2_member", "LVM"},
        {"zfs_member", "ZFS"},
    };
    for (size_t i = 0; i < countof(names); i++)
        if (str_eq(fs, names[i][0])) return T(names[i][1]);
    return fs;
}

/* A disk's name in the language in use: its model is data, the words the scan gave a disk without one aren't. */
static const char *drive_name(const Drive *d) {
    if (str_eq(d->name, "USB disk")) return T("USB disk");
    if (str_eq(d->name, "Disk")) return T("Disk");
    return d->name;
}

/* --------------------------------------------------------- what's on offer */

static const Mine *mine_by_uuid(const char *uuid) {
    for (int i = 0; i < g_nmine; i++)
        if (uuid[0] && str_eq(g_mine[i].uuid, uuid)) return &g_mine[i];
    return NULL;
}

static const DriveVol *vol_by_uuid(const char *uuid, const Drive **d) {
    for (int i = 0; i < g_ndrives; i++)
        for (int k = 0; k < g_drives[i].nvol; k++)
            if (uuid[0] && str_eq(g_drives[i].vol[k].uuid, uuid)) {
                if (d) *d = &g_drives[i];
                return &g_drives[i].vol[k];
            }
    return NULL;
}

static bool offered(const Drive *d) { return d->usb && !d->system; }

static bool in_use(const Drive *d) {
    for (int k = 0; k < d->nvol; k++)
        if (d->vol[k].mounted[0]) return true;
    return false;
}

static bool blank(const Drive *d) { /* nothing anyone could lose: no file system anywhere on it */
    for (int k = 0; k < d->nvol; k++)
        if (d->vol[k].fstype[0]) return false;
    return true;
}

/* a volume to use: a file system Hemma can keep movies on, with a UUID, not mounted, not Hemma's yet */
static bool usable(const DriveVol *v) {
    return media_drive_fs_ok(v->fstype) && v->uuid[0] && !v->mounted[0] && !mine_by_uuid(v->uuid);
}

static int count_new(void) {
    int n = 0;
    for (int i = 0; i < g_ndrives; i++) {
        const Drive *d = &g_drives[i];
        if (!offered(d) || in_use(d)) continue;
        bool any = blank(d);
        for (int k = 0; k < d->nvol && !any; k++) any = usable(&d->vol[k]);
        n += any;
    }
    return n;
}

static void vol_title(const Drive *d, const DriveVol *v, char *out, size_t cap) {
    str_copy(out, cap, v && v->label[0] ? v->label : drive_name(d));
}

/* ---------------------------------------------------------------- the helper */

static void path_in(char *out, size_t cap, const char *dir, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

static bool present(void) { return path_is_dir(g_ask) && path_is_dir(g_ans); }

static bool read_answer(const char *name, Buf *raw, int64_t *mt) {
    char path[760];
    path_in(path, sizeof path, g_ans, name);
    int64_t m = 0;
    Buf now = {0};
    if (!path_stat(path, NULL, &m, NULL) || !file_read_all(path, &now, 16384)) m = 0, buf_reset(&now);
    bool changed = m != *mt || now.len != raw->len || (now.len && memcmp(now.p, raw->p, now.len) != 0);
    if (changed) {
        buf_reset(raw);
        if (now.len) buf_put(raw, now.p, now.len);
        *mt = m;
    }
    buf_free(&now);
    return changed;
}

/* "<state> <unix> <uuid|-> <path|-> <words>" */
static void parse_result(const char *s) {
    g_state[0] = g_res_uuid[0] = g_res_path[0] = g_res_words[0] = '\0';
    g_res_at = 0;
    static const char *const known[] = {"working", "used", "forgotten", "refused", "failed"};
    char state[16];
    str_ncopy(state, sizeof state, s, strcspn(s, " \r\n"));
    bool ok = false;
    for (size_t i = 0; i < countof(known); i++) ok |= str_eq(state, known[i]);
    const char *p = strchr(s, ' ');
    if (!ok || !p) return;
    g_res_at = atoll(++p);
    char *fields[2] = {g_res_uuid, g_res_path};
    size_t caps[2] = {sizeof g_res_uuid, sizeof g_res_path};
    for (int i = 0; i < 2; i++) {
        p = strchr(p, ' ');
        if (!p) return;
        p++;
        str_ncopy(fields[i], caps[i], p, strcspn(p, " \r\n"));
        if (str_eq(fields[i], "-")) fields[i][0] = '\0';
    }
    p = strchr(p, ' ');
    if (p) str_copy(g_res_words, sizeof g_res_words, p + 1);
    str_trim(g_res_words);
    str_copy(g_state, sizeof g_state, state);
}

/* "<uuid> <path> <fstype> <label>" a line */
static void parse_mine(const char *s) {
    g_nmine = 0;
    for (const char *line = s; line && *line && g_nmine < MINE_MAX;) {
        const char *end = strchr(line, '\n');
        char row[600];
        str_ncopy(row, sizeof row, line, end ? (size_t)(end - line) : strlen(line));
        line = end ? end + 1 : NULL;
        Mine m = {0};
        int at = 0;
        if (sscanf(row, "%63s %299s %23s %n", m.uuid, m.path, m.fstype, &at) < 3 || !strchr(m.path, '/')) continue;
        str_copy(m.label, sizeof m.label, row + at);
        str_trim(m.label);
        g_mine[g_nmine++] = m;
    }
}

static void read_asked(void) {
    const char *s = setting(SET_ASKED, "");
    g_asked_at = atoll(s);
    const char *p = strchr(s, ' ');
    p = p ? strchr(p + 1, ' ') : NULL;
    str_copy(g_asked, sizeof g_asked, p ? p + 1 : "");
}

static int asked_by(void) {
    const char *p = strchr(setting(SET_ASKED, ""), ' ');
    return p ? atoi(p + 1) : 0;
}

static bool ask(int person, const char *what) {
    char nonce[17], line[200], path[760], set[200];
    rand_hex(nonce, 8);
    snprintf(line, sizeof line, "%s %s\n", what, nonce);
    path_in(path, sizeof path, g_ask, "request");
    if (!file_write_atomic(path, line, strlen(line))) {
        log_warn("disks: couldn't write %s", path);
        return false;
    }
    g_asked_at = now_sec();
    str_copy(g_asked, sizeof g_asked, what);
    snprintf(set, sizeof set, "%lld %d %s", (long long)g_asked_at, person, what);
    setting_set(SET_ASKED, set);
    return true;
}

static bool answered(void) { return !g_asked_at || g_result_mt >= g_asked_at; }

static bool working(int64_t now) {
    if (!answered() && now - g_asked_at < SILENT_S) return true;
    return str_eq(g_state, "working") && now - g_res_at < SILENT_S;
}

static bool silent(int64_t now) {
    return !answered() && now - g_asked_at >= SILENT_S && now - g_asked_at < 7 * 86400;
}

/* What the helper did, once: a disk in the library (or out of it), for whoever asked. */
static void settle_result(void) {
    if (!g_res_at || g_res_at <= setting_int(SET_DONE, 0)) return;
    if (!str_eq(g_state, "used") && !str_eq(g_state, "forgotten")) return;
    char at[24];
    snprintf(at, sizeof at, "%lld", (long long)g_res_at);
    setting_set(SET_DONE, at);
    if (!g_res_path[0]) return;
    const Mine *m = mine_by_uuid(g_res_uuid);
    const char *label = m && m->label[0] ? m->label : g_res_path;
    int who = asked_by();
    const Person *p = who ? person_get(who) : NULL;
    if (str_eq(g_state, "used")) {
        if (media_root_add(g_res_path)) {
            log_info("disks: the movie disk \"%s\" (%s) is a library folder now", label, g_res_path);
            if (p) activity_add(who, "💽", "/media/settings", N_("%s added the movie disk \"%s\""), p->name, label);
            else activity_add(who, "💽", "/media/settings", N_("Someone added the movie disk \"%s\""), label);
        }
    } else if (media_root_drop(g_res_path)) {
        log_info("disks: the movie disk at %s was forgotten - its library folder went with it", g_res_path);
    }
}

static void read_answers(void) {
    bool changed = false;
    if (read_answer("disks", &g_list_raw, &g_list_mt)) {
        parse_mine(buf_cstr(&g_list_raw));
        changed = true;
    }
    if (read_answer("result", &g_result_raw, &g_result_mt)) {
        parse_result(buf_cstr(&g_result_raw));
        changed = true;
    }
    settle_result();
    if (changed) app_changed_where("/media");
}

/* One of Hemma's disks is plugged in again but not mounted (the Pi doesn't mount it by itself after
   start): ask the helper, which only mounts what it set up. */
static void remount(int64_t now) {
    if (working(now) || !g_present) return;
    for (int i = 0; i < g_nmine; i++) {
        const DriveVol *v = vol_by_uuid(g_mine[i].uuid, NULL);
        if (!v || v->mounted[0]) continue;
        int slot = -1;
        for (int k = 0; k < MINE_MAX && slot < 0; k++)
            if (str_eq(g_remount[k].uuid, g_mine[i].uuid)) slot = k;
        if (slot < 0)
            for (int k = 0; k < MINE_MAX && slot < 0; k++)
                if (!g_remount[k].uuid[0]) slot = k;
        if (slot < 0) slot = 0;
        if (str_eq(g_remount[slot].uuid, g_mine[i].uuid) && now - g_remount[slot].at < REMOUNT_S) continue;
        str_copy(g_remount[slot].uuid, sizeof g_remount[slot].uuid, g_mine[i].uuid);
        g_remount[slot].at = now;
        char what[100];
        snprintf(what, sizeof what, "use %.64s", g_mine[i].uuid);
        if (ask(0, what)) log_info("disks: the movie disk \"%s\" is back - asking for it to be mounted", g_mine[i].label);
        return; /* (one at a time) */
    }
}

/* --------------------------------------------------------------- lifecycle */

void media_drives_start(void) {
    if (!g_test) {
        str_copy(g_sys, sizeof g_sys, "/sys/block");
        str_copy(g_udev, sizeof g_udev, "/run/udev/data");
        str_copy(g_minfo, sizeof g_minfo, "/proc/self/mountinfo");
        snprintf(g_ask, sizeof g_ask, "%s/disks", app_config()->data_dir ? app_config()->data_dir : "data");
        str_copy(g_ans, sizeof g_ans, ANSWERS);
    }
    g_ndrives = 0;
    g_sig = 0;
    g_scan_at = g_read_at = 0;
    buf_reset(&g_result_raw);
    buf_reset(&g_list_raw);
    g_result_mt = g_list_mt = g_res_at = 0;
    g_state[0] = '\0';
    g_nmine = 0;
    memset(g_remount, 0, sizeof g_remount);
    read_asked();
    g_present = present();
    if (g_present) read_answers();
}

void media_drives_stop(void) {
    buf_free(&g_result_raw);
    buf_free(&g_list_raw);
    g_test = false; /* (the next boot looks at the real places unless its test says otherwise) */
}

static void scan(void) {
    Buf mi = {0};
    file_read_all(g_minfo, &mi, 1 << 20);
    Drive *found = xcalloc(DRIVES_MAX, sizeof *found);
    int n = media_drives_scan(g_sys, g_udev, buf_cstr(&mi), found, DRIVES_MAX);
    buf_free(&mi);
    uint64_t sig = hash64(found, (size_t)n * sizeof *found);
    memcpy(g_drives, found, (size_t)n * sizeof *found);
    g_ndrives = n;
    free(found);
    if (sig != g_sig) {
        if (g_sig) log_info("disks: %d disk%s seen (%d new to offer)", n, n == 1 ? "" : "s", count_new());
        Buf say = {0}; /* (detailed logging: what each disk is taken for) */
        for (int i = 0; i < n; i++) {
            const Drive *d = &g_drives[i];
            char size[24];
            media_drive_size(d->bytes, size, sizeof size);
            buf_printf(&say, "%s%s %s, %s%s", i ? "; " : "", d->kname, d->name, size,
                       d->system ? ", the system's" : !d->usb ? ", not USB" : "");
            for (int k = 0; k < d->nvol; k++)
                buf_printf(&say, " [%s %s%s%s]", d->vol[k].kname, d->vol[k].fstype[0] ? d->vol[k].fstype : "-",
                           d->vol[k].mounted[0] ? " at " : "", d->vol[k].mounted);
        }
        log_debug("disks: %s", n ? buf_cstr(&say) : "none");
        buf_free(&say);
        g_sig = sig;
        app_changed_where("/media");
        app_changed_where("/system");
    }
}

void media_drives_tick(int64_t now_ms_) {
    if (app_testing() && !g_test) return; /* (a test looks at a made-up machine, never this one) */
    if (!g_scan_at || now_ms_ - g_scan_at >= SCAN_MS) {
        g_scan_at = now_ms_ ? now_ms_ : 1;
        g_present = present();
        scan();
        if (g_present) remount(now_sec());
    }
    if (g_present && (g_test || !g_read_at || now_ms_ - g_read_at >= 2000)) {
        g_read_at = now_ms_ ? now_ms_ : 1;
        read_answers();
    }
}

/* ---------------------------------------------------------------- the page */

static void result_notice(Buf *b, int64_t now) {
    if (!g_state[0] || str_eq(g_state, "working") || g_res_at <= setting_int(SET_SEEN, 0)) return;
    (void)now;
    const Mine *m = mine_by_uuid(g_res_uuid);
    bool bad = !str_eq(g_state, "used") && !str_eq(g_state, "forgotten");
    H(b, "<div class=\"notice media-drive-note%r\" id=\"media-drive-result\"><p>", bad ? " media-drive-bad" : "");
    if (str_eq(g_state, "used") && m && m->label[0])
        H(b,
          T("<b>%s is ready.</b> It's mounted at <code class=\"media-code\">%s</code> and in the library; Hemma may "
            "save uploads and copies on it."),
          m->label, g_res_path);
    else if (str_eq(g_state, "used"))
        H(b,
          T("<b>The disk is ready.</b> It's mounted at <code class=\"media-code\">%s</code> and in the library; Hemma "
            "may save uploads and copies on it."),
          g_res_path);
    else if (str_eq(g_state, "forgotten"))
        H(b,
          T("<b>Forgotten.</b> The Pi won't mount <code class=\"media-code\">%s</code> at start any more, and it's out "
            "of the library. Its files are untouched."),
          g_res_path);
    else if (g_res_words[0]) /* (the helper's words: English, as it wrote them) */
        H(b, T("<b>Not done:</b> %s. Nothing on the disk was changed."), g_res_words);
    else buf_puts(b, T("<b>Not done:</b> the disk helper said no. Nothing on the disk was changed."));
    buf_puts(b, "</p>");
    ui_act(b, "media.drive_seen", "btn sm", T("Got it"), NULL, NULL, NULL);
    buf_puts(b, "</div>");
}

static void use_form(Buf *b, const DriveVol *v, bool big) {
    ui_form(b, "media.drive_use", "inline");
    ui_hidden(b, "uuid", v->uuid);
    H(b, "<button class=\"btn%r\">%s</button>", big ? " primary" : " sm", T("Use it for movies"));
    ui_form_end(b);
}

static void new_rows(Buf *b) {
    for (int i = 0; i < g_ndrives; i++) {
        const Drive *d = &g_drives[i];
        if (!offered(d) || in_use(d)) continue;
        char size[24], title[100];
        media_drive_size(d->bytes, size, sizeof size);
        if (blank(d)) {
            H(b, "<li class=\"row media-drive\" id=\"media-drive-%s\"><div class=\"row-main\"><b>%s</b><small "
                 "class=\"muted\">%s · %s</small></div>",
              d->kname, drive_name(d), size, T("empty: nothing on it Hemma can read"));
            if (g_present)
                H(b, "<a class=\"btn sm\" href=\"/media/settings?format=%u\">%s</a>", d->kname, T("Format for Hemma"));
            buf_puts(b, "</li>");
            continue;
        }
        for (int k = 0; k < d->nvol; k++) {
            const DriveVol *v = &d->vol[k];
            if (!v->fstype[0] || mine_by_uuid(v->uuid)) continue;
            vol_title(d, v, title, sizeof title);
            if (!media_drive_fs_ok(v->fstype)) {
                H(b, "<li class=\"row media-drive\" id=\"media-drive-%s\"><div class=\"row-main\"><b>%s</b><small "
                     "class=\"muted\">%s · %s - %r</small></div></li>",
                  v->kname, title, size, fs_name(v->fstype),
                  T("Hemma can't keep movies on that (ext4, exFAT, NTFS and FAT disks work)"));
                continue;
            }
            const char *name = drive_name(d);
            H(b, "<li class=\"row media-drive\" id=\"media-drive-%s\"><div class=\"row-main\"><b>", v->kname);
            H(b, T("New disk found: %s"), title);
            H(b, "</b><small class=\"muted\">%s · %s%s%s</small></div>", fs_name(v->fstype), size,
              str_eq(title, name) ? "" : " · ", str_eq(title, name) ? "" : name);
            if (g_present && usable(v)) use_form(b, v, false);
            buf_puts(b, "</li>");
        }
    }
}

static void mine_rows(Buf *b) {
    for (int i = 0; i < g_nmine; i++) {
        const Mine *m = &g_mine[i];
        const DriveVol *v = vol_by_uuid(m->uuid, NULL);
        char state[200];
        if (v && v->mounted[0]) {
            int64_t fr = -1, tot = -1;
            char a[24], t[24];
            path_disk_space(v->mounted, &fr, &tot);
            media_drive_size(fr > 0 ? fr : 0, a, sizeof a);
            media_drive_size(tot > 0 ? tot : 0, t, sizeof t);
            snprintf(state, sizeof state, T("%s free of %s"), a, t);
        } else if (v) str_copy(state, sizeof state, T("plugged in - mounting it…"));
        else str_copy(state, sizeof state, T("missing: plug it back in"));
        H(b, "<li class=\"row media-drive%r\" id=\"media-mine-%s\"><div class=\"row-main\"><b>%s</b><small "
             "class=\"muted\"><code class=\"media-code\">%s</code> · %s · %s</small></div>",
          v && v->mounted[0] ? "" : " media-drive-gone", m->uuid, m->label[0] ? m->label : m->path, m->path,
          fs_name(m->fstype), state);
        char q[400];
        snprintf(q, sizeof q,
                 T("Forget the disk \"%s\"? The Pi stops mounting it at start and it leaves the library. The files on "
                   "it stay as they are."),
                 m->label[0] ? m->label : m->path);
        ui_form_confirm(b, "media.drive_forget", "inline", q);
        ui_hidden(b, "uuid", m->uuid);
        H(b, "<button class=\"btn sm ghost\">%s</button>", T("Forget"));
        ui_form_end(b);
        buf_puts(b, "</li>");
    }
}

void media_drives_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    int64_t now = now_sec();
    int fresh = count_new();
    bool notice = g_state[0] && !str_eq(g_state, "working") && g_res_at > setting_int(SET_SEEN, 0);
    if (!fresh && !g_nmine && !notice && !working(now) && !silent(now)) {
        bool any = false; /* unusable disks still say why */
        for (int i = 0; i < g_ndrives && !any; i++) any = offered(&g_drives[i]) && !in_use(&g_drives[i]);
        if (!any) return;
    }
    ui_card(b, T("Disks"), NULL, NULL);
    if (g_present) result_notice(b, now);
    if (g_present && working(now))
        H(b, "<p class=\"notice media-drive-note\" id=\"media-drive-busy\" role=\"status\">%r</p>",
          T("Setting up the disk… Hemma restarts once to reach it; this page comes back on its own."));
    else if (g_present && silent(now))
        H(b, "<p class=\"notice media-drive-note media-drive-bad\" id=\"media-drive-busy\">%r</p>",
          T("The disk helper didn't answer. On the Pi: <code>sudo systemctl enable --now hemma-disks.path</code>."));
    buf_puts(b, "<ul class=\"rows media-drives\">");
    mine_rows(b);
    new_rows(b);
    buf_puts(b, "</ul>");
    if (fresh && !g_present)
        H(b, "<p class=\"muted media-small\">%r</p>",
          T("To use a disk from here, Hemma needs its disk helper: install Hemma with its installer (0.22.0 or newer, "
            "which brings it), or set the disk up by hand (docs/raspberry-pi.md, step 7)."));
    else if (fresh)
        H(b, "<p class=\"muted media-small\">%r</p>",
          T("Use it for movies: the Pi mounts it now and at every start (a missing disk never stops it starting), "
            "Hemma may save uploads and copies on it, and it becomes a library folder. Nothing on it is changed."));
    ui_card_end(b);
}

void media_drives_dialog(Ctx *c, Buf *b) {
    const char *k = ctx_q(c, "format");
    if (!k[0] || !is_admin(c) || !g_present) return;
    const Drive *d = NULL;
    for (int i = 0; i < g_ndrives && !d; i++)
        if (str_eq(g_drives[i].kname, k) && offered(&g_drives[i]) && !in_use(&g_drives[i]) && blank(&g_drives[i]))
            d = &g_drives[i];
    if (!d) return;
    char size[24];
    media_drive_size(d->bytes, size, sizeof size);
    ui_modal(b, T("Format for Hemma"), "/media/settings");
    buf_puts(b, "<p>");
    H(b,
      T("<b>%s</b> (%s) gets a new, empty file system for movies (ext4). Hemma sees nothing on it now - but if it ever "
        "held something, that's gone for good."),
      drive_name(d), size);
    buf_puts(b, "</p>");
    ui_form(b, "media.drive_format", "stack");
    ui_hidden(b, "disk", d->kname);
    char bytes[32];
    snprintf(bytes, sizeof bytes, "%lld", (long long)d->bytes);
    ui_hidden(b, "bytes", bytes);
    H(b, /* (the word typed stays ERASE in every language: media_drives_act_format asks for it) */
      "<label class=\"field\"><span>%s</span><input name=\"confirm\" autocomplete=\"off\" autocapitalize=\"characters\" "
      "spellcheck=\"false\" required></label><div class=\"modal-f\"><a class=\"btn\" href=\"/media/settings\">%s</a>"
      "<button class=\"btn danger\">%s</button></div>",
      T("Type ERASE to format it"), T("Keep it as it is"), T("Format and use it"));
    ui_form_end(b);
    ui_modal_end(b);
}

bool media_drives_offer(Ctx *c, Buf *b) {
    if (!is_admin(c) || !g_present) return false;
    for (int i = 0; i < g_ndrives; i++) {
        const Drive *d = &g_drives[i];
        if (!offered(d) || in_use(d)) continue;
        for (int k = 0; k < d->nvol; k++) {
            if (!usable(&d->vol[k])) continue;
            char size[24], title[100];
            media_drive_size(d->bytes, size, sizeof size);
            vol_title(d, &d->vol[k], title, sizeof title);
            buf_puts(b, "<div class=\"media-center media-suggest-big\" id=\"media-drive-offer\"><p>");
            H(b, T("<b>New disk found:</b> %s (%s, %s)"), title, fs_name(d->vol[k].fstype), size);
            buf_puts(b, "</p>");
            use_form(b, &d->vol[k], true);
            buf_puts(b, "</div>");
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- checks */

void media_drives_health(Health *h) {
    char d[400], todo[200], size[24], title[100];
    int64_t now = now_sec();
    for (int i = 0; i < g_nmine; i++) {
        const DriveVol *v = vol_by_uuid(g_mine[i].uuid, NULL);
        if (v && v->mounted[0]) continue;
        const char *name = g_mine[i].label[0] ? g_mine[i].label : g_mine[i].path;
        if (v) snprintf(d, sizeof d, T("The movie disk \"%s\" is plugged in but not mounted yet."), name);
        else snprintf(d, sizeof d, T("The movie disk \"%s\" (%s) is missing."), name, g_mine[i].path);
        health_add(h, v ? HEALTH_INFO : HEALTH_WARN, T("Movie disk"), d,
                   v ? T("Hemma asks for it to be mounted by itself - give it a minute.")
                     : T("Plug it back in: Hemma finds it again by itself. Not using it any more? Forget it under Watch "
                         "→ Library settings."),
                   "/media/settings");
    }
    if (g_present && silent(now)) {
        snprintf(todo, sizeof todo, T("On the Pi: %s"), "sudo systemctl enable --now hemma-disks.path");
        health_add(h, HEALTH_WARN, T("Disk helper"), T("The disk helper didn't answer."), todo, "/media/settings");
    }
    for (int i = 0; i < g_ndrives; i++) {
        const Drive *dr = &g_drives[i];
        if (!offered(dr) || in_use(dr)) continue;
        media_drive_size(dr->bytes, size, sizeof size);
        if (blank(dr)) {
            snprintf(d, sizeof d, T("An empty USB disk is plugged in: %s (%s)."), drive_name(dr), size);
            health_add(h, HEALTH_INFO, T("New disk found"), d,
                       g_present ? T("Format it for movies under Watch → Library settings.") : NULL, "/media/settings");
            continue;
        }
        for (int k = 0; k < dr->nvol; k++) {
            if (!usable(&dr->vol[k])) continue;
            vol_title(dr, &dr->vol[k], title, sizeof title);
            snprintf(d, sizeof d, T("A USB disk is plugged in: %s (%s, %s)."), title, fs_name(dr->vol[k].fstype), size);
            health_add(h, HEALTH_INFO, T("New disk found"), d,
                       g_present ? T("Use it for movies under Watch → Library settings.") : NULL, "/media/settings");
            break;
        }
    }
}

/* --------------------------------------------------------------- actions */

static bool admin_ok(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "%s", T("Only an admin can do that"));
    return false;
}

static bool helper_ok(Ctx *c) {
    g_present = present();
    if (!g_present) {
        ctx_fail(c, "%s", T("This install has no disk helper - see docs/raspberry-pi.md, step 7"));
        return false;
    }
    if (working(now_sec())) {
        ctx_fail(c, "%s", T("The disk helper is busy - give it a moment"));
        return false;
    }
    return true;
}

void media_drives_act_use(Ctx *c, Fields *f) {
    if (!admin_ok(c) || !helper_ok(c)) return;
    const char *uuid = fields_str(f, "uuid");
    const Drive *d = NULL;
    const DriveVol *v = vol_by_uuid(uuid, &d);
    if (!v || !offered(d) || !usable(v)) {
        ctx_fail(c, "%s", T("That disk isn't here any more - or it's in use"));
        return;
    }
    char what[100], title[100];
    snprintf(what, sizeof what, "use %s", v->uuid);
    vol_title(d, v, title, sizeof title);
    if (!ask(c->me->id, what)) {
        ctx_fail(c, "%s", T("Couldn't ask the disk helper (see the log)"));
        return;
    }
    log_info("disks: %s asked for \"%s\" (%s, %s) to be used for movies", c->me->name, title, v->kname, v->fstype);
    ctx_ok(c, T("Setting up \"%s\"…"), title);
}

void media_drives_act_format(Ctx *c, Fields *f) {
    if (!admin_ok(c) || !helper_ok(c)) return;
    const char *k = fields_str(f, "disk");
    long long bytes = str_int_or(fields_str(f, "bytes"), -1);
    const Drive *d = NULL;
    for (int i = 0; i < g_ndrives && !d; i++)
        if (str_eq(g_drives[i].kname, k)) d = &g_drives[i];
    if (!d || d->bytes != bytes || !offered(d) || in_use(d) || !blank(d)) {
        ctx_fail(c, "%s", T("That disk isn't here any more - or it isn't empty"));
        return;
    }
    char typed[32];
    str_copy(typed, sizeof typed, fields_str(f, "confirm"));
    str_trim(typed);
    if (!str_ieq(typed, "erase")) {
        ctx_fail_at(c, "confirm", "%s", T("Type ERASE to format it"));
        return;
    }
    char what[100];
    snprintf(what, sizeof what, "format %s %lld", d->kname, (long long)d->bytes);
    if (!ask(c->me->id, what)) {
        ctx_fail(c, "%s", T("Couldn't ask the disk helper (see the log)"));
        return;
    }
    log_info("disks: %s asked for the empty disk %s (%s) to be formatted for movies", c->me->name, d->name, d->kname);
    ctx_ok(c, T("Formatting \"%s\"…"), drive_name(d));
    ctx_redirect(c, "/media/settings");
}

void media_drives_act_forget(Ctx *c, Fields *f) {
    if (!admin_ok(c) || !helper_ok(c)) return;
    const Mine *m = mine_by_uuid(fields_str(f, "uuid"));
    if (!m) {
        ctx_fail(c, "%s", T("Hemma doesn't know that disk"));
        return;
    }
    char what[100];
    snprintf(what, sizeof what, "forget %s", m->uuid);
    if (!ask(c->me->id, what)) {
        ctx_fail(c, "%s", T("Couldn't ask the disk helper (see the log)"));
        return;
    }
    log_info("disks: %s asked for the movie disk \"%s\" to be forgotten", c->me->name, m->label);
    ctx_ok(c, T("Forgetting \"%s\"…"), m->label[0] ? m->label : m->path);
}

void media_drives_act_seen(Ctx *c, Fields *f) {
    (void)f;
    if (!admin_ok(c)) return;
    char at[24];
    snprintf(at, sizeof at, "%lld", (long long)g_res_at);
    setting_set(SET_SEEN, at);
    ctx_ok(c, "%s", T("Noted"));
}

/* ------------------------------------------------------------------ tests */

void media_test_drives(const char *sys, const char *udev, const char *mountinfo_file) {
    g_test = sys != NULL;
    str_copy(g_sys, sizeof g_sys, sys ? sys : "");
    str_copy(g_udev, sizeof g_udev, udev ? udev : "");
    str_copy(g_minfo, sizeof g_minfo, mountinfo_file ? mountinfo_file : "");
    g_scan_at = 0;
}

void media_test_drive_dirs(const char *ask_dir, const char *answers) {
    g_test = true;
    str_copy(g_ask, sizeof g_ask, ask_dir ? ask_dir : "");
    str_copy(g_ans, sizeof g_ans, answers ? answers : "");
    g_present = present();
    g_read_at = 0;
}
