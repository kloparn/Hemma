/* Hemma in three languages (issue #46): the lookups, the browser's language, texts kept for later,
 * the translation tables' own checks - and that every text the code marks (T, TN, N_; wire.js's T)
 * has its Swedish and Spanish. Run from the top of the repo, as every runner does. */
#include "../tests/harness.h"
#include "plat.h"
#include "ui.h"

static const Tr TEST_TR[] = {
    {"i18n test: %s booked %d", "i18n-test: %s bokade %d", "prueba i18n: %s reservó %d"},
    {"i18n test: one thing", "i18n-test: en sak", "prueba i18n: una cosa"},
    {"i18n test: %d things", "i18n-test: %d saker", "prueba i18n: %d cosas"},
    {"i18n test: press <b>Save</b>", "i18n-test: tryck på <b>Spara</b>", "prueba i18n: pulsa <b>Guardar</b>"},
    {"i18n test day\x04" "until %s", "till %s", "hasta el %s"},
};
TRANSLATIONS(i18n_test, TEST_TR)

TEST(i18n_finds_a_translation_and_falls_back_to_english) {
    const char *en = "i18n test: one thing";
    CHECK(T_in(LANG_EN, en) == en);
    CHECK_STR(T_in(LANG_SV, en), "i18n-test: en sak");
    CHECK_STR(T_in(LANG_ES, en), "prueba i18n: una cosa");
    CHECK_STR(T_in(LANG_SV, "i18n test: nobody wrote this one"), "i18n test: nobody wrote this one");
    CHECK_STR(T_in(LANG_SV, NULL), "");
    Lang was = i18n_use(LANG_SV);
    CHECK_STR(T(en), "i18n-test: en sak");
    CHECK_STR(TN(1, "i18n test: one thing", "i18n test: %d things"), "i18n-test: en sak");
    CHECK_STR(TN(0, "i18n test: one thing", "i18n test: %d things"), "i18n-test: %d saker");
    CHECK_STR(TN(7, "i18n test: one thing", "i18n test: %d things"), "i18n-test: %d saker");
    CHECK(i18n_decimal() == ',');
    CHECK_STR(weekday_name(0, false), "måndag");
    CHECK_STR(month_name(10, true), "okt");
    i18n_use(LANG_ES);
    CHECK_STR(weekday_name(2, false), "miércoles");
    CHECK_STR(month_name(1, false), "enero");
    i18n_use(LANG_EN);
    CHECK_STR(weekday_name(0, true), "Mon");
    CHECK(i18n_decimal() == '.');
    i18n_use(was);
}

TEST(i18n_reads_language_codes_and_accept_language) {
    Lang l = LANG_EN;
    CHECK(i18n_parse("sv", &l) && l == LANG_SV);
    CHECK(i18n_parse("es-419", &l) && l == LANG_ES);
    CHECK(i18n_parse("EN-gb", &l) && l == LANG_EN);
    CHECK(i18n_parse("sv_SE", &l) && l == LANG_SV);
    CHECK(!i18n_parse("swe", &l) && !i18n_parse("s", &l) && !i18n_parse("", &l) && !i18n_parse("de", &l));
    CHECK(!i18n_parse(NULL, &l));
    CHECK_INT(i18n_accept("sv-SE,sv;q=0.9,en-US;q=0.8,en;q=0.7"), LANG_SV);
    CHECK_INT(i18n_accept("de-DE,de;q=0.9,es;q=0.8,en;q=0.5"), LANG_ES);
    CHECK_INT(i18n_accept("en;q=0.5, sv;q=0.6"), LANG_SV);
    CHECK_INT(i18n_accept("fr-FR, es-MX;q=0.4 ,en ; q=0.4"), LANG_ES); /* the first of equals */
    CHECK_INT(i18n_accept("de, fr;q=0.8"), -1);
    CHECK_INT(i18n_accept("es;q=0"), -1); /* "not this one" */
    CHECK_INT(i18n_accept(""), -1);
    CHECK_INT(i18n_accept(NULL), -1);
    CHECK_STR(i18n_code(LANG_ES), "es");
    CHECK_STR(i18n_name(LANG_SV), "Svenska");
}

