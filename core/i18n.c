/* core/i18n.c — see i18n.h. */
#include "i18n.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static I18nTable *g_tables;
static const Tr **g_slot; /* open addressing by the English's hash; the first registered wins */
static size_t g_mask;
static int g_entries;
static bool g_built;

void i18n_register(I18nTable *t) {
    t->next = g_tables;
    g_tables = t;
    g_built = false;
}

/* Built at the first lookup that needs it - app_boot makes that happen before any thread starts. */
static void build(void) {
    int n = 0;
    for (const I18nTable *t = g_tables; t; t = t->next) n += t->n;
    size_t cap = 64;
    while (cap < (size_t)n * 2) cap <<= 1;
    free(g_slot);
    g_slot = xcalloc(cap, sizeof *g_slot);
    g_mask = cap - 1;
    g_entries = n;
    for (const I18nTable *t = g_tables; t; t = t->next)
        for (int i = 0; i < t->n; i++) {
            const Tr *e = &t->tr[i];
            if (!e->en) continue;
            size_t h = (size_t)hash64_str(e->en) & g_mask;
            while (g_slot[h] && strcmp(g_slot[h]->en, e->en) != 0) h = (h + 1) & g_mask;
            if (!g_slot[h]) g_slot[h] = e;
        }
    g_built = true;
}

const Tr *i18n_find(const char *en) {
    if (!en) return NULL;
    if (!g_built) build();
    for (size_t h = (size_t)hash64_str(en) & g_mask; g_slot[h]; h = (h + 1) & g_mask)
        if (strcmp(g_slot[h]->en, en) == 0) return g_slot[h];
    return NULL;
}

int i18n_count(void) {
    if (!g_built) build();
    return g_entries;
}

static const char *plain(const char *en) { /* the English of a TC() text: past its context */
    const char *x = strchr(en, '\x04');
    return x ? x + 1 : en;
}

const char *T_in(Lang lang, const char *en) {
    if (!en) return "";
    if (lang == LANG_EN) return plain(en);
    const Tr *e = i18n_find(en);
    const char *s = !e ? NULL : lang == LANG_SV ? e->sv : e->es;
    return s && *s ? s : plain(en);
}

const char *T(const char *en) { return T_in(i18n_lang(), en); }
const char *TN(long long n, const char *one, const char *other) { return T(n == 1 ? one : other); }

Lang i18n_lang(void) { return (Lang)lang_now(); }
Lang i18n_use(Lang lang) { return (Lang)lang_use((int)lang); }

const char *i18n_code(Lang l) {
    static const char *const codes[LANG_COUNT] = {"en", "sv", "es"};
    return l >= 0 && l < LANG_COUNT ? codes[l] : "en";
}

const char *i18n_name(Lang l) {
    static const char *const names[LANG_COUNT] = {"English", "Svenska", "Español"};
    return l >= 0 && l < LANG_COUNT ? names[l] : "English";
}

bool i18n_parse(const char *code, Lang *out) {
    if (!code || !isalpha((unsigned char)code[0]) || !isalpha((unsigned char)code[1])) return false;
    if (code[2] && code[2] != '-' && code[2] != '_') return false;
    char two[3] = {(char)tolower((unsigned char)code[0]), (char)tolower((unsigned char)code[1]), 0};
    for (int l = 0; l < LANG_COUNT; l++)
        if (strcmp(two, i18n_code((Lang)l)) == 0) {
            if (out) *out = (Lang)l;
            return true;
        }
    return false;
}

/* "sv-SE,sv;q=0.9,en-US;q=0.8,en;q=0.7": the highest q among ours, the first of equals. */
int i18n_accept(const char *h) {
    int best = -1;
    double bq = 0;
    for (const char *p = h; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char tag[24];
        size_t n = 0;
        while (*p && *p != ',' && *p != ';' && *p != ' ') {
            if (n + 1 < sizeof tag) tag[n++] = *p;
            p++;
        }
        tag[n] = '\0';
        double q = 1.0;
        while (*p && *p != ',') { /* parameters: only q matters */
            if (*p == ';') {
                p++;
                while (*p == ' ') p++;
                if ((p[0] == 'q' || p[0] == 'Q') && p[1] == '=') q = strtod(p + 2, NULL);
            }
            if (*p && *p != ',' && *p != ';') p++;
        }
        Lang l;
        if (q > bq && i18n_parse(tag, &l)) best = (int)l, bq = q;
    }
    return best;
}

char i18n_decimal(void) { return i18n_lang() == LANG_EN ? '.' : ','; }

/* ---------------------------------------------------------------- specifiers */

/* the end of the specifier starting at p ('%'); H()'s conversions (%r %u %L %X) and printf's alike */
static const char *spec_end(const char *p) {
    p++;
    if (*p == '%') return p + 1;
    while (*p && strchr("-+ #0'", *p)) p++;
    while (*p == '*' || isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
        p++;
        while (*p == '*' || isdigit((unsigned char)*p)) p++;
    }
    while (*p && strchr("hlqjzt", *p)) p++;
    return *p ? p + 1 : p;
}

