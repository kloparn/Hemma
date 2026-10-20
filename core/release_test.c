#include "../tests/test.h"
#include "codec.h"
#include "ed25519.h"
#include "plat.h"
#include "release.h"

static const char *SEED = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

static void put(const char *dir, const char *name, const char *text) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(file_write_atomic(path, text, strlen(text)));
}

static void line(Buf *sums, const char *name, const char *text) {
    char hex[65];
    sha256_hex(text, strlen(text), hex);
    buf_printf(sums, "%s  %s\n", hex, name);
}

/* a bundle folder as tools/ci/bundle.sh makes one, signed with SEED */
static void bundle(const char *dir) {
    Buf sums = {0};
    put(dir, "hemma-pi64", "\x7f" "ELF the program");
    put(dir, "install-pi.sh", "#!/bin/sh\necho installing\n");
    put(dir, "VERSION", "0.21.0\n");
    line(&sums, "VERSION", "0.21.0\n");
    line(&sums, "hemma-pi64", "\x7f" "ELF the program");
    line(&sums, "install-pi.sh", "#!/bin/sh\necho installing\n");
    put(dir, RELEASE_SUMS, buf_cstr(&sums));
    char path[600], sig[129], why[400], text[140];
    snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SUMS);
    CHECK(release_sign(SEED, path, sig, why, sizeof why));
    snprintf(text, sizeof text, "%s\n", sig);
    put(dir, RELEASE_SIG, text);
    buf_free(&sums);
}

static bool verify(const char *dir, bool partial, const char *want) {
    Buf say = {0};
    bool ok = release_verify_dir(dir, partial, &say);
    if (want && !strstr(buf_cstr(&say), want)) test_fail(__FILE__, __LINE__, "\"%s\" lacks \"%s\"", buf_cstr(&say), want);
    buf_free(&say);
    return ok;
}

static void use_test_key(void) {
    uint8_t seed[32], pub[32];
    CHECK(hex_decode(SEED, seed, 32));
    ed25519_public(seed, pub);
    release_test_key(pub);
}

TEST(release_signed_bundle_verifies) {
    use_test_key();
    char dir[600];
    snprintf(dir, sizeof dir, "%s/hemma-pi64", test_tmpdir());
    CHECK(dir_make(dir));
    bundle(dir);
    CHECK(verify(dir, false, "3 files checked"));
    release_test_key(NULL);
}

TEST(release_refuses_changed_extra_or_missing_files) {
    use_test_key();
    char dir[600];
    snprintf(dir, sizeof dir, "%s/b", test_tmpdir());
    CHECK(dir_make(dir));
    bundle(dir);
    put(dir, "install-pi.sh", "#!/bin/sh\nrm -rf /\n"); /* changed after signing */
    CHECK(!verify(dir, false, "install-pi.sh isn't the file that was released"));
    bundle(dir);
    put(dir, "evil.sh", "#!/bin/sh\n"); /* one more */
    CHECK(!verify(dir, false, "evil.sh is here but not in SHA256SUMS"));
    char path[700];
    snprintf(path, sizeof path, "%s/evil.sh", dir);
    CHECK(file_remove(path));
    snprintf(path, sizeof path, "%s/VERSION", dir); /* one less */
    CHECK(file_remove(path));
    CHECK(!verify(dir, false, "VERSION is missing"));
    CHECK(verify(dir, true, "2 files checked")); /* fine beside a release's own SHA256SUMS */
    snprintf(path, sizeof path, "%s/sub", dir); /* a folder isn't a file */
    CHECK(dir_make(path));
    CHECK(!verify(dir, true, "sub is here but not in SHA256SUMS"));
    release_test_key(NULL);
}

