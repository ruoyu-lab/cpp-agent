Value agent_server_governance_summary_to_value(const AgentServerGovernanceSummary& summary) {
  return Value::object({{"redacted", summary.redacted},
                        {"schemaValidated", summary.schema_validated},
                        {"citationCount", summary.citation_count},
                        {"citationsAppended", summary.citations_appended}});
}

std::string to_string(ApprovalRecordStatus status) {
  switch (status) {
    case ApprovalRecordStatus::Pending:
      return "pending";
    case ApprovalRecordStatus::Resolved:
      return "resolved";
  }
  return "pending";
}

ApprovalRecordStatus approval_record_status_from_string(const std::string& value,
                                                        ApprovalRecordStatus fallback) {
  if (value == "pending") return ApprovalRecordStatus::Pending;
  if (value == "resolved") return ApprovalRecordStatus::Resolved;
  return fallback;
}

Value permission_decision_to_value(const PermissionDecision& decision) {
  Value::Array resources;
  for (const auto& resource : decision.resources) {
    resources.push_back(permission_resource_to_value(resource));
  }
  return Value::object({{"decision", to_string(decision.decision)},
                        {"reason", decision.reason.empty() ? Value() : Value(decision.reason)},
                        {"source", to_string(decision.source)},
                        {"ruleId", decision.rule_id.empty() ? Value() : Value(decision.rule_id)},
                        {"priority", decision.priority},
                        {"resources", Value(std::move(resources))},
                        {"sandboxRequest", decision.sandbox_request ? sandbox_request_to_value(*decision.sandbox_request)
                                                                     : Value()}});
}

PermissionDecision permission_decision_from_value(const Value& value) {
  PermissionDecision decision;
  decision.decision = permission_decision_kind_from_string(value.at("decision").as_string(), PermissionDecisionKind::Deny);
  decision.reason = value.at("reason").as_string();
  decision.source = permission_decision_source_from_string(value.at("source").as_string());
  decision.rule_id = value.at("ruleId").as_string(value.at("rule_id").as_string());
  if (value.at("priority").is_number()) {
    decision.priority = static_cast<PermissionPriority>(value.at("priority").as_integer());
  }
  const auto& resources = value.at("resources");
  if (resources.is_array()) {
    for (const auto& entry : resources.as_array()) {
      decision.resources.push_back(permission_resource_from_value(entry));
    }
  }
  const auto& sandbox_request = value.at("sandboxRequest").is_object() ? value.at("sandboxRequest")
                                                                       : value.at("sandbox_request");
  if (sandbox_request.is_object()) {
    decision.sandbox_request = sandbox_request_from_value(sandbox_request);
  }
  return decision;
}

Value permission_request_to_value(const PermissionRequest& request) {
  Value::Array actions;
  for (const auto action : request.actions) {
    actions.emplace_back(to_string(action));
  }
  Value::Array resources;
  for (const auto& resource : request.resources) {
    resources.push_back(permission_resource_to_value(resource));
  }
  return Value::object({{"toolName", request.tool_name},
                        {"tool", Value::object({
                                     {"name", request.tool.name},
                                     {"capabilities", strings_to_value(request.tool.capabilities)},
                                     {"riskLevel", request.tool.risk_level},
                                     {"tags", strings_to_value(request.tool.tags)},
                                     {"bundle", request.tool.bundle.empty() ? Value() : Value(request.tool.bundle)},
                                     {"builtin", request.tool.builtin},
                                 })},
                        {"capabilities", strings_to_value(request.capabilities)},
                        {"riskLevel", to_string(request.risk_level)},
                        {"tags", strings_to_value(request.tags)},
                        {"bundle", request.bundle.empty() ? Value() : Value(request.bundle)},
                        {"builtin", request.builtin},
                        {"actions", Value(std::move(actions))},
                        {"resources", Value(std::move(resources))},
                        {"source", to_string(request.source)},
                        {"priority", request.priority},
                        {"toolCall", tool_call_to_value(request.tool_call)},
                        {"context", tool_execution_context_to_value(request.context)}});
}

PermissionRequest permission_request_from_value(const Value& value) {
  PermissionRequest request;
  request.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  request.capabilities = strings_from_value(value.at("capabilities"));
  request.risk_level = tool_risk_level_from_string(value.at("riskLevel").as_string(
      value.at("risk_level").as_string()), ToolRiskLevel::Low);
  request.tags = strings_from_value(value.at("tags"));
  request.bundle = value.at("bundle").as_string();
  request.builtin = value.at("builtin").as_bool();
  const auto& tool = value.at("tool");
  if (tool.is_object()) {
    request.tool = PermissionToolRef{
        .name = tool.at("name").as_string(),
        .capabilities = strings_from_value(tool.at("capabilities")),
        .risk_level = tool.at("riskLevel").as_string(tool.at("risk_level").as_string()),
        .tags = strings_from_value(tool.at("tags")),
        .bundle = tool.at("bundle").as_string(),
        .builtin = tool.at("builtin").as_bool(),
    };
  } else {
    request.tool = PermissionToolRef{
        .name = request.tool_name,
        .capabilities = request.capabilities,
        .risk_level = to_string(request.risk_level),
        .tags = request.tags,
        .bundle = request.bundle,
        .builtin = request.builtin,
    };
  }
  const auto& actions = value.at("actions");
  if (actions.is_array()) {
    for (const auto& entry : actions.as_array()) {
      if (entry.is_string()) {
        request.actions.push_back(permission_action_from_string(entry.as_string()));
      }
    }
  }
  const auto& resources = value.at("resources");
  if (resources.is_array()) {
    for (const auto& entry : resources.as_array()) {
      request.resources.push_back(permission_resource_from_value(entry));
    }
  }
  request.source = permission_decision_source_from_string(value.at("source").as_string());
  if (value.at("priority").is_number()) {
    request.priority = static_cast<PermissionPriority>(value.at("priority").as_integer());
  }
  request.tool_call = tool_call_from_value(value.at("toolCall").is_object() ? value.at("toolCall")
                                                                            : value.at("tool_call"));
  request.context = tool_execution_context_from_value(value.at("context"));
  return request;
}

Value approval_record_to_value(const ApprovalRecord& record) {
  return Value::object({{"id", record.id},
                        {"createdAt", record.created_at},
                        {"resolvedAt", record.resolved_at.empty() ? Value() : Value(record.resolved_at)},
                        {"status", to_string(record.status)},
                        {"request", permission_request_to_value(record.request)},
                        {"proposedDecision", permission_decision_to_value(record.proposed_decision)},
                        {"finalDecision", record.final_decision ? permission_decision_to_value(*record.final_decision)
                                                                 : Value()},
                        {"metadata", record.metadata}});
}

ApprovalRecord approval_record_from_value(const Value& value) {
  ApprovalRecord record;
  record.id = value.at("id").as_string();
  record.created_at = value.at("createdAt").as_string(value.at("created_at").as_string());
  record.resolved_at = value.at("resolvedAt").as_string(value.at("resolved_at").as_string());
  record.status = approval_record_status_from_string(value.at("status").as_string(), ApprovalRecordStatus::Pending);
  record.request = permission_request_from_value(value.at("request"));
  record.proposed_decision = permission_decision_from_value(value.at("proposedDecision").is_object()
                                                               ? value.at("proposedDecision")
                                                               : value.at("proposed_decision"));
  const auto& final_decision = value.at("finalDecision").is_object() ? value.at("finalDecision")
                                                                     : value.at("final_decision");
  if (final_decision.is_object()) {
    record.final_decision = permission_decision_from_value(final_decision);
  }
  record.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return record;
}
