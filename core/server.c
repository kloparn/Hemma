/* core/server.c — see server.h */
#include "server.h"
#include "ws.h"

#include <signal.h>

#define MAX_CONNS 400
/* One device can't take every slot. Streams and uploads are handed to threads
   (conn_detach) and don't count; a browser keeps up to 6 keep-alive connections per
   host plus one WebSocket per open tab - a TV or phone uses well under 10. */
#define MAX_PER_IP 32
#define READ_BUDGET (256 * 1024)        /* per readiness event, keeps the loop fair */
#define MAX_BODY (1024 * 1024)          /* largest buffered request body (bigger ones stream) */
#define MAX_IN (MAX_BODY + 4 * HTTP_MAX_HEADER_BYTES)
/* Input buffers are filled before anyone is signed in, so all of them together get a
   budget: every connection may hold SMALL_IN (headers, forms, wire messages); beyond
   that, bodies and big WebSocket messages share BUFFER_BUDGET. A connection that would
   push past it is answered 503 and closed (review: 400 connections x 1 MB bodies took
   the server from 13 MB to 420 MB). */
#define SMALL_IN (64 * 1024)
#define BUFFER_BUDGET (32u << 20)
#define OUT_PAUSE (256 * 1024)          /* stop reading requests while this much output waits */
#define MAX_PENDING_OUT (32u << 20)     /* a client this far behind is dropped */
#define WS_MAX_MESSAGE (1024 * 1024)
#define T_HEADERS 15000 /* a request must arrive completely within this */
#define T_IDLE 75000    /* idle keep-alive connections are closed */
#define T_WS_PING 25000 /* ping quiet WebSockets ... */
#define T_WS_DEAD 70000 /* ... and drop them if nothing comes back */
#define T_LINGER 2000   /* after half-closing, wait this long for the peer */
#define T_CLOSING 5000  /* a WebSocket close handshake gets this long */
#define T_STALL 60000   /* queued output that makes no progress for this long: the peer is gone */

typedef struct {
    sock_t fd;
    short events;
    WatchFn fn;
    void *ctx;
    bool dead;
} Watch;

static sock_t g_listen = SOCK_BAD;
static sock_t g_wake = SOCK_BAD; /* loopback UDP: other threads (and signals) nudge poll() awake */
static uint16_t g_wake_port;
static int g_port;
static volatile sig_atomic_t g_stop;
static const ServerHooks *H;
static Conn **conns;
static int nconns, capconns;
static Watch *watches;
static int nwatches, capwatches;
static unsigned next_id;
static size_t g_buffer_budget = BUFFER_BUDGET;
static int g_max_conns = MAX_CONNS;

void server_set_buffer_budget(size_t bytes) { g_buffer_budget = bytes ? bytes : BUFFER_BUDGET; }
void server_set_max_conns(int n) { g_max_conns = n > 0 ? n : MAX_CONNS; }

bool server_listen(const char *host, int port) {
    char err[200];
    g_listen = tcp_listen(host, port, err, sizeof err);
    if (g_listen == SOCK_BAD) {
        log_error("%s", err);
        return false;
    }
    g_port = tcp_local_port(g_listen);
    return true;
}

int server_port(void) { return g_port; }
void server_wake(void) {
    if (g_wake != SOCK_BAD) udp_send(g_wake, 0x7F000001, g_wake_port, "!", 1); /* sendto: signal-safe */
}

void server_stop(void) {
    g_stop = 1;
    server_wake();
}

int server_conn_count(void) {
    int n = 0;
    for (int i = 0; i < nconns; i++) n += !conns[i]->dead;
    return n;
}

/* ------------------------------------------------------------ connections */

/* Buffers are freed in compact(), not here: a handler may still be reading
   the request body that lives in c->in when a write kills the connection. */
static void conn_kill(Conn *c) { c->dead = true; }

static void conn_free(Conn *c) {
    if (c->ws && H && H->ws_closed) H->ws_closed(c);
    if (!c->detached) sock_close(c->fd);
    buf_free(&c->in);
    buf_free(&c->out);
    buf_free(&c->frag);
    free(c);
}

