HttpResponse AgentServerApp::handle_chat_like(const AgentServerRequestContext& context, bool stream) {
  if (!has_runner_source()) {
    throw HttpRequestError(404, "Agent runner is not configured.",
                           Value::object({{"error", "Agent runner is not configured."}}));
  }
  auto body = read_json_body(context.request);
  if (!body.contains("input")) {
    throw HttpRequestError(400, "Request body must include input.",
                           Value::object({{"error", "Request body must include input."}}));
  }
  if (body.contains("context") && !body.at("context").is_object()) {
    throw HttpRequestError(400, "Request body field \"context\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"context\" must be a JSON object."},
                                          {"field", "context"}}));
  }
  if (body.contains("modelSettings") && !body.at("modelSettings").is_object()) {
    throw HttpRequestError(400, "Request body field \"modelSettings\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"modelSettings\" must be a JSON object."},
                                          {"field", "modelSettings"}}));
  }
  validate_server_model_settings(body.at("modelSettings"));
  auto request_governance = request_governance_from_body(body);
  const auto governance_policy = resolve_governance_policy(options_.governance, request_governance);

  const auto started = now_ms();
  const auto session_id = resolve_session_id(body);
  const auto agent_id = resolve_authorized_agent_id(context.access, optional_string_field(body, "agent"));
  append_audit(AuditRecord{
      .type = "http.request.started",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .session_id = session_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"stream", stream}}),
  });
  Value server_details = Value::object({{"sessionId", session_id}});
  if (!agent_id.empty()) {
    server_details["agentId"] = agent_id;
  }
  auto run_context = merge_server_execution_context(
      body.at("context").is_object() ? body.at("context") : Value::object({}),
      context,
      stream ? "stream" : "chat",
      std::move(server_details));
  Value runner_body = body;
  runner_body["sessionId"] = session_id;
  runner_body["context"] = run_context;
  if (!agent_id.empty()) {
    runner_body["agent"] = agent_id;
  }
  auto request_runner = resolve_request_runner(runner_body, agent_id);
  auto* runner = request_runner.get();
  if (!runner) {
    throw HttpRequestError(404, "Agent runner is not configured.",
                           Value::object({{"error", "Agent runner is not configured."}}));
  }
  auto result = runner->execution().run(input_to_text(body.at("input")),
                            session_id,
                            model_settings_from_server_value(body.at("modelSettings")),
                            RunnerRetrievalOptions{},
                            RunnerWritebackOptions{},
                            std::vector<SkillActivation>{},
                            std::move(run_context));
  const auto duration = now_ms() - started;
  auto result_value = runner_result_summary(result);
  Value::Array tool_calls;
  for (const auto& item : result_value.at("toolCalls").as_array()) {
    tool_calls.push_back(item.at("name"));
  }
  auto governed = apply_governance_to_run(result.text, std::move(result_value), governance_policy);
  auto replay_events = replay_events_from_result(result, governed.result);
  const auto replay_path = maybe_write_replay(context, body.at("input"), governed.result, replay_events, agent_id);
  Value payload = Value::object({{"sessionId", result.session_id},
                                 {"durationMs", duration},
                                 {"output", governed.output},
                                 {"toolCalls", Value(std::move(tool_calls))},
                                 {"statusStages", Value::Array{}},
                                 {"events", Value::Array{}},
                                 {"response", governed.result.at("response")},
                                 {"reactTrace", governed.result.at("reactTrace")},
                                 {"knowledgeHits", governed.result.at("knowledgeHits")},
                                 {"result", governed.result},
                                 {"replayPath", replay_path.empty() ? Value() : Value(replay_path)},
                                 {"governance", agent_server_governance_summary_to_value(governed.summary)}});
  add_trace_payload(payload, context);
  append_audit(AuditRecord{
      .type = "http.request.completed",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .session_id = result.session_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .status_code = 200,
      .duration_ms = duration,
      .detail = Value::object({{"toolCalls", payload.at("toolCalls")},
                               {"replayPath", replay_path.empty() ? Value() : Value(replay_path)},
                               {"governance", payload.at("governance")}}),
  });
  Value metrics_metadata = Value::object({{"agentId", agent_id},
                                          {"modelResponse", model_response_summary(result.response)}});
  if (!stream) {
    auto response = send_json(200, payload);
    response.metadata = metrics_metadata;
    return response;
  }
  auto response = send_sse({{"replay", Value::object({{"replayPath", replay_path.empty() ? Value() : Value(replay_path)}})},
                            {"done", payload}});
  response.metadata = std::move(metrics_metadata);
  return response;
}
