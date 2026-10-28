/* Net.java's rules, checked with a JDK alone (apps/android/build.sh and build.ps1 run it before building the app):
 *   javac -d out src/io/github/kloparn/hemma/Net.java test/NetTest.java && java -cp out NetTest */
import io.github.kloparn.hemma.Net;
import java.util.List;

public class NetTest {
    static int failed, checks;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failed++;
            System.out.println("FAILED: " + what);
        }
    }

    static void eq(Object got, Object want, String what) {
        check(want == null ? got == null : want.equals(got), what + ": got " + got + ", want " + want);
    }

    static byte[] v6(int... b) {
        byte[] a = new byte[16];
        for (int i = 0; i < b.length; i++) a[i] = (byte) b[i];
        return a;
    }

    public static void main(String[] args) {
        /* the home network, and nothing else */
        check(Net.isPrivate(Net.ipv4("192.168.0.238")), "192.168.x");
        check(Net.isPrivate(Net.ipv4("10.1.2.3")), "10.x");
        check(Net.isPrivate(Net.ipv4("172.16.0.1")) && Net.isPrivate(Net.ipv4("172.31.255.1")), "172.16-31");
        check(!Net.isPrivate(Net.ipv4("172.32.0.1")) && !Net.isPrivate(Net.ipv4("172.15.0.1")), "not 172.32 / 172.15");
        check(Net.isPrivate(Net.ipv4("127.0.0.1")) && Net.isPrivate(Net.ipv4("169.254.3.4")), "loopback, link-local");
        check(!Net.isPrivate(Net.ipv4("8.8.8.8")) && !Net.isPrivate(Net.ipv4("100.64.0.1")), "the internet, CGNAT");
        check(Net.isPrivate(v6(0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1)) && Net.isPrivate(v6(0xfd, 0x12)), "fe80::, fd..");
        byte[] one = new byte[16];
        one[15] = 1;
        check(Net.isPrivate(one) && !Net.isPrivate(v6(0x20, 0x01, 0x0d, 0xb8)), "::1 yes, 2001:db8:: no");
        check(Net.ipv4("1.2.3") == null && Net.ipv4("1.2.3.256") == null && Net.ipv4("a.b.c.d") == null, "not addresses");

        /* names a home network gives */
        check(Net.isHomeName("smarthome") && Net.isHomeName("smarthome.local") && Net.isHomeName("pi.home.arpa"), "home names");
        check(Net.isHomeName("hemma.lan") && Net.isHomeName("box.internal") && Net.isHomeName("smarthome.local."), "more");
        check(!Net.isHomeName("example.com") && !Net.isHomeName("evil.local.example.com") && !Net.isHomeName(""), "not");

        /* what someone typed */
        eq(Net.typed("192.168.0.238"), "http://192.168.0.238", "an address");
        eq(Net.typed(" http://192.168.0.238/ "), "http://192.168.0.238", "with http:// and /");
        eq(Net.typed("smarthome.local:8080"), "http://smarthome.local:8080", "a name and a port");
        eq(Net.typed("SmartHome:80"), "http://smarthome", "port 80 left out");
        eq(Net.typed("[fe80::1]:8080"), "http://[fe80::1]:8080", "IPv6 link-local");
        eq(Net.typed("https://192.168.0.238"), null, "https");
        eq(Net.typed("8.8.8.8"), null, "the internet");
        eq(Net.typed("example.com"), null, "a name on the internet");
        eq(Net.typed("192.168.0.238/media"), null, "a path");
        eq(Net.typed("user@192.168.0.238"), null, "a user name");
        eq(Net.typed("192.168.0.238:99999"), null, "a port too big");
        eq(Net.typed("1.2.3"), null, "numbers that aren't an address");
        eq(Net.typed("[2001:db8::1]"), null, "IPv6 on the internet");
        eq(Net.base("192.168.0.238", 80), "http://192.168.0.238", "base, port 80");
        eq(Net.base("fe80::1", 8080), "http://[fe80::1]:8080", "base, IPv6");

        /* the Wi-Fi's addresses, nearest first */
        List<String> n = Net.neighbours(Net.ipv4("192.168.0.69"), 24);
        eq(n.size(), 253, "a /24 less the phone");
        check(!n.contains("192.168.0.69") && !n.contains("192.168.0.0") && !n.contains("192.168.0.255"), "no self, net, broadcast");
        eq(n.get(0), "192.168.0.1", "the router's end first");
        check(n.indexOf("192.168.0.68") < 4 && n.indexOf("192.168.0.70") < 4, "then the phone's neighbours");
        check(n.contains("192.168.0.238") && n.contains("192.168.0.254"), "the rest");
        eq(Net.neighbours(Net.ipv4("10.0.0.5"), 8).size(), 253, "a huge network: its /24 only");
        eq(Net.neighbours(Net.ipv4("10.0.4.5"), 22).size(), 1021, "a /22");

        /* Hemma's answer */
        Net.Info i = Net.parse("{\"hemma\":\"2.0.0\",\"id\":\"0123456789abcdef\",\"name\":\"Bj\\u00f6rk \\\"by the lake\\\"\"}\n");
        check(i != null, "an answer");
        if (i != null) {
            eq(i.version, "2.0.0", "version");
            eq(i.id, "0123456789abcdef", "id");
            eq(i.name, "Björk \"by the lake\"", "name, unescaped");
        }
        eq(Net.parse("{\"ok\":true}"), null, "not Hemma");
        eq(Net.parse("{\"hemma\":\"2.0.0\",\"id\":\"nope\"}"), null, "a bad id");
        eq(Net.parse("<html>sign in</html>"), null, "a page");
        Net.Info nameless = Net.parse("{ \"hemma\" : \"2.0.0\" , \"id\" : \"0123456789abcdef\" }");
        check(nameless != null && nameless.name.equals("Hemma"), "spaces, no name");

        System.out.println(failed == 0 ? "NetTest: all " + checks + " checks passed" : "NetTest: " + failed + " of " + checks + " FAILED");
        System.exit(failed == 0 ? 0 : 1);
    }
}
