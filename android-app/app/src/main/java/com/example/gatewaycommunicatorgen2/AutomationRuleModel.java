package com.example.gatewaycommunicatorgen2;

import java.io.Serializable;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Model pojedynczej reguły automatyzacji.
 * Pozwala na maksymalnie 3 warunki (Time, Parameter, ParameterDelta).
 */
public class AutomationRuleModel implements Serializable {
    private static final long serialVersionUID = 1L;

    public enum ConditionType {
        TIME,
        PARAMETER,
        PARAMETER_DELTA
    }

    public enum ParameterOperator {
        LESS_THAN,
        MORE_THAN,    // ">"// "<"
        BETWEEN          // "Between"
    }

    public enum ParameterType {
        PARAM_T1,
        PARAM_T2,
        PARAM_T3,
        PARAM_T4,
        PARAM_SBUF_TEMP,
        PARAM_UNKNOWN
    }
    public enum ActionType {
        SET_RELAY,
        SEND_MESSAGE
    }

    public enum TargetNodeType {
        TARGET_SOLAR_CONTROLLER,
        TARGET_BUFFER_CONTROLLER,
        TARGET_SMARTPHONE
    }

    public enum DeviceType {
        DEVICE_SOLAR_CONTROLLER,
        DEVICE_BUFFER_CONTROLLER,
        DEVICE_SMARTPHONE,
        DEVICE_COUNT
    }

    // --- Name ---
    private String name;

    // --- Conditions (max 3) ---
    private final List<Condition> conditions = new ArrayList<>(3);

    // --- Action ---
    private TargetNodeType target;
    private ActionType actionType;  // np. SET_RELAY_ON
    private int value;           // "On" / "Off" / payload

    public AutomationRuleModel() {
        // domyślnie pusta
    }

    // --- Name ---
    public String getName() { return name; }
    public void setName(String name) { this.name = name; }

    // --- Conditions ---
    public List<Condition> getConditions() { return conditions; }
    public boolean addCondition(Condition c) {
        if (conditions.size() < 3) {
            conditions.add(c);
            return true;
        }
        return false;
    }
    public void removeCondition(int index) {
        if (index >= 0 && index < conditions.size()) {
            conditions.remove(index);
        }
    }

    // --- Action ---
    public TargetNodeType getTarget() { return target; }
    public void setTarget(TargetNodeType target) { this.target = target; }

    public ActionType getActionType() { return actionType; }
    public void setActionType(ActionType actionType) { this.actionType = actionType; }

    public int getValue() { return value; }
    public void setValue(int value) { this.value = value; }

    /**
     * Pojedynczy warunek w regule (różne warianty).
     */
    public static class Condition implements Serializable {
        private static final long serialVersionUID = 1L;

        private ConditionType conditionType = ConditionType.TIME;

        // --- Time-based ---
        private int startHour, startMin;
        private int stopHour, stopMin;

        // --- Parameter-based ---
        private DeviceType device;
        private ParameterType deviceParameter;
        private ParameterOperator parameterOp;
        private Float thresholdMin; // dolny próg / ">" wartość
        private Float thresholdMax; // górny próg / dla BETWEEN

        // --- Parameter Delta ---
        private DeviceType device1;
        private ParameterType parameter1;
        private DeviceType device2;
        private ParameterType parameter2;
        private ParameterOperator deltaOp;
        private Float deltaMin;
        private Float deltaMax;

        // --- Konstruktor domyślny ---
        public Condition() {}

        // --- ConditionType ---
        public ConditionType getConditionType() { return conditionType; }
        public void setConditionType(ConditionType conditionType) { this.conditionType = conditionType; }

        // --- Time getters/setters ---
        public int getStartHour() { return startHour; }
        public void setStartHour(int startHour) { this.startHour = startHour; }
        public int getStartMin() { return startMin; }
        public void setStartMin(int startMin) { this.startMin = startMin; }
        public int getStopHour() { return stopHour; }
        public void setStopHour(int stopHour) { this.stopHour = stopHour; }
        public int getStopMin() { return stopMin; }
        public void setStopMin(int stopMin) { this.stopMin = stopMin; }

        // --- Parameter-based getters/setters ---
        public DeviceType getDevice() { return device; }
        public void setDevice(DeviceType device) { this.device = device; }
        public ParameterType getDeviceParameter() { return deviceParameter; }
        public void setDeviceParameter(ParameterType deviceParameter) { this.deviceParameter = deviceParameter; }
        public ParameterOperator getParameterOp() { return parameterOp; }
        public void setParameterOp(ParameterOperator parameterOp) { this.parameterOp = parameterOp; }
        public Float getThresholdMin() { return thresholdMin; }
        public void setThresholdMin(Float thresholdMin) { this.thresholdMin = thresholdMin; }
        public Float getThresholdMax() { return thresholdMax; }
        public void setThresholdMax(Float thresholdMax) { this.thresholdMax = thresholdMax; }

