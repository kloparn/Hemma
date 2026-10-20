#include "../tests/test.h"
#include "codec.h"
#include "ed25519.h"

static void unhex(const char *hex, uint8_t *out, size_t n) { CHECK(hex_decode(hex, out, n)); }

/* RFC 8032 section 7.1, tests 1-3, and two of our own checked against Node's crypto (OpenSSL) */
static const struct {
    const char *seed, *msg, *pub, *sig;
} VECTORS[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
     "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "72",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "af82",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
     "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    {"0000000000000000000000000000000000000000000000000000000000000000",
     "68656d6d6120302e32312e302053484132353653554d53", /* "hemma 0.21.0 SHA256SUMS" */
     "3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29",
     "561e5b89b9f46faa2008230aa3e51091cb0e62df11e5322c25f274cbefe7ffdb"
     "efe228c5cc4f5f342c39f9bbd28f415569d43dba760a8d570e31d2947d85450a"},
};

TEST(ed25519_rfc8032_vectors) {
    for (size_t i = 0; i < sizeof VECTORS / sizeof VECTORS[0]; i++) {
        uint8_t seed[32], pub[32], want_pub[32], sig[64], want_sig[64], msg[64];
        size_t n = strlen(VECTORS[i].msg) / 2;
        unhex(VECTORS[i].seed, seed, 32);
        unhex(VECTORS[i].pub, want_pub, 32);
        unhex(VECTORS[i].sig, want_sig, 64);
        if (n) unhex(VECTORS[i].msg, msg, n);
        ed25519_public(seed, pub);
        CHECK(memcmp(pub, want_pub, 32) == 0);
        ed25519_sign(seed, msg, n, sig);
        CHECK(memcmp(sig, want_sig, 64) == 0);
        CHECK(ed25519_verify(pub, msg, n, sig));
    }
}

TEST(ed25519_long_message_and_all_ones_seed) {
    uint8_t seed[32], pub[32], want[32], sig[64], want_sig[64], msg[300];
    memset(seed, 0xff, 32);
    memset(msg, 7, sizeof msg);
    unhex("76a1592044a6e4f511265bca73a604d90b0529d1df602be30a19a9257660d1f5", want, 32);
    unhex("84228285053f8d9aa2865c2597b084da345c345d2effd6cc14324af5ee04031e"
          "510d523728ca25310d58f550d23de3d5446a84f3f8b267c63a349d5e3ca74109",
          want_sig, 64);
    ed25519_public(seed, pub);
    CHECK(memcmp(pub, want, 32) == 0);
    ed25519_sign(seed, msg, sizeof msg, sig);
    CHECK(memcmp(sig, want_sig, 64) == 0);
    CHECK(ed25519_verify(pub, msg, sizeof msg, sig));
}

TEST(ed25519_refuses_what_wasnt_signed) {
    uint8_t seed[32], pub[32], sig[64], bad[64], other[32];
    const char *msg = "abc123  hemma-pi64.tar.gz\n";
    size_t n = strlen(msg);
    unhex(VECTORS[0].seed, seed, 32);
    ed25519_public(seed, pub);
    ed25519_sign(seed, msg, n, sig);
    CHECK(ed25519_verify(pub, msg, n, sig));
    CHECK(!ed25519_verify(pub, msg, n - 1, sig)); /* a byte less */
    CHECK(!ed25519_verify(pub, "abc124  hemma-pi64.tar.gz\n", n, sig)); /* one changed */
    for (int i = 0; i < 64; i += 7) { /* any bit of R or S */
        memcpy(bad, sig, 64);
        bad[i] ^= 0x10;
        CHECK(!ed25519_verify(pub, msg, n, bad));
    }
    unhex(VECTORS[1].pub, other, 32); /* someone else's key */
    CHECK(!ed25519_verify(other, msg, n, sig));
    /* S + L is the same point but a second form of the signature: refused (malleability) */
    static const uint8_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                  0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                  0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
    memcpy(bad, sig, 64);
    unsigned carry = 0;
    for (int i = 0; i < 32; i++) {
        unsigned v = bad[32 + i] + L[i] + carry;
        bad[32 + i] = (uint8_t)v;
        carry = v >> 8;
    }
    CHECK(carry == 0);
    CHECK(!ed25519_verify(pub, msg, n, bad));
    /* a public key that isn't a point on the curve (y = 2 has no x) */
    uint8_t off[32] = {2};
    CHECK(!ed25519_verify(off, msg, n, sig));
    /* all zeros everywhere */
    uint8_t z32[32] = {0}, z64[64] = {0};
    CHECK(!ed25519_verify(z32, msg, n, z64));
    CHECK(!ed25519_verify(pub, msg, n, z64));
}
