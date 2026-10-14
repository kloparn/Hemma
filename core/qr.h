/* core/qr.h — QR codes (ISO/IEC 18004): byte mode, versions 1-10 - room for an address
 * (213 bytes at level M), drawn as SVG. For phones and tablets that can scan Hemma's address. */
#ifndef HEMMA_QR_H
#define HEMMA_QR_H

#include "base.h"

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (QR_MAX_VERSION * 4 + 17)

enum { QR_L, QR_M, QR_Q, QR_H }; /* error correction: ~7, 15, 25, 30 % of the code may be lost */

typedef struct {
    int version, size, mask; /* size = modules per side (21 for version 1) */
    uint8_t m[QR_MAX_SIZE][QR_MAX_SIZE]; /* [row][column], 1 = dark */
} QrCode;

/* The smallest version that holds `data` at level `ecl`; mask -1 picks the best of the eight.
   false: too long for version 10. */
bool qr_make(QrCode *q, const void *data, size_t len, int ecl, int mask);
/* <svg class="qr"> with the four-module quiet zone around it; `label` = its aria-label. */
void qr_svg(Buf *b, const QrCode *q, const char *label);

#endif