int i18n_specs(const char *fmt, char out[][12], int max) {
    int n = 0;
    for (const char *p = fmt; p && *p;) {
        if (*p != '%') {
            p++;
            continue;
        }
        const char *e = spec_end(p);
        if (n < max) snprintf(out[n], 12, "%.*s", (int)(e - p), p);
        n++;
        p = e;
    }
    return n;
}

/* ---------------------------------------------------------------- pack */

#define PACK_FMT '\x1e'
#define PACK_VAL '\x1f'
#define PACK_SEG '\x1d' /* between sentences packed one after another (i18n_pack_add) */

static void put_clean(Buf *b, const char *s) { /* a value: never one of the separators */
    for (; s && *s; s++) buf_putc(b, *s == PACK_FMT || *s == PACK_VAL || *s == PACK_SEG ? ' ' : *s);
}

bool i18n_vpack(char *out, size_t cap, const char *fmt, va_list args) {
    va_list ap, plain;
    va_copy(ap, args);
    va_copy(plain, args);
    Buf b = {0};
    put_clean(&b, fmt);
    buf_putc(&b, PACK_FMT);
    bool first = true, ok = true;
    for (const char *p = fmt; *p && ok;) {
        if (*p != '%') {
            p++;
            continue;
        }
        const char *e = spec_end(p);
        char spec[16], val[64];
        snprintf(spec, sizeof spec, "%.*s", (int)(e - p), p);
        for (char *q = spec; (q = strchr(q, '\'')) != NULL;) /* (grouping: done when shown, by the language) */
            memmove(q, q + 1, strlen(q));
        char conv = e > p ? e[-1] : 0;
        bool l = strstr(spec, "l") != NULL, ll = strstr(spec, "ll") != NULL, z = strchr(spec, 'z') != NULL;
        const char *s = val;
        val[0] = '\0';
        if (conv == '%') {
            p = e;
            continue;
        } else if (conv == 's') {
            s = va_arg(ap, const char *);
            if (!s) s = "";
            if (strcmp(spec, "%s") != 0) { /* a width or precision: formatted as it says */
                snprintf(val, sizeof val, spec, s);
                s = val;
            }
        } else if (conv == 'd' || conv == 'i') {
            if (ll) snprintf(val, sizeof val, spec, va_arg(ap, long long));
            else if (l) snprintf(val, sizeof val, spec, va_arg(ap, long));
            else if (z) snprintf(val, sizeof val, spec, va_arg(ap, size_t));
            else snprintf(val, sizeof val, spec, va_arg(ap, int));
        } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            if (ll) snprintf(val, sizeof val, spec, va_arg(ap, unsigned long long));
            else if (l) snprintf(val, sizeof val, spec, va_arg(ap, unsigned long));
            else if (z) snprintf(val, sizeof val, spec, va_arg(ap, size_t));
            else snprintf(val, sizeof val, spec, va_arg(ap, unsigned));
        } else if (conv == 'c') {
            snprintf(val, sizeof val, "%c", (char)va_arg(ap, int));
        } else if (conv == 'f' || conv == 'g' || conv == 'e') {
            snprintf(val, sizeof val, spec, va_arg(ap, double));
        } else {
            ok = false; /* a specifier this doesn't know: keep the English text instead */
            break;
        }
        if (!first) buf_putc(&b, PACK_VAL);
        put_clean(&b, s);
        first = false;
        p = e;
    }
    va_end(ap);
    if (ok && b.len < cap) {
        memcpy(out, b.p, b.len + 1);
    } else {
        ok = false;
        vsnprintf(out, cap, fmt, plain);
    }
    va_end(plain);
    buf_free(&b);
    return ok;
}

bool i18n_pack(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool ok = i18n_vpack(out, cap, fmt, ap);
    va_end(ap);
    return ok;
}

bool i18n_pack_add(char *out, size_t cap, const char *fmt, ...) {
    char seg[1024];
    va_list ap;
    va_start(ap, fmt);
    bool ok = i18n_vpack(seg, sizeof seg, fmt, ap);
    va_end(ap);
    size_t have = strlen(out), n = strlen(seg);
    if (!ok || have + 1 + n + 1 > cap) return false;
    if (have) out[have++] = PACK_SEG;
    memcpy(out + have, seg, n + 1);
    return true;
}