static void conn_flush(Conn *c) {
    while (!c->dead && c->out.len) {
        long n = sock_send(c->fd, c->out.p, c->out.len);
        if (n > 0) {
            buf_consume(&c->out, (size_t)n);
            c->t_sent = now_ms();
            continue;
        }
        if (n < 0 && sock_again(sock_errno())) break;
        conn_kill(c);
        return;
    }
    if (!c->dead && c->closing && !c->out.len && !c->lingering) {
        if (c->peer_eof) { /* it already said goodbye: nothing to wait for */
            conn_kill(c);
            return;
        }
        sock_shutdown_write(c->fd);
        c->lingering = true;
        c->linger_until = now_ms() + T_LINGER;
    }
}

void conn_write(Conn *c, const void *p, size_t n) {
    if (c->dead || c->lingering) return;
    if (!c->out.len) c->t_sent = now_ms(); /* the stall clock starts with the first queued byte */
    buf_put(&c->out, p, n);
    if (c->out.len > MAX_PENDING_OUT) {
        log_warn("dropping slow client %s", c->peer);
        conn_kill(c);
    }
}

void conn_reply(Conn *c, int status, const char *ctype, const void *body, size_t n, const char *extra) {
    if (c->dead) return;
    Buf b = {0};
    if (c->head_req) http_head(&b, status, ctype, (long long)n, extra, c->keep_alive); /* HEAD: no body */
    else http_reply(&b, status, ctype, body, n, extra, c->keep_alive);
    conn_write(c, b.p, b.len);
    buf_free(&b);
    if (!c->keep_alive) c->closing = true;
    conn_flush(c);
}

static void ws_send_frame(Conn *c, int op, const void *p, size_t n) {
    if (c->dead || c->lingering) return;
    Buf b = {0};
    ws_frame(&b, op, p, n);
    conn_write(c, b.p, b.len);
    buf_free(&b);
    conn_flush(c);
}