        // --- Parameter Delta getters/setters ---
        public DeviceType getDevice1() { return device1; }
        public void setDevice1(DeviceType device1) { this.device1 = device1; }
        public ParameterType getParameter1() { return parameter1; }
        public void setParameter1(ParameterType parameter1) { this.parameter1 = parameter1; }
        public DeviceType getDevice2() { return device2; }
        public void setDevice2(DeviceType device2) { this.device2 = device2; }
        public ParameterType getParameter2() { return parameter2; }
        public void setParameter2(ParameterType parameter2) { this.parameter2 = parameter2; }
        public ParameterOperator getDeltaOp() { return deltaOp; }
        public void setDeltaOp(ParameterOperator deltaOp) { this.deltaOp = deltaOp; }
        public Float getDeltaMin() { return deltaMin; }
        public void setDeltaMin(Float deltaMin) { this.deltaMin = deltaMin; }
        public Float getDeltaMax() { return deltaMax; }
        public void setDeltaMax(Float deltaMax) { this.deltaMax = deltaMax; }
    }

    /** Serializuje tę regułę do JSONObject */
    public JSONObject toJson() throws JSONException {
        JSONObject o = new JSONObject();
        o.put("name", name);
        //o.put("conditionCount", conditions.size());
        o.put("condCnt", conditions.size());

        JSONArray conds = new JSONArray();
        for (Condition c : conditions) {
            if (c.getConditionType() == ConditionType.TIME) {
                JSONObject t = new JSONObject();
                t.put("type", c.getConditionType().ordinal());
                //t.put("hourStart", c.getStartHour());
                //t.put("minStart", c.getStartMin());
                //t.put("hourEnd", c.getStopHour());
                //t.put("minEnd", c.getStopMin());
                t.put("hS", c.getStartHour());
                t.put("mS", c.getStartMin());
                t.put("hE", c.getStopHour());
                t.put("mE", c.getStopMin());
                //co.put("time", t);
                conds.put(t);
            } else if (c.getConditionType() == ConditionType.PARAMETER) {
                JSONObject p = new JSONObject();
                p.put("type", c.getConditionType().ordinal());
                //p.put("device", c.getDevice().ordinal());
                //p.put("parameter", c.getDeviceParameter().ordinal());
                //p.put("op", c.getParameterOp().ordinal());
                //if (c.getThresholdMin() != null) p.put("thresholdMin", c.getThresholdMin());
                //if (c.getThresholdMax() != null) p.put("thresholdMax", c.getThresholdMax());
                p.put("d", c.getDevice().ordinal());
                p.put("p", c.getDeviceParameter().ordinal());
                p.put("op", c.getParameterOp().ordinal());
                if (c.getThresholdMin() != null) p.put("mn", c.getThresholdMin());
                if (c.getThresholdMax() != null) p.put("mx", c.getThresholdMax());
                conds.put(p);
            } else if (c.getConditionType() == ConditionType.PARAMETER_DELTA) {
                JSONObject d = new JSONObject();
                d.put("type", c.getConditionType().ordinal());
                //d.put("device1", c.getDevice1().ordinal());
                //d.put("parameter1", c.getParameter1().ordinal());
                //d.put("device2", c.getDevice2().ordinal());
                //d.put("parameter2", c.getParameter2().ordinal());
                //d.put("deltaOp", c.getDeltaOp().ordinal());
                //if (c.getDeltaMin() != null) d.put("deltaMin", c.getDeltaMin());
                //if (c.getDeltaMax() != null) d.put("deltaMax", c.getDeltaMax());
                d.put("d1", c.getDevice1().ordinal());
                d.put("p1", c.getParameter1().ordinal());
                d.put("d2", c.getDevice2().ordinal());
                d.put("p2", c.getParameter2().ordinal());
                d.put("op", c.getDeltaOp().ordinal());
                if (c.getDeltaMin() != null) d.put("mn", c.getDeltaMin());
                if (c.getDeltaMax() != null) d.put("mx", c.getDeltaMax());
                conds.put(d);
            }
            //conds.put(co);
        }
        //o.put("conditions", conds);
        o.put("conds", conds);
        JSONObject actionArray = new JSONObject();
        actionArray.put("target", getTarget().ordinal());
        actionArray.put("actionType", getActionType().ordinal());
        actionArray.put("value", getValue());

        o.put("action",actionArray);
        //o.put("conditions", conds);
        return o;
    }


