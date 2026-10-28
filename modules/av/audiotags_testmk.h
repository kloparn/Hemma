/* modules/av/audiotags_testmk.h — for tests only: audiobooks made in memory, an MP4 (.m4b) and an MP3, with the
 * tags, chapters and cover modules/av/audiotags.c reads - so its tests (audiotags_test.c, where these are
 * defined) and others (a browser test's audiobook) need no binary files in the repository.
 *
 * at_test_m4b with real AAC makes a file a browser plays: give it access units from modules/av/aac.c and the
 * encoder's AudioSpecificConfig, e.g.
 *
 *     AacConfig ac = {44100, 2, 64000, 0, 0};
 *     AacEnc *e = aac_open(&ac, err, sizeof err);
 *     uint8_t asc[2];
 *     aac_specific_config(e, asc);
 *     Buf au = {0};  int lens[N];  ... for each 1024 samples: int n = aac_encode(e, pcm, tmp, sizeof tmp);
 *                                      if (n > 0) buf_put(&au, tmp, n), lens[k++] = n;  ... then aac_flush
 *     AtTestM4b m = {.title = "Book", .aac = (const uint8_t *)au.p, .aac_len = lens, .aac_frames = k,
 *                    .asc = asc, .asc_len = 2, .sample_rate = 44100, .channels = 2, .priming = AAC_PRIMING,
 *                    .chapters = ch, .nchapters = 3};
 *     Buf file = {0};  at_test_m4b(&file, &m);
 *
 * Without frames it writes a few made-up 6-byte samples that claim duration_ms between them: right for a reader,
 * not for a player. */
#ifndef HEMMA_AV_AUDIOTAGS_TESTMK_H
#define HEMMA_AV_AUDIOTAGS_TESTMK_H

#include "../../core/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t start_ms;
    const char *title; /* UTF-8 */
} AtTestChapter;

typedef struct {
    /* tags in moov/udta/meta/ilst; NULL or 0 = left out. Strings are UTF-8, written as they are. */
    const char *title, *artist, *album_artist, *album, *narrator, *genre; /* ©nam ©ART aART ©alb ©nrt ©gen */
    const char *desc, *ldes, *des, *cmt;                                  /* desc, ldes, ©des, ©cmt */
    int year;                                                             /* ©day "2025" */
    int track, tracks, disc, discs;                                       /* trkn, disk */
    int gnre;                                                             /* gnre: an ID3v1 genre + 1 */
    const uint8_t *cover;                                                 /* covr: a JPEG or PNG (type 13 or 14) */
    size_t cover_len;
    bool meta_qt;   /* 'meta' without version and flags, as QuickTime writes it */
    bool moov_last; /* ftyp, mdat, moov (as most audiobooks are made); else ftyp, moov, mdat */
    /* chapters: a QuickTime text track the sound's tref 'chap' names, and/or Nero's chpl in moov/udta */
    const AtTestChapter *chapters;
    int nchapters;
    bool qt, chpl; /* both false = a QuickTime track (give its chapters in time order: their gaps are its
                      durations); a first chapter after 0 gets an empty edit before it. chpl takes any order. */
    /* the sound: AAC-LC in mp4a + esds */
    int sample_rate, channels; /* 0 = 44100, 2 */
    const uint8_t *asc;        /* AudioSpecificConfig (aac_specific_config); NULL = made from rate and channels */
    size_t asc_len;
    const uint8_t *aac;        /* real access units, back to back, 1024 samples each */
    const int *aac_len;        /* each one's bytes */
    int aac_frames;
    int priming;          /* samples an edit list skips at the start (AAC_PRIMING for aac.c's output); 0 = no edit */
    int64_t duration_ms;  /* without frames: what the made-up samples claim (0 = 60 s) */
    int fake_frames;      /* without frames: how many (0 = 8) */
} AtTestM4b;

typedef struct {
    int id3;            /* an ID3v2.2, 2.3 or 2.4 tag first; 0 = none */
    bool unsync;        /* unsynchronised: v2.2/2.3 the whole tag, v2.4 each frame (with a data length) */
    bool ext_header;    /* v2.3/2.4: an extended header */
    bool plain_sizes;   /* v2.4 frame sizes as plain numbers, as iTunes once wrote them */
    int enc;            /* its text: 0 Latin-1, 1 UTF-16 with a BOM (little-endian), 2 UTF-16BE, 3 UTF-8 */
    bool raw_utf8;      /* Latin-1 text (enc 0, ID3v1) written as UTF-8 bytes, as FFmpeg's ID3v1 writer does */
    const char *title, *artist, *album_artist, *album, *genre; /* TIT2 TPE1 TPE2 TALB TCON (as given: "(183)") */
    const char *comment, *narrator; /* COMM (after an iTunNORM one), TXXX "NARRATOR" */
    int year, track, tracks, disc, discs; /* TYER (v2.4 TDRC "2025-01-02"), TRCK, TPOS */
    const uint8_t *cover;    /* APIC (v2.2 PIC), a front cover, after a back cover of 4 bytes when back_first */
    size_t cover_len;
    bool back_first;
    const AtTestChapter *chapters; /* CHAP frames as given (v2.3/2.4), each with a TIT2; a CTOC first */
    int nchapters;
    int padding;     /* zeros after the frames */
    bool id3v1;      /* an ID3v1.1 tag at the end: title, artist, album, year, comment, track, genre 183 */
    int frames;      /* MPEG-1 Layer III frames, all zeros after their headers */
    int kbps, rate;  /* 0 = 128, 44100 (MPEG-1 rates: 32000, 44100, 48000) */
    bool mono;
    int vbr;               /* the first frame: 0 audio, 1 a Xing header, 2 an Info header, 3 a VBRI header */
    uint32_t vbr_frames;   /* the frame count it states; 0 = frames - 1 */
    int lame_delay, lame_padding; /* with a Xing/Info header: a LAME tag saying these (samples; 0 and 0 = no LAME tag) */
} AtTestMp3;

/* Appends the file to out (zero it first). */
void at_test_m4b(Buf *out, const AtTestM4b *m);
void at_test_mp3(Buf *out, const AtTestMp3 *m);

#endif
