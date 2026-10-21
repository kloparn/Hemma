/* modules/book/book.c — booking rules, quick add, actions and the module. */
#include "book.h"
#include "../../core/db.h"
#include "../../core/ui.h"

static const FieldDef res_fields[] = {
    FIELD(Resource, id, F_INT),          FIELD(Resource, name, F_STR),        FIELD(Resource, emoji, F_STR),
    FIELD(Resource, hue, F_INT),         FIELD(Resource, slot, F_INT),        FIELD(Resource, open_min, F_INT),
    FIELD(Resource, close_min, F_INT),   FIELD(Resource, default_min, F_INT), FIELD(Resource, max_min, F_INT),
    FIELD(Resource, rules, F_STR),       FIELD(Resource, archived, F_BOOL),   FIELD(Resource, sort, F_INT),
    FIELD(Resource, aliases, F_STR),     FIELD(Resource, own_only, F_BOOL),   FIELD(Resource, kids, F_BOOL),
    FIELD(Resource, guests, F_BOOL),
};
Table T_resource = TABLE("resource", Resource, res_fields);

static const FieldDef booking_fields[] = {
    FIELD(Booking, id, F_INT),     FIELD(Booking, res, F_INT),  FIELD(Booking, who, F_INT),
    FIELD(Booking, by, F_INT),     FIELD(Booking, start, F_I64), FIELD(Booking, end, F_I64),
    FIELD(Booking, note, F_STR),   FIELD(Booking, created, F_I64), FIELD(Booking, series, F_INT),
};
Table T_booking = TABLE("booking", Booking, booking_fields);

static const FieldDef series_fields[] = {
    FIELD(BookSeries, id, F_INT),       FIELD(BookSeries, res, F_INT),    FIELD(BookSeries, who, F_INT),
    FIELD(BookSeries, by, F_INT),       FIELD(BookSeries, start, F_I64),  FIELD(BookSeries, end, F_I64),
    FIELD(BookSeries, repeat, F_INT),   FIELD(BookSeries, until, F_I64),  FIELD(BookSeries, made, F_INT),
    FIELD(BookSeries, note, F_STR),     FIELD(BookSeries, created, F_I64), FIELD(BookSeries, stopped, F_BOOL),
};
Table T_book_series = TABLE("booking_series", BookSeries, series_fields);

static const FieldDef news_fields[] = {
    FIELD(BookNews, id, F_INT),     FIELD(BookNews, person, F_INT), FIELD(BookNews, ts, F_I64),
    FIELD(BookNews, text, F_STR),   FIELD(BookNews, link, F_STR),   FIELD(BookNews, msg, F_STR),
};
Table T_book_news = TABLE("book_news", BookNews, news_fields);

#define NEWS_DAYS 30 /* a change nobody came to see is old news after this */
#define NEWS_MAX 20  /* per person: the oldest give way */

bool book_may_change(const Ctx *c, const Resource *r, const Booking *b) {
    if (!c->me || c->me->role == ROLE_DISPLAY) return false;
    if (is_child(c) || is_guest(c)) return c->me->id == b->who || c->me->id == b->by; /* (their own: issue #44) */
    if (!r || !r->own_only) return true;
    return c->me->id == b->who || c->me->id == b->by || c->me->role == ROLE_ADMIN;
}

bool book_open_to(const Ctx *c, const Resource *r) {
    if (!r) return false;
    if (is_child(c)) return r->kids;
    if (is_guest(c)) return r->guests;
    return true;
}

/* ================================================================ lookups */

Resource *res_get(int id) { return id > 0 ? tbl_get(&T_resource, id) : NULL; }
Booking *book_get(int id) { return id > 0 ? tbl_get(&T_booking, id) : NULL; }

static int res_order(const void *a, const void *b) {
    const Resource *x = *(Resource *const *)a, *y = *(Resource *const *)b;
    if (x->sort != y->sort) return x->sort - y->sort;
    return x->id - y->id;
}

int res_list(Resource **out, int max, bool with_archived) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_resource) && n < max; i++) {
        Resource *r = tbl_at(&T_resource, i);
        if (with_archived || !r->archived) out[n++] = r;
    }
    qsort(out, (size_t)n, sizeof *out, res_order);
    return n;
}

int book_res_list(const Ctx *c, Resource **out, int max) {
    int n = res_list(out, max, false), k = 0;
    for (int i = 0; i < n; i++)
        if (book_open_to(c, out[i])) out[k++] = out[i];
    return k;
}

Booking *book_conflict(int res, int64_t start, int64_t end, int except) {
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *b = tbl_at(&T_booking, i);
        if (b->res == res && b->id != except && b->start < end && start < b->end) return b;
    }
    return NULL;
}

Booking *book_current(int res, int64_t t) { return book_conflict(res, t, t + 1, 0); }

Booking *book_next(int res, int64_t t) {
    Booking *best = NULL;
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *b = tbl_at(&T_booking, i);
        if (b->res == res && b->start > t && (!best || b->start < best->start)) best = b;
    }
    return best;
}

/* ================================================================== rules */

static bool around_the_clock(const Resource *r) { return r->open_min <= 0 && r->close_min >= 1440; }

static int64_t step_of(const Resource *r) { return res_daily(r) ? 86400 : (int64_t)MAX(r->slot, 5) * 60; }

static int64_t snap_down(const Resource *r, int64_t t) {
    int64_t d0 = day_start(t);
    if (res_daily(r)) return d0;
    int64_t st = step_of(r);
    return d0 + ((t - d0) / st) * st;
}

static int64_t snap_up(const Resource *r, int64_t t) {
    int64_t d = snap_down(r, t);
    if (d == t) return t;
    return res_daily(r) ? add_days(d, 1) : d + step_of(r);
}

void book_snap(const Resource *r, int64_t *start, int64_t *end) {
    *start = snap_down(r, *start);
    *end = snap_up(r, *end);
    if (*end <= *start) *end = res_daily(r) ? add_days(*start, 1) : *start + step_of(r);
}

void book_day(Buf *b, int64_t day, int64_t now) {
    if (i18n_lang() == LANG_EN) ui_day(b, day, now);
    else ui_day_inline(b, day, now);
}

