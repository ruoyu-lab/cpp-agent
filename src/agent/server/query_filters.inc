std::optional<TaskStatus> task_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  const auto& value = found->second;
  if (value == "queued") return TaskStatus::Queued;
  if (value == "running") return TaskStatus::Running;
  if (value == "completed") return TaskStatus::Completed;
  if (value == "failed") return TaskStatus::Failed;
  if (value == "cancelled") return TaskStatus::Cancelled;
  if (value == "interrupted") return TaskStatus::Interrupted;
  if (value == "waiting") return TaskStatus::Waiting;
  throw HttpRequestError(400, "Invalid task status filter.",
                         Value::object({{"error", "Invalid task status filter."}, {"status", value}}));
}

std::optional<AutonomousRunStatus> autonomous_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  const auto& value = found->second;
  if (value == "queued" || value == "running" || value == "waiting" || value == "completed" ||
      value == "failed" || value == "cancelled" || value == "interrupted") {
    return autonomous_run_status_from_string(value);
  }
  throw HttpRequestError(400, "Invalid autonomous run status filter.",
                         Value::object({{"error", "Invalid autonomous run status filter."},
                                        {"status", value}}));
}

std::optional<AsyncAgentRunStatus> async_run_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  const auto& value = found->second;
  if (value == "queued" || value == "running" || value == "waiting" || value == "completed" ||
      value == "failed" || value == "cancelled") {
    return async_agent_run_status_from_string(value);
  }
  throw HttpRequestError(400, "Invalid async run status filter.",
                         Value::object({{"error", "Invalid async run status filter."},
                                        {"status", value}}));
}

std::optional<ApprovalRecordStatus> approval_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  if (found->second == "pending") return ApprovalRecordStatus::Pending;
  if (found->second == "resolved") return ApprovalRecordStatus::Resolved;
  throw HttpRequestError(400, "Invalid approval status filter.",
                         Value::object({{"error", "Invalid approval status filter."},
                                        {"status", found->second}}));
}
