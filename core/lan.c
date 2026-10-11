/* core/lan.c — see lan.h. Approved platform file: holds the Windows
 * (iphlpapi), Linux (/proc, /sys, getifaddrs) and macOS (routing-table
 * sysctl, getifaddrs) code paths for LAN discovery. */
#include "lan.h"

#include <limits.h>

#ifdef _WIN32
#include <iphlpapi.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <ifaddrs.h>
#include <net/if.h>
#ifdef __APPLE__
#include <net/route.h>
#include <sys/sysctl.h>
#endif
#endif

/* ===================================================================== MAC */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool mac_parse(const char *s, Mac *m) {
    if (!s || !m) return false;
    while (is_space((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && is_space((unsigned char)s[n - 1])) n--;
    Mac r = {0};

    if (!memchr(s, ':', n) && !memchr(s, '-', n)) { /* aabbccddeeff / aabb.ccdd.eeff */
        if (n != 12 && n != 14) return false;
        int k = 0;
        for (size_t i = 0; i < n; i++) {
            if (n == 14 && (i == 4 || i == 9)) {
                if (s[i] != '.') return false;
                continue;
            }
            int v = hexval((unsigned char)s[i]);
            if (v < 0) return false;
            if (k % 2 == 0) r.b[k / 2] = (uint8_t)(v << 4);
            else r.b[k / 2] |= (uint8_t)v;
            k++;
        }
        if (k != 12) return false;
        *m = r;
        return true;
    }

    /* six groups of 1-2 hex digits with one consistent separator */
    char sep = 0;
    size_t i = 0;
    for (int g = 0; g < 6; g++) {
        int digits = 0, v = 0;
        while (i < n && hexval((unsigned char)s[i]) >= 0 && digits < 3) {
            v = v * 16 + hexval((unsigned char)s[i]);
            digits++, i++;
        }
        if (digits < 1 || digits > 2) return false;
        r.b[g] = (uint8_t)v;
        if (g < 5) {
            if (i >= n || (s[i] != ':' && s[i] != '-')) return false;
            if (!sep) sep = s[i];
            else if (s[i] != sep) return false;
            i++;
        }
    }
    if (i != n) return false;
    *m = r;
    return true;
}

void mac_format(const Mac *m, char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m->b[0], m->b[1], m->b[2], m->b[3], m->b[4], m->b[5]);
}

bool mac_eq(const Mac *a, const Mac *b) { return memcmp(a->b, b->b, 6) == 0; }

bool mac_is_zero(const Mac *m) {
    for (int i = 0; i < 6; i++)
        if (m->b[i]) return false;
    return true;
}

bool mac_is_multicast(const Mac *m) { return (m->b[0] & 0x01) != 0; }

bool mac_is_random(const Mac *m) { return (m->b[0] & 0x02) && !(m->b[0] & 0x01); }

/* ============================================================== addresses */

bool lan_is_private(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 /* 172.16/12 */ || (ip >> 16) == 0xC0A8;
}

bool lan_in_subnet(const LanIface *ifc, uint32_t ip) {
    return ifc && ifc->ip && ip && (ip & ifc->mask) == (ifc->ip & ifc->mask);
}

int lan_sweep_range(const LanIface *ifc, uint32_t *first, uint32_t *last, int max_hosts) {
    if (!ifc || !ifc->ip || max_hosts <= 0) return 0;
    uint32_t mask = ifc->mask;
    for (;;) {
        uint32_t net = ifc->ip & mask, bc = net | ~mask;
        if (bc - net < 2) return 0; /* /31, /32: nothing to sweep */
        uint32_t hosts = bc - net - 1;
        if (hosts <= (uint32_t)max_hosts) {
            *first = net + 1;
            *last = bc - 1;
            return (int)hosts;
        }
        mask = (mask >> 1) | 0x80000000u;
    }
}

static uint32_t prefix_mask(unsigned bits) {
    if (bits == 0) return 0;
    if (bits >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - bits);
}

/* An IP that can never be a neighbour worth reporting. */
static bool bad_neighbor_ip(uint32_t ip) { return ip == 0 || ip == 0xFFFFFFFFu || (ip >> 28) == 0xE; }

static bool keep_neighbor(uint32_t ip, const Mac *m, NeighState st) {
    if (bad_neighbor_ip(ip)) return false;
    if (st == NEIGH_FAILED) return true;
    return !mac_is_zero(m) && !mac_is_multicast(m);
}

const char *neigh_state_name(NeighState s) {
    switch (s) {
    case NEIGH_REACHABLE: return "reachable";
    case NEIGH_STALE: return "stale";
    case NEIGH_FAILED: return "failed";
    default: return "unknown";
    }
}

/* ======================================================= /proc parsers */

/* Split on blanks in place; returns token count. */
static int split_ws(char *s, char **tok, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s) break;
        tok[n++] = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++;
        if (*s) *s++ = '\0';
    }
    return n;
}

