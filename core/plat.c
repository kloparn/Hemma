/* core/plat.c — Windows and POSIX implementations of plat.h */
#define _FILE_OFFSET_BITS 64 /* 64-bit off_t for fseeko on 32-bit glibc (musl always has it) */
#include "plat.h"

#ifdef _WIN32
#include <bcrypt.h>
#include <direct.h> /* _wrmdir */
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#ifdef __APPLE__
#include <pthread/qos.h> /* thread_background */
#endif
#endif

/* ================================================================ process */

static void (*quit_fn)(void);

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (quit_fn) quit_fn();
        return TRUE;
    }
    return FALSE;
}
#else
static void on_signal(int sig) {
    (void)sig;
    if (quit_fn) quit_fn();
}
#endif

bool plat_init(void) {
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    return true;
}

const char *plat_getenv(const char *key) { return getenv(key); }

bool plat_setenv(const char *key, const char *val, bool overwrite) {
    if (!overwrite && getenv(key)) return true;
#ifdef _WIN32
    return _putenv_s(key, val) == 0;
#else
    return setenv(key, val, 1) == 0;
#endif
}

void plat_on_quit(void (*fn)(void)) {
    quit_fn = fn;
#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
}

void plat_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

/* ================================================================ sockets */

void sock_close(sock_t s) {
    if (s == SOCK_BAD) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void sock_shutdown_write(sock_t s) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}

bool sock_nonblock(sock_t s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
#endif
}

int sock_errno(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool sock_again(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINTR;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS || err == EINTR;
#endif
}

long sock_send(sock_t s, const void *p, size_t n) {
    if (n > 1u << 30) n = 1u << 30;
#ifdef _WIN32
    return send(s, (const char *)p, (int)n, 0);
#else
    return (long)send(s, p, n, MSG_NOSIGNAL);
#endif
}

long sock_recv(sock_t s, void *p, size_t n) {
    if (n > 1u << 30) n = 1u << 30;
#ifdef _WIN32
    return recv(s, (char *)p, (int)n, 0);
#else
    return (long)recv(s, p, n, 0);
#endif
}

int plat_poll(pollfd_t *fds, int n, int timeout_ms) {
#ifdef _WIN32
    if (n == 0) { /* WSAPoll rejects empty sets */
        Sleep((DWORD)(timeout_ms < 0 ? 0 : timeout_ms));
        return 0;
    }
    return WSAPoll(fds, (ULONG)n, timeout_ms);
#else
    return poll(fds, (nfds_t)n, timeout_ms);
#endif
}

void ip_format(uint32_t ip, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool ip_parse(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        unsigned n = 0;
        int nd = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (++nd > 3 || n > 255) return false;
        }
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

static void addr_v4(struct sockaddr_in *a, uint32_t ip, int port) {
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = htonl(ip);
}

bool listen_addr(const char *host, uint32_t *ip) {
    *ip = 0; /* every address */
    if (!host || !*host || strcmp(host, "*") == 0) return true;
    if (str_ieq(host, "localhost")) {
        *ip = 0x7F000001;
        return true;
    }
    return ip_parse(host, ip);
}

sock_t tcp_listen(const char *host, int port, char *err, size_t errcap) {
    uint32_t ip = 0;
    if (!listen_addr(host, &ip)) {
        snprintf(err, errcap, "bad listen address '%s' (use an IPv4 address)", host);
        return SOCK_BAD;
    }
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD) {
        snprintf(err, errcap, "socket() failed (%d)", sock_errno());
        return SOCK_BAD;
    }
    int one = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    struct sockaddr_in a;
    addr_v4(&a, ip, port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        snprintf(err, errcap, "cannot bind %s:%d (error %d) - port in use or not permitted", host && *host ? host : "0.0.0.0",
                 port, sock_errno());
        sock_close(s);
        return SOCK_BAD;
    }
    if (listen(s, 128) != 0) {
        snprintf(err, errcap, "listen() failed (%d)", sock_errno());
        sock_close(s);
        return SOCK_BAD;
    }
    sock_nonblock(s, true);
    return s;
}

int tcp_local_port(sock_t s) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    if (getsockname(s, (struct sockaddr *)&a, &al) != 0) return 0;
    return ntohs(a.sin_port);
}

sock_t tcp_accept(sock_t listener, uint32_t *peer_ip) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    sock_t s = accept(listener, (struct sockaddr *)&a, &al);
    if (s == SOCK_BAD) return SOCK_BAD;
    sock_nonblock(s, true);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    if (peer_ip) *peer_ip = a.sin_family == AF_INET ? ntohl(a.sin_addr.s_addr) : 0;
    return s;
}