TEST(release_refuses_bad_or_foreign_signatures) {
    use_test_key();
    char dir[600];
    snprintf(dir, sizeof dir, "%s/b", test_tmpdir());
    CHECK(dir_make(dir));
    bundle(dir);
    /* SHA256SUMS changed after signing: a line for a file that was swapped */
    char path[700];
    Buf sums = {0};
    snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SUMS);
    CHECK(file_read_all(path, &sums, 4096));
    sums.p[0] = sums.p[0] == 'a' ? 'b' : 'a';
    put(dir, RELEASE_SUMS, buf_cstr(&sums));
    CHECK(!verify(dir, false, "isn't what the Hemma project released"));
    buf_free(&sums);
    /* signed with another key */
    bundle(dir);
    uint8_t other[32] = {1, 2, 3};
    release_test_key(other);
    CHECK(!verify(dir, false, "the signature doesn't fit"));
    use_test_key();
    /* no signature, or not one */
    snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SIG);
    CHECK(file_remove(path));
    CHECK(!verify(dir, false, "no SHA256SUMS.sig"));
    put(dir, RELEASE_SIG, "not a signature\n");
    CHECK(!verify(dir, false, "isn't a signature"));
    release_test_key(NULL);
}

TEST(release_sums_name_only_plain_files_here) {
    use_test_key();
    const char *bad[] = {"../etc/passwd", "/etc/passwd", "sub/file", ".hidden", "a b", "SHA256SUMS.sig", "x\\y"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s/b", test_tmpdir());
        CHECK(dir_make(dir));
        Buf sums = {0};
        line(&sums, bad[i], "x");
        put(dir, RELEASE_SUMS, buf_cstr(&sums));
        char path[700], sig[129], why[400];
        snprintf(path, sizeof path, "%s/%s", dir, RELEASE_SUMS);
        CHECK(release_sign(SEED, path, sig, why, sizeof why));
        put(dir, RELEASE_SIG, sig);
        CHECK(!verify(dir, true, "isn't a plain file here"));
        buf_free(&sums);
    }
    release_test_key(NULL);
}

TEST(release_sign_takes_only_a_known_key) {
    char dir[600], path[700], sig[129], why[400];
    snprintf(dir, sizeof dir, "%s", test_tmpdir());
    put(dir, "SHA256SUMS", "x\n");
    snprintf(path, sizeof path, "%s/SHA256SUMS", dir);
    release_test_key(NULL);
    uint8_t seed[32], pub[32];
    CHECK(hex_decode(SEED, seed, 32));
    ed25519_public(seed, pub);
    CHECK(!release_key_known(pub)); /* RFC 8032's test key is no release key */
    CHECK(!release_sign(SEED, path, sig, why, sizeof why));
    CHECK_HAS(why, "isn't one Hemma has built in");
    use_test_key();
    CHECK(!release_sign("12ab", path, sig, why, sizeof why));
    CHECK_HAS(why, "isn't 64 hex digits");
    char padded[100];
    snprintf(padded, sizeof padded, "  %s\r\n", SEED); /* as a secret pasted with its line end */
    CHECK(release_sign(padded, path, sig, why, sizeof why));
    CHECK_INT((long long)strlen(sig), 128);
    release_test_key(NULL);
}

TEST(release_key_new_never_overwrites) {
    char path[700], pub[65], why[400];
    snprintf(path, sizeof path, "%s/key.txt", test_tmpdir());
    CHECK(release_key_new(path, pub, why, sizeof why));
    CHECK_INT((long long)strlen(pub), 64);
    Buf secret = {0};
    CHECK(file_read_all(path, &secret, 200));
    CHECK_INT((long long)secret.len, 65);
    secret.p[64] = 0; /* (the line end) */
    uint8_t seed[32], derived[32], want[32];
    CHECK(hex_decode(buf_cstr(&secret), seed, 32));
    ed25519_public(seed, derived);
    CHECK(hex_decode(pub, want, 32));
    CHECK(memcmp(derived, want, 32) == 0);
    char pub2[65];
    CHECK(!release_key_new(path, pub2, why, sizeof why)); /* the first key stays */
    CHECK_HAS(why, "is there already");
    Buf again = {0};
    CHECK(file_read_all(path, &again, 200));
    CHECK(strncmp(buf_cstr(&again), buf_cstr(&secret), 64) == 0);
    buf_free(&secret);
    buf_free(&again);
}
