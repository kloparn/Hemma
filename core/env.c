/* core/env.c — see env.h */
#include "env.h"
#include "plat.h"

#include <ctype.h>

static bool key_char(char c, bool first) { return c == '_' || isalpha((unsigned char)c) || (!first && isdigit((unsigned char)c)); }

/* One line, edited in place: 1 = a pair (*key, val), 0 = blank or a comment, -1 = not KEY=VALUE. */
static int parse_line(char *ln, char **key, Buf *val) {
    char *s = str_trim(ln);
    if (str_starts(s, "\xEF\xBB\xBF")) memmove(s, s + 3, strlen(s + 3) + 1); /* BOM */
    s = str_trim(s);
    if (!*s || *s == '#') return 0;
    if (str_starts(s, "export ")) s = str_trim(s + 7);
    char *eq = strchr(s, '=');
    if (!eq) return -1;
    *eq = '\0';
    s = str_trim(s); /* "KEY = value" is fine too */
    if (!*s) return -1;
    for (char *k = s; *k; k++)
        if (!key_char(*k, k == s)) return -1;
    *key = s;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '"') {
        for (v++; *v && *v != '"'; v++) {
            if (*v == '\\' && v[1]) {
                v++;
                buf_putc(val, *v == 'n' ? '\n' : *v == 't' ? '\t' : *v);
            } else {
                buf_putc(val, *v);
            }
        }
        return *v == '"' ? 1 : -1;
    }
    if (*v == '\'') {
        char *end = strchr(v + 1, '\'');
        if (!end) return -1;
        buf_put(val, v + 1, (size_t)(end - v - 1));
        return 1;
    }
    if (*v == '#' && v > eq + 1) return 1; /* "KEY= # comment": empty */
    char *hash = strstr(v, " #");          /* trailing comment */
    if (!hash) hash = strstr(v, "\t#");
    if (hash) *hash = '\0';
    buf_puts(val, str_trim(v));
    return 1;
}

int env_parse(const char *text, void (*cb)(const char *, const char *, int, void *), void *ctx, int *first_bad) {
    int count = 0, line = 0;
    if (first_bad) *first_bad = 0;
    const char *p = text;
    while (p && *p) {
        line++;
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char *ln = xmalloc(n + 1);
        memcpy(ln, p, n);
        ln[n] = '\0';
        p = eol ? eol + 1 : NULL;
        char *key = NULL;
        Buf val = {0};
        int r = parse_line(ln, &key, &val);
        if (r > 0) {
            cb(key, buf_cstr(&val), line, ctx);
            count++;
        } else if (r < 0 && first_bad && !*first_bad) {
            *first_bad = line;
        }
        buf_free(&val);
        free(ln);
    }
    return count;
}

bool env_line_key(const char *line, char *key, size_t cap) {
    char *ln = xstrdup(line ? line : ""), *k = NULL;
    Buf val = {0};
    bool ok = parse_line(ln, &k, &val) > 0 && strlen(k) < cap;
    if (ok) str_copy(key, cap, k);
    else if (cap) key[0] = '\0';
    buf_free(&val);
    free(ln);
    return ok;
}

typedef struct {
    const char *path;
    Buf keys; /* "\nKEY=line" for every key this file set */
} Load;

static void set_one(const char *k, const char *v, int line, void *ctx) {
    Load *l = ctx;
    char needle[80];
    snprintf(needle, sizeof needle, "\n%s=", k);
    const char *seen = strstr(buf_cstr(&l->keys), needle);
    if (seen) log_warn("%s: %s is set twice (lines %d and %d) - the first one with a value counts", l->path, k,
                       atoi(seen + strlen(needle)), line);
    else buf_printf(&l->keys, "%s%d", needle, line);
    /* The real environment wins - but an empty value is the same as none (env() says so too):
       "HEMMA_AUTH_SALT=" on line 1 mustn't hide the secret on line 9 (issue #12). */
    const char *now = plat_getenv(k);
    if (!now || !*now) plat_setenv(k, v, true);
}

int env_load(const char *path) {
    Buf text = {0};
    if (!file_read_all(path, &text, 1 << 20)) {
        buf_free(&text);
        return -1;
    }
    Load l = {path, {0}};
    int bad = 0, n = env_parse(buf_cstr(&text), set_one, &l, &bad);
    if (bad) log_warn("%s: line %d isn't KEY=VALUE - skipped it", path, bad);
    buf_free(&l.keys);
    buf_free(&text);
    return n;
}

const char *env(const char *key, const char *dflt) {
    const char *v = plat_getenv(key);
    return v && *v ? v : dflt;
}

long long env_int(const char *key, long long dflt) { return str_int_or(env(key, NULL), dflt); }
