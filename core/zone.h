/* core/zone.h — this computer's time zone, and what a change of it means for the times Hemma keeps
 * (issue #39).
 *
 * Times are kept as moments, worked out from the zone when they were typed; a DAY (an all-day event, a
 * chore's due day, a whole-day booking, the last day of a note) as that day's local midnight. When the
 * zone changes (seen at a start), the days are moved to the new zone's midnight of the same day by
 * themselves - a day is a day. Typed times are moved back to the times typed only when an admin asks
 * (the System page shows what would move first): right after a wrong zone (a Pi left on UTC for a
 * week), wrong after a real move. Modules take part through Module.rezone (app.h). */
#ifndef HEMMA_ZONE_H
#define HEMMA_ZONE_H

#include "base.h"

/* "+01:00/+02:00": the offsets from UTC in January and July, "+00:00" when they're the same. (A zone's
   name isn't portable; what it does to times is.) */
void zone_sig(char *out, size_t cap);
void zone_words(const char *sig, char *out, size_t cap); /* "UTC+01:00 in January, UTC+02:00 in July" */
bool zone_offsets(const char *sig, int *jan, int *jul);  /* seconds east of UTC; false: not a signature */
void zone_test_sig(const char *sig);                     /* tests: as if the zone were sig ("" = the real one) */

#define REZONE_NOTES 8
typedef struct Rezone {
    bool days;      /* this pass: the days (at the start, by itself) - else typed times (an admin's choice) */
    bool apply;     /* false: only count what would move, and note the first few (the System page) */
    int64_t before; /* typed times: only things entered before this moment */
    int jan, jul;   /* the OLD zone's offsets */
    int notes;      /* how many of note[] are used */
    char note[REZONE_NOTES][160];
} Rezone;

/* *t, a day kept as the old zone's midnight -> this zone's midnight of the same day. false (*t as it
   was) when it's a midnight here already, or not one in the old zone either (then it's no day). */
bool rezone_day(const Rezone *z, int64_t *t);
/* *t, a time typed under the old zone -> the moment that reads the same on the clock now. (An old zone
   with summer time: its summer is told by this zone's, else by the EU's dates - exact for a zone
   without one, like a Pi's UTC.) false when nothing changes. */
bool rezone_time(const Rezone *z, int64_t *t);
/* Both ends of a span [*start, *end): kept as days (whole days: rezone_day) or typed (rezone_time, only
   when made before z->before) - each in its own pass. true when either end moved. */
bool rezone_span(const Rezone *z, bool days, int64_t made, int64_t *start, int64_t *end);
/* For the preview: "Dentist · Tue 13 Oct 16:00 → 14:00" - what it is, when it shows now, and would. */
void rezone_note(Rezone *z, const char *what, int64_t was, int64_t would);

/* The last change of zone, until an admin has seen it */
typedef struct {
    char from[32], to[32], day[11];
    int64_t at;  /* when it was noticed: what was entered before then was typed under the old zone */
    int kept;    /* days moved to the new zone's midnights at that start */
    int retimed; /* -1: typed times not moved back (yet); else how many were */
} ZoneChange;
bool zone_change(ZoneChange *out); /* false: none waiting */
void zone_boot(void);              /* app_boot: after the store loads, before the modules start */
/* Typed times entered before the change: how many would move back to the times typed (apply false,
   with notes in *z), or did (apply true, once). */
int zone_retime(bool apply, Rezone *z);
void zone_seen(void); /* "Got it" */

#endif
