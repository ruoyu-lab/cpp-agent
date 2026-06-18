std::string pg_approval_identifier(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

Value pg_approval_json_param(const Value& value) {
  return value.is_null() ? Value() : Value(value.stringify(0));
}

Value pg_approval_json_from_value(const Value& value) {
  if (!value.is_string()) {
    return value;
  }
  try {
    return parse_json(value.as_string());
  } catch (...) {
    return value;
  }
}

ApprovalRecord approval_from_pg_row(const Value& row) {
  Value value = Value::object({
      {"id", row.at("id").as_string()},
      {"createdAt", row.at("created_at").as_string()},
      {"resolvedAt", row.at("resolved_at").is_null() ? Value() : Value(row.at("resolved_at").as_string())},
      {"status", row.at("status").as_string("pending")},
      {"request", pg_approval_json_from_value(row.at("request"))},
      {"proposedDecision", pg_approval_json_from_value(row.at("proposed_decision"))},
      {"finalDecision", row.at("final_decision").is_null()
                            ? Value()
                            : pg_approval_json_from_value(row.at("final_decision"))},
      {"metadata", row.at("metadata").is_null() ? Value::object({}) : pg_approval_json_from_value(row.at("metadata"))},
  });
  return approval_record_from_value(value);
}

const Value& require_pg_approval_row(const std::vector<Value>& rows, const std::string& message) {
  if (rows.empty()) {
    throw ConfigurationError(message);
  }
  return rows.front();
}

}  // namespace

struct ApprovalStoreState {
  mutable std::mutex mutex;
  std::map<std::string, ApprovalRecord> records;
};

struct PendingApprovalResolution {
  std::mutex mutex;
  std::condition_variable ready;
  bool resolved = false;
  PermissionDecision decision;
};

struct ManualApprovalQueueState {
  std::shared_ptr<ApprovalStore> owned_store;
  ApprovalStore* store = nullptr;
  mutable std::mutex mutex;
  std::map<std::string, std::shared_ptr<PendingApprovalResolution>> pending;
};

HttpRequestError::HttpRequestError(int status_code, std::string message, Value payload)
    : AgentFrameworkError(std::move(message)),
      status_code_(status_code),
      payload_(payload.is_object() ? std::move(payload) : Value::object({})) {}

int HttpRequestError::status_code() const noexcept {
  return status_code_;
}

const Value& HttpRequestError::payload() const noexcept {
  return payload_;
}
