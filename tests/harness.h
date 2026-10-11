/* tests/harness.h — test modules without a server.
 *
 *   TEST(book_rejects_overlaps) {
 *       th_boot();                                   // fresh database, every module loaded
 *       int adam = th_person("Adam");
 *       ThResult r = th_act(adam, "book.create", "res=1&date=2026-10-12&from=14:00&to=16:00");
 *       CHECK(r.ok);
 *       CHECK_HAS(th_page(adam, "/book?w=2026-10-12"), "14:00");
 *       th_halt();
 *   }
 *
 * app_testing() is true inside, so modules skip sockets, disk scans and threads. */
#ifndef HEMMA_HARNESS_H
#define HEMMA_HARNESS_H

#include "../core/app.h"
#include "test.h"

typedef struct {
    bool ok; /* the action exists and didn't fail */
    char toast[240];
    char redirect[600];
} ThResult;

void th_boot(void);
void th_halt(void);
int th_person(const char *name);
const char *th_page(int person, const char *url); /* the rendered #app; valid until the next call */
ThResult th_act(int person, const char *action, const char *form); /* form: "a=1&b=two" url-encoded */

#endif
