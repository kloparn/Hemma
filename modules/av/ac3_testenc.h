/* modules/av/ac3_testenc.h — for tests only: AC-3 made by the small encoder in
 * ac3_test.c, so other tests (conv_test.c) can feed real Dolby frames through. */
#ifndef HEMMA_AV_AC3_TESTENC_H
#define HEMMA_AV_AC3_TESTENC_H

#include <stddef.h>
#include <stdint.h>

/* `frames` AC-3 frames (at most 64) of a stereo tone - 440 Hz left, 660 Hz right - at 48 kHz
   and 192 kbit/s, one after another in out (768 bytes each). Returns the bytes written. */
size_t ac3_test_tone(uint8_t *out, size_t cap, int frames);

/* The same at 44100 or 32000 Hz (frames of 834 or 1152 bytes). */
size_t ac3_test_tone_at(uint8_t *out, size_t cap, int frames, int rate);

/* The same tone as E-AC-3 (Dolby Digital Plus): `frames` frames of 3 blocks (768 samples, 384
   bytes at 192 kbit/s; at most 128). Returns the bytes written. */
size_t eac3_test_tone(uint8_t *out, size_t cap, int frames);

#endif