TEST(i18n_keeps_a_text_for_later_in_whoevers_language) {
    char p[200];
    CHECK(i18n_pack(p, sizeof p, "i18n test: %s booked %d", "Anna", 3));
    Buf b = {0};
    Lang was = i18n_use(LANG_SV);
    i18n_unpack(&b, p);
    CHECK_STR(buf_cstr(&b), "i18n-test: Anna bokade 3");
    i18n_use(LANG_ES);
    buf_reset(&b);
    i18n_unpack(&b, p);
    CHECK_STR(buf_cstr(&b), "prueba i18n: Anna reservó 3");
    i18n_use(LANG_EN);
    buf_reset(&b);
    i18n_unpack(&b, p);
    CHECK_STR(buf_cstr(&b), "i18n test: Anna booked 3");
    /* a value holding the separators can't shift the others */
    CHECK(i18n_pack(p, sizeof p, "i18n test: %s booked %d", "A\x1e" "B\x1f" "C", 12));
    i18n_use(LANG_SV);
    buf_reset(&b);
    i18n_unpack(&b, p);
    CHECK_STR(buf_cstr(&b), "i18n-test: A B C bokade 12");
    /* a plain text (an older Hemma's line) is shown as it is */
    buf_reset(&b);
    i18n_unpack(&b, "Anna booked the car");
    CHECK_STR(buf_cstr(&b), "Anna booked the car");
    /* too long: the English text, and false */
    char small[20];
    CHECK(!i18n_pack(small, sizeof small, "i18n test: %s booked %d", "Anna", 3));
    CHECK_STR(small, "i18n test: Anna boo");
    /* widths, %%, long long */
    CHECK(i18n_pack(p, sizeof p, "%-3s|%05d|%lld%%", "x", 42, 9000000000LL));
    i18n_use(LANG_EN);
    char out[64];
    i18n_unpack_to(out, sizeof out, p);
    CHECK_STR(out, "x  |00042|9000000000%");
    i18n_use(was);
    buf_free(&b);
}

TEST(i18n_one_english_text_by_context) {
    CHECK_STR(TC("i18n test day", "until %s"), "until %s"); /* English: what follows the context */
    Lang was = i18n_use(LANG_ES);
    CHECK_STR(TC("i18n test day", "until %s"), "hasta el %s");
    CHECK_STR(TC("i18n test nobody wrote", "until %s"), "until %s"); /* none: the English, without its context */
    i18n_use(was);
}

TEST(i18n_lists_format_specifiers_in_order) {
    char s[8][12];
    CHECK_INT(i18n_specs("%s has %d of %lld (%5.1f%%) %r %X", s, 8), 7);
    CHECK_STR(s[0], "%s");
    CHECK_STR(s[1], "%d");
    CHECK_STR(s[2], "%lld");
    CHECK_STR(s[3], "%5.1f");
    CHECK_STR(s[4], "%%");
    CHECK_STR(s[5], "%r");
    CHECK_STR(s[6], "%X");
}