static void ws_close_with(Conn *c, int code) {
    uint8_t pl[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    ws_send_frame(c, WS_CLOSE, pl, 2);
    if (!c->closing) c->close_at = now_ms();
    c->closing = true;
    c->in.len = 0; /* whatever else it sent no longer matters */
    conn_flush(c);
}

void conn_close(Conn *c) {
    if (c->dead) return;
    if (c->ws && !c->closing) {
        ws_close_with(c, 1000);
        return;
    }
    c->closing = true;
    conn_flush(c);
}

void conn_ws_send(Conn *c, const char *text, size_t n) {
    if (!c->ws || c->closing) return;
    ws_send_frame(c, WS_TEXT, text, n);
}

bool conn_ws_upgrade(Conn *c, const HttpReq *req) {
    const char *key = http_header(req, "Sec-WebSocket-Key");
    char acc[29];
    if (!str_eq(req->method, "GET") || req->content_length || !http_header_has(req, "Upgrade", "websocket") ||
        !http_header_has(req, "Connection", "upgrade") || !str_eq(http_header(req, "Sec-WebSocket-Version"), "13") ||
        !ws_accept_key(key, acc)) {
        static const char msg[] = "WebSocket upgrade required\n";
        c->keep_alive = false;
        conn_reply(c, 400, "text/plain; charset=utf-8", msg, sizeof msg - 1, "Sec-WebSocket-Version: 13\r\n");
        return false;
    }
    Buf b = {0};
    buf_printf(&b,
               "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n",
               acc);
    conn_write(c, b.p, b.len);
    buf_free(&b);
    c->ws = true;
    c->keep_alive = true;
    conn_flush(c);
    return true;
}

void conn_detach(Conn *c, const HttpReq *req, Detached *out) {
    memset(out, 0, sizeof *out);
    out->fd = c->fd;
    out->unsent = c->out; /* never block the loop on a slow peer: the new owner sends it */
    c->out = (Buf){0};
    if (req && req->streaming && c->in.len > req->head_len) {
        size_t n = c->in.len - req->head_len;
        if ((unsigned long long)n > (unsigned long long)req->content_length) n = (size_t)req->content_length;
        buf_put(&out->body, c->in.p + req->head_len, n);
    }
    c->detached = true;
    c->dead = true;
}

/* --------------------------------------------------------- input handling */

static void ws_deliver(Conn *c, int op, char *msg, size_t n) {
    if (op != WS_TEXT) return; /* the wire speaks JSON text only */
    if (!utf8_valid(msg, n)) {
        ws_close_with(c, 1007);
        return;
    }
    if (H->ws_message) H->ws_message(c, msg, n);
}

/* Handle one frame; false when nothing more can be done right now. */
static bool ws_step(Conn *c) {
    WsFrame f;
    long used = ws_parse((uint8_t *)c->in.p, c->in.len, WS_MAX_MESSAGE, &f);
    if (used == 0) return false;
    if (used < 0) goto protocol_error;
    c->ping_sent = false;
    switch (f.opcode) {
    case WS_TEXT:
    case WS_BINARY:
        if (c->frag_op) goto protocol_error;
        if (f.fin) ws_deliver(c, f.opcode, (char *)f.payload, f.len);
        else {
            c->frag_op = f.opcode;
            buf_put(&c->frag, f.payload, f.len);
        }
        break;
    case WS_CONT:
        if (!c->frag_op || c->frag.len + f.len > WS_MAX_MESSAGE) goto protocol_error;
        buf_put(&c->frag, f.payload, f.len);
        if (f.fin) {
            int op = c->frag_op;
            c->frag_op = 0;
            ws_deliver(c, op, c->frag.p, c->frag.len);
            c->frag.len = 0;
        }
        break;
    case WS_PING: ws_send_frame(c, WS_PONG, f.payload, f.len); break;
    case WS_PONG: break;
    case WS_CLOSE: {
        uint8_t code[2] = {0x03, 0xE8}; /* 1000 */
        if (f.len >= 2) code[0] = f.payload[0], code[1] = f.payload[1];
        ws_send_frame(c, WS_CLOSE, code, 2);
        if (!c->closing) c->close_at = now_ms();
        c->closing = true;
        c->in.len = 0;
        conn_flush(c);
        return false;
    }
    }
    if (c->dead || c->closing) return false; /* a handler closed it; c->in may have been cleared */
    buf_consume(&c->in, (size_t)used);
    return true;

protocol_error:
    ws_close_with(c, 1002);
    return false;
}

/* A browser trying https:// on this plain-http port (some phones and TVs do,
   then report "invalid response"): say so in the log and hang up, which also
   lets browsers that can fall back to http:// do so at once. */
static bool tls_hello(Conn *c) {
    if (c->ws || c->in.len < 3 || (uint8_t)c->in.p[0] != 0x16 || (uint8_t)c->in.p[1] != 0x03) return false;
    static struct {
        uint32_t ip;
        int64_t at;
    } told[8]; /* once per device per 10 minutes */
    int64_t now = now_ms();
    int slot = 0;
    for (int i = 0; i < (int)countof(told); i++) {
        if (told[i].ip == c->ip) {
            slot = i;
            break;
        }
        if (told[i].at < told[slot].at) slot = i;
    }
    if (told[slot].ip != c->ip || now - told[slot].at > 600000)
        log_warn("%s tried https:// - Hemma speaks plain http: open http://<this machine>:%d", c->peer, g_port);
    told[slot].ip = c->ip;
    told[slot].at = now;
    conn_kill(c);
    return true;
}

static void conn_process(Conn *c) {
    if (tls_hello(c)) return;
    while (!c->dead && c->in.len) {
        if (c->closing) { /* closing (HTTP or WebSocket): input no longer matters */
            c->in.len = 0;
            break;
        }
        if (c->out.len > OUT_PAUSE) break; /* backpressure: let the peer read what it asked for first */
        if (c->ws) {
            if (!ws_step(c)) break;
            continue;
        }
        HttpReq req;
        int r = http_parse(c->in.p, c->in.len, MAX_BODY, &req);
        if (r == 0) break;
        if (r < 0) {
            char body[80];
            int bl = snprintf(body, sizeof body, "%d %s\n", -r, http_status_text(-r));
            c->keep_alive = false;
            conn_reply(c, -r, "text/plain; charset=utf-8", body, (size_t)bl, NULL);
            c->in.len = 0;
            break;
        }
        size_t used = req.total_len;
        c->keep_alive = req.keep_alive;
        c->head_req = str_eq(req.method, "HEAD");
        c->t_req = 0;
        H->http(c, &req);
        http_req_free(&req);
        if (c->dead) return; /* killed or detached: hands off the buffers */
        c->head_req = false;
        if (req.streaming) { /* nobody took the body, and we won't read gigabytes to skip it */
            c->keep_alive = false;
            c->closing = true;
            c->in.len = 0;
            break;
        }
        buf_consume(&c->in, used);
        if (!c->keep_alive && !c->ws) {
            c->closing = true;
            c->in.len = 0;
        }
        if (c->in.len && !c->ws) c->t_req = now_ms();
    }
    if (!c->dead) conn_flush(c);
}

/* ------------------------------------------------- the input buffer budget */

/* Input memory a connection holds beyond SMALL_IN (allocated, not just used). */
static size_t held(const Conn *c) {
    size_t n = c->in.cap + c->frag.cap;
    return c->dead || n <= SMALL_IN ? 0 : n - SMALL_IN;
}

static size_t held_total(void) {
    size_t n = 0;
    for (int i = 0; i < nconns; i++) n += held(conns[i]);
    return n;
}

/* Big buffers that are empty again go back to the allocator (a keep-alive connection
   that once posted 1 MB shouldn't sit on 2 MB until it closes). */
static void trim(Conn *c) {
    if (!c->in.len && c->in.cap > SMALL_IN) buf_free(&c->in);
    if (!c->frag.len && c->frag.cap > SMALL_IN) buf_free(&c->frag);
}

/* Still holding a partial body or message while all of them together are over budget:
   refuse this one (503, or WebSocket close 1009) and free what it held at once. */
static void check_budget(Conn *c) {
    if (c->dead) return;
    trim(c);
    if (!held(c) || held_total() <= g_buffer_budget) return;
    static int64_t told;
    int64_t now = now_ms();
    if (!told || now - told > 60000) {
        told = now;
        log_warn("request bodies waiting in memory passed %zu MB: refusing %s's for now", g_buffer_budget >> 20,
                 c->peer);
    }
    if (c->ws) ws_close_with(c, 1009); /* message too big (for now) */
    else {
        static const char msg[] = "Hemma is busy - try again in a moment\n";
        c->keep_alive = false;
        conn_reply(c, 503, "text/plain; charset=utf-8", msg, sizeof msg - 1, "Retry-After: 5\r\n");
    }
    c->in.len = 0;
    c->frag.len = 0;
    c->frag_op = 0;
    buf_free(&c->in);
    buf_free(&c->frag);
}

static void conn_read(Conn *c) {
    size_t budget = READ_BUDGET;
    bool eof = false;
    for (;;) {
        buf_reserve(&c->in, 16384);
        size_t room = c->in.cap - c->in.len - 1;
        long n = sock_recv(c->fd, c->in.p + c->in.len, room);
        if (n > 0) {
            if (c->lingering || c->closing) { /* discard until the peer closes */
                if (budget <= (size_t)n) break;
                budget -= (size_t)n;
                continue;
            }
            c->in.len += (size_t)n;
            c->t_last = now_ms();
            if (!c->ws && !c->t_req) c->t_req = c->t_last;
            if ((size_t)n < room || budget <= (size_t)n) break;
            budget -= (size_t)n;
            if (c->in.len > MAX_IN) break;
            continue;
        }
        if (n == 0) eof = true;
        else if (sock_again(sock_errno())) break;
        else eof = true;
        break;
    }
    if (c->lingering) {
        if (eof) conn_kill(c);
        return;
    }
    if (c->in.len > MAX_IN) { /* nothing legitimate buffers this much without being parsed */
        conn_kill(c);
        return;
    }
    conn_process(c);
    check_budget(c);
    if (eof && !c->dead) {
        /* The peer is done sending. Finish what it asked for (a client that
           half-closes after its request still wants the whole response). */
        if (c->ws || !c->out.len) conn_kill(c);
        else {
            c->peer_eof = true;
            c->closing = true;
        }
    }
}

/* ------------------------------------------------------------------ watch */

void watch_add(sock_t fd, short events, WatchFn fn, void *ctx) {
    if (nwatches == capwatches) {
        capwatches = capwatches ? capwatches * 2 : 8;
        watches = xrealloc(watches, (size_t)capwatches * sizeof *watches);
    }
    watches[nwatches++] = (Watch){fd, events, fn, ctx, false};
}

void watch_events(sock_t fd, short events) {
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == fd && !watches[i].dead) watches[i].events = events;
}