/* Strict hex (optional 0x), 1..8 digits. */
static bool parse_hex32(const char *s, uint32_t *out) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = strlen(s);
    if (n < 1 || n > 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int h = hexval((unsigned char)s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

/* Copy the next line of *text into buf (without the newline). Lines that do
 * not fit are skipped. Returns false at the end of the text. */
static bool next_line(const char **text, char *buf, size_t cap) {
    while (*text && **text) {
        const char *s = *text;
        const char *eol = strchr(s, '\n');
        size_t len = eol ? (size_t)(eol - s) : strlen(s);
        *text = eol ? eol + 1 : NULL;
        if (len < cap) {
            memcpy(buf, s, len);
            buf[len] = '\0';
            return true;
        }
    }
    return false;
}

int lan_parse_proc_arp(const char *text, Neighbor *out, int max) {
    int n = 0;
    char line[512];
    if (!out || max <= 0) return 0;
    while (n < max && next_line(&text, line, sizeof line)) {
        char *tok[8];
        int nt = split_ws(line, tok, 8);
        uint32_t ip, flags;
        Mac m;
        /* IP address, HW type, Flags, HW address, Mask, Device */
        if (nt < 4 || !ip_parse(tok[0], &ip) || !parse_hex32(tok[2], &flags) || !mac_parse(tok[3], &m)) continue;
        NeighState st = (flags & 0x6) ? NEIGH_REACHABLE : NEIGH_FAILED; /* ATF_COM | ATF_PERM */
        if (keep_neighbor(ip, &m, st)) out[n++] = (Neighbor){ip, m, st};
    }
    return n;
}

bool lan_parse_proc_route(const char *text, char ifname[64], uint32_t *gateway) {
    bool found = false;
    unsigned long best = ULONG_MAX;
    char line[512];
    while (next_line(&text, line, sizeof line)) {
        char *tok[12];
        int nt = split_ws(line, tok, 12);
        uint32_t dst, gw, flags, mask;
        long long metric;
        /* Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT */
        if (nt < 8 || !parse_hex32(tok[1], &dst) || !parse_hex32(tok[2], &gw) || !parse_hex32(tok[3], &flags) ||
            !str_int(tok[6], &metric) || metric < 0 || !parse_hex32(tok[7], &mask))
            continue;
        if (dst != 0 || mask != 0 || !(flags & 0x1) /* RTF_UP */ || (unsigned long)metric >= best) continue;
        best = (unsigned long)metric;
        found = true;
        str_copy(ifname, 64, tok[0]);
        /* the kernel prints the in-memory (network order) word in host order */
        *gateway = (flags & 0x2) /* RTF_GATEWAY */ ? ntohl(gw) : 0;
    }
    return found;
}

/* ================================================ macOS routing records */

/* BSD sockaddrs start with a length byte and a family byte; read as bytes so
   the parser builds (and is tested) everywhere. */
#define BSD_AF_INET 2
#define BSD_AF_LINK 18

/* IPv4 address of a sockaddr_in that may be cut short (routing netmasks are:
   "5 bytes long" = only the first address byte is stored, the rest is zero). */
static uint32_t bsd_sa_v4(const uint8_t *sa, size_t len) {
    uint32_t ip = 0;
    for (size_t i = 0; i < 4; i++) ip = (ip << 8) | (4 + i < len ? sa[4 + i] : 0);
    return ip;
}

/* MAC of a sockaddr_dl: len, family, index(2), type, nlen, alen, slen, then
   the interface name (nlen bytes) followed by the address (alen bytes). */
static bool bsd_sa_mac(const uint8_t *sa, size_t len, Mac *m) {
    if (len < 8 || sa[1] != BSD_AF_LINK || sa[6] != 6) return false;
    size_t at = 8 + (size_t)sa[5];
    if (at + 6 > len) return false;
    memcpy(m->b, sa + at, 6);
    return true;
}

bool lan_parse_rt_addrs(const uint8_t *p, size_t n, int addrs, RtAddrs *out) {
    memset(out, 0, sizeof *out);
    if (!p && n) return false;
    size_t off = 0;
    for (int slot = 0; slot < 8; slot++) { /* RTA_DST, RTA_GATEWAY, RTA_NETMASK, ... in bit order */
        if (!(addrs & (1 << slot))) continue;
        if (off >= n) return false;
        size_t len = p[off];
        size_t step = len ? (len + 3) & ~(size_t)3 : 4;
        if (len > n - off) return false;
        const uint8_t *sa = p + off;
        if (slot == 0 && len >= 8 && sa[1] == BSD_AF_INET) out->dst = bsd_sa_v4(sa, len);
        if (slot == 1 && len >= 8 && sa[1] == BSD_AF_INET) out->gateway = bsd_sa_v4(sa, len);
        if (slot == 1) out->has_mac = bsd_sa_mac(sa, len, &out->mac);
        if (slot == 2) out->mask = bsd_sa_v4(sa, len); /* netmasks often carry no family */
        if (slot == 2) return true;               /* nothing further is needed */
        off += step;
    }
    return true;
}

/* ================================================ interface + neighbours */

#ifdef _WIN32

static void wide_to_utf8(const wchar_t *w, char *out, size_t cap) {
    char tmp[256];
    if (!w || WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, (int)sizeof tmp, NULL, NULL) <= 0) tmp[0] = '\0';
    str_copy(out, cap, tmp);
}

static uint32_t sa_v4(const SOCKADDR *sa) {
    if (!sa || sa->sa_family != AF_INET) return 0;
    return ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
}

static bool adapter_v4(const IP_ADAPTER_ADDRESSES *a, uint32_t *ip, uint32_t *mask) {
    for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
        uint32_t v = sa_v4(u->Address.lpSockaddr);
        if (!v || (v >> 16) == 0xA9FE /* 169.254/16 */) continue;
        *ip = v;
        *mask = prefix_mask(u->OnLinkPrefixLength);
        return true;
    }
    return false;
}

