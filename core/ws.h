/* core/ws.h — WebSocket (RFC 6455) handshake and framing, transport-free. */
#ifndef HEMMA_WS_H
#define HEMMA_WS_H

#include "base.h"

enum { WS_CONT = 0x0, WS_TEXT = 0x1, WS_BINARY = 0x2, WS_CLOSE = 0x8, WS_PING = 0x9, WS_PONG = 0xA };

typedef struct {
    int opcode;
    bool fin;
    uint8_t *payload; /* unmasked in place, inside the caller's buffer */
    size_t len;
} WsFrame;

/* Sec-WebSocket-Accept for a client key: base64(sha1(key + GUID)). */
bool ws_accept_key(const char *client_key, char out[29]);

/* Parse one client frame from p[0..n). Unmasks in place.
 * Returns bytes consumed, 0 if more data is needed, -1 on protocol error
 * (unmasked frame, reserved bits, bad control frame, payload > max). */
long ws_parse(uint8_t *p, size_t n, size_t max_payload, WsFrame *f);

/* Append an unmasked server frame. */
void ws_frame(Buf *out, int opcode, const void *payload, size_t n);

#endif