void watch_del(sock_t fd) {
    for (int i = 0; i < nwatches; i++)
        if (watches[i].fd == fd) watches[i].dead = true;
}

/* -------------------------------------------------------------------- loop */

static int conns_from(uint32_t ip) {
    int n = 0;
    for (int i = 0; i < nconns; i++) n += !conns[i]->dead && conns[i]->ip == ip;
    return n;
}

static int live_conns(void) {
    int n = 0;
    for (int i = 0; i < nconns; i++) n += !conns[i]->dead;
    return n;
}

/* A connection that only holds a slot: an idle keep-alive one, or one still dribbling in its
   request. Screens (WebSockets), handed-over streams and anything owed a reply never are. */
static bool holds_a_slot(const Conn *c) {
    return !c->dead && !c->ws && !c->detached && !c->closing && !c->lingering && !c->peer_eof && !c->out.len;
}

/* When every slot is taken, a newcomer takes the place of a slot holder - from the address
   holding the most connections, idle ones before half-sent requests, the longest-waiting
   first. Without this, one host with a dozen addresses could keep every slot with a request
   every minute and nobody else could connect (issue #5). */
static Conn *slot_holder(void) {
    Conn *best = NULL;
    int best_n = 0;
    for (int i = 0; i < nconns; i++) {
        Conn *c = conns[i];
        if (!holds_a_slot(c)) continue;
        int n = conns_from(c->ip);
        bool idle = !c->t_req, best_idle = best && !best->t_req;
        int64_t since = idle ? c->t_last : c->t_req, best_since = best ? (best_idle ? best->t_last : best->t_req) : 0;
        if (!best || n > best_n || (n == best_n && idle && !best_idle) ||
            (n == best_n && idle == best_idle && since < best_since)) {
            best = c;
            best_n = n;
        }
    }
    return best;
}

