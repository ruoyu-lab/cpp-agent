void AgentServerApp::register_core_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/health",
      .handler = [this](const AgentServerRequestContext&) {
        return send_json(200, Value::object({{"ok", true}, {"service", options_.server_name}}));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/ready",
      .handler = [this](const AgentServerRequestContext&) {
        Value::Array routes;
        for (const auto& spec : route_specs()) {
          routes.push_back(spec);
        }
        return send_json(200, Value::object({{"ok", true},
                                            {"service", options_.server_name},
                                            {"agent", resolve_default_agent_id().empty()
                                                          ? Value()
                                                          : Value(resolve_default_agent_id())},
                                            {"routes", Value(std::move(routes))},
                                            {"runner", has_runner_source()}}));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/whoami",
      .handler = [this](const AgentServerRequestContext& context) {
        const auto governance = resolve_governance_policy(options_.governance, std::nullopt);
        const auto agent_id = resolve_default_agent_id();
        return send_json(200, Value::object({
                                  {"authenticated", !context.access.bearer_token.empty()},
                                  {"agent", agent_id.empty() ? Value() : Value(agent_id)},
                                  {"apiKeyId", context.access.api_key ? Value(context.access.api_key->id) : Value()},
                                  {"apiKey", context.access.api_key
                                                 ? Value::object({
                                                       {"id", context.access.api_key->id},
                                                       {"agents", strings_to_value(context.access.api_key->agents)},
                                                       {"metadata", context.access.api_key->metadata},
                                                       {"quota", context.access.quota_limit
                                                                     ? Value::object({
                                                                           {"limit", *context.access.quota_limit},
                                                                           {"remaining", *context.access.quota_remaining},
                                                                           {"resetAtMs", *context.access.quota_reset_at_ms},
                                                                       })
                                                                     : Value()},
                                                   })
                                                 : Value()},
                                  {"quota", context.access.quota_limit
                                                ? Value::object({{"limit", *context.access.quota_limit},
                                                                 {"remaining", *context.access.quota_remaining},
                                                                 {"resetAtMs", *context.access.quota_reset_at_ms}})
                                                : Value()},
                                  {"sessionPolicy", Value::object({{"mode", options_.session.mode},
                                                                   {"idPrefix", options_.session.id_prefix.empty()
                                                                                    ? Value()
                                                                                    : Value(options_.session.id_prefix)},
                                                                   {"allowDelete", options_.session.allow_delete}})},
                                  {"governance", Value::object({
                                                     {"pii", governance.pii.has_value()},
                                                     {"citations", normalized_citation_policy_to_value(governance.citations)},
                                                     {"outputSchema", governance.output_schema
                                                                          ? json_schema_to_server_value(*governance.output_schema)
                                                                          : Value()},
                                                 })},
                              }));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/metrics",
      .handler = [this](const AgentServerRequestContext&) {
        return send_json(200, metrics_.snapshot(options_.server_name));
      },
  });
}

void AgentServerApp::register_session_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/sessions",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/sessions/:sessionId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/sessions/:sessionId/compact",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_compact(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "DELETE",
      .pattern = "/sessions/:sessionId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_delete(context);
      },
  });
}

void AgentServerApp::register_approval_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/approvals",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_approval_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/approvals/:approvalId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_approval_resolve(context);
      },
  });
}

void AgentServerApp::register_task_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_create(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "DELETE",
      .pattern = "/tasks/:taskId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_cancel(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/events",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_events(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/checkpoints",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_checkpoints(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/state",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_state(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks/:taskId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks/:taskId/cancel",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_cancel(context);
      },
  });
}

void AgentServerApp::register_async_run_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/async-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/async-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_create(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/async-runs/run-once",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_work_once(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/async-runs/:runId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/async-runs/:runId/events",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_events(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/async-runs/:runId/checkpoints",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_checkpoints(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/async-runs/:runId/transcript",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_transcript(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/async-runs/:runId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/async-runs/:runId/cancel",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_async_run_cancel(context);
      },
  });
}

void AgentServerApp::register_autonomous_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/autonomous-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_create(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/autonomous-runs/:runId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/cancel",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_cancel(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/steps/:stepId/complete",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_complete_step(context);
      },
  });
}

void AgentServerApp::register_workflow_routes() {
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/run",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_run(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/workflows/:workflowRunId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/workflows/:workflowRunId/checkpoints",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_checkpoints(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/human-response",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_human_response(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/webhook",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_webhook(context);
      },
  });
}

void AgentServerApp::register_chat_routes() {
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/chat",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_chat_like(context, false);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/stream",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_chat_like(context, true);
      },
  });
}
