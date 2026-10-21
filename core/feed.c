/* core/feed.c — a person's calendar on their phone (issue #41): an iCalendar (RFC 5545) feed of what's
 * on for them, built on the shared agenda, at an address with its own key (see app.h, cal_feed_*). */
#include "app.h"
#include "auth.h"
#include "codec.h"

#define FEED_KEY_HEX 32 /* 128 bits */

static Person *feed_owner(int id) { /* people who can sign in - not a display account or a name without one */
    Person *p = person_real(id);
    return p && p->pass[0] ? p : NULL;
}

static bool feed_key(int person, long long n, char out[FEED_KEY_HEX + 1]) {
    char msg[64];
    int len = snprintf(msg, sizeof msg, "cal:%d:%lld", person, n);
    uint8_t mac[32];
    if (!auth_mac(msg, (size_t)len, mac)) return false;
    hex_encode(mac, FEED_KEY_HEX / 2, out);
    return true;
}

bool cal_feed_path(int person, char *out, size_t cap) {
    char key[FEED_KEY_HEX + 1];
    long long n = atoll(pref(person, PREF_CAL, "0"));
    if (n <= 0 || !feed_owner(person) || !feed_key(person, n, key)) return false;
    snprintf(out, cap, "/cal/%d-%s.ics", person, key);
    return true;
}

int cal_feed_person(const char *path) {
    int id = 0, used = 0;
    char key[FEED_KEY_HEX + 1] = "", want[FEED_KEY_HEX + 1];
    if (sscanf(path, "/cal/%d-%32[0-9a-f]%n", &id, key, &used) != 2 || !str_eq(path + used, ".ics")) return 0;
    long long n = atoll(pref(id, PREF_CAL, "0"));
    if (strlen(key) != FEED_KEY_HEX || n <= 0 || !feed_owner(id) || !feed_key(id, n, want)) return 0;
    return ct_equal(key, want, FEED_KEY_HEX) ? id : 0;
}

/* ------------------------------------------------------------ iCalendar */

/* One content line, folded at 75 octets (a continuation starts with a space) and never inside a
   UTF-8 character; CRLF ends it. */
static void ics_line(Buf *b, const char *s) {
    int len = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
        if (len + n > 75) {
            buf_puts(b, "\r\n ");
            len = 1;
        }
        for (int k = 0; k < n && *p; k++) buf_putc(b, (char)*p++);
        len += n;
    }
    buf_puts(b, "\r\n");
}

/* TEXT: backslash, semicolon, comma and line breaks escaped */
static void ics_text(Buf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '\\' || *s == ';' || *s == ',') {
            buf_putc(b, '\\');
            buf_putc(b, *s);
        } else if (*s == '\n') buf_puts(b, "\\n");
        else if (*s != '\r') buf_putc(b, *s);
    }
}

static void ics_prop_text(Buf *b, const char *name, const char *text) {
    Buf l = {0};
    buf_printf(&l, "%s:", name);
    ics_text(&l, text);
    ics_line(b, l.p);
    buf_free(&l);
}

