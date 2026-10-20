/* core/release.c — signed releases (issue #73): see release.h. */
#include "release.h"
#include "codec.h"
#include "ed25519.h"
#include "plat.h"

/* The public keys Hemma takes releases from (hex). A new key goes in BEFORE the old one stops
   signing: the release that brings it is still signed with the old one, which every Hemma out there
   knows. A lost secret = the households install the next release by hand (docs/development.md). */
static const char *const RELEASE_KEYS[] = {
    "4214322893363cf426f747cb7e425f6c12b481a52ecaca29b9088fb47cf9d4f7", /* the project's, since 0.21.1 */
};

static const uint8_t *g_test_key;
static uint8_t g_test_key_buf[32];

void release_test_key(const uint8_t *pub) {
    if (pub) memcpy(g_test_key_buf, pub, 32);
    g_test_key = pub ? g_test_key_buf : NULL;
}

bool release_key_known(const uint8_t pub[32]) {
    if (g_test_key) return memcmp(pub, g_test_key, 32) == 0;
    for (size_t i = 0; i < sizeof RELEASE_KEYS / sizeof RELEASE_KEYS[0]; i++) {
        uint8_t k[32];
        if (RELEASE_KEYS[i] && hex_decode(RELEASE_KEYS[i], k, 32) && memcmp(pub, k, 32) == 0) return true;
    }
    return false;
}

static bool fail(char *why, size_t cap, const char *fmt, const char *arg) {
    snprintf(why, cap, fmt, arg ? arg : "");
    return false;
}

bool release_key_new(const char *file, char pub_hex[65], char *why, size_t cap) {
    if (path_stat(file, NULL, NULL, NULL))
        return fail(why, cap, "%s is there already: a signing key is never written over another", file);
    uint8_t seed[32], pub[32];
    rand_bytes(seed, sizeof seed);
    char hex[66];
    hex_encode(seed, 32, hex);
    strcat(hex, "\n");
    bool ok = file_write_private(file, hex, strlen(hex));
    ed25519_public(seed, pub);
    memset(seed, 0, sizeof seed);
    memset(hex, 0, sizeof hex);
    if (!ok) return fail(why, cap, "couldn't write %s", file);
    hex_encode(pub, 32, pub_hex);
    return true;
}

/* the first 2n characters of s as hex digits (n <= 64) */
static bool hex_at(const char *s, uint8_t *out, size_t n) {
    char token[130];
    if (2 * n >= sizeof token || strnlen(s, 2 * n) < 2 * n) return false;
    memcpy(token, s, 2 * n);
    token[2 * n] = 0;
    return hex_decode(token, out, n);
}

/* 2n hex digits, with only white space around them */
static bool read_hex(const char *s, uint8_t *out, size_t n) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = 0;
    while (s[len] && s[len] != ' ' && s[len] != '\t' && s[len] != '\r' && s[len] != '\n') len++;
    return len == 2 * n && hex_at(s, out, n) && str_blank(s + len);
}

bool release_sign(const char *secret, const char *file, char sig_hex[129], char *why, size_t cap) {
    uint8_t seed[32], pub[32], sig[64];
    if (!secret || !read_hex(secret, seed, 32)) return fail(why, cap, "the signing key isn't 64 hex digits%s", NULL);
    ed25519_public(seed, pub);
    if (!release_key_known(pub)) {
        memset(seed, 0, sizeof seed);
        char hex[65];
        hex_encode(pub, 32, hex);
        return fail(why, cap,
                    "this key (public half %s) isn't one Hemma has built in (core/release.c RELEASE_KEYS): "
                    "installed Hemmas would refuse what it signs",
                    hex);
    }
    Buf data = {0};
    if (!file_read_all(file, &data, 1 << 20)) {
        memset(seed, 0, sizeof seed);
        buf_free(&data);
        return fail(why, cap, "couldn't read %s (or it's over 1 MB)", file);
    }
    ed25519_sign(seed, data.p, data.len, sig);
    memset(seed, 0, sizeof seed);
    buf_free(&data);
    hex_encode(sig, 64, sig_hex);
    return true;
}

/* ------------------------------------------------------------- verifying */

#define SUMS_MAX 64

typedef struct {
    char name[200];
    uint8_t hash[32];
    bool seen;
} Sum;

/* A name a release's SHA256SUMS may list: a plain file in the same folder. */
static bool plain_name(const char *s) {
    if (!*s || *s == '.' || strlen(s) >= sizeof(((Sum *)0)->name)) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(c == '.' || c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z')))
            return false;
    }
    return !str_eq(s, RELEASE_SUMS) && !str_eq(s, RELEASE_SIG);
}