void i18n_group(Buf *b, const char *digits, size_t n) {
    static const char *const sep[LANG_COUNT] = {",", "\xC2\xA0", "."}; /* 2,944 · 2 944 · 2.944 */
    size_t at = 0;
    if (n && (digits[0] == '-' || digits[0] == '+')) buf_putc(b, digits[at++]);
    size_t len = n - at;
    for (size_t i = 0; i < len; i++) {
        if (digits[at + i] < '0' || digits[at + i] > '9') { /* not a whole number: as it is */
            buf_put(b, digits + at + i, len - i);
            return;
        }
        if (i && (len - i) % 3 == 0) buf_puts(b, sep[i18n_lang()]);
        buf_putc(b, digits[at + i]);
    }
}

static void unpack_one(Buf *b, const char *s, size_t n) {
    const char *sep = memchr(s, PACK_FMT, n), *stop = s + n;
    if (!sep) {
        buf_put(b, s, n);
        return;
    }
    char key[512];
    snprintf(key, sizeof key, "%.*s", (int)(sep - s), s);
    const char *fmt = T(key), *val = sep + 1;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            buf_putc(b, *p++);
            continue;
        }
        const char *e = spec_end(p);
        if (e - p == 2 && p[1] == '%') {
            buf_putc(b, '%');
        } else if (val) { /* the next value, as it was formatted */
            const char *end = memchr(val, PACK_VAL, (size_t)(stop - val));
            size_t len = end ? (size_t)(end - val) : (size_t)(stop - val);
            if (memchr(p, '\'', (size_t)(e - p))) i18n_group(b, val, len);
            else buf_put(b, val, len);
            val = end ? end + 1 : NULL;
        }
        p = e;
    }
}

void i18n_unpack(Buf *b, const char *packed) {
    if (!packed) return;
    for (const char *p = packed;;) {
        const char *end = strchr(p, PACK_SEG);
        if (p != packed) buf_putc(b, ' ');
        unpack_one(b, p, end ? (size_t)(end - p) : strlen(p));
        if (!end) break;
        p = end + 1;
    }
}

void i18n_unpack_to(char *out, size_t cap, const char *packed) {
    Buf b = {0};
    i18n_unpack(&b, packed);
    snprintf(out, cap, "%s", b.len ? b.p : "");
    buf_free(&b);
}

/* ---------------------------------------------------------------- checks */

static int tags(const char *s, char out[][96], int max) { /* every "<...>", in order */
    int n = 0;
    for (const char *p = s; p && (p = strchr(p, '<'));) {
        const char *e = strchr(p, '>');
        if (!e) break;
        if (n < max) snprintf(out[n], 96, "%.*s", (int)(e - p + 1), p);
        n++;
        p = e + 1;
    }
    return n;
}

static int cmp96(const void *a, const void *b) { return strcmp(a, b); }

static void check_one(Buf *r, int *bad, const char *table, const Tr *e, const char *what, const char *s) {
    if (!s || !*s) {
        buf_printf(r, "%s: \"%.80s\": no %s\n", table, e->en, what);
        (*bad)++;
        return;
    }
    char a[24][12], b[24][12];
    int na = i18n_specs(e->en, a, 24), nb = i18n_specs(s, b, 24);
    bool same = na == nb;
    for (int i = 0; same && i < na && i < 24; i++) same = strcmp(a[i], b[i]) == 0;
    if (!same) {
        buf_printf(r, "%s: \"%.80s\": the %s keeps other format specifiers (%d, the English %d) or another order\n",
                   table, e->en, what, nb, na);
        (*bad)++;
    }
    static char ta[32][96], tb[32][96];
    int ka = tags(e->en, ta, 32), kb = tags(s, tb, 32);
    bool tsame = ka == kb && ka <= 32;
    if (tsame) {
        qsort(ta, (size_t)ka, sizeof ta[0], cmp96);
        qsort(tb, (size_t)kb, sizeof tb[0], cmp96);
        for (int i = 0; i < ka; i++) tsame = tsame && strcmp(ta[i], tb[i]) == 0;
    }
    if (!tsame) {
        buf_printf(r, "%s: \"%.80s\": the %s's inline tags differ from the English\n", table, e->en, what);
        (*bad)++;
    }
}

int i18n_check(Buf *r) {
    if (!g_built) build();
    int bad = 0;
    for (const I18nTable *t = g_tables; t; t = t->next)
        for (int i = 0; i < t->n; i++) {
            const Tr *e = &t->tr[i];
            if (!e->en || !*e->en) {
                buf_printf(r, "%s: entry %d has no English\n", t->name, i);
                bad++;
                continue;
            }
            check_one(r, &bad, t->name, e, "Swedish", e->sv);
            check_one(r, &bad, t->name, e, "Spanish", e->es);
            const Tr *first = i18n_find(e->en);
            if (first && first != e && (strcmp(first->sv ? first->sv : "", e->sv ? e->sv : "") != 0 ||
                                        strcmp(first->es ? first->es : "", e->es ? e->es : "") != 0)) {
                buf_printf(r, "%s: \"%.80s\" is translated twice, differently\n", t->name, e->en);
                bad++;
            }
        }
    return bad;
}
