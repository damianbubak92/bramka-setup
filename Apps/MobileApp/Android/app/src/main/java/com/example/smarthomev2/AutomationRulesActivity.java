package com.example.smarthomev2;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.floatingactionbutton.FloatingActionButton;

import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.List;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Aktywność wyświetlająca listę reguł automatyzacji.
 * Pozwala na fetch, send, add, edit, delete.
 */
public class AutomationRulesActivity extends AppCompatActivity {

    private static final int REQUEST_ADD = 1;
    private static final int REQUEST_EDIT = 2;

    private RecyclerView recyclerView;
    private RulesAdapter adapter;
    private List<AutomationRuleModel> rulesList;

    private TextView textBrowser;
    private FloatingActionButton fabAdd;
    private View btnFetch, btnSend;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_automation_rules);

        // init views
        recyclerView = findViewById(R.id.recyclerRules);
        fabAdd = findViewById(R.id.fabAddRule);
        btnFetch = findViewById(R.id.btnFetchRules);
        btnSend  = findViewById(R.id.btnSendRules);
        textBrowser = findViewById(R.id.responseOutputBox);

        rulesList = new ArrayList<>();
        adapter = new RulesAdapter(rulesList, this::onEditRule, this::onDeleteRule);
        recyclerView.setLayoutManager(new LinearLayoutManager(this));
        recyclerView.setAdapter(adapter);

        fabAdd.setOnClickListener(v -> launchFormForAdd());
        btnFetch.setOnClickListener(v -> fetchRulesFromDevice());
        btnSend.setOnClickListener(v -> sendRulesToDevice());
    }

    private void launchFormForAdd() {
        Intent i = new Intent(this, AutomationRuleFormActivity.class);
        startActivityForResult(i, REQUEST_ADD);
    }

    private void onEditRule(int position) {
        AutomationRuleModel rule = rulesList.get(position);
        Intent i = new Intent(this, AutomationRuleFormActivity.class);
        i.putExtra(AutomationRuleFormActivity.EXTRA_RULE, rule);
        i.putExtra(AutomationRuleFormActivity.EXTRA_RULE_INDEX, position);
        startActivityForResult(i, REQUEST_EDIT);
    }

    private void onDeleteRule(int position) {
        rulesList.remove(position);
        adapter.notifyItemRemoved(position);
    }

    private void fetchRulesFromDevice() {
        // TODO: wywołaj NetworkClient.sendCommand("getrules"), parsuj JSON do List<AutomationRuleModel>
        //Toast.makeText(this, "Fetching...", Toast.LENGTH_SHORT).show();
        String command = "getrules";
        if (!command.isEmpty()) {
            new Thread(() -> {
                String response = null;
                try {
                    response = NetworkClient.sendCommand(getApplicationContext(),command);
                } catch (UnsupportedEncodingException e) {
                    throw new RuntimeException(e);
                }
                List<AutomationRuleModel> fetchedRules = parseRulesList(response);
                runOnUiThread(() -> {
                    //textBrowser.setText(response);
                    if (fetchedRules.isEmpty()) {
                        //textBrowser.setText(response);
                //        try {
                           // JSONArray arr = new JSONArray(response);
                            //textBrowser.setText(arr.getJSONObject(0).toString());

                //        } catch (JSONException e) {
                //            throw new RuntimeException(e);
                //        }
                        Toast.makeText(this, "Brak reguł lub błąd parsowania", Toast.LENGTH_SHORT).show();
                    } else {
                        rulesList.clear();
                        rulesList.addAll(fetchedRules);
                        adapter.notifyDataSetChanged();
                        Toast.makeText(this, "Załadowano " + rulesList.size() + " reguł", Toast.LENGTH_SHORT).show();
                    }
                });
            }).start();
        }
    }

    private void sendRulesToDevice() {
        if (rulesList.isEmpty()) {
            Toast.makeText(this, "No rules to send", Toast.LENGTH_SHORT).show();
            return;
        }

        String json = serializeRulesList(rulesList);
        String command = "setrules";

        new Thread(() -> {
            try {
                // Zbuduj body
                String body = "command=" + command//URLEncoder.encode(command, "UTF-8")
                        + "&rules=" + json//URLEncoder.encode(json, "UTF-8")
                        + "&authToken=" + NetworkClient.AUTH_TOKEN;//URLEncoder.encode(NetworkClient.AUTH_TOKEN, "UTF-8");

                String response = NetworkClient.sendRawPost(getApplicationContext(),body); // zobacz niżej

                runOnUiThread(() -> {
                    Toast.makeText(this, "Sent. Response: " + response, Toast.LENGTH_SHORT).show();
                    // Możesz tu zaktualizować UI, np. odświeżyć karty potwierdzając zapis
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    Toast.makeText(this, "Send failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    public static String serializeRulesList(java.util.List<AutomationRuleModel> rules) {
        JSONArray arr = new JSONArray();
        for (AutomationRuleModel r : rules) {
            try {
                arr.put(r.toJson());
            } catch (JSONException e) {
                e.printStackTrace();
            }
        }
        return arr.toString();
    }

    public static java.util.List<AutomationRuleModel> parseRulesList(String json) {
        java.util.List<AutomationRuleModel> out = new java.util.ArrayList<>();
        try {
            JSONArray arr = new JSONArray(json);
            for (int i = 0; i < arr.length(); i++) {

                JSONObject o = arr.getJSONObject(i);
                AutomationRuleModel m = AutomationRuleModel.fromJson(o);
                if (m != null) out.add(m);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        return out;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;

        AutomationRuleModel rule = (AutomationRuleModel) data.getSerializableExtra(AutomationRuleFormActivity.EXTRA_RULE);
        int index = data.getIntExtra(AutomationRuleFormActivity.EXTRA_RULE_INDEX, -1);

        if (requestCode == REQUEST_ADD) {
            rulesList.add(rule);
            adapter.notifyItemInserted(rulesList.size() - 1);
           // textBrowser.setText(serializeRulesList(rulesList));
        } else if (requestCode == REQUEST_EDIT && index >= 0) {
            rulesList.set(index, rule);
            adapter.notifyItemChanged(index);
        }
    }
}
