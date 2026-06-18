HttpResponse AgentServerApp::handle_autonomous_list(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  AutonomousRunFilter filter;
  filter.status = autonomous_status_filter(context.query);
  filter.metadata = autonomous_access_filter(context.access, options_);
  Value::Array items;
  for (const auto& run : options_.autonomous->store->list_runs(filter)) {
    items.push_back(autonomous_run_to_value(run));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_autonomous_create(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  auto body = read_json_body(context.request);
  const std::string raw_goal = optional_string_field(body, "goal");
  const auto goal = ::agent::trim_copy(raw_goal);
  if (goal.empty()) {
    throw HttpRequestError(400, "Request body field \"goal\" must be a non-empty string.",
                           Value::object({{"error", "Request body field \"goal\" must be a non-empty string."},
                                          {"field", "goal"}}));
  }
  const auto auto_start = optional_bool_field_strict(body, "autoStart");
  Value metadata = optional_object_field_strict(body, "metadata");
  metadata = build_autonomous_access_metadata(std::move(metadata), context.access, options_);
  auto run = options_.autonomous->manager->create_run(CreateAutonomousRunInput{
      .id = optional_string_field_strict(body, "id"),
      .goal = goal,
      .input = body.at("input"),
      .metadata = std::move(metadata),
  });
  append_audit(AuditRecord{
      .type = "autonomous.run.created",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run.id}}),
  });
  auto snapshot = auto_start ? options_.autonomous->manager->run(run.id)
                             : *options_.autonomous->store->get_run(run.id);
  auto payload = autonomous_run_snapshot_to_value(snapshot);
  add_trace_payload(payload, context);
  return send_json(auto_start ? 200 : 202, payload);
}

HttpResponse AgentServerApp::handle_autonomous_get(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  return send_json(200, autonomous_run_snapshot_to_value(*snapshot));
}

HttpResponse AgentServerApp::handle_autonomous_resume(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto payload = autonomous_run_snapshot_to_value(
      options_.autonomous->manager->resume(run_id));
  append_audit(AuditRecord{
      .type = "autonomous.run.resumed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id},
                               {"status", payload.at("run").at("status")}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_autonomous_cancel(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto body = read_json_body(context.request);
  const auto reason = optional_reason_field(body);
  auto payload = autonomous_run_snapshot_to_value(reason ? options_.autonomous->manager->cancel(run_id, *reason)
                                                        : options_.autonomous->manager->cancel(run_id));
  append_audit(AuditRecord{
      .type = "autonomous.run.cancelled",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_autonomous_complete_step(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto body = read_json_body(context.request);
  auto payload = autonomous_run_snapshot_to_value(
      options_.autonomous->manager->complete_waiting_step(
          run_id,
          context.params.at("stepId"),
          body.at("output")));
  append_audit(AuditRecord{
      .type = "autonomous.step.completed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id},
                               {"stepId", context.params.at("stepId")}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}