/* "Friday 14:00–17:00", "Fri 16 Oct – Sun 18 Oct" - mostly said inside a sentence (a toast, a reason, news) */
void book_span_text(const Resource *r, int64_t start, int64_t end, int64_t now, Buf *b) {
    /* a whole day of an hourly thing open around the clock (no thing: as if it were) */
    bool whole = start == day_start(start) && end == add_days(start, 1);
    if (whole && (!r || (!res_daily(r) && around_the_clock(r)))) {
        Buf day = {0};
        book_day(&day, start, now);
        buf_printf(b, T("%s, all day"), buf_cstr(&day));
        buf_free(&day);
        return;
    }
    book_day(b, start, now);
    if (r && res_daily(r)) {
        int64_t last = add_days(end, -1);
        if (day_start(last) != day_start(start)) {
            buf_puts(b, " – ");
            book_day(b, last, now);
        }
        return;
    }
    buf_putc(b, ' ');
    if (day_start(end - 1) == day_start(start)) {
        ui_hm(b, start);
        buf_puts(b, "–");
        DateTime e = dt_local(end);
        if (day_start(end) != day_start(start) && e.hour == 0 && e.min == 0) buf_puts(b, "24:00");
        else ui_hm(b, end);
    } else {
        ui_hm(b, start);
        buf_puts(b, " – ");
        book_day(b, end, now);
        buf_putc(b, ' ');
        ui_hm(b, end);
    }
}

/* Everything except "is it in the past". (A reason in the language in use: a request's, or - for a
   repeating booking's left-out one - the person's it's for.) */
static const char *check_rules(const Resource *r, int64_t s, int64_t e, int except, int64_t now) {
    static char msg[200];
    if (e <= s) return T("The end has to be after the start");
    if (!res_daily(r) && !around_the_clock(r)) {
        DateTime ds = dt_local(s), de = dt_local(e);
        int sm = ds.hour * 60 + ds.min;
        int em = (day_start(e) != day_start(s)) ? 1440 + de.hour * 60 + de.min : de.hour * 60 + de.min;
        if (day_start(e - 1) != day_start(s) || sm < r->open_min || em > r->close_min) {
            snprintf(msg, sizeof msg, T("%s can be booked %02d:%02d–%02d:%02d"), r->name, r->open_min / 60,
                     r->open_min % 60, r->close_min / 60, r->close_min % 60);
            return msg;
        }
    }
    if (r->max_min > 0 && e - s > (int64_t)r->max_min * 60) {
        Buf d = {0};
        ui_duration(&d, (int64_t)r->max_min * 60);
        snprintf(msg, sizeof msg, T("%s can be booked for at most %s"), r->name, buf_cstr(&d));
        buf_free(&d);
        return msg;
    }
    Booking *cb = book_conflict(r->id, s, e, except);
    if (cb) {
        Person *p = person_get(cb->who);
        Buf span = {0};
        book_span_text(r, cb->start, cb->end, now, &span);
        snprintf(msg, sizeof msg, T("%s has %s %s"), p ? p->name : T("Someone"), r->name, buf_cstr(&span));
        buf_free(&span);
        return msg;
    }
    return NULL;
}

const char *book_check(const Resource *r, int64_t s, int64_t e, int except, int64_t now) {
    if (!r) return T("Pick something to book");
    if (r->archived) return T("That can't be booked any more");
    if (e <= s) return T("The end has to be after the start");
    if (e <= now) return T("That's already over");
    int64_t grace = res_daily(r) ? now - day_start(now) : step_of(r);
    /* a booking under way may still be extended or ended early, as long as its start stays put */
    Booking *own = except ? book_get(except) : NULL;
    bool same_start = own && own->res == r->id && own->start == s;
    if (s < now - grace && !same_start) return T("That has already started — pick a later time");
    return check_rules(r, s, e, except, now);
}

bool book_next_free(const Resource *r, int64_t from, int64_t len, int except, int64_t *start) {
    /* whole-day things can still be booked for today; hourly ones from the next slot */
    int64_t t = res_daily(r) ? snap_down(r, from) : snap_up(r, from);
    int days = res_daily(r) ? (int)MAX(1, (len + 43200) / 86400) : 0;
    int64_t limit = add_days(from, 60);
    for (int guard = 0; guard < 20000 && t < limit; guard++) {
        int64_t e = res_daily(r) ? add_days(t, days) : t + len;
        Booking *cb = book_conflict(r->id, t, e, except);
        if (cb) {
            t = snap_up(r, cb->end); /* jump past whatever is in the way */
            continue;
        }
        if (!check_rules(r, t, e, except, from)) {
            *start = t;
            return true;
        }
        if (!res_daily(r) && !around_the_clock(r)) {
            /* outside opening hours: go to the next opening time — on the wall clock, so the
               days the clocks change open at 07:00 too, not an hour off */
            DateTime d = dt_local(t);
            int64_t open_today = dt_make(d.year, d.mon, d.day, 0, r->open_min);
            t = t < open_today ? open_today : dt_make(d.year, d.mon, d.day + 1, 0, r->open_min);
        } else {
            t = res_daily(r) ? add_days(t, 1) : t + step_of(r);
        }
    }
    return false;
}

/* ============================================================ quick add */

static size_t common_prefix(const char *a, const char *b) {
    size_t n = 0;
    while (a[n] && a[n] == b[n]) n++;
    while (n > 0 && ((unsigned char)a[n] & 0xC0) == 0x80) n--; /* whole characters only */
    return n;
}

typedef struct {
    int id, score, from, len;
} Match;

/* One name a thing goes by against the unused tokens. Multi-word names ("guest room") must match
   consecutive tokens (or one token with a dash: "tv-rum"); single words may be abbreviated
   ("laund", "tvätt" for "Tvättstuga", "bilen" for "Bil"). */
static void match_name(const Tokens *t, int id, const char *name, Match *m) {
    char whole[64], low[64];
    fold_copy(whole, sizeof whole, name);
    str_trim(whole);
    str_copy(low, sizeof low, whole);
    char words[6][32];
    int nw = 0;
    for (char *p = strtok(low, " -"); p && nw < 6; p = strtok(NULL, " -")) str_copy(words[nw++], 32, p);
    if (!nw) return;
    for (int i = 0; i < t->n; i++) {
        if (t->used[i]) continue;
        /* exact multi-word match */
        bool all = i + nw <= t->n;
        for (int k = 0; k < nw && all; k++) all = !t->used[i + k] && str_eq(t->low[i + k], words[k]);
        if (all && 50 + nw * 10 > m->score) *m = (Match){id, 50 + nw * 10, i, nw};
        if (nw > 1 && str_eq(t->low[i], whole) && 50 + nw * 10 > m->score) *m = (Match){id, 50 + nw * 10, i, 1};
        /* one word against the name's first word: prefix either way, >= 3 characters */
        size_t cp = common_prefix(t->low[i], words[0]);
        size_t tl = strlen(t->low[i]), wl = strlen(words[0]);
        bool prefix = cp >= 3 && (cp == tl || cp == wl || cp >= 5);
        int score = prefix ? (int)(10 + cp) : 0;
        if (score > m->score) *m = (Match){id, score, i, 1};
    }
}

/* Each name in a comma-separated list ("bastu, bastun"). */
static void match_names(const Tokens *t, int id, const char *list, Match *m) {
    char one[64];
    for (const char *p = list; p && *p;) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        str_ncopy(one, sizeof one, p, n);
        match_name(t, id, one, m);
        p = comma ? comma + 1 : NULL;
    }
}

