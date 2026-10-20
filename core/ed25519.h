/* core/ed25519.h — Ed25519 signatures (RFC 8032): signed releases (issue #73). A 32-byte secret seed
 * makes the key pair; a signature is 64 bytes. */
#ifndef HEMMA_ED25519_H
#define HEMMA_ED25519_H

#include "base.h"

void ed25519_public(const uint8_t seed[32], uint8_t pub[32]);
void ed25519_sign(const uint8_t seed[32], const void *msg, size_t n, uint8_t sig[64]);
/* true only for a signature made with pub's seed over exactly msg (S below the group order: one signature
   per message, no second form of it) */
bool ed25519_verify(const uint8_t pub[32], const void *msg, size_t n, const uint8_t sig[64]);

#endif
