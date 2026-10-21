/* tests/harness.c — see harness.h */
#include "harness.h"

static Buf page_buf;

void th_boot(void) { th_boot_in(test_tmpdir()); }

void th_boot_in(const char *dir) {
    app_set_testing(true);
    if (!app_boot(dir)) test_fail(__FILE__, __LINE__, "app_boot failed");
}

void th_halt(void) { app_halt(); }

int th_person(const char *name) {
    Person p = {0};
    str_copy(p.name, sizeof p.name, name);
    p.hue = 200;
    p.created = now_sec();
    tbl_put(&T_person, &p);
    return p.id;
}

const char *th_page(int person, const char *url) {
    Ctx c;
    app_ctx(&c, person, url);
    buf_reset(&page_buf);
    app_render(&c, &page_buf);
    app_ctx_free(&c);
    return buf_cstr(&page_buf);
}

ThResult th_act(int person, const char *action, const char *form) {
    Ctx c;
    app_ctx(&c, person, "/");
    Fields f = {0};
    if (form) fields_parse(&f, form, strlen(form));
    ThResult r = {0};
    bool found = app_action(&c, action, &f);
    r.ok = found && !c.failed;
    str_copy(r.toast, sizeof r.toast, c.toast);
    str_copy(r.redirect, sizeof r.redirect, c.redirect);
    str_copy(r.field, sizeof r.field, c.fail_field);
    if (c.undo) snprintf(r.undo, sizeof r.undo, "%016llx", (unsigned long long)c.undo);
    fields_free(&f);
    app_ctx_free(&c);
    return r;
}

/* ---------------------------------------------------------------- languages (issue #46) */

/* What people read on a rendered page, in order: text between tags (entities undone, trimmed) and
   the attributes that are read out or shown (title, aria-label, placeholder, alt, data-confirm,
   data-empty, a button's value). Code (code, kbd, pre, samp) and pictures (svg) are skipped. */
static int readable(const char *h, char out[][300], int max) {
    int n = 0, skip = 0;
    Buf t = {0};
    for (const char *p = h; *p;) {
        if (*p != '<') {
            buf_reset(&t);
            while (*p && *p != '<') {
                static const struct { const char *e; char c; } ent[] = {
                    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '}};
                bool done = false;
                for (size_t k = 0; k < sizeof ent / sizeof ent[0] && !done; k++)
                    if (!strncmp(p, ent[k].e, strlen(ent[k].e))) buf_putc(&t, ent[k].c), p += strlen(ent[k].e), done = true;
                if (!done) buf_putc(&t, *p++);
            }
            char s[300];
            str_copy(s, sizeof s, buf_cstr(&t));
            str_trim(s);
            if (!skip && s[0] && n < max) str_copy(out[n++], 300, s);
            continue;
        }
        const char *e = strchr(p, '>');
        if (!e) break;
        char name[16] = {0};
        bool close = p[1] == '/';
        const char *q = p + 1 + close;
        for (size_t k = 0; k + 1 < sizeof name && ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9')); k++) name[k] = *q++;
        bool hidden = !strcmp(name, "svg") || !strcmp(name, "code") || !strcmp(name, "kbd") || !strcmp(name, "pre") ||
                      !strcmp(name, "samp") || !strcmp(name, "style") || !strcmp(name, "script");
        if (hidden && e[-1] != '/') skip += close ? (skip > 0 ? -1 : 0) : 1;
        if (!close && !skip) {
            char tag[2048];
            snprintf(tag, sizeof tag, "%.*s", (int)(e - p + 1), p);
            static const char *const attrs[] = {" title=\"", " aria-label=\"", " placeholder=\"", " alt=\"",
                                                " data-confirm=\"", " data-empty=\"", " aria-valuetext=\""};
            size_t na = sizeof attrs / sizeof attrs[0];
            bool button = !strcmp(name, "input") && (strstr(tag, "type=\"submit\"") || strstr(tag, "type=\"button\""));
            for (size_t k = 0; k < na + button; k++) {
                const char *a = k < na ? attrs[k] : " value=\"";
                const char *v = strstr(tag, a);
                if (!v) continue;
                v += strlen(a);
                const char *ve = strchr(v, '"');
                if (!ve || n >= max) continue;
                char s[300];
                snprintf(s, sizeof s, "%.*s", (int)(ve - v), v);
                if (s[0]) str_copy(out[n++], 300, s);
            }
        }
        p = e + 1;
    }
    buf_free(&t);
    return n;
}

/* Nothing to translate in it: no word of letters, or only allowed words and units. */
static bool neutral(const char *s, const char *const *allow) {
    for (const char *const *a = allow; a && *a; a++)
        if (!strcmp(s, *a)) return true;
    static const char *const units[] = {"GB", "MB", "KB", "TB", "kB", "min", "h", "s", "ms", "px", "fps", "x", "Hemma",
                                        "HEVC", "MKV", "MP4", "AAC", "H", "AC", "E", "S", "Wi", "Fi", "OK", "TV", "IP",
                                        "Mbit", "kbit", "Hz", "kHz", "PIN", "QR", "SD", "HD", "USB", "UTC", NULL};
    for (const char *p = s; *p;) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            p++;
            continue;
        }
        const char *w = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
        size_t n = (size_t)(p - w);
        bool ok = false;
        for (const char *const *u = units; *u && !ok; u++) ok = strlen(*u) == n && !strncmp(*u, w, n);
        for (const char *const *a = allow; a && *a && !ok; a++) ok = strlen(*a) == n && !strncmp(*a, w, n);
        if (!ok) return false;
    }
    return true;
}

