# 0004 - Plain http, inside the home network

**Status:** accepted · **Since:** 2026-10-11

## Context

Hemma is reached by its address on the home network (`http://192.168.1.50`, `hemma.local`).
Proper HTTPS needs a domain and a certificate the devices trust: a public domain with Let's
Encrypt, a home certificate authority installed on every phone and TV, or a proxy in front. Each
is real work for a household, and older TV browsers handle little of it.

## Decision

- Hemma speaks plain `http://` and is meant to be reached only from inside the home. No port
  forwarding; from outside, use a VPN ([docs/home-network.md](../home-network.md)).
- What plain http can't protect is limited instead: admin sessions end after 30 days however
  much they're used, sign-in tries are throttled before any password is hashed, the server only
  answers to private host names (a DNS-rebinding guard), and media addresses carry keys bound to
  the device that asked.

## Consequences

- Anyone on the same network who can read its traffic could see a session cookie. That is the
  risk a household accepts by sharing its Wi-Fi; the README and SECURITY.md say so.
- Browsers that try `https://` first get no answer: people are told to type `http://`.
- HTTPS can come later (mbedTLS was weighed) without changing anything else.
