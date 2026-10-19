/* core/version.c — Hemma's version (the VERSION file at the top of the repo: MAJOR.MINOR.PATCH, see
 * semver.org) and the commit it was built from. The build passes -DHEMMA_VER=<VERSION> and
 * -DHEMMA_COMMIT=<sha>[-dirty] to this file alone, so a new version or commit recompiles only it. */
#include "app.h"

#define HEMMA_STR_(x) #x
#define HEMMA_STR(x) HEMMA_STR_(x)

#ifndef HEMMA_VER
#error "build with -DHEMMA_VER=<the VERSION file> (build.ps1 and the Makefile do)"
#endif

const char *hemma_version(void) { return HEMMA_STR(HEMMA_VER); }

const char *hemma_version_full(void) {
#ifdef HEMMA_COMMIT
    return HEMMA_STR(HEMMA_VER) " (" HEMMA_STR(HEMMA_COMMIT) ")";
#else
    return HEMMA_STR(HEMMA_VER);
#endif
}
