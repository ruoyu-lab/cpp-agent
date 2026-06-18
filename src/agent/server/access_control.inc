struct TaskAccessFields {
  std::string owner_api_key_id;
  std::string tenant_id;
  Value metadata = Value::object({});
};

std::string access_scope_or_throw(const std::string& scope, const std::string& runtime_name) {
  if (scope.empty() || scope == "api-key") {
    return "api-key";
  }
  if (scope == "tenant") {
    return "tenant";
  }
  throw HttpRequestError(500, "Invalid " + runtime_name + " access scope.",
                         Value::object({{"error", "Invalid " + runtime_name + " access scope."},
                                        {"scope", scope}}));
}

std::string task_tenant_metadata_key(const AgentServerOptions& options) {
  if (!options.tasks || options.tasks->access.tenant_metadata_key.empty()) {
    return "tenantId";
  }
  return options.tasks->access.tenant_metadata_key;
}

std::string async_run_tenant_metadata_key(const AgentServerOptions& options) {
  if (!options.async_runs || options.async_runs->access.tenant_metadata_key.empty()) {
    return "tenantId";
  }
  return options.async_runs->access.tenant_metadata_key;
}

std::string autonomous_tenant_metadata_key(const AgentServerOptions& options) {
  if (!options.autonomous || options.autonomous->access.tenant_metadata_key.empty()) {
    return "tenantId";
  }
  return options.autonomous->access.tenant_metadata_key;
}

std::string api_key_metadata_string(const AgentServerApiKey& api_key,
                                    const std::string& key) {
  if (!api_key.metadata.is_object() || !api_key.metadata.contains(key)) {
    return {};
  }
  const auto& value = api_key.metadata.at(key);
  if (!value.is_string() || value.as_string().empty()) {
    throw HttpRequestError(500, "API key metadata \"" + key + "\" must be a non-empty string.",
                           Value::object({{"error", "API key metadata \"" + key +
                                                       "\" must be a non-empty string."}}));
  }
  return value.as_string();
}

std::string task_api_key_tenant_id(const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  if (!access.api_key) {
    return {};
  }
  return api_key_metadata_string(*access.api_key, task_tenant_metadata_key(options));
}

std::string async_run_api_key_tenant_id(const AgentServerAccessContext& access,
                                        const AgentServerOptions& options) {
  if (!access.api_key) {
    return {};
  }
  return api_key_metadata_string(*access.api_key, async_run_tenant_metadata_key(options));
}

std::string autonomous_api_key_tenant_id(const AgentServerAccessContext& access,
                                         const AgentServerOptions& options) {
  if (!access.api_key) {
    return {};
  }
  return api_key_metadata_string(*access.api_key, autonomous_tenant_metadata_key(options));
}

void assert_async_run_route_access(const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  if (options.async_runs && options.async_runs->access.require_api_key && !access.api_key) {
    throw HttpRequestError(403, "Async run runtime requires API key authentication.",
                           Value::object({{"error", "Async run runtime requires API key authentication."}}));
  }
}

AsyncAgentRunFilter async_run_access_filter(const AgentServerAccessContext& access,
                                            const AgentServerOptions& options) {
  AsyncAgentRunFilter filter;
  if (!access.api_key) {
    return filter;
  }
  const auto scope = access_scope_or_throw(options.async_runs ? options.async_runs->access.scope : "api-key",
                                           "async run");
  if (scope == "tenant") {
    const auto tenant_id = async_run_api_key_tenant_id(access, options);
    if (tenant_id.empty()) {
      throw HttpRequestError(403, "Async run tenant scope requires API key tenant metadata.",
                             Value::object({{"error", "Async run tenant scope requires API key tenant metadata."},
                                            {"field", async_run_tenant_metadata_key(options)}}));
    }
    filter.tenant_id = tenant_id;
    return filter;
  }
  filter.owner_api_key_id = access.api_key->id;
  return filter;
}

void merge_async_run_access_filter(AsyncAgentRunFilter& target, AsyncAgentRunFilter source) {
  if (!source.owner_api_key_id.empty()) {
    target.owner_api_key_id = std::move(source.owner_api_key_id);
  }
  if (!source.tenant_id.empty()) {
    target.tenant_id = std::move(source.tenant_id);
  }
}

bool can_access_async_run_snapshot(const AsyncAgentRunSnapshot& snapshot,
                                   const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  if (!access.api_key) {
    return true;
  }
  const auto scope = access_scope_or_throw(options.async_runs ? options.async_runs->access.scope : "api-key",
                                           "async run");
  if (scope == "tenant") {
    const auto tenant_id = async_run_api_key_tenant_id(access, options);
    return !tenant_id.empty() && snapshot.run.tenant_id == tenant_id;
  }
  return snapshot.run.owner_api_key_id == access.api_key->id;
}