TEST(i18n_tables_are_whole_and_agree) {
    Buf r = {0};
    int bad = i18n_check(&r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    CHECK(i18n_count() > 0);
    buf_free(&r);
}

/* ---------------------------------------------------------------- the code's own texts */

typedef struct {
    Buf report;
    int texts, missing;
} Scan;

static void need(Scan *sc, const char *where, const char *en) {
    sc->texts++;
    const Tr *e = i18n_find(en);
    if (e && e->sv && e->sv[0] && e->es && e->es[0]) return;
    if (sc->missing++ < 60) buf_printf(&sc->report, "%s: \"%.100s\" has no %s\n", where, en, e ? "Swedish or Spanish" : "translation");
}

static bool ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* p at a C string literal's opening quote: appends its text (escapes undone) and returns the end */
static const char *c_literal(const char *p, Buf *out) {
    p++;
    while (*p && *p != '"') {
        if (*p != '\\') {
            buf_putc(out, *p++);
            continue;
        }
        p++;
        switch (*p) {
        case 'n': buf_putc(out, '\n'), p++; break;
        case 't': buf_putc(out, '\t'), p++; break;
        case 'r': buf_putc(out, '\r'), p++; break;
        case 'x': {
            p++;
            int v = 0;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
                v = v * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10), p++;
            buf_putc(out, (char)v);
            break;
        }
        default:
            if (*p >= '0' && *p <= '7') {
                int v = 0;
                for (int k = 0; k < 3 && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0');
                buf_putc(out, (char)v);
            } else if (*p) {
                buf_putc(out, *p++);
            }
        }
    }
    return *p ? p + 1 : p;
}

static const char *skip_space(const char *p) { /* blanks and comments */
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
        } else if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
        } else {
            return p;
        }
    }
}

/* One argument that is a string literal (adjacent ones joined): true + its text; false = not a
   literal (a variable - nothing to check here). p is moved past it. */
static bool literal_arg(const char **pp, Buf *out) {
    const char *p = skip_space(*pp);
    if (*p != '"') return false;
    buf_reset(out);
    while (*p == '"') p = skip_space(c_literal(p, out));
    *pp = p;
    return true;
}

static const char *skip_arg(const char *p) { /* to the comma (or close) ending this argument */
    int depth = 0;
    Buf junk = {0};
    while (*p) {
        if (*p == '"') {
            p = c_literal(p, &junk);
            continue;
        }
        if (*p == '\'') {
            p++;
            if (*p == '\\') p++;
            if (*p) p++;
            if (*p == '\'') p++;
            continue;
        }
        if (*p == '(') depth++;
        else if (*p == ')' && depth-- == 0) break;
        else if (*p == ',' && depth == 0) break;
        p++;
    }
    buf_free(&junk);
    return p;
}

static void scan_c(Scan *sc, const char *path) {
    Buf f = {0};
    if (!file_read_all(path, &f, 8 << 20)) {
        buf_printf(&sc->report, "%s: can't read it\n", path);
        sc->missing++;
        return;
    }
    Buf lit = {0};
    const char *p = buf_cstr(&f);
    char where[300];
    while (*p) {
        if (p[0] == '/' && (p[1] == '*' || p[1] == '/')) {
            p = skip_space(p);
            continue;
        }
        if (*p == '"') {
            p = c_literal(p, &lit);
            continue;
        }
        if (*p == '\'') {
            p++;
            if (*p == '\\') p++;
            if (*p) p++;
            if (*p == '\'') p++;
            continue;
        }
        if (!ident(*p)) {
            p++;
            continue;
        }
        const char *s = p;
        while (ident(*p)) p++;
        size_t n = (size_t)(p - s);
        bool t = n == 1 && s[0] == 'T', tn = n == 2 && !strncmp(s, "TN", 2), nn = n == 2 && !strncmp(s, "N_", 2);
        bool tc = n == 2 && (!strncmp(s, "TC", 2) || !strncmp(s, "NC", 2));
        if (!t && !tn && !nn && !tc) continue;
        const char *q = skip_space(p);
        if (*q != '(') continue;
        q++;
        int line = 1;
        for (const char *c = buf_cstr(&f); c < s; c++) line += *c == '\n';
        snprintf(where, sizeof where, "%s:%d", path, line);
        if (tc) { /* context, text */
            Buf ctx = {0};
            if (literal_arg(&q, &ctx) && *q == ',') {
                q++;
                Buf key = {0};
                if (literal_arg(&q, &lit)) {
                    buf_printf(&key, "%s\x04%s", buf_cstr(&ctx), buf_cstr(&lit));
                    need(sc, where, buf_cstr(&key));
                }
                buf_free(&key);
            }
            buf_free(&ctx);
        } else if (tn) {
            q = skip_arg(q); /* the count */
            if (*q != ',') continue;
            q++;
            if (literal_arg(&q, &lit)) need(sc, where, buf_cstr(&lit));
            if (*q != ',') continue;
            q++;
            if (literal_arg(&q, &lit)) need(sc, where, buf_cstr(&lit));
        } else if (literal_arg(&q, &lit)) {
            need(sc, where, buf_cstr(&lit));
        }
        p = q;
    }
    buf_free(&lit);
    buf_free(&f);
}

