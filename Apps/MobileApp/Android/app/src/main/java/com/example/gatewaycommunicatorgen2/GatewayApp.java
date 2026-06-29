package com.example.gatewaycommunicatorgen2;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

/**
 * Drives the single app-scoped {@link GatewayWs} live connection by foreground
 * state: the socket is up only while at least one Activity is started (app in
 * foreground), and torn down when the app goes to background. Standard
 * started-activity counting - no extra lifecycle dependency.
 */
public class GatewayApp extends Application {

    private int startedActivities = 0;

    @Override
    public void onCreate() {
        super.onCreate();
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            @Override public void onActivityStarted(@NonNull Activity activity) {
                if (startedActivities++ == 0) {
                    Log.i("GatewayApp", "foreground -> WS connect");
                    GatewayWs.get().connect();   // app entered foreground
                }
            }
            @Override public void onActivityStopped(@NonNull Activity activity) {
                if (--startedActivities <= 0) {
                    startedActivities = 0;
                    Log.i("GatewayApp", "background -> WS disconnect");
                    GatewayWs.get().disconnect(); // app went to background
                }
            }
            @Override public void onActivityCreated(@NonNull Activity a, @Nullable Bundle b) {}
            @Override public void onActivityResumed(@NonNull Activity a) {}
            @Override public void onActivityPaused(@NonNull Activity a) {}
            @Override public void onActivitySaveInstanceState(@NonNull Activity a, @NonNull Bundle b) {}
            @Override public void onActivityDestroyed(@NonNull Activity a) {}
        });
    }
}
