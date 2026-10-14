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

/* A channel goes on the air the moment it's made (issue #56: a half-hour channel used to round its
   start down to the last :00/:30, so a short first programme could be over already - colour bars
   until the next half hour). Lap 0's first airing is that LEAD-IN: the first programme from
   `epoch`, for its own length. The second starts at the first :00/:30 at or after it ends, and the
   rhythm goes on from there - tv_epoch() puts lap 0 where that works. Laps before the channel
   existed (a clock set back) end exactly at `epoch`. Back to back, all of this is the plain loop. */
int64_t tv_epoch(const TvPlan *p, int64_t epoch) {
    if (!p || !p->half_hour || p->n <= 0) return epoch;
    int64_t second = -floor_div(-(epoch + p->items[0].dur_s), TV_SLOT_S) * TV_SLOT_S; /* (rounded up) */
    return second - p->starts[1];
}

/* Where an airing sits: programme k of the lap that begins at base. Laps before the channel
   existed are counted from epoch (`before`); from lap 0 on, from tv_epoch(). */
typedef struct {
    int k;
    int64_t base;
    bool before;
} Pos;

static void airing(const TvPlan *p, int64_t epoch, int64_t e, Pos at, TvSlot *out) {
    int k = at.k;
    out->index = k;
    out->file = p->items[k].file;
    out->start = at.base + p->starts[k];
    out->next = at.base + p->starts[k + 1];
    if (!at.before && k == 0 && at.base == e) out->start = epoch; /* the lead-in (next: the slot's end) */
    out->end = out->start + p->items[k].dur_s;
    out->offset = 0;
    out->gap = false;
}

static Pos advance(const TvPlan *p, int64_t epoch, int64_t e, Pos at) {
    if (++at.k < p->n) return at;
    at.k = 0;
    at.base += p->cycle;
    if (at.before && at.base >= epoch) { /* (lands on epoch exactly) */
        at.before = false;
        at.base = e;
    }
    return at;
}

/* the airing going out at `now` */
static Pos position(const TvPlan *p, int64_t epoch, int64_t e, int64_t now) {
    Pos at = {0, 0, now < epoch};
    if (at.before) at.base = epoch + floor_div(now - epoch, p->cycle) * p->cycle;
    else if (now < e + p->starts[1]) return (Pos){0, e, false}; /* the lead-in, and its intermission */
    else at.base = e + floor_div(now - e, p->cycle) * p->cycle;
    int64_t pos = now - at.base; /* 0 <= pos < cycle */
    int lo = 0, hi = p->n - 1;
    while (lo < hi) { /* last airing that started at or before pos */
        int mid = lo + (hi - lo + 1) / 2;
        if (p->starts[mid] <= pos) lo = mid;
        else hi = mid - 1;
    }
    at.k = lo;
    return at;
}

bool tv_now(const TvPlan *p, int64_t epoch, int64_t now, TvSlot *out) {
    if (!p || p->n <= 0 || p->cycle <= 0) return false;
    int64_t e = tv_epoch(p, epoch);
    airing(p, epoch, e, position(p, epoch, e, now), out);
    out->gap = now >= out->end;
    out->offset = out->gap ? 0 : now - out->start;
    return true;
}

int tv_window(const TvPlan *p, int64_t epoch, int64_t from, int64_t to, TvSlot *out, int max) {
    if (max <= 0 || to <= from || !p || p->n <= 0 || p->cycle <= 0) return 0;
    int64_t e = tv_epoch(p, epoch);
    Pos at = position(p, epoch, e, from);
    int n = 0;
    while (n < max) {
        TvSlot x;
        airing(p, epoch, e, at, &x);
        if (x.start >= to) break;
        if (x.next > from) out[n++] = x;
        at = advance(p, epoch, e, at);
    }
    return n;
}
