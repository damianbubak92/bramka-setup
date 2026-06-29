package com.example.smarthomev2;

import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

public final class NetworkProbe {
    private static final String TAG = "PROBE_DIAG";

    private NetworkProbe(){}

    public static String probeOnce(Context ctx, String host, int port, boolean doHttp) {
        long t0 = System.nanoTime();
        try {
            // 0) Pobierz (z cache!) pinned factory
            long tCtx0 = System.nanoTime();
            SSLSocketFactory f = CertPin.getPinnedFactory();
            long tCtx1 = System.nanoTime();

            // 1) TCP connect
            long tTcp0 = System.nanoTime();
            Socket tcp = new Socket();
            tcp.setTcpNoDelay(true);
            tcp.connect(new InetSocketAddress(host, port), 2000); // connect timeout
            long tTcp1 = System.nanoTime();

            // 2) TLS handshake
            long tTls0 = System.nanoTime();
            SSLSocket ssl = (SSLSocket) f.createSocket(tcp, host, port, true);
            // Wymuś tylko TLSv1.2 na próbie (możesz skomentować, jeśli nie chcesz ograniczać)
            ssl.setEnabledProtocols(new String[]{"TLSv1.2"});
            ssl.startHandshake();
            long tTls1 = System.nanoTime();

            // 3) Opcjonalnie bardzo krótki request (żeby nie wchodzić w autoryzację)
            long tWr0 = System.nanoTime();
            long tWr1 = tWr0;
            long tRd0 = tTls1;
            long tRd1 = tTls1;
            int code = -1;

            if (doHttp) {
                // Minimalne POST z małym body – serwer i tak odda 401/404 szybko
                String body = "command=ping&authToken=invalid";
                String req =
                        "POST / HTTP/1.1\r\n" +
                                "Host: " + host + ":" + port + "\r\n" +
                                "Content-Type: application/x-www-form-urlencoded\r\n" +
                                "Content-Length: " + body.length() + "\r\n" +
                                "Connection: close\r\n\r\n" +
                                body;

                OutputStream os = ssl.getOutputStream();
                os.write(req.getBytes());
                os.flush();
                tWr1 = System.nanoTime();

                BufferedReader br = new BufferedReader(new InputStreamReader(ssl.getInputStream()));
                String status = br.readLine(); // np. "HTTP/1.1 401 Unauthorized"
                if (status != null && status.startsWith("HTTP/1.1 ")) {
                    try { code = Integer.parseInt(status.substring(9, 12)); } catch (Exception ignore) {}
                }
                // nie czytamy dalej – zamykamy
                tRd1 = System.nanoTime();
            }

            try { ssl.close(); } catch (Throwable ignore) {}

            long t1 = System.nanoTime();

            long ctxMs = (tCtx1 - tCtx0) / 1_000_000;
            long tcpMs = (tTcp1 - tTcp0) / 1_000_000;
            long tlsMs = (tTls1 - tTls0) / 1_000_000;
            long wrMs  = (tWr1  - tWr0 ) / 1_000_000;
            long rdMs  = (tRd1  - tTls1) / 1_000_000; // od końca handshake

            String line = String.format(
                    "timing(ms) ctx=%d tcpConnect=%d tlsHandshake=%d write=%d read=%d total=%d url=https://%s:%d code=%d",
                    ctxMs, tcpMs, tlsMs, wrMs, rdMs, (t1 - t0)/1_000_000, host, port, code);

            Log.i(TAG, line);
            return line;
        } catch (Throwable e) {
            String msg = (e.getMessage() == null) ? e.toString() : e.getMessage();
            Log.i(TAG, "probe error: " + msg);
            Log.i(TAG, Log.getStackTraceString(e)); // pełny stacktrace
            return "probe error: " + msg;
        }
    }
}