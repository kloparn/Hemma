/* Downloads: the zip writer (every byte read back by a little unzip of our own, and a file over 4 GB
 * through the sizes and headers alone), names people can save, the endpoints over real sockets (a film,
 * a season's zip, HEAD, who is refused, two zips at once), giving way to people watching, the buttons
 * and their words in Swedish and Spanish. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "download.h"
#include "media.h"
#include "zip.h"

/* ---------------------------------------------------- a little unzip */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint32_t crc_bitwise(const uint8_t *p, size_t n) { /* the slow way, as the standard says it */
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

typedef struct {
    char name[400];
    uint32_t crc;
    uint64_t size;
    const uint8_t *data;
    uint16_t time, date;
} Unz;

#define BAD(...) (test_fail(__FILE__, __LINE__, __VA_ARGS__), -1)

/* Reads an archive the way unzip does - the end records, then the central directory - and checks each
   entry's local header, its bytes, its data descriptor and its CRC against the central directory's.
   How many entries, or -1 (a test failure said why). */
static int unzip(const uint8_t *z, size_t n, Unz *out, int max) {
    if (n < 22) return BAD("%zu bytes: no end record", n);
    const uint8_t *eocd = z + n - 22;
    if (rd32(eocd) != 0x06054b50) return BAD("no end of central directory record");
    uint64_t entries = rd16(eocd + 10), cd_size = rd32(eocd + 12), cd_off = rd32(eocd + 16);
    if (rd16(eocd + 8) != entries) return BAD("entries on this disk differ");
    if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        const uint8_t *loc = eocd - 20;
        if (n < 98 || rd32(loc) != 0x07064b50 || rd32(loc + 16) != 1) return BAD("no zip64 locator");
        uint64_t at = rd64(loc + 8);
        const uint8_t *r = z + at;
        if (at + 56 != (uint64_t)(loc - z) || rd32(r) != 0x06064b50 || rd64(r + 4) != 44) return BAD("no zip64 end record");
        entries = rd64(r + 32), cd_size = rd64(r + 40), cd_off = rd64(r + 48);
        if (rd64(r + 24) != entries || cd_off + cd_size != at) return BAD("the zip64 end record is off");
    } else if (cd_off + cd_size != n - 22) {
        return BAD("the central directory doesn't end at the end record");
    }
    const uint8_t *p = z + cd_off;
    if ((int64_t)entries > max) return BAD("%llu entries", (unsigned long long)entries);
    for (uint64_t i = 0; i < entries; i++) {
        if (p + 46 > z + n || rd32(p) != 0x02014b50) return BAD("central header %llu", (unsigned long long)i);
        uint32_t flags = rd16(p + 8), nl = rd16(p + 28), xl = rd16(p + 30), cl = rd16(p + 32);
        if (flags != 0x0808 || rd16(p + 10) != 0) return BAD("entry %llu: flags %x, method %u", (unsigned long long)i, flags, rd16(p + 10));
        uint64_t csize = rd32(p + 20), usize = rd32(p + 24), off = rd32(p + 42);
        uint32_t crc = rd32(p + 16);
        const uint8_t *nm = p + 46, *x = nm + nl;
        for (const uint8_t *q = x; q + 4 <= x + xl; q += 4 + rd16(q + 2)) {
            if (rd16(q) != 1) continue;
            const uint8_t *v = q + 4;
            if (usize == 0xFFFFFFFFu) usize = rd64(v), v += 8;
            if (csize == 0xFFFFFFFFu) csize = rd64(v), v += 8;
            if (off == 0xFFFFFFFFu) off = rd64(v), v += 8;
            if (v != q + 4 + rd16(q + 2)) return BAD("entry %llu: the zip64 field's length", (unsigned long long)i);
        }
        if (csize != usize) return BAD("stored, yet the sizes differ");
        const uint8_t *l = z + off;
        if (off + 30 > n || rd32(l) != 0x04034b50 || rd16(l + 6) != flags || rd16(l + 8) != 0 || rd16(l + 26) != nl ||
            memcmp(l + 30, nm, nl) != 0 || rd16(l + 10) != rd16(p + 12) || rd16(l + 12) != rd16(p + 14))
            return BAD("entry %llu: the local header doesn't match", (unsigned long long)i);
        uint32_t lxl = rd16(l + 28);
        bool l64 = lxl >= 4 && rd16(l + 30 + nl) == 1;
        const uint8_t *data = l + 30 + nl + lxl, *d = data + usize;
        if ((uint64_t)(d - z) + (l64 ? 24 : 16) > n || rd32(d) != 0x08074b50 || rd32(d + 4) != crc)
            return BAD("entry %llu: no data descriptor with its CRC", (unsigned long long)i);
        if (l64 ? rd64(d + 8) != usize || rd64(d + 16) != usize : rd32(d + 8) != usize || rd32(d + 12) != usize)
            return BAD("entry %llu: the data descriptor's sizes", (unsigned long long)i);
        if (crc_bitwise(data, (size_t)usize) != crc) return BAD("entry %llu: wrong CRC", (unsigned long long)i);
        Unz *u = &out[i];
        str_ncopy(u->name, sizeof u->name, (const char *)nm, nl);
        u->crc = crc, u->size = usize, u->data = data, u->time = (uint16_t)rd16(p + 12), u->date = (uint16_t)rd16(p + 14);
        p += 46 + nl + xl + cl;
    }
    if (p != z + cd_off + cd_size) return BAD("the central directory's size");
    return (int)entries;
}

