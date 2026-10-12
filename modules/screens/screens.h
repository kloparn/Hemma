/* modules/screens/screens.h — shared by screens.c (data, actions, who's watching) and
 * screens_page.c (the screens themselves and the builder). */
#ifndef HEMMA_SCREENS_H
#define HEMMA_SCREENS_H

#include "../../core/app.h"

#define SCREENS_MAX 50          /* screens in one household */
#define SCREENS_MAX_ROWS 30     /* rows on one screen */
#define SCREENS_MAX_CHOICES 300 /* what one block may offer to pick from (series in a library...) */
/* "Who's watching?" comes back on a TV that went this long without being used */
#define SCREENS_IDLE_S (4 * 3600)

typedef struct {
    int id;
    char name[48];
    int kind; /* SCREEN_TV or SCREEN_WALL (core/app.h) */
    int64_t created;
    int created_by;
} Screen;

typedef struct {
    int id;
    int screen;
    int pos;        /* order on the screen, ascending */
    char block[48]; /* a Block id, "media.continue" - possibly one no module offers any more */
    char arg[64];   /* the BlockChoice picked; "" for blocks without choices */
    char title[96]; /* the row's heading; "" = the block's own title */
} ScreenRow;

extern Table T_screen, T_screen_row;

Screen *screen_get(int id);
/* A screen's rows in order. Pointers stay valid until the next put into T_screen_row. */
int screen_rows(int screen, ScreenRow **out, int max);
/* What the block offers to pick from, in a heap array the caller frees (NULL, 0 = nothing). */
int screens_choices(Ctx *c, const Block *bl, BlockChoice **out);
/* ctx_viewer() after the idle rule: 0 = ask "Who's watching?" (a lapsed pick is cleared). */
int screens_viewer(Ctx *c);
/* "/screens/<id>": the screen's address, and what PREF_START holds for a display account showing it. */
void screens_url(int screen, char out[32]);

void screens_page(Ctx *c, Buf *b); /* screens_page.c */

#endif
