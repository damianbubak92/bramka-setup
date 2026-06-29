package com.example.smarthomev2;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * "Zarządzaj urządzeniami": lista sprovisionowanych nodów (listnodes) + usuwanie
 * (tap -> removenode). Dodawanie jest event-driven: gdy node wciśnie JOIN, bramka
 * pushuje przez WebSocket ({@link GatewayWs}) -> okienko nazwy само wyskakuje. Brak
 * przycisków Odśwież/Dodaj - lista aktualizuje się na żywo (WS), a wejście na ekran
 * dociąga snapshot + ewentualnych oczekujących.
 */
public class DevicesActivity extends AppCompatActivity implements GatewayWs.Listener {

    private ListView devicesList;
    private TextView devicesEmpty;
    private ArrayAdapter<String> adapter;

    private final List<String> rows = new ArrayList<>();
    private final List<Integer> addrs = new ArrayList<>();   // assigned address per row
    private final List<String> names = new ArrayList<>();    // name per row
    private final List<String> states = new ArrayList<>();   // status per row (greying)

    private static final long ONLINE_WINDOW_S = 180;

    private boolean addDialogOpen = false;

    // Fallback refresh after an action (NOT continuous polling) - covers a delayed/
    // missed WS event.
    private final Handler ui = new Handler(Looper.getMainLooper());
    private void refreshAfter(long ms) { ui.postDelayed(this::loadNodes, ms); }

    // Wszystkie toasty w jednym miejscu (dół ekranu), spójnie.
    private void toast(String msg, int dur) {
        Toast t = Toast.makeText(this, msg, dur);
        t.setGravity(Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL, 0, 220);
        t.show();
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_devices);

        devicesList = findViewById(R.id.devicesList);
        devicesEmpty = findViewById(R.id.devicesEmpty);

        adapter = new DeviceAdapter();
        devicesList.setAdapter(adapter);

