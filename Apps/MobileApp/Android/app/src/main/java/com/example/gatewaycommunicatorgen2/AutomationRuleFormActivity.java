package com.example.gatewaycommunicatorgen2;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.AutoCompleteTextView;
import android.widget.LinearLayout;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.timepicker.MaterialTimePicker;
import com.google.android.material.timepicker.TimeFormat;

import java.util.Arrays;
import java.util.Calendar;

public class AutomationRuleFormActivity extends AppCompatActivity {
    public static final String EXTRA_RULE       = "extra_rule";
    public static final String EXTRA_RULE_INDEX = "extra_rule_index";

    private TextInputEditText inputName;
    private AutoCompleteTextView conditionTypeDropdown;
    private LinearLayout conditionFieldsContainer;
    private MaterialButton addConditionButton;
    private AutoCompleteTextView targetDropdown;
    private AutoCompleteTextView actionTypeDropdown;
    private TextInputEditText actionValueInput;
    private MaterialButton saveButton;
    private MaterialButton cancelButton;

    private AutomationRuleModel ruleModel;
    private int editIndex = -1;

    private interface TimeSetCallback {
        void onTimeSet(int hour, int minute);
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_automation_rule_form);

        inputName = findViewById(R.id.nameInput);
       // conditionTypeDropdown = findViewById(R.id.innerConditionTypeDropdown);
        conditionFieldsContainer = findViewById(R.id.conditionFieldsContainer);
        addConditionButton = findViewById(R.id.addConditionButton);
        targetDropdown = findViewById(R.id.targetDropdown);
        actionTypeDropdown = findViewById(R.id.actionTypeDropdown);
        actionValueInput = findViewById(R.id.actionValueInput);
        saveButton = findViewById(R.id.saveButton);
        cancelButton = findViewById(R.id.cancelButton);

        setupDropdowns();

        addConditionButton.setOnClickListener(v -> addConditionField());

        Intent intent = getIntent();
        if (intent.hasExtra(EXTRA_RULE)) {
            ruleModel = (AutomationRuleModel) intent.getSerializableExtra(EXTRA_RULE);
            editIndex = intent.getIntExtra(EXTRA_RULE_INDEX, -1);
            populateForm(ruleModel);
        } else {
            ruleModel = new AutomationRuleModel();
        }

        // Jeśli nie ma żadnego warunku, dodaj pierwszy
        if (ruleModel.getConditions().isEmpty()) {
            addConditionField();
        }