/* sha256sum's lines, "<64 hex>  <name>" (or " *<name>": binary mode) - nothing else */
static int parse_sums(const char *text, Sum *out, int max, char *why, size_t cap) {
    int n = 0;
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char line[300];
        if (len >= sizeof line) return fail(why, cap, "a line in SHA256SUMS is too long%s", NULL), -1;
        memcpy(line, p, len);
        line[len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        p = end ? end + 1 : p + len;
        if (!len) continue;
        if (n == max) return fail(why, cap, "SHA256SUMS lists too many files%s", NULL), -1;
        if (len < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*') || !hex_at(line, out[n].hash, 32))
            return fail(why, cap, "SHA256SUMS has a line that isn't \"<SHA-256>  <file>\": %.80s", line), -1;
        if (!plain_name(line + 66))
            return fail(why, cap, "SHA256SUMS names something that isn't a plain file here: %.80s", line + 66), -1;
        for (int i = 0; i < n; i++)
            if (str_eq(out[i].name, line + 66)) return fail(why, cap, "SHA256SUMS lists %s twice", line + 66), -1;
        str_copy(out[n].name, sizeof out[n].name, line + 66);
        out[n].seen = false;
        n++;
    }
    if (!n) return fail(why, cap, "SHA256SUMS lists nothing%s", NULL), -1;
    return n;
}

static bool hash_file(const char *path, uint8_t out[32]) {
    FILE *f = file_open(path, "rb");
    if (!f) return false;
    Sha256 s;
    sha256_init(&s);
    uint8_t chunk[16384];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) sha256_update(&s, chunk, got);
    bool ok = !ferror(f);
    fclose(f);
    sha256_final(&s, out);
    return ok;
}

static bool verify(const char *dir, bool partial, char *why, size_t cap, int *files) {
    Buf sums = {0}, sigtext = {0};
    char path[1024];
    bool ok = false;
    Sum *list = xcalloc(SUMS_MAX, sizeof *list);
    snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SUMS);
    if (!file_read_all(path, &sums, 64 * 1024)) {
        fail(why, cap, "no SHA256SUMS in %s", dir);
        goto done;
    }
    snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SIG);
    if (!file_read_all(path, &sigtext, 1024)) {
        fail(why, cap, "no SHA256SUMS.sig in %s: releases are signed since Hemma 0.21.1", dir);
        goto done;
    }
    uint8_t sig[64];
    if (!read_hex(buf_cstr(&sigtext), sig, 64)) {
        fail(why, cap, "SHA256SUMS.sig isn't a signature (128 hex digits)%s", NULL);
        goto done;
    }
    bool signed_ok = false;
    if (g_test_key) signed_ok = ed25519_verify(g_test_key, sums.p, sums.len, sig);
    bool any_key = g_test_key != NULL;
    for (size_t i = 0; !signed_ok && !g_test_key && i < sizeof RELEASE_KEYS / sizeof RELEASE_KEYS[0]; i++) {
        uint8_t k[32];
        if (!RELEASE_KEYS[i] || !hex_decode(RELEASE_KEYS[i], k, 32)) continue;
        any_key = true;
        signed_ok = ed25519_verify(k, sums.p, sums.len, sig);
    }
    if (!any_key) {
        fail(why, cap, "this Hemma was built without the key releases are signed with%s", NULL);
        goto done;
    }
    if (!signed_ok) {
        fail(why, cap, "the signature doesn't fit: this isn't what the Hemma project released%s", NULL);
        goto done;
    }
    int n = parse_sums(buf_cstr(&sums), list, SUMS_MAX, why, cap);
    if (n < 0) goto done;
    /* every file here must be listed; a link or a folder never is */
    DirIter *it = dir_open(dir);
    if (!it) {
        fail(why, cap, "can't read the folder %s", dir);
        goto done;
    }
    DirEntry e;
    bool stray = false;
    while (!stray && dir_next(it, &e)) {
        if (str_eq(e.name, RELEASE_SUMS) || str_eq(e.name, RELEASE_SIG)) {
            if (e.is_dir || e.is_link) stray = true, fail(why, cap, "%.200s isn't a plain file", e.name);
            continue;
        }
        int k = -1;
        for (int i = 0; i < n && k < 0; i++)
            if (str_eq(list[i].name, e.name)) k = i;
        if (k < 0) stray = true, fail(why, cap, "%.200s is here but not in SHA256SUMS", e.name);
        else if (e.is_dir || e.is_link) stray = true, fail(why, cap, "%.200s isn't a plain file", e.name);
        else list[k].seen = true;
    }
    dir_close(it);
    if (stray) goto done;
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (!list[i].seen) {
            if (partial) continue;
            fail(why, cap, "%s is missing (SHA256SUMS lists it)", list[i].name);
            goto done;
        }
        uint8_t h[32];
        snprintf(path, sizeof path, "%s/%s", dir, list[i].name);
        if (!hash_file(path, h)) {
            fail(why, cap, "can't read %s", list[i].name);
            goto done;
        }
        if (memcmp(h, list[i].hash, 32) != 0) {
            fail(why, cap, "%s isn't the file that was released (its SHA-256 differs)", list[i].name);
            goto done;
        }
        seen++;
    }
    if (!seen) {
        fail(why, cap, "none of the files SHA256SUMS lists is here%s", NULL);
        goto done;
    }
    *files = seen;
    ok = true;
done:
    free(list);
    buf_free(&sums);
    buf_free(&sigtext);
    return ok;
}

bool release_verify_dir(const char *dir, bool partial, Buf *say) {
    char why[400] = "";
    int files = 0;
    bool ok = verify(dir, partial, why, sizeof why, &files);
    if (ok) buf_printf(say, "signed by the Hemma project: %d file%s checked", files, files == 1 ? "" : "s");
    else buf_puts(say, why);
    return ok;
}
