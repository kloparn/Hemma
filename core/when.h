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
    int start_min; /* minutes after midnight (1440.. = after the next midnight: "tonight 1") */
    bool has_end;
    int end_min;  /* explicit end time (1440 = 24:00; more = after it) */
    int dur_min;  /* explicit duration ("2h", "90 min", "for 2 hours"), 0 = none */
    bool now;
    int rel_min; /* "now"/"nu" (rel 0) or "in 30 min"/"om 30 min" -> start = now + rel_min */
    int repeat;
    int repeat_wday; /* REPEAT_*; repeat_wday 0=Mon..6=Sun or -1 */
    char refused[64]; /* why the text was refused (when_parse() returned false), else "" */
} When;

/* Marks consumed tokens used[]; true if anything date/time-like was found.
 *
 * Refuses (returns false, leaves every token as the caller had it, and says why in w->refused)
 * when the text names a day that can't be - "31 sep", "31/9", "2026-09-31", "29/2/2027" ->
 * "There's no 31 September" - or a day range whose end comes before its start - "12/10-10/10",
 * "12/10/2026-10/10/2026", "14-12 okt", "until 5/10" -> "The end date is before the start date".
 * An end date without a year takes the start's year, or the next one when its month comes
 * earlier: "28/12-2/1" runs past New Year; "28-3/10" starts the month before only for a range of
 * two weeks at most ("14-12/10" is refused). Times that pass midnight ("22-02") are fine.
 * A time after the last day of a written-out range is its end: "fri till sun 17", "until
 * tomorrow kl 10" (from now, if the start is today).
 *
 * Evening days - "tonight", "ikväll", "i kväll", "inatt", "i natt", or a day + "evening"/"night"/
 * "kväll"/"natt" ("imorgon kväll", "fri night", "this evening") - take that evening and the
 * night after it: 5..11 = pm, 12 = midnight (24:00), and anything before 05:00 is after that
 * midnight: "tonight 8" = 20:00, "tonight 12" = 00:00 and "tonight 1" = 01:00 tomorrow,
 * "imorgon kväll 8" = 20:00 tomorrow. Leading zeros, am/pm and 13..24 stay as typed otherwise
 * ("tonight 08", "tonight 8am", "tonight 20"). A time belongs to the day written right before it
 * ("tonight until tomorrow 10" ends at 10:00), else to the start day. Typed between midnight
 * and 05:00, "tonight" is the night we're in for times that haven't passed ("tonight 12:30" at
 * 00:10 = in 20 minutes). */
bool when_parse(Tokens *t, int64_t now, When *w);

/* Absolute times. No date: today, or tomorrow if the start time already passed
 * today by >15 min. No start time: start of day (all-day) unless now/rel or only
 * a duration/end time was given for today (then from now). End: end_min (if <= start ->
 * next day), else dur_min, else all-day -> next midnight, else default_min.
 * Day range without times: start = day start, end = day after end date 00:00.
 * Clock changes: a start in the hour skipped in spring moves forward by the jump (02:30 ->
 * 03:30) and keeps the length typed ("02:00-03:00" -> 03:00-04:00), an end in it moves forward
 * too; in the hour that repeats in autumn a time means its first pass. end > start always.
 * Returns false when the When holds nothing at all (or was refused). */
bool when_resolve(const When *w, int64_t now, int default_min, int64_t *start, int64_t *end);

/* Friendly English range text relative to now, using an en dash:
 *   "Today 19:00–21:00", "Tomorrow 09:00–10:30", "Fri 16 Oct 14:00–17:00",
 *   "Fri 16 Oct 14:00 – Sat 17 Oct 10:00", "Sat 17 – Mon 19 Oct" (all-day span),
 *   "Sat 17 Oct, all day", "Tue 5 Jan 2027 10:00–11:00" (other year). */
void when_describe(int64_t start, int64_t end, int64_t now, char *out, size_t cap);

#endif
