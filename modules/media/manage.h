/* modules/media/manage.h — admins look after what's in the library (manage.c): the Manage view of a
 * series, fixing a film's title or an episode's number, removing episodes, seasons and series, and
 * what takes room. Module-internal: media.c calls these from its pages, actions and tick. */
#ifndef HEMMA_MEDIA_MANAGE_H
#define HEMMA_MEDIA_MANAGE_H

#include "library.h"

/* The series page (media.c page_series): the header's "Manage" link (in the Manage view: "Done") ... */
void media_manage_series_link(Ctx *c, Buf *b, const MediaSeries *s);
/* ... and the view itself, ?manage=1 (admins): true = drawn, the page ends there. */
bool media_manage_series(Ctx *c, Buf *b, const MediaSeries *s);
/* The watch page (page_watch): "Fix title" beside Remove (films and other videos), and its dialog (?fix=1). */
void media_manage_watch_button(Ctx *c, Buf *b, const MediaFile *f);
void media_manage_watch_dialog(Ctx *c, Buf *b, const MediaFile *f);
/* Library settings (page_settings): the biggest series and films (admins). */
void media_manage_room_card(Ctx *c, Buf *b);

void media_manage_act_remove(Ctx *c, Fields *f);      /* media.manage_remove: what=episode|season|series, id, s */
void media_manage_act_fix_title(Ctx *c, Fields *f);   /* media.fix_title: id, title, year */
void media_manage_act_fix_episode(Ctx *c, Fields *f); /* media.fix_episode: id, season, episode, last, title */
void media_manage_tick(int64_t now);                  /* a removal on its thread finished: history, Latest, rescan */

#endif
