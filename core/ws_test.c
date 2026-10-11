#include "../tests/test.h"
#include "ws.h"

/* Build a masked client frame like a browser would. */
static size_t client_frame(uint8_t *out, int op, bool fin, const char *payload, size_t n) {
    static const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    size_t h = 0;
    out[h++] = (uint8_t)((fin ? 0x80 : 0) | op);
    if (n < 126) out[h++] = (uint8_t)(0x80 | n);
    else if (n < 65536) {
        out[h++] = 0x80 | 126;
        out[h++] = (uint8_t)(n >> 8);
        out[h++] = (uint8_t)n;
    } else {
        out[h++] = 0x80 | 127;
        for (int i = 0; i < 8; i++) out[h++] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
    }
    memcpy(out + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) out[h + i] = (uint8_t)payload[i] ^ mask[i & 3];
    return h + n;
}

TEST(ws_accept_key_rfc_example) {
    char acc[29];
    CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc));
    CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(!ws_accept_key(NULL, acc));
    CHECK(!ws_accept_key("short", acc));
}

TEST(ws_parses_masked_frames_of_all_sizes) {
    static uint8_t buf[70000 + 16];
    static char big[70000];
    memset(big, 'x', sizeof big);
    size_t sizes[] = {0, 5, 125, 126, 300, 65535, 65536, 70000};
    for (int i = 0; i < countof(sizes); i++) {
        size_t n = client_frame(buf, WS_TEXT, true, big, sizes[i]);
        WsFrame f;
        CHECK_INT(ws_parse(buf, n, 1 << 20, &f), (long)n);
        CHECK_INT(f.len, sizes[i]);
        CHECK(f.fin && f.opcode == WS_TEXT);
        CHECK(sizes[i] == 0 || (f.payload[0] == 'x' && f.payload[f.len - 1] == 'x'));
        /* every strict prefix is "need more" */
        CHECK_INT(ws_parse(buf, n - 1, 1 << 20, &f), 0);
    }
}

TEST(ws_unmasks_text) {
    uint8_t buf[64];
    size_t n = client_frame(buf, WS_TEXT, true, "{\"t\":\"hej\"}", 11);
    WsFrame f;
    CHECK_INT(ws_parse(buf, n, 1024, &f), (long)n);
    CHECK(memcmp(f.payload, "{\"t\":\"hej\"}", 11) == 0);
}

TEST(ws_rejects_protocol_violations) {
    uint8_t buf[300];
    WsFrame f;
    uint8_t unmasked[] = {0x81, 0x02, 'h', 'i'};
    CHECK_INT(ws_parse(unmasked, sizeof unmasked, 1024, &f), -1);
    size_t n = client_frame(buf, WS_TEXT, true, "hi", 2);
    buf[0] |= 0x40; /* RSV1 without negotiated extension */
    CHECK_INT(ws_parse(buf, n, 1024, &f), -1);
    n = client_frame(buf, 0x3, true, "hi", 2); /* reserved opcode */
    CHECK_INT(ws_parse(buf, n, 1024, &f), -1);
    n = client_frame(buf, WS_PING, false, "hi", 2); /* fragmented control frame */
    CHECK_INT(ws_parse(buf, n, 1024, &f), -1);
    char big[200];
    memset(big, 'p', sizeof big);
    n = client_frame(buf, WS_PING, true, big, 200); /* control frame > 125 */
    CHECK_INT(ws_parse(buf, n, 1024, &f), -1);
    n = client_frame(buf, WS_TEXT, true, big, 200);
    CHECK_INT(ws_parse(buf, n, 100, &f), -1); /* over the size limit */
}

TEST(ws_server_frames_are_unmasked) {
    Buf b = {0};
    ws_frame(&b, WS_TEXT, "abc", 3);
    CHECK_INT(b.len, 5);
    CHECK((uint8_t)b.p[0] == 0x81 && b.p[1] == 3 && memcmp(b.p + 2, "abc", 3) == 0);
    buf_reset(&b);
    static char big[70000];
    ws_frame(&b, WS_TEXT, big, 300);
    CHECK((uint8_t)b.p[1] == 126 && (uint8_t)b.p[2] == 1 && (uint8_t)b.p[3] == 44);
    buf_reset(&b);
    ws_frame(&b, WS_BINARY, big, sizeof big);
    CHECK((uint8_t)b.p[1] == 127);
    CHECK_INT(b.len, 10 + sizeof big);
    buf_free(&b);
}
