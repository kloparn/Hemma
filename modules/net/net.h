/* modules/net/net.h — internals of the network module, shared with its tests.
 * Nothing outside modules/net should include this. */
#ifndef HEMMA_NET_H
#define HEMMA_NET_H

#include "../../core/app.h"
#include "../../core/lan.h"

/* A device ever seen on the home network, keyed by MAC address. */
typedef struct {
    int id;
    char mac[18]; /* "aa:bb:cc:dd:ee:ff" */
    char name[48];
    int owner; /* person id, 0 = shared / unknown */
    char kind[16]; /* emoji; empty = guess from the host name */
    int64_t first_seen, last_seen;
    char last_ip[16];
    char hostname[64]; /* from reverse DNS / mDNS */
    bool random_mac;   /* locally administered: a phone's private Wi-Fi address */
    bool wol;          /* show a Wake-on-LAN button */
    char note[120];
    int used_by; /* person who last opened Hemma from this device (owner suggestion) */
    /* memory only (not columns) */
    bool online;
    bool dirty; /* last_seen changed since the last write */
    int64_t ptr_asked;
} Device;

extern Table T_device;

#define NET_ONLINE_WINDOW 180 /* seconds since last sighting that still count as online */
#define NET_SCAN_EVERY 120    /* seconds between discovery sweeps */

void net_set_iface(const LanIface *ifc); /* tests: pretend we live on this interface */
/* Fold one round of neighbour-cache entries into the device list. `probed` =
 * the round right after a discovery sweep (only those announce new devices). */
void net_ingest(const Neighbor *nb, int n, int64_t now, bool probed);
void net_name_ip(uint32_t ip, const char *hostname); /* a reverse-DNS answer arrived */
/* A parsed PTR reply from `from`:`port` for `ip`: applied (true) only when that address was
   asked about in the last minute and the sender may answer for it (router DNS, or mDNS from
   the device itself). */
bool net_ptr_answer(uint32_t from, uint16_t port, uint16_t id, uint32_t ip, const char *name, int64_t now);
void net_learn(int person, uint32_t ip, int64_t now); /* this person browses from that IP */
int net_guess(uint32_t ip);                          /* presence guesser: owner of the device at ip */
bool net_online(const Device *d, int64_t now);
bool net_scan_on(void);
Device *net_by_mac(const char *mac);
const char *net_label(const Device *d);
const char *net_kind(const Device *d); /* chosen or guessed emoji */

#endif
