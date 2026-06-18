InMemoryApprovalStore::InMemoryApprovalStore() : state_(std::make_shared<ApprovalStoreState>()) {}

ApprovalRecord InMemoryApprovalStore::create(CreateApprovalRecordInput input) {
  ApprovalRecord record;
  record.id = input.id.empty() ? generate_uuid() : std::move(input.id);
  record.created_at = now_iso8601();
  record.status = ApprovalRecordStatus::Pending;
  record.request = std::move(input.request);
  record.proposed_decision = std::move(input.proposed_decision);
  record.metadata = input.metadata.is_object() ? std::move(input.metadata) : Value::object({});

  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->records.find(record.id) != state_->records.end()) {
    throw ConfigurationError("Approval already exists: " + record.id);
  }
  state_->records[record.id] = record;
  return record;
}


std::optional<ApprovalRecord> InMemoryApprovalStore::get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->records.find(id);
  if (found == state_->records.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<ApprovalRecord> InMemoryApprovalStore::list(ApprovalRecordFilter filter) const {
  std::vector<ApprovalRecord> records;
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const auto& [_, record] : state_->records) {
    if (!filter.status || record.status == *filter.status) {
      records.push_back(record);
    }
  }
  std::sort(records.begin(), records.end(), [](const ApprovalRecord& left, const ApprovalRecord& right) {
    return left.created_at < right.created_at;
  });
  return records;
}

std::optional<ApprovalRecord> InMemoryApprovalStore::resolve(const std::string& id,
                                                             PermissionDecision final_decision) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto found = state_->records.find(id);
  if (found == state_->records.end()) {
    return std::nullopt;
  }
  if (found->second.status == ApprovalRecordStatus::Resolved) {
    return found->second;
  }
  found->second.status = ApprovalRecordStatus::Resolved;
  found->second.resolved_at = now_iso8601();
  found->second.final_decision = std::move(final_decision);
  return found->second;
}

FileApprovalStore::FileApprovalStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

ApprovalRecord FileApprovalStore::create(CreateApprovalRecordInput input) {
  ensure_loaded();
  auto record = InMemoryApprovalStore::create(std::move(input));
  persist();
  return record;
}

std::optional<ApprovalRecord> FileApprovalStore::get(const std::string& id) const {
  ensure_loaded();
  return InMemoryApprovalStore::get(id);
}

std::vector<ApprovalRecord> FileApprovalStore::list(ApprovalRecordFilter filter) const {
  ensure_loaded();
  return InMemoryApprovalStore::list(std::move(filter));
}

std::optional<ApprovalRecord> FileApprovalStore::resolve(const std::string& id,
                                                         PermissionDecision final_decision) {
  ensure_loaded();
  auto record = InMemoryApprovalStore::resolve(id, std::move(final_decision));
  persist();
  return record;
}

const std::filesystem::path& FileApprovalStore::file_path() const noexcept {
  return file_path_;
}

void FileApprovalStore::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  if (!std::filesystem::exists(file_path_)) {
    return;
  }
  const auto root = read_json_file(file_path_);
  const auto& items = root.is_array() ? root.as_array() : root.at("records").as_array();
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->records.clear();
  for (const auto& item : items) {
    auto record = approval_record_from_value(item);
    const auto record_id = record.id;
    state_->records[record_id] = std::move(record);
  }
}

void FileApprovalStore::persist() const {
  Value::Array records;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto& [_, record] : state_->records) {
      records.push_back(approval_record_to_value(record));
    }
  }
  write_json_file(file_path_, Value(std::move(records)));
}

PgApprovalStore::PgApprovalStore(PgApprovalStoreConfig config)
    : client_(std::move(config.client)),
      schema_name_(config.schema_name.empty() ? "public" : std::move(config.schema_name)),
      table_name_(config.table_name.empty() ? "node_agent_approvals" : std::move(config.table_name)),
      create_table_(config.create_table),
      close_client_on_close_(config.close_client_on_close) {
  if (!client_) {
    throw ConfigurationError("PgApprovalStore requires an injected PgApprovalClient.");
  }
}

PgApprovalStore::~PgApprovalStore() {
  close();
}

ApprovalRecord PgApprovalStore::create(CreateApprovalRecordInput input) {
  ensure_ready();
  const auto id = input.id.empty() ? generate_uuid() : std::move(input.id);
  const auto created_at = now_iso8601();
  const auto request = permission_request_to_value(input.request);
  const auto proposed_decision = permission_decision_to_value(input.proposed_decision);
  const auto metadata = input.metadata.is_object() ? input.metadata : Value::object({});
  const auto rows = client_->query(
      "INSERT INTO " + table() + " ("
      "id, created_at, resolved_at, status, request, proposed_decision, final_decision, metadata"
      ") VALUES ($1, $2, NULL, 'pending', $3::jsonb, $4::jsonb, NULL, $5::jsonb) "
      "RETURNING *",
      {id, created_at, pg_approval_json_param(request), pg_approval_json_param(proposed_decision),
       pg_approval_json_param(metadata)});
  return approval_from_pg_row(require_pg_approval_row(rows, "Approval insert returned no row: " + id));
}

std::optional<ApprovalRecord> PgApprovalStore::get(const std::string& id) const {
  ensure_ready();
  const auto rows = client_->query("SELECT * FROM " + table() + " WHERE id = $1", {id});
  if (rows.empty()) {
    return std::nullopt;
  }
  return approval_from_pg_row(rows.front());
}

