/* core/when.h — natural-language dates and times for quick-add bars.
 *
 * Understands English and Swedish phrases like "car fri 14-17 ikea trip",
 * "bastu idag kl 19-21", "vacuum @eva every tuesday", "guest room 12/10-14/10",
 * "laundry in 30 min for 2h". Words that are not date/time related are left
 * alone so callers can use them (resource names, notes, task titles):
 *
 *     Tokens t;
 *     tok_split(&t, "car fri 14-17 ikea trip");
 *     mark_resource_tokens(&t);              // caller: set t.used[i] for "car"
 *     When w;
 *     if (when_parse(&t, now, &w) && when_resolve(&w, now, 60, &start, &end)) ...
 *     tok_rest(&t, note, sizeof note);       // -> "ikea trip"
 *
 * Tokens already marked used[] before when_parse() are skipped, so callers can
 * claim words first (e.g. a resource called "Room 2"). */
#ifndef HEMMA_WHEN_H
#define HEMMA_WHEN_H

#include "base.h"

#define TOK_MAX 40
#define TOK_LEN 64

typedef struct {
    int n;
    char low[TOK_MAX][TOK_LEN]; /* folded lower-case token (fold_copy); dashes – — become '-' */
    char raw[TOK_MAX][TOK_LEN]; /* original spelling (cut at TOK_LEN for matching) */
    bool used[TOK_MAX];         /* set by parsers that consumed the token */
    /* where each token sits in the text, so tok_rest() returns whole words
       and everything past the TOK_MAX-th token (the text must outlive t) */
    const char *src;
    uint32_t at[TOK_MAX], len[TOK_MAX], tail;
} Tokens;

/* Split on whitespace (incl. no-break space). Trailing , . ! ? ; are stripped
 * from tokens ("kl." -> "kl", "p.m." -> "p.m"). At most TOK_MAX tokens. */
void tok_split(Tokens *t, const char *text);
/* Unused raw tokens joined by single spaces (UTF-8 safe truncation). */
void tok_rest(const Tokens *t, char *out, size_t cap);

enum { REPEAT_NONE, REPEAT_DAILY, REPEAT_WEEKLY, REPEAT_BIWEEKLY, REPEAT_MONTHLY };

typedef struct {
    bool has_date;
    int y, m, d; /* a day was mentioned */
    bool has_end_date;
    int ey, em, ed; /* day ranges: "fri-sun", "12/10-14/10", "until sunday" */
    bool has_start;
    int start_min; /* minutes after midnight */
    bool has_end;
    int end_min;  /* explicit end time (1440 = 24:00) */
    int dur_min;  /* explicit duration ("2h", "90 min", "for 2 hours"), 0 = none */
    bool now;
    int rel_min; /* "now"/"nu" (rel 0) or "in 30 min"/"om 30 min" -> start = now + rel_min */
    int repeat;
    int repeat_wday; /* REPEAT_*; repeat_wday 0=Mon..6=Sun or -1 */
} When;

/* Marks consumed tokens used[]; true if anything date/time-like was found. */
bool when_parse(Tokens *t, int64_t now, When *w);

/* Absolute times. No date: today, or tomorrow if the start time already passed
 * today by >15 min. No start time: start of day (all-day) unless now/rel or only
 * a duration/end time was given (then from now). End: end_min (if <= start ->
 * next day), else dur_min, else all-day -> next midnight, else default_min.
 * Day range without times: start = day start, end = day after end date 00:00.
 * Returns false when the When holds nothing at all. */
bool when_resolve(const When *w, int64_t now, int default_min, int64_t *start, int64_t *end);

/* Friendly English range text relative to now, using an en dash:
 *   "Today 19:00–21:00", "Tomorrow 09:00–10:30", "Fri 16 Oct 14:00–17:00",
 *   "Fri 16 Oct 14:00 – Sat 17 Oct 10:00", "Sat 17 – Mon 19 Oct" (all-day span),
 *   "Sat 17 Oct, all day", "Tue 5 Jan 2027 10:00–11:00" (other year). */
void when_describe(int64_t start, int64_t end, int64_t now, char *out, size_t cap);

#endif