static bool skip_file(const char *name) {
    size_t n = strlen(name);
    if (n < 2 || strcmp(name + n - 2, ".c") != 0) return true;
    return (n > 7 && !strcmp(name + n - 7, "_test.c")) || (n > 7 && !strcmp(name + n - 7, "_lang.c")) ||
           !strcmp(name, "i18n.c");
}

static void scan_dir(Scan *sc, const char *dir, int *files) {
    DirIter *it = dir_open(dir);
    if (!it) return;
    DirEntry e;
    char path[700];
    while (dir_next(it, &e)) {
        if (e.is_dir || skip_file(e.name)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e.name);
        scan_c(sc, path);
        (*files)++;
    }
    dir_close(it);
}

TEST(i18n_every_marked_text_in_the_code_is_translated) {
    Scan sc = {0};
    int files = 0;
    scan_dir(&sc, "core", &files);
    DirIter *it = dir_open("modules");
    CHECK(it != NULL);
    DirEntry e;
    char dir[600];
    while (dir_next(it, &e)) {
        if (!e.is_dir || !strcmp(e.name, "av")) continue; /* (av: a codec library, nothing people read) */
        snprintf(dir, sizeof dir, "modules/%s", e.name);
        scan_dir(&sc, dir, &files);
    }
    dir_close(it);
    if (sc.missing) printf("%s(%d of %d texts in %d files)\n", buf_cstr(&sc.report), sc.missing, sc.texts, files);
    CHECK(files > 20);
    CHECK_INT(sc.missing, 0);
    buf_free(&sc.report);
}

/* wire.js: every T('...') is one of ui.c's WIRE_TEXTS (what the page sends it), and those are translated */
TEST(i18n_wire_texts_are_listed_and_translated) {
    Buf f = {0};
    CHECK(file_read_all("web/wire.js", &f, 4 << 20));
    int n = 0;
    const char *const *wt = ui_wire_texts(&n);
    Scan sc = {0};
    int calls = 0;
    for (const char *p = buf_cstr(&f); (p = strstr(p, "T(")) != NULL; p += 2) {
        if (p > f.p && (ident(p[-1]) || p[-1] == '.')) continue;
        char quote = p[2];
        if (quote != '\'' && quote != '"' && quote != '`') continue;
        Buf s = {0};
        const char *q = p + 3;
        while (*q && *q != quote) {
            if (*q == '\\' && q[1]) q++;
            buf_putc(&s, *q++);
        }
        calls++;
        bool listed = false;
        for (int i = 0; i < n && !listed; i++) listed = !strcmp(wt[i], buf_cstr(&s));
        if (!listed) {
            buf_printf(&sc.report, "web/wire.js: T('%s') isn't in core/ui.c's WIRE_TEXTS\n", buf_cstr(&s));
            sc.missing++;
        }
        buf_free(&s);
    }
    for (int i = 0; i < n; i++) need(&sc, "WIRE_TEXTS", wt[i]);
    if (sc.missing) printf("%s", buf_cstr(&sc.report));
    CHECK(calls >= 10);
    CHECK_INT(sc.missing, 0);
    buf_free(&sc.report);
    buf_free(&f);
}

