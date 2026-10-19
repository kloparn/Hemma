/* modules/plan/plan.h — internals shared by the plan module's files and its
 * tests. Nothing outside modules/plan/ includes this. */
#ifndef HEMMA_PLAN_H
#define HEMMA_PLAN_H

#include "../../core/app.h"
#include "../../core/ui.h"
#include "../../core/when.h"

/* ------------------------------------------------------------------ data */

typedef struct {
    int id;
    char title[120];
    int who;     /* assignee, 0 = anyone */
    int64_t due; /* local midnight of the due day, 0 = someday */
    int repeat;  /* REPEAT_* (when.h) */
    int rep_day; /* weekly/biweekly: ISO weekday 1..7 (0 = due's weekday); monthly: day of month 1..31 */
    int done_by;
    int64_t done_at; /* 0 = still to do */
    int by;          /* created by */
    int64_t created;
    int spawned; /* repeating: the next occurrence created on completion (undo removes it) */
    /* Taking turns (issue #47): the people, in order, as ids ("3,5,9"; "" = not). Ticking it off gives the
       next one in line the next time. turn = whose place in the line this time is; swap = the person who
       swapped this time away, and so takes the next one. */
    char turns[48];
    int turn;
    int swap;
} Task;

#define PLAN_TURNS 8 /* people taking turns at most */

typedef struct {
    int id;
    char text[100];
    bool done; /* in the basket */
    int by;
    int64_t created;
    int64_t done_at;
} Item;

typedef struct { /* how often something gets bought: feeds the suggestions */
    int id;
    char key[100]; /* folded text */
    char text[100];
    int count;
    int64_t last;
} Bought;

typedef struct {
    int id;
    char title[120];
    int64_t start, end; /* end exclusive; all-day: local midnights */
    bool allday;
    int who; /* 0 = everyone */
    char note[200];
    int by;
    int64_t created;
    int series; /* made by a repeating event (EventSeries), 0 = a one-off */
} Event;

/* A repeating event (issue #47): its events are made ahead (a year; a daily one 60 days) so the calendar,
   the wall and the phone's calendar see them as events; each one can then change or go on its own. */
typedef struct {
    int id;
    char title[120];
    int64_t start, end; /* the first one; all-day: local midnights */
    bool allday;
    int who;
    char note[200];
    int repeat;    /* REPEAT_* (when.h) */
    int64_t until; /* the last day one may start on (a local midnight); 0 = until stopped */
    int made;      /* events 0..made-1 are made */
    int by;
    int64_t created;
    bool stopped;
} EventSeries;

extern Table T_task, T_item, T_bought, T_event, T_event_series;

/* --------------------------------------------------------------- parsing */

typedef struct {
    char title[120];
    int who;
    int64_t due; /* 0 = someday */
    int repeat, rep_day;
    int turns[PLAN_TURNS]; /* everyone named ("@anna,@erik"), in order: taking turns when it repeats */
    int nturns;
} TaskDraft;

typedef struct {
    char title[120];
    int who;
    int64_t start, end;
    bool allday;
    int repeat; /* "every wednesday": REPEAT_* */
} EventDraft;

/* "vacuum @eva fri every week" -> title, assignee, due, repeat. Returns false
 * (with a reason in err) when there is no title. */
bool plan_parse_task(const char *text, int64_t now, TaskDraft *out, char *err, size_t errcap);
/* "dentist tue 14:30 @adam", "grandma visits fri-sun". Needs a title and a day. */
bool plan_parse_event(const char *text, int64_t now, EventDraft *out, char *err, size_t errcap);
/* "milk, eggs, 2 bread" -> up to max cleaned item names; returns the count. */
int plan_split_items(const char *text, char out[][100], int max);

/* Next due day after completing a repeating task (always after today). */
int64_t plan_next_due(const Task *t, int64_t now);
/* Taking turns: the line's ids (people since removed too) - returns how many; and who does the next time
   (the one who swapped this time away, else the next one in line still in the household), with *place
   = its place in the line. 0 = not taking turns. */