        saveButton.setOnClickListener(v -> onSave());
        cancelButton.setOnClickListener(v -> finish());
    }

    private void setupDropdowns() {
        String[] mainCondTypes = {"Time", "Parameter", "Parameter Delta"};
        //conditionTypeDropdown.setAdapter(new ArrayAdapter<>(
       //         this, android.R.layout.simple_dropdown_item_1line, Arrays.asList(mainCondTypes)));
        //conditionTypeDropdown.setOnItemClickListener((parent, view, pos, id) -> {
            // zmiana typu głównego: wyczyść i dodaj nowy blok
          //  conditionFieldsContainer.removeAllViews();
          //  addConditionField();
       // });

        String[] targets = {"Solar Controller", "Buffer Controller", "Smartphone"};
        targetDropdown.setAdapter(new ArrayAdapter<>(
                this, android.R.layout.simple_dropdown_item_1line, Arrays.asList(targets)));

        String[] actions = {"Set Relay", "Send Message"};
        actionTypeDropdown.setAdapter(new ArrayAdapter<>(
                this, android.R.layout.simple_dropdown_item_1line, Arrays.asList(actions)));
    }

    private void showMaterialTimePicker(TimeSetCallback callback, int initialHour, int initialMinute) {
        MaterialTimePicker picker = new MaterialTimePicker.Builder()
                .setTimeFormat(TimeFormat.CLOCK_24H)
                .setTitleText("Set time")
                .setHour(initialHour)
                .setMinute(initialMinute)
                .setTheme(R.style.ThemeOverlay_GatewayCommunicator_TimePicker)
                .build();

        picker.addOnPositiveButtonClickListener(v -> {
            callback.onTimeSet(picker.getHour(), picker.getMinute());
        });
        picker.show(getSupportFragmentManager(), "TIME_PICKER");
    }

    private void addConditionField() {
        int count = conditionFieldsContainer.getChildCount();
        if (count >= 3) {
            Toast.makeText(this, "Max 3 conditions", Toast.LENGTH_SHORT).show();
            return;
        }

        View block = getLayoutInflater().inflate(R.layout.include_condition_block, conditionFieldsContainer, false);
        AutoCompleteTextView innerType = block.findViewById(R.id.innerConditionTypeDropdown);
        LinearLayout detailContainer = block.findViewById(R.id.innerConditionFieldsContainer);

        String[] condTypes = {"Time", "Parameter", "Parameter Delta"};
        innerType.setAdapter(new ArrayAdapter<>(
                this, android.R.layout.simple_dropdown_item_1line, Arrays.asList(condTypes)));

        innerType.setOnItemClickListener((parent, view, pos, id) -> {
            detailContainer.removeAllViews();
            String sel = innerType.getText().toString();
            View detailView;

            if ("Time".equals(sel)) {
                detailView = getLayoutInflater().inflate(R.layout.include_time_condition, detailContainer, false);
                TextInputEditText startEdit = detailView.findViewById(R.id.timePickerStart);
                TextInputEditText endEdit = detailView.findViewById(R.id.timePickerEnd);
                startEdit.setOnClickListener(v -> {
                    Calendar cal = Calendar.getInstance();
                    String txt = startEdit.getText().toString();
                    int h = cal.get(Calendar.HOUR_OF_DAY), m = cal.get(Calendar.MINUTE);
                    if (txt.matches("\\d{2}:\\d{2}")) {
                        String[] p = txt.split(":");
                        h = Integer.parseInt(p[0]);
                        m = Integer.parseInt(p[1]);
                    }
                    showMaterialTimePicker((hour, minute) ->
                            startEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                });
                endEdit.setOnClickListener(v -> {
                    Calendar cal = Calendar.getInstance();
                    String txt = endEdit.getText().toString();
                    int h = cal.get(Calendar.HOUR_OF_DAY), m = cal.get(Calendar.MINUTE);
                    if (txt.matches("\\d{2}:\\d{2}")) {
                        String[] p = txt.split(":");
                        h = Integer.parseInt(p[0]);
                        m = Integer.parseInt(p[1]);
                    }
                    showMaterialTimePicker((hour, minute) ->
                            endEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                });
            } else if ("Parameter".equals(sel)) {
                detailView = getLayoutInflater().inflate(R.layout.include_parameter_condition, detailContainer, false);
                AutoCompleteTextView device = detailView.findViewById(R.id.parameterDeviceDropdown);
                AutoCompleteTextView param = detailView.findViewById(R.id.parameterParamDropdown);
                AutoCompleteTextView op = detailView.findViewById(R.id.parameterOpDropdown);
                LinearLayout thresholds = detailView.findViewById(R.id.thresholdsContainerParameter);
                TextInputEditText minInput = detailView.findViewById(R.id.parameterMinInput);
                TextInputEditText maxInput = detailView.findViewById(R.id.parameterMaxInput);

                String[] devices = {"Solar Controller", "Bufor Controller", "Smartphone"};
                device.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));

                String[] ops = {"Less than", "More than", "Between"};
                op.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, ops));

                device.setOnItemClickListener((p,v,pos1,id1) -> {
                    // po wybraniu device1 - pokaż param1
                    detailView.findViewById(R.id.parameterParamLayout).setVisibility(View.VISIBLE);
                    // w zależności od device1, ustal listę parametrów:
                    String[] paramsForDevice = {"T1","T2","T3","T4", "sBuforTemp"};
                    param.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice));
                });

                param.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.parameterOpLayout).setVisibility(View.VISIBLE);
                });

                op.setOnItemClickListener((p,v,pos1,id1) -> {
                    thresholds.setVisibility(View.VISIBLE);
                    String selected = op.getText().toString();
                    if ("Less than".equals(selected)) {
                        minInput.setVisibility(View.GONE);
                        maxInput.setVisibility(View.VISIBLE);
                    } else if ("More than".equals(selected)) {
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.GONE);
                    } else { // Between
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.VISIBLE);
                    }
                });

            } else { // Parameter Delta
                detailView = getLayoutInflater().inflate(R.layout.include_parameter_delta_condition, detailContainer, false);
                AutoCompleteTextView device1 = detailView.findViewById(R.id.device1Dropdown);
                AutoCompleteTextView param1 = detailView.findViewById(R.id.param1Dropdown);
                AutoCompleteTextView device2 = detailView.findViewById(R.id.device2Dropdown);
                AutoCompleteTextView param2 = detailView.findViewById(R.id.param2Dropdown);
                AutoCompleteTextView op = detailView.findViewById(R.id.operatorDropdown);
                LinearLayout thresholds = detailView.findViewById(R.id.thresholdsContainer);
                TextInputEditText minInput = detailView.findViewById(R.id.minInput);
                TextInputEditText maxInput = detailView.findViewById(R.id.maxInput);

// przykładowe źródła / parametry
                String[] devices = {"Solar Controller", "Bufor Controller", "Smartphone"};
                device1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));
                device2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));


