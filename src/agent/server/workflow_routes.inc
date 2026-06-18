HttpResponse AgentServerApp::handle_workflow_run(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  std::optional<WorkflowDefinition> definition;
  if (body.at("definition").is_object()) {
    definition = workflow_definition_from_value(body.at("definition"));
  } else if (workflow.definition) {
    definition = *workflow.definition;
  }
  if (!definition) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  Value run_context = merge_server_execution_context(optional_object_field(body, "context"),
                                                     context,
                                                     "workflow.run");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->run(*definition, body.at("input"), std::move(run_context)));
  append_audit(AuditRecord{
      .type = "workflow.run",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = payload.at("workflowRunId").as_string(),
      .status_code = 200,
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_get(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.store) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  const auto workflow_run_id = context.params.at("workflowRunId");
  auto state = workflow.store->get_run(workflow_run_id);
  if (!state) {
    return send_json(404, Value::object({{"error", "Workflow run not found: " + workflow_run_id}}));
  }
  return send_json(200, workflow_run_state_to_value(*state));
}

HttpResponse AgentServerApp::handle_workflow_checkpoints(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.store) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  const auto workflow_run_id = context.params.at("workflowRunId");
  auto state = workflow.store->get_run(workflow_run_id);
  if (!state) {
    return send_json(404, Value::object({{"error", "Workflow run not found: " + workflow_run_id}}));
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : state->checkpoints) {
    checkpoints.push_back(workflow_checkpoint_to_value(checkpoint));
  }
  return send_json(200, Value::object({{"workflowRunId", workflow_run_id},
                                      {"checkpoints", Value(std::move(checkpoints))}}));
}

HttpResponse AgentServerApp::handle_workflow_resume(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto payload = workflow_execution_result_to_value(
      workflow.engine->resume(context.params.at("workflowRunId"),
                              workflow.definition));
  append_audit(AuditRecord{
      .type = "workflow.resume",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_human_response(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  const auto node_id = optional_string_field(body, "nodeId");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->submit_human_response(
          context.params.at("workflowRunId"),
          node_id,
          body.at("payload")));
  append_audit(AuditRecord{
      .type = "workflow.human-response",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
      .detail = Value::object({{"nodeId", node_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_webhook(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  const auto node_id = optional_string_field(body, "nodeId");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->submit_webhook_payload(
          context.params.at("workflowRunId"),
          node_id,
          body.at("payload")));
  append_audit(AuditRecord{
      .type = "workflow.webhook",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
      .detail = Value::object({{"nodeId", node_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}
