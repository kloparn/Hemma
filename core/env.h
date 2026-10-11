/* core/env.h — configuration from the environment and an optional .env file.
 *
 *   # comment
 *   export HEMMA_PORT=8080
 *   HEMMA_ADMIN_NAME="Alex H"      # quotes allow spaces and # ; \n \" \\ escapes in "..."
 *   HEMMA_NOTE='literal $tuff'
 *
 * Variables already set in the real environment always win over the file. */
#ifndef HEMMA_ENV_H
#define HEMMA_ENV_H

#include "base.h"

/* Calls cb for every KEY=VALUE in text; returns the number of pairs, or -(line) of the first bad line. */
int env_parse(const char *text, void (*cb)(const char *key, const char *val, void *ctx), void *ctx);
/* Loads a .env file into the process environment (without overriding). -1 if the file can't be read. */
int env_load(const char *path);
const char *env(const char *key, const char *dflt);
long long env_int(const char *key, long long dflt);

#endif