// operator dropdown
                String[] ops = {"Less than", "More than", "Between"};
                op.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, ops));

                device1.setOnItemClickListener((p,v,pos1,id1) -> {
                    // po wybraniu device1 - pokaż param1
                    detailView.findViewById(R.id.param1Layout).setVisibility(View.VISIBLE);
                    // w zależności od device1, ustal listę parametrów:
                    String[] paramsForDevice1 = {"T1","T2","T3","T4","sBuforTemp"};
                    param1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice1));
                });

                param1.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.device2Layout).setVisibility(View.VISIBLE);
                });

                device2.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.param2Layout).setVisibility(View.VISIBLE);
                    String[] paramsForDevice2 = {"T1","T2","T3","T4","sBuforTemp"};
                    param2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice2));
                });

                param2.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.operatorLayout).setVisibility(View.VISIBLE);
                });

                op.setOnItemClickListener((p,v,pos1,id1) -> {
                    thresholds.setVisibility(View.VISIBLE);
                    String selected = op.getText().toString();
                    if ("Less than".equals(selected)) {
                        minInput.setVisibility(View.GONE);
                        maxInput.setVisibility(View.VISIBLE);
                    } else if ("More than".equals(selected)) {
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.GONE);
                    } else { // Between
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.VISIBLE);
                    }
                });

            }

            detailContainer.addView(detailView);
        });

        conditionFieldsContainer.addView(block);
    }

    private void addConditionField(@Nullable AutomationRuleModel.Condition cond) {
        // Inflate podstawowego bloczka z dropdownem typu
        View block = getLayoutInflater()
                .inflate(R.layout.include_condition_block, conditionFieldsContainer, false);
        AutoCompleteTextView innerType = block.findViewById(R.id.innerConditionTypeDropdown);
        LinearLayout detailContainer = block.findViewById(R.id.innerConditionFieldsContainer);

        // Podłącz adapter do wyboru typu
        String[] condTypes = {"Time", "Parameter", "Parameter Delta"};
        innerType.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_dropdown_item_1line, condTypes));


        // Jeśli mamy istniejący cond, ustaw dropdown i wypełnij szczegóły od razu
        if (cond != null) {
            //String typeStr = cond.getConditionType().name().replace('_',' ');
            innerType.setText(condTypes[cond.getConditionType().ordinal()], false);

            // ręcznie wywołujemy to, co normalnie robi listener na click:
            detailContainer.removeAllViews();
            View detailView;
            switch (cond.getConditionType()) {
                case TIME:
                    detailView = getLayoutInflater()
                            .inflate(R.layout.include_time_condition, detailContainer, false);
                    TextInputEditText startEdit = detailView.findViewById(R.id.timePickerStart);
                    TextInputEditText endEdit   = detailView.findViewById(R.id.timePickerEnd);
                    // wypełnij z cond
                    startEdit.setText(String.format("%02d:%02d", cond.getStartHour(), cond.getStartMin()));
                    startEdit.setOnClickListener(v -> {
                        int h = cond.getStartHour();
                        int m = cond.getStartMin();
                        showMaterialTimePicker((hour, minute) ->
                                startEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                    });
                    endEdit.setText(String.format("%02d:%02d", cond.getStopHour(), cond.getStopMin()));
                    endEdit.setOnClickListener(v -> {
                        int h = cond.getStopHour();
                        int m = cond.getStopMin();
                        showMaterialTimePicker((hour, minute) ->
                                endEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                    });

                    break;

                case PARAMETER:
                    detailView = getLayoutInflater()
                            .inflate(R.layout.include_parameter_condition, detailContainer, false);
                    AutoCompleteTextView device    = detailView.findViewById(R.id.parameterDeviceDropdown);
                    AutoCompleteTextView param     = detailView.findViewById(R.id.parameterParamDropdown);
                    AutoCompleteTextView op        = detailView.findViewById(R.id.parameterOpDropdown);
                    TextInputEditText minInput     = detailView.findViewById(R.id.parameterMinInput);
                    TextInputEditText maxInput     = detailView.findViewById(R.id.parameterMaxInput);
                    // ustaw dropdowny i wartości
                    String[] devices = {"Solar Controller", "Bufor Controller", "Smartphone"};
                    device.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));
                    device.setText(devices[cond.getDevice().ordinal()], false);
                    detailView.findViewById(R.id.parameterParamLayout).setVisibility(View.VISIBLE);
                    String[] paramsForDevice = {"T1","T2","T3","T4", "sBuforTemp"};
                    param.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice));
                    param.setText(paramsForDevice[cond.getDeviceParameter().ordinal()], false);
                    detailView.findViewById(R.id.parameterOpLayout).setVisibility(View.VISIBLE);
                    String[] ops = {"Less than", "More than", "Between"};
                    op.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, ops));
                    op.setText(ops[cond.getParameterOp().ordinal()], false);
                    detailView.findViewById(R.id.thresholdsContainerParameter).setVisibility(View.VISIBLE);
                    if (AutomationRuleModel.ParameterOperator.MORE_THAN.equals(cond.getParameterOp()))
                    {
                        minInput.setText(cond.getThresholdMin().toString());
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.GONE);
                    }
                    else if (AutomationRuleModel.ParameterOperator.LESS_THAN.equals(cond.getParameterOp()))
                    {
                        maxInput.setText(cond.getThresholdMax().toString());
                        minInput.setVisibility(View.GONE);
                        maxInput.setVisibility(View.VISIBLE);
                    }
                    else
                    {
                        minInput.setText(cond.getThresholdMin().toString());
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setText(cond.getThresholdMax().toString());
                        maxInput.setVisibility(View.VISIBLE);
                    }
                    op.setOnItemClickListener((p,v,pos1,id1) -> {
                        String selected = op.getText().toString();
                        if ("Less than".equals(selected)) {
                            minInput.setVisibility(View.GONE);
                            maxInput.setVisibility(View.VISIBLE);
                        } else if ("More than".equals(selected)) {
                            minInput.setVisibility(View.VISIBLE);
                            maxInput.setVisibility(View.GONE);
                        } else { // Between
                            minInput.setVisibility(View.VISIBLE);
                            maxInput.setVisibility(View.VISIBLE);
                        }
                    });

                    break;

                case PARAMETER_DELTA:
                    detailView = getLayoutInflater()
                            .inflate(R.layout.include_parameter_delta_condition, detailContainer, false);
                    // analogicznie ustaw device1,param1,device2,param2,op,min,max
                    AutoCompleteTextView d1   = detailView.findViewById(R.id.device1Dropdown);
                    AutoCompleteTextView p1   = detailView.findViewById(R.id.param1Dropdown);
                    AutoCompleteTextView d2   = detailView.findViewById(R.id.device2Dropdown);
                    AutoCompleteTextView p2   = detailView.findViewById(R.id.param2Dropdown);
                    AutoCompleteTextView op2  = detailView.findViewById(R.id.operatorDropdown);
                    TextInputEditText   mn2   = detailView.findViewById(R.id.minInput);
                    TextInputEditText   mx2   = detailView.findViewById(R.id.maxInput);
                    String[] dNames = {"Solar Controller", "Bufor Controller", "Smartphone"};
                    d1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, dNames));
                    d1.setText(dNames[cond.getDevice1().ordinal()], false);
                    detailView.findViewById(R.id.param1Layout).setVisibility(View.VISIBLE);
                    String[] paramsDevice1 = {"T1","T2","T3","T4","sBuforTemp"};
                    p1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsDevice1));
                    p1.setText(paramsDevice1[cond.getParameter1().ordinal()], false);
                    detailView.findViewById(R.id.device2Layout).setVisibility(View.VISIBLE);
                    d2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, dNames));
                    d2.setText(dNames[cond.getDevice2().ordinal()], false);
                    detailView.findViewById(R.id.param2Layout).setVisibility(View.VISIBLE);
                    String[] paramsDevice2 = {"T1","T2","T3","T4","sBuforTemp"};
                    p2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsDevice2));
                    p2.setText(paramsDevice2[cond.getParameter2().ordinal()], false);
                    detailView.findViewById(R.id.operatorLayout).setVisibility(View.VISIBLE);
                    String[] opNames = {"Less than", "More than", "Between"};
                    op2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, opNames));
                    op2.setText(opNames[cond.getDeltaOp().ordinal()], false);
                    detailView.findViewById(R.id.thresholdsContainer).setVisibility(View.VISIBLE);
                    if (AutomationRuleModel.ParameterOperator.MORE_THAN.equals(cond.getDeltaOp()))
                    {
                        mn2.setText(cond.getDeltaMin().toString());
                        mn2.setVisibility(View.VISIBLE);
                        mx2.setVisibility(View.GONE);
                    }
                    else if (AutomationRuleModel.ParameterOperator.LESS_THAN.equals(cond.getDeltaOp()))
                    {
                        mx2.setText(cond.getDeltaMax().toString());
                        mn2.setVisibility(View.GONE);
                        mx2.setVisibility(View.VISIBLE);
                    }
                    else
                    {
                        mn2.setText(cond.getDeltaMin().toString());
                        mn2.setVisibility(View.VISIBLE);
                        mx2.setText(cond.getDeltaMax().toString());
                        mx2.setVisibility(View.VISIBLE);
                    }
                    op2.setOnItemClickListener((p,v,pos1,id1) -> {
                        String selected = op2.getText().toString();
                        if ("Less than".equals(selected)) {
                            mn2.setVisibility(View.GONE);
                            mx2.setVisibility(View.VISIBLE);
                        } else if ("More than".equals(selected)) {
                            mn2.setVisibility(View.VISIBLE);
                            mx2.setVisibility(View.GONE);
                        } else { // Between
                            mn2.setVisibility(View.VISIBLE);
                            mx2.setVisibility(View.VISIBLE);
                        }
                    });
                    break;

                default:
                    detailView = null; // nie powinno się przytrafić
            }
            if (detailView != null) {
                detailContainer.addView(detailView);
            }
        }

        // Teraz podłącz zwykły listener, żeby móc ręcznie zmieniać typ warunku
        innerType.setOnItemClickListener((parent, view, pos, id) -> {
            detailContainer.removeAllViews();
            String sel = innerType.getText().toString();
            View detailView;

            if ("Time".equals(sel)) {
                detailView = getLayoutInflater().inflate(R.layout.include_time_condition, detailContainer, false);
                TextInputEditText startEdit = detailView.findViewById(R.id.timePickerStart);
                TextInputEditText endEdit = detailView.findViewById(R.id.timePickerEnd);
                startEdit.setOnClickListener(v -> {
                    Calendar cal = Calendar.getInstance();
                    String txt = startEdit.getText().toString();
                    int h = cal.get(Calendar.HOUR_OF_DAY), m = cal.get(Calendar.MINUTE);
                    if (txt.matches("\\d{2}:\\d{2}")) {
                        String[] p = txt.split(":");
                        h = Integer.parseInt(p[0]);
                        m = Integer.parseInt(p[1]);
                    }
                    showMaterialTimePicker((hour, minute) ->
                            startEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                });
                endEdit.setOnClickListener(v -> {
                    Calendar cal = Calendar.getInstance();
                    String txt = endEdit.getText().toString();
                    int h = cal.get(Calendar.HOUR_OF_DAY), m = cal.get(Calendar.MINUTE);
                    if (txt.matches("\\d{2}:\\d{2}")) {
                        String[] p = txt.split(":");
                        h = Integer.parseInt(p[0]);
                        m = Integer.parseInt(p[1]);
                    }
                    showMaterialTimePicker((hour, minute) ->
                            endEdit.setText(String.format("%02d:%02d", hour, minute)), h, m);
                });
            } else if ("Parameter".equals(sel)) {
                detailView = getLayoutInflater().inflate(R.layout.include_parameter_condition, detailContainer, false);
                AutoCompleteTextView device = detailView.findViewById(R.id.parameterDeviceDropdown);
                AutoCompleteTextView param = detailView.findViewById(R.id.parameterParamDropdown);
                AutoCompleteTextView op = detailView.findViewById(R.id.parameterOpDropdown);
                LinearLayout thresholds = detailView.findViewById(R.id.thresholdsContainerParameter);
                TextInputEditText minInput = detailView.findViewById(R.id.parameterMinInput);
                TextInputEditText maxInput = detailView.findViewById(R.id.parameterMaxInput);

                String[] devices = {"Solar Controller", "Bufor Controller", "Smartphone"};
                device.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));

                String[] ops = {"Less than", "More than", "Between"};
                op.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, ops));

                device.setOnItemClickListener((p,v,pos1,id1) -> {
                    // po wybraniu device1 - pokaż param1
                    detailView.findViewById(R.id.parameterParamLayout).setVisibility(View.VISIBLE);
                    // w zależności od device1, ustal listę parametrów:
                    String[] paramsForDevice = {"T1","T2","T3","T4", "sBuforTemp"};
                    param.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice));
                });

                param.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.parameterOpLayout).setVisibility(View.VISIBLE);
                });

                op.setOnItemClickListener((p,v,pos1,id1) -> {
                    thresholds.setVisibility(View.VISIBLE);
                    String selected = op.getText().toString();
                    if ("Less than".equals(selected)) {
                        minInput.setVisibility(View.GONE);
                        maxInput.setVisibility(View.VISIBLE);
                    } else if ("More than".equals(selected)) {
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.GONE);
                    } else { // Between
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.VISIBLE);
                    }
                });

            } else { // Parameter Delta
                detailView = getLayoutInflater().inflate(R.layout.include_parameter_delta_condition, detailContainer, false);
                AutoCompleteTextView device1 = detailView.findViewById(R.id.device1Dropdown);
                AutoCompleteTextView param1 = detailView.findViewById(R.id.param1Dropdown);
                AutoCompleteTextView device2 = detailView.findViewById(R.id.device2Dropdown);
                AutoCompleteTextView param2 = detailView.findViewById(R.id.param2Dropdown);
                AutoCompleteTextView op = detailView.findViewById(R.id.operatorDropdown);
                LinearLayout thresholds = detailView.findViewById(R.id.thresholdsContainer);
                TextInputEditText minInput = detailView.findViewById(R.id.minInput);
                TextInputEditText maxInput = detailView.findViewById(R.id.maxInput);

// przykładowe źródła / parametry
                String[] devices = {"Solar Controller", "Bufor Controller", "Smartphone"};
                device1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));
                device2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, devices));


