/* core/tar.h — the little of the tar format (POSIX ustar) Hemma needs: the backup download
 * (the database and the password secret in one file anyone can open - tar comes with Windows,
 * macOS and Linux) and reading one back (hemma --restore). Plain files only, in memory. */
#ifndef HEMMA_TAR_H
#define HEMMA_TAR_H

#include "base.h"

/* Appends one file: its 512-byte header, the bytes, padding to 512. Names up to 99 bytes. */
void tar_add(Buf *out, const char *name, const void *data, size_t n, int64_t mtime, int mode);
void tar_end(Buf *out); /* the two empty blocks that close an archive */
/* Starts with a ustar header whose checksum adds up? */
bool tar_looks(const void *data, size_t n);
/* The file called `name` in the archive: its bytes (pointing into data). False when there's none,
   or the archive is broken before it. */
bool tar_find(const void *data, size_t n, const char *name, const void **p, size_t *len);

#endif
