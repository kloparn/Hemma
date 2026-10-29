/* Looking for Hemma on the Wi-Fi the phone is on (2.0): two ways at once -
 *   - mDNS / DNS-SD: a Hemma whose computer announces "_hemma._tcp" (deploy/avahi-hemma.service, installed on a Pi);
 *   - asking every address of the Wi-Fi (its /24, at most a /22) for GET /hemma.json on ports 80 and 8080.
 * Every answer is checked: it must be Hemma's /hemma.json, from an address on the home network (Net.isPrivate).
 * Each Hemma is reported once (by its id), on the main thread; done() when the addresses are all asked. */
package io.github.kloparn.hemma;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Handler;
import android.os.Looper;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.URL;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class Finder {
    static final String SERVICE = "_hemma._tcp";
    static final int[] PORTS = {80, 8080};

    interface Listener {
        void found(String base, Net.Info info);
        void done(boolean onWifi);
    }

    private final Context ctx;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Set<String> seen = new HashSet<>();
    private ExecutorService pool;
    private NsdManager nsd;
    private NsdManager.DiscoveryListener discovery;
    private final ArrayDeque<NsdServiceInfo> toResolve = new ArrayDeque<>();
    private boolean resolving, stopped;

    Finder(Context ctx, Listener listener) {
        this.ctx = ctx.getApplicationContext();
        this.listener = listener;
    }

    /** Asks one address: Hemma's answer, or null (not Hemma, not the home network, no answer within the time) */
    static Net.Info ask(String base, int connectMs) {
        try {
            URL u = new URL(base + "/hemma.json");
            InetAddress a = InetAddress.getByName(u.getHost());
            if (!Net.isPrivate(a.getAddress())) return null; /* (a name that leads off the home network) */
            HttpURLConnection c = (HttpURLConnection) u.openConnection();
            c.setConnectTimeout(connectMs);
            c.setReadTimeout(2500);
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            try {
                if (c.getResponseCode() != 200) return null;
                InputStream in = c.getInputStream();
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[1024];
                for (int n; (n = in.read(buf)) > 0 && out.size() < 4096;) out.write(buf, 0, n);
                return Net.parse(out.toString("UTF-8"));
            } finally {
                c.disconnect();
            }
        } catch (Exception e) {
            return null;
        }
    }

    void start() {
        stop();
        stopped = false;
        seen.clear();
        pool = Executors.newFixedThreadPool(48);
        startNsd();
        scan();
    }

    void stop() {
        stopped = true;
        if (pool != null) pool.shutdownNow();
        pool = null;
        if (nsd != null && discovery != null) {
            try {
                nsd.stopServiceDiscovery(discovery);
            } catch (Exception ignored) {
            }
        }
        discovery = null;
        toResolve.clear();
        resolving = false;
    }

    private void report(String base, Net.Info info) {
        main.post(() -> {
            if (stopped || !seen.add(info.id)) return;
            listener.found(base, info);
        });
    }

    /* ---- every address of the Wi-Fi */

    private void scan() {
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        byte[] ip = null;
        int prefix = 24;
        boolean wifi = false;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities caps = cm.getNetworkCapabilities(n);
            LinkProperties lp = cm.getLinkProperties(n);
            if (caps == null || lp == null) continue;
            boolean local = caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                || caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET);
            if (!local) continue;
            for (LinkAddress la : lp.getLinkAddresses()) {
                if (la.getAddress() instanceof Inet4Address && Net.isPrivate(la.getAddress().getAddress())) {
                    ip = la.getAddress().getAddress();
                    prefix = la.getPrefixLength();
                    wifi = true;
                }
            }
            if (wifi) break;
        }
        final boolean onWifi = wifi;
        if (!wifi) { /* (the emulator's own network counts: 10.0.2.x leads to the computer at 10.0.2.2) */
            Network act = cm.getActiveNetwork();
            LinkProperties lp = act == null ? null : cm.getLinkProperties(act);
            if (lp != null)
                for (LinkAddress la : lp.getLinkAddresses())
                    if (la.getAddress() instanceof Inet4Address && Net.isPrivate(la.getAddress().getAddress())) {
                        ip = la.getAddress().getAddress();
                        prefix = la.getPrefixLength();
                    }
        }
        List<String> hosts = Net.neighbours(ip, prefix);
        if (hosts.isEmpty()) {
            main.post(() -> {
                if (!stopped) listener.done(onWifi);
            });
            return;
        }
        final ExecutorService p = pool;
        final AtomicInteger left = new AtomicInteger(hosts.size() * PORTS.length);
        for (int port : PORTS)
            for (String h : hosts) {
                final String base = Net.base(h, port);
                p.execute(() -> {
                    Net.Info info = ask(base, 700);
                    if (info != null) report(base, info);
                    if (left.decrementAndGet() == 0)
                        main.post(() -> {
                            if (!stopped) listener.done(onWifi);
                        });
                });
            }
        p.shutdown();
        new Thread(() -> { /* (a pool shut down by stop() just ends) */
            try {
                p.awaitTermination(60, TimeUnit.SECONDS);
            } catch (InterruptedException ignored) {
            }
        }).start();
    }

    /* ---- mDNS: a Hemma that announces itself (one resolve at a time: Android refuses two) */

    private void startNsd() {
        nsd = (NsdManager) ctx.getSystemService(Context.NSD_SERVICE);
        if (nsd == null) return;
        discovery = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String type, int code) {}
            @Override public void onStopDiscoveryFailed(String type, int code) {}
            @Override public void onDiscoveryStarted(String type) {}
            @Override public void onDiscoveryStopped(String type) {}
            @Override public void onServiceLost(NsdServiceInfo info) {}

            @Override public void onServiceFound(NsdServiceInfo info) {
                main.post(() -> {
                    if (stopped) return;
                    toResolve.add(info);
                    resolveNext();
                });
            }
        };
        try {
            nsd.discoverServices(SERVICE, NsdManager.PROTOCOL_DNS_SD, discovery);
        } catch (Exception e) {
            discovery = null;
        }
    }

    @SuppressWarnings("deprecation") /* (resolveService: the one way on every Android from 8 on) */
    private void resolveNext() {
        if (resolving || stopped || toResolve.isEmpty()) return;
        resolving = true;
        NsdServiceInfo next = toResolve.poll();
        nsd.resolveService(next, new NsdManager.ResolveListener() {
            @Override public void onResolveFailed(NsdServiceInfo info, int code) {
                main.post(() -> {
                    resolving = false;
                    resolveNext();
                });
            }

            @Override public void onServiceResolved(NsdServiceInfo info) {
                InetAddress host = info.getHost();
                int port = info.getPort();
                main.post(() -> {
                    resolving = false;
                    resolveNext();
                });
                if (host == null || !Net.isPrivate(host.getAddress()) || pool == null) return;
                final String base = Net.base(host.getHostAddress(), port);
                try {
                    pool.execute(() -> {
                        Net.Info i = ask(base, 2000);
                        if (i != null) report(base, i);
                    });
                } catch (Exception ignored) { /* (the scan finished and shut the pool: ask on a thread of its own) */
                    new Thread(() -> {
                        Net.Info i = ask(base, 2000);
                        if (i != null) report(base, i);
                    }).start();
                }
            }
        });
    }
}