static void match_starter(const Tokens *t, int id, const char *name, Match *m);

/* Find the resource named in the tokens - by its emoji, its name, its other names, and the starter it's
   named like: its name in every language and its Swedish words ("Sauna": "bastu") - and mark them used. */
static int match_resource(Tokens *t) {
    Match m = {0};
    for (int ri = 0; ri < tbl_count(&T_resource); ri++) {
        Resource *r = tbl_at(&T_resource, ri);
        if (r->archived) continue;
        for (int i = 0; i < t->n; i++)
            if (!t->used[i] && r->emoji[0] && str_eq(t->raw[i], r->emoji) && 100 > m.score) m = (Match){r->id, 100, i, 1};
        match_name(t, r->id, r->name, &m);
        match_names(t, r->id, r->aliases, &m);
        match_starter(t, r->id, r->name, &m);
    }
    for (int k = 0; m.id && k < m.len; k++) t->used[m.from + k] = true;
    return m.id;
}

/* "You can book: Car, Laundry, Sauna." - the bookable things, for a word that named none of them. */
static void bookable_list(char *out, size_t cap) {
    Resource *rs[8];
    int n = res_list(rs, countof(rs), false), total = 0;
    for (int i = 0; i < tbl_count(&T_resource); i++) total += !((const Resource *)tbl_at(&T_resource, i))->archived;
    char names[400] = "";
    size_t at = 0;
    for (int i = 0; i < n && at < sizeof names; i++)
        at += (size_t)snprintf(names + at, sizeof names - at, "%s%s", i ? ", " : "", rs[i]->name);
    snprintf(out, cap, total > n ? T("You can book: %s …") : T("You can book: %s."), names);
}

static void take_person(Tokens *t, int *who) {
    for (int i = 0; i < t->n; i++) {
        if (t->used[i]) continue;
        const char *w = t->low[i];
        if (w[0] == '@' && w[1]) {
            Person *p = person_find(t->raw[i] + 1);
            if (p) *who = p->id, t->used[i] = true;
        } else if ((str_eq(w, "for") || str_eq(w, "för") || str_eq(w, "åt")) && i + 1 < t->n && !t->used[i + 1]) {
            Person *p = strlen(t->low[i + 1]) >= 2 ? person_find(t->raw[i + 1]) : NULL;
            if (p && str_ieq(p->name, t->raw[i + 1])) *who = p->id, t->used[i] = t->used[i + 1] = true;
        }
    }
}

void book_quick_parse(const char *q, int default_res, int64_t now, Quick *out) {
    memset(out, 0, sizeof *out);
    if (str_blank(q)) return;
    Tokens t;
    tok_split(&t, q);
    out->res = match_resource(&t);
    if (!out->res) out->res = default_res;
    take_person(&t, &out->who);
    When w;
    out->has_when = when_parse(&t, now, &w);
    if (out->has_when) out->repeat = w.repeat; /* "every tuesday": the next Tuesday, then every one */
    tok_rest(&t, out->note, sizeof out->note);
    if (str_istarts(out->note, "for ")) memmove(out->note, out->note + 4, strlen(out->note + 4) + 1);
    else if (str_istarts(out->note, "för ")) memmove(out->note, out->note + 5, strlen(out->note + 5) + 1);

    Resource *r = res_get(out->res);
    if (!r || r->archived) {
        Resource *first[1];
        int n = res_list(first, 1, false);
        char word[TOK_LEN] = "";
        for (int i = 0; i < t.n && !word[0]; i++) /* the first word nothing took: what they meant to book */
            if (!t.used[i]) str_copy(word, sizeof word, t.raw[i]);
        if (word[0] && n) { /* issue #55: "bastu" met a thing called "Sauna" and was only told to start with one */
            char list[400], msg[512]; /* (room for 8 names of 40 bytes: only str_copy cuts, never mid-letter) */
            bookable_list(list, sizeof list);
            snprintf(msg, sizeof msg, T("I don't know “%s”. %s"), word, list);
            str_copy(out->problem, sizeof out->problem, msg);
        } else {
            snprintf(out->problem, sizeof out->problem, T("Start with what to book, e.g. “%s fri 14-17”"),
                     n ? first[0]->name : T("car"));
        }
        out->res = 0;
        return;
    }
    if (!out->has_when && w.refused[0]) { /* "31 sep", an end before the start: say why */
        str_copy(out->problem, sizeof out->problem, w.refused);
        return;
    }
    if (!out->has_when) {
        snprintf(out->problem, sizeof out->problem,
                 res_daily(r) ? T("When? Add a day, e.g. “%s fri-sun”") : T("When? Add a day and a time, e.g. “%s fri 14-17”"),
                 r->name);
        return;
    }
    int64_t s, e;
    bool timed = w.has_start || w.now || w.rel_min;
    if (!res_daily(r) && !timed && !around_the_clock(r)) {
        snprintf(out->problem, sizeof out->problem, T("What time? e.g. “%s sat 10-13”"), r->name);
        return;
    }
    int dflt = r->default_min > 0 ? r->default_min : (res_daily(r) ? 1440 : 60);
    if (!when_resolve(&w, now, dflt, &s, &e)) { /* never with has_when - but s and e would be garbage */
        snprintf(out->problem, sizeof out->problem, T("When? e.g. “%s fri 14-17”"), r->name);
        return;
    }
    if (!res_daily(r) && !timed) { /* around-the-clock thing for a whole day/span */
        s = day_start(s);
        if (e - s < 86400) e = add_days(s, 1);
    }
    if (res_daily(r) && !w.has_end_date && !w.dur_min) e = add_days(day_start(s), 1);
    book_snap(r, &s, &e);
    out->start = s;
    out->end = e;
    const char *why = book_check(r, s, e, 0, now);
    if (why) {
        str_copy(out->problem, sizeof out->problem, why);
        int64_t alt;
        if (book_conflict(r->id, s, e, 0) && book_next_free(r, MAX(s, now), e - s, 0, &alt)) {
            out->has_alt = true;
            out->alt_start = alt;
            out->alt_end = res_daily(r) ? add_days(alt, (int)((e - s + 43200) / 86400)) : alt + (e - s);
        }
    }
}

/* ================================================================ actions */

/* The language a line for someone is made in (the days and reasons inside it): theirs when they chose one,
   else the one in use (issue #46). */
static Lang lang_of(int person) {
    Lang l;
    return i18n_parse(pref(person, PREF_LANG, ""), &l) ? l : i18n_lang();
}

/* Keep a line for someone whose booking changed (and toast it on their open screens): fmt is English, marked
   N_(), with up to five values (texts; NULL past the format's own) - kept with them, so each reader gets it in
   their language. */
