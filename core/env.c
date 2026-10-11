/* core/env.c — see env.h */
#include "env.h"
#include "plat.h"

#include <ctype.h>

static bool key_char(char c, bool first) { return c == '_' || isalpha((unsigned char)c) || (!first && isdigit((unsigned char)c)); }

int env_parse(const char *text, void (*cb)(const char *, const char *, void *), void *ctx) {
    int count = 0, line = 0;
    const char *p = text;
    while (p && *p) {
        line++;
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char *ln = xmalloc(n + 1);
        memcpy(ln, p, n);
        ln[n] = '\0';
        p = eol ? eol + 1 : NULL;

        char *s = str_trim(ln);
        if (str_starts(s, "\xEF\xBB\xBF")) memmove(s, s + 3, strlen(s + 3) + 1); /* BOM */
        s = str_trim(s);
        if (!*s || *s == '#') {
            free(ln);
            continue;
        }
        if (str_starts(s, "export ")) s = str_trim(s + 7);
        char *eq = strchr(s, '=');
        bool ok = eq != NULL;
        if (ok) {
            *eq = '\0';
            s = str_trim(s); /* "KEY = value" is fine too */
            ok = *s != '\0';
            for (char *k = s; ok && *k; k++) ok = key_char(*k, k == s);
        }
        if (!ok) {
            free(ln);
            return -line;
        }
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        Buf val = {0};
        if (*v == '"') {
            for (v++; *v && *v != '"'; v++) {
                if (*v == '\\' && v[1]) {
                    v++;
                    buf_putc(&val, *v == 'n' ? '\n' : *v == 't' ? '\t' : *v);
                } else {
                    buf_putc(&val, *v);
                }
            }
            if (*v != '"') {
                buf_free(&val);
                free(ln);
                return -line;
            }
        } else if (*v == '\'') {
            char *end = strchr(v + 1, '\'');
            if (!end) {
                free(ln);
                return -line;
            }
            buf_put(&val, v + 1, (size_t)(end - v - 1));
        } else {
            char *hash = strstr(v, " #"); /* trailing comment */
            if (hash) *hash = '\0';
            buf_puts(&val, str_trim(v));
        }
        cb(s, buf_cstr(&val), ctx);
        count++;
        buf_free(&val);
        free(ln);
    }
    return count;
}

static void set_one(const char *k, const char *v, void *ctx) {
    (void)ctx;
    plat_setenv(k, v, false);
}

int env_load(const char *path) {
    Buf text = {0};
    if (!file_read_all(path, &text, 1 << 20)) {
        buf_free(&text);
        return -1;
    }
    int n = env_parse(buf_cstr(&text), set_one, NULL);
    if (n < 0) log_warn("%s: line %d isn't KEY=VALUE - ignored the rest", path, -n);
    buf_free(&text);
    return n;
}

const char *env(const char *key, const char *dflt) {
    const char *v = plat_getenv(key);
    return v && *v ? v : dflt;
}

long long env_int(const char *key, long long dflt) { return str_int_or(env(key, NULL), dflt); }
