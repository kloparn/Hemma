/* The app's rules about addresses (2.0), in plain Java - no Android here, so test/NetTest.java checks them with
 * a JDK alone: which addresses are the home network (the app connects to nothing else), the addresses of the
 * Wi-Fi to look through, an address someone typed, and Hemma's answer to GET /hemma.json. */
package io.github.kloparn.hemma;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class Net {
    private Net() {}

    /** What GET /hemma.json says: this is Hemma, which version, which one (its id) and the household's name. */
    public static final class Info {
        public final String version, id, name;

        Info(String version, String id, String name) {
            this.version = version;
            this.id = id;
            this.name = name;
        }
    }

    /** An address on the home network: 10/8, 172.16/12, 192.168/16, 169.254/16 (link-local), 127/8; IPv6
     *  fe80::/10, fc00::/7, ::1. Never the internet, never a carrier's shared 100.64/10. */
    public static boolean isPrivate(byte[] a) {
        if (a == null) return false;
        if (a.length == 4) {
            int b0 = a[0] & 0xff, b1 = a[1] & 0xff;
            return b0 == 10 || b0 == 127 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168)
                || (b0 == 169 && b1 == 254);
        }
        if (a.length == 16) {
            boolean loop = true;
            for (int i = 0; i < 15; i++) loop &= a[i] == 0;
            if (loop && a[15] == 1) return true;
            int b0 = a[0] & 0xff, b1 = a[1] & 0xff;
            return (b0 == 0xfe && (b1 & 0xc0) == 0x80) || (b0 & 0xfe) == 0xfc;
        }
        return false;
    }

    /** "192.168.0.38" as bytes; null when it isn't four numbers 0-255 */
    public static byte[] ipv4(String s) {
        String[] p = s.split("\\.", -1);
        if (p.length != 4) return null;
        byte[] out = new byte[4];
        for (int i = 0; i < 4; i++) {
            if (p[i].isEmpty() || p[i].length() > 3) return null;
            for (char ch : p[i].toCharArray()) if (ch < '0' || ch > '9') return null;
            int v = Integer.parseInt(p[i]);
            if (v > 255) return null;
            out[i] = (byte) v;
        }
        return out;
    }

    /** A name a home network gives (a router's or mDNS): one word, or ending .local .lan .home .home.arpa
     *  .internal - it still has to lead to a private address (isPrivate) before the app connects. */
    public static boolean isHomeName(String host) {
        String h = host.toLowerCase(Locale.ROOT);
        if (h.endsWith(".")) h = h.substring(0, h.length() - 1);
        if (h.isEmpty() || h.startsWith("-")) return false;
        for (char ch : h.toCharArray())
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '-' && ch != '.') return false;
        if (h.indexOf('.') < 0) return true;
        return h.endsWith(".local") || h.endsWith(".lan") || h.endsWith(".home") || h.endsWith(".home.arpa")
            || h.endsWith(".internal");
    }

    /** The addresses to ask on a network ip/prefix (the phone's own left out). Wider than /22 (1,022 addresses)
     *  only the /24 around the phone. IPv4 only. */
    public static List<String> neighbours(byte[] ip, int prefix) {
        List<String> out = new ArrayList<>();
        if (ip == null || ip.length != 4) return out;
        if (prefix < 22 || prefix > 30) prefix = 24;
        long me = ((ip[0] & 0xffL) << 24) | ((ip[1] & 0xffL) << 16) | ((ip[2] & 0xffL) << 8) | (ip[3] & 0xffL);
        long mask = (0xffffffffL << (32 - prefix)) & 0xffffffffL, net = me & mask, last = net | (~mask & 0xffffffffL);
        /* nearest first: the router and a Pi usually sit close to the phone's own number - or near the start */
        List<Long> all = new ArrayList<>();
        for (long a = net + 1; a < last; a++) if (a != me) all.add(a);
        all.sort((x, y) -> {
            long dx = Math.min(Math.abs(x - me), x - net), dy = Math.min(Math.abs(y - me), y - net);
            return dx != dy ? Long.compare(dx, dy) : Long.compare(x, y);
        });
        for (long a : all) out.add(((a >> 24) & 0xff) + "." + ((a >> 16) & 0xff) + "." + ((a >> 8) & 0xff) + "." + (a & 0xff));
        return out;
    }

    /** "http://host[:port]" for what someone typed ("192.168.0.38", "smarthome.local:8080", "http://hemma/..."),
     *  or null: https, a user name, a path that isn't "/", or a host that isn't the home network's */
    public static String typed(String s) {
        if (s == null) return null;
        s = s.trim();
        String low = s.toLowerCase(Locale.ROOT);
        if (low.startsWith("http://")) s = s.substring(7);
        else if (low.contains("://")) return null;
        int slash = s.indexOf('/');
        if (slash >= 0) {
            if (!s.substring(slash).equals("/")) return null;
            s = s.substring(0, slash);
        }
        if (s.isEmpty() || s.contains("@")) return null;
        String host = s, port = "";
        if (s.startsWith("[")) { /* [fe80::1]:8080 */
            int end = s.indexOf(']');
            if (end < 0) return null;
            host = s.substring(1, end);
            String rest = s.substring(end + 1);
            if (!rest.isEmpty() && !rest.startsWith(":")) return null;
            port = rest.isEmpty() ? "" : rest.substring(1);
            if (host.indexOf(':') < 0) return null;
        } else if (s.indexOf(':') >= 0) {
            if (s.indexOf(':') != s.lastIndexOf(':')) return null; /* (a bare IPv6 address needs its brackets) */
            host = s.substring(0, s.indexOf(':'));
            port = s.substring(s.indexOf(':') + 1);
        }
        if (!port.isEmpty()) {
            if (port.length() > 5) return null;
            for (char ch : port.toCharArray()) if (ch < '0' || ch > '9') return null;
            int p = Integer.parseInt(port);
            if (p < 1 || p > 65535) return null;
        }
        if (host.indexOf(':') >= 0) { /* IPv6: link-local, unique-local or loopback only */
            return host.toLowerCase(Locale.ROOT).matches("(fe[89ab][0-9a-f]|f[cd][0-9a-f]{2}):[0-9a-f:%a-z.]*|::1")
                ? url("[" + host + "]", port) : null;
        }
        byte[] v4 = ipv4(host);
        if (v4 != null) return isPrivate(v4) ? url(host, port) : null;
        if (host.matches("[0-9.]+")) return null; /* (numbers that aren't an address) */
        return isHomeName(host) ? url(host.toLowerCase(Locale.ROOT), port) : null;
    }

    private static String url(String host, String port) {
        return "http://" + host + (port.isEmpty() || port.equals("80") ? "" : ":" + port);
    }

    /** "http://host:port" for a found address and port */
    public static String base(String host, int port) {
        String h = host.indexOf(':') >= 0 ? "[" + host + "]" : host;
        return "http://" + h + (port == 80 ? "" : ":" + port);
    }

    /** Hemma's answer to GET /hemma.json; null when it isn't one (no "hemma" and "id") */
    public static Info parse(String json) {
        if (json == null) return null;
        String v = field(json, "hemma"), id = field(json, "id"), name = field(json, "name");
        if (v == null || id == null || !id.matches("[0-9a-f]{16}")) return null;
        return new Info(v, id, name == null ? "Hemma" : name);
    }

    /* a string field of a flat JSON object - all /hemma.json holds */
    static String field(String json, String key) {
        String q = "\"" + key + "\"";
        int at = 0;
        while ((at = json.indexOf(q, at)) >= 0) {
            int i = at + q.length();
            while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
            if (i < json.length() && json.charAt(i) == ':') {
                i++;
                while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
                if (i >= json.length() || json.charAt(i) != '"') return null;
                StringBuilder sb = new StringBuilder();
                for (i++; i < json.length(); i++) {
                    char ch = json.charAt(i);
                    if (ch == '"') return sb.toString();
                    if (ch != '\\') {
                        sb.append(ch);
                        continue;
                    }
                    if (++i >= json.length()) return null;
                    char e = json.charAt(i);
                    switch (e) {
                        case 'n': sb.append('\n'); break;
                        case 't': sb.append('\t'); break;
                        case 'r': sb.append('\r'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'u':
                            if (i + 4 >= json.length()) return null;
                            try {
                                sb.append((char) Integer.parseInt(json.substring(i + 1, i + 5), 16));
                            } catch (NumberFormatException x) {
                                return null;
                            }
                            i += 4;
                            break;
                        default: sb.append(e);
                    }
                }
                return null;
            }
            at = i;
        }
        return null;
    }
}