static uint32_t adapter_gateway(const IP_ADAPTER_ADDRESSES *a) {
    for (const IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next) {
        uint32_t v = sa_v4(g->Address.lpSockaddr);
        if (v) return v;
    }
    return 0;
}

bool lan_iface(LanIface *out) {
    memset(out, 0, sizeof *out);
    ULONG size = 16 * 1024, rc = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *list = NULL;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = xmalloc(size);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, list, &size);
    }
    if (rc != NO_ERROR) {
        free(list);
        return false;
    }
    /* Which interface would the OS route 1.1.1.1 through? A table lookup only. */
    DWORD best_if = 0;
    bool have_best = GetBestInterface(htonl(0x01010101), &best_if) == NO_ERROR;

    const IP_ADAPTER_ADDRESSES *pick = NULL;
    int pick_score = 0;
    ULONG pick_metric = ULONG_MAX;
    for (const IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        uint32_t ip, mask;
        if (!adapter_v4(a, &ip, &mask)) continue;
        uint32_t gw = adapter_gateway(a);
        int score = (have_best && a->IfIndex == best_if) ? 3 : gw ? 2 : lan_is_private(ip) ? 1 : 0;
        if (!score) continue;
        if (score > pick_score || (score == 2 && pick_score == 2 && a->Ipv4Metric < pick_metric)) {
            pick = a;
            pick_score = score;
            pick_metric = a->Ipv4Metric;
        }
    }
    if (pick) {
        adapter_v4(pick, &out->ip, &out->mask);
        out->gateway = adapter_gateway(pick);
        out->broadcast = out->ip | ~out->mask;
        if (pick->PhysicalAddressLength == 6) memcpy(out->mac.b, pick->PhysicalAddress, 6);
        wide_to_utf8(pick->FriendlyName, out->name, sizeof out->name);
    }
    free(list);
    return pick != NULL;
}