/* Test-only modules (ids "zz...": a test registers one, and it stays for the rest of its process) speak
   the tests' own words, never translated - whichever later test meets them, depending on how the
   runner split the tests (CI's 4 processes put the screens builder after core/app_screen_test.c's
   "first" and "second"): their titles, block titles and block choices count as names. */
static char g_zz[96][96];
static int g_nzz;

static void zz_add(const char *s) {
    if (s && *s && g_nzz < (int)(sizeof g_zz / sizeof g_zz[0])) str_copy(g_zz[g_nzz++], sizeof g_zz[0], s);
}

static void zz_collect(int person) {
    g_nzz = 0;
    Ctx c;
    app_ctx(&c, person, "/");
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (strncmp(m->id, "zz", 2) != 0) continue;
        zz_add(m->title);
        for (const Block *b = m->blocks; b && b->id; b++) {
            zz_add(b->title);
            BlockChoice ch[16];
            int n = b->choices ? b->choices(&c, ch, 16) : 0;
            for (int k = 0; k < n; k++) zz_add(ch[k].label);
        }
    }
    app_ctx_free(&c);
}

static bool zz_word(const char *s) {
    for (int i = 0; i < g_nzz; i++)
        if (!strcmp(g_zz[i], s)) return true;
    return false;
}

int th_untranslated(int person, const char *url, const char *const *allow, Buf *out) {
    static char en[1500][300], other[1500][300];
    zz_collect(person);
    char was[16];
    str_copy(was, sizeof was, person ? pref(person, PREF_LANG, "") : "");
    int bad = 0, ne = 0;
    for (int l = 0; l < 3; l++) {
        Lang lang = l == 0 ? LANG_EN : l == 1 ? LANG_SV : LANG_ES;
        if (person) pref_set(person, PREF_LANG, i18n_code(lang));
        else app_test_dev_lang(lang);
        const char *h = th_page(person, url);
        if (lang == LANG_EN) {
            ne = readable(h, en, 1500);
            for (int i = ne; i < 1500 && en[i][0]; i++) en[i][0] = '\0';
            continue;
        }
        int no = readable(h, other, 1500);
        for (int i = 0; i < no; i++) {
            if (neutral(other[i], allow) || zz_word(other[i])) continue;
            const Tr *tr = i18n_find(other[i]); /* translated - to the same word ("System" in Swedish) */
            if (tr && !strcmp(lang == LANG_SV ? (tr->sv ? tr->sv : "") : (tr->es ? tr->es : ""), other[i])) continue;
            if (!strchr(other[i], 32) && (strchr(other[i], 47) || strchr(other[i], 92))) continue; /* a path */
            /* the same markup in every language: compared piece by piece when the counts agree (Spanish
               "Red" = Network is no English "Red"), else against any English piece */
            bool same = false;
            if (no == ne) same = !strcmp(en[i], other[i]);
            else
                for (int k = 0; k < 1500 && en[k][0] && !same; k++) same = !strcmp(en[k], other[i]);
            if (!same) continue;
            bad++;
            if (out) buf_printf(out, "%s (%s): \"%s\"\n", url, i18n_code(lang), other[i]);
        }
    }
    if (person) pref_set(person, PREF_LANG, was);
    else app_test_dev_lang(-1);
    return bad;
}
