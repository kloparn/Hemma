/* core/version.c — the version with the commit it was built from (see HEMMA_VERSION in app.h). The
 * build passes -DHEMMA_COMMIT=<sha>[-dirty] to this file alone. */
#include "app.h"

#define HEMMA_STR_(x) #x
#define HEMMA_STR(x) HEMMA_STR_(x)

const char *hemma_version_full(void) {
#ifdef HEMMA_COMMIT
    return HEMMA_VERSION " (" HEMMA_STR(HEMMA_COMMIT) ")";
#else
    return HEMMA_VERSION;
#endif
}
