/* core/plat.h — the only place that knows about Windows vs POSIX.
 *
 * Sockets, polling, files & directories (UTF-8 paths everywhere, also on
 * Windows), threads, locks, randomness and shutdown signals. */
#ifndef HEMMA_PLAT_H
#define HEMMA_PLAT_H

#include "base.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
typedef WSAPOLLFD pollfd_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
typedef struct pollfd pollfd_t;
#endif

bool plat_init(void);
const char *plat_getenv(const char *key);                       /* NULL when unset */
bool plat_setenv(const char *key, const char *val, bool overwrite); /* process environment */
/* Tests: run in another time zone, a POSIX TZ string ("UTC0", "XXX-2" = UTC+2 all year); NULL = the
   computer's own again. Local-time functions follow at once (tzset). */
void plat_set_tz(const char *tz);
/* Called once on Ctrl+C / SIGTERM. Must only set flags (signal context). */
void plat_on_quit(void (*fn)(void));
void plat_sleep_ms(int ms);

/* ---- sockets (IPv4; addresses in host byte order) ---- */
void sock_close(sock_t s);
void sock_shutdown_write(sock_t s); /* half-close: we're done sending */
bool sock_nonblock(sock_t s, bool on);
int sock_errno(void);
bool sock_again(int err); /* would block / in progress / interrupted */
long sock_send(sock_t s, const void *p, size_t n); /* never raises SIGPIPE */
long sock_recv(sock_t s, void *p, size_t n);
int plat_poll(pollfd_t *fds, int n, int timeout_ms);
/* The address --host means, as tcp_listen reads it: "", "*" and 0.0.0.0 = every address (0), "localhost" =
   127.0.0.1, else an IPv4 address. false: none of those (tcp_listen refuses it). */
bool listen_addr(const char *host, uint32_t *ip);
sock_t tcp_listen(const char *host, int port, char *err, size_t errcap);
int tcp_local_port(sock_t s);
sock_t tcp_accept(sock_t listener, uint32_t *peer_ip);
sock_t tcp_connect_nb(uint32_t ip, int port); /* non-blocking; poll for POLLOUT */
/* A name's IPv4 address by the system's resolver (DNS, /etc/hosts). BLOCKS - seconds when the network is
   gone: threads only, never the event loop (core/fetch.c). */
bool plat_resolve(const char *host, uint32_t *ip);
bool tcp_connect_done(sock_t s);              /* after POLLOUT: did it connect? */
sock_t udp_open(uint16_t port, bool broadcast);
sock_t udp_open_loopback(uint16_t *port); /* 127.0.0.1 only, ephemeral port (wake-ups) */
long udp_send(sock_t s, uint32_t ip, uint16_t port, const void *p, size_t n);
long udp_recv(sock_t s, void *p, size_t cap, uint32_t *ip, uint16_t *port);
bool udp_join_multicast(sock_t s, uint32_t group);
void ip_format(uint32_t ip, char out[16]);
bool ip_parse(const char *s, uint32_t *ip);

/* ---- randomness (OS CSPRNG) ---- */
void rand_bytes(void *out, size_t n);
void rand_hex(char *out, size_t nbytes); /* writes 2*nbytes hex chars + NUL */

/* ---- files & directories (UTF-8 paths, '/' or '\\' separators) ---- */
FILE *file_open(const char *path, const char *mode);
bool file_seek(FILE *f, int64_t offset);                    /* 64-bit safe everywhere (files > 4 GB) */
bool file_pread(FILE *f, int64_t offset, void *dst, size_t n); /* exactly n bytes at offset */
bool file_read_all(const char *path, Buf *out, size_t max_bytes);
bool file_write_atomic(const char *path, const void *data, size_t n); /* tmp + fsync + rename */
/* The same for secrets: readable by its owner only (POSIX 0600); on Windows it gets the
   folder's permissions (a user's own folders are private to them and administrators). */
bool file_write_private(const char *path, const void *data, size_t n);
bool file_sync(FILE *f);
bool file_replace(const char *from, const char *to); /* atomic rename over existing */
bool file_remove(const char *path);
bool path_is_dir(const char *path);
bool path_stat(const char *path, int64_t *size, int64_t *mtime, bool *is_dir);
bool dir_make(const char *path); /* mkdir -p */
bool dir_remove(const char *path); /* an empty directory */
bool path_free_space(const char *path, int64_t *free_bytes); /* on the volume holding path */
bool path_disk_space(const char *path, int64_t *free_bytes, int64_t *total_bytes); /* either may be NULL */
/* Disks a media library might live on (issue #27: suggestions on Library settings): Linux the mount points under
   /mnt and /media (from /proc/self/mounts, file systems that hold files), macOS those under /Volumes, Windows the
   fixed and removable drive letters but the system's ("D:/"). Returns how many. */
int plat_mounts(char out[][300], int max);
/* The Linux part on its own (any platform: tests): /proc/self/mounts' text -> those mount points. */
int plat_parse_mounts(const char *text, char out[][300], int max);
/* Zero-copy file -> socket (Linux sendfile): up to n bytes of f from offset
   `off`. Returns bytes sent, 0 when the socket would block, -1 on error or
   when the file ends early, -2 where unsupported (fall back to read + send). */
long file_sendfile(sock_t s, FILE *f, int64_t off, size_t n);

typedef struct DirIter DirIter;
typedef struct {
    char name[512]; /* UTF-8, no path */
    bool is_dir;    /* a directory, or a link to one (is_dir then says what it points at) */
    bool is_link;   /* a symbolic link / junction: never follow it when deleting things */
    int64_t size, mtime;
} DirEntry;
DirIter *dir_open(const char *path);
bool dir_next(DirIter *it, DirEntry *e); /* skips "." and ".." */
void dir_close(DirIter *it);

/* ---- threads & locks ---- */
bool thread_spawn(void (*fn)(void *), void *arg); /* detached */
void thread_background(void); /* lower the calling thread's priority (long batch work) */
int plat_cpu_count(void);     /* logical CPUs */
const char *plat_bundle(void); /* the release download that is this build ("hemma-pi64" ...), NULL = none */

typedef struct {
#ifdef _WIN32
    SRWLOCK l;
#else
    pthread_mutex_t l;
#endif
} Lock;
#ifdef _WIN32
#define LOCK_INIT {SRWLOCK_INIT}
#else
#define LOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
#endif
void lock_take(Lock *l);
void lock_give(Lock *l);

#endif
