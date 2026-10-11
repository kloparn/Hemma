/* core/ws.c — see ws.h */
#include "ws.h"
#include "codec.h"

bool ws_accept_key(const char *key, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (!key) return false;
    size_t kl = strlen(key);
    if (kl < 16 || kl > 64) return false;
    Sha1 s;
    uint8_t dig[20];
    sha1_init(&s);
    sha1_update(&s, key, kl);
    sha1_update(&s, guid, sizeof guid - 1);
    sha1_final(&s, dig);
    base64_encode(dig, 20, out);
    return true;
}

long ws_parse(uint8_t *p, size_t n, size_t max_payload, WsFrame *f) {
    if (n < 2) return 0;
    bool fin = (p[0] & 0x80) != 0;
    int rsv = p[0] & 0x70, op = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    size_t hdr = 2;
    if (rsv) return -1; /* no extensions negotiated */
    if (!masked) return -1; /* clients must mask */
    if (op != WS_CONT && op != WS_TEXT && op != WS_BINARY && op != WS_CLOSE && op != WS_PING && op != WS_PONG)
        return -1;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)p[2] << 8 | p[3];
        if (len < 126) return -1; /* non-minimal */
        hdr = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
        if (len >> 63 || len < 65536) return -1;
        hdr = 10;
    }
    if (op >= 0x8 && (!fin || len > 125)) return -1; /* control frames: short, unfragmented */
    if (len > max_payload) return -1;
    if (n < hdr + 4) return 0;
    uint8_t mask[4];
    memcpy(mask, p + hdr, 4);
    hdr += 4;
    if (n - hdr < len) return 0;
    uint8_t *pl = p + hdr;
    for (uint64_t i = 0; i < len; i++) pl[i] ^= mask[i & 3];
    f->opcode = op;
    f->fin = fin;
    f->payload = pl;
    f->len = (size_t)len;
    return (long)(hdr + len);
}

void ws_frame(Buf *out, int opcode, const void *payload, size_t n) {
    uint8_t h[10];
    size_t hl;
    h[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (n < 126) {
        h[1] = (uint8_t)n;
        hl = 2;
    } else if (n < 65536) {
        h[1] = 126;
        h[2] = (uint8_t)(n >> 8);
        h[3] = (uint8_t)n;
        hl = 4;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        hl = 10;
    }
    buf_put(out, h, hl);
    buf_put(out, payload, n);
}