static void news_add(int person, const char *link, const char *fmt, const char *v1, const char *v2, const char *v3,
                     const char *v4, const char *v5) {
    int64_t now = now_sec();
    int mine = 0, oldest = 0;
    int64_t oldest_ts = INT64_MAX;
    for (int i = tbl_count(&T_book_news) - 1; i >= 0; i--) {
        BookNews *n = tbl_at(&T_book_news, i);
        if (n->ts < now - (int64_t)NEWS_DAYS * 86400) {
            tbl_del(&T_book_news, n->id);
            continue;
        }
        if (n->person != person) continue;
        mine++;
        if (n->ts < oldest_ts) oldest_ts = n->ts, oldest = n->id;
    }
    if (mine >= NEWS_MAX && oldest) tbl_del(&T_book_news, oldest);
    BookNews n = {.person = person, .ts = now};
    snprintf(n.text, sizeof n.text, fmt, v1, v2, v3, v4, v5); /* (printf leaves extra values alone) */
    if (!i18n_pack(n.msg, sizeof n.msg, fmt, v1, v2, v3, v4, v5)) n.msg[0] = '\0'; /* too long: the text as it is */
    str_copy(n.link, sizeof n.link, link);
    tbl_put(&T_book_news, &n);
    app_notify_person(person, TOAST_INFO, fmt, v1, v2, v3, v4, v5);
}

/* ======================================================= repeating bookings */

static int64_t month_shift(int64_t t, int months) {
    DateTime d = dt_local(t);
    int y = d.year, m = d.mon - 1 + months;
    y += m / 12;
    m = m % 12 + 1;
    return dt_make(y, m, MIN(d.day, days_in_month(y, m)), d.hour, d.min);
}

int64_t book_series_at(const BookSeries *s, int k, int64_t *end) {
    if (s->repeat == REPEAT_MONTHLY) {
        int64_t a = month_shift(s->start, k);
        *end = a + (s->end - s->start);
        return a;
    }
    int step = s->repeat == REPEAT_DAILY ? 1 : s->repeat == REPEAT_BIWEEKLY ? 14 : 7;
    *end = add_days(s->end, step * k); /* (the same clock times, summer or winter) */
    return add_days(s->start, step * k);
}

const BookSeries *book_series_of(const Booking *b) { return b->series ? tbl_get(&T_book_series, b->series) : NULL; }

void book_series_label(Buf *b, const BookSeries *s) {
    int wday = dt_local(s->start).wday;
    switch (s->repeat) {
    case REPEAT_DAILY: buf_puts(b, T("every day")); break;
    case REPEAT_WEEKLY: buf_printf(b, T("every %s"), weekday_name(wday, false)); break;
    case REPEAT_BIWEEKLY: buf_printf(b, T("every other %s"), weekday_name(wday, false)); break;
    case REPEAT_MONTHLY: buf_printf(b, T("every month on the %d"), dt_local(s->start).day); break;
    default: break;
    }
    if (s->stopped) buf_puts(b, T(", stopped"));
    else if (s->until) {
        DateTime u = dt_local(s->until);
        buf_printf(b, T(", until %d %s"), u.day, month_name(u.mon, true));
    }
}

int book_series_fill(int64_t now) {
    int made = 0;
    for (int i = 0; i < tbl_count(&T_book_series); i++) {
        BookSeries s = *(BookSeries *)tbl_at(&T_book_series, i);
        if (s.stopped || s.repeat == REPEAT_NONE) continue;
        Resource *r = res_get(s.res);
        if (!r || r->archived || !person_real(s.who)) { /* the thing or the person is gone: so is the series */
            s.stopped = true;
            tbl_put(&T_book_series, &s);
            continue;
        }
        bool changed = false;
        int64_t horizon = add_days(day_start(now), SERIES_AHEAD_DAYS);
        for (int guard = 0; guard < 500; guard++) {
            int64_t e, st = book_series_at(&s, s.made, &e);
            if (s.until && day_start(st) > s.until) {
                s.stopped = true; /* (its last one is made) */
                changed = true;
                break;
            }
            if (st >= horizon) break;
            s.made++;
            changed = true;
            if (e <= now) continue; /* (time that passed while Hemma was off: nothing to book) */
            if (book_check(r, st, e, 0, now)) { /* taken (or outside the hours now): left out, and whoever it's for hears */
                Lang was = i18n_use(lang_of(s.who)); /* (the day, the repeat and the reason in their words) */
                char why[200];
                str_copy(why, sizeof why, book_check(r, st, e, 0, now));
                Buf span = {0}, rep = {0}, link = {0};
                book_span_text(r, st, e, now, &span);
                book_series_label(&rep, &s);
                i18n_use(was);
                char ymd[11];
                fmt_ymd(st, ymd);
                buf_printf(&link, "/book/%d?w=%s", r->id, ymd);
                news_add(s.who, buf_cstr(&link), N_("Your %s %s: %s isn't booked - %s"), r->name, buf_cstr(&rep),
                         buf_cstr(&span), why, NULL);
                buf_free(&span);
                buf_free(&rep);
                buf_free(&link);
                continue;
            }
            Booking b = {.res = s.res, .who = s.who, .by = s.by, .start = st, .end = e, .created = now, .series = s.id};
            str_copy(b.note, sizeof b.note, s.note);
            tbl_put(&T_booking, &b);
            made++;
        }
        if (changed) tbl_put(&T_book_series, &s);
    }
    return made;
}

/* What happened to a booking - each with its own words (word order differs between languages) */
enum { DID_BOOK, DID_MOVE, DID_CANCEL, DID_STOP };

/* To the person it was for, to whoever made it for them, and the same when they made it for someone else -
   a moved one says where it went and where it was. */
static const char *const TELL[3][3] = {
    {N_("%s moved your %s booking · now %s (was %s)"), N_("%s moved the %s booking you made · now %s (was %s)"),
     N_("%s moved the %s booking you made for %s · now %s (was %s)")},
    {N_("%s cancelled your %s booking · %s"), N_("%s cancelled the %s booking you made · %s"),
     N_("%s cancelled the %s booking you made for %s · %s")},
    {N_("%s stopped repeating your %s booking · %s"), N_("%s stopped repeating the %s booking you made · %s"),
     N_("%s stopped repeating the %s booking you made for %s · %s")},
};

/* Someone else cancelled or moved a booking (issue #43): the person it was for and whoever made it
   hear about it - a toast now, and a line that waits on their Bookings page and home card. Who was
   told this way is in *told (0 = nobody), so the household's toast leaves them out. */
