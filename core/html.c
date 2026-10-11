/* core/html.c — see html.h */
#include "html.h"
#include "codec.h"

void html_escn(Buf *b, const char *s, size_t n) {
    if (!s) return;
    buf_reserve(b, n + n / 8);
    size_t i = 0, run = 0; /* run: start of a span that needs no escaping */
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = NULL;
        size_t adv = 1;
        if (c < 0x80) {
            switch (c) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            case 0: rep = "\xEF\xBF\xBD"; break;
            default: break;
            }
        } else {
            uint32_t cp;
            int k = utf8_decode(s + i, n - i, &cp);
            if (k == 1) rep = "\xEF\xBF\xBD"; /* invalid byte */
            else adv = (size_t)k;
        }
        if (rep) {
            buf_put(b, s + run, i - run);
            buf_puts(b, rep);
            run = i + 1;
        }
        i += adv;
    }
    buf_put(b, s + run, n - run);
}

void html_esc(Buf *b, const char *s) {
    if (s) html_escn(b, s, strlen(s));
}

void H(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *p = fmt, *lit = fmt;
    while (*p) {
        if (*p != '%') {
            p++;
            continue;
        }
        buf_put(b, lit, (size_t)(p - lit));
        p++;
        /* optional printf-style padding for numbers: %02d, %5L */
        bool zero = false;
        int width = 0;
        if (*p == '0') zero = true, p++;
        while (*p >= '0' && *p <= '9' && width < 100) width = width * 10 + (*p++ - '0');
        char d = *p;
        if (d) p++;
        lit = p;
        switch (d) {
        case 'd':
        case 'L': {
            long long v = d == 'd' ? (long long)va_arg(ap, int) : va_arg(ap, long long);
            if (width) buf_printf(b, zero ? "%0*lld" : "%*lld", width, v);
            else buf_printf(b, "%lld", v);
            break;
        }
        case 's': html_esc(b, va_arg(ap, const char *)); break;
        case 'r': buf_puts(b, va_arg(ap, const char *)); break;
        case 'u': {
            Buf u = {0};
            url_encode(&u, va_arg(ap, const char *), false);
            buf_put(b, u.p, u.len); /* only [A-Za-z0-9-_.~%] remain: nothing to escape */
            buf_free(&u);
            break;
        }
        case 'X': buf_printf(b, "%016llx", (unsigned long long)va_arg(ap, uint64_t)); break;
        case 'c': buf_putc(b, (char)va_arg(ap, int)); break;
        case '%': buf_putc(b, '%'); break;
        default: /* unknown directive: print it verbatim so the bug is visible */
            buf_putc(b, '%');
            if (d) buf_putc(b, d);
            break;
        }
    }
    buf_put(b, lit, (size_t)(p - lit));
    va_end(ap);
}