sock_t tcp_connect_nb(uint32_t ip, int port) {
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCK_BAD) return s;
    sock_nonblock(s, true);
    struct sockaddr_in a;
    addr_v4(&a, ip, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0 && !sock_again(sock_errno())) {
        sock_close(s);
        return SOCK_BAD;
    }
    return s;
}

bool tcp_connect_done(sock_t s) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return false;
    return err == 0;
}

sock_t udp_open(uint16_t port, bool broadcast) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) return s;
    int one = 1;
    if (broadcast) setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    if (port) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    addr_v4(&a, 0, port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        sock_close(s);
        return SOCK_BAD;
    }
    sock_nonblock(s, true);
    return s;
}

sock_t udp_open_loopback(uint16_t *port) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == SOCK_BAD) return s;
    struct sockaddr_in a;
    addr_v4(&a, 0x7F000001, 0);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        sock_close(s);
        return SOCK_BAD;
    }
    sock_nonblock(s, true);
    *port = (uint16_t)tcp_local_port(s);
    return s;
}

long udp_send(sock_t s, uint32_t ip, uint16_t port, const void *p, size_t n) {
    struct sockaddr_in a;
    addr_v4(&a, ip, port);
#ifdef _WIN32
    return sendto(s, (const char *)p, (int)n, 0, (struct sockaddr *)&a, sizeof a);
#else
    return (long)sendto(s, p, n, 0, (struct sockaddr *)&a, sizeof a);
#endif
}

long udp_recv(sock_t s, void *p, size_t cap, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
#ifdef _WIN32
    long n = recvfrom(s, (char *)p, (int)cap, 0, (struct sockaddr *)&a, &al);
#else
    long n = (long)recvfrom(s, p, cap, 0, (struct sockaddr *)&a, &al);
#endif
    if (n >= 0) {
        if (ip) *ip = ntohl(a.sin_addr.s_addr);
        if (port) *port = ntohs(a.sin_port);
    }
    return n;
}

bool udp_join_multicast(sock_t s, uint32_t group) {
    struct ip_mreq m;
    memset(&m, 0, sizeof m);
    m.imr_multiaddr.s_addr = htonl(group);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m) == 0;
}

/* ============================================================= randomness */

void rand_bytes(void *out, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        log_error("BCryptGenRandom failed");
        abort();
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        ssize_t r = read(fd, (char *)out + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        got += (size_t)r;
    }
    if (fd >= 0) close(fd);
    if (got != n) {
        log_error("cannot read /dev/urandom");
        abort();
    }
#endif
}

void rand_hex(char *out, size_t nbytes) {
    unsigned char tmp[64];
    if (nbytes > sizeof tmp) nbytes = sizeof tmp;
    rand_bytes(tmp, nbytes);
    hex_encode(tmp, nbytes, out);
}

/* ================================================================== files */

#ifdef _WIN32
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static bool narrow(const wchar_t *w, char *out, size_t cap) {
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

static int64_t filetime_unix(FILETIME ft) {
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((v - 116444736000000000ULL) / 10000000ULL);
}
#endif

FILE *file_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wm[8];
    int i = 0;
    bool has_b = false;
    for (; mode[i] && i < 6; i++) {
        wm[i] = (wchar_t)mode[i];
        if (mode[i] == 'b') has_b = true;
    }
    if (!has_b) wm[i++] = L'b'; /* never translate newlines */
    wm[i] = 0;
    wchar_t *wp = widen(path);
    if (!wp) return NULL;
    FILE *f = _wfopen(wp, wm);
    free(wp);
    return f;
#else
    return fopen(path, mode);
#endif
}