int lan_neighbors(Neighbor *out, int max) {
    PMIB_IPNET_TABLE2 t = NULL;
    if (!out || max <= 0 || GetIpNetTable2(AF_INET, &t) != NO_ERROR || !t) return 0;
    int n = 0;
    for (ULONG i = 0; i < t->NumEntries && n < max; i++) {
        const MIB_IPNET_ROW2 *r = &t->Table[i];
        if (r->Address.si_family != AF_INET) continue;
        uint32_t ip = ntohl(r->Address.Ipv4.sin_addr.s_addr);
        NeighState st;
        switch (r->State) {
        case NlnsReachable:
        case NlnsPermanent: st = NEIGH_REACHABLE; break;
        case NlnsStale:
        case NlnsDelay:
        case NlnsProbe: st = NEIGH_STALE; break;
        case NlnsUnreachable:
        case NlnsIncomplete: st = NEIGH_FAILED; break;
        default: st = NEIGH_UNKNOWN; break;
        }
        Mac m = {{0}};
        if (r->PhysicalAddressLength == 6) memcpy(m.b, r->PhysicalAddress, 6);
        if (keep_neighbor(ip, &m, st)) out[n++] = (Neighbor){ip, m, st};
    }
    FreeMibTable(t);
    return n;
}

#else /* POSIX: getifaddrs everywhere; the default route, MACs and neighbours from
         /proc + /sys on Linux, from the routing table (sysctl) on macOS */

#if defined(__APPLE__)

/* The kernel's IPv4 routes with `flag` set (RTF_GATEWAY, RTF_LLINFO), as
   rt_msghdr records. Caller frees; NULL when unavailable. */
static uint8_t *rt_table(int flag, size_t *len) {
    for (int tries = 0; tries < 4; tries++) {
        int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, flag};
        size_t need = 0;
        if (sysctl(mib, 6, NULL, &need, NULL, 0) != 0 || need > (8u << 20)) return NULL;
        need += need / 8 + 1024; /* room for entries added in between */
        uint8_t *b = xmalloc(need);
        if (sysctl(mib, 6, b, &need, NULL, 0) == 0) {
            *len = need;
            return b;
        }
        free(b);
        if (errno != ENOMEM) return NULL; /* ENOMEM: the table grew, ask again */
    }
    return NULL;
}

/* Calls fn for every well-formed record of the table. */
static void rt_each(int flag, void (*fn)(const struct rt_msghdr *h, const RtAddrs *a, void *ctx), void *ctx) {
    size_t len = 0;
    uint8_t *b = rt_table(flag, &len);
    if (!b) return;
    for (size_t off = 0; off + sizeof(struct rt_msghdr) <= len;) {
        struct rt_msghdr h;
        memcpy(&h, b + off, sizeof h);
        if (h.rtm_msglen < sizeof h || h.rtm_msglen > len - off) break;
        RtAddrs a;
        if (h.rtm_version == RTM_VERSION &&
            lan_parse_rt_addrs(b + off + sizeof h, h.rtm_msglen - sizeof h, h.rtm_addrs, &a))
            fn(&h, &a, ctx);
        off += h.rtm_msglen;
    }
    free(b);
}

typedef struct {
    char *ifname;
    uint32_t *gw;
    bool found;
} DefRoute;