static void tell(Ctx *c, int did, const Resource *r, const Booking *was, const Booking *now_b, int told[2]) {
    told[0] = told[1] = 0;
    int me = c->me ? c->me->id : 0;
    const char *name = c->me ? c->me->name : T("Someone");
    Buf old = {0}, nw = {0}, link = {0};
    char ymd[11];
    fmt_ymd(now_b ? now_b->start : was->start, ymd);
    buf_printf(&link, "/book/%d?w=%s", r->id, ymd);
    int who[2] = {was->who, was->by != was->who ? was->by : 0};
    for (int k = 0; k < 2; k++) {
        Person *p = person_real(who[k]);
        if (!p || p->id == me) continue;
        Lang lang = i18n_use(lang_of(p->id)); /* (the days in their words) */
        buf_reset(&old);
        buf_reset(&nw);
        book_span_text(r, was->start, was->end, c->now, &old);
        if (now_b) book_span_text(r, now_b->start, now_b->end, c->now, &nw);
        i18n_use(lang);
        Person *for_p = person_get(was->who);
        int form = k == 0 ? 0 : me == was->who || !for_p ? 1 : 2;
        const char *v[5] = {name, r->name, NULL, NULL, NULL};
        int n = 2;
        if (form == 2) v[n++] = for_p->name;
        if (now_b) v[n++] = buf_cstr(&nw);
        v[n++] = buf_cstr(&old);
        news_add(p->id, buf_cstr(&link), TELL[did - DID_MOVE][form], v[0], v[1], v[2], v[3], v[4]);
        told[k] = p->id;
    }
    buf_free(&old);
    buf_free(&nw);
    buf_free(&link);
}

/* The household's Latest line: for someone else, or for themselves; and everyone's toast (the thing's emoji too) */
static const char *const ANNOUNCE_FOR[4] = {N_("%s booked %s for %s · %s"), N_("%s moved %s for %s · %s"),
                                            N_("%s cancelled %s for %s · %s"), N_("%s stopped repeating %s for %s · %s")};
static const char *const ANNOUNCE[4] = {N_("%s booked %s · %s"), N_("%s moved %s · %s"), N_("%s cancelled %s · %s"),
                                        N_("%s stopped repeating %s · %s")};
static const char *const ANNOUNCE_ALL[4] = {N_("%s booked %s %s · %s"), N_("%s moved %s %s · %s"),
                                            N_("%s cancelled %s %s · %s"), N_("%s stopped repeating %s %s · %s")};

static void announce(Ctx *c, int did, const Resource *r, const Booking *b, const int told[2]) {
    Person *who = person_get(b->who);
    Buf span = {0}, link = {0};
    book_span_text(r, b->start, b->end, c->now, &span);
    const BookSeries *series = book_series_of(b);
    if (series && did == DID_BOOK) {
        buf_puts(&span, ", ");
        book_series_label(&span, series);
    }
    buf_printf(&link, "/book/%d?w=", r->id);
    char ymd[11];
    fmt_ymd(b->start, ymd);
    buf_puts(&link, ymd);
    const char *me = c->me ? c->me->name : T("Someone");
    if (who && c->me && who->id != c->me->id)
        activity_add(c->me->id, r->emoji[0] ? r->emoji : "📅", buf_cstr(&link), ANNOUNCE_FOR[did], me, r->name, who->name,
                     buf_cstr(&span));
    else
        activity_add(c->me ? c->me->id : 0, r->emoji[0] ? r->emoji : "📅", buf_cstr(&link), ANNOUNCE[did], me, r->name,
                     buf_cstr(&span));
    app_notify_but(c, told ? told[0] : 0, told ? told[1] : 0, TOAST_INFO, ANNOUNCE_ALL[did], me, r->emoji, r->name,
                   buf_cstr(&span));
    buf_free(&span);
    buf_free(&link);
}

/* "Only Eva, Adam or an admin can cancel this booking" - false (and said) when the thing's rule keeps c out. */
static bool may_change_or_say(Ctx *c, const Resource *r, const Booking *b, bool cancel) {
    if (book_may_change(c, r, b)) return true;
    Person *who = person_get(b->who), *by = person_get(b->by);
    const char *whose = who ? who->name : T("the person it's for");
    if (by && by != who)
        ctx_fail(c,
                 cancel ? T("Only %s, %s or an admin can cancel this booking")
                        : T("Only %s, %s or an admin can move this booking"),
                 whose, by->name);
    else
        ctx_fail(c,
                 cancel ? T("Only %s or an admin can cancel this booking") : T("Only %s or an admin can move this booking"),
                 whose);
    return false;
}

static bool save_booking(Ctx *c, Resource *r, int id, int who, int64_t s, int64_t e, const char *note, int repeat,
                         int64_t until) {
    if (r && !book_open_to(c, r)) { /* children and guests: what an admin opened to them (issue #44) */
        if (is_child(c)) ctx_fail(c, T("Ask a grown-up to book the %s"), r->name);
        else ctx_fail(c, T("The %s isn't open to guests"), r->name);
        return false;
    }
    if ((is_child(c) || is_guest(c)) && who && who != c->me->id) {
        ctx_fail(c, "%s", T("You can book for yourself"));
        return false;
    }
    const char *why = book_check(r, s, e, id, c->now);
    if (why) {
        ctx_fail(c, "%s", why);
        return false;
    }
    Booking b = {0}, was = {0};
    if (id) {
        Booking *old = book_get(id);
        if (!old) {
            ctx_fail(c, "%s", T("That booking is gone"));
            return false;
        }
        if (!may_change_or_say(c, res_get(old->res), old, false)) return false;
        b = was = *old;
    } else {
        b.by = c->me ? c->me->id : 0;
        b.created = c->now;
    }
    b.res = r->id;
    if (!id || who) b.who = person_real(who) ? who : (c->me ? c->me->id : 0); /* pickers list real people only */
    b.start = s;
    b.end = e;
    str_copy(b.note, sizeof b.note, note ? note : "");
    str_trim(b.note);
    if (!id && repeat) { /* issue #47: this one first, then the ones ahead */
        BookSeries ser = {.res = r->id, .who = b.who, .by = b.by, .start = s, .end = e, .repeat = repeat,
                          .until = until, .made = 1, .created = c->now};
        str_copy(ser.note, sizeof ser.note, b.note);
        tbl_put(&T_book_series, &ser);
        b.series = ser.id;
    }
    tbl_put(&T_booking, &b);
    if (b.series) book_series_fill(c->now);
    Buf span = {0};
    book_span_text(r, s, e, c->now, &span);
    const BookSeries *ser = book_series_of(&b);
    if (ser) {
        Buf rep = {0};
        book_series_label(&rep, ser);
        ctx_ok(c, T("Booked %s · %s, %s - the next %d weeks are booked, later ones as they come"), r->name,
               buf_cstr(&span), buf_cstr(&rep), SERIES_AHEAD_DAYS / 7);
        buf_free(&rep);
    } else {
        ctx_ok(c, id ? T("Moved %s · %s") : T("Booked %s · %s"), r->name, buf_cstr(&span));
    }
    buf_free(&span);
    int told[2] = {0, 0};
    if (id && (was.start != b.start || was.end != b.end || was.res != b.res)) tell(c, DID_MOVE, r, &was, &b, told);
    announce(c, id ? DID_MOVE : DID_BOOK, r, &b, told);
    return true;
}