std::vector<ApprovalRecord> PgApprovalStore::list(ApprovalRecordFilter filter) const {
  ensure_ready();
  std::vector<Value> params;
  std::string where;
  if (filter.status) {
    where = " WHERE status = $1";
    params.push_back(to_string(*filter.status));
  }
  const auto rows = client_->query("SELECT * FROM " + table() + where + " ORDER BY created_at ASC", params);
  std::vector<ApprovalRecord> records;
  records.reserve(rows.size());
  for (const auto& row : rows) {
    records.push_back(approval_from_pg_row(row));
  }
  return records;
}

std::optional<ApprovalRecord> PgApprovalStore::resolve(const std::string& id,
                                                       PermissionDecision final_decision) {
  ensure_ready();
  const auto resolved_at = now_iso8601();
  const auto final_decision_value = permission_decision_to_value(final_decision);
  const auto rows = client_->query(
      "UPDATE " + table() +
          " SET status = 'resolved', resolved_at = $2, final_decision = $3::jsonb "
          "WHERE id = $1 AND status = 'pending' RETURNING *",
      {id, resolved_at, pg_approval_json_param(final_decision_value)});
  if (!rows.empty()) {
    return approval_from_pg_row(rows.front());
  }
  return get(id);
}

void PgApprovalStore::close() {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (!close_client_on_close_ || !client_) {
    return;
  }
  client_->close();
  close_client_on_close_ = false;
}

std::string PgApprovalStore::table() const {
  return pg_approval_identifier(schema_name_) + "." + pg_approval_identifier(table_name_);
}

void PgApprovalStore::ensure_ready() const {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (ready_ || !create_table_) {
    return;
  }
  client_->query("CREATE SCHEMA IF NOT EXISTS " + pg_approval_identifier(schema_name_));
  client_->query("CREATE TABLE IF NOT EXISTS " + table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "created_at TEXT NOT NULL,"
                 "resolved_at TEXT,"
                 "status TEXT NOT NULL,"
                 "request JSONB NOT NULL,"
                 "proposed_decision JSONB NOT NULL,"
                 "final_decision JSONB,"
                 "metadata JSONB"
                 ")");
  client_->query("CREATE INDEX IF NOT EXISTS " + pg_approval_identifier(table_name_ + "_status_idx") +
                 " ON " + table() + " (status, created_at)");
  ready_ = true;
}

ManualApprovalQueue::ManualApprovalQueue(ManualApprovalQueueConfig config)
    : state_(std::make_shared<ManualApprovalQueueState>()) {
  if (config.store) {
    state_->store = config.store;
  } else {
    state_->owned_store = std::make_shared<InMemoryApprovalStore>();
    state_->store = state_->owned_store.get();
  }
}

PermissionDecision ManualApprovalQueue::approve(const PermissionRequest& request,
                                                const PermissionDecision& decision) {
  auto pending = std::make_shared<PendingApprovalResolution>();
  ApprovalRecord record;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    record = state_->store->create(CreateApprovalRecordInput{
        .request = request,
        .proposed_decision = decision,
    });
    state_->pending[record.id] = pending;
  }

  std::unique_lock<std::mutex> lock(pending->mutex);
  pending->ready.wait(lock, [&pending]() {
    return pending->resolved;
  });
  return pending->decision;
}

PermissionApprovalHandler ManualApprovalQueue::handler() {
  return [this](const PermissionRequest& request, const PermissionDecision& decision) {
    return approve(request, decision);
  };
}

std::vector<ApprovalRecord> ManualApprovalQueue::list(ApprovalRecordFilter filter) const {
  return state_->store->list(std::move(filter));
}

std::optional<ApprovalRecord> ManualApprovalQueue::get(const std::string& id) const {
  return state_->store->get(id);
}

std::optional<ApprovalRecord> ManualApprovalQueue::resolve(const std::string& id,
                                                           PermissionDecision final_decision) {
  auto record = state_->store->resolve(id, final_decision);
  if (!record) {
    return std::nullopt;
  }

  std::shared_ptr<PendingApprovalResolution> pending;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->pending.find(id);
    if (found != state_->pending.end()) {
      pending = found->second;
      state_->pending.erase(found);
    }
  }
  if (pending) {
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      pending->decision = record->final_decision.value_or(std::move(final_decision));
      pending->resolved = true;
    }
    pending->ready.notify_all();
  }
  return record;
}

ApprovalStore& ManualApprovalQueue::store() const {
  return *state_->store;
}

PermissionApprovalHandler create_manual_approval_handler(ManualApprovalQueue& queue) {
  return queue.handler();
}

Value agent_server_metrics_bucket_to_value(const AgentServerMetricsBucket& bucket) {
  return Value::object({{"requests", bucket.requests},
                        {"succeeded", bucket.succeeded},
                        {"failed", bucket.failed},
                        {"rejected", bucket.rejected},
                        {"durationMs", bucket.duration_ms},
                        {"statusCodes", status_codes_to_value(bucket.status_codes)},
                        {"inputTokens", bucket.input_tokens},
                        {"outputTokens", bucket.output_tokens},
                        {"totalTokens", bucket.total_tokens},
                        {"estimatedCostByCurrency", double_map_to_value(bucket.estimated_cost_by_currency)}});
}

AgentServerMetricsCollector::AgentServerMetricsCollector(bool enabled,
                                                         std::map<std::string, UsagePricing> pricing)
    : enabled_(enabled), pricing_(std::move(pricing)) {}

AgentServerMetricsBucket& AgentServerMetricsCollector::bucket_for(
    std::map<std::string, AgentServerMetricsBucket>& buckets,
    const std::string& key) {
  return buckets[key];
}
