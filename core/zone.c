/* core/zone.c — the time zone and what a change of it means for kept times (issue #39; see zone.h). */
#include "zone.h"
#include "app.h"
#include "db.h"

/* (named for the System page, which kept them first: a household's data has them under these names) */
#define SET_ZONE "system.time_zone"          /* this computer's zone at the last start: "+01:00/+02:00" */
#define SET_ZONE_CHANGED "system.tz_changed" /* "<from>|<to>|<day>|<unix>|<kept>|<retimed>" until seen */

static char g_test_sig[32];
void zone_test_sig(const char *sig) { str_copy(g_test_sig, sizeof g_test_sig, sig ? sig : ""); }

static void offset_text(int secs, char out[8]) {
    char sign = secs < 0 ? '-' : '+';
    if (secs < 0) secs = -secs;
    snprintf(out, 8, "%c%02d:%02d", sign, secs / 3600 % 100, secs / 60 % 60);
}

void zone_sig(char *out, size_t cap) {
    if (g_test_sig[0]) {
        str_copy(out, cap, g_test_sig);
        return;
    }
    int year = dt_local(now_sec()).year;
    char a[8], b[8];
    offset_text(utc_offset(dt_make(year, 1, 15, 12, 0)), a);
    offset_text(utc_offset(dt_make(year, 7, 15, 12, 0)), b);
    if (str_eq(a, b)) snprintf(out, cap, "%s", a);
    else snprintf(out, cap, "%s/%s", a, b);
}

void zone_words(const char *sig, char *out, size_t cap) {
    const char *slash = strchr(sig, '/');
    if (slash) snprintf(out, cap, T("UTC%.*s in January, UTC%s in July"), (int)(slash - sig), sig, slash + 1);
    else snprintf(out, cap, T("UTC%s all year"), sig);
}

static bool one_offset(const char *s, int *out) {
    int h = 0, m = 0;
    if ((s[0] != '+' && s[0] != '-') || sscanf(s + 1, "%2d:%2d", &h, &m) != 2 || h > 14 || m > 59) return false;
    *out = (s[0] == '-' ? -1 : 1) * (h * 3600 + m * 60);
    return true;
}

