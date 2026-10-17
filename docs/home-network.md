# Hemma and your home network

Hemma lives on your home network, the one your router makes with its Wi-Fi and cables.
This page covers the network side: finding the server, giving it an address that never
changes, names versus numbers, why the address starts with `http://`, what the Network
page can and can't tell you, and how to reach Hemma from outside without putting it on
the internet.

The examples use `192.168.1.50` for the server and `192.168.1.1` for the router. Yours
will be different, but they usually look similar (`192.168.0.x`, `192.168.1.x` or
`10.0.0.x`).

## Find the server's address

- **Hemma tells you.** When it starts it logs a line like
  `Hemma is up on http://192.168.1.50:8080`. On a Pi or Linux server:
  `journalctl -u hemma | grep "is up on"`.
- **On the server itself:** `hostname -I` (Linux, Pi), `ipconfig` → *IPv4 Address*
  (Windows), `ipconfig getifaddr en0` (Mac).
- **In your router:** its list of connected devices shows each device's name and address.

The port comes after the colon. The Pi installer uses port 80, which is the default, so
you can leave it out: `http://192.168.1.50`. Elsewhere Hemma uses 8080:
`http://192.168.1.50:8080`.

## Give the server a fixed address

The router hands out addresses and may give the server a new one after a restart. Then
bookmarks on the TV and phones stop working. Fix that once, in the router, by
*reserving* the server's current address for it:

1. **Open the router's admin page.** Its address is your network's "default gateway":
   `ipconfig` on Windows (*Default Gateway*), `ip route` on Linux (`default via
   192.168.1.1`), `route -n get default` on a Mac. It's often printed on a sticker on the
   router, together with the admin password.
2. **Find the DHCP settings.** The name varies by brand: *DHCP reservation*, *address
   reservation*, *static lease*, *fixed IP*, or *always use the same IP address*. A few
   examples (menus differ between models and versions):
   - FRITZ!Box: *Home Network → Network*, edit the device, tick *Always assign this
     network device the same IPv4 address*.
   - ASUS: *LAN → DHCP Server → Manually assigned IP*.
   - TP-Link: *Advanced → Network → DHCP Server → Address Reservation*.
   - UniFi: open the client → *Settings* → *Fixed IP Address*.
   - OpenWrt: *Network → DHCP and DNS → Static Leases*.
   - Mesh systems are usually set up in their phone app, under advanced network settings.
3. **Pick the server** from the device list (by its name, such as `hemma`, or its MAC
   address), keep the address it has, and save.
4. Restart the server (`sudo reboot`) so it picks up the reservation.

It's better to do this in the router than to type a fixed address into the server
itself. The router then knows about it and won't give the same address to someone else.

## Names: hemma.local or the IP address

There are three ways to reach the server:

| Address | Works on | Notes |
|---|---|---|
| `http://192.168.1.50` | everything | Always works. Best for TVs and bookmarks, once the address is fixed |
| `http://hemma.local` | iPhone, iPad, Mac, Windows, Linux, most recent Android phones | The server announces its own name (mDNS, also called Bonjour or Avahi) |
| `http://hemma` | mostly Windows PCs | Phones usually can't look up bare names |

- **`.local` names** come from the server itself. Raspberry Pi OS does this out of the
  box. On other Linux systems install `avahi-daemon`. A Mac does it built-in (its name is
  under *System Settings → General → Sharing → Local hostname*).
- **Some routers give devices names too**, with their own ending: `hemma.fritz.box`,
  `hemma.lan` or `hemma.home`. Hemma accepts those.
- **TVs** often can't look up `.local` names. Use the IP address on the TV and add it to
  the TV browser's bookmarks or home screen. That's why the fixed address matters.

## Why plain http://

Hemma speaks plain `http://` and never `https://`, on purpose:

- `https://` needs a certificate that every phone and TV trusts. Those are issued for
  internet names, not for `hemma.local` or `192.168.1.50`. A home-made certificate would
  make every browser show a scary warning.
- Hemma's traffic never leaves your home. The Wi-Fi password keeps people outside out,
  but it doesn't hide Hemma's traffic from the devices you let in: anyone who knows the
  Wi-Fi password can read it, and any device on the network could listen in. Treat
  everyone you let onto your Wi-Fi as being "in the house", and give visitors the guest
  network.

What this means in practice:

- **Type `http://` in front of the address.** Many TVs and some phones try `https://`
  first and then say "invalid response", "can't connect" or "page not found". Hemma
  notices these attempts and logs a hint (*"tried https:// - Hemma speaks plain http"*).
- **Chrome's warning before `http://` sites.** Since version 154 (October 2026), Chrome's
  *Always use secure connections* is on by default and asks before loading a *public*
  website over `http://`. Addresses on your own network don't count: the IP address
  (`http://192.168.1.50`), a plain name (`http://hemma`) and `.local` names open without
  a question. Chrome does ask, once, if someone switched it to warn about *all* insecure
  sites, and it may for a router-given name with a real internet ending such as
  `hemma.fritz.box`. On your own home server that's expected: choose **Continue to site**.
  Chrome doesn't ask again for a site you visit regularly.
