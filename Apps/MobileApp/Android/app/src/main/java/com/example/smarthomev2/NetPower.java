// NetPower.java
package com.example.smarthomev2;

import android.content.Context;
import android.net.wifi.WifiManager;

public final class NetPower {
    private NetPower() {}

    /** Uchwyt do Wi-Fi high-perf locka; zamyka się sam w try-with-resources. */
    public static final class WifiHighPerfLock implements AutoCloseable {
        private final WifiManager.WifiLock lock;

        WifiHighPerfLock(WifiManager.WifiLock lock) {
            this.lock = lock;
        }

        @Override public void close() {
            try {
                if (lock != null && lock.isHeld()) lock.release();
            } catch (Throwable ignore) {}
        }
    }

    /** Aktywuje WIFI_MODE_FULL_HIGH_PERF na czas operacji sieciowej. */
    public static WifiHighPerfLock acquireWifiHighPerf(Context ctx) {
        WifiManager wm = (WifiManager) ctx.getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);

        WifiManager.WifiLock wl = null;
        if (wm != null) {
            try {
                wl = wm.createWifiLock(
                        WifiManager.WIFI_MODE_FULL_HIGH_PERF,
                        "Gateway:WiFiHighPerf");
                wl.setReferenceCounted(false);
                wl.acquire();
            } catch (Throwable ignore) {
                wl = null; // fallback: nic nie robimy
            }
        }
        return new WifiHighPerfLock(wl);
    }
}