void assert_async_run_snapshot_access(const AsyncAgentRunSnapshot& snapshot,
                                      const AgentServerAccessContext& access,
                                      const AgentServerOptions& options,
                                      const std::string& run_id) {
  if (!can_access_async_run_snapshot(snapshot, access, options)) {
    throw HttpRequestError(404, "Async agent run not found: " + run_id,
                           Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
}

TaskAccessFields build_async_run_access_fields(Value metadata,
                                               const AgentServerAccessContext& access,
                                               const AgentServerOptions& options) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  TaskAccessFields fields{.metadata = std::move(metadata)};
  if (!access.api_key) {
    return fields;
  }
  const auto tenant_key = async_run_tenant_metadata_key(options);
  const auto tenant_id = async_run_api_key_tenant_id(access, options);
  if (tenant_id.empty() && options.async_runs && options.async_runs->access.scope == "tenant") {
    throw HttpRequestError(403, "Async run tenant scope requires API key tenant metadata.",
                           Value::object({{"error", "Async run tenant scope requires API key tenant metadata."},
                                          {"field", tenant_key}}));
  }
  if (!tenant_id.empty()) {
    fields.tenant_id = tenant_id;
    fields.metadata[tenant_key] = tenant_id;
  }
  fields.owner_api_key_id = access.api_key->id;
  fields.metadata["ownerApiKeyId"] = access.api_key->id;
  return fields;
}

void assert_task_route_access(const AgentServerAccessContext& access,
                              const AgentServerOptions& options) {
  if (options.tasks && options.tasks->access.require_api_key && !access.api_key) {
    throw HttpRequestError(403, "Task runtime requires API key authentication.",
                           Value::object({{"error", "Task runtime requires API key authentication."}}));
  }
}

TaskScopeFilter task_access_filter(const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  TaskScopeFilter filter;
  if (!access.api_key) {
    return filter;
  }
  const auto scope = access_scope_or_throw(options.tasks ? options.tasks->access.scope : "api-key", "task");
  if (scope == "tenant") {
    const auto tenant_id = task_api_key_tenant_id(access, options);
    if (tenant_id.empty()) {
      throw HttpRequestError(403, "Task tenant scope requires API key tenant metadata.",
                             Value::object({{"error", "Task tenant scope requires API key tenant metadata."},
                                            {"field", task_tenant_metadata_key(options)}}));
    }
    filter.tenant_id = tenant_id;
    return filter;
  }
  filter.owner_api_key_id = access.api_key->id;
  return filter;
}

void merge_task_access_filter(TaskScopeFilter& target, TaskScopeFilter source) {
  if (source.owner_api_key_id) {
    target.owner_api_key_id = std::move(source.owner_api_key_id);
  }
  if (source.tenant_id) {
    target.tenant_id = std::move(source.tenant_id);
  }
}

bool can_access_task_snapshot(const TaskStoreSnapshot& snapshot,
                              const AgentServerAccessContext& access,
                              const AgentServerOptions& options) {
  if (!access.api_key) {
    return true;
  }
  const auto scope = access_scope_or_throw(options.tasks ? options.tasks->access.scope : "api-key", "task");
  if (scope == "tenant") {
    const auto tenant_id = task_api_key_tenant_id(access, options);
    return !tenant_id.empty() && snapshot.task.tenant_id == tenant_id;
  }
  return snapshot.task.owner_api_key_id == access.api_key->id;
}

void assert_task_snapshot_access(const TaskStoreSnapshot& snapshot,
                                 const AgentServerAccessContext& access,
                                 const AgentServerOptions& options,
                                 const std::string& task_id) {
  if (!can_access_task_snapshot(snapshot, access, options)) {
    throw HttpRequestError(404, "Task not found: " + task_id,
                           Value::object({{"error", "Task not found: " + task_id}}));
  }
}

TaskAccessFields build_task_access_fields(Value metadata,
                                          const AgentServerAccessContext& access,
                                          const AgentServerOptions& options) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  TaskAccessFields fields{.metadata = std::move(metadata)};
  if (!access.api_key) {
    return fields;
  }

  if (fields.metadata.contains("ownerApiKeyId") &&
      fields.metadata.at("ownerApiKeyId") != Value(access.api_key->id)) {
    throw HttpRequestError(403, "Task metadata \"ownerApiKeyId\" must match API key owner.",
                           Value::object({{"error", "Task metadata \"ownerApiKeyId\" must match API key owner."},
                                          {"field", "ownerApiKeyId"}}));
  }

  const auto tenant_key = task_tenant_metadata_key(options);
  const auto tenant_id = task_api_key_tenant_id(access, options);
  if (!tenant_id.empty() && fields.metadata.contains(tenant_key) &&
      fields.metadata.at(tenant_key) != Value(tenant_id)) {
    throw HttpRequestError(403, "Task metadata \"" + tenant_key + "\" must match API key tenant.",
                           Value::object({{"error", "Task metadata \"" + tenant_key +
                                                       "\" must match API key tenant."},
                                          {"field", tenant_key}}));
  }

  fields.owner_api_key_id = access.api_key->id;
  fields.tenant_id = tenant_id;
  fields.metadata["ownerApiKeyId"] = access.api_key->id;
  if (!tenant_id.empty()) {
    fields.metadata[tenant_key] = tenant_id;
  }
  return fields;
}