bool zone_offsets(const char *sig, int *jan, int *jul) {
    if (!one_offset(sig, jan)) return false;
    const char *slash = strchr(sig, '/');
    if (!slash) {
        *jul = *jan;
        return true;
    }
    return one_offset(slash + 1, jul);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

bool rezone_day(const Rezone *z, int64_t *t) {
    if (*t <= 0 || day_start(*t) == *t) return false;
    int offs[2] = {z->jan, z->jul};
    for (int k = 0; k < 2; k++) {
        int64_t w = *t + offs[k]; /* the old zone's clock, as seconds since 1970-01-01 00:00 */
        if (w - floor_div(w, 86400) * 86400 != 0) continue;
        int y, m, d;
        date_of_days(floor_div(w, 86400), &y, &m, &d);
        *t = dt_make(y, m, d, 0, 0);
        return true;
    }
    return false;
}

/* The EU's summer: from the last Sunday of March to the last Sunday of October, 01:00 UTC. */
static int64_t last_sunday(int year, int mon, int day31) {
    int64_t d = days_of_date(year, mon, day31);
    int wd = (int)(((d + 3) % 7 + 7) % 7); /* 0 = Monday (1970-01-01 was a Thursday) */
    return d - (wd + 1) % 7;
}
static bool eu_summer(int64_t t) {
    int y, m, d;
    date_of_days(floor_div(t, 86400), &y, &m, &d);
    return t >= last_sunday(y, 3, 31) * 86400 + 3600 && t < last_sunday(y, 10, 31) * 86400 + 3600;
}

static int old_offset(const Rezone *z, int64_t t) {
    if (z->jan == z->jul) return z->jan;
    int year = dt_local(t).year;
    int jan = utc_offset(dt_make(year, 1, 15, 12, 0)), jul = utc_offset(dt_make(year, 7, 15, 12, 0));
    bool summer = jan != jul ? utc_offset(t) == jul : eu_summer(t);
    return summer ? z->jul : z->jan;
}

bool rezone_time(const Rezone *z, int64_t *t) {
    if (*t <= 0) return false;
    int64_t w = *t + old_offset(z, *t), days = floor_div(w, 86400), s = w - days * 86400;
    int y, m, d;
    date_of_days(days, &y, &m, &d);
    int64_t n = dt_make(y, m, d, (int)(s / 3600), (int)(s / 60 % 60)) + s % 60;
    if (n == *t) return false;
    *t = n;
    return true;
}

bool rezone_span(const Rezone *z, bool days, int64_t made, int64_t *start, int64_t *end) {
    if (days != z->days || (!days && made >= z->before)) return false;
    bool a = days ? rezone_day(z, start) : rezone_time(z, start);
    bool b = days ? rezone_day(z, end) : rezone_time(z, end);
    return a || b;
}

static void when_text(int64_t t, bool with_day, char *out, size_t cap) {
    DateTime d = dt_local(t);
    if (with_day)
        snprintf(out, cap, "%s %d %s %02d:%02d", weekday_name(d.wday, true), d.day, month_name(d.mon, true), d.hour,
                 d.min);
    else snprintf(out, cap, "%02d:%02d", d.hour, d.min);
}

void rezone_note(Rezone *z, const char *what, int64_t was, int64_t would) {
    if (z->notes >= REZONE_NOTES) return;
    char a[40], b[40];
    when_text(was, true, a, sizeof a);
    when_text(would, day_start(was) != day_start(would), b, sizeof b);
    snprintf(z->note[z->notes++], sizeof z->note[0], "%s · %s → %s", what, a, b);
}

/* every module's things, through its hook */
static int rezone_all(Rezone *z) {
    int n = 0;
    db_begin();
    for (int i = 0; i < module_count(); i++)
        if (module_at(i)->rezone) n += module_at(i)->rezone(z);
    db_commit();
    return n;
}

bool zone_change(ZoneChange *out) {
    memset(out, 0, sizeof *out);
    out->retimed = -1;
    const char *rec = setting(SET_ZONE_CHANGED, "");
    long long at = 0;
    int kept = 0, retimed = -1;
    int got = sscanf(rec, "%31[^|]|%31[^|]|%10[^|]|%lld|%d|%d", out->from, out->to, out->day, &at, &kept, &retimed);
    if (got < 3) return false;
    if (got < 4) { /* (written by 0.14: no moment, nothing kept) - from the start of that day */
        int y, m, d;
        at = parse_ymd(out->day, &y, &m, &d) ? dt_make(y, m, d, 0, 0) : 0;
    }
    out->at = at;
    out->kept = got >= 5 ? kept : 0;
    out->retimed = got >= 6 ? retimed : -1;
    return true;
}

static void record(const ZoneChange *zc) {
    char rec[160];
    snprintf(rec, sizeof rec, "%s|%s|%s|%lld|%d|%d", zc->from, zc->to, zc->day, (long long)zc->at, zc->kept,
             zc->retimed);
    setting_set(SET_ZONE_CHANGED, rec);
}

void zone_boot(void) {
    char sig[32], was[32];
    zone_sig(sig, sizeof sig);
    str_copy(was, sizeof was, setting(SET_ZONE, ""));
    if (str_eq(was, sig)) return;
    setting_set(SET_ZONE, sig);
    Rezone z = {.days = true, .apply = true};
    if (!was[0] || !zone_offsets(was, &z.jan, &z.jul)) return; /* the first start: nothing kept in another zone */
    ZoneChange zc = {.at = now_sec(), .retimed = -1};
    str_copy(zc.from, sizeof zc.from, was);
    str_copy(zc.to, sizeof zc.to, sig);
    fmt_ymd(zc.at, zc.day);
    zc.kept = rezone_all(&z);
    record(&zc);
    char a[80], b[80];
    zone_words(was, a, sizeof a);
    zone_words(sig, b, sizeof b);
    log_warn("the time zone changed from %s to %s: %d all-day thing%s kept on %s days; times typed before may "
             "look shifted - the System page can move them back",
             a, b, zc.kept, zc.kept == 1 ? "" : "s", zc.kept == 1 ? "its" : "their");
}

int zone_retime(bool apply, Rezone *z) {
    ZoneChange zc;
    memset(z, 0, sizeof *z);
    if (!zone_change(&zc) || zc.retimed >= 0 || !zone_offsets(zc.from, &z->jan, &z->jul)) return 0;
    z->days = false;
    z->apply = apply;
    z->before = zc.at;
    int n = rezone_all(z);
    if (apply) {
        zc.retimed = n;
        record(&zc);
        log_info("the times of %d thing%s typed before the time zone changed were moved back to the times typed", n,
                 n == 1 ? "" : "s");
    }
    return n;
}

void zone_seen(void) { setting_set(SET_ZONE_CHANGED, ""); }
