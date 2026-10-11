/* modules/media/subs.c — SRT -> WebVTT, and which subtitle belongs to which video.
 *
 * Real-world SRT files are messy: UTF-8 with or without BOM, UTF-16, or
 * Windows-1252 (most older Swedish subs), CRLF, missing blank lines,
 * {\an8} ASS tags and <font> soup. Browsers only take clean WebVTT. */
#include "mediainfo.h"

#include <ctype.h>

/* Windows-1252 0x80..0x9F (0 = undefined, dropped) */
static const uint16_t CP1252_HI[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

static void put_cp(Buf *b, uint32_t cp) {
    char e[4];
    buf_put(b, e, utf8_encode(cp, e));
}

static uint32_t u16(const unsigned char *s, bool le) {
    return le ? (uint32_t)(s[0] | s[1] << 8) : (uint32_t)(s[0] << 8 | s[1]);
}

/* Any input -> UTF-8 without BOM and without NUL bytes. */
static void decode_text(const unsigned char *s, size_t n, Buf *out) {
    if (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) {
        bool le = s[0] == 0xFF;
        for (size_t i = 2; i + 1 < n; i += 2) {
            uint32_t u = u16(s + i, le);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                uint32_t lo = u16(s + i + 2, le);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                } else {
                    u = 0xFFFD;
                }
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            if (u) put_cp(out, u);
        }
        return;
    }
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {
        s += 3;
        n -= 3;
    }
    if (utf8_valid((const char *)s, n)) {
        for (size_t i = 0; i < n; i++)
            if (s[i]) buf_putc(out, (char)s[i]);
        return;
    }
    for (size_t i = 0; i < n; i++) { /* Windows-1252 */
        unsigned c = s[i];
        if (c < 0x80) {
            if (c) buf_putc(out, (char)c);
        } else if (c < 0xA0) {
            if (CP1252_HI[c - 0x80]) put_cp(out, CP1252_HI[c - 0x80]);
        } else {
            put_cp(out, c);
        }
    }
}

typedef struct {
    const char *p;
    size_t n;
} Line;

static bool blank(Line l) {
    for (size_t i = 0; i < l.n; i++)
        if (!isspace((unsigned char)l.p[i])) return false;
    return true;
}

static bool is_index(Line l) {
    size_t a = 0, b = l.n, digits = 0;
    while (a < b && isspace((unsigned char)l.p[a])) a++;
    while (b > a && isspace((unsigned char)l.p[b - 1])) b--;
    for (size_t i = a; i < b; i++, digits++)
        if (!isdigit((unsigned char)l.p[i])) return false;
    return digits > 0 && digits < 10;
}

/* "00:01:02,345", "1:02:03.5", "01:02,345" (no hours), "00:01:02" (no millis) */
static bool parse_ts(const char **pp, const char *end, int64_t *ms) {
    const char *p = *pp;
    long parts[3];
    int np = 0;
    for (;;) {
        if (p >= end || !isdigit((unsigned char)*p)) return false;
        long v = 0;
        int nd = 0;
        while (p < end && isdigit((unsigned char)*p)) {
            if (++nd > 6) return false;
            v = v * 10 + (*p++ - '0');
        }
        parts[np++] = v;
        if (np < 3 && p < end && *p == ':') {
            p++;
            continue;
        }
        break;
    }
    if (np < 2) return false;
    long frac = 0;
    if (p < end && (*p == ',' || *p == '.')) {
        p++;
        int nd = 0;
        long scale = 100;
        while (p < end && isdigit((unsigned char)*p)) {
            if (nd < 3) {
                frac += (*p - '0') * scale;
                scale /= 10;
            }
            nd++;
            p++;
        }
        if (!nd) return false;
    }
    long h = np == 3 ? parts[0] : 0, m = parts[np - 2], s = parts[np - 1];
    if (m > 59 || s > 59) return false;
    *ms = ((int64_t)(h * 60 + m) * 60 + s) * 1000 + frac;
    *pp = p;
    return true;
}

static bool parse_timing(Line l, int64_t *a, int64_t *b) {
    const char *p = l.p, *end = l.p + l.n;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (!parse_ts(&p, end, a)) return false;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (end - p >= 3 && !memcmp(p, "-->", 3)) p += 3;
    else if (end - p >= 2 && !memcmp(p, "->", 2)) p += 2;
    else return false;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return parse_ts(&p, end, b); /* anything after (X1:... positions) is ignored */
}

