/* modules/media/health.h — the library's part of the System page (the core's health hook).
 * Module-internal (health.c); media.c wires the hook, the tick and the resets. */
#ifndef HEMMA_MEDIA_HEALTH_H
#define HEMMA_MEDIA_HEALTH_H

#include "library.h"

void media_health(Ctx *c, Health *h); /* Module.health: folders, room, the converter, a getting-started step */
void media_health_tick(int64_t now_ms); /* refreshes the cached facts while someone looks */
void media_health_reset(void);          /* the folders changed: look again */

#endif
