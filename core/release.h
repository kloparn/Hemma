/* core/release.h — signed releases (issue #73). A release's downloads, and each download's own folder,
 * carry SHA256SUMS (sha256sum's lines: "<64 hex>  <name>") and SHA256SUMS.sig: an Ed25519 signature
 * over SHA256SUMS, 128 hex digits. The public keys that may sign are built into Hemma (release.c
 * RELEASE_KEYS), so an installed Hemma only takes what the project released: hemma-upgrade
 * (deploy/) runs `hemma --verify-release DIR` with the INSTALLED program before anything is put in
 * place. The secret half lives only in the project's CI (GitHub secret HEMMA_SIGNING_KEY) and with
 * its maintainer: `hemma --gen-signing-key FILE` makes one, `hemma --sign FILE` signs with it. */
#ifndef HEMMA_RELEASE_H
#define HEMMA_RELEASE_H

#include "base.h"

#define RELEASE_SUMS "SHA256SUMS"
#define RELEASE_SIG "SHA256SUMS.sig"

/* A new key pair: the secret (64 hex digits + a line end) into a new file only its owner reads -
   never over an existing one - and the public half in pub_hex. */
bool release_key_new(const char *file, char pub_hex[65], char *why, size_t cap);
/* Signs file's bytes with secret (64 hex digits; spaces and line ends around them are fine): sig_hex
   = 128 hex digits. Refused when secret's public half isn't one Hemma has built in - a release
   signed so would be refused by every installed Hemma. */
bool release_sign(const char *secret, const char *file, char sig_hex[129], char *why, size_t cap);
/* The folder dir as a release made it: SHA256SUMS.sig is a built-in key's signature over
   SHA256SUMS, every file listed there is in dir (a plain file, not a link) with that SHA-256, and
   dir holds nothing else. partial: files listed but not in dir are fine (a release's own
   SHA256SUMS beside the one download taken from it) - at least one must be. say: what was found
   (one line), also when it fails. */
bool release_verify_dir(const char *dir, bool partial, Buf *say);
/* Whether a public key is one Hemma takes releases from (false for all while none is built in). */
bool release_key_known(const uint8_t pub[32]);
/* tests: take releases signed by pub instead of the built-in keys (NULL = those again) */
void release_test_key(const uint8_t *pub);

#endif
