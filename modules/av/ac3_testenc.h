/* modules/av/ac3_testenc.h — for tests only: AC-3 made by the small encoder in
 * ac3_test.c, so other tests (conv_test.c) can feed real Dolby frames through. */
#ifndef HEMMA_AV_AC3_TESTENC_H
#define HEMMA_AV_AC3_TESTENC_H

#include <stddef.h>
#include <stdint.h>

/* `frames` AC-3 frames (at most 64) of a stereo tone - 440 Hz left, 660 Hz right - at 48 kHz
   and 192 kbit/s, one after another in out (768 bytes each). Returns the bytes written. */
size_t ac3_test_tone(uint8_t *out, size_t cap, int frames);

#endif
