package com.example.smarthomev2;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.NonNull;

import org.json.JSONObject;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;

/**
 * Singleton live channel to the gateway (WebSocket over the pinned TLS :9443).
 * App-scoped: connect when the app is in foreground, disconnect in background
 * (driven by {@link GatewayApp}). Push-only for now (server->app events); client
 * ->server commands with reqId come in Phase B.
 *
 * Listener callbacks are delivered on the MAIN thread, so UI code is safe.
 */
public final class GatewayWs {

    private static final String TAG = "GatewayWs";
    private static final long RECONNECT_MS = 3000;

    public interface Listener {
        default void onWsState(boolean connected) {}
        default void onJoinPending(String factory, int nodeType) {}
        default void onNodeStatus(int address, String status) {}
        default void onTelemetry(int address, int nodeType, JSONObject params, long ts) {}
    }

    private static final GatewayWs INSTANCE = new GatewayWs();
    public static GatewayWs get() { return INSTANCE; }
    private GatewayWs() {}

    private final List<Listener> listeners = new CopyOnWriteArrayList<>();
    private final Handler main = new Handler(Looper.getMainLooper());

    private OkHttpClient client;
    private WebSocket ws;
    private boolean want;       // app wants the socket up (foreground)
    private boolean connecting; // an open attempt is in flight
    private boolean connected;

    public void addListener(Listener l) { if (!listeners.contains(l)) listeners.add(l); }
    public void removeListener(Listener l) { listeners.remove(l); }
    public boolean isConnected() { return connected; }

    /** Bring the socket up (idempotent). Called when the app enters foreground. */
    public synchronized void connect() {
        want = true;
        if (ws != null || connecting) return;
        connecting = true;
        // baseWsUrl() does a blocking TCP probe -> MUST be off the main thread
        // (else NetworkOnMainThreadException, which is exactly what kept the WS
        // from ever reaching the gateway).
        new Thread(this::openSocket, "ws-connect").start();
    }

    /** Tear the socket down. Called when the app goes to background. */
    public synchronized void disconnect() {
        want = false;
        main.removeCallbacksAndMessages(null);
        if (ws != null) {
            ws.close(1000, "app background");
            ws = null;
        }
    }

    private void openSocket() {
        try {
            OkHttpClient c;
            synchronized (this) {
                if (client == null) {
                    client = new OkHttpClient.Builder()
                            .sslSocketFactory(CertPin.getPinnedFactory(), CertPin.getPinnedTrustManager())
                            .hostnameVerifier((h, s) -> true) // pinning is the real identity check
                            .pingInterval(20, TimeUnit.SECONDS)
                            .connectTimeout(8, TimeUnit.SECONDS)
                            .build();
                }
                c = client;
            }
            String url = NetworkClient.baseWsUrl();
            Log.i(TAG, "connecting " + url);
            Request req = new Request.Builder().url(url).build();
            WebSocket w = c.newWebSocket(req, new WebSocketListener() {
                @Override public void onOpen(@NonNull WebSocket w, @NonNull Response r) {
                    Log.i(TAG, "open");
                    setConnected(true);
                }
                @Override public void onMessage(@NonNull WebSocket w, @NonNull String text) {
                    handle(text);
                }
                @Override public void onFailure(@NonNull WebSocket w, @NonNull Throwable t, Response r) {
                    Log.w(TAG, "failure: " + t.getMessage());
                    onDown();
                }
                @Override public void onClosed(@NonNull WebSocket w, int code, @NonNull String reason) {
                    Log.i(TAG, "closed: " + reason);
                    onDown();
                }
            });
            synchronized (this) {
                ws = w;
                connecting = false;
                if (!want) { // backgrounded while connecting -> tear down
                    ws.close(1000, "app background");
                    ws = null;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "openSocket failed: " + e.getMessage());
            synchronized (this) { connecting = false; }
            onDown();
        }
    }

    private synchronized void onDown() {
        setConnected(false);
        ws = null;
        if (want && !connecting) main.postDelayed(this::reconnect, RECONNECT_MS);
    }

    private void reconnect() {
        connect(); // re-enters the off-main connect path (no-op if already up/connecting)
    }

    private void setConnected(boolean c) {
        connected = c;
        main.post(() -> { for (Listener l : listeners) l.onWsState(c); });
    }

    private void handle(String text) {
        try {
            JSONObject o = new JSONObject(text);
            String type = o.optString("type", "");
            switch (type) {
                case "join_pending": {
                    String factory = o.optString("factory", "");
                    int nodeType = o.optInt("nodeType", -1);
                    main.post(() -> { for (Listener l : listeners) l.onJoinPending(factory, nodeType); });
                    break;
                }
                case "node_status": {
                    int addr = o.optInt("address", -1);
                    String status = o.optString("status", "");
                    main.post(() -> { for (Listener l : listeners) l.onNodeStatus(addr, status); });
                    break;
                }
                case "telemetry": {
                    int addr = o.optInt("address", -1);
                    int nodeType = o.optInt("nodeType", -1);
                    JSONObject params = o.optJSONObject("params");
                    long ts = o.optLong("ts", 0);
                    main.post(() -> { for (Listener l : listeners) l.onTelemetry(addr, nodeType, params, ts); });
                    break;
                }
                default:
                    Log.d(TAG, "unknown event: " + type);
            }
        } catch (Exception e) {
            Log.w(TAG, "bad event json: " + e.getMessage());
        }
    }
}