static void act_preview(Ctx *c, Fields *f) { ctx_set_live(c, "book.q", fields_str(f, "q")); }

static void act_quick(Ctx *c, Fields *f) {
    Quick q;
    book_quick_parse(fields_str(f, "q"), (int)fields_int(f, "res", 0), c->now, &q);
    if (!q.has_when && !q.res && str_blank(fields_str(f, "q"))) {
        ctx_fail(c, "%s", T("Type what to book, e.g. “car fri 14-17”"));
        return;
    }
    if (q.problem[0]) {
        ctx_fail(c, "%s", q.problem);
        return;
    }
    if (save_booking(c, res_get(q.res), 0, q.who, q.start, q.end, q.note, q.repeat, 0)) ctx_set_live(c, "book.q", "");
}

/* Accepts either explicit epochs (start/end) or form fields:
 * hourly: date + from + to;  daily: date + date_to (last day, inclusive).
 * Either way the span is snapped to the resource's grid (10:07 becomes 10:00),
 * except a booking's own start while it's being changed: one under way keeps it. */
static bool read_span(Ctx *c, Fields *f, Resource *r, int64_t *s, int64_t *e) {
    long long es = fields_int(f, "start", 0), ee = fields_int(f, "end", 0);
    if (es > 0 && ee > 0) {
        *s = es;
        *e = ee;
    } else {
        int y, m, d;
        if (!parse_ymd(fields_str(f, "date"), &y, &m, &d)) {
            ctx_fail_at(c, "date", "%s", T("Pick a day"));
            return false;
        }
        if (res_daily(r)) {
            int y2 = y, m2 = m, d2 = d;
            const char *to = fields_str(f, "date_to");
            if (*to && !parse_ymd(to, &y2, &m2, &d2)) {
                ctx_fail_at(c, "date_to", "%s", T("That end date doesn't look right"));
                return false;
            }
            *s = dt_make(y, m, d, 0, 0);
            *e = dt_make(y2, m2, d2 + 1, 0, 0);
        } else {
            int a, b;
            if (!parse_hm(fields_str(f, "from"), &a) || !parse_hm(fields_str(f, "to"), &b)) {
                ctx_fail_at(c, "from", "%s", T("Pick a start and an end time"));
                return false;
            }
            *s = dt_make(y, m, d, a / 60, a % 60);
            *e = dt_make(y, m, d + (b <= a ? 1 : 0), b / 60, b % 60);
        }
    }
    Booking *own = book_get((int)fields_int(f, "id", 0));
    bool keep = own && own->res == r->id && own->start == *s;
    book_snap(r, s, e);
    if (keep) *s = own->start;
    return true;
}

static void act_create(Ctx *c, Fields *f) {
    Resource *r = res_get((int)fields_int(f, "res", 0));
    if (!r) {
        ctx_fail(c, "%s", T("Pick something to book"));
        return;
    }
    int64_t s, e;
    if (!read_span(c, f, r, &s, &e)) return;
    int id = (int)fields_int(f, "id", 0);
    int rep = id ? 0 : (int)CLAMP(fields_int(f, "rep", 0), REPEAT_NONE, REPEAT_MONTHLY);
    int64_t until = 0;
    if (rep && fields_str(f, "rep_until")[0]) {
        int y, m, d;
        if (!parse_ymd(fields_str(f, "rep_until"), &y, &m, &d) || dt_make(y, m, d, 0, 0) < day_start(s)) {
            ctx_fail_at(c, "rep_until", "%s", T("Repeat until a day on or after the first one"));
            return;
        }
        until = dt_make(y, m, d, 0, 0);
    }
    if (save_booking(c, r, id, (int)fields_int(f, "who", 0), s, e, fields_str(f, "note"), rep, until)) {
        char ymd[11];
        fmt_ymd(s, ymd);
        ctx_redirect(c, "/book/%d?w=%s", r->id, ymd);
        ctx_set_live(c, "book.q", "");
    }
}

static void act_cancel(Ctx *c, Fields *f) {
    Booking *b = book_get((int)fields_int(f, "id", 0));
    if (!b) {
        ctx_fail(c, "%s", T("That booking is already gone"));
        return;
    }
    Booking copy = *b;
    Resource *r = res_get(copy.res);
    if (!may_change_or_say(c, r, &copy, true)) return;
    BookSeries *sp = copy.series ? tbl_get(&T_book_series, copy.series) : NULL;
    bool later = sp && str_eq(fields_str(f, "later"), "1");
    if (later) { /* stop repeating: this one and every later one go */
        BookSeries s = *sp;
        s.stopped = true;
        tbl_put(&T_book_series, &s);
        int n = 0;
        for (int i = tbl_count(&T_booking) - 1; i >= 0; i--) {
            const Booking *x = tbl_at(&T_booking, i);
            if (x->series != copy.series || x->start < copy.start) continue;
            tbl_del(&T_booking, x->id);
            n++;
        }
        ctx_ok(c, TN(n, "Stopped repeating: %d %s booking cancelled", "Stopped repeating: %d %s bookings cancelled"), n,
               r ? r->name : "");
    } else {
        tbl_del(&T_booking, copy.id);
        ctx_ok(c, "%s", T("Cancelled"));
    }
    if (r) {
        int told[2];
        tell(c, later ? DID_STOP : DID_CANCEL, r, &copy, NULL, told);
        announce(c, later ? DID_STOP : DID_CANCEL, r, &copy, told);
        char ymd[11];
        fmt_ymd(copy.start, ymd);
        ctx_redirect(c, "/book/%d?w=%s", r->id, ymd);
    }
}

/* ------------------------------------------------------------ resources */

typedef struct {
    const char *name, *emoji;
    int hue, slot, open, close, dflt, max;
    const char *rules;
    const char *aliases; /* Swedish names quick booking knows it by (issue #55); a prefix covers "bilen", "bastun" */
    bool kids, guests;   /* open to children / guests from the start (issue #44; an admin changes it) */
} Preset;

/* A starter's name and rules (English, the form's value) become the new thing's in the words of the admin who
   adds it (issue #46) - the household's own data from then on. */
static const Preset PRESETS[] = {
    {N_("Car"), "🚗", 205, 30, 0, 1440, 120, 0, N_("Fill up when it's below a quarter tank"), "bil", false, false},
    {N_("Laundry"), "🧺", 175, 60, 7 * 60, 22 * 60, 180, 240, N_("Clean the lint filter afterwards"),
     "tvätt, tvättstuga", false, true},
    {N_("Guest room"), "🛏️", 25, 1440, 0, 1440, 1440, 0, "", "gästrum", false, true},
    {N_("Sauna"), "🧖", 15, 30, 6 * 60, 24 * 60, 90, 180, "", "bastu", true, true},
    {N_("TV room"), "📺", 255, 30, 0, 1440, 120, 0, "", "tv-rum, tv-rummet", true, true},
    {N_("Cargo bike"), "🚲", 90, 30, 0, 1440, 120, 0, "", "lådcykel", false, false},
    {N_("Workshop"), "🔧", 45, 60, 8 * 60, 21 * 60, 120, 0, "", "verkstad", false, false},
    {N_("Office"), "💻", 220, 60, 7 * 60, 20 * 60, 240, 0, N_("Quiet please"), "kontor, arbetsrum", true, false},
};

