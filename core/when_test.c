#include "../tests/test.h"
#include "i18n.h"
#include "when.h"

/* All expectations are relative to Wednesday 7 Oct 2026, 10:00 local time and
 * compared through dt_local(), so they hold in any time zone. */
static int64_t NOW;

static void fmt_t(int64_t t, char *out, size_t cap) {
    DateTime d = dt_local(t);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", d.year, d.mon, d.day, d.hour, d.min);
}

typedef struct {
    const char *in, *start, *end, *rest; /* start/end "-" = nothing parsed; rest NULL = don't care */
} Case;

static void check_case(const Case *c, int default_min) {
    Tokens t;
    When w;
    int64_t s = 0, e = 0;
    tok_split(&t, c->in);
    bool any = when_parse(&t, NOW, &w);
    bool ok = any && when_resolve(&w, NOW, default_min, &s, &e);
    char gs[32] = "-", ge[32] = "-", rest[256];
    if (ok) {
        fmt_t(s, gs, sizeof gs);
        fmt_t(e, ge, sizeof ge);
    }
    tok_rest(&t, rest, sizeof rest);
    test_checks++;
    if (!str_eq(gs, c->start) || !str_eq(ge, c->end) || (c->rest && !str_eq(rest, c->rest)))
        test_fail(__FILE__, __LINE__,
                  "\"%s\"\n      got:      %s -> %s  rest \"%s\"\n      expected: %s -> %s  rest \"%s\"", c->in, gs, ge,
                  rest, c->start, c->end, c->rest ? c->rest : "(any)");
}

#define RUN(cases)                                                                                                     \
    do {                                                                                                               \
        NOW = dt_make(2026, 10, 7, 10, 0);                                                                             \
        for (int i_ = 0; i_ < countof(cases); i_++) check_case(&(cases)[i_], 60);                                      \
    } while (0)

TEST(when_now_is_wednesday) {
    DateTime d = dt_local(dt_make(2026, 10, 7, 10, 0));
    CHECK_INT(d.wday, 2);
}