static bool any_slot_holder(void) {
    for (int i = 0; i < nconns; i++)
        if (holds_a_slot(conns[i])) return true;
    return false;
}

static void accept_all(void) {
    for (int k = 0; k < 64; k++) {
        bool full = live_conns() >= g_max_conns;
        if (full && !any_slot_holder()) break;
        uint32_t ip = 0;
        sock_t s = tcp_accept(g_listen, &ip);
        if (s == SOCK_BAD) break;
        if (conns_from(ip) >= MAX_PER_IP) {
            sock_close(s);
            continue;
        }
        if (full) conn_kill(slot_holder()); /* freed by the next compact() */
        Conn *c = xcalloc(1, sizeof *c);
        c->fd = s;
        c->ip = ip;
        c->id = ++next_id;
        c->t_open = c->t_last = now_ms();
        ip_format(ip, c->peer);
        if (nconns == capconns) {
            capconns = capconns ? capconns * 2 : 32;
            conns = xrealloc(conns, (size_t)capconns * sizeof *conns);
        }
        conns[nconns++] = c;
    }
}

static void check_timeouts(int64_t now) {
    for (int i = 0; i < nconns; i++) {
        Conn *c = conns[i];
        if (c->dead) continue;
        if (c->out.len && now - c->t_sent > T_STALL) {
            conn_kill(c); /* it stopped reading what it asked for */
        } else if (c->lingering) {
            if (now > c->linger_until) conn_kill(c);
        } else if (c->ws) {
            if (c->closing) {
                if (now - c->close_at > T_CLOSING) conn_kill(c);
            } else if (now - c->t_last > T_WS_DEAD) conn_kill(c);
            else if (now - c->t_last > T_WS_PING && !c->ping_sent) {
                ws_send_frame(c, WS_PING, "", 0);
                c->ping_sent = true;
            }
        } else if (c->t_req && now - c->t_req > T_HEADERS) {
            conn_kill(c);
        } else if (!c->out.len && now - c->t_last > T_IDLE) {
            conn_kill(c);
        }
    }
}

static void compact(void) {
    int w = 0;
    for (int i = 0; i < nconns; i++) {
        Conn *c = conns[i];
        if (c->dead) conn_free(c);
        else conns[w++] = c;
    }
    nconns = w;
    w = 0;
    for (int i = 0; i < nwatches; i++)
        if (!watches[i].dead) watches[w++] = watches[i];
    nwatches = w;
}

