/* core/codec.h — small encoders: SHA-1 (WebSocket handshake), Base64,
 * URL percent-encoding. */
#ifndef HEMMA_CODEC_H
#define HEMMA_CODEC_H

#include "base.h"

typedef struct {
    uint32_t h[5];
    uint64_t total;
    uint8_t block[64];
    size_t fill;
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, size_t n);
void sha1_final(Sha1 *s, uint8_t out[20]);
void sha1(const void *data, size_t n, uint8_t out[20]);

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    size_t fill;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
void sha256_hex(const void *data, size_t n, char out[65]);
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
/* PBKDF2-HMAC-SHA256 (RFC 8018) */
void pbkdf2_sha256(const void *pass, size_t plen, const void *salt, size_t slen, uint32_t iterations, uint8_t *out,
                   size_t outlen);

/* out needs 4 * ((n + 2) / 3) + 1 bytes; returns the encoded length */
size_t base64_encode(const void *data, size_t n, char *out);
/* Decodes exactly 2*n hex chars; false on bad input. */
bool hex_decode(const char *hex, uint8_t *out, size_t n);
/* Compares without leaking where the first difference is. */
bool ct_equal(const void *a, const void *b, size_t n);

/* Percent-decode s[0..n) into out (may equal s). '+' becomes a space when
 * plus_space. Invalid escapes are copied literally. Returns the new length
 * and NUL-terminates. */
size_t url_decode(const char *s, size_t n, char *out, bool plus_space);
/* Append s percent-encoded (everything but A-Z a-z 0-9 - _ . ~ ; '/' kept when keep_slash). */
void url_encode(Buf *b, const char *s, bool keep_slash);

#endif