- **Other browsers that prefer `https://`** may show a "not secure" page first too. The
  same goes: it's your own server, so continue.
- **Never put Hemma on the open internet.** For access from outside, see
  [below](#reaching-hemma-from-outside-the-safe-way).

## Other names (HEMMA_HOSTS)

Hemma only answers when it's addressed by an IP address, a plain name (`hemma`,
`raspberrypi`) or a home-network name. Those end in `.local`, `.lan`, `.home`,
`.home.arpa`, `.internal`, `.localdomain`, `.localhost`, `.intranet`, `.private`,
`.corp` or `.fritz.box`. This blocks a trick called *DNS rebinding*, which lets a website
on the internet talk to devices in your home through your own browser.

If you reach Hemma by another name, for example your own domain pointed at the server, or
a VPN name like `hemma.example-tailnet.ts.net`, the browser shows *"Hemma doesn't answer
to this host name"*. Add the name to the settings file:

```sh
HEMMA_HOSTS=hemma.example.com, .ts.net
```

A name starting with a dot allows every name under it. Restart Hemma afterwards
(`sudo systemctl restart hemma` on a Pi or Linux server).

## The Network page: devices and who's home

The *Network* page lists the devices Hemma has seen on your network. By itself Hemma only
reads the server's own list of neighbours, which sends nothing. When an admin turns on
*Discovery*, Hemma also sends a tiny packet to each address on your own network every two
minutes, so quiet phones show up too, and asks the router and the devices for their names.

- **Who's home is a good guess, not proof.** Once an admin has said whose phone is whose,
  a person counts as home while their phone is seen on the network. A phone left at home
  says "home", a phone with Wi-Fi off says "away". And nothing on a home network can prove
  which device is which: any device on it can borrow another one's hardware address (MAC)
  and look like someone's phone. Don't let locks, alarms or anything else important depend
  on it.
- **Names** come from the router or from the device itself, and a device can call itself
  anything. Hemma only takes a name as the answer to its own question, from the router or
  from the device at that address, but the names that matter are best set by an admin
  (the edit button on each device).
- **The list keeps up to 1,024 devices.** When it's full, Hemma forgets the unnamed devices
  it hasn't seen for the longest; devices someone opens Hemma from go last. Devices someone
  named, gave an owner, picked a kind or wrote a note for, or set up for Wake-on-LAN always
  stay, and so does the router (the one device at the router's address right now). If the
  list is full of those (or of devices on the network right now), new devices are left out
  until an admin forgets some; the log says so once.
- **Cleaning up:** once some unknown devices haven't been seen for a week, admins get a
  *Forget … unknown devices not seen for a week* button under the list. It leaves everything
  the household set up alone, and any device that comes back simply shows up again.
- **New devices** show up in the latest news as one line an hour at most ("12 new devices
  on the network"); more arriving within the hour are added to that line, and open screens
  get a pop-up about it at most once an hour. Lots at once is unusual on a home network:
  have a look at the *Unknown* filter on the Network page.
- **Long lists** show 100 devices at a time. *Show 100 more* is at the bottom, and the
  filters at the top (*People's*, *Unknown*) narrow the list down.

## Keep it inside your home

- **Never forward a port to Hemma** on your router. Look for *port forwarding*, *virtual
  server* or *DMZ* in its settings and make sure none of them point at the server.
- Hemma has a sign-in on every page, limits repeated password guesses and limits each
  device's connections. Still, it's built for a trusted home network, not to face the
  internet.
- A guest Wi-Fi usually can't see the home network. That's a feature: guests can't
  reach Hemma.

## Reaching Hemma from outside, the safe way

Use a **VPN**: an encrypted tunnel that puts your phone "back home" wherever you are.
Hemma then works exactly as it does at home, and nothing about it is exposed to the
internet. A few ways to set one up:

- **Your router's own VPN.** Many routers have one built in (FRITZ!Box, ASUS, UniFi,
  OpenWrt and others). Choose WireGuard if it's offered. You usually scan a QR code with
  the WireGuard app on the phone.
- **Tailscale** (built on WireGuard, free for personal use) on the server and on your
  phones. On a Pi or Linux server:

  ```sh
  curl -fsSL https://tailscale.com/install.sh | sh
  sudo tailscale up
  ```

  Sign in with the same account on the phone's Tailscale app. Then open the server by its
  Tailscale address (`http://100.x.y.z`) or its Tailscale name. For the name, add
  `.ts.net` to [HEMMA_HOSTS](#other-names-hemma_hosts).
- **Your own WireGuard server** on the Pi (tools such as PiVPN help). This needs one UDP
  port forwarded to *WireGuard*, which is fine: it's the VPN that faces the internet,
  never Hemma.
