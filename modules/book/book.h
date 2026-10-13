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
} Booking;

extern Table T_resource, T_booking;

Resource *res_get(int id);
int res_list(Resource **out, int max, bool with_archived); /* ordered for display */
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
} Quick;
void book_quick_parse(const char *q, int default_res, int64_t now, Quick *out);

/* Rendering helpers shared by the page and the widgets */
void book_span_text(const Resource *r, int64_t start, int64_t end, int64_t now, Buf *b);
void book_page(Ctx *c, Buf *b);
void book_widget(Ctx *c, Buf *b, int where);
bool book_wall(Ctx *c, Buf *b); /* the "Bookings" wall block: free or whose now, what's coming (false: nothing to book) */

#endif
