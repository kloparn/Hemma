/* core/json.c — see json.h */
#include "json.h"

typedef struct {
    const char *s;
    size_t n, i;
    JsonTok *t;
    int nt, max, depth;
} Parser;

static void skip_ws(Parser *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->i++;
    }
}

static int new_tok(Parser *p, JsonType type, size_t start) {
    if (p->nt >= p->max) return -1;
    JsonTok *t = &p->t[p->nt];
    t->type = type;
    t->start = (int)start;
    t->len = 0;
    t->count = 0;
    t->skip = 0;
    return p->nt++;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_value(Parser *p);

static int parse_string(Parser *p) {
    p->i++; /* opening quote */
    int idx = new_tok(p, JSON_STRING, p->i);
    if (idx < 0) return -1;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->t[idx].len = (int)(p->i - (size_t)p->t[idx].start);
            p->i++;
            p->t[idx].skip = p->nt;
            return idx;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (p->i + 1 >= p->n) return -1;
            char e = p->s[p->i + 1];
            if (e == 'u') {
                if (p->i + 5 >= p->n) return -1;
                for (int k = 2; k < 6; k++)
                    if (!is_hex(p->s[p->i + (size_t)k])) return -1;
                p->i += 6;
                continue;
            }
            if (!strchr("\"\\/bfnrt", e)) return -1;
            p->i += 2;
            continue;
        }
        p->i++;
    }
    return -1;
}

static int parse_number(Parser *p) {
    size_t st = p->i;
    if (p->s[p->i] == '-') p->i++;
    if (p->i >= p->n) return -1;
    if (p->s[p->i] == '0') p->i++;
    else if (is_digit(p->s[p->i]))
        while (p->i < p->n && is_digit(p->s[p->i])) p->i++;
    else return -1;
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (p->i >= p->n || !is_digit(p->s[p->i])) return -1;
        while (p->i < p->n && is_digit(p->s[p->i])) p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i >= p->n || !is_digit(p->s[p->i])) return -1;
        while (p->i < p->n && is_digit(p->s[p->i])) p->i++;
    }
    int idx = new_tok(p, JSON_NUMBER, st);
    if (idx < 0) return -1;
    p->t[idx].len = (int)(p->i - st);
    p->t[idx].skip = p->nt;
    return idx;
}

static int parse_literal(Parser *p, const char *word, JsonType type) {
    size_t k = strlen(word);
    if (p->n - p->i < k || memcmp(p->s + p->i, word, k) != 0) return -1;
    int idx = new_tok(p, type, p->i);
    if (idx < 0) return -1;
    p->i += k;
    p->t[idx].len = (int)k;
    p->t[idx].skip = p->nt;
    return idx;
}

static int parse_container(Parser *p, bool object) {
    if (++p->depth > 32) return -1;
    size_t st = p->i;
    int idx = new_tok(p, object ? JSON_OBJECT : JSON_ARRAY, st);
    if (idx < 0) return -1;
    char close = object ? '}' : ']';
    p->i++;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
    } else {
        for (;;) {
            skip_ws(p);
            if (object) {
                if (p->i >= p->n || p->s[p->i] != '"') return -1;
                if (parse_string(p) < 0) return -1;
                skip_ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return -1;
                p->i++;
            }
            if (parse_value(p) < 0) return -1;
            p->t[idx].count++;
            skip_ws(p);
            if (p->i >= p->n) return -1;
            char c = p->s[p->i++];
            if (c == ',') continue;
            if (c == close) break;
            return -1;
        }
    }
    p->t[idx].len = (int)(p->i - st);
    p->t[idx].skip = p->nt;
    p->depth--;
    return idx;
}

static int parse_value(Parser *p) {
    skip_ws(p);
    if (p->i >= p->n) return -1;
    switch (p->s[p->i]) {
    case '{': return parse_container(p, true);
    case '[': return parse_container(p, false);
    case '"': return parse_string(p);
    case 't': return parse_literal(p, "true", JSON_TRUE);
    case 'f': return parse_literal(p, "false", JSON_FALSE);
    case 'n': return parse_literal(p, "null", JSON_NULL);
    default: return parse_number(p);
    }
}

bool json_parse(Json *j, const char *src, size_t len, JsonTok *toks, int max) {
    Parser p = {src, len, 0, toks, 0, max, 0};
    j->src = src;
    j->tok = toks;
    j->n = 0;
    if (len > 0x7FFFFFF0) return false;
    if (parse_value(&p) < 0) return false;
    skip_ws(&p);
    if (p.i != p.n) return false;
    j->n = p.nt;
    return true;
}

