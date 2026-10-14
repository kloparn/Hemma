/* QR codes: the structure every reader looks for, the capacity limits, and fingerprints of codes an
 * independent reader (jsQR 1.4.0) decoded back to their text - as were 137 others across every
 * version, level and mask when the encoder was written. */
#include "../tests/test.h"
#include "html.h"
#include "qr.h"

static uint64_t fingerprint(const QrCode *q) { /* FNV-1a over the modules, row by row */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++) {
            h ^= q->m[y][x];
            h *= 0x100000001b3ULL;
        }
    return h;
}

static bool finder_at(const QrCode *q, int x0, int y0) {
    for (int dy = 0; dy < 7; dy++)
        for (int dx = 0; dx < 7; dx++) {
            int d = MAX(abs(dx - 3), abs(dy - 3));
            if (q->m[y0 + dy][x0 + dx] != (d != 2)) return false;
        }
    return true;
}

/* the 15 format bits, read back from the copy by the top-left finder */
static int format_read(const QrCode *q) {
    int bits = 0;
    for (int i = 0; i <= 5; i++) bits |= q->m[i][8] << i;
    bits |= q->m[7][8] << 6 | q->m[8][8] << 7 | q->m[8][7] << 8;
    for (int i = 9; i < 15; i++) bits |= q->m[8][14 - i] << i;
    return bits ^ 0x5412;
}

TEST(qr_an_address_reads_back) {
    static const char url[] = "http://192.168.1.50:8080";
    QrCode q;
    CHECK(qr_make(&q, url, strlen(url), QR_M, -1));
    CHECK_INT(q.version, 2);
    CHECK_INT(q.size, 25);
    CHECK(fingerprint(&q) == 0x6F913C2D88377DE7ULL); /* decoded by jsQR: "http://192.168.1.50:8080" */
    CHECK_INT(q.mask, 6);
    CHECK(finder_at(&q, 0, 0) && finder_at(&q, q.size - 7, 0) && finder_at(&q, 0, q.size - 7));
    for (int i = 8; i < q.size - 8; i++) CHECK(q.m[6][i] == (i % 2 == 0) && q.m[i][6] == (i % 2 == 0)); /* timing */
    CHECK(q.m[q.size - 8][8] == 1); /* the dark module */
    int f = format_read(&q);
    CHECK_INT(f >> 10, 0 << 3 | 6); /* level M (00), mask 6 */
    int second = 0; /* the copy by the other two finders says the same */
    for (int i = 0; i < 8; i++) second |= q.m[8][q.size - 1 - i] << i;
    for (int i = 8; i < 15; i++) second |= q.m[q.size - 15 + i][8] << i;
    CHECK_INT(second ^ 0x5412, f);

    CHECK(qr_make(&q, "http://hemma.local", 18, QR_L, 3)); /* a chosen mask is kept */
    CHECK(fingerprint(&q) == 0x8557D9D8E086D3C9ULL); /* decoded by jsQR too */
    CHECK_INT(q.mask, 3);
    CHECK_INT(format_read(&q) >> 10, 1 << 3 | 3);
}

TEST(qr_versions_grow_with_the_text_up_to_ten) {
    char text[300];
    memset(text, 'a', sizeof text);
    QrCode q;
    static const struct {
        int len, ecl, version;
    } cases[] = {{14, QR_M, 1}, {15, QR_M, 2}, {17, QR_L, 1}, {18, QR_L, 2}, {213, QR_M, 10},
                 {271, QR_L, 10}, {151, QR_Q, 10}, {119, QR_H, 10}, {1, QR_H, 1}, {0, QR_M, 1}};
    for (int i = 0; i < countof(cases); i++) {
        CHECK(qr_make(&q, text, (size_t)cases[i].len, cases[i].ecl, -1));
        CHECK_INT(q.version, cases[i].version);
        CHECK_INT(q.size, cases[i].version * 4 + 17);
        CHECK(q.mask >= 0 && q.mask <= 7);
        CHECK(finder_at(&q, 0, 0) && finder_at(&q, q.size - 7, 0) && finder_at(&q, 0, q.size - 7));
    }
    CHECK(!qr_make(&q, text, 214, QR_M, -1)); /* past version 10 */
    CHECK(!qr_make(&q, text, 272, QR_L, -1));
    CHECK(!qr_make(&q, text, 10, 4, -1));
    CHECK(!qr_make(&q, text, 10, QR_M, 8));
    CHECK(qr_make(&q, text, 213, QR_M, -1)); /* version 7 and up carry their version twice */
    int v = 0;
    for (int i = 17; i >= 0; i--) v = v << 1 | q.m[i / 3][q.size - 11 + i % 3];
    CHECK_INT(v >> 12, 10);
}

TEST(qr_svg_draws_the_code_with_its_quiet_zone) {
    QrCode q;
    CHECK(qr_make(&q, "http://192.168.1.50:8080", 24, QR_M, -1));
    Buf b = {0};
    qr_svg(&b, &q, "Hemma's address <here>");
    const char *s = buf_cstr(&b);
    CHECK_HAS(s, "<svg class=\"qr\" viewBox=\"-4 -4 33 33\" role=\"img\" aria-label=\"Hemma&#39;s address &lt;here&gt;\"");
    CHECK_HAS(s, "<rect x=\"-4\" y=\"-4\" width=\"33\" height=\"33\" fill=\"#fff\"/>");
    CHECK_HAS(s, "d=\"M0 0h7v1h-7zM"); /* the top-left finder's top edge, as one run */
    CHECK(str_ends(s, "\"/></svg>"));
    buf_free(&b);
}