/* ------------------------------------------------------ the zip writer */

typedef struct {
    const char *data[8];
    size_t at;
    int i;
    Buf out;
} Mem;

static size_t mem_read(void *ctx, int i, void *buf, size_t cap) {
    Mem *m = ctx;
    if (m->i != i) m->i = i, m->at = 0;
    size_t n = strlen(m->data[i]) - m->at;
    if (n > cap) n = cap;
    memcpy(buf, m->data[i] + m->at, n);
    m->at += n;
    return n;
}

static bool mem_write(void *ctx, const void *p, size_t n) {
    buf_put(&((Mem *)ctx)->out, p, n);
    return true;
}

TEST(zip_writes_what_it_said_and_unzips) {
    CHECK_INT(zip_crc32(0, "123456789", 9), 0xCBF43926u); /* the check value */
    CHECK_INT(zip_crc32(zip_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
    uint16_t t = 0, d = 0;
    zip_dos_time(2026, 10, 21, 14, 30, 59, &t, &d);
    CHECK_INT(t, (14 << 11) | (30 << 5) | 29);
    CHECK_INT(d, (46 << 9) | (10 << 5) | 21);
    zip_dos_time(1970, 1, 1, 0, 0, 0, &t, &d); /* before DOS: its first day */
    CHECK_INT(d, (0 << 9) | (1 << 5) | 1);

    Buf big = {0}; /* bigger than the writer's buffer below, so it's read in pieces */
    for (int i = 0; i < 5000; i++) buf_printf(&big, "line %d of the episode\n", i);
    Mem m = {{"hello", "", buf_cstr(&big), "1\n00:00:01,000 --> 00:00:02,000\nHej då\n"}, 0, -1, {0}};
    ZipEntry e[4] = {{.name = "a.txt"}, {.name = "empty.bin"}, {.name = "Show/Season 1/Show S01E01 - Pilot.mp4"},
                     {.name = "Show/Säsong 1/Åsa.sv.srt"}};
    for (int i = 0; i < 4; i++) {
        e[i].size = strlen(m.data[i]);
        zip_dos_time(2026, 10, 21, 12, i, 0, &e[i].dos_time, &e[i].dos_date);
    }
    char buf[4096];
    ZipIo io = {mem_read, mem_write, &m};
    CHECK(zip_write(e, 4, &io, buf, sizeof buf));
    CHECK_INT(m.out.len, zip_size(e, 4)); /* exactly what Content-Length says */
    Unz u[8];
    CHECK_INT(unzip((const uint8_t *)m.out.p, m.out.len, u, 8), 4);
    for (int i = 0; i < 4; i++) {
        CHECK_STR(u[i].name, e[i].name);
        CHECK_INT(u[i].size, strlen(m.data[i]));
        CHECK(memcmp(u[i].data, m.data[i], strlen(m.data[i])) == 0);
        CHECK_INT(u[i].crc, e[i].crc);
        CHECK_INT(u[i].time, e[i].dos_time);
    }
    for (size_t i = 0; i + 4 <= m.out.len; i++) /* small: no zip64 anywhere */
        if (rd32((const uint8_t *)m.out.p + i) == 0x06064b50 || rd32((const uint8_t *)m.out.p + i) == 0x07064b50)
            test_fail(__FILE__, __LINE__, "a zip64 record at %zu", i);

    /* a read that comes up short ends the archive unfinished */
    Mem m2 = {{"hello", "", "x", "y"}, 0, -1, {0}};
    e[0].size = 10;
    io.ctx = &m2;
    CHECK(!zip_write(e, 1, &io, buf, sizeof buf));
    buf_free(&m2.out);
    buf_free(&m.out);
    buf_free(&big);
}

TEST(zip_over_4_gb_uses_zip64_where_it_must) {
    /* a 5 GB episode and its subtitle, which then starts past 4 GB - through the sizes and the headers */
    const uint64_t G5 = 5ULL << 30, M32 = 0xFFFFFFFFu;
    ZipEntry e[2] = {{.name = "big.mkv", .size = G5, .crc = 0x11223344}, {.name = "small.srt", .size = 100, .crc = 7}};
    uint64_t off1 = 30 + 7 + 20 + G5 + 24;
    uint64_t cd = (46 + 7 + 4 + 16) + (46 + 9 + 4 + 8);
    CHECK_INT(zip_size(e, 2), off1 + 30 + 9 + 100 + 16 + cd + 56 + 20 + 22);

    uint8_t h[ZIP_HEAD_MAX];
    CHECK_INT(zip_local_header(&e[0], 0, h), 57);
    CHECK_INT(rd16(h + 4), 45);              /* needs zip64 */
    CHECK_INT(rd32(h + 18), M32);            /* sizes: in the zip64 field ... */
    CHECK_INT(rd32(h + 22), M32);
    CHECK_INT(rd16(h + 28), 20);
    CHECK_INT(rd16(h + 30 + 7), 1);          /* ... which is there (so the descriptor has 8-byte sizes) */
    CHECK_INT(rd16(h + 30 + 7 + 2), 16);
    CHECK_INT(zip_data_descriptor(&e[0], h), 24);
    CHECK_INT(rd32(h + 4), 0x11223344u);
    CHECK_INT(rd64(h + 8), G5);
    CHECK_INT(rd64(h + 16), G5);
    CHECK_INT(zip_central_header(&e[0], 0, h), 46 + 7 + 20);
    CHECK_INT(rd32(h + 20), M32);
    CHECK_INT(rd32(h + 24), M32);
    CHECK_INT(rd32(h + 42), 0);              /* at 0: no zip64 offset */
    CHECK_INT(rd16(h + 30), 20);
    CHECK_INT(rd16(h + 46 + 7 + 2), 16);
    CHECK_INT(rd64(h + 46 + 7 + 4), G5);
    CHECK_INT(rd64(h + 46 + 7 + 12), G5);

    CHECK_INT(zip_local_header(&e[1], off1, h), 39); /* small, but past 4 GB */
    CHECK_INT(rd16(h + 4), 45);
    CHECK_INT(rd16(h + 28), 0);                      /* no zip64 field: its descriptor has 4-byte sizes */
    CHECK_INT(zip_data_descriptor(&e[1], h), 16);
    CHECK_INT(rd32(h + 8), 100);
    CHECK_INT(zip_central_header(&e[1], off1, h), 46 + 9 + 12);
    CHECK_INT(rd32(h + 20), 100);
    CHECK_INT(rd32(h + 42), M32);                    /* the offset is in the zip64 field */
    CHECK_INT(rd16(h + 46 + 9 + 2), 8);
    CHECK_INT(rd64(h + 46 + 9 + 4), off1);

    uint64_t cd_at = off1 + 30 + 9 + 100 + 16;
    uint8_t end[ZIP_END_MAX];
    CHECK_INT(zip_end_records(2, cd_at, cd, end), 98);
    CHECK_INT(rd32(end), 0x06064b50);
    CHECK_INT(rd64(end + 32), 2);
    CHECK_INT(rd64(end + 40), cd);
    CHECK_INT(rd64(end + 48), cd_at);
    CHECK_INT(rd32(end + 56), 0x07064b50);
    CHECK_INT(rd64(end + 56 + 8), cd_at + cd);
    CHECK_INT(rd32(end + 56 + 16), 1);
    CHECK_INT(rd32(end + 76), 0x06054b50);
    CHECK_INT(rd16(end + 76 + 10), 2);    /* the count fits ... */
    CHECK_INT(rd32(end + 76 + 16), M32);  /* ... the offset doesn't */
    CHECK_INT(zip_end_records(70000, 1000, 500, end), 98); /* more entries than 16 bits hold */
    CHECK_INT(rd16(end + 76 + 10), 0xFFFF);
    CHECK_INT(rd32(end + 76 + 16), 1000);
    CHECK_INT(zip_end_records(3, 1000, 500, end), 22); /* nothing big: the plain one only */
    ZipEntry noname = {.name = "", .size = 1};
    CHECK_INT(zip_local_header(&noname, 0, h), 0);
}

/* --------------------------------------------------------------- names */

TEST(download_names_are_safe_and_friendly) {
    char out[200];
    static const char *const cases[][2] = {
        {"Star Wars: A New Hope", "Star Wars - A New Hope"}, {"Title : Sub", "Title - Sub"}, {"10:30", "10-30"},
        {"AC/DC\\Live|Tour", "AC-DC-Live-Tour"},             {"What?*<>", "What"},            {"say \"hi\"", "say 'hi'"},
        {"  .hidden. ", "hidden"},                           {"", "video"},                   {"...", "video"},
        {"Åsa\x01 och Öyvind", "Åsa och Öyvind"},
        {"Con", "Con_"},                                     {"nul.x", "nul_.x"},             {"COM1", "COM1_"},
        {"Console", "Console"},                              {"Com", "Com"},                {"COM0", "COM0"},
        {"Lpt9.srt", "Lpt9_.srt"},                           {"Auxiliary (2020)", "Auxiliary (2020)"},
    };
    for (int i = 0; i < countof(cases); i++) {
        media_safe_name(cases[i][0], out, sizeof out);
        CHECK_STR(out, cases[i][1]);
    }
    Buf b = {0};
    media_content_disposition(&b, "Älska mig (2020) 100%.mp4");
    CHECK_STR(buf_cstr(&b), "Content-Disposition: attachment; filename=\"Alska mig (2020) 100_.mp4\"; "
                            "filename*=UTF-8''%C3%84lska%20mig%20%282020%29%20100%25.mp4\r\n");
    buf_reset(&b);
    media_content_disposition(&b, "千と千尋.mkv");
    CHECK_HAS(buf_cstr(&b), "filename=\"____.mkv\"");
    buf_free(&b);
}

/* -------------------------------------------- a library and its people */

static void put(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static char *pattern(size_t n, int seed) {
    char *p = xmalloc(n);
    for (size_t i = 0; i < n; i++) p[i] = (char)((i * 31 + (i >> 7) + (size_t)seed * 101) & 0xFF);
    return p;
}

/* A film with its play-everywhere copy, and a series: two seasons, subtitles, a copy, a double episode. */
static void library(const char *root) {
    static const struct {
        const char *rel;
        size_t n;
    } files[] = {
        {"Clip (2020)/Clip (2020).mkv", 90000},          {"Clip (2020)/Clip (2020).web.mp4", 70000},
        {"Testshow/Season 1/Testshow.S01E01.Pilot.mkv", 60000}, {"Testshow/Season 1/Testshow.S01E02.mkv", 50000},
        {"Testshow/Season 1/Testshow.S01E02.web.mp4", 40000},   {"Testshow/Season 2/Testshow.S02E01.mkv", 30000},
        {"Testshow/Season 2/Testshow.S02E02E03.mkv", 20000},
    };
    for (int i = 0; i < countof(files); i++) {
        char *p = pattern(files[i].n, i);
        put(root, files[i].rel, p, files[i].n);
        free(p);
    }
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHej\n";
    put(root, "Testshow/Season 1/Testshow.S01E01.Pilot.sv.srt", srt, strlen(srt));
    put(root, "Testshow/Season 1/Testshow.S01E02.en.forced.srt", srt, strlen(srt));
    setting_set("media.roots", root);
    media_scan_sync();
}

static const MediaFile *file_named(const char *suffix) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, suffix)) return &ix->files[i];
    return NULL;
}