// operator dropdown
                String[] ops = {"Less than", "More than", "Between"};
                op.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, ops));

                device1.setOnItemClickListener((p,v,pos1,id1) -> {
                    // po wybraniu device1 - pokaż param1
                    detailView.findViewById(R.id.param1Layout).setVisibility(View.VISIBLE);
                    // w zależności od device1, ustal listę parametrów:
                    String[] paramsForDevice1 = {"T1","T2","T3","T4","sBuforTemp"};
                    param1.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice1));
                });

                param1.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.device2Layout).setVisibility(View.VISIBLE);
                });

                device2.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.param2Layout).setVisibility(View.VISIBLE);
                    String[] paramsForDevice2 = {"T1","T2","T3","T4","sBuforTemp"};
                    param2.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line, paramsForDevice2));
                });

                param2.setOnItemClickListener((p,v,pos1,id1) -> {
                    detailView.findViewById(R.id.operatorLayout).setVisibility(View.VISIBLE);
                });

                op.setOnItemClickListener((p,v,pos1,id1) -> {
                    thresholds.setVisibility(View.VISIBLE);
                    String selected = op.getText().toString();
                    if ("Less than".equals(selected)) {
                        minInput.setVisibility(View.GONE);
                        maxInput.setVisibility(View.VISIBLE);
                    } else if ("More than".equals(selected)) {
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.GONE);
                    } else { // Between
                        minInput.setVisibility(View.VISIBLE);
                        maxInput.setVisibility(View.VISIBLE);
                    }
                });

            }

            detailContainer.addView(detailView);
        });

        conditionFieldsContainer.addView(block);
    }

    private void populateForm(AutomationRuleModel model) {
        inputName.setText(model.getName());

        conditionFieldsContainer.removeAllViews();

        for (AutomationRuleModel.Condition cond : model.getConditions()) {
            addConditionField(cond);
        }

        String[] targets = {"Solar Controller", "Buffer Controller", "Smartphone"};
        targetDropdown.setText(targets[model.getTarget().ordinal()], false);
        String[] actions = {"Set Relay", "Send Message"};
        actionTypeDropdown.setText(actions[model.getActionType().ordinal()], false);
        actionValueInput.setText(String.valueOf(model.getValue()));
    }

    private void onSave() {
        // Walidacja podstawowych pól
        if (inputName.getText().toString().trim().isEmpty()
                || targetDropdown.getText().toString().isEmpty()
                || actionTypeDropdown.getText().toString().isEmpty()) {
            Toast.makeText(this, "Wypełnij wszystkie pola", Toast.LENGTH_SHORT).show();
            return;
        }

        // Ustaw podstawowe pola reguły
        ruleModel.setName(inputName.getText().toString().trim());
        String targ = targetDropdown.getText().toString();
        if (targ.equalsIgnoreCase("Solar Controller"))
        {
            ruleModel.setTarget(AutomationRuleModel.TargetNodeType.TARGET_SOLAR_CONTROLLER);
        }
        else if (targ.equalsIgnoreCase("Buffer Controller"))
        {
            ruleModel.setTarget(AutomationRuleModel.TargetNodeType.TARGET_BUFFER_CONTROLLER);
        }
        else
        {
            ruleModel.setTarget(AutomationRuleModel.TargetNodeType.TARGET_SMARTPHONE);
        }

        String at = actionTypeDropdown.getText().toString();
        if (at.equalsIgnoreCase("Set Relay")) {
            ruleModel.setActionType(AutomationRuleModel.ActionType.SET_RELAY);
        } else if (at.equalsIgnoreCase("Send Message")) {
            ruleModel.setActionType(AutomationRuleModel.ActionType.SEND_MESSAGE);
        }
        ruleModel.setValue(Integer.parseInt(actionValueInput.getText().toString().trim()));
        //Log.d("DEBUG", "mojInt = " + Integer.parseInt(actionValueInput.getText().toString().trim()));
        // Czyść stare warunki i zbierz nowe
        ruleModel.getConditions().clear();
        int blocks = conditionFieldsContainer.getChildCount();
        for (int i = 0; i < blocks; i++) {
            View block = conditionFieldsContainer.getChildAt(i);
            AutoCompleteTextView innerType = block.findViewById(R.id.innerConditionTypeDropdown);
            String typeStr = innerType.getText().toString();
            AutomationRuleModel.Condition cond = new AutomationRuleModel.Condition();

            if ("Time".equalsIgnoreCase(typeStr)) {
                cond.setConditionType(AutomationRuleModel.ConditionType.TIME);
                ViewGroup group = block.findViewById(R.id.innerConditionFieldsContainer);
                if (group.getChildCount() == 0) continue; // brak wypełnionego pola
                View detailView = group.getChildAt(0);
                TextInputEditText startEdit = detailView.findViewById(R.id.timePickerStart);
                TextInputEditText endEdit = detailView.findViewById(R.id.timePickerEnd);

                String start = startEdit.getText().toString();
                String end = endEdit.getText().toString();
                if (start.matches("\\d{2}:\\d{2}")) {
                    String[] p = start.split(":");
                    cond.setStartHour(Integer.parseInt(p[0]));
                    cond.setStartMin(Integer.parseInt(p[1]));
                }
                if (end.matches("\\d{2}:\\d{2}")) {
                    String[] p = end.split(":");
                    cond.setStopHour(Integer.parseInt(p[0]));
                    cond.setStopMin(Integer.parseInt(p[1]));
                }

            } else if ("Parameter".equalsIgnoreCase(typeStr)) {
                cond.setConditionType(AutomationRuleModel.ConditionType.PARAMETER);
                ViewGroup group = block.findViewById(R.id.innerConditionFieldsContainer);
                if (group.getChildCount() == 0) continue;
                View detailView = group.getChildAt(0);
                AutoCompleteTextView deviceView = detailView.findViewById(R.id.parameterDeviceDropdown);
                AutoCompleteTextView paramView = detailView.findViewById(R.id.parameterParamDropdown);
                AutoCompleteTextView opView = detailView.findViewById(R.id.parameterOpDropdown);
                TextInputEditText minView = detailView.findViewById(R.id.parameterMinInput);
                TextInputEditText maxView = detailView.findViewById(R.id.parameterMaxInput);

                String dev = deviceView.getText().toString();
                if (dev.equalsIgnoreCase("Solar Controller"))
                {
                    cond.setDevice(AutomationRuleModel.DeviceType.DEVICE_SOLAR_CONTROLLER);
                }
                else if (dev.equalsIgnoreCase("Bufor Controller"))
                {
                    cond.setDevice(AutomationRuleModel.DeviceType.DEVICE_BUFFER_CONTROLLER);
                }
                else
                {
                    cond.setDevice(AutomationRuleModel.DeviceType.DEVICE_SMARTPHONE);
                }

                String devPar = paramView.getText().toString().trim();
                if (devPar.equalsIgnoreCase("T1"))
                {
                    cond.setDeviceParameter(AutomationRuleModel.ParameterType.PARAM_T1);
                }
                else if (devPar.equalsIgnoreCase("T2"))
                {
                    cond.setDeviceParameter(AutomationRuleModel.ParameterType.PARAM_T2);
                }
                else if (devPar.equalsIgnoreCase("T3"))
                {
                    cond.setDeviceParameter(AutomationRuleModel.ParameterType.PARAM_T3);
                }
                else if (devPar.equalsIgnoreCase("T4"))
                {
                    cond.setDeviceParameter(AutomationRuleModel.ParameterType.PARAM_T4);
                }
                else
                {
                    cond.setDeviceParameter(AutomationRuleModel.ParameterType.PARAM_SBUF_TEMP);
                }

                String opStr = opView.getText().toString();
                if (opStr.equalsIgnoreCase("Less than")) {
                    cond.setParameterOp(AutomationRuleModel.ParameterOperator.LESS_THAN);
                } else if (opStr.equalsIgnoreCase("More than")) {
                    cond.setParameterOp(AutomationRuleModel.ParameterOperator.MORE_THAN);
                } else if (opStr.equalsIgnoreCase("Between")) {
                    cond.setParameterOp(AutomationRuleModel.ParameterOperator.BETWEEN);
                }

                String minTxt = minView.getText().toString();
                String maxTxt = maxView.getText().toString();
                if (!minTxt.isEmpty()) cond.setThresholdMin(Float.parseFloat(minTxt));
                if (!maxTxt.isEmpty()) cond.setThresholdMax(Float.parseFloat(maxTxt));

            } else if ("Parameter Delta".equalsIgnoreCase(typeStr)) {
                cond.setConditionType(AutomationRuleModel.ConditionType.PARAMETER_DELTA);
                ViewGroup group = block.findViewById(R.id.innerConditionFieldsContainer);
                if (group.getChildCount() == 0) continue;
                View detailView = group.getChildAt(0);
                AutoCompleteTextView dev1 = detailView.findViewById(R.id.device1Dropdown);
                AutoCompleteTextView param1 = detailView.findViewById(R.id.param1Dropdown);
                AutoCompleteTextView dev2 = detailView.findViewById(R.id.device2Dropdown);
                AutoCompleteTextView param2 = detailView.findViewById(R.id.param2Dropdown);
                AutoCompleteTextView opView = detailView.findViewById(R.id.operatorDropdown);
                TextInputEditText minView = detailView.findViewById(R.id.minInput);
                TextInputEditText maxView = detailView.findViewById(R.id.maxInput);

                String dev11 = dev1.getText().toString();
                if (dev11.equalsIgnoreCase("Solar Controller"))
                {
                    cond.setDevice1(AutomationRuleModel.DeviceType.DEVICE_SOLAR_CONTROLLER);
                }
                else if (dev11.equalsIgnoreCase("Bufor Controller"))
                {
                    cond.setDevice1(AutomationRuleModel.DeviceType.DEVICE_BUFFER_CONTROLLER);
                }
                else
                {
                    cond.setDevice1(AutomationRuleModel.DeviceType.DEVICE_SMARTPHONE);
                }

                String devPar1 = param1.getText().toString();
                if (devPar1.equalsIgnoreCase("T1"))
                {
                    cond.setParameter1(AutomationRuleModel.ParameterType.PARAM_T1);
                }
                else if (devPar1.equalsIgnoreCase("T2"))
                {
                    cond.setParameter1(AutomationRuleModel.ParameterType.PARAM_T2);
                }
                else if (devPar1.equalsIgnoreCase("T3"))
                {
                    cond.setParameter1(AutomationRuleModel.ParameterType.PARAM_T3);
                }
                else if (devPar1.equalsIgnoreCase("T4"))
                {
                    cond.setParameter1(AutomationRuleModel.ParameterType.PARAM_T4);
                }
                else
                {
                    cond.setParameter1(AutomationRuleModel.ParameterType.PARAM_SBUF_TEMP);
                }

                String dev22 = dev2.getText().toString();
                if (dev22.equalsIgnoreCase("Solar Controller"))
                {
                    cond.setDevice2(AutomationRuleModel.DeviceType.DEVICE_SOLAR_CONTROLLER);
                }
                else if (dev22.equalsIgnoreCase("Bufor Controller"))
                {
                    cond.setDevice2(AutomationRuleModel.DeviceType.DEVICE_BUFFER_CONTROLLER);
                }
                else
                {
                    cond.setDevice2(AutomationRuleModel.DeviceType.DEVICE_SMARTPHONE);
                }

                String devPar2 = param2.getText().toString();
                if (devPar2.equalsIgnoreCase("T1"))
                {
                    cond.setParameter2(AutomationRuleModel.ParameterType.PARAM_T1);
                }
                else if (devPar2.equalsIgnoreCase("T2"))
                {
                    cond.setParameter2(AutomationRuleModel.ParameterType.PARAM_T2);
                }
                else if (devPar2.equalsIgnoreCase("T3"))
                {
                    cond.setParameter2(AutomationRuleModel.ParameterType.PARAM_T3);
                }
                else if (devPar2.equalsIgnoreCase("T4"))
                {
                    cond.setParameter2(AutomationRuleModel.ParameterType.PARAM_T4);
                }
                else
                {
                    cond.setParameter2(AutomationRuleModel.ParameterType.PARAM_SBUF_TEMP);
                }

                String opStr = opView.getText().toString();
                if (opStr.equalsIgnoreCase("Less than")) {
                    cond.setDeltaOp(AutomationRuleModel.ParameterOperator.LESS_THAN);
                } else if (opStr.equalsIgnoreCase("More than")) {
                    cond.setDeltaOp(AutomationRuleModel.ParameterOperator.MORE_THAN);
                } else if (opStr.equalsIgnoreCase("Between")) {
                    cond.setDeltaOp(AutomationRuleModel.ParameterOperator.BETWEEN);
                }

                String minTxt = minView.getText().toString();
                String maxTxt = maxView.getText().toString();
                if (!minTxt.isEmpty()) cond.setDeltaMin(Float.parseFloat(minTxt));
                if (!maxTxt.isEmpty()) cond.setDeltaMax(Float.parseFloat(maxTxt));
            }

            ruleModel.addCondition(cond);
        }

        // Zwrócenie wyniku
        Intent result = new Intent();
        result.putExtra(EXTRA_RULE, ruleModel);
        result.putExtra(EXTRA_RULE_INDEX, editIndex);
        setResult(RESULT_OK, result);
        finish();
    }
}