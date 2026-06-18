HttpResponse AgentServerApp::handle_task_list(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  TaskScopeFilter filter;
  filter.status = task_status_filter(context.query);
  const auto type = context.query.find("type");
  if (type != context.query.end()) {
    filter.type = type->second;
  }
  merge_task_access_filter(filter, task_access_filter(context.access, options_));
  Value::Array items;
  for (const auto& task : options_.tasks->store->list_tasks(filter)) {
    items.push_back(agent_task_to_value(task));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_task_create(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  auto body = read_json_body(context.request);
  if (!body.contains("input")) {
    throw HttpRequestError(400, "Request body must include input.",
                           Value::object({{"error", "Request body must include input."}}));
  }
  const auto requested_type = optional_string_field(body, "type");
  const auto type = requested_type.empty() ? options_.tasks->default_type : requested_type;
  const auto session_id = resolve_session_id(body);
  const auto model_settings = optional_object_field(body, "modelSettings");
  validate_server_model_settings(model_settings);
  const auto idempotency_header_name = options_.tasks->idempotency.header_name.empty()
                                           ? std::string("Idempotency-Key")
                                           : options_.tasks->idempotency.header_name;
  const auto idempotency_header_value = header_value(context.request, idempotency_header_name);
  const auto idempotency_body_value = optional_string_field(body, "idempotencyKey");
  if (!idempotency_header_value.empty() && !idempotency_body_value.empty() &&
      idempotency_header_value != idempotency_body_value) {
    throw HttpRequestError(400, "Task idempotency key header and body value must match.",
                           Value::object({{"error", "Task idempotency key header and body value must match."},
                                          {"headerName", idempotency_header_name}}));
  }
  const auto idempotency_key =
      idempotency_header_value.empty() ? idempotency_body_value : idempotency_header_value;
  if (options_.tasks->idempotency.require_key && idempotency_key.empty()) {
    throw HttpRequestError(400, "Task creation requires an idempotency key.",
                           Value::object({{"error", "Task creation requires an idempotency key."},
                                          {"headerName", idempotency_header_name}}));
  }
  const auto agent_id = resolve_authorized_agent_id(context.access, optional_string_field(body, "agent"));
  if (!idempotency_key.empty()) {
    TaskScopeFilter idempotency_filter{.type = type};
    merge_task_access_filter(idempotency_filter, task_access_filter(context.access, options_));
    auto existing = options_.tasks->store->find_task_by_idempotency_key(idempotency_key, idempotency_filter);
    if (existing) {
      auto payload = task_store_snapshot_to_value(*existing);
      payload["idempotent"] = true;
      add_trace_payload(payload, context);
      return send_json(200, payload);
    }
  }
  const auto task_id = optional_string_field(body, "id").empty()
                           ? generate_uuid()
                           : optional_string_field(body, "id");
  Value task_details = Value::object({{"taskId", task_id}});
  if (!agent_id.empty()) {
    task_details["agentId"] = agent_id;
  }
  Value task_context = merge_server_execution_context(optional_object_field(body, "context"),
                                                      context,
                                                      "task.create",
                                                      std::move(task_details));
  Value input = Value::object({{"input", body.at("input")},
                               {"sessionId", session_id},
                               {"agent", agent_id.empty()
                                             ? Value()
                                             : Value(agent_id)},
                               {"modelSettings", model_settings},
                               {"context", task_context}});
  Value metadata = optional_object_field(body, "metadata");
  metadata["source"] = "server";
  if (!idempotency_key.empty()) {
    metadata["idempotencyKey"] = idempotency_key;
  }
  auto access_fields = build_task_access_fields(std::move(metadata), context.access, options_);
  if (!agent_id.empty()) {
    access_fields.metadata["agent"] = agent_id;
  }
  auto task = options_.tasks->store->create_task(CreateTaskInput{
      .id = task_id,
      .type = type.empty() ? "agent.run" : type,
      .input = std::move(input),
      .idempotency_key = idempotency_key,
      .owner_api_key_id = access_fields.owner_api_key_id,
      .tenant_id = access_fields.tenant_id,
      .metadata = std::move(access_fields.metadata),
  });
  options_.tasks->queue->enqueue(task.id);
  options_.tasks->store->append_event(task.id, {}, "task.created",
                                      Value::object({{"source", "server"}, {"type", task.type}}));
  append_audit(AuditRecord{
      .type = "task.created",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task.id,
      .status_code = 202,
      .detail = Value::object({{"taskId", task.id}, {"taskType", task.type}}),
  });
  auto snapshot = options_.tasks->store->get_task(task.id);
  auto payload = snapshot ? task_store_snapshot_to_value(*snapshot) : Value::object({{"task", agent_task_to_value(task)}});
  payload["idempotent"] = false;
  add_trace_payload(payload, context);
  return send_json(202, payload);
}

HttpResponse AgentServerApp::handle_task_get(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  return send_json(200, task_store_snapshot_to_value(*snapshot));
}

HttpResponse AgentServerApp::handle_task_events(const AgentServerRequestContext& context) {
  auto response = handle_task_get(context);
  if (response.status != 200) {
    return response;
  }
  auto snapshot = parse_json(response.body);
  return send_json(200, Value::object({{"items", snapshot.at("events")}}));
}

HttpResponse AgentServerApp::handle_task_checkpoints(const AgentServerRequestContext& context) {
  auto response = handle_task_get(context);
  if (response.status != 200) {
    return response;
  }
  auto snapshot = parse_json(response.body);
  return send_json(200, Value::object({{"items", snapshot.at("checkpoints")}}));
}

HttpResponse AgentServerApp::handle_task_state(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  const TaskCheckpoint* runner_state = nullptr;
  for (auto it = snapshot->checkpoints.rbegin(); it != snapshot->checkpoints.rend(); ++it) {
    if (it->name == "runner.state") {
      runner_state = &(*it);
      break;
    }
  }
  const bool terminal = snapshot->task.status == TaskStatus::Completed ||
                        snapshot->task.status == TaskStatus::Cancelled ||
                        snapshot->task.status == TaskStatus::Running;
  return send_json(200, Value::object({{"taskId", task_id},
                                       {"status", to_string(snapshot->task.status)},
                                       {"runnerState", runner_state ? runner_state->state : Value()},
                                       {"checkpointId", runner_state ? Value(runner_state->id) : Value()},
                                       {"checkpointCreatedAtMs", runner_state ? time_point_to_ms(runner_state->created_at) : 0},
                                       {"resumable", runner_state != nullptr && !terminal},
                                       {"reason", runner_state == nullptr ? Value("Task has no runner.state checkpoint.") : Value()}}));
}

HttpResponse AgentServerApp::handle_task_resume(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  if (snapshot->task.status == TaskStatus::Completed || snapshot->task.status == TaskStatus::Cancelled ||
      snapshot->task.status == TaskStatus::Running) {
    throw HttpRequestError(409, "Task is not resumable.",
                           Value::object({{"error", "Task is not resumable."},
                                          {"taskId", task_id},
                                          {"status", to_string(snapshot->task.status)}}));
  }
  options_.tasks->store->update_task_status(task_id, TaskStatus::Queued);
  options_.tasks->queue->enqueue(task_id);
  options_.tasks->store->append_event(task_id, {}, "task.resumed", Value::object({{"source", "server"}}));
  append_audit(AuditRecord{
      .type = "task.resumed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task_id,
      .status_code = 202,
  });
  auto updated = options_.tasks->store->get_task(task_id);
  auto payload = updated ? task_store_snapshot_to_value(*updated) : Value::object({});
  payload["resumed"] = true;
  add_trace_payload(payload, context);
  return send_json(202, payload);
}

HttpResponse AgentServerApp::handle_task_cancel(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  if (options_.tasks->worker) {
    options_.tasks->worker->cancel(task_id);
  } else {
    options_.tasks->queue->cancel(task_id);
  }
  options_.tasks->store->append_event(task_id, {}, "task.cancelled", Value::object({{"source", "server"}}));
  append_audit(AuditRecord{
      .type = "task.cancelled",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task_id,
      .status_code = 200,
  });
  auto updated = options_.tasks->store->get_task(task_id);
  auto payload = updated ? task_store_snapshot_to_value(*updated) : Value::object({});
  payload["cancelled"] = true;
  add_trace_payload(payload, context);
  return send_json(200, payload);
}
