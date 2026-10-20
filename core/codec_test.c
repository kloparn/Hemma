#include "../tests/test.h"
#include "codec.h"

static void sha1_hex(const char *s, size_t n, char out[41]) {
    uint8_t d[20];
    sha1(s, n, d);
    hex_encode(d, 20, out);
}

TEST(sha1_known_vectors) {
    char h[41];
    sha1_hex("", 0, h);
    CHECK_STR(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1_hex("abc", 3, h);
    CHECK_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_hex(m, strlen(m), h);
    CHECK_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    /* a million 'a's, fed in uneven chunks */
    Sha1 s;
    sha1_init(&s);
    char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    size_t left = 1000000;
    while (left) {
        size_t k = MIN(left, sizeof chunk);
        sha1_update(&s, chunk, k);
        left -= k;
    }
    uint8_t d[20];
    sha1_final(&s, d);
    hex_encode(d, 20, h);
    CHECK_STR(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST(sha256_known_vectors) {
    char h[65];
    sha256_hex("", 0, h);
    CHECK_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256_hex("abc", 3, h);
    CHECK_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hex(m, strlen(m), h);
    CHECK_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* a million 'a's in uneven chunks exercises the buffering */
    Sha256 s;
    sha256_init(&s);
    char chunk[999];
    memset(chunk, 'a', sizeof chunk);
    for (size_t left = 1000000; left;) {
        size_t k = MIN(left, sizeof chunk);
        sha256_update(&s, chunk, k);
        left -= k;
    }
    uint8_t d[32];
    sha256_final(&s, d);
    hex_encode(d, 32, h);
    CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void sha512_hex(const void *s, size_t n, char out[129]) {
    uint8_t d[64];
    sha512(s, n, d);
    hex_encode(d, 64, out);
}

TEST(sha512_known_vectors) {
    char h[129];
    sha512_hex("", 0, h);
    CHECK_STR(h, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    sha512_hex("abc", 3, h);
    CHECK_STR(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    const char *m = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512_hex(m, strlen(m), h);
    CHECK_STR(h, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                 "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    /* the padding's edges: a length that just fits in the block, one that needs another */
    static const struct {
        int n;
        const char *want;
    } edges[] = {
        {111, "9a2a120825c2319867758ec277924f6faa254968bf752046dacdd948d8ad299b"
              "10359fd04bfd7d3810b5fa1b16a294236138baff981cbb85248478053ac4d3dd"},
        {112, "a3722b515ef40c910f2419f6e0da8ca51d410114ce6272faae64045f9e9f630e"
              "7fa8dd5a3243c9860b899d148c3da4bc0f9e07454542604d030bb55531fe0d5b"},
        {127, "1d5a8893e7b7ed83d485d26f88cfb846f3760279916976fe538e539fc16f7cd1"
              "9ba3e1c2cd5fda78749a74205755cdf694e8fa90b2bfed8815f406af76c1d7bf"},
        {128, "e2e22f8422b54b06e35c3ea30a383d1de7a8fbc27992923074103117020d8dd7"
              "024c3ecf7d6d1a15a6de5a75ff32fb486b9e8ced4c02ffe05822bf2cb734d0e0"},
        {239, "805c40517b44cef414a06c632b4ee9c21d34c85a84c3f9530274db86e2a3e38c"
              "b0e54025666b925426bd6860f81538abbfab13011e85767d814a245d232a6044"},
        {240, "3e05caa7e0fddab685e6052bd902c316505180504344d00b1217e1349a532d1d"
              "0e5ce4b24d69eea3aca369dc16d8bbdf872b2fe1770e0e1aad22f84e0519bc6c"},
    };
    char xs[240];
    memset(xs, 'x', sizeof xs);
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        sha512_hex(xs, (size_t)edges[i].n, h);
        CHECK_STR(h, edges[i].want);
    }
    Sha512 s; /* a million 'a's in uneven chunks */
    sha512_init(&s);
    char chunk[1001];
    memset(chunk, 'a', sizeof chunk);
    for (size_t left = 1000000; left;) {
        size_t k = MIN(left, sizeof chunk);
        sha512_update(&s, chunk, k);
        left -= k;
    }
    uint8_t d[64];
    sha512_final(&s, d);
    hex_encode(d, 64, h);
    CHECK_STR(h, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                 "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
}

TEST(hmac_sha256_rfc4231) {
    uint8_t mac[32];
    char h[65];
    const char *data = "what do ya want for nothing?";
    hmac_sha256("Jefe", 4, data, strlen(data), mac);
    hex_encode(mac, 32, h);
    CHECK_STR(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t key[131];
    memset(key, 0xaa, sizeof key); /* key longer than a block gets hashed first (test case 6) */
    const char *big = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key, sizeof key, big, strlen(big), mac);
    hex_encode(mac, 32, h);
    CHECK_STR(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(pbkdf2_sha256_vectors) {
    uint8_t out[64];
    char h[129];
    pbkdf2_sha256("password", 8, "salt", 4, 1, out, 32);
    hex_encode(out, 32, h);
    CHECK_STR(h, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", 8, "salt", 4, 2, out, 32);
    hex_encode(out, 32, h);
    CHECK_STR(h, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    pbkdf2_sha256("password", 8, "salt", 4, 4096, out, 32);
    hex_encode(out, 32, h);
    CHECK_STR(h, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    pbkdf2_sha256("passwd", 6, "salt", 4, 1, out, 64); /* RFC 7914 §11: two output blocks */
    hex_encode(out, 64, h);
    CHECK_STR(h, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                 "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST(hex_decode_and_constant_time_compare) {
    uint8_t b[4];
    CHECK(hex_decode("01abFF10", b, 4));
    CHECK(b[0] == 1 && b[1] == 0xab && b[2] == 0xff && b[3] == 0x10);
    CHECK(!hex_decode("01ab", b, 4));
    CHECK(!hex_decode("01abzz10", b, 4));
    CHECK(!hex_decode(NULL, b, 4));
    CHECK(ct_equal("abc", "abc", 3));
    CHECK(!ct_equal("abc", "abd", 3));
}

TEST(base64_rfc4648_vectors) {
    const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *out[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char b[16];
        base64_encode(in[i], strlen(in[i]), b);
        CHECK_STR(b, out[i]);
        uint8_t back[16];
        long n = base64_decode(out[i], strlen(out[i]), back, sizeof back);
        CHECK_INT((int)n, (int)strlen(in[i]));
        CHECK(n >= 0 && memcmp(back, in[i], (size_t)n) == 0);
    }
    uint8_t x[8];
    CHECK_INT((int)base64_decode("Zm9vYg", 6, x, sizeof x), 4); /* (padding left off) */
    CHECK_INT((int)base64_decode("Zm9v!g==", 8, x, sizeof x), -1);
    CHECK_INT((int)base64_decode("Z", 1, x, sizeof x), -1);
    CHECK_INT((int)base64_decode("Zm9vYmFy", 8, x, 5), -1); /* doesn't fit */
    uint8_t all[256], round[256];
    char enc[400];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    base64_encode(all, sizeof all, enc);
    CHECK_INT((int)base64_decode(enc, strlen(enc), round, sizeof round), 256);
    CHECK(memcmp(all, round, sizeof all) == 0);
}

TEST(url_decode_and_encode) {
    char out[64];
    url_decode("a%20b+c%2Fd%zz%4", 16, out, true);
    CHECK_STR(out, "a b c/d%zz%4");
    url_decode("a+b", 3, out, false);
    CHECK_STR(out, "a+b");
    url_decode("%C3%A5%C3%A4%C3%B6", 18, out, false);
    CHECK_STR(out, "åäö");
    Buf b = {0};
    url_encode(&b, "Tvättstuga & bastu/1", false);
    CHECK_STR(buf_cstr(&b), "Tv%C3%A4ttstuga%20%26%20bastu%2F1");
    buf_reset(&b);
    url_encode(&b, "Movies/A b.mkv", true);
    CHECK_STR(buf_cstr(&b), "Movies/A%20b.mkv");
    buf_free(&b);
}
