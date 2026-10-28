/* modules/av/audiotags.h — what an audio file says about itself, without decoding a sound: an audiobook's
 * title, author, narrator, chapters, cover and length (1.6).
 *
 *  - MP4 / M4B / M4A (ISO BMFF): the boxes are walked with read_at, reading only box headers and the small
 *    boxes it needs - an audiobook's moov can be 18 MB, nearly all of it the sound's sample tables, and it may
 *    come before or after the mdat. mvhd (the length), the sound track's mdhd + stsd (codec, channels, rate),
 *    udta/meta/ilst (©nam ©ART aART ©alb ©nrt ©day ©gen trkn disk desc/©des/ldes/©cmt covr - the cover's place
 *    is found, its bytes aren't read), chapters from a QuickTime chapter track (the sound track's tref 'chap'
 *    names a text track: its samples' times, from stts, and texts, from stsz/stsc/stco or co64 - each a 16-bit
 *    length and UTF-8 or UTF-16 text) or else Nero's 'chpl' (in moov/udta).
 *  - MP3: an ID3v2 tag at the start (v2.2, v2.3, v2.4; unsynchronisation; extended header skipped): TIT2 TPE1
 *    TPE2 TALB TYER/TDRC TRCK TPOS TCON COMM APIC (located) and CHAP (chapters: their start times, and a TIT2 in
 *    each); an ID3v1 tag at the end when there's no v2. The length from the first frame's Xing/Info header
 *    (its frame count) or VBRI header, else - constant bitrate - the bytes between the tags over its bitrate.
 *
 * Text comes out as valid UTF-8 (ID3's Latin-1 and UTF-16 converted; anything invalid replaced), control
 * characters gone, cut at a whole character to fit.
 *
 * Pure library: libc only (no core/), no globals. Corrupt input never crashes or loops: every count, size and
 * offset is bounded and checked against the file's size. */
#ifndef HEMMA_AV_AUDIOTAGS_H
#define HEMMA_AV_AUDIOTAGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    int64_t size; /* total bytes */
    /* Copy exactly n bytes at offset off into dst; false on error or short read. */
    bool (*read_at)(void *ctx, int64_t off, void *dst, size_t n);
} AtReader;

#define AT_MAX_CHAPTERS 2000 /* more are left out */
#define AT_TEXT 256          /* a tag's text, bytes with the NUL */
#define AT_CHAPTER_TEXT 160  /* a chapter's title */
#define AT_DESC_MAX 4096     /* the description, bytes with the NUL */

typedef struct {
    int64_t start_ms; /* from the start of the file */
    char title[AT_CHAPTER_TEXT];
} AtChapter;

typedef struct {
    char format[8]; /* "mp4" or "mp3"; "" = neither (audio_tags_read said false) */
    char codec[12]; /* the sound: "aac" "mp3" "alac" "ac3" "eac3" "opus" "flac" ... (as media/probe.c names them) */
    int channels, sample_rate;
    int64_t duration_ms; /* 0 = unknown */
    char title[AT_TEXT], artist[AT_TEXT], album_artist[AT_TEXT], album[AT_TEXT], narrator[AT_TEXT];
    char genre[96];
    int year;          /* 0 = none */
    int track, tracks; /* trkn / TRCK "3/12"; 0 = none */
    int disc, discs;   /* disk / TPOS */
    char *description; /* malloc'd, at most AT_DESC_MAX bytes with its NUL; NULL = none (ldes > desc > ©des > ©cmt; COMM) */
    int64_t cover_off, cover_len; /* the cover picture's bytes in the file (cover_len 0 = none) */
    char cover_type[16];          /* "image/jpeg" or "image/png" (as its bytes start), "" = none */
    AtChapter *chapters;          /* malloc'd, by start; NULL = none */
    int nchapters;
} AudioTags;

/* Reads what it can (out is always set, the rest zero); true when the file is an MP4 or an MP3 it recognised.
   name_hint: the file's name ("x.mp3" helps tell an MP3 without an ID3 tag); NULL = none. Free with
   audio_tags_free (also after false). */
bool audio_tags_read(const AtReader *r, const char *name_hint, AudioTags *out);
void audio_tags_free(AudioTags *t);

#endif