int plan_turn_ids(const Task *t, int out[PLAN_TURNS]);
int plan_next_turn(const Task *t, int *place);
/* Takes `person` out of t's line (they left the household: issue #45); an open one that was their turn goes to
   the next in line. False when they weren't in it. */
bool plan_drop_turn(Task *t, int person);
void plan_turn_names(Buf *b, const Task *t); /* "Anna, Erik" */

/* Repeating events: occurrence k of a series (start; *end), and making what's due (a year ahead). */
int64_t plan_series_at(const EventSeries *s, int k, int64_t *end);
int plan_series_fill(int64_t now); /* returns how many events were made */
void plan_series_label(Buf *b, const EventSeries *s); /* "every Wednesday, until 20 Dec" */

enum { GRP_OVERDUE, GRP_TODAY, GRP_TOMORROW, GRP_WEEK, GRP_LATER, GRP_SOMEDAY, GRP_COUNT };
int plan_task_group(const Task *t, int64_t now);

/* --------------------------------------------------------------- helpers */

const char *plan_task_icon(const char *title); /* 🧹 for "vacuum", 🗑️ for "trash", ... or "" */
void plan_repeat_label(Buf *b, int repeat, int rep_day);
void plan_capitalise(char *s);
void plan_tabs(Ctx *c, Buf *b, int active); /* 0 to-dos, 1 shopping, 2 calendar (a guest: none - only shopping) */
/* Whose to-dos c's person sees: a child their own only (issue #44) - their id; anyone else 0 = everyone's */
int plan_mine(const Ctx *c);

/* pages + widget parts (one file each) */
void plan_tasks_page(Ctx *c, Buf *b);
void plan_shop_page(Ctx *c, Buf *b);
void plan_cal_page(Ctx *c, Buf *b);
int plan_today_tasks(Ctx *c, Buf *b, bool wall);  /* overdue + due today rows; returns how many */
int plan_today_events(Ctx *c, Buf *b, bool wall); /* today's event rows; returns how many */
void plan_shop_widget(Ctx *c, Buf *b); /* the home dashboard's card */
/* Calendar events for other modules' agendas (the wall's "now and next"); the plan's own calendar
   reads the agenda without "plan", so they never show twice there. */
void plan_agenda(Ctx *c, int64_t from, int64_t to, Agenda *out);
int plan_shop_items(Buf *b, int max);   /* what's still to buy (max shown, "+N more"); returns how many, 0 = nothing drawn */
/* The days after today, up to `days` of them: each day with something on it under its name
   (events; chores due then), at most `max` rows in all. Returns the rows drawn. */
int plan_days(Ctx *c, Buf *b, int days, int max);
int plan_open_tasks(int who); /* who: 0 = everyone */
int plan_open_items(void);
void plan_subtitle(Ctx *c, Buf *b); /* "4 to-dos · 3 on the list" (a child's own to-dos; a guest's: the list) */

/* actions */
void plan_act_task_preview(Ctx *c, Fields *f);
void plan_act_task_add(Ctx *c, Fields *f);
void plan_act_task_done(Ctx *c, Fields *f);
void plan_act_task_save(Ctx *c, Fields *f);
void plan_act_task_delete(Ctx *c, Fields *f);
void plan_act_task_swap(Ctx *c, Fields *f);
void plan_act_shop_preview(Ctx *c, Fields *f);
void plan_act_shop_add(Ctx *c, Fields *f);
void plan_act_shop_toggle(Ctx *c, Fields *f);
void plan_act_shop_delete(Ctx *c, Fields *f);
void plan_act_shop_clear(Ctx *c, Fields *f);
void plan_act_event_preview(Ctx *c, Fields *f);
void plan_act_event_add(Ctx *c, Fields *f);
void plan_act_event_save(Ctx *c, Fields *f);
void plan_act_event_delete(Ctx *c, Fields *f);

#endif