/* ---------------------------------------------------------------- in the app */

TEST(i18n_a_page_speaks_the_persons_language_else_the_browsers) {
    th_boot();
    int anna = th_person("Anna");
    app_test_dev_lang(-1);
    CHECK_HAS(th_page(anna, "/"), "data-lang=\"en\"");
    app_test_dev_lang(LANG_ES); /* the browser asks for Spanish */
    CHECK_HAS(th_page(anna, "/"), "data-lang=\"es\"");
    CHECK_HAS(th_page(0, "/"), "data-lang=\"es\""); /* the sign-in page too */
    pref_set(anna, PREF_LANG, "sv"); /* her own choice wins */
    CHECK_HAS(th_page(anna, "/"), "data-lang=\"sv\"");
    pref_set(anna, PREF_LANG, "");
    CHECK_HAS(th_page(anna, "/"), "data-lang=\"es\"");
    CHECK(i18n_lang() == LANG_EN); /* (a page leaves the thread in English) */
    app_test_dev_lang(-1);
    th_halt();
}

TEST(i18n_the_latest_feed_is_in_the_readers_language) {
    th_boot();
    activity_add(0, "🧪", "", N_("i18n test: %s booked %d"), "Anna", 2);
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "i18n test: Anna booked 2"); /* (the English stays for /api and older Hemmas) */
    Buf b = {0};
    Lang was = i18n_use(LANG_SV);
    activity_text(a, &b);
    CHECK_STR(buf_cstr(&b), "i18n-test: Anna bokade 2");
    i18n_use(was);
    buf_free(&b);
    th_halt();
}

TEST(i18n_days_in_words_start_with_a_capital) {
    th_boot();
    int64_t now = dt_make(2026, 10, 20, 12, 0); /* a Tuesday */
    Buf b = {0};
    Lang was = i18n_use(LANG_SV);
    ui_day(&b, now, now);
    CHECK_STR(buf_cstr(&b), "Idag");
    buf_reset(&b);
    ui_day_inline(&b, add_days(now, 1), now);
    CHECK_STR(buf_cstr(&b), "imorgon");
    buf_reset(&b);
    ui_day(&b, add_days(now, 2), now);
    CHECK_STR(buf_cstr(&b), "Torsdag");
    buf_reset(&b);
    ui_day_inline(&b, add_days(now, 2), now);
    CHECK_STR(buf_cstr(&b), "torsdag");
    buf_reset(&b);
    ui_day(&b, add_days(now, 10), now);
    CHECK_STR(buf_cstr(&b), "Fre 30 okt");
    i18n_use(LANG_ES);
    buf_reset(&b);
    ui_day(&b, add_days(now, -1), now);
    CHECK_STR(buf_cstr(&b), "Ayer");
    buf_reset(&b);
    ui_day(&b, add_days(now, 400), now);
    CHECK_STR(buf_cstr(&b), "24 nov 2027");
    i18n_use(LANG_EN);
    buf_reset(&b);
    ui_day(&b, add_days(now, 10), now);
    CHECK_STR(buf_cstr(&b), "Fri 30 Oct");
    i18n_use(was);
    buf_free(&b);
    th_halt();
}

TEST(i18n_audit_pages_left_in_english) {
    /* not a check: HEMMA_I18N_AUDIT=1 lists what reads the same in every language on a few pages */
    if (!getenv("HEMMA_I18N_AUDIT")) return;
    th_boot();
    int anna = th_person("Anna");
    Buf r = {0};
    static const char *const urls[] = {"/", "/book", "/plan", "/board", "/media", "/people", "/screens", "/net",
                                       "/system", "/more"};
    for (size_t i = 0; i < sizeof urls / sizeof urls[0]; i++) th_untranslated(anna, urls[i], NULL, &r);
    th_untranslated(0, "/", NULL, &r);
    printf("%s", buf_cstr(&r));
    buf_free(&r);
    th_halt();
}