#define STAMP 40 /* 20261025T120000Z (room past its 16 for the compiler's worst case) */
static void utc_stamp(int64_t t, char out[STAMP]) {
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400, s = t - days * 86400;
    int y, m, d;
    date_of_days(days, &y, &m, &d);
    snprintf(out, STAMP, "%04d%02d%02dT%02d%02d%02dZ", y, m, d, (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

static void local_date(int64_t t, char out[9]) { /* 20261025: the day it is here */
    DateTime d = dt_local(t);
    snprintf(out, 9, "%04d%02d%02d", d.year, d.mon, d.day);
}

void cal_feed_ics(Ctx *c, int person, Buf *out) {
    bool all = str_eq(pref(person, PREF_CAL_ALL, ""), "1");
    int64_t today = day_start(c->now);
    Agenda ag = {.todos = true};
    agenda_collect(c, add_days(today, -31), add_days(today, 400), NULL, &ag);
    char stamp[STAMP], line[320];
    utc_stamp(c->now, stamp);
    ics_line(out, "BEGIN:VCALENDAR");
    ics_line(out, "VERSION:2.0");
    snprintf(line, sizeof line, "PRODID:-//Hemma//Hemma %s//EN", HEMMA_VERSION);
    ics_line(out, line);
    ics_line(out, "CALSCALE:GREGORIAN");
    ics_line(out, "METHOD:PUBLISH");
    ics_prop_text(out, "X-WR-CALNAME", app_name());
    ics_line(out, "REFRESH-INTERVAL;VALUE=DURATION:PT1H");
    ics_line(out, "X-PUBLISHED-TTL:PT1H");
    for (int i = 0; i < ag.n; i++) {
        const AgendaItem *it = &ag.items[i];
        bool todo = str_eq(it->kind, "To-do");
        /* for them or for everyone - and with "every booking", other people's bookings too */
        if (it->who && it->who != person && !(all && str_eq(it->kind, "Booking"))) continue;
        ics_line(out, "BEGIN:VEVENT");
        char uid[200];
        snprintf(uid, sizeof uid, "%s|%s", it->module ? it->module : "", it->link);
        snprintf(line, sizeof line, "UID:%016llx@hemma", (unsigned long long)hash64_str(uid));
        ics_line(out, line);
        snprintf(line, sizeof line, "DTSTAMP:%s", stamp);
        ics_line(out, line);
        if (it->allday) {
            char a[9], z[9];
            local_date(it->start, a);
            int64_t end = it->end > it->start ? it->end : add_days(day_start(it->start), 1);
            local_date(end, z);
            if (str_eq(a, z)) local_date(add_days(day_start(it->start), 1), z);
            snprintf(line, sizeof line, "DTSTART;VALUE=DATE:%s", a);
            ics_line(out, line);
            snprintf(line, sizeof line, "DTEND;VALUE=DATE:%s", z);
            ics_line(out, line);
        } else {
            char a[STAMP], z[STAMP];
            utc_stamp(it->start, a);
            snprintf(line, sizeof line, "DTSTART:%s", a);
            ics_line(out, line);
            if (it->end > it->start) {
                utc_stamp(it->end, z);
                snprintf(line, sizeof line, "DTEND:%s", z);
                ics_line(out, line);
            }
        }
        Buf s = {0};
        if (it->icon[0]) buf_printf(&s, "%s ", it->icon);
        buf_puts(&s, it->title);
        const Person *for_whom = it->who && it->who != person ? person_get(it->who) : NULL;
        if (for_whom) buf_printf(&s, " (%s)", for_whom->name);
        ics_prop_text(out, "SUMMARY", s.p);
        buf_reset(&s);
        const Module *m = it->kind[0] || !it->module ? NULL : module_find(it->module);
        const char *kind = it->kind[0] ? T(it->kind) : m ? T(m->title) : ""; /* (kind: English, "Booking") */
        if (for_whom) buf_printf(&s, T("%s for %s - %s"), kind, for_whom->name, app_name());
        else if (it->who == person) buf_printf(&s, T("%s for you - %s"), kind, app_name());
        else buf_printf(&s, "%s - %s", kind, app_name());
        ics_prop_text(out, "DESCRIPTION", s.p);
        buf_free(&s);
        if (c->host[0] && it->link[0] == '/') {
            snprintf(line, sizeof line, "URL:http://%s%s", c->host, it->link);
            ics_line(out, line);
        }
        if (todo) ics_line(out, "TRANSP:TRANSPARENT"); /* a to-do doesn't make you busy */
        ics_line(out, "END:VEVENT");
    }
    ics_line(out, "END:VCALENDAR");
    agenda_free(&ag);
}

void cal_feed_http(Conn *conn, HttpReq *r) {
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return;
    }
    int id = cal_feed_person(r->path);
    if (!id) { /* a link made anew since, turned off, or never one */
        static const char msg[] = "No calendar here. Make a new calendar link in Hemma, under Your account.\n";
        conn_reply(conn, 404, "text/plain; charset=utf-8", msg, sizeof msg - 1, "Cache-Control: no-store\r\n");
        return;
    }
    Ctx c;
    memset(&c, 0, sizeof c);
    c.me = person_get(id);
    c.ip = conn->ip;
    c.now = now_sec();
    const char *h = http_header(r, "Host");
    str_copy(c.host, sizeof c.host, h ? h : "");
    /* in their language (issue #46): their own choice, else what the calendar app asks for */
    const char *mine = pref(id, PREF_LANG, "");
    if (!mine[0] || !i18n_parse(mine, &c.lang)) {
        int dev = i18n_accept(http_header(r, "Accept-Language"));
        c.lang = dev >= 0 ? (Lang)dev : LANG_EN;
    }
    Lang was = i18n_use(c.lang);
    Buf ics = {0};
    cal_feed_ics(&c, id, &ics);
    i18n_use(was);
    conn_reply(conn, 200, "text/calendar; charset=utf-8", ics.p, ics.len,
               "Content-Disposition: inline; filename=\"hemma.ics\"\r\nCache-Control: no-cache\r\n");
    buf_free(&ics);
}
