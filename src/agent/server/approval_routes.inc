HttpResponse AgentServerApp::handle_approval_list(const AgentServerRequestContext& context) {
  auto* queue = approval_queue();
  if (!queue) {
    throw HttpRequestError(404, "Approval runtime is not configured.",
                           Value::object({{"error", "Approval runtime is not configured."}}));
  }
  Value::Array items;
  for (const auto& record : queue->list(ApprovalRecordFilter{.status = approval_status_filter(context.query)})) {
    items.push_back(approval_record_to_value(record));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_approval_resolve(const AgentServerRequestContext& context) {
  auto* queue = approval_queue();
  if (!queue) {
    throw HttpRequestError(404, "Approval runtime is not configured.",
                           Value::object({{"error", "Approval runtime is not configured."}}));
  }
  const auto body = read_json_body(context.request);
  const auto raw_decision = body.at("decision").as_string();
  PermissionDecision decision{
      .decision = raw_decision == "allow" ? PermissionDecisionKind::Allow
                  : raw_decision == "ask" ? PermissionDecisionKind::Ask
                                          : PermissionDecisionKind::Deny,
      .reason = body.at("reason").as_string(),
  };
  const auto approval_id = context.params.at("approvalId");
  auto record = queue->resolve(approval_id, std::move(decision));
  if (!record) {
    return send_json(404, Value::object({{"error", "Approval not found: " + approval_id}}));
  }
  append_audit(AuditRecord{
      .type = "approval.resolved",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .approval_id = approval_id,
      .status_code = 200,
      .detail = Value::object({{"decision", record->final_decision
                                                 ? to_string(record->final_decision->decision)
                                                 : std::string{}},
                               {"reason", record->final_decision
                                             ? Value(record->final_decision->reason)
                                             : Value()},
                               {"toolName", record->request.tool_name}}),
  });
  return send_json(200, approval_record_to_value(*record));
}
