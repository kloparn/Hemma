/* core/tar.c — see tar.h. A header: name[100] mode[8] uid[8] gid[8] size[12] mtime[12]
 * chksum[8] type[1] linkname[100] "ustar\0" "00" uname[32] gname[32] devmajor[8] devminor[8]
 * prefix[155], numbers in octal; the checksum is the sum of the header's bytes with its own
 * field read as spaces. */
#include "tar.h"

#define BLOCK 512

static unsigned checksum(const unsigned char *h) {
    unsigned sum = 0;
    for (int i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? ' ' : h[i];
    return sum;
}

void tar_add(Buf *out, const char *name, const void *data, size_t n, int64_t mtime, int mode) {
    unsigned char h[BLOCK];
    memset(h, 0, sizeof h);
    snprintf((char *)h, 100, "%s", name);
    snprintf((char *)h + 100, 8, "%07o", (unsigned)mode & 07777);
    snprintf((char *)h + 108, 8, "%07o", 0u);
    snprintf((char *)h + 116, 8, "%07o", 0u);
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)n & 077777777777ULL); /* (8 GB: far beyond a backup) */
    snprintf((char *)h + 136, 12, "%011llo", (unsigned long long)(mtime > 0 ? mtime : 0) & 077777777777ULL);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6); /* (with its NUL) */
    memcpy(h + 263, "00", 2);
    snprintf((char *)h + 265, 32, "hemma");
    snprintf((char *)h + 297, 32, "hemma");
    snprintf((char *)h + 148, 8, "%06o", checksum(h));
    h[155] = ' ';
    buf_put(out, h, sizeof h);
    buf_put(out, data, n);
    static const char zeros[BLOCK];
    if (n % BLOCK) buf_put(out, zeros, BLOCK - n % BLOCK);
}

void tar_end(Buf *out) {
    static const char zeros[2 * BLOCK];
    buf_put(out, zeros, sizeof zeros);
}

static bool octal(const unsigned char *p, int len, uint64_t *v) {
    int i = 0;
    *v = 0;
    while (i < len && p[i] == ' ') i++;
    if (i == len || p[i] < '0' || p[i] > '7') return false;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; i++) {
        if (*v >> 60) return false;
        *v = *v * 8 + (uint64_t)(p[i] - '0');
    }
    return true;
}

static bool header_ok(const unsigned char *h) {
    uint64_t want;
    return octal(h + 148, 8, &want) && want == checksum(h);
}

bool tar_looks(const void *data, size_t n) {
    return n >= BLOCK && header_ok(data) && memcmp((const char *)data + 257, "ustar", 5) == 0;
}

bool tar_find(const void *data, size_t n, const char *name, const void **p, size_t *len) {
    const unsigned char *a = data;
    size_t at = 0;
    while (at + BLOCK <= n) {
        const unsigned char *h = a + at;
        bool empty = true;
        for (int i = 0; i < BLOCK && empty; i++) empty = h[i] == 0;
        if (empty || !header_ok(h)) return false; /* (the end, or rubbish) */
        uint64_t size;
        if (!octal(h + 124, 12, &size) || size > n - at - BLOCK) return false;
        char full[300];
        if (h[345] && memcmp(h + 257, "ustar", 5) == 0)
            snprintf(full, sizeof full, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        else snprintf(full, sizeof full, "%.100s", (const char *)h);
        if ((h[156] == '0' || h[156] == 0) && str_eq(full, name)) {
            *p = h + BLOCK;
            *len = (size_t)size;
            return true;
        }
        size_t step = BLOCK + ((size_t)size + BLOCK - 1) / BLOCK * BLOCK;
        if (step > n - at) return false;
        at += step;
    }
    return false;
}