    /** Tworzy model z JSONObject; zwraca null przy błędzie */
    public static AutomationRuleModel fromJson(JSONObject o) {
        try {
            AutomationRuleModel m = new AutomationRuleModel();
            m.setName(o.optString("name", ""));


            JSONArray conds = o.optJSONArray("conds");
            if (conds != null) {
                for (int i = 0; i < conds.length(); i++) {
                    JSONObject co = conds.getJSONObject(i);
                    AutomationRuleModel.Condition c = new AutomationRuleModel.Condition();
                    c.setConditionType(enumFromOrdinal(AutomationRuleModel.ConditionType.class, co.optInt("type", 0), AutomationRuleModel.ConditionType.TIME));

                    if (c.getConditionType() == ConditionType.TIME) {
                        //JSONObject t = co.getJSONObject("time");
                        c.setStartHour(co.optInt("hS", 0));
                        c.setStartMin(co.optInt("mS", 0));
                        c.setStopHour(co.optInt("hE", 0));
                        c.setStopMin(co.optInt("mE", 0));
                    } else if (c.getConditionType() == ConditionType.PARAMETER) {
                        //JSONObject p = co.getJSONObject("parameter");
                        //c.setDevice(DeviceType.valueOf(co.optString("device", "")));
                        c.setDevice(enumFromOrdinal(AutomationRuleModel.DeviceType.class, co.optInt("d", 0), DeviceType.DEVICE_SOLAR_CONTROLLER));
                        //c.setDeviceParameter(ParameterType.valueOf(co.optString("deviceParameter", "")));
                        c.setDeviceParameter(enumFromOrdinal(AutomationRuleModel.ParameterType.class, co.optInt("p", 0), ParameterType.PARAM_SBUF_TEMP));
                        //c.setParameterOp(ParameterOperator.valueOf(co.optString("parameterOp", "LESS_THAN")));
                        c.setParameterOp(enumFromOrdinal(AutomationRuleModel.ParameterOperator.class, co.optInt("op", 0), ParameterOperator.MORE_THAN));
                        if (co.has("mn")) c.setThresholdMin((float)co.getDouble("mn"));
                        if (co.has("mx")) c.setThresholdMax((float)co.getDouble("mx"));
                    } else if (c.getConditionType() == ConditionType.PARAMETER_DELTA) {
                        //JSONObject d = co.getJSONObject("parameterDelta");
                        //c.setDevice1(DeviceType.valueOf(co.optString("device1", "")));
                        c.setDevice1(enumFromOrdinal(AutomationRuleModel.DeviceType.class, co.optInt("d1", 0), DeviceType.DEVICE_SOLAR_CONTROLLER));
                        //c.setParameter1(ParameterType.valueOf(co.optString("parameter1", "")));
                        c.setParameter1(enumFromOrdinal(AutomationRuleModel.ParameterType.class, co.optInt("p1", 0), ParameterType.PARAM_SBUF_TEMP));
                        //c.setDevice2(DeviceType.valueOf(co.optString("device2", "")));
                        c.setDevice2(enumFromOrdinal(AutomationRuleModel.DeviceType.class, co.optInt("d2", 0), DeviceType.DEVICE_SOLAR_CONTROLLER));
                        //c.setParameter2(ParameterType.valueOf(co.optString("parameter2", "")));
                        c.setParameter2(enumFromOrdinal(AutomationRuleModel.ParameterType.class, co.optInt("p2", 0), ParameterType.PARAM_SBUF_TEMP));
                        //c.setDeltaOp(ParameterOperator.valueOf(co.optString("deltaOp", "LESS_THAN")));
                        c.setDeltaOp(enumFromOrdinal(AutomationRuleModel.ParameterOperator.class, co.optInt("op", 0), ParameterOperator.MORE_THAN));
                        if (co.has("mn")) c.setDeltaMin((float)co.getDouble("mn"));
                        if (co.has("mx")) c.setDeltaMax((float)co.getDouble("mx"));
                    }
                    m.addCondition(c);
                }
            }

            JSONObject action = o.optJSONObject("action");
            m.setTarget(enumFromOrdinal(AutomationRuleModel.TargetNodeType.class, action.optInt("target", 0), TargetNodeType.TARGET_SOLAR_CONTROLLER));
            m.setActionType(enumFromOrdinal(AutomationRuleModel.ActionType.class, action.optInt("actionType", 0), ActionType.SET_RELAY));
            m.setValue(action.optInt("value", 0));
            //m.setTarget(enumFromOrdinal(AutomationRuleModel.TargetNodeType.class, o.optInt("target", 0), TargetNodeType.TARGET_SOLAR_CONTROLLER));
            //m.setActionType(enumFromOrdinal(AutomationRuleModel.ActionType.class, o.optInt("actionType", 0), ActionType.SET_RELAY));
            //m.setValue(o.optInt("value", 0));

            return m;
        } catch (JSONException | IllegalArgumentException e) {
            e.printStackTrace();
            return null;
        }
    }
    private static <T extends Enum<T>> T enumFromOrdinal(Class<T> clazz, int ordinal, T fallback) {
        T[] vals = clazz.getEnumConstants();
        if (ordinal >= 0 && ordinal < vals.length) return vals[ordinal];
        return fallback;
    }
}