static short conn_events(const Conn *c) {
    short ev = 0;
    /* read more only while the peer keeps up with our output (pipelining,
       pings) and hasn't finished sending */
    if (!c->peer_eof && c->out.len <= OUT_PAUSE) ev |= POLLIN;
    if (c->out.len) ev |= POLLOUT;
    return ev;
}

void server_run(const ServerHooks *hooks) {
    H = hooks;
    g_stop = 0;
    pollfd_t *pf = NULL;
    int cap = 0;
    int64_t last_tick = 0;
    if (g_wake == SOCK_BAD) g_wake = udp_open_loopback(&g_wake_port);
    while (!g_stop) {
        compact();
        int need = 2 + nconns + nwatches;
        if (need > cap) {
            cap = need + 32;
            pf = xrealloc(pf, (size_t)cap * sizeof *pf);
        }
        int n = 0, nc = nconns, nw = nwatches;
        /* at capacity - nothing a newcomer could take the place of - the listener stays out of
           the poll set (or poll would spin on it) */
        bool room = nconns < g_max_conns || any_slot_holder();
        pf[n++] = (pollfd_t){.fd = g_listen, .events = (short)(room ? POLLIN : 0)};
        pf[n++] = (pollfd_t){.fd = g_wake, .events = (short)(g_wake != SOCK_BAD ? POLLIN : 0)};
        for (int i = 0; i < nc; i++) pf[n++] = (pollfd_t){.fd = conns[i]->fd, .events = conn_events(conns[i])};
        for (int i = 0; i < nw; i++) pf[n++] = (pollfd_t){.fd = watches[i].fd, .events = watches[i].events};

        int64_t now = now_ms();
        int64_t due = H->next_tick ? H->next_tick(now) : last_tick + 100;
        int wait = due <= now ? 0 : (int)MIN(due - now, 60000);
        if (plat_poll(pf, n, wait) < 0 && !sock_again(sock_errno())) plat_sleep_ms(20);

        for (int i = 0; i < nc; i++) {
            Conn *c = conns[i];
            short re = pf[2 + i].revents;
            if (!re || c->dead) continue;
            if (re & POLLNVAL) {
                conn_kill(c);
                continue;
            }
            if (c->peer_eof) { /* only sending is left */
                if (re & POLLOUT) conn_flush(c);
                else if (re & (POLLHUP | POLLERR)) conn_kill(c);
                continue;
            }
            if (re & (POLLIN | POLLHUP | POLLERR)) conn_read(c);
            if (!c->dead && (re & POLLOUT)) {
                conn_flush(c);
                /* output drained below the pause mark: carry on with buffered requests */
                if (!c->dead && c->in.len && c->out.len <= OUT_PAUSE) {
                    conn_process(c);
                    if (!c->dead) trim(c);
                }
            }
        }
        for (int i = 0; i < nw; i++) {
            short re = pf[2 + nc + i].revents;
            if (re && !watches[i].dead && watches[i].fd == pf[2 + nc + i].fd)
                watches[i].fn(watches[i].fd, re, watches[i].ctx);
        }
        if (pf[0].revents & POLLIN) accept_all();
        if (pf[1].revents & POLLIN) {
            char drain[16];
            while (udp_recv(g_wake, drain, sizeof drain, NULL, NULL) > 0) {}
        }

        now = now_ms();
        due = H->next_tick ? H->next_tick(now) : last_tick + 100;
        if (now >= due) {
            last_tick = now;
            check_timeouts(now);
            if (H->tick) H->tick(now);
        }
        if (H->idle) H->idle();
    }
    free(pf);
}

void server_shutdown(void) {
    for (int i = 0; i < nconns; i++) {
        Conn *c = conns[i];
        if (c->ws && !c->dead) ws_close_with(c, 1001); /* going away */
        conn_kill(c);
    }
    compact();
    free(conns);
    conns = NULL;
    nconns = capconns = 0;
    free(watches);
    watches = NULL;
    nwatches = capwatches = 0;
    sock_close(g_listen);
    g_listen = SOCK_BAD;
    if (g_wake != SOCK_BAD) sock_close(g_wake);
    g_wake = SOCK_BAD;
}
