HttpResponse AgentServerApp::handle_async_run_list(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  AsyncAgentRunFilter filter;
  filter.status = async_run_status_filter(context.query);
  merge_async_run_access_filter(filter, async_run_access_filter(context.access, options_));
  Value::Array items;
  for (const auto& run : options_.async_runs->controller->list(filter)) {
    items.push_back(async_agent_run_to_value(run));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_async_run_create(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  auto body = read_json_body(context.request);
  if (!body.contains("input")) {
    throw HttpRequestError(400, "Request body must include input.",
                           Value::object({{"error", "Request body must include input."}}));
  }
  const auto agent_id = resolve_authorized_agent_id(context.access, optional_string_field(body, "agent"));
  if (!agent_id.empty()) {
    body["agent"] = agent_id;
  }
  body["sessionId"] = resolve_session_id(body);
  Value metadata = optional_object_field(body, "metadata");
  metadata["source"] = "server";
  metadata["requestId"] = context.access.request_id;
  if (!agent_id.empty()) {
    metadata["agent"] = agent_id;
  }
  auto access_fields = build_async_run_access_fields(std::move(metadata), context.access, options_);
  body["metadata"] = access_fields.metadata;
  if (!access_fields.owner_api_key_id.empty()) {
    body["ownerApiKeyId"] = access_fields.owner_api_key_id;
  }
  if (!access_fields.tenant_id.empty()) {
    body["tenantId"] = access_fields.tenant_id;
  }
  auto snapshot = options_.async_runs->controller->start(async_agent_run_start_input_from_value(body));
  append_audit(AuditRecord{
      .type = "async_run.created",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = snapshot.run.id,
      .status_code = 202,
      .detail = Value::object({{"runId", snapshot.run.id}}),
  });
  auto payload = async_agent_run_snapshot_to_value(snapshot);
  payload["idempotent"] = false;
  add_trace_payload(payload, context);
  return send_json(202, payload);
}

HttpResponse AgentServerApp::handle_async_run_get(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  return send_json(200, async_agent_run_snapshot_to_value(*snapshot));
}

HttpResponse AgentServerApp::handle_async_run_events(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  Value::Array items;
  for (const auto& event : snapshot->events) {
    items.push_back(async_agent_run_event_to_value(event));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_async_run_checkpoints(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  Value::Array items;
  for (const auto& checkpoint : snapshot->checkpoints) {
    items.push_back(async_agent_run_checkpoint_to_value(checkpoint));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_async_run_transcript(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  Value::Array items;
  for (const auto& entry : options_.async_runs->controller->transcript(run_id)) {
    items.push_back(run_transcript_entry_to_value(entry));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_async_run_resume(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  const auto body = context.request.body.empty() ? Value::object({}) : read_json_body(context.request);
  auto payload = async_agent_run_snapshot_to_value(
      options_.async_runs->controller->resume(run_id, async_agent_run_resume_input_from_value(body)));
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_async_run_cancel(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller) {
    throw HttpRequestError(404, "Async run runtime is not configured.",
                           Value::object({{"error", "Async run runtime is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.async_runs->controller->get(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Async agent run not found: " + run_id}}));
  }
  assert_async_run_snapshot_access(*snapshot, context.access, options_, run_id);
  const auto body = context.request.body.empty() ? Value::object({}) : read_json_body(context.request);
  auto payload = async_agent_run_snapshot_to_value(
      options_.async_runs->controller->cancel(run_id, async_agent_run_cancel_input_from_value(body)));
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_async_run_work_once(const AgentServerRequestContext& context) {
  if (!options_.async_runs || !options_.async_runs->controller || !options_.async_runs->worker) {
    throw HttpRequestError(404, "Async run worker is not configured.",
                           Value::object({{"error", "Async run worker is not configured."}}));
  }
  assert_async_run_route_access(context.access, options_);
  const bool processed = options_.async_runs->controller->run_once();
  return send_json(200, Value::object({{"processed", processed}}));
}
