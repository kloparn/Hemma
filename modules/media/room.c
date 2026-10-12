/* modules/media/room.c — room on the library's disks (see room.h). */
#include "room.h"

static int64_t g_free_override = -1;

void media_test_free_space(int64_t bytes) { g_free_override = bytes; }

static bool disk(const char *path, int64_t *free_bytes, int64_t *total) {
    if (g_free_override >= 0) { /* tests: one disk with this much free */
        *free_bytes = g_free_override;
        *total = 1;
        return true;
    }
    *total = 0;
    return path_disk_space(path, free_bytes, total);
}

bool media_disk_space(const char *path, int64_t *free_bytes, int64_t *total) { return disk(path, free_bytes, total); }

int64_t media_room(const char *root, const MediaHold *holds, int n, int64_t *held) {
    int64_t fr = 0, tot = 0, sum = 0;
    if (held) *held = 0;
    if (!disk(root, &fr, &tot)) return INT64_MAX / 4; /* can't tell: don't refuse */
    for (int i = 0; i < n; i++) {
        /* another folder on the same disk: the same size, and the same free space give or take
           what was written between the two looks (as the sidebar's storage meter tells them) */
        int64_t f2 = 0, t2 = 0;
        bool same = str_eq(holds[i].root, root) ||
                    (disk(holds[i].root, &f2, &t2) && t2 == tot && llabs(f2 - fr) < (64LL << 20));
        if (same) sum += MAX(holds[i].bytes, 0);
    }
    if (held) *held = sum;
    return fr - MEDIA_RESERVE - sum;
}

void media_size_text(int64_t n, char *out, size_t cap) {
    double v = (double)n;
    if (v >= 1099511627776.0) snprintf(out, cap, "%.1f TB", v / 1099511627776.0);
    else if (v >= 10737418240.0) snprintf(out, cap, "%.0f GB", v / 1073741824.0);
    else if (v >= 1073741824.0) snprintf(out, cap, "%.1f GB", v / 1073741824.0);
    else if (v >= 1048576.0) snprintf(out, cap, "%.0f MB", v / 1048576.0);
    else snprintf(out, cap, "%.0f KB", v / 1024.0);
}

void media_room_text(char *out, size_t cap, int64_t spare, int64_t held) {
    char s[24], h[24];
    media_size_text(MAX(spare, 0), s, sizeof s);
    media_size_text(held, h, sizeof h);
    if (held > 0)
        snprintf(out, cap, "the drive has %s to spare (Hemma keeps %lld GB free, and %s for the uploads and "
                           "conversions running now)", s, (long long)(MEDIA_RESERVE >> 30), h);
    else snprintf(out, cap, "the drive has %s to spare (Hemma keeps %lld GB free)", s, (long long)(MEDIA_RESERVE >> 30));
}
