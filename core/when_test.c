#include "../tests/test.h"
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
    };
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    uint32_t seed = 12345;
    char phrase[512], desc[96];
    for (int iter = 0; iter < 4000; iter++) {
        phrase[0] = '\0';
        size_t len = 0;
        seed = seed * 1103515245u + 12345u;
        int n = 1 + (int)((seed >> 16) % 10);
        for (int k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            const char *wd = pool[(seed >> 16) % (uint32_t)countof(pool)];
            len += (size_t)snprintf(phrase + len, sizeof phrase - len, "%s%s", k ? " " : "", wd);
        }
        Tokens t;
        When w;
        int64_t s, e;
        tok_split(&t, phrase);
        when_parse(&t, now, &w);
        if (when_resolve(&w, now, 60, &s, &e)) {
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