static void on_route(const struct rt_msghdr *h, const RtAddrs *a, void *ctx) {
    DefRoute *d = ctx;
    /* the primary default route; RTF_IFSCOPE ones are per-interface extras */
    if (d->found || !(h->rtm_addrs & RTA_DST) || a->dst != 0 || (h->rtm_flags & (RTF_HOST | RTF_IFSCOPE)) ||
        !(h->rtm_flags & RTF_UP))
        return;
    char name[IF_NAMESIZE + 1] = "";
    if (!if_indextoname(h->rtm_index, name)) return;
    str_copy(d->ifname, 64, name);
    *d->gw = (h->rtm_flags & RTF_GATEWAY) ? a->gateway : 0;
    d->found = true;
}

static bool default_route(char ifname[64], uint32_t *gw) {
    DefRoute d = {ifname, gw, false};
    rt_each(RTF_GATEWAY, on_route, &d);
    return d.found;
}

static void iface_mac(const struct ifaddrs *all, const char *name, Mac *mac) {
    for (const struct ifaddrs *i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_LINK || !i->ifa_name || strcmp(i->ifa_name, name) != 0)
            continue;
        const uint8_t *sa = (const uint8_t *)i->ifa_addr;
        if (bsd_sa_mac(sa, sa[0], mac)) return;
    }
}

typedef struct {
    Neighbor *out;
    int max, n;
} NeighList;

static void on_arp(const struct rt_msghdr *h, const RtAddrs *a, void *ctx) {
    NeighList *l = ctx;
    (void)h;
    if (l->n >= l->max) return;
    Mac m = {{0}};
    if (a->has_mac) m = a->mac;
    NeighState st = mac_is_zero(&m) ? NEIGH_FAILED : NEIGH_REACHABLE; /* no MAC yet = "(incomplete)" */
    if (keep_neighbor(a->dst, &m, st)) l->out[l->n++] = (Neighbor){a->dst, m, st};
}

int lan_neighbors(Neighbor *out, int max) {
    if (!out || max <= 0) return 0;
    NeighList l = {out, max, 0};
    rt_each(RTF_LLINFO, on_arp, &l);
    return l.n;
}

#else /* Linux (other Unixes: no /proc, so no gateway, MAC or neighbours - just the address) */

static bool default_route(char ifname[64], uint32_t *gw) {
    Buf route = {0};
    bool ok = file_read_all("/proc/net/route", &route, 1 << 20) && lan_parse_proc_route(buf_cstr(&route), ifname, gw);
    buf_free(&route);
    return ok;
}

/* MAC from sysfs; aliases ("eth0:1") share the base device's address */
static void iface_mac(const struct ifaddrs *all, const char *name, Mac *mac) {
    (void)all;
    char dev[64];
    str_copy(dev, sizeof dev, name);
    char *colon = strchr(dev, ':');
    if (colon) *colon = '\0';
    Buf path = {0}, addr = {0};
    buf_printf(&path, "/sys/class/net/%s/address", dev);
    if (file_read_all(buf_cstr(&path), &addr, 64)) mac_parse(buf_cstr(&addr), mac);
    buf_free(&path);
    buf_free(&addr);
}

int lan_neighbors(Neighbor *out, int max) {
    Buf b = {0};
    int n = 0;
    if (file_read_all("/proc/net/arp", &b, 4 << 20)) n = lan_parse_proc_arp(buf_cstr(&b), out, max);
    buf_free(&b);
    return n;
}

#endif

static uint32_t netmask_v4(const struct sockaddr *sa) {
    if (!sa) return prefix_mask(24);
#ifdef __APPLE__
    /* BSD netmasks may be cut short and carry no family: read the bytes that are there */
    uint32_t m = bsd_sa_v4((const uint8_t *)sa, ((const uint8_t *)sa)[0]);
    return m ? m : prefix_mask(24);
#else
    if (sa->sa_family != AF_INET) return prefix_mask(24);
    return ntohl(((const struct sockaddr_in *)(const void *)sa)->sin_addr.s_addr);
#endif
}

