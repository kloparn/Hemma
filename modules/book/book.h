/* modules/book/book.h — module-local: the booking model and its rules.
 *
 * A Resource is anything the household shares (car, laundry room, guest
 * room, sauna...). Hourly resources are booked in `slot`-minute steps within
 * opening hours; daily resources (slot 1440, e.g. a guest room) in whole days.
 * Bookings are half-open intervals [start, end): back-to-back is fine. */
#ifndef HEMMA_BOOK_H
#define HEMMA_BOOK_H

#include "../../core/app.h"
#include "../../core/when.h"

typedef struct {
    int id;
    char name[40];
    char emoji[16];
    int hue;
    int slot;        /* minutes: 15, 30, 60, or 1440 = whole days */
    int open_min;    /* bookable from (minutes after midnight) */
    int close_min;   /* until (1440 = midnight); open 0 + close 1440 = around the clock */
    int default_min; /* suggested length of a new booking */
    int max_min;     /* longest allowed booking, 0 = no limit */
    char rules[160]; /* "Fill the tank below a quarter", shown when booking */
    char aliases[120]; /* other names quick booking knows it by, comma-separated ("bastu, bastun"; issue #55) */
    bool own_only; /* only the person a booking is for, whoever made it, or an admin may cancel or move it (#43) */
    bool kids;     /* children may book it (issue #44: the TV room, not the car) - an admin's to set */
    bool guests;   /* ... and guests (the guest room, the sauna) */
    bool archived;
    int sort;
} Resource;

typedef struct {
    int id;
    int res;
    int who; /* the person it's for */
    int by;  /* the person who booked it */
    int64_t start, end;
    char note[120];
    int64_t created;
    int series; /* made by a repeating booking (BookSeries), 0 = a one-off */
} Booking;

/* A repeating booking (issue #47: "laundry every Tuesday 18-21"): its bookings are made ahead,
   SERIES_AHEAD_DAYS at a time (topped up hourly), so every view, clash check and calendar sees them as
   bookings - and each can be moved or cancelled on its own. One that's taken by then is left out, and
   whoever it's for is told. */
typedef struct {
    int id;
    int res, who, by;
    int64_t start, end; /* the first one */
    int repeat;         /* REPEAT_* (when.h) */
    int64_t until;      /* the last day one may start on (a local midnight); 0 = until stopped */
    int made;           /* bookings 0..made-1 are made (or were left out: taken) */
    char note[120];
    int64_t created;
    bool stopped;
} BookSeries;

#define SERIES_AHEAD_DAYS 56 /* eight weeks */

/* What happened to someone's booking while someone else had it in hand (issue #43: "someone cancelled
   my laundry slot and I never knew"): shown on their Bookings page and home card until "Got it". */
typedef struct {
    int id;
    int person; /* whose booking it was */
    int64_t ts;
    char text[200]; /* "Adam cancelled your Laundry booking · Fri 14:00–17:00" (English: /api, older versions) */
    char link[160];
    char msg[480]; /* the same as its English format and values (i18n_pack): shown in the reader's language
                      (issue #46); "" = a line from before, shown as text */
} BookNews;

extern Table T_resource, T_booking, T_book_news, T_book_series;

int64_t book_series_at(const BookSeries *s, int k, int64_t *end); /* booking k's start (and end) */
int book_series_fill(int64_t now);                                /* makes what's due; returns how many */
void book_series_label(Buf *b, const BookSeries *s);              /* "every Tuesday, until 20 Dec" */
const BookSeries *book_series_of(const Booking *b);               /* NULL for a one-off */

Resource *res_get(int id);
/* May the one signed in on c cancel or move b? Everyone (not a display account), unless r is own_only:
   then the person it's for, whoever booked it, or an admin. A child or a guest: only their own (issue #44). */
bool book_may_change(const Ctx *c, const Resource *r, const Booking *b);
/* May c's person book r at all? A child only what's open to children (Resource.kids), a guest what's open to
   guests; everyone else anything (a display account looks at everything). */
bool book_open_to(const Ctx *c, const Resource *r);
int book_res_list(const Ctx *c, Resource **out, int max); /* res_list without archived ones, only book_open_to's */
int res_list(Resource **out, int max, bool with_archived); /* ordered for display */
/* The Swedish names a thing still called like its starter answers to ("Sauna": "bastu"), or "". */
const char *book_starter_aliases(const char *name);
static inline bool res_daily(const Resource *r) { return r->slot >= 1440; }

Booking *book_get(int id);
/* First booking of `res` overlapping [start, end) other than `except`; NULL when free. */
Booking *book_conflict(int res, int64_t start, int64_t end, int except);
/* Booking in progress at time t, or NULL. */
Booking *book_current(int res, int64_t t);
/* Next booking starting after t, or NULL. */
Booking *book_next(int res, int64_t t);
/* Earliest free window of `len` seconds starting at or after `from` that respects
 * opening hours, slots and the max length (searches 60 days). */
bool book_next_free(const Resource *r, int64_t from, int64_t len, int except, int64_t *start);
/* Snap to the resource's grid: start down, end up (whole days for daily resources). */
void book_snap(const Resource *r, int64_t *start, int64_t *end);
/* NULL when the booking may be made; otherwise a friendly reason (static buffer). */
const char *book_check(const Resource *r, int64_t start, int64_t end, int except, int64_t now);

/* Quick add: "car fri 14-17 ikea trip", "tvätt imorgon 18-21 @eva", "guest room 12/10-14/10" */
typedef struct {
    int res;      /* 0 = no resource recognised */
    int who;      /* 0 = the person booking */
    bool has_when;
    int64_t start, end;
    char note[120];
    char problem[200]; /* why it can't be booked as typed ("" = fine) */
    bool has_alt;      /* a free alternative when there is a clash */
    int64_t alt_start, alt_end;
    int repeat; /* "every tuesday": REPEAT_* (the first one is start..end) */
} Quick;
void book_quick_parse(const char *q, int default_res, int64_t now, Quick *out);

/* Rendering helpers shared by the page and the widgets */
/* A day mid-sentence ("until tomorrow"): Swedish and Spanish in lower case; English keeps the capital it always
   had here ("until Tomorrow 02:00") - ui_day / ui_day_inline (issue #46). */
void book_day(Buf *b, int64_t day, int64_t now);
void book_span_text(const Resource *r, int64_t start, int64_t end, int64_t now, Buf *b);
void book_page(Ctx *c, Buf *b);
void book_widget(Ctx *c, Buf *b, int where);
bool book_wall(Ctx *c, Buf *b); /* the "Bookings" wall block: free or whose now, what's coming (false: nothing to book) */

#endif
