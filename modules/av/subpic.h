/* modules/av/subpic.h — picture subtitles: Blu-ray PGS (a .sup file) and DVD VobSub (an .idx text +
 * its .sub file), the two kinds browsers can't show. They're indexed into cues (when, where) and each
 * cue is drawn on demand as an indexed-colour picture, which png_indexed() turns into a PNG for the
 * player to lay over the video.
 *
 * Opening reads headers only - a .sup's segment headers (and the small composition, window and object
 * headers), a .sub's pack/PES headers and each subpicture's control part (its times, area, colours) -
 * and remembers where the rest lies; subpic_render() reads one cue's bytes. Everything is bounded: at
 * most SUBPIC_MAX_CUES cues, pictures at most SUBPIC_MAX_SIDE a side, and every size and offset is
 * checked before it's used: a damaged or hostile file is skipped where it's broken, never trusted.
 *
 * Pure library: libc only (no core/), no mutable globals. Input is random access through MkvReader
 * (mkv.h); the reader's ctx must stay valid until subpic_close(). One SubPic per thread at a time. */
#ifndef HEMMA_AV_SUBPIC_H
#define HEMMA_AV_SUBPIC_H

#include "mkv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUBPIC_MAX_CUES 20000
#define SUBPIC_MAX_SIDE 4096

/* A cue: shown from start_ms to end_ms (the copy's clock), its picture's place and size on the frame of
   subpic_frame(). Cues come in order of start. */
typedef struct {
    int64_t start_ms, end_ms;
    int x, y, w, h;
} SubPicCue;

typedef struct SubPic SubPic;

/* A .sup (PGS: "PG" + PTS + DTS + segment, per segment). NULL + err when it holds no PGS segment at all
   (an empty but well-formed file opens with 0 cues). */
SubPic *subpic_open_sup(const MkvReader *sup, char *err, size_t cap);
/* VobSub: the .idx text (size:, palette:, custom colors:, the FIRST id: track's delay:/timestamp: lines)
   and its .sub (MPEG-PS: private stream 1 packets, substreams 0x20-0x3F). NULL + err when the idx
   says nothing VobSub. */
SubPic *subpic_open_vobsub(const char *idx, size_t idx_len, const MkvReader *sub, char *err, size_t cap);
void subpic_close(SubPic *s);
int subpic_count(const SubPic *s);
const SubPicCue *subpic_cue(const SubPic *s, int i); /* NULL when i is out of range */
/* The video frame cue positions refer to (1920x1080, 720x480 ...) */
void subpic_frame(const SubPic *s, int *w, int *h);
/* Cue i's picture (its w x h) as indexed colour: w*h bytes into px (cap), ncolors RGBA entries into pal
   (0xRRGGBBAA, straight alpha; every value in px is below ncolors). False when it can't be drawn (cap
   too small, the file can't be read there). Data cut short draws what's there, the rest transparent. */
bool subpic_render(SubPic *s, int i, uint8_t *px, size_t cap, uint32_t pal[256], int *ncolors);

/* An indexed-colour PNG of px (w x h, 1..16384 a side; tRNS for alpha), malloc'd, its size in *len.
   pal is 0xRRGGBBAA; values in px past ncolors get transparent black. NULL when out of memory or the
   size is out of range. */
uint8_t *png_indexed(const uint8_t *px, int w, int h, const uint32_t *pal, int ncolors, size_t *len);

#endif