static bool is_entity(const char *p, size_t n) {
    static const char *const ents[] = {"&amp;", "&lt;", "&gt;", "&nbsp;", "&lrm;", "&rlm;", "&quot;", NULL};
    for (int i = 0; ents[i]; i++) {
        size_t k = strlen(ents[i]);
        if (n >= k && !memcmp(p, ents[i], k)) return true;
    }
    if (n >= 4 && p[1] == '#') { /* &#123; &#x1F600; */
        size_t j = 2;
        if (p[j] == 'x' || p[j] == 'X') j++;
        size_t d0 = j;
        while (j < n && isxdigit((unsigned char)p[j])) j++;
        return j > d0 && j < n && p[j] == ';';
    }
    return false;
}

/* One line of cue text, cleaned for WebVTT. */
static void cue_text(const char *p, size_t n, Buf *out) {
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    for (; i < n; i++) {
        char c = p[i];
        if (c == '{' && i + 1 < n && p[i + 1] == '\\') { /* ASS override: {\an8} {\i1} */
            size_t j = i + 2;
            while (j < n && p[j] != '}') j++;
            if (j < n) {
                i = j;
                continue;
            }
        }
        if (c == '<') {
            size_t j = i + 1;
            bool closing = j < n && p[j] == '/';
            if (closing) j++;
            size_t ns = j;
            while (j < n && isalpha((unsigned char)p[j])) j++;
            size_t name_len = j - ns, close = j;
            while (close < n && p[close] != '>' && close - i < 128) close++;
            if (name_len && close < n && p[close] == '>') {
                char t = (char)tolower((unsigned char)p[ns]);
                if (name_len == 1 && (t == 'i' || t == 'b' || t == 'u')) {
                    buf_putc(out, '<');
                    if (closing) buf_putc(out, '/');
                    buf_putc(out, t);
                    buf_putc(out, '>');
                }
                i = close; /* <font ...>, <br>, <span> ...: drop the tag, keep the text */
                continue;
            }
            buf_puts(out, "&lt;");
            continue;
        }
        if (c == '>') {
            buf_puts(out, "&gt;"); /* also defuses a literal "-->" */
            continue;
        }
        if (c == '&') {
            buf_puts(out, is_entity(p + i, n - i) ? "&" : "&amp;");
            continue;
        }
        buf_putc(out, c);
    }
}

static void put_time(Buf *b, int64_t ms) {
    buf_printf(b, "%02lld:%02lld:%02lld.%03lld", (long long)(ms / 3600000), (long long)(ms / 60000 % 60),
               (long long)(ms / 1000 % 60), (long long)(ms % 1000));
}

bool srt_to_vtt(const char *src, size_t n, Buf *out) {
    buf_puts(out, "WEBVTT\n\n");
    if (!src || !n) return false;
    Buf text = {0};
    decode_text((const unsigned char *)src, n, &text);
    if (!text.len) {
        buf_free(&text);
        return false;
    }

    Line *lines = NULL;
    size_t nl = 0, cap = 0;
    const char *p = text.p, *end = text.p + text.len;
    while (p && p < end) {
        const char *e = p;
        while (e < end && *e != '\n' && *e != '\r') e++;
        if (nl == cap) {
            cap = cap ? cap * 2 : 256;
            lines = xrealloc(lines, cap * sizeof *lines);
        }
        lines[nl++] = (Line){p, (size_t)(e - p)};
        if (e < end && *e == '\r' && e + 1 < end && e[1] == '\n') e++;
        p = e + 1;
    }

    int cues = 0;
    Buf cue = {0};
    for (size_t i = 0; i < nl;) {
        int64_t a, b, x, y;
        if (!parse_timing(lines[i], &a, &b)) { /* index numbers and garbage */
            i++;
            continue;
        }
        i++;
        buf_reset(&cue);
        while (i < nl && !blank(lines[i])) {
            if (parse_timing(lines[i], &x, &y)) break; /* missing blank line */
            if (is_index(lines[i]) && i + 1 < nl && parse_timing(lines[i + 1], &x, &y)) break;
            size_t before = cue.len;
            if (cue.len) buf_putc(&cue, '\n');
            size_t mark = cue.len;
            cue_text(lines[i].p, lines[i].n, &cue);
            if (cue.len == mark) cue.len = before; /* line was only tags */
            i++;
        }
        if (!cue.len) continue;
        if (b < a) b = a;
        put_time(out, a);
        buf_puts(out, " --> ");
        put_time(out, b);
        buf_putc(out, '\n');
        buf_put(out, cue.p, cue.len);
        buf_puts(out, "\n\n");
        cues++;
    }
    free(lines);
    buf_free(&text);
    buf_free(&cue);
    return cues > 0;
}

