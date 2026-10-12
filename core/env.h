/* core/env.h — configuration from the environment and an optional .env file.
 *
 *   # comment
 *   export HEMMA_PORT=8080
 *   HEMMA_ADMIN_NAME="Alex H"      # quotes allow spaces and # ; \n \" \\ escapes in "..."
 *   HEMMA_NOTE='literal $tuff'
 *
 * Variables already set (non-empty) in the real environment always win over the file. Within
 * the file the first line with a value counts: an empty "KEY=" never hides a later "KEY=x". A
 * line that isn't KEY=VALUE is skipped (and logged); the lines after it still count. */
#ifndef HEMMA_ENV_H
#define HEMMA_ENV_H

#include "base.h"

/* Calls cb for every KEY=VALUE in text (line = its 1-based line number) and returns how many
   there were. Bad lines are skipped; *first_bad (may be NULL) = the first one's number, or 0. */
int env_parse(const char *text, void (*cb)(const char *key, const char *val, int line, void *ctx), void *ctx,
              int *first_bad);
/* The key a settings-file line sets (BOM, "export " and spaces allowed): false for blank lines,
   comments and lines that aren't KEY=VALUE. For code that rewrites the file. */
bool env_line_key(const char *line, char *key, size_t cap);
/* Loads a .env file into the process environment (without overriding). -1 if the file can't be read. */
int env_load(const char *path);
const char *env(const char *key, const char *dflt);
long long env_int(const char *key, long long dflt);

#endif
