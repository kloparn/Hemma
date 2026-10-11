/* core/html.h — HTML output that escapes by default.
 *
 *   H(b, "<a href=\"/book?r=%d\" title=\"%s\">%s</a>", id, title, name);
 *
 * Directives (NOT printf — the argument types below are what H expects):
 *   %s  const char*  escaped text; safe in element text and quoted attributes
 *   %r  const char*  raw, trusted HTML (constants, other renderers' output)
 *   %u  const char*  URL-component encoded (query values), then escaped
 *   %d  int          %L  long long / int64_t    %X  uint64_t as 16 hex digits
 *       numbers take printf-style padding: %02d  %3L
 *   %c  int (char)   %%  literal percent
 * NULL strings print as nothing. Invalid UTF-8 is replaced by U+FFFD, so the
 * output can always travel in a WebSocket text frame. */
#ifndef HEMMA_HTML_H
#define HEMMA_HTML_H

#include "base.h"

void H(Buf *b, const char *fmt, ...);
void html_esc(Buf *b, const char *s);
void html_escn(Buf *b, const char *s, size_t n);

#endif