bool file_seek(FILE *f, int64_t offset) {
    if (offset < 0) return false;
#ifdef _WIN32
    return _fseeki64(f, offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

bool file_pread(FILE *f, int64_t offset, void *dst, size_t n) {
    return file_seek(f, offset) && fread(dst, 1, n, f) == n;
}

bool file_read_all(const char *path, Buf *out, size_t max_bytes) {
    FILE *f = file_open(path, "rb");
    if (!f) return false;
    char tmp[16384];
    size_t n;
    bool ok = true;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (out->len + n > max_bytes) {
            ok = false;
            break;
        }
        buf_put(out, tmp, n);
    }
    if (ferror(f)) ok = false;
    fclose(f);
    return ok;
}

bool file_sync(FILE *f) {
    if (fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

bool file_replace(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *a = widen(from), *b = widen(to);
    bool ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok;
#else
    return rename(from, to) == 0;
#endif
}

bool file_write_atomic(const char *path, const void *data, size_t n) {
    Buf tmp = {0};
    buf_printf(&tmp, "%s.tmp", path);
    FILE *f = file_open(buf_cstr(&tmp), "wb");
    bool ok = f != NULL;
    if (ok) {
        ok = fwrite(data, 1, n, f) == n;
        ok = file_sync(f) && ok;
        ok = fclose(f) == 0 && ok;
    }
    ok = ok && file_replace(buf_cstr(&tmp), path);
    if (!ok) file_remove(buf_cstr(&tmp));
    buf_free(&tmp);
    return ok;
}

bool file_write_private(const char *path, const void *data, size_t n) {
#ifdef _WIN32
    return file_write_atomic(path, data, n); /* inherits the folder's permissions */
#else
    /* A settings file that is a link (into a config folder, a backup) keeps being one: the
       file it points to gets the new contents, not a fresh file in place of the link (issue #12). */
    char *real = realpath(path, NULL);
    if (real) path = real;
    Buf tmp = {0};
    buf_printf(&tmp, "%s.tmp", path);
    remove(buf_cstr(&tmp));
    int fd = open(buf_cstr(&tmp), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); /* private from the first byte */
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (fd >= 0 && !f) close(fd);
    bool ok = f != NULL;
    if (ok) {
        ok = fwrite(data, 1, n, f) == n;
        ok = file_sync(f) && ok;
        ok = fclose(f) == 0 && ok;
    }
    ok = ok && file_replace(buf_cstr(&tmp), path);
    if (!ok) remove(buf_cstr(&tmp));
    buf_free(&tmp);
    free(real);
    return ok;
#endif
}

bool dir_remove(const char *path) {
#ifdef _WIN32
    wchar_t *w = widen(path);
    bool ok = w && _wrmdir(w) == 0;
    free(w);
    return ok;
#else
    return rmdir(path) == 0;
#endif
}

bool file_remove(const char *path) {
#ifdef _WIN32
    wchar_t *w = widen(path);
    bool ok = w && _wremove(w) == 0;
    free(w);
    return ok;
#else
    return remove(path) == 0;
#endif
}

bool path_stat(const char *path, int64_t *size, int64_t *mtime, bool *is_dir) {
#ifdef _WIN32
    wchar_t *w = widen(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    bool ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return false;
    if (size) *size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (mtime) *mtime = filetime_unix(d.ftLastWriteTime);
    if (is_dir) *is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (int64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
#endif
}

bool path_is_dir(const char *path) {
    bool d = false;
    return path_stat(path, NULL, NULL, &d) && d;
}

bool path_disk_space(const char *path, int64_t *free_bytes, int64_t *total_bytes) {
#ifdef _WIN32
    wchar_t *w = widen(path);
    ULARGE_INTEGER avail, total;
    bool ok = w && GetDiskFreeSpaceExW(w, &avail, &total, NULL);
    free(w);
    if (ok && free_bytes) *free_bytes = (int64_t)avail.QuadPart;
    if (ok && total_bytes) *total_bytes = (int64_t)total.QuadPart;
    return ok;
#else
    struct statvfs st;
    if (statvfs(path, &st) != 0) return false;
    if (free_bytes) *free_bytes = (int64_t)st.f_bavail * (int64_t)st.f_frsize; /* what an unprivileged user may still use */
    if (total_bytes) *total_bytes = (int64_t)st.f_blocks * (int64_t)st.f_frsize;
    return true;
#endif
}

bool path_free_space(const char *path, int64_t *free_bytes) { return path_disk_space(path, free_bytes, NULL); }

long file_sendfile(sock_t s, FILE *f, int64_t off, size_t n) {
#ifdef __linux__
    off_t o = (off_t)off;
    ssize_t k = sendfile(s, fileno(f), &o, n);
    if (k > 0) return (long)k;
    if (k == 0) return -1; /* the file ended early */
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
#else
    (void)s, (void)f, (void)off, (void)n;
    return -2;
#endif
}

static bool mkdir_one(const char *p) {
    if (path_is_dir(p)) return true;
#ifdef _WIN32
    wchar_t *w = widen(p);
    bool ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

bool dir_make(const char *path) {
    size_t n = strlen(path);
    if (!n) return false;
    char *p = xstrdup(path);
    bool ok = true;
    for (size_t i = 1; i <= n && ok; i++) {
        if (p[i] == '/' || p[i] == '\\' || p[i] == '\0') {
            char c = p[i];
            p[i] = '\0';
            bool drive = (i == 2 && p[1] == ':'); /* "C:" */
            if (!drive) ok = mkdir_one(p);
            p[i] = c;
        }
    }
    free(p);
    return ok;
}

/* ------------------------------------------------------------ directories */

#ifdef _WIN32
struct DirIter {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first;
};

DirIter *dir_open(const char *path) {
    Buf pat = {0};
    buf_puts(&pat, path);
    if (pat.len && pat.p[pat.len - 1] != '/' && pat.p[pat.len - 1] != '\\') buf_putc(&pat, '\\');
    buf_putc(&pat, '*');
    wchar_t *w = widen(buf_cstr(&pat));
    buf_free(&pat);
    if (!w) return NULL;
    DirIter *it = xcalloc(1, sizeof *it);
    it->h = FindFirstFileExW(w, FindExInfoBasic, &it->fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(w);
    if (it->h == INVALID_HANDLE_VALUE) {
        free(it);
        return NULL;
    }
    it->first = true;
    return it;
}

bool dir_next(DirIter *it, DirEntry *e) {
    for (;;) {
        if (!it->first && !FindNextFileW(it->h, &it->fd)) return false;
        it->first = false;
        const wchar_t *nm = it->fd.cFileName;
        if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
        if (it->fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!narrow(nm, e->name, sizeof e->name)) continue;
        e->is_dir = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->is_link = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e->size = ((int64_t)it->fd.nFileSizeHigh << 32) | it->fd.nFileSizeLow;
        e->mtime = filetime_unix(it->fd.ftLastWriteTime);
        return true;
    }
}

void dir_close(DirIter *it) {
    if (!it) return;
    FindClose(it->h);
    free(it);
}
#else
struct DirIter {
    DIR *d;
    char *path;
};

DirIter *dir_open(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    DirIter *it = xcalloc(1, sizeof *it);
    it->d = d;
    it->path = xstrdup(path);
    return it;
}

bool dir_next(DirIter *it, DirEntry *e) {
    struct dirent *de;
    while ((de = readdir(it->d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        if (strlen(nm) >= sizeof e->name) continue;
        Buf full = {0};
        buf_printf(&full, "%s/%s", it->path, nm);
        struct stat st;
        bool link = lstat(buf_cstr(&full), &st) == 0 && S_ISLNK(st.st_mode);
        bool ok = stat(buf_cstr(&full), &st) == 0;
        buf_free(&full);
        if (!ok) continue; /* dangling symlink etc. */
        str_copy(e->name, sizeof e->name, nm);
        e->is_link = link;
        e->is_dir = S_ISDIR(st.st_mode);
        e->size = (int64_t)st.st_size;
        e->mtime = (int64_t)st.st_mtime;
        return true;
    }
    return false;
}

void dir_close(DirIter *it) {
    if (!it) return;
    closedir(it->d);
    free(it->path);
    free(it);
}
#endif

/* ========================================================= threads, locks */

typedef struct {
    void (*fn)(void *);
    void *arg;
} ThreadStart;

#ifdef _WIN32
static DWORD WINAPI thread_tramp(LPVOID p) {
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    ts.fn(ts.arg);
    return 0;
}

bool thread_spawn(void (*fn)(void *), void *arg) {
    ThreadStart *ts = xmalloc(sizeof *ts);
    ts->fn = fn;
    ts->arg = arg;
    HANDLE h = CreateThread(NULL, 512 * 1024, thread_tramp, ts, 0, NULL);
    if (!h) {
        free(ts);
        return false;
    }
    CloseHandle(h);
    return true;
}

void lock_take(Lock *l) { AcquireSRWLockExclusive(&l->l); }
void lock_give(Lock *l) { ReleaseSRWLockExclusive(&l->l); }

int plat_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

void thread_background(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
#else
static void *thread_tramp(void *p) {
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    ts.fn(ts.arg);
    return NULL;
}

bool thread_spawn(void (*fn)(void *), void *arg) {
    ThreadStart *ts = xmalloc(sizeof *ts);
    ts->fn = fn;
    ts->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024); /* musl's default (128K) is tight */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &at, thread_tramp, ts);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        free(ts);
        return false;
    }
    return true;
}

int plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* Only the calling thread: nice() is per thread on Linux but per process
   elsewhere (it would slow the whole server), so macOS gets a QoS class. */
void thread_background(void) {
#if defined(__linux__)
    (void)!nice(10);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

void lock_take(Lock *l) { pthread_mutex_lock(&l->l); }
void lock_give(Lock *l) { pthread_mutex_unlock(&l->l); }
#endif