int json_child(const Json *j, int parent) {
    if (parent < 0 || parent >= j->n) return -1;
    const JsonTok *t = &j->tok[parent];
    if ((t->type != JSON_OBJECT && t->type != JSON_ARRAY) || t->count == 0) return -1;
    return parent + 1;
}

int json_next(const Json *j, int parent, int cur) {
    if (parent < 0 || cur <= parent || cur >= j->n) return -1;
    int nx = j->tok[parent].type == JSON_OBJECT ? (cur + 1 < j->n ? j->tok[cur + 1].skip : j->n) : j->tok[cur].skip;
    return nx < j->tok[parent].skip ? nx : -1;
}

int json_get(const Json *j, int obj, const char *key) {
    if (json_type(j, obj) != JSON_OBJECT) return -1;
    for (int k = json_child(j, obj); k >= 0; k = json_next(j, obj, k))
        if (json_eq(j, k, key)) return k + 1;
    return -1;
}

int json_index(const Json *j, int arr, int i) {
    if (json_type(j, arr) != JSON_ARRAY || i < 0) return -1;
    int k = json_child(j, arr);
    while (k >= 0 && i-- > 0) k = json_next(j, arr, k);
    return k;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return v;
}

/* Unescape a string token into out (cap bytes incl. NUL). */
static size_t unescape(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n;) {
        char enc[4];
        size_t elen;
        if (s[i] != '\\') {
            /* copy one UTF-8 sequence (raw input is validated by the WS layer / our writers) */
            size_t k = 1;
            while (i + k < n && ((unsigned char)s[i + k] & 0xC0) == 0x80) k++;
            if (o + k > cap - 1) break;
            memcpy(out + o, s + i, k);
            o += k;
            i += k;
            continue;
        }
        char e = s[i + 1];
        i += 2;
        uint32_t cp;
        switch (e) {
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            cp = (uint32_t)hex4(s + i);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
                uint32_t lo = (uint32_t)hex4(s + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            break;
        default: cp = (uint32_t)(unsigned char)e; break; /* \" \\ \/ */
        }
        elen = utf8_encode(cp, enc);
        if (o + elen > cap - 1) break;
        memcpy(out + o, enc, elen);
        o += elen;
    }
    out[o] = '\0';
    return o;
}

bool json_string(const Json *j, int t, char *out, size_t cap) {
    if (json_type(j, t) != JSON_STRING) {
        if (cap) out[0] = '\0';
        return false;
    }
    unescape(j->src + j->tok[t].start, (size_t)j->tok[t].len, out, cap);
    return true;
}

bool json_integer(const Json *j, int t, long long *out) {
    if (json_type(j, t) != JSON_NUMBER) return false;
    char tmp[32];
    int len = j->tok[t].len;
    if (len >= (int)sizeof tmp) return false;
    memcpy(tmp, j->src + j->tok[t].start, (size_t)len);
    tmp[len] = '\0';
    return str_int(tmp, out);
}

bool json_bool(const Json *j, int t, bool *out) {
    JsonType ty = json_type(j, t);
    if (ty != JSON_TRUE && ty != JSON_FALSE) return false;
    *out = ty == JSON_TRUE;
    return true;
}

bool json_eq(const Json *j, int t, const char *s) {
    if (json_type(j, t) != JSON_STRING || !s) return false;
    const char *raw = j->src + j->tok[t].start;
    size_t len = (size_t)j->tok[t].len, sl = strlen(s);
    if (!memchr(raw, '\\', len)) return len == sl && memcmp(raw, s, len) == 0;
    char tmp[256];
    if (sl >= sizeof tmp) return false;
    size_t n = unescape(raw, len, tmp, sizeof tmp);
    return n == sl && memcmp(tmp, s, n) == 0;
}

void json_strn(Buf *b, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    buf_reserve(b, n + 2);
    buf_putc(b, '"');
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            uint32_t cp;
            int k = utf8_decode(s + i, n - i, &cp);
            if (k == 1) buf_puts(b, "\xEF\xBF\xBD"); /* invalid byte -> U+FFFD (valid sequences are >= 2 bytes) */
            else buf_put(b, s + i, (size_t)k);
            i += (size_t)k;
            continue;
        }
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char e[6] = {'\\', 'u', '0', '0', hx[c >> 4], hx[c & 15]};
                buf_put(b, e, 6);
            } else {
                buf_putc(b, (char)c);
            }
        }
        i++;
    }
    buf_putc(b, '"');
}

void json_str(Buf *b, const char *s) { json_strn(b, s ? s : "", s ? strlen(s) : 0); }