/* A starter is named in its adder's language - but a household that speaks two still books the "Car" as
   "coche" or "bil" (issue #46). */
static void match_starter(const Tokens *t, int id, const char *name, Match *m) {
    for (int i = 0; i < countof(PRESETS); i++) {
        const char *en = PRESETS[i].name, *sv = T_in(LANG_SV, en), *es = T_in(LANG_ES, en);
        if (!str_ieq(en, name) && !str_ieq(sv, name) && !str_ieq(es, name)) continue;
        match_name(t, id, en, m);
        match_name(t, id, sv, m);
        match_name(t, id, es, m);
        match_names(t, id, PRESETS[i].aliases, m);
        return;
    }
}

/* (a starter named in English, Swedish or Spanish - whoever added it) */
const char *book_starter_aliases(const char *name) {
    for (int i = 0; i < countof(PRESETS); i++)
        if (str_ieq(PRESETS[i].name, name) || str_ieq(T_in(LANG_SV, PRESETS[i].name), name) ||
            str_ieq(T_in(LANG_ES, PRESETS[i].name), name))
            return PRESETS[i].aliases;
    return "";
}

static void act_preset(Ctx *c, Fields *f) {
    const char *name = fields_str(f, "name");
    for (int i = 0; i < countof(PRESETS); i++) {
        if (!str_eq(PRESETS[i].name, name)) continue;
        Resource r = {0};
        str_copy(r.name, sizeof r.name, T(PRESETS[i].name));
        str_copy(r.emoji, sizeof r.emoji, PRESETS[i].emoji);
        r.hue = PRESETS[i].hue;
        r.slot = PRESETS[i].slot;
        r.open_min = PRESETS[i].open;
        r.close_min = PRESETS[i].close;
        r.default_min = PRESETS[i].dflt;
        r.max_min = PRESETS[i].max;
        str_copy(r.rules, sizeof r.rules, PRESETS[i].rules[0] ? T(PRESETS[i].rules) : "");
        r.kids = PRESETS[i].kids;
        r.guests = PRESETS[i].guests;
        r.sort = tbl_count(&T_resource) + 1;
        tbl_put(&T_resource, &r);
        activity_add(c->me ? c->me->id : 0, r.emoji, "/book", N_("%s added %s to bookings"),
                     c->me ? c->me->name : T("Someone"), r.name);
        ctx_ok(c, T("%s added"), r.name);
        return;
    }
    ctx_fail(c, "%s", T("Unknown suggestion"));
}

static void act_res_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Resource r = {0};
    if (id) {
        Resource *old = res_get(id);
        if (!old) {
            ctx_fail(c, "%s", T("Unknown thing"));
            return;
        }
        r = *old;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    if (!name[0]) {
        ctx_fail_at(c, "name", "%s", T("Give it a name"));
        return;
    }
    str_copy(r.name, sizeof r.name, name);
    str_copy(r.emoji, sizeof r.emoji, fields_str(f, "emoji"));
    str_trim(r.emoji);
    r.hue = (int)CLAMP(fields_int(f, "hue", 200), 0, 359);
    bool daily = str_eq(fields_str(f, "kind"), "daily");
    r.slot = daily ? 1440 : (int)fields_int(f, "slot", 30);
    if (r.slot != 15 && r.slot != 30 && r.slot != 60 && r.slot != 1440) r.slot = 30;
    int open = 0, close = 1440;
    if (!daily && !str_eq(fields_str(f, "always"), "1")) {
        bool ok = parse_hm(fields_str(f, "open"), &open) && parse_hm(fields_str(f, "close"), &close);
        if (ok && close == 0) close = 1440; /* "until 00:00" = until midnight */
        if (!ok || close <= open) {
            ctx_fail_at(c, "open", "%s", T("Opening hours look odd — from must be before until"));
            return;
        }
    }
    r.open_min = daily ? 0 : open;
    r.close_min = daily ? 1440 : close;
    r.default_min = daily ? 1440 : (int)CLAMP(fields_int(f, "dflt", 60), 15, 7 * 1440);
    r.max_min = (int)CLAMP(fields_int(f, "max", 0), 0, 30 * 1440);
    if (daily && r.max_min) r.max_min = MAX(1440, r.max_min / 1440 * 1440); /* whole days; a limit never rounds to none */
    str_copy(r.rules, sizeof r.rules, fields_str(f, "rules"));
    str_copy(r.aliases, sizeof r.aliases, fields_str(f, "aliases"));
    str_trim(r.aliases);
    if (is_admin(c)) { /* the household's rules: an admin's to set */
        r.own_only = str_eq(fields_str(f, "own_only"), "1");
        r.kids = str_eq(fields_str(f, "kids"), "1");
        r.guests = str_eq(fields_str(f, "guests"), "1");
    }
    if (!id) r.sort = tbl_count(&T_resource) + 1;
    tbl_put(&T_resource, &r);
    ctx_ok(c, id ? T("Saved %s") : T("Added %s"), r.name);
    ctx_redirect(c, "/book/%d", r.id);
}

static void act_news_seen(Ctx *c, Fields *f) {
    (void)f;
    for (int i = tbl_count(&T_book_news) - 1; i >= 0; i--) {
        BookNews *n = tbl_at(&T_book_news, i);
        if (n->person == c->me->id) tbl_del(&T_book_news, n->id);
    }
    ctx_ok(c, "%s", T("Noted"));
}

static void act_res_archive(Ctx *c, Fields *f) {
    Resource *r = res_get((int)fields_int(f, "id", 0));
    if (!r) {
        ctx_fail(c, "%s", T("Unknown thing"));
        return;
    }
    Resource copy = *r;
    copy.archived = !copy.archived;
    tbl_put(&T_resource, &copy);
    ctx_ok(c, copy.archived ? T("%s hidden from bookings") : T("%s is back"), copy.name);
    ctx_redirect(c, "/book");
}

static void init(void) {
    T_resource.what = "things to book";
    store_register(&T_resource);
    T_booking.what = "bookings";
    store_register(&T_booking);
    T_book_news.what = "booking changes to tell";
    T_book_news.api_owner = "person";
    store_register(&T_book_news);
    T_book_series.what = "repeating bookings";
    store_register(&T_book_series);
}

static void start(void) {
    db_begin();
    book_series_fill(now_sec()); /* (the days that passed while Hemma was off) */
    db_commit();
}