        devicesList.setOnItemClickListener((parent, view, pos, id) -> {
            if (pos >= 0 && pos < addrs.size() && isActive(pos)) {
                confirmRemove(addrs.get(pos), names.get(pos));
            }
        });
    }

    private boolean isActive(int pos) {
        return pos < states.size() && "active".equals(states.get(pos));
    }

    // Greys out non-active rows (pending_join / pending_remove).
    private class DeviceAdapter extends ArrayAdapter<String> {
        DeviceAdapter() { super(DevicesActivity.this, android.R.layout.simple_list_item_1, rows); }
        @NonNull @Override
        public View getView(int position, View convertView, @NonNull ViewGroup parent) {
            View v = super.getView(position, convertView, parent);
            String st = position < states.size() ? states.get(position) : "active";
            boolean pending = "pending_join".equals(st) || "pending_remove".equals(st);
            v.setAlpha(pending ? 0.4f : 1.0f);
            return v;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        GatewayWs.get().addListener(this); // live events; connection owned by GatewayApp
        onWsState(GatewayWs.get().isConnected());
        loadNodes();          // snapshot listy
        checkPendingJoins();  // jeśli ktoś JOINował zanim wszedłeś - pokaż okienko
    }

    @Override
    protected void onPause() {
        super.onPause();
        GatewayWs.get().removeListener(this);
        ui.removeCallbacksAndMessages(null);
    }

    // ---- live events (WebSocket) ----

    @Override
    public void onWsState(boolean connected) {
        runOnUiThread(() -> setTitle(connected ? "Urządzenia  ● live" : "Urządzenia  (offline)"));
    }

    @Override
    public void onJoinPending(String factory, int nodeType) {
        runOnUiThread(() -> { if (!addDialogOpen) askNameAndApprove(factory, nodeType); });
    }

    @Override
    public void onNodeStatus(int address, String status) {
        runOnUiThread(this::loadNodes);
    }

    // ---- lista urządzeń (listnodes) ----

    private void loadNodes() {
        new Thread(() -> {
            String resp;
            try {
                resp = NetworkClient.sendCommand(getApplicationContext(), "listnodes");
            } catch (Exception e) {
                resp = "❌ " + e.getMessage();
            }
            final String r = resp;
            runOnUiThread(() -> renderNodes(r));
        }).start();
    }

    private void renderNodes(String json) {
        rows.clear();
        addrs.clear();
        names.clear();
        states.clear();
        try {
            JSONArray arr = new JSONArray(json);
            long nowS = System.currentTimeMillis() / 1000L;
            for (int i = 0; i < arr.length(); i++) {
                JSONObject o = arr.getJSONObject(i);
                int addr = o.optInt("address", 0);
                int type = o.optInt("type", -1);
                String name = o.optString("name", "");
                String state = o.optString("status", "active");
                long lastSeen = o.optLong("lastSeen", 0);
                boolean online = lastSeen > 0 && (nowS - lastSeen) <= ONLINE_WINDOW_S;
                String status;
                if ("pending_join".equals(state)) {
                    status = "… dodawanie (czekam na potwierdzenie)";
                } else if ("pending_remove".equals(state)) {
                    status = "… usuwanie (czekam na potwierdzenie)";
                } else {
                    status = (lastSeen == 0) ? "—" : (online ? "● online" : "○ offline");
                }
                if (name.isEmpty()) name = "(bez nazwy)";
                rows.add(name + "\n" + typeLabel(type)
                        + "  •  adres 0x" + String.format("%02X", addr)
                        + "  •  " + status);
                addrs.add(addr);
                names.add(name);
                states.add(state);
            }
        } catch (Exception e) {
            toast("Błąd listy: " + json, Toast.LENGTH_LONG);
        }
        adapter.notifyDataSetChanged();
        devicesEmpty.setVisibility(rows.isEmpty() ? View.VISIBLE : View.GONE);
    }

    // ---- usuwanie urządzenia (removenode) ----

    private void confirmRemove(int address, String name) {
        new AlertDialog.Builder(this)
                .setTitle("Usunąć urządzenie?")
                .setMessage(name + " (adres 0x" + String.format("%02X", address) + ")\n\n"
                        + "Node zostanie wyrejestrowany i wróci do stanu fabrycznego.")
                .setPositiveButton("Usuń", (d, w) -> removeNode(address))
                .setNegativeButton("Anuluj", null)
                .show();
    }

    private void removeNode(int address) {
        new Thread(() -> {
            String resp = NetworkClient.removeNode(getApplicationContext(), address);
            boolean ok = resp != null && resp.contains("pending_remove");
            // Znika z listy OD RAZU - bramka po cichu dokańcza (potwierdzenie z nodem,
            // zwolnienie adresu albo trzyma go zarezerwowanego). listnodes wyklucza
            // pending_remove, więc loadNodes() usuwa pozycję natychmiast.
            final String msg = ok ? "Pomyślnie usunięto" : ("Nie usunięto: " + resp);
            runOnUiThread(() -> {
                toast(msg, ok ? Toast.LENGTH_SHORT : Toast.LENGTH_LONG);
                if (ok) loadNodes();
            });
        }).start();
    }

    // ---- dodawanie urządzenia (event-driven: JOIN -> okienko) ----

    // Wejście na ekran: dociągnij oczekujących z bramki (gdyby ktoś wcisnął JOIN
    // zanim ekran był otwarty) i pokaż okienko dla pierwszego. Cicho gdy brak.
    private void checkPendingJoins() {
        new Thread(() -> {
            String resp;
            try {
                resp = NetworkClient.sendCommand(getApplicationContext(), "listjoins");
            } catch (Exception e) {
                resp = "[]";
            }
            final String r = resp;
            runOnUiThread(() -> {
                if (addDialogOpen) return;
                try {
                    JSONArray arr = new JSONArray(r);
                    if (arr.length() == 0) return;
                    JSONObject o = arr.optJSONObject(0);
                    askNameAndApprove(o.optString("factory", ""), o.optInt("type", -1));
                } catch (Exception ignored) {}
            });
        }).start();
    }

    private void askNameAndApprove(String factory, int type) {
        final EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setHint("np. Salon parter");

        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("Nowe: " + typeLabel(type))
                .setMessage("Chip: " + factory + "\n\nNadaj nazwę:")
                .setView(input)
                .setPositiveButton("Zapisz", (d, w) -> {
                    String name = input.getText().toString().trim();
                    if (name.isEmpty()) {
                        toast("Podaj nazwę", Toast.LENGTH_SHORT);
                        return;
                    }
                    approve(factory, name);
                })
                .setNegativeButton("Anuluj", null)
                .create();
        dlg.setOnDismissListener(d -> addDialogOpen = false);
        addDialogOpen = true;
        dlg.show();
    }

    private void approve(String factory, String name) {
        new Thread(() -> {
            String resp = NetworkClient.approveJoin(getApplicationContext(), factory, name);
            String msg;
            boolean ok = false;
            try {
                JSONObject o = new JSONObject(resp);
                int addr = o.optInt("address", -1);
                if (addr >= 0) {
                    ok = true;
                    msg = "Dodano „" + o.optString("name", name) + "” (adres 0x"
                            + String.format("%02X", addr) + ")";
                } else {
                    msg = "Nie dodano: " + resp;
                }
            } catch (Exception e) {
                msg = "Nie dodano: " + resp;
            }
            final String fmsg = msg;
            final boolean fok = ok;
            runOnUiThread(() -> {
                toast(fmsg, fok ? Toast.LENGTH_SHORT : Toast.LENGTH_LONG);
                if (fok) { loadNodes(); refreshAfter(2000); } // belt: gdyby WS event się spóźnił
            });
        }).start();
    }

    // NODE_* (node_protocol.h) -> etykieta dla użytkownika.
    private static String typeLabel(int t) {
        switch (t) {
            case 0: return "Sterownik solarny";
            case 1: return "Sterownik bufora";
            case 2: return "Sterownik rolet";
            case 3: return "Sterownik oświetlenia";
            case 4: return "Sterownik wentylacji";
            case 6: return "Czujnik temp/wilgotność";
            default: return "Urządzenie (typ " + t + ")";
        }
    }
}