void assert_autonomous_route_access(const AgentServerAccessContext& access,
                                    const AgentServerOptions& options) {
  if (options.autonomous && options.autonomous->access.require_api_key && !access.api_key) {
    throw HttpRequestError(403, "Autonomous runtime requires API key authentication.",
                           Value::object({{"error", "Autonomous runtime requires API key authentication."}}));
  }
}

Value build_autonomous_access_metadata(Value metadata,
                                       const AgentServerAccessContext& access,
                                       const AgentServerOptions& options) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  if (!access.api_key) {
    return metadata;
  }
  if (metadata.contains("ownerApiKeyId") && metadata.at("ownerApiKeyId") != Value(access.api_key->id)) {
    throw HttpRequestError(
        403,
        "Autonomous metadata ownerApiKeyId does not match authenticated API key.",
        Value::object({{"error", "Autonomous metadata ownerApiKeyId does not match authenticated API key."}}));
  }
  const auto tenant_key = autonomous_tenant_metadata_key(options);
  const auto tenant_id = autonomous_api_key_tenant_id(access, options);
  if (!tenant_id.empty() && metadata.contains(tenant_key) && metadata.at(tenant_key) != Value(tenant_id)) {
    throw HttpRequestError(403,
                           "Autonomous metadata " + tenant_key +
                               " does not match authenticated API key.",
                           Value::object({{"error", "Autonomous metadata " + tenant_key +
                                                       " does not match authenticated API key."},
                                          {"field", tenant_key}}));
  }
  metadata["ownerApiKeyId"] = access.api_key->id;
  if (!tenant_id.empty()) {
    metadata[tenant_key] = tenant_id;
  }
  return metadata;
}

Value autonomous_access_filter(const AgentServerAccessContext& access,
                               const AgentServerOptions& options) {
  if (!access.api_key) {
    return Value::object({});
  }
  const auto scope = access_scope_or_throw(options.autonomous ? options.autonomous->access.scope : "api-key",
                                           "autonomous");
  if (scope == "tenant") {
    const auto tenant_id = autonomous_api_key_tenant_id(access, options);
    if (tenant_id.empty()) {
      throw HttpRequestError(403, "Autonomous tenant scope requires API key tenant metadata.",
                             Value::object({{"error", "Autonomous tenant scope requires API key tenant metadata."},
                                            {"field", autonomous_tenant_metadata_key(options)}}));
    }
    return Value::object({{autonomous_tenant_metadata_key(options), tenant_id}});
  }
  return Value::object({{"ownerApiKeyId", access.api_key->id}});
}

bool can_access_autonomous_snapshot(const AutonomousRunSnapshot& snapshot,
                                    const AgentServerAccessContext& access,
                                    const AgentServerOptions& options) {
  if (!access.api_key) {
    return true;
  }
  const auto scope = access_scope_or_throw(options.autonomous ? options.autonomous->access.scope : "api-key",
                                           "autonomous");
  if (scope == "tenant") {
    const auto tenant_id = autonomous_api_key_tenant_id(access, options);
    const auto tenant_key = autonomous_tenant_metadata_key(options);
    return !tenant_id.empty() &&
           snapshot.run.metadata.is_object() &&
           snapshot.run.metadata.at(tenant_key) == Value(tenant_id);
  }
  return snapshot.run.metadata.is_object() &&
         snapshot.run.metadata.at("ownerApiKeyId") == Value(access.api_key->id);
}

void assert_autonomous_snapshot_access(const AutonomousRunSnapshot& snapshot,
                                       const AgentServerAccessContext& access,
                                       const AgentServerOptions& options,
                                       const std::string& run_id) {
  if (!can_access_autonomous_snapshot(snapshot, access, options)) {
    throw HttpRequestError(404, "Autonomous run not found: " + run_id,
                           Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
}