/* repeating bookings: topped up an hour at a time */
static int64_t g_filled_ms;
static void tick(int64_t now_ms) {
    if (g_filled_ms && now_ms - g_filled_ms < 3600 * 1000) return;
    g_filled_ms = now_ms;
    db_begin();
    if (book_series_fill(now_sec())) app_changed();
    db_commit();
}

static void widget(Ctx *c, Buf *b, int where) { book_widget(c, b, where); }

static bool row_now(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)kind, (void)arg;
    return book_wall(c, b);
}

static const Block blocks[] = {
    {"book.now", N_("Bookings"), SCREEN_WALL, row_now, NULL},
    {0},
};

/* Bookings in other modules' agendas (the planner's calendar, ...). A guest: only what's open to guests. */
static void agenda(Ctx *c, int64_t from, int64_t to, Agenda *out) {
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *bk = tbl_at(&T_booking, i);
        if (bk->end <= from || bk->start >= to) continue;
        Resource *r = res_get(bk->res);
        if (!r || r->archived || (is_guest(c) && !r->guests)) continue;
        AgendaItem it = {0};
        it.start = bk->start;
        it.end = bk->end;
        it.allday = res_daily(r);
        snprintf(it.title, sizeof it.title, "%s%s%s", r->name, bk->note[0] ? " · " : "", bk->note);
        str_copy(it.icon, sizeof it.icon, r->emoji[0] ? r->emoji : "📅");
        char ymd[11];
        fmt_ymd(bk->start, ymd);
        snprintf(it.link, sizeof it.link, "/book/%d?w=%s&b=%d", r->id, ymd, bk->id);
        it.who = bk->who;
        str_copy(it.kind, sizeof it.kind, N_("Booking")); /* (English: core/feed.c tells bookings by it; said where shown) */
        agenda_add(out, &it);
    }
}

/* Someone leaves (issue #45): what's ahead of them is cancelled; erasing, the rest of theirs goes too and what
   they booked for others keeps no "by". */
static int forget(Forget *f) {
    int ahead = 0, past = 0, by = 0, series = 0, news = 0;
    for (int i = tbl_count(&T_booking) - 1; i >= 0; i--) {
        Booking *bk = tbl_at(&T_booking, i);
        if (bk->who == f->person) {
            bool future = bk->start >= f->now;
            if (!future && f->keep) continue; /* (history: under way or over) */
            if (future) ahead++;
            else past++;
            if (f->apply) tbl_del(&T_booking, bk->id);
        } else if (bk->by == f->person && !f->keep) {
            by++;
            if (f->apply) {
                Booking rec = *bk;
                rec.by = 0;
                tbl_put(&T_booking, &rec);
            }
        }
    }
    for (int i = tbl_count(&T_book_series) - 1; i >= 0; i--) {
        BookSeries rec = *(BookSeries *)tbl_at(&T_book_series, i);
        if (rec.who == f->person) { /* nothing more is made for them */
            series += !rec.stopped;
            rec.stopped = true;
            if (f->apply && f->keep) tbl_put(&T_book_series, &rec);
            else if (f->apply) tbl_del(&T_book_series, rec.id);
        } else if (rec.by == f->person && !f->keep && f->apply) {
            rec.by = 0;
            tbl_put(&T_book_series, &rec);
        }
    }
    for (int i = tbl_count(&T_book_news) - 1; i >= 0; i--) {
        BookNews *x = tbl_at(&T_book_news, i);
        if (x->person != f->person) continue;
        news++;
        if (f->apply) tbl_del(&T_book_news, x->id);
    }
    if (ahead)
        forget_note(f, TN(ahead, "%d booking still to come is cancelled", "%d bookings still to come are cancelled"), ahead);
    if (series) forget_note(f, TN(series, "%d repeating booking stops", "%d repeating bookings stop"), series);
    if (past) forget_note(f, TN(past, "%d past booking is deleted", "%d past bookings are deleted"), past);
    if (by)
        forget_note(f, TN(by, "%d booking they made for others stays, without their name",
                          "%d bookings they made for others stay, without their name"),
                    by);
    return ahead + past + by + series + news;
}

/* Children and guests (issue #44) book what's open to them, for themselves, and change only their own. */
static const Action actions[] = {
    {"book.preview", act_preview, true, .may = ACT_ALL},
    {"book.quick", act_quick, .may = ACT_ALL},
    {"book.create", act_create, .may = ACT_ALL},
    {"book.cancel", act_cancel, .may = ACT_ALL},
    {"book.preset", act_preset},
    {"book.res_save", act_res_save},
    {"book.res_archive", act_res_archive},
    {"book.news_seen", act_news_seen, .may = ACT_ALL},
    {0},
};

const char *book_presets_name(int i);
int book_presets_count(void);
const char *book_presets_name(int i) { return i >= 0 && i < countof(PRESETS) ? PRESETS[i].name : NULL; }
int book_presets_count(void) { return countof(PRESETS); }
const char *book_presets_emoji(int i);
const char *book_presets_emoji(int i) { return i >= 0 && i < countof(PRESETS) ? PRESETS[i].emoji : NULL; }

/* Issue #39: a whole-day thing's bookings are kept as local midnights; the others' times were typed. */
static int rezone(Rezone *z) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking bk = *(Booking *)tbl_at(&T_booking, i);
        const Resource *r = res_get(bk.res);
        int64_t was = bk.start;
        /* one a series made counts as typed when the series was: made at a start after the change, from its
           times as they were typed */
        const BookSeries *from = book_series_of(&bk);
        int64_t typed = from && from->created < bk.created ? from->created : bk.created;
        if (!rezone_span(z, r && res_daily(r), typed, &bk.start, &bk.end)) continue;
        n++;
        if (z->apply) tbl_put(&T_booking, &bk);
        else if (!z->days) rezone_note(z, r ? r->name : T("A booking"), was, bk.start);
    }
    for (int i = 0; i < tbl_count(&T_book_series); i++) { /* what the bookings still to be made follow (#47) */
        BookSeries s = *(BookSeries *)tbl_at(&T_book_series, i);
        const Resource *r = res_get(s.res);
        bool moved = rezone_span(z, r && res_daily(r), s.created, &s.start, &s.end);
        moved |= z->days && rezone_day(z, &s.until);
        if (moved && z->apply) tbl_put(&T_book_series, &s); /* (not counted: no booking of its own) */
    }
    return n;
}

static const Module book = {
    .id = "book",
    .title = N_("Book"),
    .icon = "calendar",
    .nav = 2,
    .init = init,
    .start = start,
    .tick = tick,
    .page = book_page,
    .actions = actions,
    .widget = widget,
    .widget_order = 10,
    .agenda = agenda,
    .rezone = rezone,
    .forget = forget,
    .blocks = blocks,
};
MODULE(book)
