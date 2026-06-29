package com.example.smarthomev2;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import java.util.List;

public class AutomationRulesAdapter extends RecyclerView.Adapter<AutomationRulesAdapter.RuleViewHolder> {
    private final List<AutomationRuleModel> ruleList;

    public AutomationRulesAdapter(List<AutomationRuleModel> ruleList) {
        this.ruleList = ruleList;
    }

    @NonNull
    @Override
    public RuleViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_rule, parent, false);
        return new RuleViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull RuleViewHolder holder, int position) {
        AutomationRuleModel rule = ruleList.get(position);
        holder.textName.setText(rule.getName());
        holder.textTarget.setText("Target: " + rule.getTarget());
        holder.textActionType.setText("Action: " + rule.getActionType());
        holder.textValue.setText("Value: " + rule.getValue());
        holder.editButton.setOnClickListener(v -> {
            Context ctx = v.getContext();
            Intent intent = new Intent(ctx, AutomationRuleFormActivity.class);
            intent.putExtra(AutomationRuleFormActivity.EXTRA_RULE, rule);
            intent.putExtra("index", holder.getAdapterPosition());
            ((Activity) ctx).startActivityForResult(intent, 101);
        });

        holder.deleteButton.setOnClickListener(v -> {
            // TODO: Pokaż dialog potwierdzenia i usuń regułę
        });
    }

    @Override
    public int getItemCount() {
        return ruleList.size();
    }

    public static class RuleViewHolder extends RecyclerView.ViewHolder {
        TextView textName, textTarget, textActionType, textValue;
        ImageButton editButton, deleteButton;

        public RuleViewHolder(@NonNull View itemView) {
            super(itemView);
            textName = itemView.findViewById(R.id.textName);
            textTarget = itemView.findViewById(R.id.textTarget);
            textActionType = itemView.findViewById(R.id.textActionType);
            textValue = itemView.findViewById(R.id.textValue);
            editButton = itemView.findViewById(R.id.buttonEdit);
            deleteButton = itemView.findViewById(R.id.buttonDelete);
        }
    }
}