static int person(const char *name, const char *user, const char *pass, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pass, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

TEST(download_names_and_zip_contents_follow_the_library) {
    th_boot();
    library(test_tmpdir());
    const MediaFile *film = file_named("Clip (2020).mkv"), *pilot = file_named("S01E01.Pilot.mkv"),
                    *e2 = file_named("S01E02.mkv"), *dbl = file_named("S02E02E03.mkv");
    CHECK(film && pilot && e2 && dbl);
    char name[300];
    media_download_name(film, name, sizeof name);
    CHECK_STR(name, "Clip (2020).mp4"); /* the copy goes out: its extension */
    media_download_name(pilot, name, sizeof name);
    CHECK_STR(name, "Testshow S01E01 - Pilot.mkv");
    media_download_name(e2, name, sizeof name);
    CHECK_STR(name, "Testshow S01E02.mp4");
    media_download_name(dbl, name, sizeof name);
    CHECK_STR(name, "Testshow S02E02-E03.mkv");

    const MediaSeries *s = media_series(pilot->series);
    CHECK(s);
    int n = 0;
    MediaZipItem *it = media_zip_items(s, 1, &n);
    CHECK_INT(n, 4);
    CHECK_STR(it[0].name, "Testshow/Season 1/Testshow S01E01 - Pilot.mkv");
    CHECK_STR(it[1].name, "Testshow/Season 1/Testshow S01E01 - Pilot.sv.srt");
    CHECK_STR(it[2].name, "Testshow/Season 1/Testshow S01E02.mp4");
    CHECK_STR(it[3].name, "Testshow/Season 1/Testshow S01E02.en.forced.srt");
    CHECK_INT(it[2].file, e2->web); /* the copy, not the original */
    free(it);
    it = media_zip_items(s, -1, &n);
    CHECK_INT(n, 6);
    CHECK_STR(it[5].name, "Testshow/Season 2/Testshow S02E02-E03.mkv");
    free(it);
    it = media_zip_items(s, 5, &n);
    CHECK_INT(n, 0);
    free(it);
    media_zip_name(s, 1, name, sizeof name);
    CHECK_STR(name, "Testshow - Season 1.zip");
    media_zip_name(s, -1, name, sizeof name);
    CHECK_STR(name, "Testshow.zip");
    int64_t files = 60000 + 40000 + 2 * 36; /* the episodes as they go out, and their subtitles */
    CHECK(media_zip_bytes(s, 1) > (uint64_t)files && media_zip_bytes(s, 1) < (uint64_t)files + 1000);
    th_halt();
}

/* A DVD VobSub is two files: in a zip they go as a pair under the episode's name (an .idx without its
   .sub is nothing, and a .sub alone is MicroDVD text - not the library's); picture subtitles (.sup) and
   numbered ones (".2") go along like any. */
TEST(download_zips_keep_a_vobsub_pair_together) {
    th_boot();
    const char *root = test_tmpdir();
    put(root, "Show/Season 1/Show.S01E01.mkv", "e1", 2);
    put(root, "Show/Season 1/Show.S01E01.en.idx", "idx", 3);
    put(root, "Show/Season 1/Show.S01E01.en.sub", "pictures", 8);
    put(root, "Show/Season 1/Show.S01E01.en.2.srt", "srt", 3);
    put(root, "Show/Season 1/Show.S01E01.sv.sup", "PG", 2);
    put(root, "Show/Season 1/Show.S01E01.fr.idx", "idx alone", 9);
    put(root, "Show/Season 1/Show.S01E02.mkv", "e2", 2);
    put(root, "Show/Season 1/Show.S01E02.de.sub", "{1}{2}MicroDVD", 14);
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaFile *e1 = file_named("Show.S01E01.mkv");
    CHECK(e1);
    if (!e1) {
        th_halt();
        return;
    }
    int n = 0;
    MediaZipItem *it = media_zip_items(media_series(e1->series), 1, &n);
    CHECK_INT(n, 6);
    int idx = -1, sub = -1, srt = -1, sup = -1;
    for (int i = 0; i < n; i++) {
        if (str_eq(it[i].name, "Show/Season 1/Show S01E01.en.idx")) idx = i;
        if (str_eq(it[i].name, "Show/Season 1/Show S01E01.en.sub")) sub = i;
        if (str_eq(it[i].name, "Show/Season 1/Show S01E01.en.2.srt")) srt = i;
        if (str_eq(it[i].name, "Show/Season 1/Show S01E01.sv.sup")) sup = i;
        CHECK(!strstr(it[i].name, ".fr.") && !strstr(it[i].name, ".de."));
    }
    CHECK(idx >= 0 && sub == idx + 1 && srt >= 0 && sup >= 0);
    if (sub >= 0) CHECK(it[sub].file == file_named("Show.S01E01.en.sub")->id && it[sub].size == 8);
    free(it);
    th_halt();
}

/* ----------------------------------------------- the endpoints, over sockets */

static volatile int g_done;
static int g_port;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static sock_t dial(void) {
    sock_t s = tcp_connect_nb(0x7F000001, g_port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    for (int64_t end = now_ms() + 5000; n && now_ms() < end;) {
        long k = sock_send(s, c, n);
        if (k > 0) c += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

static void get(const char *method, const char *path, const char *cookie, const char *extra, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sConnection: close\r\n\r\n", method,
             path, cookie, extra ? extra : "");
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, head, strlen(head));
    for (int64_t end = now_ms() + 10000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[16384];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(resp, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
}

static const char *body_of(Buf *r, size_t *n) {
    const char *e = r->p ? strstr(buf_cstr(r), "\r\n\r\n") : NULL;
    *n = e ? r->len - (size_t)(e + 4 - r->p) : 0;
    return e ? e + 4 : "";
}

static long long header_int(Buf *r, const char *name) {
    char want[64];
    snprintf(want, sizeof want, "\r\n%s: ", name);
    const char *h = r->p ? strstr(buf_cstr(r), want) : NULL;
    return h ? strtoll(h + strlen(want), NULL, 10) : -1;
}

static void sign_in(const char *user, const char *pass, char *cookie, size_t cap) {
    char req[600], form[200];
    snprintf(form, sizeof form, "username=%s&password=%s&next=%%2F", user, pass);
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             strlen(form), form);
    cookie[0] = '\0';
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, req, strlen(req));
    Buf r = {0};
    for (int64_t end = now_ms() + 5000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[4096];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(&r, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

static bool same_as_file(const char *root, const char *rel, const char *data, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    Buf got = {0};
    bool ok = file_read_all(path, &got, n + 1) && got.len == n && memcmp(got.p, data, n) == 0;
    buf_free(&got);
    return ok;
}

static void settle(void) { /* every download's thread done */
    for (int i = 0; i < 500 && media_downloads_active(); i++) plat_sleep_ms(10);
}

TEST(download_endpoints_send_files_and_zips_to_grown_ups) {
    th_boot();
    auth_configure("download-salt", 1000);
    person("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    person("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    person("Leo", "leo", "pw-leo-1234", ROLE_CHILD);
    int gus = person("Gus", "gus", "pw-gus-1234", ROLE_GUEST);
    person("Living room TV", "tv", "pw-tv-12345", ROLE_DISPLAY);
    pref_set(gus, PREF_LANG, "sv");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    library(root);
    const MediaFile *film = file_named("Clip (2020).mkv"), *pilot = file_named("S01E01.Pilot.mkv");
    CHECK(film && pilot);
    const MediaSeries *s = media_series(pilot->series);
    uint64_t season1 = media_zip_bytes(s, 1);
    char url[200], zurl[200], hfilm[17], hshow[17];
    media_id_hex(film->id, hfilm);
    media_id_hex(s->id, hshow);
    media_stream_resume(); /* (an earlier test's th_halt() ran the module's stop hook) */

    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], eva[64], leo[64], gusc[64], tv[64];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    sign_in("leo", "pw-leo-1234", leo, sizeof leo);
    sign_in("gus", "pw-gus-1234", gusc, sizeof gusc);
    sign_in("tv", "pw-tv-12345", tv, sizeof tv);
    CHECK(strlen(adam) == 32 && strlen(eva) == 32 && strlen(leo) == 32 && strlen(gusc) == 32 && strlen(tv) == 32);
    Buf r = {0};
    size_t n = 0;
    const char *body;

    /* a film: the copy that plays everywhere, called what it is */
    snprintf(url, sizeof url, "/media/file/%s?download=1", hfilm);
    get("GET", url, eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Disposition: attachment; filename=\"Clip (2020).mp4\"; "
                            "filename*=UTF-8''Clip%20%282020%29.mp4\r\n");
    CHECK_INT(header_int(&r, "Content-Length"), 70000);
    body = body_of(&r, &n);
    CHECK_INT(n, 70000);
    CHECK(same_as_file(root, "Clip (2020)/Clip (2020).web.mp4", body, n));
    get("GET", url, adam, "Range: bytes=100-199\r\n", &r); /* a download can be resumed */
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 206 "));
    CHECK_HAS(buf_cstr(&r), "Content-Disposition: attachment");
    CHECK_INT(header_int(&r, "Content-Length"), 100);
    get("HEAD", url, eva, NULL, &r);
    CHECK_HAS(buf_cstr(&r), "filename=\"Clip (2020).mp4\"");
    body_of(&r, &n);
    CHECK_INT(n, 0);
    snprintf(url, sizeof url, "/media/file/%s", hfilm); /* watching: no download header */
    get("GET", url, eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 ") && !strstr(buf_cstr(&r), "Content-Disposition"));

    /* a season as one zip: the episodes as they play everywhere, their subtitles, the exact length up front */
    snprintf(zurl, sizeof zurl, "/media/zip/%s?s=1", hshow);
    get("GET", zurl, eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: application/zip\r\n");
    CHECK_HAS(buf_cstr(&r), "Accept-Ranges: none\r\n");
    CHECK_HAS(buf_cstr(&r), "filename=\"Testshow - Season 1.zip\"");
    CHECK_INT(header_int(&r, "Content-Length"), season1); /* what the button says, to the byte */
    body = body_of(&r, &n);
    CHECK_INT(n, season1);
    Unz u[16];
    CHECK_INT(unzip((const uint8_t *)body, n, u, 16), 4);
    CHECK_STR(u[0].name, "Testshow/Season 1/Testshow S01E01 - Pilot.mkv");
    CHECK(same_as_file(root, "Testshow/Season 1/Testshow.S01E01.Pilot.mkv", (const char *)u[0].data, (size_t)u[0].size));
    CHECK_STR(u[1].name, "Testshow/Season 1/Testshow S01E01 - Pilot.sv.srt");
    CHECK_STR(u[2].name, "Testshow/Season 1/Testshow S01E02.mp4");
    CHECK(same_as_file(root, "Testshow/Season 1/Testshow.S01E02.web.mp4", (const char *)u[2].data, (size_t)u[2].size));
    CHECK_STR(u[3].name, "Testshow/Season 1/Testshow S01E02.en.forced.srt");
    const char *keep = plat_getenv("HEMMA_TEST_ZIP_OUT"); /* (a copy for unzip -t / Python's zipfile, by hand) */
    if (keep && *keep) file_write_atomic(keep, body, n);
    snprintf(zurl, sizeof zurl, "/media/zip/%s", hshow); /* the whole series */
    get("GET", zurl, adam, "Range: bytes=10-\r\n", &r);   /* (no ranges for a zip: all of it) */
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "filename=\"Testshow.zip\"");
    body = body_of(&r, &n);
    CHECK_INT(unzip((const uint8_t *)body, n, u, 16), 6);
    CHECK_STR(u[4].name, "Testshow/Season 2/Testshow S02E01.mkv");
    get("HEAD", zurl, eva, NULL, &r);
    CHECK_INT(header_int(&r, "Content-Length"), media_zip_bytes(s, -1));
    body_of(&r, &n);
    CHECK_INT(n, 0);
    settle();

    /* not there */
    snprintf(zurl, sizeof zurl, "/media/zip/%s?s=7", hshow);
    get("GET", zurl, eva, NULL, &r);
    CHECK_HAS(buf_cstr(&r), "404");
    CHECK_HAS(body_of(&r, &n), "That season isn't in the library");
    get("GET", "/media/zip/0123456789abcdef", eva, NULL, &r);
    CHECK_HAS(body_of(&r, &n), "This series isn't in the library");

    /* two zips at once at most */
    media_zip_test_busy(MEDIA_MAX_ZIPS);
    snprintf(zurl, sizeof zurl, "/media/zip/%s?s=1", hshow);
    get("GET", zurl, eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 503 "));
    CHECK_HAS(buf_cstr(&r), "Retry-After: 60\r\n");
    CHECK_HAS(body_of(&r, &n), "Two seasons are being downloaded already");
    media_zip_test_busy(MEDIA_MAX_ZIPS - 1); /* one running: a second may */
    get("HEAD", zurl, eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    media_zip_test_busy(0);
    settle();

    /* grown-ups only: not a child, a guest (in Swedish, her language), a TV - nor anyone signed out */
    snprintf(url, sizeof url, "/media/file/%s?download=1", hfilm);
    const char *who[] = {leo, gusc, tv};
    for (int i = 0; i < 3; i++) {
        get("GET", url, who[i], NULL, &r);
        CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 "));
        CHECK(!strstr(buf_cstr(&r), "Content-Disposition"));
        get("GET", zurl, who[i], NULL, &r);
        CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 "));
        CHECK(!strstr(buf_cstr(&r), "application/zip"));
    }
    get("GET", url, leo, NULL, &r);
    CHECK_HAS(body_of(&r, &n), "Only grown-ups can download videos.");
    get("GET", url, gusc, NULL, &r);
    CHECK_HAS(body_of(&r, &n), "Bara vuxna kan ladda ner videor.");
    snprintf(url, sizeof url, "/media/file/%s", hfilm); /* (watching is still theirs) */
    get("GET", url, leo, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    snprintf(url, sizeof url, "/media/file/%s?download=1", hfilm);
    get("GET", url, "-", NULL, &r); /* (signed out: the sign-in page) */
    CHECK(!strstr(buf_cstr(&r), "Content-Disposition"));
    CHECK_HAS(body_of(&r, &n), "data-mod=\"auth\"");
    get("GET", zurl, "-", NULL, &r);
    CHECK(!strstr(buf_cstr(&r), "application/zip"));
    CHECK_HAS(body_of(&r, &n), "data-mod=\"auth\"");
    settle();

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

TEST(downloads_give_way_to_people_watching) {
    th_boot();
    auth_configure("download-salt", 1000);
    person("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    library(root);
    const MediaFile *film = file_named("Clip (2020).mkv"), *pilot = file_named("S01E01.Pilot.mkv");
    CHECK(film && pilot);
    char url[200], zurl[200], hfilm[17], hshow[17];
    media_id_hex(film->id, hfilm);
    media_id_hex(pilot->series, hshow);
    snprintf(url, sizeof url, "/media/file/%s?download=1", hfilm);
    snprintf(zurl, sizeof zurl, "/media/zip/%s?s=2", hshow); /* 50 KB */
    media_stream_resume();
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char eva[64];
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    Buf r = {0};
    size_t n = 0;

    /* nobody watching: at once */
    media_download_test_pace(30000, 0);
    int64_t t0 = now_ms();
    get("GET", url, eva, NULL, &r);
    body_of(&r, &n);
    CHECK_INT(n, 70000);
    CHECK(now_ms() - t0 < 1500);
    /* someone watching: 30 KB a second - 70 KB takes into a third second, a 50 KB zip into a second */
    media_download_test_pace(30000, 1);
    t0 = now_ms();
    get("GET", url, eva, NULL, &r);
    body_of(&r, &n);
    CHECK_INT(n, 70000);
    CHECK(now_ms() - t0 >= 1900);
    t0 = now_ms();
    get("GET", zurl, eva, NULL, &r);
    body_of(&r, &n);
    CHECK(n > 50000);
    CHECK(now_ms() - t0 >= 900);
    /* ... and watching itself never waits */
    snprintf(url, sizeof url, "/media/file/%s", hfilm);
    t0 = now_ms();
    get("GET", url, eva, NULL, &r);
    body_of(&r, &n);
    CHECK_INT(n, 70000);
    CHECK(now_ms() - t0 < 1500);
    media_download_test_pace(0, 0);
    settle();

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* --------------------------------------------------------- the buttons */

static int as(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    str_copy(p.username, sizeof p.username, name);
    str_copy(p.pass, sizeof p.pass, "hash");
    tbl_put(&T_person, &p);
    return id;
}

TEST(download_buttons_are_for_grown_ups_and_speak_their_language) {
    th_boot();
    library(test_tmpdir());
    int adam = as("Adam", ROLE_ADMIN), eva = as("Eva", ROLE_MEMBER), leo = as("Leo", ROLE_CHILD),
        gus = as("Gus", ROLE_GUEST), tv = as("TV", ROLE_DISPLAY);
    const MediaFile *film = file_named("Clip (2020).mkv"), *pilot = file_named("S01E01.Pilot.mkv");
    CHECK(film && pilot);
    char watch[80], series[80], series2[100], tvmode[120], dl[80], z1[80], z2[80], zall[80];
    snprintf(watch, sizeof watch, "/media/watch/%016llx", (unsigned long long)film->id);
    snprintf(series, sizeof series, "/media/series/%016llx", (unsigned long long)pilot->series);
    snprintf(series2, sizeof series2, "%s?s=2", series);
    snprintf(tvmode, sizeof tvmode, "%s?tv=1&back=/", watch);
    snprintf(dl, sizeof dl, "/media/file/%016llx?download=1\" download data-native", (unsigned long long)film->id);
    snprintf(z1, sizeof z1, "/media/zip/%016llx?s=1\"", (unsigned long long)pilot->series);
    snprintf(z2, sizeof z2, "/media/zip/%016llx?s=2\"", (unsigned long long)pilot->series);
    snprintf(zall, sizeof zall, "/media/zip/%016llx\"", (unsigned long long)pilot->series);

    CHECK_HAS(th_page(eva, watch), dl);
    CHECK_HAS(th_page(eva, watch), "Download (68 KB)"); /* the copy's size: what goes out */
    CHECK_HAS(th_page(adam, watch), dl);
    CHECK_HAS(th_page(eva, series), z1); /* season 1 shown first */
    CHECK_HAS(th_page(eva, series), "Download season 1 (");
    CHECK_HAS(th_page(eva, series), zall);
    CHECK_HAS(th_page(eva, series), "Download all (");
    CHECK_HAS(th_page(eva, series2), z2);
    CHECK_HAS(th_page(eva, series), "Downloads slow down while someone is watching.");
    const int nobody[] = {leo, gus, tv};
    for (int i = 0; i < 3; i++) {
        CHECK(!strstr(th_page(nobody[i], watch), "download=1"));
        CHECK(!strstr(th_page(nobody[i], series), "/media/zip/"));
    }
    CHECK(!strstr(th_page(eva, tvmode), "download=1")); /* never in TV mode */

    /* in Swedish and Spanish: only data reads the same */
    static const char *const allow[] = {"Adam", "Eva", "A", "E", "Clip", "Testshow", "Pilot", "Show", NULL};
    Buf rep = {0};
    int bad = 0;
    const char *urls[] = {watch, series, series2};
    for (int i = 0; i < 3; i++) bad += th_untranslated(eva, urls[i], allow, &rep) + th_untranslated(adam, urls[i], allow, &rep);
    if (bad) printf("%s", buf_cstr(&rep));
    CHECK_INT(bad, 0);
    pref_set(eva, PREF_LANG, "sv");
    CHECK_HAS(th_page(eva, watch), "Ladda ner (68 KB)");
    CHECK_HAS(th_page(eva, series), "Ladda ner säsong 1 (");
    pref_set(eva, PREF_LANG, "es");
    CHECK_HAS(th_page(eva, series), "Descargar todo (");
    buf_free(&rep);
    th_halt();
}
