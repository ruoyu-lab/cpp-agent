Value audit_record_to_value(const AuditRecord& record) {
  Value value = Value::object({{"at", record.at.empty() ? now_iso8601() : record.at},
                               {"type", record.type}});
  add_optional_audit_field(value, "method", record.method);
  add_optional_audit_field(value, "path", record.path);
  add_optional_audit_field(value, "agentId", record.agent_id);
  add_optional_audit_field(value, "sessionId", record.session_id);
  add_optional_audit_field(value, "workflowRunId", record.workflow_run_id);
  add_optional_audit_field(value, "approvalId", record.approval_id);
  add_optional_audit_field(value, "apiKeyId", record.api_key_id);
  add_optional_audit_field(value, "requestId", record.request_id);
  add_optional_audit_field(value, "traceId", record.trace_id);
  add_optional_audit_field(value, "taskId", record.task_id);
  if (record.status_code) {
    value["statusCode"] = *record.status_code;
  }
  if (record.duration_ms) {
    value["durationMs"] = *record.duration_ms;
  }
  if (record.detail.is_object() && !record.detail.as_object().empty()) {
    value["detail"] = record.detail;
  }
  return value;
}

void append_audit_log(const std::filesystem::path& file_path, AuditRecord record) {
  if (file_path.empty()) {
    return;
  }
  if (record.at.empty()) {
    record.at = now_iso8601();
  }
  if (!file_path.parent_path().empty()) {
    std::filesystem::create_directories(file_path.parent_path());
  }
  std::ofstream output(file_path, std::ios::binary | std::ios::app);
  if (!output) {
    throw ConfigurationError("Unable to append audit log: " + file_path.string());
  }
  output << audit_record_to_value(record).stringify(0) << '\n';
}
