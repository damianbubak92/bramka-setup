package com.example.gatewaycommunicatorgen2;

import android.content.Context;
import android.net.wifi.WifiManager;

import javax.net.ssl.*;
import java.io.*;
import java.net.*;
import java.security.cert.X509Certificate;
import java.util.Locale;

public final class NetworkClient {
    // --- Bramka gen2 (AM62). Gen1 jest WYŁĄCZONA na czas testów; port-forward 9443 -> .170 ---
    private static final String GATEWAY_LAN_HOST = "192.168.2.170"; // gen2 w LAN
    private static final String PUBLIC_URL = "https://91.123.191.192:9443"; // WAN -> port-forward -> gen2
    private static final String LAN_URL    = "https://" + GATEWAY_LAN_HOST + ":9443";
    public static final String AUTH_TOKEN = "c228cecbca32894a526092abd305cddc";

    // Jednorazowo – budowa pinningowego SSLSocketFactory bywa kosztowna
    private static final javax.net.ssl.SSLSocketFactory SSL;

    static {
        try {
            SSL = CertPin.getPinnedFactory();
        } catch (Exception e) {
            throw new RuntimeException(e);
        }
    }

    // Pinning zapewnia tożsamość, więc tu możemy zwracać true (ułatwia IP publiczne/LAN)
    private static final javax.net.ssl.HostnameVerifier ANY_HOST = (h, s) -> true;

    // Wybór URL: jeśli bramka gen2 odpowiada w LAN -> LAN-direct (bez hairpin NAT);
    // inaczej publiczny (dane komórkowe / spoza sieci) przez port-forward.
    // Sonda to TCP-connect na port 9443 (nie ICMP isReachable - to bywa blokowane
    // na Androidzie i fałszywie zwraca false, spychając ruch na WAN/hairpin).
    private static final String PUBLIC_HOST = "91.123.191.192"; // WAN host (port-forward 9443)

    private static String pickUrl() {
        if (lanReachable(GATEWAY_LAN_HOST, 9443, 400)) return LAN_URL;
        return PUBLIC_URL;
    }

    // WebSocket URL for the live channel (same host pick + token as pickUrl).
    public static String baseWsUrl() {
        String host = lanReachable(GATEWAY_LAN_HOST, 9443, 400) ? GATEWAY_LAN_HOST : PUBLIC_HOST;
        return "wss://" + host + ":9443/ws?token=" + AUTH_TOKEN;
    }

    private static boolean lanReachable(String host, int port, int timeoutMs) {
        try (java.net.Socket s = new java.net.Socket()) {
            s.connect(new java.net.InetSocketAddress(host, port), timeoutMs);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public static String sendCommand(Context ctx, String command) throws UnsupportedEncodingException {
        long t0 = android.os.SystemClock.elapsedRealtime();
        String baseUrl = pickUrl();
        String data = "command=" + java.net.URLEncoder.encode(command, "UTF-8")
                + "&authToken=" + AUTH_TOKEN;
        byte[] body = data.getBytes(java.nio.charset.StandardCharsets.UTF_8);

        try {
            javax.net.ssl.HttpsURLConnection.setDefaultSSLSocketFactory(SSL);
            java.net.URL url = new java.net.URL(baseUrl);
            javax.net.ssl.HttpsURLConnection conn = (javax.net.ssl.HttpsURLConnection) url.openConnection();
            conn.setHostnameVerifier(ANY_HOST);
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
            conn.setRequestProperty("Connection", "close"); // serwer i tak zamyka
            conn.setConnectTimeout(1800); // krócej – szybciej failuje hairpin
            conn.setReadTimeout(1800);
            conn.setDoOutput(true);
            conn.setFixedLengthStreamingMode(body.length); // unika chunked/100-continue
            WifiManager wm = (WifiManager) ctx.getSystemService(Context.WIFI_SERVICE);
            WifiManager.WifiLock lock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "gw:highperf");
            lock.setReferenceCounted(false);
            long t1 = android.os.SystemClock.elapsedRealtime();
            try { lock.acquire();

            conn.connect(); // start TLS zanim zaczniemy pisać

            } finally { if (lock.isHeld()) lock.release(); }
            long t2 = android.os.SystemClock.elapsedRealtime();
            try (java.io.OutputStream os = conn.getOutputStream()) {
                os.write(body);
                os.flush();
            }
            long t3 = android.os.SystemClock.elapsedRealtime();

            int code = conn.getResponseCode();
            java.io.InputStream is = (code < 400) ? conn.getInputStream() : conn.getErrorStream();
            java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(is));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            long t4 = android.os.SystemClock.elapsedRealtime();

            android.util.Log.i("NC",
                    "timing(ms) build=" + (t1-t0) + " connect(TLS)=" + (t2-t1) +
                            " write=" + (t3-t2) + " read=" + (t4-t3) + " total=" + (t4-t0) +
                            " url=" + baseUrl + " code=" + code);

            return sb.toString().trim();
        } catch (Exception e) {
            return "❌ Błąd: " + e.getMessage();
        }
    }
    // Provisioning: approve a pending JOIN. factory = hex id from listjoins; name
    // is URL-encoded (server url-decodes). Rides sendRawPost like setrules; the
    // server matches command/fields over the body. Returns the raw JSON/response.
    public static String approveJoin(Context ctx, String factory, String name) {
        String encName;
        try {
            encName = java.net.URLEncoder.encode(name, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            encName = name;
        }
        String body = "command=approvejoin"
                + "&factory=" + factory
                + "&name=" + encName
                + "&authToken=" + AUTH_TOKEN;
        return sendRawPost(ctx, body);
    }

    // Provisioning: remove (unprovision) a node by its assigned address. The
    // gateway tells the node to drop its identity and deletes its DB row.
    public static String removeNode(Context ctx, int address) {
        String body = "command=removenode"
                + "&address=" + address
                + "&authToken=" + AUTH_TOKEN;
        return sendRawPost(ctx, body);
    }

    public static String sendRawPost(Context ctx, String body) {
        try {
            // Pinning ustala tożsamość serwera; URL wybieramy jak w sendCommand:
            // LAN-direct gdy gen2 w sieci, inaczej publiczny (port-forward). Dzięki
            // temu setrules też działa lokalnie bez hairpin NAT.
            HttpsURLConnection.setDefaultSSLSocketFactory(SSL);

            URL url = new URL(pickUrl());
            HttpsURLConnection conn = (HttpsURLConnection) url.openConnection();
            conn.setHostnameVerifier(ANY_HOST);

            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
            conn.setRequestProperty("Connection", "close");
            conn.setConnectTimeout(3000);
            conn.setReadTimeout(3000);
            conn.setDoOutput(true);

            byte[] bytes = body.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            conn.setFixedLengthStreamingMode(bytes.length); // uniknij chunked

            conn.connect(); // start TLS przed zapisem

            try (OutputStream os = conn.getOutputStream()) {
                os.write(bytes);
                os.flush();
            }

            int code = conn.getResponseCode();
            InputStream is = (code < 400) ? conn.getInputStream() : conn.getErrorStream();

            BufferedReader reader = new BufferedReader(new InputStreamReader(is, java.nio.charset.StandardCharsets.UTF_8));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) sb.append(line).append('\n');

            return sb.toString().trim();
        } catch (Exception e) {
            return "❌ Błąd: " + e.getMessage();
        }
    }

}