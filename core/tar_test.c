#include "../tests/test.h"
#include "tar.h"

TEST(tar_round_trip_and_padding) {
    Buf a = {0};
    char big[1500];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
    tar_add(&a, "hemma.db", big, sizeof big, 1760000000, 0600);
    tar_add(&a, "hemma-secret.txt", "HEMMA_AUTH_SALT=abc\n", 20, 1760000000, 0600);
    tar_add(&a, "empty", "", 0, 0, 0644);
    tar_end(&a);
    CHECK_INT((int)(a.len % 512), 0);
    CHECK_INT((int)a.len, 512 + 1536 + 512 + 512 + 512 + 1024); /* headers, data padded, the end */
    CHECK(tar_looks(a.p, a.len));
    const void *p = NULL;
    size_t n = 0;
    CHECK(tar_find(a.p, a.len, "hemma.db", &p, &n));
    CHECK_INT((int)n, (int)sizeof big);
    CHECK(memcmp(p, big, n) == 0);
    CHECK(tar_find(a.p, a.len, "hemma-secret.txt", &p, &n));
    CHECK_INT((int)n, 20);
    CHECK(memcmp(p, "HEMMA_AUTH_SALT=abc\n", 20) == 0);
    CHECK(tar_find(a.p, a.len, "empty", &p, &n) && n == 0);
    CHECK(!tar_find(a.p, a.len, "missing", &p, &n));
    /* the header's own fields, as other tar programs read them */
    CHECK(memcmp(a.p + 257, "ustar\0" "00", 8) == 0);
    CHECK(memcmp(a.p + 124, "00000002734", 11) == 0); /* 1500 in octal */
    CHECK_INT(a.p[156], '0');
    buf_free(&a);
}

TEST(tar_refuses_broken_archives) {
    Buf a = {0};
    tar_add(&a, "one", "1", 1, 0, 0600);
    tar_add(&a, "two", "22", 2, 0, 0600);
    tar_end(&a);
    const void *p;
    size_t n;
    CHECK(!tar_looks("not a tar", 9));
    CHECK(!tar_find(a.p, 700, "two", &p, &n));          /* cut short */
    a.p[512 + 512 + 10] ^= 1;                             /* the second header's name: checksum off */
    CHECK(tar_find(a.p, a.len, "one", &p, &n));
    CHECK(!tar_find(a.p, a.len, "two", &p, &n));
    memcpy(a.p + 124, "77777777777", 11); /* a size past the end, with a checksum that adds up */
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : (unsigned char)a.p[i];
    snprintf(a.p + 148, 8, "%06o", sum);
    a.p[155] = ' ';
    CHECK(tar_looks(a.p, a.len));
    CHECK(!tar_find(a.p, a.len, "one", &p, &n));
    buf_free(&a);
}