bool lan_iface(LanIface *out) {
    memset(out, 0, sizeof *out);
    char defif[64] = "";
    uint32_t gw = 0;
    default_route(defif, &gw); /* leaves both empty when there is none */

    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return false;
    const struct ifaddrs *pick = NULL, *fallback = NULL;
    for (const struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t ip = ntohl(((const struct sockaddr_in *)(const void *)i->ifa_addr)->sin_addr.s_addr);
        if (!ip || (ip >> 16) == 0xA9FE) continue;
        if (defif[0] && strcmp(i->ifa_name, defif) == 0) {
            pick = i;
            break;
        }
        if (!fallback && lan_is_private(ip)) fallback = i;
    }
    bool on_default = pick != NULL;
    if (!pick) pick = fallback;
    if (pick) {
        out->ip = ntohl(((const struct sockaddr_in *)(const void *)pick->ifa_addr)->sin_addr.s_addr);
        out->mask = netmask_v4(pick->ifa_netmask);
        out->gateway = on_default ? gw : 0;
        out->broadcast = out->ip | ~out->mask;
        str_copy(out->name, sizeof out->name, pick->ifa_name);
        iface_mac(ifs, pick->ifa_name, &out->mac);
    }
    freeifaddrs(ifs);
    return pick != NULL;
}

#endif

/* ======================================================= probes, waking */

sock_t lan_udp_open(bool broadcast) {
    sock_t s = udp_open(0, broadcast);
#ifdef _WIN32
    if (s != SOCK_BAD) {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
#endif
    return s;
}

bool lan_probe(sock_t udp, uint32_t ip) {
    static const uint8_t zero = 0;
    if (udp == SOCK_BAD || bad_neighbor_ip(ip)) return false;
    return udp_send(udp, ip, 9, &zero, 1) == 1;
}

size_t lan_wol_packet(const Mac *m, uint8_t out[102]) {
    memset(out, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(out + 6 + 6 * i, m->b, 6);
    return 102;
}

bool lan_wake(sock_t udp_broadcast, const Mac *m, uint32_t broadcast) {
    uint8_t pkt[102];
    size_t n = lan_wol_packet(m, pkt);
    bool ok = false;
    if (udp_broadcast == SOCK_BAD) return false;
    if (broadcast && broadcast != 0xFFFFFFFFu && udp_send(udp_broadcast, broadcast, 9, pkt, n) == (long)n) ok = true;
    if (udp_send(udp_broadcast, 0xFFFFFFFFu, 9, pkt, n) == (long)n) ok = true;
    return ok;
}

/* ===================================================================== DNS */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

size_t dns_ptr_query(uint32_t ip, uint16_t id, bool mdns, uint8_t *out, size_t cap) {
    uint8_t q[64];
    size_t n = 0;
    uint16_t flags = mdns ? 0x0000 : 0x0100; /* RD for unicast DNS only */
    uint16_t qclass = mdns ? 0x8001 : 0x0001; /* QU bit: please answer by unicast */
    q[n++] = (uint8_t)(id >> 8), q[n++] = (uint8_t)id;
    q[n++] = (uint8_t)(flags >> 8), q[n++] = (uint8_t)flags;
    q[n++] = 0, q[n++] = 1; /* QDCOUNT */
    memset(q + n, 0, 6), n += 6;

    char name[32];
    snprintf(name, sizeof name, "%u.%u.%u.%u.in-addr.arpa", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, len);
        n += len;
        p += len;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0, q[n++] = 12; /* QTYPE PTR */
    q[n++] = (uint8_t)(qclass >> 8), q[n++] = (uint8_t)qclass;
    if (!out || n > cap) return 0;
    memcpy(out, q, n);
    return n;
}

/* Decode a (possibly compressed) name at *pos into dotted form. *pos moves
 * past the name's in-place bytes. Rejects loops, overruns and > 255 bytes. */
static bool read_name(const uint8_t *pkt, size_t n, size_t *pos, char *out, size_t cap) {
    size_t p = *pos, olen = 0, wire = 0;
    bool jumped = false;
    int hops = 0;
    for (;;) {
        if (p >= n) return false;
        uint8_t len = pkt[p];
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++hops > 16) return false;
            size_t target = ((size_t)(len & 0x3F) << 8) | pkt[p + 1];
            if (!jumped) *pos = p + 2;
            jumped = true;
            p = target;
            continue;
        }
        if (len & 0xC0) return false; /* extended label types */
        if (len == 0) {
            if (!jumped) *pos = p + 1;
            break;
        }
        if (p + 1 + len > n) return false;
        wire += (size_t)len + 1;
        if (wire > 255) return false;
        if (olen + (olen ? 1 : 0) + len >= cap) return false;
        if (olen) out[olen++] = '.';
        for (size_t i = 0; i < len; i++) {
            uint8_t c = pkt[p + 1 + i];
            out[olen++] = (char)((c < 0x20 || c == 0x7F) ? '?' : c);
        }
        p += 1 + (size_t)len;
    }
    out[olen] = '\0';
    return true;
}

