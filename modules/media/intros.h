/* modules/media/intros.h — a series' intros, found by themselves, and the player's "Skip intro" (2.9).
 * Module-internal: media.c wires the card, the actions, start, tick and stop; the watch page asks
 * media_intro_of; Manage shows and fixes an episode's (manage.c). */
#ifndef HEMMA_MEDIA_INTROS_H
#define HEMMA_MEDIA_INTROS_H

#include "../../core/app.h"
#include "library.h"

#define INTROS_SETTING "media.intros" /* "1" (or unset) = on, "0" = off: admins, Library settings */

void intros_start(void); /* its table (its own format), what's known into memory */
void intros_tick(int64_t now_ms);
void intros_stop(void);  /* a season being read is dropped (what was read stays in memory for the next time) */
bool intros_busy(void);  /* a season is being read (subtake.c gives way, the disk being one) */

void intros_card(Ctx *c, Buf *b);           /* Library settings (admins): how far it is, the switch */
void intros_act_switch(Ctx *c, Fields *f);  /* media.intros: on=1 */
void intros_act_set(Ctx *c, Fields *f);     /* media.intro (admins): id, how = set (from, to: "1:02") | none | auto */

/* The player: the episode's intro in ms - found by itself, or set on Manage; false = none known. */
bool media_intro_of(const MediaFile *f, int64_t *from_ms, int64_t *to_ms);
/* Manage: what an episode's row says of it (" · Intro 1:02–1:34"), and its dialog (?intro=<id>). */
void media_intro_note(Buf *b, const MediaFile *e);
void media_intro_dialog(Ctx *c, Buf *b, const MediaFile *e, const char *close);

/* tests (no thread there: the tick never starts one) - one season read and matched on this thread: 1 = a season
   done, 0 = none left, -1 = waiting (switched off, someone watching, a conversion, a folder away) */
int intros_test_step(void);
void intros_test_read_s(int seconds); /* how much of each episode is read (0 = as for real) */
int intros_test_reads(void);          /* episodes read since the start (the rest came from memory) */

#endif
