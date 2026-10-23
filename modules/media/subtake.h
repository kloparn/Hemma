/* modules/media/subtake.h — the subtitles inside the library's MKVs, taken out by themselves (1.2), and
 * the picture subtitles' endpoints that draw them for the player (stream.c, contract C5).
 * Module-internal: media.c wires the card, the switch, start, tick and stop. */
#ifndef HEMMA_MEDIA_SUBTAKE_H
#define HEMMA_MEDIA_SUBTAKE_H

#include "../../core/app.h"

/* ---- the job (subtake.c) ---- */
#define SUBTAKE_SETTING "media.take_subs" /* "1" (or unset) = on, "0" = off: admins, Library settings */

void subtake_start(void); /* the done table (its own format), what's done into memory */
void subtake_tick(int64_t now_ms);
void subtake_stop(void); /* the file being read is dropped (read again next time) */
void subtake_card(Ctx *c, Buf *b);           /* Library settings (admins): how far it is, the switch */
void subtake_act_switch(Ctx *c, Fields *f);  /* media.take_subs: on=1 */

/* tests (no thread there: the tick never starts one) - what a run does, one file at a time on this thread:
   1 = a file looked at, 0 = none left (the library rescans first when something was saved), -1 = waiting
   (switched off, someone watching, a conversion queued or running, a library folder away) */
int subtake_test_step(void);
int subtake_test_reads(void); /* files read whole (av_subtitles) since the start */
int subtake_test_opens(void); /* files whose headers were read (mkv_open) since the start */
void subtake_test_halt_after(int reads); /* a file being read whole stops after this many reads (0 = never) */

/* ---- picture subtitles on the wire (stream.c, C5) ---- */
/* GET /media/subpic/<id>.json = {"w":..,"h":..,"cues":[[start_ms,end_ms,x,y,w,h],...]} of a .sup / .idx in the
   library; GET /media/subpic/<id>/<n>.png = cue n drawn (subpic_render + png_indexed). Opened files are kept
   (at most MEDIA_SUBPIC_CACHE, by id + size + mtime): playing doesn't index a film's .sup for every picture. */
#define MEDIA_SUBPIC_CACHE 4
void media_subpic_drop(void);      /* forget the opened ones (media_stream_stop / _resume) */
int media_subpic_test_opens(void); /* tests: files opened (indexed) since the start */

#endif