/* "42.1.168.192.in-addr.arpa" -> 192.168.1.42 */
static bool arpa_ip(const char *s, uint32_t *ip) {
    unsigned v[4];
    for (int i = 0; i < 4; i++) {
        unsigned x = 0;
        int nd = 0;
        while (*s >= '0' && *s <= '9') {
            x = x * 10 + (unsigned)(*s++ - '0');
            if (++nd > 3) return false;
        }
        if (!nd || x > 255 || *s != '.') return false;
        v[i] = x;
        s++;
    }
    if (!str_ieq(s, "in-addr.arpa")) return false;
    *ip = (v[3] << 24) | (v[2] << 16) | (v[1] << 8) | v[0];
    return true;
}

static void strip_home_suffix(char *s) {
    static const char *const sfx[] = {".localdomain", ".home.arpa", ".fritz.box", ".internal",
                                      ".local",       ".lan",       ".home"};
    size_t n = strlen(s);
    while (n && s[n - 1] == '.') s[--n] = '\0';
    for (int i = 0; i < countof(sfx); i++) {
        size_t k = strlen(sfx[i]);
        if (n > k && str_ieq(s + n - k, sfx[i])) {
            s[n - k] = '\0';
            return;
        }
    }
}

bool dns_ptr_reply(const uint8_t *pkt, size_t n, uint16_t *id, uint32_t *ip, char *name, size_t cap) {
    if (!pkt || n < 12 || !name || !cap) return false;
    uint16_t flags = rd16(pkt + 2);
    if (!(flags & 0x8000) || (flags & 0x000F)) return false; /* not a response / error rcode */
    unsigned qd = rd16(pkt + 4);
    unsigned rr = (unsigned)rd16(pkt + 6) + rd16(pkt + 8) + rd16(pkt + 10);
    if (qd > 16) return false;
    if (rr > 64) rr = 64;

    size_t pos = 12;
    char owner[300], target[300];
    uint32_t qip = 0;
    bool have_qip = false;
    for (unsigned i = 0; i < qd; i++) {
        if (!read_name(pkt, n, &pos, owner, sizeof owner) || pos + 4 > n) return false;
        if (!have_qip) have_qip = arpa_ip(owner, &qip);
        pos += 4;
    }
    for (unsigned i = 0; i < rr; i++) {
        if (!read_name(pkt, n, &pos, owner, sizeof owner) || pos + 10 > n) return false;
        uint16_t type = rd16(pkt + pos), cls = rd16(pkt + pos + 2) & 0x7FFF; /* mDNS cache-flush bit */
        size_t rdlen = rd16(pkt + pos + 8);
        pos += 10;
        if (pos + rdlen > n) return false;
        if (type == 12 && cls == 1) {
            uint32_t aip = 0;
            bool ok = arpa_ip(owner, &aip);
            if (!ok && have_qip && str_iends(owner, ".in-addr.arpa")) { /* RFC 2317 CNAME chains */
                aip = qip;
                ok = true;
            }
            size_t rp = pos;
            if (ok && read_name(pkt, n, &rp, target, sizeof target) && rp <= pos + rdlen) {
                strip_home_suffix(target);
                if (target[0]) {
                    if (id) *id = rd16(pkt);
                    if (ip) *ip = aip;
                    str_copy(name, cap, target);
                    return true;
                }
            }
        }
        pos += rdlen;
    }
    return false;
}
