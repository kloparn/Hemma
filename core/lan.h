/* core/lan.h — LAN helpers for device discovery, presence and Wake-on-LAN.
 *
 * MAC addresses, the primary IPv4 interface, the OS neighbour (ARP) cache,
 * ARP-refreshing probes, magic packets and non-blocking reverse DNS (PTR).
 * Nothing here blocks or keeps state: the caller owns the sockets and drives
 * everything from its event loop. Addresses are IPv4 in host byte order. */
#ifndef HEMMA_LAN_H
#define HEMMA_LAN_H

#include "plat.h"

/* ---- MAC addresses ---- */
typedef struct {
    uint8_t b[6];
} Mac;

/* Accepts "aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF" (1-2 hex digits per group,
 * one consistent separator), "aabbccddeeff" and Cisco "aabb.ccdd.eeff".
 * Surrounding whitespace is ignored. */
bool mac_parse(const char *s, Mac *m);
void mac_format(const Mac *m, char out[18]); /* lower-case, colons */
bool mac_eq(const Mac *a, const Mac *b);
bool mac_is_zero(const Mac *m);
bool mac_is_multicast(const Mac *m); /* group bit; includes broadcast ff:ff:.. */
/* Locally administered unicast address: what phones use as their private /
 * randomised Wi-Fi address (stable per network on iOS/Android by default). */
bool mac_is_random(const Mac *m);

/* ---- the interface we live on ---- */
typedef struct {
    char name[64];                         /* interface (friendly) name, UTF-8 */
    uint32_t ip, mask, gateway, broadcast; /* gateway 0 if unknown */
    Mac mac;                               /* zero if unknown */
} LanIface;

/* The IPv4 interface holding the default route; falls back to the first
 * private (RFC 1918) non-loopback address. Never sends packets. */
bool lan_iface(LanIface *out);
bool lan_in_subnet(const LanIface *ifc, uint32_t ip);
bool lan_is_private(uint32_t ip); /* 10/8, 172.16/12, 192.168/16 */

/* Hosts worth sweeping: the subnet minus network/broadcast addresses, shrunk
 * to the block around our own address while it holds more than max_hosts
 * (max_hosts 254 on a /16 gives our /24). Returns the host count (0 = none);
 * our own address is inside the range — skip it when probing. */
int lan_sweep_range(const LanIface *ifc, uint32_t *first, uint32_t *last, int max_hosts);

/* ---- the OS neighbour (ARP) cache ---- */
typedef enum { NEIGH_UNKNOWN, NEIGH_REACHABLE, NEIGH_STALE, NEIGH_FAILED } NeighState;
typedef struct {
    uint32_t ip;
    Mac mac; /* zero for NEIGH_FAILED entries that never resolved */
    NeighState state;
} Neighbor;

/* Reads the IPv4 neighbour cache of all interfaces (filter with
 * lan_in_subnet). Skips multicast/broadcast/zero IPs and multicast/zero MACs,
 * except that unresolved (incomplete/unreachable) entries are reported as
 * NEIGH_FAILED: "something was probed there and nobody answered".
 * Linux (/proc/net/arp) and macOS (the routing table's ARP entries) have no
 * reachable/stale distinction: every complete entry is NEIGH_REACHABLE.
 * Elsewhere (other Unixes) the list is empty. Returns the number written. */
int lan_neighbors(Neighbor *out, int max);
const char *neigh_state_name(NeighState s);

/* Pure parsers for the Linux /proc files, exposed for tests. */
int lan_parse_proc_arp(const char *text, Neighbor *out, int max);
/* Default route with the lowest metric: interface name and gateway
 * (0 if the route has no gateway). */
bool lan_parse_proc_route(const char *text, char ifname[64], uint32_t *gateway);

/* Pure parser for macOS routing-socket records (its routing table and ARP
 * cache come from sysctl as rt_msghdr records), exposed for tests. `addrs` is
 * the record's rtm_addrs bitmask, p/n the sockaddrs after the header: one per
 * bit (destination, gateway, netmask, ...), each padded to 4 bytes, a zero
 * length taking 4. Fills the IPv4 destination, gateway and netmask (0 when
 * absent or not IPv4) and, when the gateway is a 6-byte link-layer address
 * (an ARP entry), its MAC. False when the record is cut short. */
typedef struct {
    uint32_t dst, gateway, mask;
    Mac mac;
    bool has_mac;
} RtAddrs;
bool lan_parse_rt_addrs(const uint8_t *p, size_t n, int addrs, RtAddrs *out);

/* ---- probing and waking ---- */
/* UDP socket for probes / WoL / DNS (bound to an ephemeral port). On Windows
 * it also disables the "ICMP port unreachable resets the next recvfrom"
 * quirk, which probing port 9 would otherwise trigger. */
sock_t lan_udp_open(bool broadcast);
/* 1-byte datagram to ip:9 (discard) so the OS resolves/refreshes the ARP
 * entry; read lan_neighbors() a few seconds later. Non-blocking. */
bool lan_probe(sock_t udp, uint32_t ip);
size_t lan_wol_packet(const Mac *m, uint8_t out[102]);
/* Magic packet to broadcast:9 (if given) and 255.255.255.255:9. The socket
 * needs SO_BROADCAST (lan_udp_open(true)). True if any send succeeded. */
bool lan_wake(sock_t udp_broadcast, const Mac *m, uint32_t broadcast);

/* ---- reverse DNS without blocking ----
 * Build a PTR query for `ip`; send it with udp_send() to the router:53, or to
 * 224.0.0.251:5353 with mdns=true (RD clear, QU bit set: unicast reply).
 * Returns the packet length (0 if cap is too small; 46 bytes always fit). */
size_t dns_ptr_query(uint32_t ip, uint16_t id, bool mdns, uint8_t *out, size_t cap);
/* Parse a DNS/mDNS response. True when it carries a PTR record for an
 * x.y.z.w.in-addr.arpa name: `ip` is that address, `name` the PTR target
 * without trailing dot and without a home-network suffix (.local .lan .home
 * .localdomain .home.arpa .fritz.box .internal). Control characters become
 * '?'. Bounds- and loop-safe on hostile input. */
bool dns_ptr_reply(const uint8_t *pkt, size_t n, uint16_t *id, uint32_t *ip, char *name, size_t cap);

#endif
