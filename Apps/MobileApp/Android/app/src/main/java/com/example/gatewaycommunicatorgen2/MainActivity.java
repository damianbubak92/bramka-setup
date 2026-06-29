package com.example.gatewaycommunicatorgen2;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.*;
import androidx.appcompat.app.AppCompatActivity;

import java.io.UnsupportedEncodingException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity {

    private EditText commandInput;
    private Button sendButton;
    private Button automationRulesButton;
    private Button manageDevicesButton;
    private Button pumpOnButton;
    private Button pumpOffButton;
    private TextView responseOutput;
    private final ExecutorService netExec = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        commandInput = findViewById(R.id.commandInput);
        sendButton = findViewById(R.id.sendButton);
        pumpOnButton = findViewById(R.id.pumpOnButton);
        pumpOffButton = findViewById(R.id.pumpOffButton);
        responseOutput = findViewById(R.id.responseOutput);
        automationRulesButton = findViewById(R.id.openAutomationRulesButton);
        manageDevicesButton = findViewById(R.id.manageDevicesButton);

        pumpOnButton.setOnClickListener(v -> {
            new Thread(() -> {
                String response = null;
                try {
                    response = NetworkClient.sendCommand(getApplicationContext(),"PUMP_ON");
                } catch (UnsupportedEncodingException e) {
                    throw new RuntimeException(e);
                }
                String finalResponse = "Pump On: " + response;
                runOnUiThread(() -> responseOutput.setText(finalResponse));
            }).start();
        });

        pumpOffButton.setOnClickListener(v -> {
            new Thread(() -> {
                String response = null;
                try {
                    response = NetworkClient.sendCommand(getApplicationContext(),"PUMP_OFF");
                } catch (UnsupportedEncodingException e) {
                    throw new RuntimeException(e);
                }
                String finalResponse = "Pump Off: " + response;
                runOnUiThread(() -> responseOutput.setText(finalResponse));
            }).start();
        });

        sendButton.setOnClickListener(v -> {
            /*
            String command = commandInput.getText().toString();
            if (!command.isEmpty()) {
                new Thread(() -> {
                    String response = null;
                    try {
                        response = NetworkClient.sendCommand(getApplicationContext(),command);
                    } catch (UnsupportedEncodingException e) {
                        throw new RuntimeException(e);
                    }
                    String finalResponse = response;
                    runOnUiThread(() -> responseOutput.setText(finalResponse));
                }).start();
            } */
            netExec.execute(() -> {
                // (opcjonalnie) high-perf Wi-Fi lock na czas pomiaru
                try (NetPower.WifiHighPerfLock ignored = NetPower.acquireWifiHighPerf(getApplicationContext())) {

                    String res = NetworkProbe.probeOnce(
                            getApplicationContext(),
                            "91.123.191.192",   // Twój host
                            9443,              // Twój port
                            false              // doHttp=false na pierwszy rzut oka
                    );

                    main.post(() -> {
                        Log.i("PROBE", res);
                        //textView.setText(res); // jeśli chcesz coś pokazać w UI
                    });
                } catch (Throwable t) {
                    String msg = (t.getMessage()==null) ? t.toString() : t.getMessage();
                    Log.e("PROBE", "fatal: " + msg, t);
                    main.post(() -> Toast.makeText(this, msg, Toast.LENGTH_LONG).show());
                }
            });
        });

        automationRulesButton.setOnClickListener(v -> {
            Intent intent = new Intent(MainActivity.this, AutomationRulesActivity.class);
            startActivity(intent);
        });

        manageDevicesButton.setOnClickListener(v -> {
            Intent intent = new Intent(MainActivity.this, DevicesActivity.class);
            startActivity(intent);
        });
    }
}