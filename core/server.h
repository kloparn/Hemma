/* core/server.h — single-threaded non-blocking event loop: HTTP/1.1
 * keep-alive connections, WebSocket upgrade/framing, extra watched sockets
 * (UDP discovery, outgoing TCP) and a tick whose pace the app sets.
 *
 * Defensive by design (it faces every device on the LAN): bounded buffers (and a
 * shared budget for bodies still arriving), backpressure on pipelined requests,
 * stall and per-device limits, and a loop that sleeps in poll() until there is work. */
#ifndef HEMMA_SERVER_H
#define HEMMA_SERVER_H

#include "http.h"
#include "plat.h"

typedef struct Conn Conn;
struct Conn {
    sock_t fd;
    unsigned id;
    uint32_t ip; /* peer IPv4, host order */
    char peer[16];
    Buf in, out;
    int64_t t_open, t_last, t_req; /* monotonic ms: opened, last activity, start of a partial request */
    int64_t t_sent;                /* last time queued output made progress */
    int64_t t_moved;               /* last time queued output moved a whole chunk (or began) */
    size_t moved;                  /* bytes sent since then */
    bool ws, keep_alive, closing, lingering, dead, detached, ping_sent;
    bool head_req; /* the request being answered is HEAD: replies carry no body */
    bool peer_eof; /* the peer finished sending; we are still sending to it */
    int64_t linger_until, close_at;
    Buf frag; /* fragmented WebSocket message being reassembled */
    int frag_op;
    void *user; /* owned by the app (e.g. its live-client record) */
};

typedef struct {
    void (*http)(Conn *c, HttpReq *req); /* respond, upgrade or detach */
    void (*ws_message)(Conn *c, const char *msg, size_t n);
    void (*ws_closed)(Conn *c); /* called once before an upgraded conn is freed */
    void (*tick)(int64_t now_ms);
    void (*idle)(void); /* after every loop turn: flush pending work */
    /* When the next tick is due (monotonic ms); the loop sleeps until then
       unless sockets need attention. NULL = every 100 ms. */
    int64_t (*next_tick)(int64_t now_ms);
} ServerHooks;

/* A connection handed over to another owner (e.g. a worker thread). */
typedef struct {
    sock_t fd;   /* still non-blocking; the new owner closes it */
    Buf unsent;  /* response bytes queued but not yet written: send these first */
    Buf body;    /* streamed request (req->streaming): body bytes already read */
} Detached;

bool server_listen(const char *host, int port);
int server_port(void);
void server_run(const ServerHooks *hooks); /* returns after server_stop() */
void server_stop(void);                    /* async-signal / thread safe; wakes the loop */
void server_wake(void);                    /* thread safe: end the current poll() wait now */
void server_shutdown(void);                /* close every socket (after run) */
int server_conn_count(void);
/* All connections' input buffers beyond 64 KB each share this budget (default 32 MB;
   0 = default): past it a request is answered 503 and closed. Tests make it small. */
void server_set_buffer_budget(size_t bytes);
void server_set_max_conns(int n); /* tests: fewer connection slots (0 = the default, 400) */
void server_set_slow_after(int ms); /* tests: when a slow reader may be pushed out (0 = the default, 10 s) */

void conn_write(Conn *c, const void *p, size_t n);
/* Full response honouring the request's keep-alive. */
void conn_reply(Conn *c, int status, const char *ctype, const void *body, size_t n, const char *extra);
void conn_close(Conn *c); /* graceful: flush, half-close, then close */
bool conn_ws_upgrade(Conn *c, const HttpReq *req);
void conn_ws_send(Conn *c, const char *text, size_t n);
/* Hand the socket to someone else (e.g. a streaming thread) without blocking
   the loop; the Conn is gone afterwards. `req` (may be NULL) is the request
   being handled: for a streamed upload, the body bytes already received are
   moved into out->body. The caller owns out->unsent / out->body (buf_free). */
void conn_detach(Conn *c, const HttpReq *req, Detached *out);

typedef void (*WatchFn)(sock_t fd, short revents, void *ctx);
void watch_add(sock_t fd, short events, WatchFn fn, void *ctx);
void watch_events(sock_t fd, short events);
void watch_del(sock_t fd); /* stops watching; does not close */

#endif
