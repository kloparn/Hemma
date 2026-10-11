/* modules/tv/tv_plan.c — the schedule math: pure, deterministic, no I/O. */
#include "tv.h"

int tv_rotate(const TvStream *streams, int nstreams, TvItem *out, int max) {
    int n = 0;
    for (int round = 0;; round++) {
        bool any = false;
        for (int i = 0; i < nstreams; i++) {
            if (round >= streams[i].n) continue;
            any = true;
            if (n < max) out[n++] = streams[i].items[round];
        }
        if (!any || n >= max) break;
    }
    return n;
}

/* splitmix64: tiny, well-mixed, and identical on every platform */
static uint64_t mix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void tv_shuffle(TvItem *items, int n, uint32_t seed) {
    uint64_t s = seed;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(mix(&s) % (uint64_t)(i + 1));
        TvItem t = items[i];
        items[i] = items[j];
        items[j] = t;
    }
}

void tv_plan_init(TvPlan *p, const TvItem *items, int n, bool half_hour) {
    memset(p, 0, sizeof *p);
    p->half_hour = half_hour;
    if (n <= 0) return;
    p->items = xmalloc((size_t)n * sizeof *items);
    p->starts = xmalloc((size_t)(n + 1) * sizeof *p->starts);
    int64_t at = 0;
    for (int i = 0; i < n; i++) {
        TvItem it = items[i];
        if (it.dur_s < 1) it.dur_s = 1;
        p->items[p->n] = it;
        p->starts[p->n] = at;
        int64_t slot = it.dur_s;
        if (half_hour) slot = (slot + TV_SLOT_S - 1) / TV_SLOT_S * TV_SLOT_S;
        at += slot;
        p->n++;
    }
    p->starts[p->n] = at;
    p->cycle = at;
}

void tv_plan_free(TvPlan *p) {
    free(p->items);
    free(p->starts);
    memset(p, 0, sizeof *p);
}

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

int64_t tv_epoch(const TvPlan *p, int64_t epoch) {
    /* Unix half-hours line up with local :00/:30 in every whole- or half-hour time zone */
    return p && p->half_hour ? floor_div(epoch, TV_SLOT_S) * TV_SLOT_S : epoch;
}

static void airing(const TvPlan *p, int k, int64_t base, TvSlot *out) {
    out->index = k;
    out->file = p->items[k].file;
    out->start = base + p->starts[k];
    out->end = out->start + p->items[k].dur_s;
    out->next = base + p->starts[k + 1];
    out->offset = 0;
    out->gap = false;
}

bool tv_now(const TvPlan *p, int64_t epoch, int64_t now, TvSlot *out) {
    if (!p || p->n <= 0 || p->cycle <= 0) return false;
    int64_t e = tv_epoch(p, epoch);
    int64_t laps = floor_div(now - e, p->cycle);
    int64_t base = e + laps * p->cycle;
    int64_t pos = now - base; /* 0 <= pos < cycle, also for epochs in the future */
    int lo = 0, hi = p->n - 1;
    while (lo < hi) { /* last airing that started at or before pos */
        int mid = lo + (hi - lo + 1) / 2;
        if (p->starts[mid] <= pos) lo = mid;
        else hi = mid - 1;
    }
    airing(p, lo, base, out);
    out->gap = now >= out->end;
    out->offset = out->gap ? 0 : now - out->start;
    return true;
}

int tv_window(const TvPlan *p, int64_t epoch, int64_t from, int64_t to, TvSlot *out, int max) {
    TvSlot s;
    if (max <= 0 || to <= from || !tv_now(p, epoch, from, &s)) return 0;
    int n = 0, k = s.index;
    int64_t base = s.start - p->starts[k];
    while (n < max) {
        TvSlot x;
        airing(p, k, base, &x);
        if (x.start >= to) break;
        if (x.next > from) out[n++] = x;
        if (++k == p->n) {
            k = 0;
            base += p->cycle;
        }
    }
    return n;
}
