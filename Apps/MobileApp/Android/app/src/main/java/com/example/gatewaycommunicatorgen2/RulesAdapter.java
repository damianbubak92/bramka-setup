package com.example.gatewaycommunicatorgen2;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

/**
 * Adapter do RecyclerView wyświetlającej reguły automatyzacji.
 */
public class RulesAdapter extends RecyclerView.Adapter<RulesAdapter.RuleViewHolder> {

    public interface OnEditClickListener {
        void onEdit(int position);
    }
    public interface OnDeleteClickListener {
        void onDelete(int position);
    }

    private final List<AutomationRuleModel> rules;
    private final OnEditClickListener editListener;
    private final OnDeleteClickListener deleteListener;

    public RulesAdapter(List<AutomationRuleModel> rules,
                        OnEditClickListener editListener,
                        OnDeleteClickListener deleteListener) {
        this.rules = rules;
        this.editListener = editListener;
        this.deleteListener = deleteListener;
    }

    @NonNull
    @Override
    public RuleViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_rule, parent, false);
        return new RuleViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull RuleViewHolder holder, int position) {
        AutomationRuleModel rule = rules.get(position);
        holder.textName.setText(rule.getName());
        holder.textTarget.setText("Target: " + rule.getTarget());
        holder.textActionType.setText("Action: " + rule.getActionType());
        holder.textValue.setText("Value: " + rule.getValue());
        // Przycisk edit
        holder.btnEdit.setOnClickListener(v -> {
            int pos = holder.getAdapterPosition();
            if (pos == RecyclerView.NO_POSITION) return;
            if (editListener != null) editListener.onEdit(pos);
        });
        // Przycisk delete
        holder.btnDelete.setOnClickListener(v -> {
            int pos = holder.getAdapterPosition();
            if (pos == RecyclerView.NO_POSITION) return;
            if (deleteListener != null) deleteListener.onDelete(pos);
        });
    }

    @Override
    public int getItemCount() {
        return rules.size();
    }

    static class RuleViewHolder extends RecyclerView.ViewHolder {
        TextView textName, textTarget, textActionType, textValue;
        ImageButton btnEdit, btnDelete;

        RuleViewHolder(@NonNull View itemView) {
            super(itemView);
            textName = itemView.findViewById(R.id.textName);
            textTarget = itemView.findViewById(R.id.textTarget);
            textActionType = itemView.findViewById(R.id.textActionType);
            textValue = itemView.findViewById(R.id.textValue);
            btnEdit = itemView.findViewById(R.id.buttonEdit);
            btnDelete = itemView.findViewById(R.id.buttonDelete);
        }
    }
}