/* ------------------------------------------------------ subtitle matching */

static const struct {
    const char *name, *code;
} LANGS[] = {
    {"english", "en"},  {"eng", "en"},       {"swedish", "sv"},    {"svenska", "sv"},   {"swe", "sv"},
    {"sve", "sv"},      {"se", "sv"},        {"norwegian", "no"},  {"norsk", "no"},     {"nor", "no"},
    {"nob", "no"},      {"nb", "no"},        {"danish", "da"},     {"dansk", "da"},     {"dan", "da"},
    {"finnish", "fi"},  {"suomi", "fi"},     {"fin", "fi"},        {"german", "de"},    {"deutsch", "de"},
    {"ger", "de"},      {"deu", "de"},       {"french", "fr"},     {"français", "fr"},  {"fre", "fr"},
    {"fra", "fr"},      {"spanish", "es"},   {"español", "es"},    {"spa", "es"},       {"italian", "it"},
    {"ita", "it"},      {"dutch", "nl"},     {"nld", "nl"},        {"dut", "nl"},       {"polish", "pl"},
    {"pol", "pl"},      {"portuguese", "pt"}, {"por", "pt"},       {"japanese", "ja"},  {"jpn", "ja"},
    {"korean", "ko"},   {"kor", "ko"},       {"chinese", "zh"},    {"chi", "zh"},       {"zho", "zh"},
    {"russian", "ru"},  {"rus", "ru"},       {"arabic", "ar"},     {"ara", "ar"},       {"turkish", "tr"},
    {"tur", "tr"},      {"icelandic", "is"}, {"isl", "is"},        {"ice", "is"},
};

static const char *const SUB_FLAGS[] = {"forced", "sdh", "cc", "hi", "default", "full", "sub", "subs", "subtitle",
                                        "subtitles", "foreign", "text", NULL};

bool media_sub_for(const char *video, const char *sub, char *lang, size_t cap) {
    if (lang && cap) lang[0] = 0;
    if (!video || !sub || !media_is_sub_ext(sub)) return false;
    char vb[512], sb[512];
    fold_copy(vb, sizeof vb, video);
    fold_copy(sb, sizeof sb, sub);
    char *dot = strrchr(vb, '.');
    if (dot && dot != vb) *dot = 0;
    dot = strrchr(sb, '.');
    if (dot) *dot = 0;
    size_t n = strlen(vb);
    if (!n || strncmp(sb, vb, n) != 0) return false;
    const char *rest = sb + n;
    if (!*rest) return true;
    if (!strchr("._- ", *rest)) return false;

    char found[16] = "";
    char tags[256];
    str_copy(tags, sizeof tags, rest + 1);
    for (char *t = tags;;) {
        char *e = t + strcspn(t, "._- ");
        char c = *e;
        *e = 0;
        if (*t) {
            const char *code = NULL;
            for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0] && !code; i++)
                if (!strcmp(t, LANGS[i].name)) code = LANGS[i].code;
            bool flag = false;
            for (int i = 0; SUB_FLAGS[i]; i++)
                if (!strcmp(t, SUB_FLAGS[i])) flag = true;
            if (!code && !flag && strlen(t) == 2 && isalpha((unsigned char)t[0]) && isalpha((unsigned char)t[1]))
                code = t;
            if (!code && !flag) return false; /* "Movie 2.srt" is not a subtitle for "Movie.mkv" */
            if (code && !found[0]) str_copy(found, sizeof found, code);
        }
        if (!c) break;
        t = e + 1;
    }
    if (lang && cap) str_copy(lang, cap, found);
    return true;
}