TEST(when_relative_days) {
    static const Case cases[] = {
        {"today 19-21", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"idag 19-21", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"i dag 19-21", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"Idag kl 19-21", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"tonight 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"ikväll 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"i kväll 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"IKVÄLL kl 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"ikvall 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"tomorrow 9-10:30", "2026-10-08 09:00", "2026-10-08 10:30", ""},
        {"imorgon 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"imorn 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"i morgon 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"tmrw 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"Tomorrow at 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"day after tomorrow 12", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"the day after tomorrow at noon", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"övermorgon 12", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"i övermorgon 12", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"overmorgon 12", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"tomorrow", "2026-10-08 00:00", "2026-10-09 00:00", ""},
    };
    RUN(cases);
}

TEST(when_weekdays_english_and_swedish) {
    static const Case cases[] = {
        {"wed 18", "2026-10-07 18:00", "2026-10-07 19:00", ""}, /* today counts */
        {"ons 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"onsdag 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"Wednesday 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"thu 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"thur 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"thurs 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"thursday 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"tors 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"tor 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"torsdag 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"fri 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"fre 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"Fredag 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"friday 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"sat 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"lör 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"LÖRDAG 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"lordag 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"saturday 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"sun 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"sön 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"söndag 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"sunday 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"mon 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"mån 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"Måndag 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"mandag 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"monday 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"tue 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"tues 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"tis 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"tisdag 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"tuesday 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"tue", "2026-10-13 00:00", "2026-10-14 00:00", ""},
        {"on friday at 6", "2026-10-09 18:00", "2026-10-09 19:00", ""},
        {"på fredag kl 18", "2026-10-09 18:00", "2026-10-09 19:00", ""},
        {"this friday 10", "2026-10-09 10:00", "2026-10-09 11:00", ""},
        {"next fri 14", "2026-10-16 14:00", "2026-10-16 15:00", ""},
        {"nästa fredag 14", "2026-10-16 14:00", "2026-10-16 15:00", ""},
        {"next wed", "2026-10-14 00:00", "2026-10-15 00:00", ""},
        {"next mon", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"next week", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"nästa vecka", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"this weekend", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"weekend", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"helgen", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"i helgen", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"next weekend", "2026-10-17 00:00", "2026-10-19 00:00", ""},
        {"nästa helg", "2026-10-17 00:00", "2026-10-19 00:00", ""},
    };
    RUN(cases);
}

TEST(when_dates) {
    static const Case cases[] = {
        {"12/10", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"12/10 14-16", "2026-10-12 14:00", "2026-10-12 16:00", ""},
        {"12/10/2026 14-16", "2026-10-12 14:00", "2026-10-12 16:00", ""},
        {"12/10/26 14-16", "2026-10-12 14:00", "2026-10-12 16:00", ""},
        {"2026-10-12 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12 oct 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"oct 12 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12 okt 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12 oktober 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12th october 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12th of October 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"October 12th 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12:e oktober 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"den 12 oktober kl 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"den 12 kl 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"the 12th at 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"on the 12th at 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"12th 9", "2026-10-12 09:00", "2026-10-12 10:00", ""},
        {"den 7", "2026-10-07 00:00", "2026-10-08 00:00", ""}, /* today has not passed */
        {"den 3", "2026-11-03 00:00", "2026-11-04 00:00", ""}, /* passed -> next month */
        {"1/10", "2027-10-01 00:00", "2027-10-02 00:00", ""},  /* > a day ago -> next year */
        {"6/10", "2026-10-06 00:00", "2026-10-07 00:00", ""},  /* yesterday stays */
        {"5 jan", "2027-01-05 00:00", "2027-01-06 00:00", ""},
        {"12 oct 2027", "2027-10-12 00:00", "2027-10-13 00:00", ""},
        {"may 2 18", "2027-05-02 18:00", "2027-05-02 19:00", ""},
        {"2 maj", "2027-05-02 00:00", "2027-05-03 00:00", ""},
        {"31/2", "-", "-", "31/2"},
        {"29/2/2028", "2028-02-29 00:00", "2028-03-01 00:00", ""},
        {"12/13", "-", "-", "12/13"},
    };
    RUN(cases);
}

TEST(when_day_ranges) {
    static const Case cases[] = {
        {"fri-sun", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"fre-sön", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"fri–sun", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"friday - sunday", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"from friday to sunday", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"från fredag till söndag", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"mellan fredag och söndag", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"sat-mon", "2026-10-10 00:00", "2026-10-13 00:00", ""},
        {"12/10-14/10", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"12/10 - 14/10", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"12-14 okt", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"12-14/10", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"28/12-2/1", "2026-12-28 00:00", "2027-01-03 00:00", ""},
        {"until sunday", "2026-10-07 00:00", "2026-10-12 00:00", ""},
        {"till söndag", "2026-10-07 00:00", "2026-10-12 00:00", ""},
        {"tills söndag", "2026-10-07 00:00", "2026-10-12 00:00", ""},
        {"fri 14 until sunday", "2026-10-09 14:00", "2026-10-12 00:00", ""},
    };
    RUN(cases);
}

/* A refused text: nothing parsed, every word left for the caller, and a reason. */
typedef struct {
    const char *in, *why;
} RefuseCase;

static void check_refused(const RefuseCase *c) {
    Tokens t;
    When w;
    int64_t s = 1, e = 1;
    tok_split(&t, c->in);
    bool any = when_parse(&t, NOW, &w);
    bool resolved = when_resolve(&w, NOW, 60, &s, &e);
    char rest[256], gs[32] = "-", ge[32] = "-";
    tok_rest(&t, rest, sizeof rest);
    if (resolved) {
        fmt_t(s, gs, sizeof gs);
        fmt_t(e, ge, sizeof ge);
    }
    test_checks++;
    if (any || resolved || !str_eq(w.refused, c->why) || !str_eq(rest, c->in))
        test_fail(__FILE__, __LINE__,
                  "\"%s\"\n      got:      %s -> %s  refused \"%s\"  rest \"%s\"\n"
                  "      expected: refused \"%s\", every word left",
                  c->in, gs, ge, w.refused, rest, c->why);
}

#define RUN_REFUSED(cases)                                                                                             \
    do {                                                                                                               \
        NOW = dt_make(2026, 10, 7, 10, 0);                                                                             \
        for (int i_ = 0; i_ < countof(cases); i_++) check_refused(&(cases)[i_]);                                       \
    } while (0)

/* Issue #13: a day that doesn't exist used to be dropped and the words after it read again
 * ("31 sep 14" became 14 Sep 2027, "bil 31 sep 14-16" today 14-16). */
TEST(when_refuses_days_that_dont_exist) {
    static const RefuseCase refused[] = {
        {"31 sep 14", "There's no 31 September"},
        {"bil 31 sep 14-16", "There's no 31 September"},
        {"sep 31", "There's no 31 September"},
        {"31 sep 2027", "There's no 31 September"},
        {"31st of september", "There's no 31 September"},
        {"den 31 september kl 10", "There's no 31 September"},
        {"31/9", "There's no 31 September"},
        {"31/9 14-16", "There's no 31 September"},
        {"31/9/2026", "There's no 31 September"},
        {"2026-09-31", "There's no 31 September"},
        {"2026-09-31 14-16", "There's no 31 September"},
        {"30 feb", "There's no 30 February"},
        {"29/2", "There's no 29 February 2027"}, /* the next one is in 2028: say which year was meant */
        {"29 feb 2027 10", "There's no 29 February 2027"},
        {"30-31 sep", "There's no 31 September"},
        {"31-2/10", "There's no 31 September"},
        {"31/9-2/10", "There's no 31 September"},
        {"1/10-31/9", "There's no 31 September"},
        {"fri until 31 sep", "There's no 31 September"},
        {"from fri to 31/9", "There's no 31 September"},
        {"1/1/1960 10", "Pick a year from 1970 to 2200"},
    };
    RUN_REFUSED(refused);
    static const Case fine[] = {
        {"30 sep 14", "2027-09-30 14:00", "2027-09-30 15:00", ""}, /* exists: passed, so next year */
        {"31 oct", "2026-10-31 00:00", "2026-11-01 00:00", ""},
        {"31/12 14-16", "2026-12-31 14:00", "2026-12-31 16:00", ""},
        {"29/2/2028 9", "2028-02-29 09:00", "2028-02-29 10:00", ""},
        {"12/13 14", "2026-10-07 14:00", "2026-10-07 15:00", "12/13"}, /* month 13: not a date, just a word */
        {"50/50 14", "2026-10-07 14:00", "2026-10-07 15:00", "50/50"},
    };
    RUN(fine);

    /* words the caller claimed stay claimed, the rest are free again */
    Tokens t;
    When w;
    tok_split(&t, "bil 31 sep 14-16");
    t.used[0] = true; /* the booking module recognised "bil" */
    CHECK(!when_parse(&t, NOW, &w));
    CHECK_STR(w.refused, "There's no 31 September");
    CHECK(t.used[0] && !t.used[1] && !t.used[2] && !t.used[3]);
    CHECK(!w.has_date && !w.has_start && !w.has_end);
    char rest[64];
    tok_rest(&t, rest, sizeof rest);
    CHECK_STR(rest, "31 sep 14-16");
    /* and a fresh parse starts clean */
    tok_split(&t, "fri 14");
    CHECK(when_parse(&t, NOW, &w));
    CHECK_STR(w.refused, "");
}

/* Issue #13: an end date before the start used to be pushed a year on (a year-long booking). */
TEST(when_refuses_ranges_that_end_before_they_start) {
    static const RefuseCase refused[] = {
        {"12/10/2026-10/10/2026", "The end date is before the start date"},
        {"12/10-10/10", "The end date is before the start date"},
        {"12/10/2026-10/10", "The end date is before the start date"},
        {"12/10 - 10/10/2026", "The end date is before the start date"},
        {"14-12 okt", "The end date is before the start date"},
        {"from 12 oct to 10 oct", "The end date is before the start date"},
        {"2026-10-12 to 2026-10-10", "The end date is before the start date"},
        {"tomorrow - today", "The end date is before the start date"},
        {"until 5/10", "The end date is before the start date"},
        {"guest room fri until 5/10", "The end date is before the start date"},
        /* "28-3/10" may start the month before, but only for two weeks at most */
        {"14-12/10", "The end date is before the start date"},
        {"14-12/11", "The end date is before the start date"},
        {"19-3/11", "The end date is before the start date"}, /* 19 Oct - 3 Nov: 15 days */
    };
    RUN_REFUSED(refused);
    static const Case fine[] = {
        {"28-3/11", "2026-10-28 00:00", "2026-11-04 00:00", ""},
        {"20-3/11", "2026-10-20 00:00", "2026-11-04 00:00", ""}, /* 14 days: still fine */
        {"28-2/1", "2026-12-28 00:00", "2027-01-03 00:00", ""},
        {"28/12/2026-2/1", "2026-12-28 00:00", "2027-01-03 00:00", ""}, /* past New Year: next year */
        {"28/12-2/1/2027", "2026-12-28 00:00", "2027-01-03 00:00", ""},
        {"30/12/2027-2/1", "2027-12-30 00:00", "2028-01-03 00:00", ""}, /* the start's year, not today's */
        {"20 dec - 6 jan", "2026-12-20 00:00", "2027-01-07 00:00", ""},
        {"1/11-31/3", "2026-11-01 00:00", "2027-04-01 00:00", ""},
        {"12/10-12/10", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"12/10-14/10/2026", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"from the 28th to the 3rd", "2026-10-28 00:00", "2026-11-04 00:00", ""},
        {"until 7/10", "2026-10-07 00:00", "2026-10-08 00:00", ""},
        {"until 2/1", "2026-10-07 00:00", "2027-01-03 00:00", ""},
        {"fri 22-02", "2026-10-09 22:00", "2026-10-10 02:00", ""}, /* times past midnight are fine */
        {"fri 23:30-00:15", "2026-10-09 23:30", "2026-10-10 00:15", ""},
    };
    RUN(fine);
}

/* Issue #13: "tonight 8" used to be 08:00 (already past). */
TEST(when_evening_words_mean_pm) {
    static const Case cases[] = {
        {"tonight 8", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"bastu ikväll 7", "2026-10-07 19:00", "2026-10-07 20:00", "bastu"},
        {"ikväll 7", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"i kväll 7", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"ikvall 7", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"IKVÄLL kl 7", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"8 tonight", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"kl 8 ikväll", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"tonight 5", "2026-10-07 17:00", "2026-10-07 18:00", ""},
        {"tonight 6", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"tonight 11", "2026-10-07 23:00", "2026-10-08 00:00", ""},
        {"tonight 12", "2026-10-08 00:00", "2026-10-08 01:00", ""}, /* midnight, not noon */
        /* before 05:00: that night, after midnight */
        {"tonight 1", "2026-10-08 01:00", "2026-10-08 02:00", ""},
        {"tonight 4", "2026-10-08 04:00", "2026-10-08 05:00", ""},
        {"tonight 1am", "2026-10-08 01:00", "2026-10-08 02:00", ""},
        {"tonight 12am", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        {"tonight 00:30", "2026-10-08 00:30", "2026-10-08 01:30", ""},
        {"tonight 1-3", "2026-10-08 01:00", "2026-10-08 03:00", ""},
        {"tonight 1-3am", "2026-10-08 01:00", "2026-10-08 03:00", ""},
        {"tonight 9-1", "2026-10-07 21:00", "2026-10-08 01:00", ""},
        {"inatt 2", "2026-10-08 02:00", "2026-10-08 03:00", ""},
        {"i natt 11", "2026-10-07 23:00", "2026-10-08 00:00", ""},
        {"tonight at midnight", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        {"tonight 12:30", "2026-10-08 00:30", "2026-10-08 01:30", ""},
        {"tonight 8:30", "2026-10-07 20:30", "2026-10-07 21:30", ""},
        {"ikväll 6:30", "2026-10-07 18:30", "2026-10-07 19:30", ""},
        {"tonight 7-9", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"ikväll 7-10", "2026-10-07 19:00", "2026-10-07 22:00", ""},
        {"tonight 6-8", "2026-10-07 18:00", "2026-10-07 20:00", ""},
        {"tonight 11-1", "2026-10-07 23:00", "2026-10-08 01:00", ""},
        {"tonight 11-12", "2026-10-07 23:00", "2026-10-08 00:00", ""},
        {"tonight 12-2", "2026-10-08 00:00", "2026-10-08 02:00", ""},
        {"tonight 9 till 11", "2026-10-07 21:00", "2026-10-07 23:00", ""},
        {"tonight 7-9pm", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"ikväll till 11", "2026-10-07 10:00", "2026-10-07 23:00", ""}, /* from now */
        /* as typed: 24-hour clock, leading zero, am/pm, words */
        {"tonight 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"tonight 8pm", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"tonight 8am", "2026-10-07 08:00", "2026-10-07 09:00", ""},
        {"tonight 08", "2026-10-07 08:00", "2026-10-07 09:00", ""},
        {"tonight noon", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        /* other day words don't shift */
        {"today 8", "2026-10-07 08:00", "2026-10-07 09:00", ""},
        {"tomorrow 8", "2026-10-08 08:00", "2026-10-08 09:00", ""},
        {"8", "2026-10-08 08:00", "2026-10-08 09:00", ""},
    };
    RUN(cases);
}

/* An evening is a day's: a time belongs to the day written right before it, else the start day. */
TEST(when_evening_belongs_to_its_day) {
    static const Case cases[] = {
        {"tonight until tomorrow 10", "2026-10-07 10:00", "2026-10-08 10:00", ""}, /* tomorrow's 10, from now */
        {"tonight - tomorrow 10", "2026-10-07 10:00", "2026-10-08 10:00", ""},
        {"8 tonight until tomorrow 10", "2026-10-07 20:00", "2026-10-08 10:00", ""},
        {"tonight 8 until 11", "2026-10-07 20:00", "2026-10-07 23:00", ""},
        /* a day + evening/night/kväll/natt */
        {"imorgon kväll 8", "2026-10-08 20:00", "2026-10-08 21:00", ""},
        {"i morgon kväll kl 7", "2026-10-08 19:00", "2026-10-08 20:00", ""},
        {"tomorrow evening 8", "2026-10-08 20:00", "2026-10-08 21:00", ""},
        {"8 tomorrow evening", "2026-10-08 20:00", "2026-10-08 21:00", ""},
        {"bastu imorgon kväll 7-9", "2026-10-08 19:00", "2026-10-08 21:00", "bastu"},
        {"fredag kväll 19", "2026-10-09 19:00", "2026-10-09 20:00", ""},
        {"fri night 11", "2026-10-09 23:00", "2026-10-10 00:00", ""},
        {"fri night 1", "2026-10-10 01:00", "2026-10-10 02:00", ""},
        {"this evening 7", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"tomorrow evening", "2026-10-08 00:00", "2026-10-09 00:00", ""}, /* the word is taken; all day */
        {"until tomorrow evening 8", "2026-10-07 10:00", "2026-10-08 20:00", ""},
        {"tomorrow 8", "2026-10-08 08:00", "2026-10-08 09:00", ""},
        {"call mom in the evening", "-", "-", "call mom in the evening"},
        /* a time after the last day of a written-out range ends it, on that day */
        {"fri 14 until sunday 17", "2026-10-09 14:00", "2026-10-11 17:00", ""},
        {"fri until sun 17", "2026-10-09 00:00", "2026-10-11 17:00", ""},
        {"från fredag till söndag kl 17", "2026-10-09 00:00", "2026-10-11 17:00", ""},
        {"until sunday 17", "2026-10-07 10:00", "2026-10-11 17:00", ""},
        {"until tomorrow kl 10", "2026-10-07 10:00", "2026-10-08 10:00", ""},
        {"fri-sun 10", "2026-10-09 10:00", "2026-10-12 00:00", ""}, /* written as one word: still the start */
    };
    RUN(cases);
}

/* Typed between midnight and 05:00, "tonight" is the night we're in for times that haven't passed. */
TEST(when_tonight_after_midnight) {
    static const Case at_0010[] = {
        {"tonight 12:30", "2026-10-07 00:30", "2026-10-07 01:30", ""}, /* in 20 minutes */
        {"tonight 2", "2026-10-07 02:00", "2026-10-07 03:00", ""},
        {"tonight 12", "2026-10-07 00:00", "2026-10-07 01:00", ""}, /* 10 minutes ago: the usual grace */
        {"tonight 12-2", "2026-10-07 00:00", "2026-10-07 02:00", ""},
        {"inatt 3", "2026-10-07 03:00", "2026-10-07 04:00", ""},
        {"ikväll till 2", "2026-10-07 00:10", "2026-10-07 02:00", ""},
        {"tonight 11", "2026-10-07 23:00", "2026-10-08 00:00", ""}, /* the evening to come */
    };
    NOW = dt_make(2026, 10, 7, 0, 10);
    for (int i = 0; i < countof(at_0010); i++) check_case(&at_0010[i], 60);
    static const Case at_0300[] = {
        {"tonight 2:50", "2026-10-07 02:50", "2026-10-07 03:50", ""},
        {"tonight 4", "2026-10-07 04:00", "2026-10-07 05:00", ""},
        {"tonight 2", "2026-10-08 02:00", "2026-10-08 03:00", ""}, /* an hour ago: the next night */
    };
    NOW = dt_make(2026, 10, 7, 3, 0);
    for (int i = 0; i < countof(at_0300); i++) check_case(&at_0300[i], 60);
}

/* Like Case, plus the real minutes between start and end: an autumn wall clock shows 02:30 twice. */
typedef struct {
    const char *in, *start, *end;
    int minutes;
} DstCase;

static void check_dst(const DstCase *c, int64_t now) {
    Tokens t;
    When w;
    int64_t s = 0, e = 0;
    tok_split(&t, c->in);
    bool ok = when_parse(&t, now, &w) && when_resolve(&w, now, 60, &s, &e);
    char gs[32] = "-", ge[32] = "-";
    if (ok) {
        fmt_t(s, gs, sizeof gs);
        fmt_t(e, ge, sizeof ge);
    }
    long long mins = ok ? (e - s) / 60 : -1;
    test_checks++;
    if (!str_eq(gs, c->start) || !str_eq(ge, c->end) || mins != c->minutes)
        test_fail(__FILE__, __LINE__, "\"%s\"\n      got:      %s -> %s (%lld min)\n      expected: %s -> %s (%d min)",
                  c->in, gs, ge, mins, c->start, c->end, c->minutes);
}

/* Central European clocks: 2026-03-29 02:00 -> 03:00 and 2026-10-25 03:00 -> 02:00
 * (Stockholm, Berlin, Paris ...). The cases below only hold in such a zone. */
static bool central_european_clocks(void) {
    return dt_local(dt_make(2026, 3, 29, 1, 0) + 3600).hour == 3 &&
           dt_local(dt_make(2026, 10, 25, 1, 0) + 3 * 3600).hour == 3;
}

/* Issue #13: "tomorrow 02:00-03:00" on the night the clocks go forward was a 24-hour booking. */
TEST(when_clock_changes_central_europe) {
    if (!central_european_clocks()) return; /* another zone: when_clock_changes_never_wrap covers it */
    static const DstCase spring[] = {
        /* 02:00-03:00 doesn't exist that night: starts when the clocks go on, keeps its length */
        {"tomorrow 02:00-03:00", "2026-03-29 03:00", "2026-03-29 04:00", 60},
        {"tomorrow 2-3am", "2026-03-29 03:00", "2026-03-29 04:00", 60},
        {"tomorrow 02:30", "2026-03-29 03:30", "2026-03-29 04:30", 60},
        {"02:30", "2026-03-29 03:30", "2026-03-29 04:30", 60}, /* passed today -> tomorrow -> skipped */
        {"tomorrow 02:30-02:45", "2026-03-29 03:30", "2026-03-29 03:45", 15},
        {"tomorrow 02:00-04:00", "2026-03-29 03:00", "2026-03-29 05:00", 120},
        {"tomorrow 02:00 for 2h", "2026-03-29 03:00", "2026-03-29 05:00", 120},
        /* a start before the jump stays; an end in it moves on with the clocks */
        {"tomorrow 01:30-02:30", "2026-03-29 01:30", "2026-03-29 03:30", 60},
        {"tomorrow 01:00-04:00", "2026-03-29 01:00", "2026-03-29 04:00", 120},
        {"sat 23-03", "2026-03-28 23:00", "2026-03-29 03:00", 180},
        {"tomorrow", "2026-03-29 00:00", "2026-03-30 00:00", 23 * 60},
    };
    int64_t now = dt_make(2026, 3, 28, 10, 0);
    for (int i = 0; i < countof(spring); i++) check_dst(&spring[i], now);

    static const DstCase autumn[] = {
        {"tomorrow 02:30", "2026-10-25 02:30", "2026-10-25 02:30", 60}, /* the first 02:30, an hour */
        {"tomorrow 02:00-03:00", "2026-10-25 02:00", "2026-10-25 03:00", 120},
        {"tomorrow 02:30-02:45", "2026-10-25 02:30", "2026-10-25 02:45", 15},
        {"tomorrow 01:00-04:00", "2026-10-25 01:00", "2026-10-25 04:00", 240},
        {"sat 22-02", "2026-10-24 22:00", "2026-10-25 02:00", 240},
        {"tonight 11-3", "2026-10-24 23:00", "2026-10-25 03:00", 300},
        {"tomorrow", "2026-10-25 00:00", "2026-10-26 00:00", 25 * 60},
        {"sat-sun", "2026-10-24 00:00", "2026-10-26 00:00", 49 * 60},
    };
    now = dt_make(2026, 10, 24, 10, 0);
    for (int i = 0; i < countof(autumn); i++) check_dst(&autumn[i], now);

    /* the first pass of the repeated hour: 90 minutes after 01:00 (summer time) */
    Tokens t;
    When w;
    int64_t s, e;
    tok_split(&t, "tomorrow 02:30");
    CHECK(when_parse(&t, now, &w) && when_resolve(&w, now, 60, &s, &e));
    CHECK_INT(s - dt_make(2026, 10, 25, 1, 0), 90 * 60);
}

/* Whatever this machine's zone: on each day of 2026 its clocks change, every hour-long range
 * stays short and forward, a start is never earlier than typed, and nothing wraps to the next
 * day. (No clock changes here: nothing to check.) */
TEST(when_clock_changes_never_wrap) {
    int64_t now = dt_make(2026, 1, 1, 10, 0);
    for (int doy = 0; doy < 365; doy++) {
        if (dt_make(2026, 1, 2 + doy, 0, 0) - dt_make(2026, 1, 1 + doy, 0, 0) == 86400) continue;
        DateTime d = dt_local(dt_make(2026, 1, 1 + doy, 12, 0));
        int64_t noon = dt_make(d.year, d.mon, d.day, 12, 0);
        for (int at = 0; at < 1440; at += 30) {
            char in[64];
            int to = (at + 60) % 1440;
            snprintf(in, sizeof in, "%04d-%02d-%02d %02d:%02d-%02d:%02d", d.year, d.mon, d.day, at / 60, at % 60,
                     to / 60, to % 60);
            Tokens t;
            When w;
            int64_t s, e;
            tok_split(&t, in);
            CHECK(when_parse(&t, now, &w) && when_resolve(&w, now, 60, &s, &e));
            DateTime ds = dt_local(s);
            int shown = (ds.day == d.day ? 0 : 1440) + ds.hour * 60 + ds.min; /* the start on the day's clock */
            test_checks++;
            if (e <= s || e - s > 3 * 3600 || shown < at || shown > at + 120 || s > noon + 13 * 3600)
                test_fail(__FILE__, __LINE__, "\"%s\": %+lld min after noon, %lld min long", in,
                          (long long)(s - noon) / 60, (long long)(e - s) / 60);

            snprintf(in, sizeof in, "%04d-%02d-%02d %02d:%02d", d.year, d.mon, d.day, at / 60, at % 60);
            tok_split(&t, in);
            CHECK(when_parse(&t, now, &w) && when_resolve(&w, now, 60, &s, &e));
            CHECK_INT(e - s, 3600);
        }
    }
}

TEST(when_single_times) {
    static const Case cases[] = {
        {"14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"14:00", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"14.00", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"kl 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"kl. 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"klockan 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"at 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"vid 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"2pm", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"2 pm", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"2 p.m.", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"2PM", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"2:30pm", "2026-10-07 14:30", "2026-10-07 15:30", ""},
        {"noon", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"lunch", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"midnight", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        {"3", "2026-10-07 15:00", "2026-10-07 16:00", ""},  /* bare 1..6 = afternoon */
        {"6", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"06", "2026-10-08 06:00", "2026-10-08 07:00", ""}, /* leading zero = morning (passed -> tomorrow) */
        {"6:00", "2026-10-08 06:00", "2026-10-08 07:00", ""},
        {"6am", "2026-10-08 06:00", "2026-10-08 07:00", ""},
        {"7", "2026-10-08 07:00", "2026-10-08 08:00", ""},
        {"12am", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        {"12pm", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"9:50", "2026-10-07 09:50", "2026-10-07 10:50", ""}, /* only 10 min ago: stays today */
        {"9:40", "2026-10-08 09:40", "2026-10-08 10:40", ""}, /* 20 min ago: tomorrow */
    };
    RUN(cases);
}

TEST(when_time_ranges) {
    static const Case cases[] = {
        {"14-17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14–17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14—17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14 - 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14:00-17:30", "2026-10-07 14:00", "2026-10-07 17:30", ""},
        {"14.00-17.30", "2026-10-07 14:00", "2026-10-07 17:30", ""},
        {"2-5pm", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"2-5 pm", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"2 – 5 pm", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"2pm-5pm", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"2pm-5", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"2 - 5", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14 to 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14 till 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"14 until 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"from 14 to 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"från 14 till 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"från kl 14 till 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"mellan 14 och 17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"between 2 and 5pm", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"kl 14-17", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"22-02", "2026-10-07 22:00", "2026-10-08 02:00", ""},
        {"22-2", "2026-10-07 22:00", "2026-10-08 02:00", ""},
        {"22-midnight", "2026-10-07 22:00", "2026-10-08 00:00", ""},
        {"19-24", "2026-10-07 19:00", "2026-10-08 00:00", ""},
        {"6-8", "2026-10-07 18:00", "2026-10-07 20:00", ""},
        {"11-2", "2026-10-07 11:00", "2026-10-07 14:00", ""},
        {"11-1pm", "2026-10-07 11:00", "2026-10-07 13:00", ""},
        {"10am-2", "2026-10-07 10:00", "2026-10-07 14:00", ""},
        {"9-5", "2026-10-08 09:00", "2026-10-08 17:00", ""}, /* 09:00 already passed today */
        {"fri 9-11am", "2026-10-09 09:00", "2026-10-09 11:00", ""},
        {"fri 7-9", "2026-10-09 07:00", "2026-10-09 09:00", ""},
        {"imorgon 08-10", "2026-10-08 08:00", "2026-10-08 10:00", ""},
        {"fri 12-2pm", "2026-10-09 12:00", "2026-10-09 14:00", ""},
    };
    RUN(cases);
}

TEST(when_durations) {
    static const Case cases[] = {
        {"2h", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"2 h", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"2 hours", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"2 timmar", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"for 2 hours", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"i 2 timmar", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"för 2 timmar", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"two hours", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"två timmar", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"1.5h", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"1,5h", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"1.5 hours", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"90m", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"90 min", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"90 minutes", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"90 minuter", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"45min", "2026-10-07 10:00", "2026-10-07 10:45", ""},
        {"1h30", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"1h30m", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"1 hour 30 min", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"2 timmar och 30 minuter", "2026-10-07 10:00", "2026-10-07 12:30", ""},
        {"an hour and a half", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"one and a half hours", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"en och en halv timme", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"an hour", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"en timme", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"half an hour", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"a half hour", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"en halvtimme", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"en kvart", "2026-10-07 10:00", "2026-10-07 10:15", ""},
        {"quarter of an hour", "2026-10-07 10:00", "2026-10-07 10:15", ""},
        {"fri 14 for 2h", "2026-10-09 14:00", "2026-10-09 16:00", ""},
        {"fri for 3 days", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"fre 2 nätter", "2026-10-09 00:00", "2026-10-11 00:00", ""},
        {"fri for a week", "2026-10-09 00:00", "2026-10-16 00:00", ""},
        {"fri 3d", "2026-10-09 00:00", "2026-10-12 00:00", ""},
    };
    RUN(cases);
}

TEST(when_relative_starts) {
    static const Case cases[] = {
        {"now", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"nu", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"right now", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"now for 2h", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"nu 2h", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"in 30 min", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"om 30 min", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"in 30 minutes", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"om 30 minuter", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"in 2 hours", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"in 2h", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"om 2 timmar", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"om en kvart", "2026-10-07 10:15", "2026-10-07 11:15", ""},
        {"in an hour", "2026-10-07 11:00", "2026-10-07 12:00", ""},
        {"om en timme", "2026-10-07 11:00", "2026-10-07 12:00", ""},
        {"in half an hour", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"om en halvtimme", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"in 45 minutes for 1h", "2026-10-07 10:45", "2026-10-07 11:45", ""},
        {"nu till 17", "2026-10-07 10:00", "2026-10-07 17:00", ""},
        {"till 17", "2026-10-07 10:00", "2026-10-07 17:00", ""},
        {"until 5", "2026-10-07 10:00", "2026-10-07 17:00", ""},
        {"today until 5pm", "2026-10-07 10:00", "2026-10-07 17:00", ""},
    };
    RUN(cases);
}

TEST(when_leaves_other_words_alone) {
    static const Case cases[] = {
        {"car fri 14-17 ikea trip", "2026-10-09 14:00", "2026-10-09 17:00", "car ikea trip"},
        {"laundry on friday at 6", "2026-10-09 18:00", "2026-10-09 19:00", "laundry"},
        {"Bastu på lördag kl 19-21", "2026-10-10 19:00", "2026-10-10 21:00", "Bastu"},
        {"pick up son from school at 15", "2026-10-07 15:00", "2026-10-07 16:00", "pick up son from school"},
        {"call mom in the evening", "-", "-", "call mom in the evening"},
        {"for eva fri", "2026-10-09 00:00", "2026-10-10 00:00", "for eva"},
        {"may I borrow the car fri 14", "2026-10-09 14:00", "2026-10-09 15:00", "may I borrow the car"},
        {"room 2 fri 14-17", "2026-10-09 14:00", "2026-10-09 17:00", "room 2"},
        {"room 2 fri 14", "2026-10-09 14:00", "2026-10-09 15:00", "room 2"},
        {"car 3", "2026-10-07 15:00", "2026-10-07 16:00", "car"},
        {"åka till ikea imorgon 10", "2026-10-08 10:00", "2026-10-08 11:00", "åka till ikea"},
        {"Tvättstugan, fredag 7-10!", "2026-10-09 07:00", "2026-10-09 10:00", "Tvättstugan"},
        {"guest room fri for 3 days", "2026-10-09 00:00", "2026-10-12 00:00", "guest room"},
        {"vacuum the living room", "-", "-", "vacuum the living room"},
        {"next to nothing", "-", "-", "next to nothing"},
        {"every now and then", "2026-10-07 10:00", "2026-10-07 11:00", "every and then"},
        {"bike on the 14th", "2026-10-14 00:00", "2026-10-15 00:00", "bike"},
    };
    RUN(cases);
}

TEST(when_default_duration_is_used) {
    NOW = dt_make(2026, 10, 7, 10, 0);
    Case c = {"fri 14", "2026-10-09 14:00", "2026-10-09 17:00", ""};
    check_case(&c, 180);
    Case d = {"fri 14", "2026-10-09 14:00", "2026-10-09 15:00", ""};
    check_case(&d, 0); /* <= 0 falls back to an hour */
}

typedef struct {
    const char *in;
    int repeat, wday;
    const char *date; /* NULL = no date */
    const char *rest;
} RepeatCase;

TEST(when_repeats) {
    static const RepeatCase cases[] = {
        {"vacuum every week", REPEAT_WEEKLY, -1, NULL, "vacuum"},
        {"every day", REPEAT_DAILY, -1, NULL, ""},
        {"daily", REPEAT_DAILY, -1, NULL, ""},
        {"varje dag", REPEAT_DAILY, -1, NULL, ""},
        {"dagligen", REPEAT_DAILY, -1, NULL, ""},
        {"every week", REPEAT_WEEKLY, -1, NULL, ""},
        {"weekly", REPEAT_WEEKLY, -1, NULL, ""},
        {"varje vecka", REPEAT_WEEKLY, -1, NULL, ""},
        {"veckovis", REPEAT_WEEKLY, -1, NULL, ""},
        {"every other week", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"every 2 weeks", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"varannan vecka", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"biweekly", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"every month", REPEAT_MONTHLY, -1, NULL, ""},
        {"monthly", REPEAT_MONTHLY, -1, NULL, ""},
        {"varje månad", REPEAT_MONTHLY, -1, NULL, ""},
        {"månadsvis", REPEAT_MONTHLY, -1, NULL, ""},
        {"every monday", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"varje måndag", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"mondays", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"måndagar", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"on mondays", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"på måndagar", REPEAT_WEEKLY, 0, "2026-10-12", ""},
        {"every wednesday", REPEAT_WEEKLY, 2, "2026-10-07", ""},
        {"trash every tuesday 19", REPEAT_WEEKLY, 1, "2026-10-13", "trash"},
        {"varannan fredag", REPEAT_BIWEEKLY, 4, "2026-10-09", ""},
        {"every other friday", REPEAT_BIWEEKLY, 4, "2026-10-09", ""},
        {"water plants @eva every sunday", REPEAT_WEEKLY, 6, "2026-10-11", "water plants @eva"},
    };
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    for (int i = 0; i < countof(cases); i++) {
        const RepeatCase *c = &cases[i];
        Tokens t;
        When w;
        tok_split(&t, c->in);
        bool any = when_parse(&t, now, &w);
        char date[16] = "", rest[128];
        if (w.has_date) snprintf(date, sizeof date, "%04d-%02d-%02d", w.y, w.m, w.d);
        tok_rest(&t, rest, sizeof rest);
        test_checks++;
        if (!any || w.repeat != c->repeat || w.repeat_wday != c->wday || w.has_date != (c->date != NULL) ||
            (c->date && !str_eq(date, c->date)) || !str_eq(rest, c->rest))
            test_fail(__FILE__, __LINE__, "\"%s\": repeat %d wday %d date \"%s\" rest \"%s\"", c->in, w.repeat,
                      w.repeat_wday, date, rest);
    }
    /* repeat + time resolves to the first occurrence */
    Tokens t;
    When w;
    int64_t s, e;
    tok_split(&t, "trash every tuesday 19");
    CHECK(when_parse(&t, now, &w));
    CHECK(when_resolve(&w, now, 30, &s, &e));
    char buf[32];
    fmt_t(s, buf, sizeof buf);
    CHECK_STR(buf, "2026-10-13 19:00");
    /* repeat without a date resolves to today, all day */
    tok_split(&t, "vacuum every week");
    CHECK(when_parse(&t, now, &w));
    CHECK(when_resolve(&w, now, 30, &s, &e));
    fmt_t(s, buf, sizeof buf);
    CHECK_STR(buf, "2026-10-07 00:00");
}

TEST(when_fields_are_exposed) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    Tokens t;
    When w;
    tok_split(&t, "fri-sun");
    CHECK(when_parse(&t, now, &w));
    CHECK(w.has_date && w.y == 2026 && w.m == 10 && w.d == 9);
    CHECK(w.has_end_date && w.ey == 2026 && w.em == 10 && w.ed == 11);
    CHECK(!w.has_start && !w.has_end && w.dur_min == 0 && !w.now);

    tok_split(&t, "kl 19-21");
    CHECK(when_parse(&t, now, &w));
    CHECK(!w.has_date && w.has_start && w.start_min == 19 * 60 && w.has_end && w.end_min == 21 * 60);

    tok_split(&t, "om 20 min i 2 timmar");
    CHECK(when_parse(&t, now, &w));
    CHECK(w.now && w.rel_min == 20 && w.dur_min == 120);

    tok_split(&t, "car");
    CHECK(!when_parse(&t, now, &w));
    int64_t s = 1, e = 1;
    CHECK(!when_resolve(&w, now, 60, &s, &e));
    CHECK(s == 1 && e == 1);
}

TEST(when_skips_tokens_the_caller_claimed) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    Tokens t;
    When w;
    int64_t s, e;
    tok_split(&t, "Room 2 fri 14");
    CHECK_INT(t.n, 4);
    t.used[0] = t.used[1] = true; /* the booking module recognised "Room 2" */
    CHECK(when_parse(&t, now, &w));
    CHECK(when_resolve(&w, now, 60, &s, &e));
    char buf[32], rest[64];
    fmt_t(s, buf, sizeof buf);
    CHECK_STR(buf, "2026-10-09 14:00");
    tok_rest(&t, rest, sizeof rest);
    CHECK_STR(rest, "");
}

TEST(tok_split_and_rest) {
    Tokens t;
    tok_split(&t, "  Bil,  fre\t14–17!  \n  IKEA-resa?  ");
    CHECK_INT(t.n, 4);
    CHECK_STR(t.raw[0], "Bil");
    CHECK_STR(t.low[0], "bil");
    CHECK_STR(t.raw[2], "14–17");
    CHECK_STR(t.low[2], "14-17");
    CHECK_STR(t.raw[3], "IKEA-resa");
    CHECK_STR(t.low[3], "ikea-resa");
    char rest[64];
    tok_rest(&t, rest, sizeof rest);
    CHECK_STR(rest, "Bil fre 14–17 IKEA-resa");
    t.used[1] = t.used[2] = true;
    tok_rest(&t, rest, sizeof rest);
    CHECK_STR(rest, "Bil IKEA-resa");

    tok_split(&t, "kl. 14 p.m. ÅÄÖ");
    CHECK_STR(t.low[0], "kl");
    CHECK_STR(t.low[2], "p.m");
    CHECK_STR(t.low[3], "åäö");

    tok_split(&t, "no\xC2\xA0"
                  "break space"); /* U+00A0 separates tokens too */
    CHECK_INT(t.n, 3);

    /* truncation never splits a UTF-8 sequence */
    tok_split(&t, "åäö åäö åäö");
    char small[8];
    tok_rest(&t, small, sizeof small);
    CHECK(utf8_valid(small, strlen(small)));
    CHECK(strlen(small) < sizeof small);
    CHECK_STR(small, "åäö"); /* "åäö åäö" needs 13 bytes; only the first word fits whole */
    char one[1];
    tok_rest(&t, one, sizeof one);
    CHECK_STR(one, "");
}

TEST(when_describe_formats) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    char out[96];
    struct {
        int64_t s, e;
        const char *want;
    } cases[] = {
        {dt_make(2026, 10, 7, 19, 0), dt_make(2026, 10, 7, 21, 0), "Today 19:00–21:00"},
        {dt_make(2026, 10, 8, 9, 0), dt_make(2026, 10, 8, 10, 30), "Tomorrow 09:00–10:30"},
        {dt_make(2026, 10, 6, 10, 0), dt_make(2026, 10, 6, 11, 0), "Yesterday 10:00–11:00"},
        {dt_make(2026, 10, 16, 14, 0), dt_make(2026, 10, 16, 17, 0), "Fri 16 Oct 14:00–17:00"},
        {dt_make(2026, 10, 16, 14, 0), dt_make(2026, 10, 17, 10, 0), "Fri 16 Oct 14:00 – Sat 17 Oct 10:00"},
        {dt_make(2026, 10, 7, 22, 0), dt_make(2026, 10, 8, 2, 0), "Today 22:00 – Tomorrow 02:00"},
        {dt_make(2026, 10, 7, 22, 0), dt_make(2026, 10, 8, 0, 0), "Today 22:00–00:00"},
        {dt_make(2026, 10, 17, 0, 0), dt_make(2026, 10, 20, 0, 0), "Sat 17 – Mon 19 Oct"},
        {dt_make(2026, 10, 31, 0, 0), dt_make(2026, 11, 3, 0, 0), "Sat 31 Oct – Mon 2 Nov"},
        {dt_make(2026, 12, 30, 0, 0), dt_make(2027, 1, 2, 0, 0), "Wed 30 Dec – Fri 1 Jan 2027"},
        {dt_make(2026, 10, 7, 0, 0), dt_make(2026, 10, 8, 0, 0), "Today, all day"},
        {dt_make(2026, 10, 17, 0, 0), dt_make(2026, 10, 18, 0, 0), "Sat 17 Oct, all day"},
        {dt_make(2027, 1, 5, 10, 0), dt_make(2027, 1, 5, 11, 0), "Tue 5 Jan 2027 10:00–11:00"},
        {dt_make(2026, 10, 7, 10, 0), dt_make(2026, 10, 7, 10, 0), "Today 10:00"},
    };
    for (int i = 0; i < countof(cases); i++) {
        when_describe(cases[i].s, cases[i].e, now, out, sizeof out);
        CHECK_STR(out, cases[i].want);
    }
    char tiny[8];
    when_describe(dt_make(2026, 10, 16, 14, 0), dt_make(2026, 10, 17, 10, 0), now, tiny, sizeof tiny);
    CHECK(utf8_valid(tiny, strlen(tiny)));
    CHECK_STR(tiny, "Fri 16 ");
}

TEST(when_survives_garbage) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    static const char *const inputs[] = {
        "", "   ", "!!! ... ???", ",", "-", "–", "--", "14-", "-17", "1-2-3", "99:99", "25", "12/13", "0/0",
        "2026-13-45", "kl", "next", "every", "in", "om", "between", "between 2 and", "the", "den", "i", "half",
        "a", "an hour and a", "12th of", "oct", "1h30x", "1e9h", "999999999h", "9999999h", "0h", ":", "12:",
        ":30", "1.5.5h", "p.m", "pm", "2026-10-1", "29/2", "31/4", "\xff\xfe 14 \xc3", "until", "till",
        "från", "mellan och", "every other", "varannan", "next next next", "12-14", "12-14 okt 2026 kl",
        "den 31", "0", "00", "24", "24:30", "13pm", "0am", "12:60", "fri-", "-sun", "fri-xyz", "1/1/1/1",
        "i i i i", "om om 5", "in in 5 min", "half half hour", "en och en", "for for 2h",
    };
    for (int i = 0; i < countof(inputs); i++) {
        Tokens t;
        When w;
        int64_t s, e;
        tok_split(&t, inputs[i]);
        when_parse(&t, now, &w);
        if (when_resolve(&w, now, 60, &s, &e)) CHECK(e > s);
        char rest[128], desc[64];
        tok_rest(&t, rest, sizeof rest);
        when_describe(now, now + 3600, now, desc, sizeof desc);
    }
    /* huge tokens are truncated, too many tokens are dropped */
    char big[220];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    Tokens t;
    tok_split(&t, big);
    CHECK_INT(t.n, 1);
    CHECK(strlen(t.raw[0]) < TOK_LEN);
    Buf many = {0};
    for (int i = 0; i < 60; i++) buf_puts(&many, "fri ");
    tok_split(&t, buf_cstr(&many));
    CHECK_INT(t.n, TOK_MAX);
    When w;
    CHECK(when_parse(&t, now, &w));
    buf_free(&many);
    tok_split(&t, NULL);
    CHECK_INT(t.n, 0);
    CHECK(!when_parse(&t, now, &w));
}

TEST(when_fuzz_random_phrases) {
    static const char *const pool[] = {
        "car",   "fri",   "sun",   "14",     "17",     "-",      "to",    "till",  "until", "2pm",   "pm",
        "am",    "kl",    "at",    "on",     "på",     "next",   "nästa", "every", "varje", "week",  "vecka",
        "2h",    "90",    "min",   "hours",  "timmar", "in",     "om",    "now",   "nu",    "12/10", "14/10",
        "12-14", "okt",   "oct",   "the",    "den",    "12th",   "3",     "06",    "22-02", "6-8",   "half",
        "an",    "en",    "kvart", "and",    "och",    "between", "mellan", "from", "från",  "i",     "dag",
        "morgon", "helgen", "weekend", "tomorrow", "idag", "1.5h", "for", "för",  "2026",  "mondays", "28/12",
        "2/1",   "noon",  "midnight", "24",  "0",      "fri-sun", "a",    "quarter", "of",  "hour",  "x",
        "tonight", "ikväll", "kväll", "8",  "7-9",    "12",     "8:30",  "31",    "sep",   "29/2",  "31/9",
        "10/10", "14-12", "2026-09-31", "02:00-03:00", "02:30", "01:30-02:30", "2026-03-29", "2026-10-25", "today",
        "evening", "night", "inatt", "natt", "1am", "14-12/10", "28-3/10", "1",
    };
    /* an ordinary day, and the two nights the clocks change in much of Europe */
    const int64_t nows[] = {dt_make(2026, 10, 7, 10, 0), dt_make(2026, 3, 28, 10, 0), dt_make(2026, 10, 24, 10, 0)};
    uint32_t seed = 12345;
    char phrase[512], desc[96];
    for (int iter = 0; iter < 6000; iter++) {
        phrase[0] = '\0';
        size_t len = 0;
        seed = seed * 1103515245u + 12345u;
        int n = 1 + (int)((seed >> 16) % 10);
        for (int k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            const char *wd = pool[(seed >> 16) % (uint32_t)countof(pool)];
            len += (size_t)snprintf(phrase + len, sizeof phrase - len, "%s%s", k ? " " : "", wd);
        }
        int64_t now = nows[iter % countof(nows)];
        Tokens t;
        When w;
        int64_t s, e;
        tok_split(&t, phrase);
        t.used[0] = iter & 1; /* sometimes the caller claimed the first word */
        bool any = when_parse(&t, now, &w);
        if (w.refused[0]) { /* refused: false, nothing to resolve, the words as they were */
            bool restored = t.used[0] == (iter & 1);
            for (int k = 1; k < t.n; k++) restored = restored && !t.used[k];
            test_checks++;
            if (any || when_resolve(&w, now, 60, &s, &e) || !restored) {
                test_fail(__FILE__, __LINE__, "\"%s\": refused (%s) but not cleanly", phrase, w.refused);
                return;
            }
            continue;
        }
        if (any != when_resolve(&w, now, 60, &s, &e)) {
            test_fail(__FILE__, __LINE__, "\"%s\": parsed %d but resolved %d", phrase, any, !any);
            return;
        }
        if (any) {
            test_checks++;
            if (e <= s) {
                test_fail(__FILE__, __LINE__, "\"%s\": end <= start", phrase);
                return;
            }
            when_describe(s, e, now, desc, sizeof desc);
            if (!utf8_valid(desc, strlen(desc)) || !desc[0]) {
                test_fail(__FILE__, __LINE__, "\"%s\": bad description", phrase);
                return;
            }
        }
    }
}

/* ---------------------------------------------------------------- Spanish (issue #46) */

/* The cases in another language of the person's (the person's language decides what a word
 * Spanish shares with Swedish means: "el 12", "dom"). */
#define RUN_IN(lang, cases)                                                                                            \
    do {                                                                                                               \
        Lang was_ = i18n_use(lang);                                                                                    \
        RUN(cases);                                                                                                    \
        i18n_use(was_);                                                                                                \
    } while (0)

TEST(when_spanish_days) {
    static const Case cases[] = {
        {"hoy 19-21", "2026-10-07 19:00", "2026-10-07 21:00", ""},
        {"hoy a las 19", "2026-10-07 19:00", "2026-10-07 20:00", ""},
        {"mañana a las 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"manana a las 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"MAÑANA 9", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"pasado mañana a las 12", "2026-10-09 12:00", "2026-10-09 13:00", ""},
        {"pasado manana", "2026-10-09 00:00", "2026-10-10 00:00", ""},
        {"el lunes", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"lunes 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"lun 8", "2026-10-12 08:00", "2026-10-12 09:00", ""},
        {"martes 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"el martes a las 8", "2026-10-13 08:00", "2026-10-13 09:00", ""},
        {"miércoles 18", "2026-10-07 18:00", "2026-10-07 19:00", ""}, /* today counts */
        {"Miércoles 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"miercoles 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"mié 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"mie 18", "2026-10-07 18:00", "2026-10-07 19:00", ""},
        {"jueves 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"jue 18", "2026-10-08 18:00", "2026-10-08 19:00", ""},
        {"viernes 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"el vie 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"sábado 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"SÁBADO 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"sabado 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"sáb 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"sab 10", "2026-10-10 10:00", "2026-10-10 11:00", ""},
        {"domingo 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"el dom a las 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"el domingo, a las 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        /* "próximo" / "que viene": the next one to come (not next week's, as English "next fri") */
        {"el próximo lunes", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"el proximo viernes", "2026-10-09 00:00", "2026-10-10 00:00", ""},
        {"próximo viernes a las 10", "2026-10-09 10:00", "2026-10-09 11:00", ""},
        {"el viernes que viene", "2026-10-09 00:00", "2026-10-10 00:00", ""},
        {"el lunes próximo", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"el miércoles que viene", "2026-10-14 00:00", "2026-10-15 00:00", ""}, /* never today */
        {"el próximo miércoles", "2026-10-14 00:00", "2026-10-15 00:00", ""},
        {"el viernes de la semana que viene", "2026-10-16 00:00", "2026-10-17 00:00", ""},
        {"el viernes de la próxima semana", "2026-10-16 00:00", "2026-10-17 00:00", ""},
        {"este viernes a las 10", "2026-10-09 10:00", "2026-10-09 11:00", ""},
        {"este miércoles", "2026-10-07 00:00", "2026-10-08 00:00", ""},
        {"la semana que viene", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"la próxima semana", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"proxima semana", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"semana que viene", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"la semana próxima", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        /* the weekend */
        {"este fin de semana", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"el fin de semana", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"fin de semana", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"finde", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"este finde", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"el próximo fin de semana", "2026-10-10 00:00", "2026-10-12 00:00", ""}, /* the coming one */
        {"el finde que viene", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        /* a weekday with its date: the date says which */
        {"el lunes 9 de noviembre a las 10", "2026-11-09 10:00", "2026-11-09 11:00", ""},
        {"lunes, 9 de noviembre", "2026-11-09 00:00", "2026-11-10 00:00", ""},
        /* "el jueves 12": the Thursday that is a 12th ("jueves 12" without "el" is Thursday at 12) */
        {"médico el jueves 12 a las 9:30", "2026-11-12 09:30", "2026-11-12 10:30", "médico"},
        {"el martes 13", "2026-10-13 00:00", "2026-10-14 00:00", ""},
        {"el dom 10", "2027-01-10 00:00", "2027-01-11 00:00", ""},
        {"jueves 12", "2026-10-08 12:00", "2026-10-08 13:00", ""},
        {"el lunes 8 pm", "2026-10-12 20:00", "2026-10-12 21:00", ""},
        {"el viernes 2 horas", "2026-10-09 00:00", "2026-10-09 02:00", ""},
    };
    RUN(cases);

    static const Case saturday[] = { /* Saturday 10 Oct: this weekend is under way */
        {"el próximo fin de semana", "2026-10-17 00:00", "2026-10-19 00:00", ""},
        {"el fin de semana que viene", "2026-10-17 00:00", "2026-10-19 00:00", ""},
        {"este fin de semana", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"el próximo sábado", "2026-10-17 00:00", "2026-10-18 00:00", ""},
        {"este sábado", "2026-10-10 00:00", "2026-10-11 00:00", ""},
    };
    NOW = dt_make(2026, 10, 10, 10, 0);
    for (int i = 0; i < countof(saturday); i++) check_case(&saturday[i], 60);
}

TEST(when_spanish_dates) {
    static const Case cases[] = {
        {"5/11", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"5 de noviembre", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"el 5 de noviembre a las 10", "2026-11-05 10:00", "2026-11-05 11:00", ""},
        {"5 nov", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"5 de nov", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"5 noviembre", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"el 5 de noviembre de 2027", "2027-11-05 00:00", "2027-11-06 00:00", ""},
        {"5 de noviembre 2027", "2027-11-05 00:00", "2027-11-06 00:00", ""},
        {"1 de enero", "2027-01-01 00:00", "2027-01-02 00:00", ""},
        {"3 ene", "2027-01-03 00:00", "2027-01-04 00:00", ""},
        {"14 de febrero", "2027-02-14 00:00", "2027-02-15 00:00", ""},
        {"19 de marzo", "2027-03-19 00:00", "2027-03-20 00:00", ""},
        {"1 de abril", "2027-04-01 00:00", "2027-04-02 00:00", ""},
        {"12 abr", "2027-04-12 00:00", "2027-04-13 00:00", ""},
        {"2 de mayo", "2027-05-02 00:00", "2027-05-03 00:00", ""},
        {"24 de junio", "2027-06-24 00:00", "2027-06-25 00:00", ""},
        {"25 de julio", "2027-07-25 00:00", "2027-07-26 00:00", ""},
        {"15 de agosto", "2027-08-15 00:00", "2027-08-16 00:00", ""},
        {"15 de ago", "2027-08-15 00:00", "2027-08-16 00:00", ""},
        {"30 de septiembre", "2027-09-30 00:00", "2027-10-01 00:00", ""},
        {"30 de setiembre", "2027-09-30 00:00", "2027-10-01 00:00", ""},
        {"12 de octubre", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"24 dic", "2026-12-24 00:00", "2026-12-25 00:00", ""},
        {"31 de diciembre a las 22", "2026-12-31 22:00", "2026-12-31 23:00", ""},
        {"el día 12", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"día 3", "2026-11-03 00:00", "2026-11-04 00:00", ""}, /* passed -> next month */
        {"el día 12 de noviembre", "2026-11-12 00:00", "2026-11-13 00:00", ""},
        /* day ranges */
        {"12-14 de noviembre", "2026-11-12 00:00", "2026-11-15 00:00", ""},
        {"del 5 al 10 de noviembre", "2026-11-05 00:00", "2026-11-11 00:00", ""},
        {"5 al 10 de noviembre", "2026-11-05 00:00", "2026-11-11 00:00", ""},
        {"de 5 a 10 de noviembre", "2026-11-05 00:00", "2026-11-11 00:00", ""},
        {"del 28 de octubre al 3 de noviembre", "2026-10-28 00:00", "2026-11-04 00:00", ""},
        {"del 28/12 al 2/1", "2026-12-28 00:00", "2027-01-03 00:00", ""},
        {"del 3 al 5", "2026-11-03 00:00", "2026-11-06 00:00", ""}, /* the next 3rd: November's */
        {"del 28 al 3", "2026-10-28 00:00", "2026-11-04 00:00", ""},
        {"viaje del 1 de diciembre al 3", "2026-12-01 00:00", "2026-12-04 00:00", "viaje"},
        {"del 28 de diciembre al 2", "2026-12-28 00:00", "2027-01-03 00:00", ""},
        {"del 10 al 12 a las 9", "2026-10-10 09:00", "2026-10-13 00:00", ""},
        {"vacaciones del 20 de diciembre al 6 de enero", "2026-12-20 00:00", "2027-01-07 00:00", "vacaciones"},
    };
    RUN(cases);
}

TEST(when_spanish_refusals_and_swedish_ones_speak_their_language) {
    static const RefuseCase es[] = {
        {"31 de septiembre", "No existe el 31 de septiembre"},
        {"cena el 31 de septiembre a las 10", "No existe el 31 de septiembre"},
        {"31/9", "No existe el 31 de septiembre"},
        {"29 de febrero de 2027", "No existe el 29 de febrero de 2027"},
        {"29/2", "No existe el 29 de febrero de 2027"},
        {"del 10 al 5 de noviembre", "La fecha de fin es anterior a la de inicio"},
        {"del 12/10 al 10/10", "La fecha de fin es anterior a la de inicio"},
        {"hasta el 5/10", "La fecha de fin es anterior a la de inicio"},
        {"1/1/1960 10", "Elige un año entre 1970 y 2200"},
    };
    Lang was = i18n_use(LANG_ES);
    RUN_REFUSED(es);
    static const RefuseCase sv[] = {
        {"31 sep 14", "Det finns ingen 31 september"},
        {"bil 31 sep 14-16", "Det finns ingen 31 september"},
        {"29/2", "Det finns ingen 29 februari 2027"},
        {"12/10-10/10", "Slutdatumet är före startdatumet"},
        {"until 5/10", "Slutdatumet är före startdatumet"},
        {"1/1/1960 10", "Välj ett år från 1970 till 2200"},
    };
    i18n_use(LANG_SV);
    RUN_REFUSED(sv);
    i18n_use(was);
    /* every reason fits When.refused in every language: the longest ones */
    char why[64];
    Lang langs[] = {LANG_EN, LANG_SV, LANG_ES};
    for (int i = 0; i < countof(langs); i++) {
        CHECK((size_t)snprintf(why, sizeof why, T_in(langs[i], "There's no %d %s %d"), 29, "septiembre", 2027) <
              sizeof why);
        CHECK(strlen(T_in(langs[i], "The end date is before the start date")) < sizeof why);
        CHECK(strlen(T_in(langs[i], "Pick a year from 1970 to 2200")) < sizeof why);
    }
}

TEST(when_spanish_evenings_afternoons_and_mornings) {
    static const Case cases[] = {
        {"esta noche a las 8", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"esta noche 8", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"esta noche a las 12", "2026-10-08 00:00", "2026-10-08 01:00", ""}, /* midnight, not noon */
        {"esta noche a la 1", "2026-10-08 01:00", "2026-10-08 02:00", ""},
        {"esta noche de 9 a 11", "2026-10-07 21:00", "2026-10-07 23:00", ""},
        {"esta noche 20", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"esta tarde a las 4", "2026-10-07 16:00", "2026-10-07 17:00", ""},
        {"esta tarde a las 4:30", "2026-10-07 16:30", "2026-10-07 17:30", ""},
        {"esta tarde de 3 a 5", "2026-10-07 15:00", "2026-10-07 17:00", ""},
        {"esta mañana a las 9", "2026-10-07 09:00", "2026-10-07 10:00", ""},
        {"mañana por la noche a las 9", "2026-10-08 21:00", "2026-10-08 22:00", ""},
        {"mañana por la tarde a las 5", "2026-10-08 17:00", "2026-10-08 18:00", ""},
        {"mañana por la mañana a las 6", "2026-10-08 06:00", "2026-10-08 07:00", ""}, /* not 18:00 */
        {"mañana tarde 5", "2026-10-08 17:00", "2026-10-08 18:00", ""},
        {"mañana noche 10", "2026-10-08 22:00", "2026-10-08 23:00", ""},
        {"mañana en la noche a las 10", "2026-10-08 22:00", "2026-10-08 23:00", ""}, /* Latin America */
        {"el viernes por la noche a las 11", "2026-10-09 23:00", "2026-10-10 00:00", ""},
        {"viernes noche 11", "2026-10-09 23:00", "2026-10-10 00:00", ""},
        {"el sábado de madrugada a las 3", "2026-10-10 03:00", "2026-10-10 04:00", ""},
        {"mañana por la noche", "2026-10-08 00:00", "2026-10-09 00:00", ""}, /* the words are taken; all day */
        /* a time typed before its day */
        {"a las 8 esta noche", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"a las 4 esta tarde", "2026-10-07 16:00", "2026-10-07 17:00", ""},
        {"a las 6 mañana por la mañana", "2026-10-08 06:00", "2026-10-08 07:00", ""},
        /* a time belongs to the day right before it */
        {"esta noche hasta mañana a las 10", "2026-10-07 10:00", "2026-10-08 10:00", ""},
        {"hasta mañana por la noche a las 8", "2026-10-07 10:00", "2026-10-08 20:00", ""},
        {"esta noche hasta las 11", "2026-10-07 10:00", "2026-10-07 23:00", ""},
        {"hoy de 8 a 10 de la noche", "2026-10-07 20:00", "2026-10-07 22:00", ""},
    };
    RUN(cases);
    static const Case at_0010[] = {
        {"esta noche a las 12:30", "2026-10-07 00:30", "2026-10-07 01:30", ""}, /* in 20 minutes */
        {"esta noche a las 2", "2026-10-07 02:00", "2026-10-07 03:00", ""},
    };
    NOW = dt_make(2026, 10, 7, 0, 10);
    for (int i = 0; i < countof(at_0010); i++) check_case(&at_0010[i], 60);
}

TEST(when_spanish_times) {
    static const Case cases[] = {
        {"a las 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las 14:00", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las 14.30", "2026-10-07 14:30", "2026-10-07 15:30", ""},
        {"a las 9", "2026-10-08 09:00", "2026-10-08 10:00", ""}, /* passed -> tomorrow */
        {"a las 5", "2026-10-07 17:00", "2026-10-07 18:00", ""}, /* like "at 5" */
        {"a la 1", "2026-10-07 13:00", "2026-10-07 14:00", ""},
        {"a la una", "2026-10-07 13:00", "2026-10-07 14:00", ""},
        {"a las dos", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las doce", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"sobre las 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"hacia las 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"para las 14", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las 5 de la tarde", "2026-10-07 17:00", "2026-10-07 18:00", ""},
        {"a las 8 de la tarde", "2026-10-07 20:00", "2026-10-07 21:00", ""},
        {"a las 11 de la mañana", "2026-10-07 11:00", "2026-10-07 12:00", ""},
        {"a las 9 de la mañana", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"a las 10 de la noche", "2026-10-07 22:00", "2026-10-07 23:00", ""},
        {"a las 12 de la noche", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        {"a la 1 de la noche", "2026-10-08 01:00", "2026-10-08 02:00", ""},
        {"a las 3 de la madrugada", "2026-10-08 03:00", "2026-10-08 04:00", ""},
        {"a las 5 por la tarde", "2026-10-07 17:00", "2026-10-07 18:00", ""},
        {"5 de la tarde", "2026-10-07 17:00", "2026-10-07 18:00", ""},
        {"a las 12 del mediodía", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"a la una del mediodía", "2026-10-07 13:00", "2026-10-07 14:00", ""},
        {"a las 8 y media", "2026-10-08 08:30", "2026-10-08 09:30", ""},
        {"a las 5 y media", "2026-10-07 17:30", "2026-10-07 18:30", ""},
        {"a las 4 y cuarto", "2026-10-07 16:15", "2026-10-07 17:15", ""},
        {"a las 7 menos cuarto", "2026-10-08 06:45", "2026-10-08 07:45", ""},
        {"a las 2 menos cuarto", "2026-10-07 13:45", "2026-10-07 14:45", ""},
        {"a las 9 y media de la noche", "2026-10-07 21:30", "2026-10-07 22:30", ""},
        {"a las 9 en punto", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"a las 14h", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las 14:30h", "2026-10-07 14:30", "2026-10-07 15:30", ""},
        {"a las 9h", "2026-10-08 09:00", "2026-10-08 10:00", ""},
        {"a las 14 horas", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"a las 14 hs", "2026-10-07 14:00", "2026-10-07 15:00", ""},
        {"al mediodía", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"al mediodia", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"a medianoche", "2026-10-08 00:00", "2026-10-08 01:00", ""},
        /* ranges */
        {"de 14 a 16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"de 14:00 a 16:30", "2026-10-07 14:00", "2026-10-07 16:30", ""},
        {"desde las 14 hasta las 16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"desde 14 hasta 16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"de las 14 a las 16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"entre las 14 y las 16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"entre 2 y 4", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"14-16", "2026-10-07 14:00", "2026-10-07 16:00", ""},
        {"de 10 a 2", "2026-10-07 10:00", "2026-10-07 14:00", ""},
        {"de 2 a 5 de la tarde", "2026-10-07 14:00", "2026-10-07 17:00", ""},
        {"de 8 a 10 de la noche", "2026-10-07 20:00", "2026-10-07 22:00", ""},
        {"8-10 de la noche", "2026-10-07 20:00", "2026-10-07 22:00", ""},
        {"de 10 de la noche a 2", "2026-10-07 22:00", "2026-10-08 02:00", ""},
        {"de 9 a 11 de la mañana", "2026-10-08 09:00", "2026-10-08 11:00", ""},
        {"de la una a las tres", "2026-10-07 13:00", "2026-10-07 15:00", ""},
        {"hasta las 17", "2026-10-07 10:00", "2026-10-07 17:00", ""},
        {"hasta la 1", "2026-10-07 10:00", "2026-10-07 13:00", ""},
        {"viernes de 18 a 20", "2026-10-09 18:00", "2026-10-09 20:00", ""},
    };
    RUN(cases);
}

TEST(when_spanish_durations_and_now) {
    static const Case cases[] = {
        {"2 horas", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"2 hora", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"30 minutos", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"1 minuto", "2026-10-07 10:00", "2026-10-07 10:01", ""},
        {"90minutos", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"media hora", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"una media hora", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"una hora", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"una hora y media", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"dos horas y media", "2026-10-07 10:00", "2026-10-07 12:30", ""},
        {"hora y media", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"un cuarto de hora", "2026-10-07 10:00", "2026-10-07 10:15", ""},
        {"cuarto de hora", "2026-10-07 10:00", "2026-10-07 10:15", ""},
        {"durante 2 horas", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"por 2 horas", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"para 2 horas", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"2 horas y 30 minutos", "2026-10-07 10:00", "2026-10-07 12:30", ""},
        {"1 hora 30 minutos", "2026-10-07 10:00", "2026-10-07 11:30", ""},
        {"tres horas", "2026-10-07 10:00", "2026-10-07 13:00", ""},
        {"viernes a las 14 durante 2 horas", "2026-10-09 14:00", "2026-10-09 16:00", ""},
        {"viernes durante 3 días", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"viernes 3 dias", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"el vie por una semana", "2026-10-09 00:00", "2026-10-16 00:00", ""},
        {"sábado 2 noches", "2026-10-10 00:00", "2026-10-12 00:00", ""},
        {"un día", "2026-10-07 10:00", "2026-10-08 10:00", ""},
        {"2 semanas", "2026-10-07 10:00", "2026-10-21 10:00", ""},
        /* now, and from now */
        {"ahora", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"ahora mismo", "2026-10-07 10:00", "2026-10-07 11:00", ""},
        {"ahora durante 2 horas", "2026-10-07 10:00", "2026-10-07 12:00", ""},
        {"ahora hasta las 17", "2026-10-07 10:00", "2026-10-07 17:00", ""},
        {"en 30 minutos", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"en una hora", "2026-10-07 11:00", "2026-10-07 12:00", ""},
        {"en un cuarto de hora", "2026-10-07 10:15", "2026-10-07 11:15", ""},
        {"en media hora", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"en 2 horas", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"en 2 horas durante 1 hora", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"dentro de 2 horas", "2026-10-07 12:00", "2026-10-07 13:00", ""},
        {"dentro de 30 min", "2026-10-07 10:30", "2026-10-07 11:30", ""},
    };
    RUN(cases);
}

TEST(when_spanish_ranges_and_until) {
    static const Case cases[] = {
        {"hasta el domingo", "2026-10-07 00:00", "2026-10-12 00:00", ""},
        {"hasta el dom", "2026-10-07 00:00", "2026-10-12 00:00", ""},
        {"hasta mañana a las 10", "2026-10-07 10:00", "2026-10-08 10:00", ""},
        {"hasta el domingo a las 17", "2026-10-07 10:00", "2026-10-11 17:00", ""},
        {"del lunes al miércoles", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"de lunes a viernes", "2026-10-12 00:00", "2026-10-17 00:00", ""},
        {"lunes a viernes", "2026-10-12 00:00", "2026-10-17 00:00", ""},
        {"lun-vie", "2026-10-12 00:00", "2026-10-17 00:00", ""},
        {"vie-dom", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"viernes a domingo", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"del viernes al dom", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"viernes hasta el domingo", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"entre el lunes y el miércoles", "2026-10-12 00:00", "2026-10-15 00:00", ""},
        {"desde el viernes hasta el domingo a las 17", "2026-10-09 00:00", "2026-10-11 17:00", ""},
        {"del viernes al domingo a las 17", "2026-10-09 00:00", "2026-10-11 17:00", ""},
        {"el viernes a las 14 hasta el domingo", "2026-10-09 14:00", "2026-10-12 00:00", ""},
        {"de hoy a mañana", "2026-10-07 00:00", "2026-10-09 00:00", ""},
        {"desde el viernes", "2026-10-09 00:00", "2026-10-10 00:00", ""},
        {"para el lunes", "2026-10-12 00:00", "2026-10-13 00:00", ""},
    };
    RUN(cases);
}

/* Spanish's little words stay in the title unless they're part of the day or the time. */
TEST(when_spanish_leaves_other_words_alone) {
    static const Case cases[] = {
        {"cena con la abuela mañana a las 20", "2026-10-08 20:00", "2026-10-08 21:00", "cena con la abuela"},
        {"llevar el coche al taller el lunes", "2026-10-12 00:00", "2026-10-13 00:00", "llevar el coche al taller"},
        {"reunión de padres el martes a las 18", "2026-10-13 18:00", "2026-10-13 19:00", "reunión de padres"},
        {"pagar la luz el 5 de noviembre", "2026-11-05 00:00", "2026-11-06 00:00", "pagar la luz"},
        {"sauna hoy de 19 a 21", "2026-10-07 19:00", "2026-10-07 21:00", "sauna"},
        {"coche viernes 14-17 viaje a ikea", "2026-10-09 14:00", "2026-10-09 17:00", "coche viaje a ikea"},
        {"comprar las 2 pizzas el viernes a las 20", "2026-10-09 20:00", "2026-10-09 21:00", "comprar las 2 pizzas"},
        {"la cena de mañana", "2026-10-08 00:00", "2026-10-09 00:00", "la cena"},
        {"la reunión del lunes", "2026-10-12 00:00", "2026-10-13 00:00", "la reunión"},
        {"recoger a los niños a las 15", "2026-10-07 15:00", "2026-10-07 16:00", "recoger a los niños"},
        {"Lavandería, viernes 7-10!", "2026-10-09 07:00", "2026-10-09 10:00", "Lavandería"},
        {"cuarto de invitados viernes por 3 días", "2026-10-09 00:00", "2026-10-12 00:00", "cuarto de invitados"},
        {"gimnasio por la mañana", "-", "-", "gimnasio por la mañana"},
        {"llegar tarde", "-", "-", "llegar tarde"},
        {"ver la tele en la noche", "-", "-", "ver la tele en la noche"},
        {"la mañana", "-", "-", "la mañana"},
        {"el coche", "-", "-", "el coche"},
        {"de vez en cuando", "-", "-", "de vez en cuando"},
        {"dentista el 15 a las 10", "2026-10-07 10:00", "2026-10-07 11:00", "dentista el 15"}, /* see ..._in_spanish */
    };
    RUN(cases);
}

/* What these meant before Spanish came (each checked against the parser without it): Swedish "el"
 * (electricity), "dom" (they), "de" (they), "en" (a), "fin" (nice), "la" (laid), and English "a",
 * "ya", "mayo", "julio", "dos". */
TEST(when_spanish_leaves_english_and_swedish_alone) {
    static const Case cases[] = {
        {"betala el fredag", "2026-10-09 00:00", "2026-10-10 00:00", "betala el"},
        {"betala el 12", "2026-10-07 12:00", "2026-10-07 13:00", "betala el"},
        {"betala el 25", "-", "-", "betala el 25"},
        {"läs del 2 fre", "2026-10-09 14:00", "2026-10-09 15:00", "läs del"},
        {"dom kommer fre 18", "2026-10-09 18:00", "2026-10-09 19:00", "dom kommer"},
        {"hämta dom 18", "2026-10-07 18:00", "2026-10-07 19:00", "hämta dom"},
        {"fre till dom 18", "2026-10-09 18:00", "2026-10-09 19:00", "till dom"},
        {"skicka till dom fre", "2026-10-09 00:00", "2026-10-10 00:00", "skicka till dom"},
        {"de kommer 18", "2026-10-07 18:00", "2026-10-07 19:00", "de kommer"},
        {"de 14-16 platserna", "2026-10-07 14:00", "2026-10-07 16:00", "de platserna"},
        {"en 30 min promenad", "2026-10-07 10:00", "2026-10-07 10:30", "en promenad"},
        {"en halvtimme", "2026-10-07 10:00", "2026-10-07 10:30", ""},
        {"en kvart", "2026-10-07 10:00", "2026-10-07 10:15", ""},
        {"fin middag fre 19", "2026-10-09 19:00", "2026-10-09 20:00", "fin middag"},
        {"la 2 ägg fre", "2026-10-09 14:00", "2026-10-09 15:00", "la ägg"},
        {"see ya fri 18", "2026-10-09 18:00", "2026-10-09 19:00", "see ya"},
        {"buy 2 mayo", "2026-10-07 14:00", "2026-10-07 15:00", "buy mayo"},
        {"dinner with julio 8", "2026-10-08 08:00", "2026-10-08 09:00", "dinner with julio"},
        {"a midnight snack", "2026-10-08 00:00", "2026-10-08 01:00", "a snack"},
        {"practice fri a 3rd time", "2026-10-09 00:00", "2026-10-10 00:00", "practice a 3rd time"},
        {"at 2 a 3 hour drive", "2026-10-07 14:00", "2026-10-07 17:00", "a drive"},
        {"take pills 2 a 3 times", "2026-10-07 14:00", "2026-10-07 15:00", "take pills a 3 times"},
        {"media night fri", "2026-10-09 00:00", "2026-10-10 00:00", "media night"},
        {"every friday night 11", "2026-10-09 23:00", "2026-10-10 00:00", "night"},
        {"vie for it fri", "2026-10-09 00:00", "2026-10-10 00:00", "vie for it"},
        {"las vegas trip fri", "2026-10-09 00:00", "2026-10-10 00:00", "las vegas trip"},
        {"2 a day", "2026-10-07 14:00", "2026-10-08 14:00", ""},
        {"call a 2 hours", "2026-10-07 10:00", "2026-10-07 12:00", "call a"},
        {"buy dos fri", "2026-10-09 00:00", "2026-10-10 00:00", "buy dos"},
        {"once a week", "2026-10-07 10:00", "2026-10-14 10:00", "once"},
        {"dom 10", "2026-10-07 10:00", "2026-10-07 11:00", "dom"},
        {"sauna ya", "-", "-", "sauna ya"},
        {"mar 14", "2027-03-14 00:00", "2027-03-15 00:00", ""}, /* March, not "martes" */
    };
    RUN(cases);
    RUN_IN(LANG_SV, cases); /* the same for someone using Hemma in Swedish */
}

/* For people who use Hemma in Spanish, the words Spanish shares with Swedish and English are read
 * the Spanish way - and English and Swedish still work. */
TEST(when_spanish_in_spanish) {
    static const Case cases[] = {
        {"el 12", "2026-10-12 00:00", "2026-10-13 00:00", ""},
        {"dentista el 15 a las 10", "2026-10-15 10:00", "2026-10-15 11:00", "dentista"},
        {"el 3", "2026-11-03 00:00", "2026-11-04 00:00", ""},
        {"el 5/11", "2026-11-05 00:00", "2026-11-06 00:00", ""},
        {"dom 10", "2026-10-11 10:00", "2026-10-11 11:00", ""},
        {"vie 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"coche vie 14-17 viaje a ikea", "2026-10-09 14:00", "2026-10-09 17:00", "coche viaje a ikea"},
        {"vie a dom", "2026-10-09 00:00", "2026-10-12 00:00", ""},
        {"sauna 14 a 16", "2026-10-07 14:00", "2026-10-07 16:00", "sauna"},
        {"sauna ya", "2026-10-07 10:00", "2026-10-07 11:00", "sauna"},
        {"en 30 min", "2026-10-07 10:30", "2026-10-07 11:30", ""},
        {"2 mayo", "2027-05-02 00:00", "2027-05-03 00:00", ""},
        {"fri 14-17", "2026-10-09 14:00", "2026-10-09 17:00", ""},
        {"bastu idag kl 19-21", "2026-10-07 19:00", "2026-10-07 21:00", "bastu"},
        {"tonight 8", "2026-10-07 20:00", "2026-10-07 21:00", ""},
    };
    RUN_IN(LANG_ES, cases);
}

TEST(when_spanish_repeats) {
    static const RepeatCase cases[] = {
        {"cada martes", REPEAT_WEEKLY, 1, "2026-10-13", ""},
        {"todos los martes", REPEAT_WEEKLY, 1, "2026-10-13", ""},
        {"los martes", REPEAT_WEEKLY, 1, "2026-10-13", ""},
        {"cada dom", REPEAT_WEEKLY, 6, "2026-10-11", ""},
        {"los domingos", REPEAT_WEEKLY, 6, "2026-10-11", ""},
        {"todos los domingos", REPEAT_WEEKLY, 6, "2026-10-11", ""},
        {"los sábados", REPEAT_WEEKLY, 5, "2026-10-10", ""},
        {"sábados", REPEAT_WEEKLY, 5, "2026-10-10", ""},
        {"cada miércoles", REPEAT_WEEKLY, 2, "2026-10-07", ""},
        {"cada día", REPEAT_DAILY, -1, NULL, ""},
        {"cada dia", REPEAT_DAILY, -1, NULL, ""},
        {"todos los días", REPEAT_DAILY, -1, NULL, ""},
        {"todos los dias", REPEAT_DAILY, -1, NULL, ""},
        {"a diario", REPEAT_DAILY, -1, NULL, ""},
        {"diariamente", REPEAT_DAILY, -1, NULL, ""},
        {"cada semana", REPEAT_WEEKLY, -1, NULL, ""},
        {"todas las semanas", REPEAT_WEEKLY, -1, NULL, ""},
        {"semanalmente", REPEAT_WEEKLY, -1, NULL, ""},
        {"reunión semanal", REPEAT_WEEKLY, -1, NULL, "reunión"},
        {"cada dos semanas", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"cada 2 semanas", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"cada quince días", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"cada 15 días", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"quincenal", REPEAT_BIWEEKLY, -1, NULL, ""},
        {"cada dos viernes", REPEAT_BIWEEKLY, 4, "2026-10-09", ""},
        {"cada mes", REPEAT_MONTHLY, -1, NULL, ""},
        {"todos los meses", REPEAT_MONTHLY, -1, NULL, ""},
        {"mensualmente", REPEAT_MONTHLY, -1, NULL, ""},
        {"pago mensual", REPEAT_MONTHLY, -1, NULL, "pago"},
        {"sacar la basura cada martes a las 19", REPEAT_WEEKLY, 1, "2026-10-13", "sacar la basura"},
        {"regar las plantas @eva todos los domingos", REPEAT_WEEKLY, 6, "2026-10-11", "regar las plantas @eva"},
        {"pizza los viernes por la noche", REPEAT_WEEKLY, 4, "2026-10-09", "pizza"},
        {"comprar el diario", REPEAT_NONE, -1, NULL, "comprar el diario"}, /* the newspaper */
    };
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    for (int i = 0; i < countof(cases); i++) {
        const RepeatCase *c = &cases[i];
        Tokens t;
        When w;
        tok_split(&t, c->in);
        bool any = when_parse(&t, now, &w);
        char date[16] = "", rest[128];
        if (w.has_date) snprintf(date, sizeof date, "%04d-%02d-%02d", w.y, w.m, w.d);
        tok_rest(&t, rest, sizeof rest);
        test_checks++;
        if (any != (c->repeat != REPEAT_NONE) || w.repeat != c->repeat || w.repeat_wday != c->wday ||
            w.has_date != (c->date != NULL) || (c->date && !str_eq(date, c->date)) || !str_eq(rest, c->rest))
            test_fail(__FILE__, __LINE__, "\"%s\": repeat %d wday %d date \"%s\" rest \"%s\"", c->in, w.repeat,
                      w.repeat_wday, date, rest);
    }
    /* a repeat at a time: the first one, at that time; "el martes" is one Tuesday */
    static const Case timed[] = {
        {"cada martes a las 19", "2026-10-13 19:00", "2026-10-13 20:00", ""},
        {"los viernes por la noche a las 9", "2026-10-09 21:00", "2026-10-09 22:00", ""},
        {"el martes", "2026-10-13 00:00", "2026-10-14 00:00", ""},
    };
    RUN(timed);
    Tokens t;
    When w;
    tok_split(&t, "el martes");
    CHECK(when_parse(&t, now, &w) && w.repeat == REPEAT_NONE);
}

TEST(when_describe_in_swedish_and_spanish) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    char out[96];
    struct {
        int64_t s, e;
        const char *sv, *es;
    } cases[] = {
        {dt_make(2026, 10, 7, 19, 0), dt_make(2026, 10, 7, 21, 0), "Idag 19:00–21:00", "Hoy 19:00–21:00"},
        {dt_make(2026, 10, 8, 9, 0), dt_make(2026, 10, 8, 10, 30), "Imorgon 09:00–10:30", "Mañana 09:00–10:30"},
        {dt_make(2026, 10, 6, 10, 0), dt_make(2026, 10, 6, 11, 0), "Igår 10:00–11:00", "Ayer 10:00–11:00"},
        {dt_make(2026, 10, 16, 14, 0), dt_make(2026, 10, 16, 17, 0), "Fre 16 okt 14:00–17:00",
         "Vie 16 oct 14:00–17:00"},
        {dt_make(2026, 10, 16, 14, 0), dt_make(2026, 10, 17, 10, 0), "Fre 16 okt 14:00 – lör 17 okt 10:00",
         "Vie 16 oct 14:00 – sáb 17 oct 10:00"},
        {dt_make(2026, 10, 7, 22, 0), dt_make(2026, 10, 8, 2, 0), "Idag 22:00 – imorgon 02:00",
         "Hoy 22:00 – mañana 02:00"},
        {dt_make(2026, 10, 15, 22, 0), dt_make(2026, 10, 16, 2, 0), "Tors 15 okt 22:00 – fre 16 okt 02:00",
         "Jue 15 oct 22:00 – vie 16 oct 02:00"},
        {dt_make(2026, 10, 17, 0, 0), dt_make(2026, 10, 20, 0, 0), "Lör 17 – mån 19 okt", "Sáb 17 – lun 19 oct"},
        {dt_make(2026, 10, 31, 0, 0), dt_make(2026, 11, 3, 0, 0), "Lör 31 okt – mån 2 nov", "Sáb 31 oct – lun 2 nov"},
        {dt_make(2026, 12, 30, 0, 0), dt_make(2027, 1, 2, 0, 0), "Ons 30 dec – fre 1 jan 2027",
         "Mié 30 dic – vie 1 ene 2027"},
        {dt_make(2026, 10, 7, 0, 0), dt_make(2026, 10, 8, 0, 0), "Idag, heldag", "Hoy, todo el día"},
        {dt_make(2026, 10, 17, 0, 0), dt_make(2026, 10, 18, 0, 0), "Lör 17 okt, heldag", "Sáb 17 oct, todo el día"},
        {dt_make(2027, 1, 5, 10, 0), dt_make(2027, 1, 5, 11, 0), "Tis 5 jan 2027 10:00–11:00",
         "Mar 5 ene 2027 10:00–11:00"},
        {dt_make(2026, 10, 7, 10, 0), dt_make(2026, 10, 7, 10, 0), "Idag 10:00", "Hoy 10:00"},
    };
    Lang was = i18n_use(LANG_SV);
    for (int i = 0; i < countof(cases); i++) {
        when_describe(cases[i].s, cases[i].e, now, out, sizeof out);
        CHECK_STR(out, cases[i].sv);
    }
    i18n_use(LANG_ES);
    for (int i = 0; i < countof(cases); i++) {
        when_describe(cases[i].s, cases[i].e, now, out, sizeof out);
        CHECK_STR(out, cases[i].es);
    }
    i18n_use(was);
}

TEST(when_spanish_survives_garbage) {
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    static const char *const inputs[] = {
        "el", "del", "de", "a las", "a la", "hasta", "hasta las", "hasta el", "cada", "todos los", "los", "pasado",
        "fin de", "fin de semana que", "y media", "menos cuarto", "de la", "por la", "en la", "entre las y",
        "del al", "de a", "el día", "día", "próximo", "la semana", "a las 25", "a las 14h30", "14hh", "a las h",
        "de la noche", "dentro de", "en", "ahora mismo mismo", "el el el 5", "del 5 al", "del 5 al 40 de noviembre",
        "5 al 10 de", "las las las 3", "a las 0 menos cuarto", "a las 12 menos cuarto de la noche", "de 5 a",
        "de lunes a", "lunes a", "del lunes al", "hasta mañana a las", "cada dos", "cada quince", "todos los x",
        "a las 24 y media", "a las 00 de la tarde", "a la una y media de la madrugada", "el próximo", "el lunes que",
        "semana que", "31 de febrero", "del 31 al 2 de febrero", "de hoy a", "dom", "el dom dom", "vie-dom-sab",
        "a medianoche y media", "al", "en punto", "a las 9 en", "mañana por la", "esta", "este finde que viene",
    };
    Lang langs[] = {LANG_EN, LANG_ES};
    for (int l = 0; l < countof(langs); l++) {
        Lang was = i18n_use(langs[l]);
        for (int i = 0; i < countof(inputs); i++) {
            Tokens t;
            When w;
            int64_t s, e;
            tok_split(&t, inputs[i]);
            bool any = when_parse(&t, now, &w);
            bool resolved = when_resolve(&w, now, 60, &s, &e);
            test_checks++;
            if (any != resolved || (resolved && e <= s))
                test_fail(__FILE__, __LINE__, "\"%s\": parsed %d resolved %d", inputs[i], any, resolved);
            char rest[128];
            tok_rest(&t, rest, sizeof rest);
        }
        i18n_use(was);
    }
}

TEST(when_spanish_fuzz_random_phrases) {
    static const char *const pool[] = {
        "cena",   "el",     "la",      "las",     "los",    "de",     "del",    "al",     "a",       "y",
        "en",     "por",    "para",    "desde",   "hasta",  "entre",  "hoy",    "mañana", "pasado",  "lunes",
        "vie",    "dom",    "sábado",  "próximo", "que",    "viene",  "semana", "fin",    "finde",   "esta",
        "noche",  "tarde",  "madrugada", "14",    "16",     "9",      "12",     "1",      "una",     "dos",
        "media",  "cuarto", "menos",   "horas",   "hora",   "minutos", "días",  "cada",   "todos",   "5",
        "noviembre", "mayo", "ago",    "día",     "14h",    "14-16",  "5/11",   "31",     "septiembre", "febrero",
        "29",     "2027",   "ahora",   "mismo",   "ya",     "dentro", "durante", "punto", "mediodía", "medianoche",
        "fri",    "kl",     "tonight", "until",   "every",  "29/2",   "10/10",  "12/10",  "x",
    };
    const int64_t nows[] = {dt_make(2026, 10, 7, 10, 0), dt_make(2026, 3, 28, 10, 0), dt_make(2026, 10, 10, 0, 30)};
    uint32_t seed = 4242;
    char phrase[512], desc[96];
    for (int iter = 0; iter < 6000; iter++) {
        phrase[0] = '\0';
        size_t len = 0;
        seed = seed * 1103515245u + 12345u;
        int n = 1 + (int)((seed >> 16) % 10);
        for (int k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            const char *wd = pool[(seed >> 16) % (uint32_t)countof(pool)];
            len += (size_t)snprintf(phrase + len, sizeof phrase - len, "%s%s", k ? " " : "", wd);
        }
        int64_t now = nows[iter % countof(nows)];
        Lang was = i18n_use(iter & 2 ? LANG_ES : LANG_EN);
        Tokens t;
        When w;
        int64_t s, e;
        tok_split(&t, phrase);
        t.used[0] = iter & 1;
        bool any = when_parse(&t, now, &w);
        bool fail = false;
        if (w.refused[0]) {
            bool restored = t.used[0] == (iter & 1);
            for (int k = 1; k < t.n; k++) restored = restored && !t.used[k];
            test_checks++;
            if (any || when_resolve(&w, now, 60, &s, &e) || !restored) {
                test_fail(__FILE__, __LINE__, "\"%s\": refused (%s) but not cleanly", phrase, w.refused);
                fail = true;
            }
        } else if (any != when_resolve(&w, now, 60, &s, &e)) {
            test_fail(__FILE__, __LINE__, "\"%s\": parsed %d but resolved %d", phrase, any, !any);
            fail = true;
        } else if (any) {
            test_checks++;
            when_describe(s, e, now, desc, sizeof desc);
            if (e <= s || !utf8_valid(desc, strlen(desc)) || !desc[0]) {
                test_fail(__FILE__, __LINE__, "\"%s\": end <= start or a bad description", phrase);
                fail = true;
            }
        }
        i18n_use(was);
        if (fail) return;
    }
}
