/* modules/media/upload.h — admins add videos to the library from a browser.
 * Module-internal (upload.c); media.c wires the page, endpoint, action and tick. */
#ifndef HEMMA_MEDIA_UPLOAD_H
#define HEMMA_MEDIA_UPLOAD_H

#include "library.h"

bool upload_http(Ctx *c, Conn *conn, HttpReq *r); /* /media/upload/... endpoints */
void upload_page(Ctx *c, Buf *b);                 /* the /media/upload page */
void upload_summary(Ctx *c, Buf *b);              /* one-line status for the settings page */
/* Pictures for a series or film go the same way: the plan carries "art":"<id>" and one image,
   saved where media_art_target() says (replacing the old one). This renders how this person's
   latest picture for `id` is doing (nothing when there is none). */
void upload_art_status(Ctx *c, Buf *b, uint64_t id);
void upload_act_stop(Ctx *c, Fields *f);          /* action media.upload_stop */
void upload_start(void);
void upload_tick(int64_t now_ms);
void upload_stop(void); /* module stop: running transfers give up (their .part files stay for resuming) */

/* "Show/Season 1/x.mkv" -> the same, every part checked (no "..", no names
   Windows can't store, valid UTF-8). False when anything is off. */
bool upload_clean_rel(const char *in, char *out, size_t cap);
void upload_test_free_space(int64_t bytes); /* tests: pretend the disk has this much room (-1 = ask it) */

#endif
