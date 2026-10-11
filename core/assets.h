/* core/assets.h — web/ files baked into the binary by tools/embed.c */
#ifndef HEMMA_ASSETS_H
#define HEMMA_ASSETS_H

#include <stddef.h>

typedef struct {
    const char *path; /* "/app.css" */
    const char *type; /* content type */
    const unsigned char *data;
    size_t len;
    const char *etag; /* 16 hex chars, changes when the content changes */
} Asset;

extern const Asset g_assets[];
extern const int g_nassets;

#endif
