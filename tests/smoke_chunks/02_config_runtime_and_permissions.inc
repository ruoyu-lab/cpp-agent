      true,
      {{"fixture:*", agent::UsagePricing{.input_per_1k_tokens = 0.01,
                                         .output_per_1k_tokens = 0.03}}});
  direct_metrics.record("POST /chat",
                        200,
                        7,
                        "succeeded",
                        "api-key-one",
                        "agent-one",
                        agent::Value::object({
                            {"provider", "fixture"},
                            {"model", "other-model"},
                            {"raw", agent::Value::object({
                                        {"usage", agent::Value::object({
                                                      {"prompt_tokens", 10},
                                                      {"completion_tokens", 5},
                                                      {"total_tokens", 15},
                                                  })},
                                    })},
                        }));
  auto direct_metrics_snapshot = direct_metrics.snapshot("metrics-smoke");
  assert(direct_metrics_snapshot.at("apiKeys").at("api-key-one").at("totalTokens").as_integer() == 15);
  assert(direct_metrics_snapshot.at("agents").at("agent-one").at("estimatedCostByCurrency").at("USD").as_number() > 0);
  auto sessions_response = server_app.handle(agent::HttpRequest{
      .url = "/sessions",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"}},
  });
  assert(sessions_response.status == 200);
  auto sessions_payload = agent::parse_json(sessions_response.body);
  const auto& server_session_items = sessions_payload.at("items").as_array();
  assert(server_session_items.size() >= 2);
  bool saw_server_session_one = false;
  bool saw_server_session_reasoning = false;
  for (const auto& item : server_session_items) {
    saw_server_session_one = saw_server_session_one || item.as_string() == "srv:one";
    saw_server_session_reasoning = saw_server_session_reasoning || item.as_string() == "srv:reasoning";
  }
  assert(saw_server_session_one);
  assert(saw_server_session_reasoning);
  auto session_snapshot_response = server_app.handle(agent::HttpRequest{
      .url = "/sessions/srv:one",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"}},
  });
  assert(session_snapshot_response.status == 200);
  auto server_session_snapshot = agent::parse_json(session_snapshot_response.body);
  assert(server_session_snapshot.at("sessionId").as_string() == "srv:one");
  assert(server_session_snapshot.at("messages").as_array().size() == 2);
  auto delete_session_response = server_app.handle(agent::HttpRequest{
      .url = "/sessions/srv:one",
      .method = "DELETE",
      .headers = {{"authorization", "Bearer server-token"}},
  });
  assert(delete_session_response.status == 200);
  assert(agent::parse_json(delete_session_response.body).at("ok").as_bool());
  auto sessions_after_delete = agent::parse_json(server_app.handle(agent::HttpRequest{
      .url = "/sessions",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"}},
  }).body);
  bool deleted_session_still_listed = false;
  for (const auto& item : sessions_after_delete.at("items").as_array()) {
    deleted_session_still_listed = deleted_session_still_listed || item.as_string() == "srv:one";
  }
  assert(!deleted_session_still_listed);
  std::ifstream server_audit_stream(server_audit_file);
  assert(server_audit_stream.good());
  std::vector<agent::Value> server_audit_records;
  std::string audit_line;
  while (std::getline(server_audit_stream, audit_line)) {
    if (!audit_line.empty()) {
      server_audit_records.push_back(agent::parse_json(audit_line));
    }
  }
  const auto audit_has = [&](const std::string& type) {
    return std::any_of(server_audit_records.begin(), server_audit_records.end(),
                       [&](const agent::Value& record) {
                         return record.at("type").as_string() == type;
                       });
  };
  assert(audit_has("http.request.rejected"));
  assert(audit_has("http.request.started"));
  assert(audit_has("http.request.completed"));
  assert(audit_has("http.request.failed"));
  assert(audit_has("session.cleared"));
  const auto completed_audit = std::find_if(server_audit_records.begin(), server_audit_records.end(),
                                           [](const agent::Value& record) {
                                             return record.at("type").as_string() == "http.request.completed";
                                           });
  assert(completed_audit != server_audit_records.end());
  assert(completed_audit->at("requestId").as_string() == "req-chat-1");
  assert(completed_audit->at("traceId").as_string() == "trace-chat-1");
  assert(std::filesystem::exists(completed_audit->at("detail").at("replayPath").as_string()));
  {
    agent::JsonSchema governed_schema;
    governed_schema.type = agent::JsonSchemaType::Object;
    governed_schema.additional_properties = false;
    governed_schema.required = {"message"};
    governed_schema.properties["message"].type = agent::JsonSchemaType::String;
    auto governance_model = std::make_shared<GovernanceModel>();
    agent::AgentRunner governance_runner(agent::AgentRunnerConfig{
        .model_runtime = {
            .adapter = governance_model,
        },
        .context_runtime = {
            .max_iterations = 1,
        },
    });
    agent::AgentServerApp governance_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .runner = &governance_runner,
        .governance = agent::AgentServerGovernanceConfig{
            .pii = agent::AgentServerPiiGovernanceConfig{},
            .output_schema = governed_schema,
        },
    });
    auto governance_whoami_response = governance_server.handle(agent::HttpRequest{
        .url = "/whoami",
        .method = "GET",
    });
    assert(governance_whoami_response.status == 200);
    auto governance_whoami = agent::parse_json(governance_whoami_response.body);
    assert(governance_whoami.at("governance").at("pii").as_bool());
    assert(governance_whoami.at("governance").at("outputSchema").at("type").as_string() == "object");
    auto governed_chat_response = governance_server.handle(agent::HttpRequest{
        .url = "/chat",
        .method = "POST",
        .body = agent::Value::object({
            {"input", "schema-governance"},
            {"sessionId", "gov-one"},
        }).stringify(0),
    });
    assert(governed_chat_response.status == 200);
    auto governed_chat = agent::parse_json(governed_chat_response.body);
    assert(agent::parse_json(governed_chat.at("output").as_string()).at("message").as_string() ==
           "contact [REDACTED]");
    assert(governed_chat.at("governance").at("redacted").as_bool());
    assert(governed_chat.at("governance").at("schemaValidated").as_bool());
    assert(governed_chat.at("result").at("response").at("raw").is_null());

    agent::AgentServerApp citation_governance_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .runner = &governance_runner,
        .governance = agent::AgentServerGovernanceConfig{
            .citations = agent::AgentServerCitationGovernanceConfig{.mode = "require"},
        },
    });
    auto citation_failure_response = citation_governance_server.handle(agent::HttpRequest{
        .url = "/chat",
        .method = "POST",
        .body = agent::Value::object({
            {"input", "citation-required"},
            {"sessionId", "gov-two"},
        }).stringify(0),
    });
    assert(citation_failure_response.status == 422);
    auto citation_failure = agent::parse_json(citation_failure_response.body);
    assert(citation_failure.at("violations").as_array().front().as_string().find("Citation policy") !=
           std::string::npos);
  }
  agent::InMemorySessionStore locked_sessions;
  locked_sessions.get("locked")->add(agent::create_message(agent::MessageRole::User, "keep"));
  agent::AgentServerApp locked_session_server(agent::AgentServerOptions{.allow_unauthenticated = true,
      .session = agent::AgentServerSessionPolicy{.allow_delete = false},
      .session_store = &locked_sessions,
  });
  auto locked_delete_response = locked_session_server.handle(agent::HttpRequest{
      .url = "/sessions/locked",
      .method = "DELETE",
  });
  assert(locked_delete_response.status == 403);
  assert(!locked_sessions.list_session_ids().empty());

  agent::SessionMemoryOptions compact_route_options;
  compact_route_options.compaction.compaction_budget.max_messages = 1;
  agent::InMemorySessionStore compact_route_sessions(agent::InMemorySessionStoreConfig{
      .session_options = compact_route_options,
  });
  auto compact_route_session = compact_route_sessions.get("manual-compact");
  compact_route_session->add(agent::create_message(agent::MessageRole::User, "first"));
  compact_route_session->add(agent::create_message(agent::MessageRole::Assistant, "second"));
  compact_route_session->add(agent::create_message(agent::MessageRole::User, "third"));
  agent::AgentServerApp compact_route_server(agent::AgentServerOptions{.allow_unauthenticated = true,
      .session_store = &compact_route_sessions,
  });
  auto compact_route_response = compact_route_server.handle(agent::HttpRequest{
      .url = "/sessions/manual-compact/compact",
      .method = "POST",
  });
  assert(compact_route_response.status == 200);
  auto compact_route_payload = agent::parse_json(compact_route_response.body);
  assert(compact_route_payload.at("ok").as_bool());
  assert(compact_route_payload.at("sessionId").as_string() == "manual-compact");
  assert(compact_route_payload.at("snapshot").at("messages").as_array().size() == 1);
  assert(compact_route_payload.at("snapshot").at("summary").as_string().find("first") !=
         std::string::npos);

  agent::SessionMemoryOptions configured_session_options;
  configured_session_options.storage.summary_label = "Configured summary";
  configured_session_options.storage.summary = "seed summary";
  configured_session_options.compaction.compaction_budget.max_messages = 8;
  agent::InMemorySessionStore configured_memory_sessions(agent::InMemorySessionStoreConfig{
      .session_options = configured_session_options,
  });
  auto configured_memory_session = configured_memory_sessions.get("configured-memory");
  auto configured_memory_messages = configured_memory_session->get_messages();
  assert(agent::extract_text_content(configured_memory_messages.front().content).find("Configured summary") !=
         std::string::npos);
  const auto configured_session_dir =
      smoke_artifacts_root() / "configured-session-store";
  std::filesystem::remove_all(configured_session_dir);
  agent::FileSessionStore configured_file_sessions(agent::FileSessionStoreConfig{
      .base_dir = configured_session_dir,
      .file_extension = "session",
      .session_options = configured_session_options,
  });
  auto configured_file_session = configured_file_sessions.get("cfg:file/path");
  configured_file_session->add(agent::create_message(agent::MessageRole::User, "from config store"));
  configured_file_sessions.flush("cfg:file/path");
  assert(contains_string(configured_file_sessions.list_session_ids(), "cfg:file/path"));
  agent::FileSessionStore reopened_file_sessions(agent::FileSessionStoreConfig{
      .base_dir = configured_session_dir,
      .file_extension = "session",
      .session_options = configured_session_options,
  });
  auto reopened_file_session = reopened_file_sessions.get("cfg:file/path");
  auto reopened_file_messages = reopened_file_session->get_messages(false);
  assert(!reopened_file_messages.empty());
  assert(agent::extract_text_content(reopened_file_messages.back().content) == "from config store");
  {
    auto approval_request = agent::PermissionRequest{
        .tool_name = "shell.exec",
        .capabilities = {"process.exec"},
        .risk_level = agent::ToolRiskLevel::High,
        .tags = {"developer"},
        .bundle = "developer",
        .builtin = true,
        .tool_call = agent::ToolCall{
            .id = "approval-call-1",
            .name = "shell.exec",
            .arguments = agent::Value::object({{"command", "echo ok"}}),
        },
    };
    agent::InMemoryApprovalStore approval_store;
    agent::ManualApprovalQueue approval_queue(agent::ManualApprovalQueueConfig{.store = &approval_store});
    agent::AgentServerApp approval_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .approvals = agent::AgentServerApprovalsConfig{.queue = &approval_queue},
    });
    agent::PermissionDecision resolved_decision{agent::PermissionDecisionKind::Deny, "not resolved"};
    auto approval_handler = approval_queue.handler();
    std::thread approval_thread([&]() {
      resolved_decision = approval_handler(
          approval_request,
          agent::PermissionDecision{agent::PermissionDecisionKind::Ask, "needs approval"});
    });

    std::string approval_id;
    for (int attempt = 0; attempt < 100 && approval_id.empty(); ++attempt) {
      auto list_response = approval_server.handle(agent::HttpRequest{.url = "/approvals?status=pending",
                                                                     .method = "GET"});
      assert(list_response.status == 200);
      auto list_payload = agent::parse_json(list_response.body);
      if (!list_payload.at("items").as_array().empty()) {
        const auto& item = list_payload.at("items").as_array().front();
        approval_id = item.at("id").as_string();
        assert(item.at("status").as_string() == "pending");
        assert(item.at("request").at("toolName").as_string() == "shell.exec");
        assert(item.at("proposedDecision").at("decision").as_string() == "ask");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!approval_id.empty());
    auto resolve_approval_response = approval_server.handle(agent::HttpRequest{
        .url = "/approvals/" + approval_id,
        .method = "POST",
        .body = agent::Value::object({{"decision", "allow"}, {"reason", "approved"}}).stringify(0),
    });
    assert(resolve_approval_response.status == 200);
    auto resolved_approval = agent::parse_json(resolve_approval_response.body);
    assert(resolved_approval.at("status").as_string() == "resolved");
    assert(resolved_approval.at("finalDecision").at("decision").as_string() == "allow");
    approval_thread.join();
    assert(resolved_decision.decision == agent::PermissionDecisionKind::Allow);
    auto resolved_list_response = approval_server.handle(agent::HttpRequest{.url = "/approvals?status=resolved",
                                                                           .method = "GET"});
    assert(resolved_list_response.status == 200);
    assert(agent::parse_json(resolved_list_response.body).at("items").as_array().size() == 1);
    auto missing_approval_response = approval_server.handle(agent::HttpRequest{
        .url = "/approvals/missing",
        .method = "POST",
        .body = agent::Value::object({{"decision", "deny"}}).stringify(0),
    });
    assert(missing_approval_response.status == 404);

    const auto approval_file = smoke_artifacts_root() / "approvals.json";
    std::filesystem::remove(approval_file);
    agent::FileApprovalStore file_approval_store(approval_file);
    file_approval_store.create(agent::CreateApprovalRecordInput{
        .id = "file-approval-1",
        .request = approval_request,
        .proposed_decision = agent::PermissionDecision{agent::PermissionDecisionKind::Ask, "file approval"},
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    auto file_resolved = file_approval_store.resolve(
        "file-approval-1",
        agent::PermissionDecision{agent::PermissionDecisionKind::Deny, "persisted deny"});
    assert(file_resolved);
    agent::FileApprovalStore reloaded_approval_store(approval_file);
    auto loaded_approvals = reloaded_approval_store.list(
        agent::ApprovalRecordFilter{.status = agent::ApprovalRecordStatus::Resolved});
    assert(loaded_approvals.size() == 1);
    assert(loaded_approvals.front().final_decision);
    assert(loaded_approvals.front().final_decision->decision == agent::PermissionDecisionKind::Deny);

    auto pg_approval_client = std::make_shared<FakePgApprovalClient>();
    agent::PgApprovalStore pg_approval_store(agent::PgApprovalStoreConfig{
        .client = pg_approval_client,
        .create_table = true,
        .close_client_on_close = true,
    });
    auto pg_approval = pg_approval_store.create(agent::CreateApprovalRecordInput{
        .id = "approval-pg-1",
        .request = approval_request,
        .proposed_decision = agent::PermissionDecision{agent::PermissionDecisionKind::Ask, "pg approval"},
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    assert(pg_approval.id == "approval-pg-1");
    assert(pg_approval.status == agent::ApprovalRecordStatus::Pending);
    assert(pg_approval.request.tool_name == "shell.exec");
    assert(pg_approval.metadata.at("tenant").as_string() == "native");
    assert(pg_approval_client->queries.size() >= 4);
    assert(pg_approval_client->queries.at(0).sql.find("CREATE SCHEMA IF NOT EXISTS") != std::string::npos);
    assert(pg_approval_client->queries.at(1).sql.find("CREATE TABLE IF NOT EXISTS") != std::string::npos);
    assert(pg_approval_client->queries.at(2).sql.find("CREATE INDEX IF NOT EXISTS") != std::string::npos);
    assert(pg_approval_client->queries.at(3).sql.find("INSERT INTO") != std::string::npos);
    assert(agent::parse_json(pg_approval_client->queries.at(3).params.at(2).as_string())
               .at("toolName")
               .as_string() == "shell.exec");
    auto pg_pending = pg_approval_store.list(
        agent::ApprovalRecordFilter{.status = agent::ApprovalRecordStatus::Pending});
    assert(pg_pending.size() == 1);
    auto pg_resolved = pg_approval_store.resolve(
        "approval-pg-1",
        agent::PermissionDecision{agent::PermissionDecisionKind::Allow, "pg approved"});
    assert(pg_resolved);
    assert(pg_resolved->status == agent::ApprovalRecordStatus::Resolved);
    assert(pg_resolved->final_decision);
    assert(pg_resolved->final_decision->decision == agent::PermissionDecisionKind::Allow);
    auto pg_loaded = pg_approval_store.get("approval-pg-1");
    assert(pg_loaded);
    assert(pg_loaded->resolved_at.empty() == false);
    assert(pg_loaded->final_decision->reason == "pg approved");
    auto pg_resolved_list = pg_approval_store.list(
        agent::ApprovalRecordFilter{.status = agent::ApprovalRecordStatus::Resolved});
    assert(pg_resolved_list.size() == 1);
    pg_approval_store.close();
    assert(pg_approval_client->closed);
  }
  {
    auto pg_approval_ready_client = std::make_shared<FakePgApprovalClient>();
    agent::PgApprovalStore pg_approval_ready_store(agent::PgApprovalStoreConfig{
        .client = pg_approval_ready_client,
        .create_table = true,
        .close_client_on_close = true,
    });

    constexpr int approval_ready_thread_count = 8;
    std::atomic<int> approval_ready_threads_waiting{0};
    std::atomic<bool> approval_ready_start{false};
    std::vector<std::thread> approval_ready_threads;
    for (int i = 0; i < approval_ready_thread_count; ++i) {
      approval_ready_threads.emplace_back([&]() {
        approval_ready_threads_waiting.fetch_add(1, std::memory_order_acq_rel);
        while (!approval_ready_start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        auto approvals = pg_approval_ready_store.list(agent::ApprovalRecordFilter{});
        assert(approvals.empty());
      });
    }
    while (approval_ready_threads_waiting.load(std::memory_order_acquire) != approval_ready_thread_count) {
      std::this_thread::yield();
    }
    approval_ready_start.store(true, std::memory_order_release);
    for (auto& thread : approval_ready_threads) {
      thread.join();
    }

    const auto approval_schema_creates = std::count_if(
        pg_approval_ready_client->queries.begin(),
        pg_approval_ready_client->queries.end(),
        [](const FakePgApprovalClient::QueryRecord& record) {
          return record.sql.find("CREATE SCHEMA IF NOT EXISTS") != std::string::npos;
        });
    const auto approval_table_creates = std::count_if(
        pg_approval_ready_client->queries.begin(),
        pg_approval_ready_client->queries.end(),
        [](const FakePgApprovalClient::QueryRecord& record) {
          return record.sql.find("CREATE TABLE IF NOT EXISTS") != std::string::npos;
        });
    const auto approval_index_creates = std::count_if(
        pg_approval_ready_client->queries.begin(),
        pg_approval_ready_client->queries.end(),
        [](const FakePgApprovalClient::QueryRecord& record) {
          return record.sql.find("CREATE INDEX IF NOT EXISTS") != std::string::npos;
        });
    assert(approval_schema_creates == 1);
    assert(approval_table_creates == 1);
    assert(approval_index_creates == 1);

    constexpr int approval_close_thread_count = 4;
    std::atomic<int> approval_close_threads_waiting{0};
    std::atomic<bool> approval_close_start{false};
    std::vector<std::thread> approval_close_threads;
    for (int i = 0; i < approval_close_thread_count; ++i) {
      approval_close_threads.emplace_back([&]() {
        approval_close_threads_waiting.fetch_add(1, std::memory_order_acq_rel);
        while (!approval_close_start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        pg_approval_ready_store.close();
      });
    }
    while (approval_close_threads_waiting.load(std::memory_order_acquire) != approval_close_thread_count) {
      std::this_thread::yield();
    }
    approval_close_start.store(true, std::memory_order_release);
    for (auto& thread : approval_close_threads) {
      thread.join();
    }
    pg_approval_ready_store.close();
    assert(pg_approval_ready_client->closed);
    assert(pg_approval_ready_client->close_count == 1);
  }
  agent::AgentServerApp quota_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .runner = &server_runner,
      .api_keys = {
          agent::AgentServerApiKey{
              .id = "key-one",
              .token = "quota-token",
              .quota = agent::AgentServerApiKeyQuotaConfig{.max_requests = 1, .window_ms = 60000},
          },
      },
  });
  auto quota_first = quota_app.handle(agent::HttpRequest{
      .url = "/whoami",
      .method = "GET",
      .headers = {{"authorization", "Bearer quota-token"}},
  });
  assert(quota_first.status == 200);
  assert(quota_first.headers.at("x-api-key-id") == "key-one");
  assert(agent::parse_json(quota_first.body).at("apiKeyId").as_string() == "key-one");
  auto quota_second = quota_app.handle(agent::HttpRequest{
      .url = "/whoami",
      .method = "GET",
      .headers = {{"authorization", "Bearer quota-token"}},
  });
  assert(quota_second.status == 429);
  assert(quota_second.headers.at("x-quota-limit") == "1");
  assert(server_model->prompts().size() >= 3);
  {
    agent::AgentServerApp agent_access_app(agent::AgentServerOptions{.allow_unauthenticated = true,
        .runner = &server_runner,
        .agent = "allowed-agent",
        .api_keys = {
            agent::AgentServerApiKey{
                .id = "agent-key",
                .token = "agent-token",
                .agents = {"allowed-agent"},
            },
        },
    });
    auto forbidden_agent_response = agent_access_app.handle(agent::HttpRequest{
        .url = "/chat",
        .method = "POST",
        .headers = {{"authorization", "Bearer agent-token"}},
        .body = agent::Value::object({
            {"input", "forbidden-agent"},
            {"agent", "denied-agent"},
        }).stringify(0),
    });
    assert(forbidden_agent_response.status == 403);
    assert(agent::parse_json(forbidden_agent_response.body).at("agentId").as_string() == "denied-agent");
    auto default_agent_response = agent_access_app.handle(agent::HttpRequest{
        .url = "/chat",
        .method = "POST",
        .headers = {{"authorization", "Bearer agent-token"}},
        .body = agent::Value::object({
            {"input", "server-input-allowed-default-agent"},
            {"context", agent::Value::object({{"source", "server-source"}})},
        }).stringify(0),
    });
    assert(default_agent_response.status == 200);
    auto agent_whoami_response = agent_access_app.handle(agent::HttpRequest{
        .url = "/whoami",
        .method = "GET",
        .headers = {{"authorization", "Bearer agent-token"}},
    });
    assert(agent_whoami_response.status == 200);
    auto agent_whoami = agent::parse_json(agent_whoami_response.body);
    assert(agent_whoami.at("agent").as_string() == "allowed-agent");
    assert(agent_whoami.at("apiKey").at("agents").as_array().front().as_string() == "allowed-agent");
  }
  {
    agent::InMemoryTaskStore route_task_store;
    agent::InMemoryTaskQueue route_task_queue(route_task_store);
    const auto task_audit_file = smoke_artifacts_root() / "task-audit.jsonl";
    std::filesystem::remove(task_audit_file);
    agent::AgentServerApp task_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .audit_log_path = task_audit_file,
        .tasks = agent::AgentServerTasksConfig{
            .store = &route_task_store,
            .queue = &route_task_queue,
            .default_type = "agent.run",
        },
    });
    auto create_task_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"x-request-id", "req-task-1"}, {"x-trace-id", "trace-task-1"}},
        .body = agent::Value::object({
            {"id", "route-task-1"},
            {"input", "task-input"},
            {"sessionId", "task-session"},
            {"idempotencyKey", "idem-1"},
            {"metadata", agent::Value::object({{"tenant", "native"}})},
        }).stringify(0),
    });
    assert(create_task_response.status == 202);
    auto created_task = agent::parse_json(create_task_response.body);
    assert(created_task.at("task").at("id").as_string() == "route-task-1");
    assert(created_task.at("requestId").as_string() == "req-task-1");
    assert(created_task.at("traceId").as_string() == "trace-task-1");
    assert(created_task.at("task").at("input").at("context").at("server").at("path").as_string() == "/tasks");
    assert(created_task.at("task").at("input").at("context").at("server").at("requestId").as_string() == "req-task-1");
    assert(created_task.at("task").at("input").at("context").at("traceId").as_string() == "trace-task-1");
    assert(created_task.at("idempotent").as_bool() == false);
    auto idempotent_task_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({
            {"input", "task-input"},
            {"idempotencyKey", "idem-1"},
        }).stringify(0),
    });
    assert(idempotent_task_response.status == 200);
    assert(agent::parse_json(idempotent_task_response.body).at("idempotent").as_bool());
    auto header_idempotent_task_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"Idempotency-Key", "idem-header-1"}},
        .body = agent::Value::object({
            {"input", "task-input-header-key"},
        }).stringify(0),
    });
    assert(header_idempotent_task_response.status == 202);
    auto header_idempotent_task = agent::parse_json(header_idempotent_task_response.body);
    assert(header_idempotent_task.at("task").at("idempotencyKey").as_string() == "idem-header-1");
    assert(header_idempotent_task.at("task").at("metadata").at("idempotencyKey").as_string() == "idem-header-1");
    auto header_duplicate_task_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"idempotency-key", "idem-header-1"}},
        .body = agent::Value::object({
            {"input", "different-task-input-header-key"},
        }).stringify(0),
    });
    assert(header_duplicate_task_response.status == 200);
    assert(agent::parse_json(header_duplicate_task_response.body).at("idempotent").as_bool());
    auto mismatched_idempotency_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"Idempotency-Key", "idem-header-mismatch"}},
        .body = agent::Value::object({
            {"input", "task-input-mismatch"},
            {"idempotencyKey", "idem-body-mismatch"},
        }).stringify(0),
    });
    assert(mismatched_idempotency_response.status == 400);
    assert(agent::parse_json(mismatched_idempotency_response.body)
               .at("error")
               .as_string()
               .find("must match") != std::string::npos);
    auto task_list_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks?status=queued&type=agent.run",
        .method = "GET",
    });
    assert(task_list_response.status == 200);
    assert(agent::parse_json(task_list_response.body).at("items").as_array().size() == 2);
    agent::InMemoryTaskStore required_idempotency_store;
    agent::InMemoryTaskQueue required_idempotency_queue(required_idempotency_store);
    agent::AgentServerApp required_idempotency_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .tasks = agent::AgentServerTasksConfig{
            .store = &required_idempotency_store,
            .queue = &required_idempotency_queue,
            .idempotency = agent::AgentServerTaskIdempotencyConfig{
                .require_key = true,
                .header_name = "X-Task-Idempotency",
            },
        },
    });
    auto missing_required_idempotency_response = required_idempotency_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({{"input", "missing-required-key"}}).stringify(0),
    });
    assert(missing_required_idempotency_response.status == 400);
    assert(agent::parse_json(missing_required_idempotency_response.body)
               .at("headerName")
               .as_string() == "X-Task-Idempotency");
    auto custom_header_idempotency_response = required_idempotency_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"X-Task-Idempotency", "required-idem-1"}},
        .body = agent::Value::object({{"input", "required-key"}}).stringify(0),
    });
    assert(custom_header_idempotency_response.status == 202);
    assert(agent::parse_json(custom_header_idempotency_response.body)
               .at("task")
               .at("idempotencyKey")
               .as_string() == "required-idem-1");
    agent::InMemoryTaskStore policy_task_store;
    agent::InMemoryTaskQueue policy_task_queue(policy_task_store);
    agent::AgentServerApp policy_task_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .session = agent::AgentServerSessionPolicy{.mode = "require", .id_prefix = "task:"},
        .tasks = agent::AgentServerTasksConfig{
            .store = &policy_task_store,
            .queue = &policy_task_queue,
        },
    });
    auto missing_task_session_response = policy_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({{"input", "missing-task-session"}}).stringify(0),
    });
    assert(missing_task_session_response.status == 400);
    auto task_prefixed_session_response = policy_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({
            {"input", "task-session-policy"},
            {"sessionId", "one"},
        }).stringify(0),
    });
    assert(task_prefixed_session_response.status == 202);
    assert(agent::parse_json(task_prefixed_session_response.body)
               .at("task")
               .at("input")
               .at("sessionId")
               .as_string() == "task:one");
    auto invalid_task_reasoning_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({
            {"input", "task-invalid-reasoning"},
            {"modelSettings", agent::Value::object({
                                  {"reasoning", agent::Value::object({{"enabled", "yes"}})},
                              })},
        }).stringify(0),
    });
    assert(invalid_task_reasoning_response.status == 400);
    assert(agent::parse_json(invalid_task_reasoning_response.body).at("field").as_string() ==
           "modelSettings.reasoning");
    auto task_get_response = task_server.handle(agent::HttpRequest{.url = "/tasks/route-task-1", .method = "GET"});
    assert(task_get_response.status == 200);
    assert(agent::parse_json(task_get_response.body).at("events").as_array().size() == 1);
    auto task_events_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks/route-task-1/events",
        .method = "GET",
    });
    assert(task_events_response.status == 200);
    assert(agent::parse_json(task_events_response.body).at("items").as_array().size() == 1);
    route_task_store.append_checkpoint("route-task-1", "run-1", "runner.state",
                                       agent::Value::object({{"version", 1}, {"sessionId", "task-session"}}));
    auto task_state_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks/route-task-1/state",
        .method = "GET",
    });
    assert(task_state_response.status == 200);
    assert(agent::parse_json(task_state_response.body).at("resumable").as_bool());
    auto task_cancel_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks/route-task-1/cancel",
        .method = "POST",
    });
    assert(task_cancel_response.status == 200);
    assert(agent::parse_json(task_cancel_response.body).at("task").at("status").as_string() == "cancelled");
    auto create_delete_task_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .body = agent::Value::object({
            {"id", "route-task-delete"},
            {"input", "delete-input"},
        }).stringify(0),
    });
    assert(create_delete_task_response.status == 202);
    auto task_delete_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks/route-task-delete",
        .method = "DELETE",
    });
    assert(task_delete_response.status == 200);
    auto deleted_task = agent::parse_json(task_delete_response.body);
    assert(deleted_task.at("cancelled").as_bool());
    assert(deleted_task.at("task").at("status").as_string() == "cancelled");
    auto missing_task_delete_response = task_server.handle(agent::HttpRequest{
        .url = "/tasks/missing-delete",
        .method = "DELETE",
    });
    assert(missing_task_delete_response.status == 404);
    std::ifstream task_audit_stream(task_audit_file);
    assert(task_audit_stream.good());
    std::vector<std::string> task_audit_types;
    while (std::getline(task_audit_stream, audit_line)) {
      if (!audit_line.empty()) {
        task_audit_types.push_back(agent::parse_json(audit_line).at("type").as_string());
      }
    }
    assert(std::find(task_audit_types.begin(), task_audit_types.end(), "task.created") != task_audit_types.end());
    assert(std::find(task_audit_types.begin(), task_audit_types.end(), "task.cancelled") != task_audit_types.end());
  }
  {
    agent::InMemoryTaskStore scoped_task_store;
    agent::InMemoryTaskQueue scoped_task_queue(scoped_task_store);
    agent::AgentServerApp scoped_task_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .api_keys = {
            agent::AgentServerApiKey{
                .id = "task-key-a",
                .token = "task-token-a",
                .metadata = agent::Value::object({{"tenantId", "tenant-a"}}),
            },
            agent::AgentServerApiKey{
                .id = "task-key-b",
                .token = "task-token-b",
                .metadata = agent::Value::object({{"tenantId", "tenant-b"}}),
            },
        },
        .tasks = agent::AgentServerTasksConfig{
            .store = &scoped_task_store,
            .queue = &scoped_task_queue,
            .default_type = "agent.run",
            .access = agent::AgentServerTaskAccessConfig{.scope = "tenant"},
        },
    });
    auto scoped_task_conflict = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"authorization", "Bearer task-token-a"}},
        .body = agent::Value::object({
            {"id", "scoped-task-conflict"},
            {"input", "tenant-conflict"},
            {"metadata", agent::Value::object({{"tenantId", "tenant-b"}})},
        }).stringify(0),
    });
    assert(scoped_task_conflict.status == 403);
    auto scoped_task_create = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "POST",
        .headers = {{"authorization", "Bearer task-token-a"}},
        .body = agent::Value::object({
            {"id", "scoped-task-a"},
            {"input", "tenant-task"},
            {"metadata", agent::Value::object({{"custom", "kept"}})},
        }).stringify(0),
    });
    assert(scoped_task_create.status == 202);
    auto scoped_task_payload = agent::parse_json(scoped_task_create.body);
    assert(scoped_task_payload.at("task").at("ownerApiKeyId").as_string() == "task-key-a");
    assert(scoped_task_payload.at("task").at("tenantId").as_string() == "tenant-a");
    assert(scoped_task_payload.at("task").at("metadata").at("tenantId").as_string() == "tenant-a");
    auto scoped_task_list_a = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "GET",
        .headers = {{"authorization", "Bearer task-token-a"}},
    });
    assert(scoped_task_list_a.status == 200);
    assert(agent::parse_json(scoped_task_list_a.body).at("items").as_array().size() == 1);
    auto scoped_task_list_b = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks",
        .method = "GET",
        .headers = {{"authorization", "Bearer task-token-b"}},
    });
    assert(scoped_task_list_b.status == 200);
    assert(agent::parse_json(scoped_task_list_b.body).at("items").as_array().empty());
    auto scoped_task_get_b = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks/scoped-task-a",
        .method = "GET",
        .headers = {{"authorization", "Bearer task-token-b"}},
    });
    assert(scoped_task_get_b.status == 404);
    auto scoped_task_cancel_b = scoped_task_server.handle(agent::HttpRequest{
        .url = "/tasks/scoped-task-a/cancel",
        .method = "POST",
        .headers = {{"authorization", "Bearer task-token-b"}},
    });
    assert(scoped_task_cancel_b.status == 404);
  }
  {
    agent::InMemoryAutonomousStore route_autonomous_store;
    agent::StaticAutonomousPlanner route_autonomous_planner(std::vector<agent::AutonomousPlanStep>{
        agent::AutonomousPlanStep{.id = "approval", .title = "Approval", .objective = "Wait"},
    });
    agent::CallbackAutonomousStepExecutor route_autonomous_executor(
        [](const agent::AutonomousStepExecutionInput&) {
          return agent::AutonomousStepExecutionResult{
              .status = agent::AutonomousStepStatus::Waiting,
              .wait_reason = "server approval",
          };
        });
    agent::AutonomousRunManager route_autonomous_manager(agent::AutonomousRunManagerConfig{
        .store = &route_autonomous_store,
        .planner = &route_autonomous_planner,
        .executor = &route_autonomous_executor,
    });
    agent::AgentServerApp autonomous_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .autonomous = agent::AgentServerAutonomousConfig{
            .store = &route_autonomous_store,
            .manager = &route_autonomous_manager,
        },
    });
    auto blank_goal_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({{"goal", "   "}}).stringify(0),
    });
    assert(blank_goal_response.status == 400);
    assert(agent::parse_json(blank_goal_response.body).at("field").as_string() == "goal");
    auto invalid_auto_start_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"goal", "invalid auto start"},
            {"autoStart", "yes"},
        }).stringify(0),
    });
    assert(invalid_auto_start_response.status == 400);
    assert(agent::parse_json(invalid_auto_start_response.body).at("field").as_string() == "autoStart");
    auto null_auto_start_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"goal", "null auto start"},
            {"autoStart", agent::Value()},
        }).stringify(0),
    });
    assert(null_auto_start_response.status == 400);
    assert(agent::parse_json(null_auto_start_response.body).at("field").as_string() == "autoStart");
    auto null_autonomous_id_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"id", agent::Value()},
            {"goal", "null id"},
        }).stringify(0),
    });
    assert(null_autonomous_id_response.status == 400);
    assert(agent::parse_json(null_autonomous_id_response.body).at("field").as_string() == "id");
    auto null_autonomous_metadata_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"goal", "null metadata"},
            {"metadata", agent::Value()},
        }).stringify(0),
    });
    assert(null_autonomous_metadata_response.status == 400);
    assert(agent::parse_json(null_autonomous_metadata_response.body).at("field").as_string() == "metadata");
    auto queued_run_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"id", "route-auto-queued"},
            {"goal", "  Queued autonomous  "},
            {"input", agent::Value::object({{"mode", "queued"}})},
        }).stringify(0),
    });
    assert(queued_run_response.status == 202);
    auto queued_run = agent::parse_json(queued_run_response.body);
    assert(queued_run.at("run").at("goal").as_string() == "Queued autonomous");
    assert(queued_run.at("run").at("status").as_string() == "queued");
    assert(queued_run.at("run").at("metadata").at("source").is_null());
    auto create_run_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .body = agent::Value::object({
            {"id", "route-auto-1"},
            {"goal", "Route autonomous"},
            {"input", agent::Value::object({{"mode", "server"}})},
            {"autoStart", true},
        }).stringify(0),
    });
    assert(create_run_response.status == 200);
    auto created_run = agent::parse_json(create_run_response.body);
    assert(created_run.at("run").at("status").as_string() == "waiting");
    assert(created_run.at("steps").as_array().front().at("waitReason").as_string() == "server approval");
    auto complete_step_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/route-auto-1/steps/approval/complete",
        .method = "POST",
        .body = agent::Value::object({{"output", agent::Value::object({{"approved", true}})}}).stringify(0),
    });
    assert(complete_step_response.status == 200);
    assert(agent::parse_json(complete_step_response.body).at("run").at("status").as_string() == "queued");
    auto resume_run_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/route-auto-1/resume",
        .method = "POST",
    });
    assert(resume_run_response.status == 200);
    assert(agent::parse_json(resume_run_response.body).at("run").at("status").as_string() == "completed");
    auto autonomous_list_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs?status=completed",
        .method = "GET",
    });
    assert(autonomous_list_response.status == 200);
    assert(agent::parse_json(autonomous_list_response.body).at("items").as_array().size() == 1);
    auto autonomous_get_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/route-auto-1",
        .method = "GET",
    });
    assert(autonomous_get_response.status == 200);
    assert(agent::parse_json(autonomous_get_response.body).at("checkpoints").as_array().size() >= 3);
    auto invalid_cancel_reason_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/route-auto-queued/cancel",
        .method = "POST",
        .body = agent::Value::object({{"reason", agent::Value()}}).stringify(0),
    });
    assert(invalid_cancel_reason_response.status == 400);
    assert(agent::parse_json(invalid_cancel_reason_response.body).at("field").as_string() == "reason");
    auto cancel_run_response = autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/route-auto-queued/cancel",
        .method = "POST",
        .body = agent::Value::object({{"reason", "not needed"}}).stringify(0),
    });
    assert(cancel_run_response.status == 200);
    auto cancelled_run = agent::parse_json(cancel_run_response.body);
    assert(cancelled_run.at("run").at("status").as_string() == "cancelled");
    assert(cancelled_run.at("run").at("error").as_string() == "not needed");
  }
  {
    agent::InMemoryAutonomousStore scoped_autonomous_store;
    agent::StaticAutonomousPlanner scoped_autonomous_planner(std::vector<agent::AutonomousPlanStep>{
        agent::AutonomousPlanStep{.id = "noop", .title = "Noop", .objective = "Noop"},
    });
    agent::CallbackAutonomousStepExecutor scoped_autonomous_executor(
        [](const agent::AutonomousStepExecutionInput&) {
          return agent::AutonomousStepExecutionResult{};
        });
    agent::AutonomousRunManager scoped_autonomous_manager(agent::AutonomousRunManagerConfig{
        .store = &scoped_autonomous_store,
        .planner = &scoped_autonomous_planner,
        .executor = &scoped_autonomous_executor,
    });
    agent::AgentServerApp scoped_autonomous_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .api_keys = {
            agent::AgentServerApiKey{
                .id = "auto-key-a",
                .token = "auto-token-a",
                .metadata = agent::Value::object({{"tenantId", "tenant-a"}}),
            },
            agent::AgentServerApiKey{
                .id = "auto-key-b",
                .token = "auto-token-b",
                .metadata = agent::Value::object({{"tenantId", "tenant-b"}}),
            },
        },
        .autonomous = agent::AgentServerAutonomousConfig{
            .store = &scoped_autonomous_store,
            .manager = &scoped_autonomous_manager,
            .access = agent::AgentServerAutonomousAccessConfig{.scope = "tenant"},
        },
    });
    auto scoped_auto_conflict = scoped_autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .headers = {{"authorization", "Bearer auto-token-a"}},
        .body = agent::Value::object({
            {"id", "scoped-auto-conflict"},
            {"goal", "conflict"},
            {"metadata", agent::Value::object({{"ownerApiKeyId", "auto-key-b"}})},
        }).stringify(0),
    });
    assert(scoped_auto_conflict.status == 403);
    auto scoped_auto_create = scoped_autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "POST",
        .headers = {{"authorization", "Bearer auto-token-a"}},
        .body = agent::Value::object({
            {"id", "scoped-auto-a"},
            {"goal", "tenant autonomous"},
            {"input", agent::Value::object({{"kind", "scope"}})},
        }).stringify(0),
    });
    assert(scoped_auto_create.status == 202);
    auto scoped_auto_payload = agent::parse_json(scoped_auto_create.body);
    assert(scoped_auto_payload.at("run").at("metadata").at("ownerApiKeyId").as_string() == "auto-key-a");
    assert(scoped_auto_payload.at("run").at("metadata").at("tenantId").as_string() == "tenant-a");
    assert(scoped_auto_payload.at("run").at("metadata").at("source").is_null());
    auto scoped_auto_list_b = scoped_autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs",
        .method = "GET",
        .headers = {{"authorization", "Bearer auto-token-b"}},
    });
    assert(scoped_auto_list_b.status == 200);
    assert(agent::parse_json(scoped_auto_list_b.body).at("items").as_array().empty());
    auto scoped_auto_get_b = scoped_autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/scoped-auto-a",
        .method = "GET",
        .headers = {{"authorization", "Bearer auto-token-b"}},
    });
    assert(scoped_auto_get_b.status == 404);
    auto scoped_auto_get_a = scoped_autonomous_server.handle(agent::HttpRequest{
        .url = "/autonomous-runs/scoped-auto-a",
        .method = "GET",
        .headers = {{"authorization", "Bearer auto-token-a"}},
    });
    assert(scoped_auto_get_a.status == 200);
  }
  {
    agent::InMemoryWorkflowStore route_workflow_store;
    agent::WorkflowEngine route_workflow_engine(nullptr, nullptr, &route_workflow_store);
    auto human_workflow = agent::WorkflowBuilder("route_human_workflow")
                              .start()
                              .human_wait("approve", "Approve route")
                              .end("end", agent::workflow_ref("nodes.approve.output.value"))
                              .sequence({"start", "approve", "end"})
                              .build();
    agent::AgentServerApp workflow_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .workflow = agent::AgentServerWorkflowConfig{
            .engine = &route_workflow_engine,
            .store = &route_workflow_store,
            .definition = human_workflow,
        },
    });
    auto workflow_run_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/run",
        .method = "POST",
        .body = agent::Value::object({
            {"input", agent::Value::object({{"subject", "route"}})},
            {"context", agent::Value::object({{"tenant", "native"}})},
        }).stringify(0),
    });
    assert(workflow_run_response.status == 200);
    auto workflow_run = agent::parse_json(workflow_run_response.body);
    const auto workflow_run_id = workflow_run.at("workflowRunId").as_string();
    assert(!workflow_run_id.empty());
    assert(workflow_run.at("status").as_string() == "waiting");
    assert(workflow_run.at("state").at("context").at("server").at("path").as_string() == "/workflows/run");
    auto workflow_get_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + workflow_run_id,
        .method = "GET",
    });
    assert(workflow_get_response.status == 200);
    assert(agent::parse_json(workflow_get_response.body).at("workflowRunId").as_string() == workflow_run_id);
    auto workflow_checkpoints_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + workflow_run_id + "/checkpoints",
        .method = "GET",
    });
    assert(workflow_checkpoints_response.status == 200);
    assert(agent::parse_json(workflow_checkpoints_response.body).at("checkpoints").as_array().size() >= 2);
    auto workflow_human_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + workflow_run_id + "/human-response",
        .method = "POST",
        .body = agent::Value::object({{"nodeId", "approve"}, {"payload", "approved"}}).stringify(0),
    });
    assert(workflow_human_response.status == 200);
    assert(agent::parse_json(workflow_human_response.body).at("status").as_string() == "completed");
    assert(agent::parse_json(workflow_human_response.body).at("output").as_string() == "approved");

    auto webhook_workflow = agent::WorkflowBuilder("route_webhook_workflow")
                                .start()
                                .webhook_wait("hook", "Webhook route")
                                .end("end", agent::workflow_ref("nodes.hook.output.value"))
                                .sequence({"start", "hook", "end"})
                                .build();
    auto webhook_run_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/run",
        .method = "POST",
        .body = agent::Value::object({
            {"definition", agent::workflow_definition_to_value(webhook_workflow)},
            {"input", agent::Value::object({{"subject", "webhook"}})},
        }).stringify(0),
    });
    assert(webhook_run_response.status == 200);
    auto webhook_run = agent::parse_json(webhook_run_response.body);
    const auto webhook_run_id = webhook_run.at("workflowRunId").as_string();
    assert(webhook_run.at("status").as_string() == "waiting");
    auto workflow_webhook_response = workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + webhook_run_id + "/webhook",
        .method = "POST",
        .body = agent::Value::object({
            {"nodeId", "hook"},
            {"payload", agent::Value::object({{"ok", true}})},
        }).stringify(0),
    });
    assert(workflow_webhook_response.status == 200);
    assert(agent::parse_json(workflow_webhook_response.body).at("status").as_string() == "completed");
    assert(agent::parse_json(workflow_webhook_response.body).at("output").at("ok").as_bool());
  }
  {
    const auto server_config_workflow_dir =
        smoke_artifacts_root() / ("server-config-workflow-" + agent::generate_uuid());
    std::filesystem::create_directories(server_config_workflow_dir);
    auto server_config_with_workflow = agent::Value::object({
        {"resources", agent::Value::object({
            {"workflowEngines", agent::Value::object({
                {"primary", agent::Value::object({
                    {"store", agent::Value::object({
                        {"kind", "file"},
                        {"baseDir", server_config_workflow_dir.string()},
                    })},
                })},
            })},
        })},
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"tools", agent::Value::object({{"bundles", agent::Value::array({"core", "workflow"})}})},
                {"workflow", agent::Value::object({
                    {"engine", agent::Value::object({{"use", "primary"}})},
                })},
            })},
        })},
    });
    auto seeded_workflow_app = agent::resolve_native_agent_app(server_config_with_workflow, "main");
    auto config_route_workflow = agent::WorkflowBuilder("config_route_workflow")
                                     .start()
                                     .human_wait("approve", "Approve config route")
                                     .end("end", agent::workflow_ref("nodes.approve.output.value"))
                                     .sequence({"start", "approve", "end"})
                                     .build();
    auto config_waiting_workflow = seeded_workflow_app.workflow->engine->run(config_route_workflow);
    assert(config_waiting_workflow.status == agent::WorkflowStatus::Waiting);
    agent::AgentServerApp config_workflow_server(agent::AgentServerOptions{.allow_unauthenticated = true,
        .config = server_config_with_workflow,
        .agent = "main",
        .api_keys = {
            agent::AgentServerApiKey{.id = "workflow-main", .token = "workflow-token", .agents = {"main"}},
            agent::AgentServerApiKey{.id = "workflow-other", .token = "workflow-other-token", .agents = {"other"}},
        },
    });
    auto config_workflow_denied = config_workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + config_waiting_workflow.workflow_run_id,
        .method = "GET",
        .headers = {{"authorization", "Bearer workflow-other-token"}},
    });
    assert(config_workflow_denied.status == 403);
    auto config_workflow_get = config_workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + config_waiting_workflow.workflow_run_id,
        .method = "GET",
        .headers = {{"authorization", "Bearer workflow-token"}},
    });
    assert(config_workflow_get.status == 200);
    assert(agent::parse_json(config_workflow_get.body).at("workflowRunId").as_string() ==
           config_waiting_workflow.workflow_run_id);
    auto config_workflow_checkpoints = config_workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + config_waiting_workflow.workflow_run_id + "/checkpoints",
        .method = "GET",
        .headers = {{"authorization", "Bearer workflow-token"}},
    });
    assert(config_workflow_checkpoints.status == 200);
    assert(agent::parse_json(config_workflow_checkpoints.body).at("checkpoints").as_array().size() >= 2);
    auto config_workflow_human_response = config_workflow_server.handle(agent::HttpRequest{
        .url = "/workflows/" + config_waiting_workflow.workflow_run_id + "/human-response",
        .method = "POST",
        .headers = {{"authorization", "Bearer workflow-token"}},
        .body = agent::Value::object({{"nodeId", "approve"}, {"payload", "config approved"}}).stringify(0),
    });
    assert(config_workflow_human_response.status == 200);
    auto config_workflow_completed = agent::parse_json(config_workflow_human_response.body);
    assert(config_workflow_completed.at("status").as_string() == "completed");
    assert(config_workflow_completed.at("output").as_string() == "config approved");
  }

  auto more_tools = agent::create_builtin_tools({"core", "local"});
  agent::ToolRegistry all_tool_registry(more_tools);
  assert(all_tool_registry.get("math.eval") != nullptr);
  assert(all_tool_registry.get("fs.readText") != nullptr);

  auto builtin_bundle_registry = agent::create_builtin_tool_bundle_registry();
  const auto* web_provider = builtin_bundle_registry.get("web");
  assert(web_provider != nullptr);
  assert(web_provider->metadata.tier == "portable");
  assert(contains_string(web_provider->metadata.tags, "fetch"));
  assert(contains_string(web_provider->metadata.capabilities, "network.http.read"));
  auto builtin_bundle_metadata = agent::list_builtin_tool_bundle_metadata();
  assert(!builtin_bundle_metadata.empty());
  assert(builtin_bundle_metadata.front().name == "core");
  bool saw_browser_metadata = false;
  for (const auto& metadata : builtin_bundle_metadata) {
    if (metadata.name == "browser") {
      saw_browser_metadata = true;
      assert(metadata.tier == "host-sensitive");
      assert(contains_string(metadata.capabilities, "browser.screenshot"));
    }
  }
  assert(saw_browser_metadata);
  auto all_builtin_bundle_tools = builtin_bundle_registry.create_tools();
  agent::ToolRegistry all_builtin_bundle_registry(all_builtin_bundle_tools);
  assert(all_builtin_bundle_registry.get("time.now") != nullptr);
  assert(all_builtin_bundle_registry.get("web.search") != nullptr);
  assert(all_builtin_bundle_registry.get("workflow.getRun") != nullptr);

  agent::ToolBundleRegistry custom_bundle_registry({
      agent::ToolBundleProvider{
          .metadata = agent::ToolBundleMetadata{
              .name = "first",
              .tags = {"first", "shared"},
              .capabilities = {"cap.first"},
          },
          .create_tools = [] { return agent::create_core_builtin_tools(); },
      },
      agent::ToolBundleProvider{
          .metadata = agent::ToolBundleMetadata{
              .name = "second",
              .tags = {"second", "shared"},
              .capabilities = {"cap.second"},
          },
          .create_tools = [] {
            auto tools = agent::create_core_builtin_tools();
            tools.push_back(agent::define_tool(agent::ToolDefinition{
                .name = "custom.unique",
                .description = "Custom unique tool.",
                .capabilities = {"tool.cap"},
                .risk_level = agent::ToolRiskLevel::Low,
                .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
                  return agent::Value::object({{"ok", true}});
                },
            }));
            return tools;
          },
      },
  });
  auto custom_bundle_tools = custom_bundle_registry.create_tools({"first", "second"});
  agent::ToolRegistry custom_registry(custom_bundle_tools);
  const auto* first_time_tool = custom_registry.get("time.now");
  assert(first_time_tool != nullptr);
  assert(first_time_tool->bundle == "first");
  assert(contains_string(first_time_tool->tags, "first"));
  assert(contains_string(first_time_tool->tags, "shared"));
  assert(contains_string(first_time_tool->tags, "bundle:first"));
  assert(!contains_string(first_time_tool->tags, "second"));
  assert(contains_string(first_time_tool->capabilities, "cap.first"));
  assert(!contains_string(first_time_tool->capabilities, "cap.second"));
  const auto* unique_tool = custom_registry.get("custom.unique");
  assert(unique_tool != nullptr);
  assert(unique_tool->bundle == "second");
  assert(contains_string(unique_tool->tags, "second"));
  assert(contains_string(unique_tool->tags, "bundle:second"));
  assert(contains_string(unique_tool->capabilities, "cap.second"));
  assert(contains_string(unique_tool->capabilities, "tool.cap"));

  agent::ToolBundleRegistry concurrent_bundle_registry;
  std::atomic<int> concurrent_bundle_ready{0};
  std::atomic<bool> concurrent_bundle_start{false};
  std::atomic<int> concurrent_bundle_factory_calls{0};
  constexpr int concurrent_bundle_thread_count = 8;
  std::vector<std::thread> concurrent_bundle_workers;
  for (int thread_index = 0; thread_index < concurrent_bundle_thread_count; ++thread_index) {
    concurrent_bundle_workers.emplace_back([&, thread_index]() {
      concurrent_bundle_ready.fetch_add(1);
      while (!concurrent_bundle_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(thread_index);
      const auto bundle_name = "bundle.concurrent." + suffix;
      const auto tool_name = "tool.concurrent." + suffix;
      concurrent_bundle_registry.register_provider(agent::ToolBundleProvider{
          .metadata = agent::ToolBundleMetadata{
              .name = bundle_name,
              .tags = {"concurrent"},
              .capabilities = {"cap.concurrent"},
          },
          .create_tools = [tool_name, &concurrent_bundle_factory_calls] {
            concurrent_bundle_factory_calls.fetch_add(1);
            return std::vector<agent::ToolDefinition>{agent::define_tool(agent::ToolDefinition{
                .name = tool_name,
                .description = "Concurrent bundle tool.",
                .execute = [tool_name](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
                  return agent::Value::object({{"tool", tool_name}});
                },
            })};
          },
      });
      auto concurrent_bundle_tools = concurrent_bundle_registry.create_tools({bundle_name});
      assert(concurrent_bundle_tools.size() == 1);
      assert(concurrent_bundle_tools.front().name == tool_name);
      assert(concurrent_bundle_tools.front().bundle == bundle_name);
      assert(contains_string(concurrent_bundle_tools.front().tags, "bundle:" + bundle_name));
      assert(contains_string(concurrent_bundle_tools.front().capabilities, "cap.concurrent"));
    });
  }
  while (concurrent_bundle_ready.load() < concurrent_bundle_thread_count) {
    std::this_thread::yield();
  }
  concurrent_bundle_start.store(true);
  for (auto& worker : concurrent_bundle_workers) {
    worker.join();
  }
  assert(concurrent_bundle_factory_calls.load() == concurrent_bundle_thread_count);

  agent::CancellationToken http_tool_cancellation;
  std::vector<agent::HttpRequest> captured_http_requests;
  auto http_tools = agent::create_http_builtin_tools([&](const agent::HttpRequest& request) {
    captured_http_requests.push_back(request);
    if (request.url.find("/not-json") != std::string::npos) {
      return agent::HttpResponse{
          .status = 200,
          .headers = {{"content-type", "text/plain"}},
          .body = "not json",
      };
    }
    if (request.url.find("/json") != std::string::npos) {
      return agent::HttpResponse{
          .status = 200,
          .headers = {{"content-type", "application/json"}},
          .body = "{\"answer\":42}",
      };
    }
    return agent::HttpResponse{
        .status = 202,
        .headers = {{"x-test", "ok"}},
        .body = "accepted",
    };
  });
  agent::ToolRegistry http_registry(http_tools);
  assert(http_registry.get("http.request") != nullptr);
  assert(http_registry.get("http.inspectJson") != nullptr);
  agent::ToolExecutor http_executor(http_registry);
  agent::ToolExecutionContext http_context;
  http_context.cancellation = &http_tool_cancellation;
  auto http_request_result = http_executor.execute_tool_call(agent::ToolCall{
      .id = "http_1",
      .name = "http.request",
      .arguments = agent::Value::object({
          {"url", "https://example.test/resource"},
          {"method", "post"},
          {"headers", agent::Value::object({{"x-mode", "native"}})},
          {"body", agent::Value::object({{"ok", true}})},
      }),
  }, http_context);
  assert(http_request_result.ok);
  assert(captured_http_requests.back().method == "POST");
  assert(captured_http_requests.back().headers.at("x-mode") == "native");
  assert(captured_http_requests.back().body.find("\"ok\":true") != std::string::npos);
  assert(captured_http_requests.back().cancellation == &http_tool_cancellation);
  assert(http_request_result.output.find("\"status\": 202") != std::string::npos);
  assert(http_request_result.output.find("\"x-test\": \"ok\"") != std::string::npos);
  auto http_json_result = http_executor.execute_tool_call(agent::ToolCall{
      .id = "http_2",
      .name = "http.inspectJson",
      .arguments = agent::Value::object({{"url", "https://example.test/json"}}),
  });
  assert(http_json_result.ok);
  assert(http_json_result.output.find("\"answer\": 42") != std::string::npos);
  auto http_invalid_json_result = http_executor.execute_tool_call(agent::ToolCall{
      .id = "http_invalid_json",
      .name = "http.inspectJson",
      .arguments = agent::Value::object({{"url", "https://example.test/not-json"}}),
  });
  assert(!http_invalid_json_result.ok);
  assert(http_invalid_json_result.output.find("Invalid JSON") != std::string::npos);
  auto http_bundle_tools = agent::create_builtin_tools({"http"});
  agent::ToolRegistry http_bundle_registry(http_bundle_tools);
  const auto* http_request_tool = http_bundle_registry.get("http.request");
  assert(http_request_tool != nullptr);
  assert(http_request_tool->bundle == "http");
  assert(contains_string(http_request_tool->tags, "builtin"));
  assert(contains_string(http_request_tool->tags, "bundle:http"));
  assert(contains_string(http_request_tool->capabilities, "network.http.read"));
  assert(contains_string(http_request_tool->capabilities, "network.http.write"));

  auto memory = std::make_shared<agent::LongTermMemory>();
  memory->remember("Native C++ port keeps the framework plugin-free.");
  auto hits = memory->search("plugin-free native framework", 2, 0.0);
  assert(!hits.empty());
  auto zero_memory_hits = memory->search("plugin-free native framework", 0, 0.0);
  assert(zero_memory_hits.empty());
  agent::LongTermMemory batch_memory(
      std::make_shared<FixedSearchEmbeddingAdapter>(),
      std::make_shared<agent::InMemoryVectorStore>(),
      "default",
      4,
      0.0);
  auto batch_records = batch_memory.remember_many({
      agent::RememberMemoryInput{
          .id = "batch-alpha",
          .content = "alpha batched memory",
          .metadata = agent::Value::object({{"kind", "batch"}}),
          .namespace_id = "batch",
      },
      agent::RememberMemoryInput{
          .id = "batch-anti",
          .content = "anti batched memory",
          .namespace_id = "batch",
      },
  });
  assert(batch_records.size() == 2);
  assert(batch_records.front().id == "batch-alpha");
  assert(batch_records.front().metadata.at("kind").as_string() == "batch");
  agent::LongTermMemoryConfig configured_memory_options;
  configured_memory_options.embedder = std::make_shared<FixedSearchEmbeddingAdapter>();
  configured_memory_options.store = std::make_shared<agent::InMemoryVectorStore>();
  configured_memory_options.namespace_id = "configured";
  configured_memory_options.top_k = 3;
  configured_memory_options.min_score = 0.0;
  configured_memory_options.context_title = "Configured memory";
  agent::LongTermMemory configured_memory(configured_memory_options);
  auto single_record = configured_memory.remember(agent::RememberMemoryInput{
      .id = "single-alpha",
      .content = "alpha object memory",
      .metadata = agent::Value::object({{"kind", "single"}}),
      .namespace_id = "configured",
  });
  assert(single_record.id == "single-alpha");
  assert(single_record.metadata.at("kind").as_string() == "single");
  auto single_record_value = agent::vector_memory_record_to_value(single_record);
  assert(single_record_value.at("id").as_string() == "single-alpha");
  assert(single_record_value.at("namespace").as_string() == "configured");
  assert(single_record_value.at("metadata").at("kind").as_string() == "single");
  assert(single_record_value.at("embedding").as_array().size() == 2);
  auto turn_record = configured_memory.remember_conversation_turn(agent::RememberConversationTurnInput{
      .session_id = "configured-session",
      .input = "remember the object input",
      .output = "object input remembered",
      .namespace_id = "configured",
      .metadata = agent::Value::object({{"source", "smoke"}}),
      .plan_steps = {"Inspect", "Store"},
  });
  assert(turn_record.metadata.at("sessionId").as_string() == "configured-session");
  assert(turn_record.metadata.at("type").as_string() == "conversation-turn");
  assert(turn_record.metadata.at("source").as_string() == "smoke");
  assert(turn_record.content.find("Plan: Inspect -> Store") != std::string::npos);
  auto nested_plan_record = configured_memory.remember_conversation_turn(agent::RememberConversationTurnInput{
      .session_id = "nested-plan-session",
      .input = "alpha nested plan",
      .output = "nested plan remembered",
      .namespace_id = "configured",
      .metadata = agent::Value::object({{"source", "nested"}}),
      .plan = agent::RememberConversationPlan{.steps = {
          agent::RememberConversationPlanStep{.title = "Read"},
          agent::RememberConversationPlanStep{.title = "Reply"},
      }},
      .plan_steps = {"Ignored"},
  });
  assert(nested_plan_record.metadata.at("sessionId").as_string() == "nested-plan-session");
  assert(nested_plan_record.metadata.at("source").as_string() == "nested");
  assert(nested_plan_record.content.find("Plan: Read -> Reply") != std::string::npos);
  assert(nested_plan_record.content.find("Ignored") == std::string::npos);
  auto configured_context = configured_memory.build_context_message("alpha object memory", 3, 0.0, "configured");
  assert(configured_context.hits.size() == 3);
  assert(configured_context.message);
  assert(agent::extract_text_content(configured_context.message->content).find("Configured memory") !=
         std::string::npos);
  agent::SearchMemoryOptions configured_search_options;
  configured_search_options.top_k = 1;
  configured_search_options.min_score = 0.0;
  configured_search_options.namespace_id = "configured";
  auto option_hits = configured_memory.search("alpha object memory", configured_search_options);
  assert(option_hits.size() == 1);
  auto option_hit_value = agent::retrieved_memory_to_value(option_hits.front());
  assert(option_hit_value.at("namespace").as_string() == "configured");
  assert(!option_hit_value.contains("namespaceId"));
  assert(option_hit_value.at("score").as_number() >= 0.0);
  auto option_context = configured_memory.build_context_message("alpha object memory", configured_search_options);
  assert(option_context.hits.size() == 1);
  assert(option_context.message);
  assert(option_context.message->metadata.at("hits").as_integer() == 1);
  auto option_context_value = agent::long_term_memory_context_result_to_value(option_context);
  assert(option_context_value.at("hits").as_array().size() == 1);
  assert(option_context_value.at("message").at("metadata").at("source").as_string() == "long-term-memory");
  auto batch_context = batch_memory.build_context_message("alpha follow-up", 2, 0.0, "batch");
  assert(batch_context.hits.size() == 1);
  assert(batch_context.hits.front().id == "batch-alpha");
  assert(batch_context.message);
  assert(batch_context.message->metadata.at("source").as_string() == "long-term-memory");
  assert(agent::extract_text_content(batch_context.message->content).find("alpha batched memory")
         != std::string::npos);
  agent::LongTermMemory zero_default_memory(
      std::make_shared<agent::HashEmbeddingAdapter>(),
      std::make_shared<agent::InMemoryVectorStore>(),
      "default",
      0,
      0.0);
  zero_default_memory.remember("Configured zero default long-term memory.");
  assert(zero_default_memory.search("zero default long-term memory").empty());
  const auto file_memory_path = smoke_artifacts_root() /
                                ("file-memory-" + agent::generate_uuid() + ".json");
  std::filesystem::create_directories(file_memory_path.parent_path());
  auto file_memory_store = std::make_shared<agent::FileVectorStore>(file_memory_path);
  agent::LongTermMemory file_memory(
      std::make_shared<agent::HashEmbeddingAdapter>(),
      file_memory_store,
      "default",
      4,
      0.0);
  file_memory.remember("File-backed memory honors explicit zero topK.");
  assert(!file_memory.search("explicit zero topK", 1, 0.0).empty());
  assert(file_memory.search("explicit zero topK", 0, 0.0).empty());

  agent::KnowledgeBase kb("docs", "default", "Native Docs");
  auto ingestion = kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "native.md",
          .title = "Native Port",
          .content = "The native C++ agent framework has tools, memory, workflow, and RAG retrieval.",
      },
  });
  assert(ingestion.document_count == 1);
  assert(ingestion.chunk_count >= 1);
  auto unchanged_ingestion = kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "native.md",
          .title = "Native Port",
          .content = "The native C++ agent framework has tools, memory, workflow, and RAG retrieval.",
      },
  });
  assert(unchanged_ingestion.document_count == 0);
  assert(unchanged_ingestion.chunk_count == 0);
  assert(kb.stats().document_count == 1);
  auto forced_unchanged_ingestion = kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "native.md",
          .title = "Native Port",
          .content = "The native C++ agent framework has tools, memory, workflow, and RAG retrieval.",
      },
  }, agent::KnowledgeIngestOptions{.skip_if_unchanged = false});
  assert(forced_unchanged_ingestion.document_count == 1);
  assert(kb.stats().document_count == 1);
  agent::KnowledgeBase duplicate_ingest_kb("dedupe-docs", "default", "Dedupe Docs");
  auto duplicate_ingest = duplicate_ingest_kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{.uri = "dup.md", .title = "Dup", .content = "first version"},
      agent::LoadedKnowledgeDocument{.uri = "dup.md", .title = "Dup", .content = "second version"},
      agent::LoadedKnowledgeDocument{.uri = "dup.md", .title = "Dup", .content = "second version"},
  });
  assert(duplicate_ingest.document_count == 1);
  assert(duplicate_ingest_kb.store()->list_documents().size() == 1);
  assert(duplicate_ingest_kb.store()->list_documents().front().content == "second version");
  auto knowledge_hits = kb.search("workflow rag retrieval", agent::KnowledgeSearchOptions{.top_k = 2});
  assert(!knowledge_hits.empty());
  bool knowledge_ingest_cancellation_called = false;
  agent::CancellationToken knowledge_ingest_cancellation;
  auto knowledge_ingest_cancel_embedder = std::make_shared<agent::OpenAIEmbeddingAdapter>(
      "text-embedding-3-small",
      [&](const agent::NativeProviderRequest& request) {
        knowledge_ingest_cancellation_called = true;
        assert(request.cancellation == &knowledge_ingest_cancellation);
        request.cancellation->cancel("knowledge ingest abort");
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.1, 0.2})}}),
            })},
        });
      },
      "openai-embedding-key");
  agent::KnowledgeBase knowledge_ingest_cancel_kb("ingest-cancel",
                                                  "default",
                                                  "Ingest Cancel",
                                                  knowledge_ingest_cancel_embedder);
  try {
    (void)knowledge_ingest_cancel_kb.ingest_loaded_documents({
        agent::LoadedKnowledgeDocument{
            .uri = "cancel.md",
            .title = "Cancel",
            .content = "This ingestion should be cancelled during embedding.",
        },
    }, agent::KnowledgeIngestOptions{.cancellation = &knowledge_ingest_cancellation});
    assert(false);
  } catch (const agent::AgentFrameworkError& error) {
    assert(std::string(error.what()).find("knowledge ingest abort") != std::string::npos);
  }
  assert(knowledge_ingest_cancellation_called);
  assert(knowledge_ingest_cancel_kb.stats().document_count == 0);

  const auto tmp_dir = smoke_artifacts_root();
  std::filesystem::create_directories(tmp_dir);

  const auto knowledge_dir = tmp_dir / ("knowledge-" + agent::generate_uuid());
  std::filesystem::create_directories(knowledge_dir / "nested");
  std::filesystem::create_directories(knowledge_dir / "src");
  {
    std::ofstream text_file(knowledge_dir / "guide.md");
    text_file << "# Native Guide\nWorkflow retrieval from directory loader.";
    std::ofstream markdown_nested(knowledge_dir / "nested" / "deep.mdx");
    markdown_nested << "# Deep Markdown\nRecursive markdown loader content.";
    std::ofstream markdown_ignored(knowledge_dir / "nested" / "ignored.txt");
    markdown_ignored << "# Ignored Text\nThis should not be loaded by markdown loader.";
    std::ofstream html_file(knowledge_dir / "nested" / "page.html");
    html_file << "<html><title>Nested Page</title><body>Directory HTML knowledge.</body></html>";
    std::ofstream repo_file(knowledge_dir / "src" / "index.ts");
    repo_file << "export const repositoryKnowledge = true;\n";
    std::ofstream pdf_file(knowledge_dir / "paper.pdf", std::ios::binary);
    pdf_file << "%PDF-1.4\n"
             << "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
             << "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
             << "3 0 obj << /Type /Page /Parent 2 0 R /Contents 4 0 R >> endobj\n"
             << "4 0 obj << /Length 64 >>\n"
             << "stream\n"
             << "BT\n"
             << "/F1 12 Tf\n"
             << "72 720 Td\n"
             << "(Native PDF Knowledge) Tj\n"
             << "ET\n"
             << "endstream\n"
             << "endobj\n"
             << "trailer << /Root 1 0 R >>\n"
             << "%%EOF\n";
  }
  agent::NativeWebPageFetcher knowledge_fetcher;
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://knowledge.example.test/web",
      .content_type = "text/html",
      .title = "Web Knowledge",
      .html = "<html><body>Web loader native knowledge.</body></html>",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://knowledge.example.test/",
      .content_type = "text/html",
      .title = "Knowledge Home",
      .html = "<html><body>Home <a href=\"/workflow\">Workflow</a></body></html>",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://knowledge.example.test/workflow",
      .content_type = "text/html",
      .title = "Workflow Knowledge",
      .html = "<html><body>Website crawler knowledge workflow.</body></html>",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://knowledge.example.test/sitemap.xml",
      .content_type = "application/xml",
      .text = "<urlset><url><loc>https://knowledge.example.test/web</loc></url></urlset>",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://api.github.com/repos/openai/example",
      .content_type = "application/json",
      .text = "{\"default_branch\":\"main\"}",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://api.github.com/repos/openai/example/git/trees/main?recursive=1",
      .content_type = "application/json",
      .text = "{\"tree\":[{\"path\":\"src/index.ts\",\"type\":\"blob\"},{\"path\":\"README.md\",\"type\":\"blob\"}]}",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://raw.githubusercontent.com/openai/example/main/src/index.ts",
      .content_type = "text/plain",
      .text = "export const repo = true;\n",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://api.github.com/repos/openai/example/git/trees/feature%2Fdocs?recursive=1",
      .content_type = "application/json",
      .text = "{\"tree\":[{\"path\":\"docs/guide.md\",\"type\":\"blob\"}]}",
  });
  knowledge_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://raw.githubusercontent.com/openai/example/feature%2Fdocs/docs/guide.md",
      .content_type = "text/plain",
      .text = "# Feature Docs\nEncoded branch ref content.\n",
  });
  agent::NativeWebCrawler knowledge_crawler(&knowledge_fetcher);
  auto knowledge_loader = agent::create_web_enabled_knowledge_loader(&knowledge_fetcher, &knowledge_crawler);
  bool pdf_requires_optional_document_io = false;
  try {
    (void)knowledge_loader->load(agent::Value::object({
        {"type", "file"},
        {"path", (knowledge_dir / "paper.pdf").string()},
    }));
  } catch (const agent::ConfigurationError& error) {
    pdf_requires_optional_document_io =
        std::string(error.what()).find("optional document I/O pipeline") != std::string::npos;
  }
  assert(pdf_requires_optional_document_io);
  auto markdown_documents = knowledge_loader->load(agent::Value::object({
      {"type", "markdown"},
      {"path", knowledge_dir.string()},
      {"metadata", agent::Value::object({{"scope", "markdown-docs"}})},
  }));
  assert(markdown_documents.size() == 2);
  bool saw_markdown_guide = false;
  bool saw_markdown_nested = false;
  for (const auto& document : markdown_documents) {
    assert(document.source_type == "markdown");
    assert(document.metadata.at("loader").as_string() == "markdown");
    assert(document.metadata.at("scope").as_string() == "markdown-docs");
    assert(document.uri.find("ignored.txt") == std::string::npos);
    saw_markdown_guide = saw_markdown_guide || document.title == "Native Guide";
    saw_markdown_nested = saw_markdown_nested || document.title == "Deep Markdown";
  }
  assert(saw_markdown_guide);
  assert(saw_markdown_nested);
  auto markdown_shallow_documents = knowledge_loader->load(agent::Value::object({
      {"type", "markdown"},
      {"path", knowledge_dir.string()},
      {"recursive", false},
  }));
  assert(markdown_shallow_documents.size() == 1);
  assert(markdown_shallow_documents.front().title == "Native Guide");
  auto markdown_file_documents = knowledge_loader->load(agent::Value::object({
      {"type", "markdown"},
      {"path", (knowledge_dir / "guide.md").string()},
  }));
  assert(markdown_file_documents.size() == 1);
  assert(markdown_file_documents.front().source_type == "markdown");
  auto repository_documents = knowledge_loader->load(agent::Value::object({
      {"type", "repository"},
      {"path", knowledge_dir.string()},
      {"include", agent::Value::array({"src/**"})},
      {"extensions", agent::Value::array({".ts"})},
      {"metadata", agent::Value::object({{"scope", "local-repo"}})},
  }));
  assert(repository_documents.size() == 1);
  assert(repository_documents.front().source_type == "repository");
  assert(repository_documents.front().metadata.at("repositoryPath").as_string() == "src/index.ts");
  auto github_documents = knowledge_loader->load(agent::Value::object({
      {"type", "github"},
      {"url", "https://github.com/openai/example/tree/main/src"},
      {"extensions", agent::Value::array({".ts"})},
  }));
  assert(github_documents.size() == 1);
  assert(github_documents.front().source_type == "github");
  assert(github_documents.front().title == "index.ts");
  assert(github_documents.front().metadata.at("repositoryName").as_string() == "example");
  auto github_ref_documents = knowledge_loader->load(agent::Value::object({
      {"type", "github"},
      {"url", "https://github.com/openai/example"},
      {"ref", "feature/docs"},
      {"extensions", agent::Value::array({".md"})},
  }));
  assert(github_ref_documents.size() == 1);
  assert(github_ref_documents.front().source_type == "github");
  assert(github_ref_documents.front().metadata.at("repositoryRef").as_string() == "feature/docs");
  assert(github_ref_documents.front().content.find("Encoded branch ref content") != std::string::npos);
  auto loaded_knowledge = agent::load_knowledge_sources({
      agent::Value::object({{"type", "text"}, {"text", "Inline native knowledge."}, {"title", "Inline"}}),
      agent::Value::object({{"type", "file"}, {"path", (knowledge_dir / "guide.md").string()}}),
      agent::Value::object({{"type", "markdown"}, {"path", knowledge_dir.string()}, {"recursive", false}}),
      agent::Value::object({{"type", "directory"},
                            {"path", knowledge_dir.string()},
                            {"recursive", true},
                            {"excludeExtensions", agent::Value::array({".pdf"})}}),
      agent::Value::object({{"type", "web"}, {"url", "https://knowledge.example.test/web"}}),
      agent::Value::object({{"type", "website"}, {"url", "https://knowledge.example.test/"}, {"maxDepth", 1}, {"maxPages", 3}}),
      agent::Value::object({{"type", "sitemap"}, {"url", "https://knowledge.example.test/sitemap.xml"}, {"limit", 1}}),
  }, *knowledge_loader);
  assert(loaded_knowledge.size() >= 6);
  bool saw_website = false;
  bool saw_sitemap = false;
  bool saw_markdown = false;
  for (const auto& document : loaded_knowledge) {
    saw_website = saw_website || document.source_type == "website";
    saw_sitemap = saw_sitemap || document.source_type == "sitemap";
    saw_markdown = saw_markdown || document.source_type == "markdown";
  }
  assert(saw_website);
  assert(saw_sitemap);
  assert(saw_markdown);
  auto zero_limit_sitemap_documents = knowledge_loader->load(agent::Value::object({
      {"type", "sitemap"},
      {"url", "https://knowledge.example.test/sitemap.xml"},
      {"limit", 0},
  }));
  assert(zero_limit_sitemap_documents.empty());
  agent::SitemapKnowledgeSourceLoader zero_default_sitemap_loader(&knowledge_fetcher,
                                                                 std::optional<std::size_t>{0});
  auto zero_default_sitemap_documents = zero_default_sitemap_loader.load(agent::Value::object({
      {"type", "sitemap"},
      {"url", "https://knowledge.example.test/sitemap.xml"},
  }));
  assert(zero_default_sitemap_documents.empty());
  auto zero_page_website_documents = knowledge_loader->load(agent::Value::object({
      {"type", "website"},
      {"url", "https://knowledge.example.test/"},
      {"maxDepth", 1},
      {"maxPages", 0},
  }));
  assert(zero_page_website_documents.empty());

  bool website_render_called = false;
  agent::BrowserCallbackRenderer knowledge_browser(
      [&](const agent::BrowserRenderRequest& request) {
        website_render_called = true;
        assert(request.url == "https://knowledge.example.test/workflow");
        return agent::BrowserRenderResult{
            .url = request.url,
            .title = "Rendered Workflow Knowledge",
            .html = "<html><body>Rendered website knowledge workflow.</body></html>",
            .text = "Rendered website knowledge workflow.",
        };
      });
  auto render_knowledge_loader =
      agent::create_web_enabled_knowledge_loader(&knowledge_fetcher, &knowledge_crawler, &knowledge_browser);
  auto rendered_website_documents = render_knowledge_loader->load(agent::Value::object({
      {"type", "website"},
      {"url", "https://knowledge.example.test/workflow"},
      {"maxDepth", 0},
      {"maxPages", 1},
      {"render", true},
  }));
  assert(website_render_called);
  assert(rendered_website_documents.size() == 1);
  assert(rendered_website_documents.front().title == "Rendered Workflow Knowledge");
  assert(rendered_website_documents.front().content.find("Rendered website knowledge") != std::string::npos);

  auto loader_registry =
      agent::create_web_enabled_knowledge_loader_registry(&knowledge_fetcher, &knowledge_crawler, &knowledge_browser);
  auto loader_provider_names = loader_registry.list_names();
  assert(contains_string(loader_provider_names, "text"));
  assert(contains_string(loader_provider_names, "file"));
  assert(contains_string(loader_provider_names, "directory"));
  assert(contains_string(loader_provider_names, "markdown"));
  assert(contains_string(loader_provider_names, "repository"));
  assert(contains_string(loader_provider_names, "web"));
  assert(contains_string(loader_provider_names, "website"));
  assert(contains_string(loader_provider_names, "sitemap"));
  assert(contains_string(loader_provider_names, "composite"));
  const auto* markdown_loader_provider = loader_registry.get("markdown");
  assert(markdown_loader_provider != nullptr);
  assert(markdown_loader_provider->metadata.tier == "portable");
  assert(contains_string(markdown_loader_provider->metadata.tags, "markdown"));
  auto registry_markdown_loader = loader_registry.create("markdown", agent::Value::object({{"recursive", false}}));
  auto registry_markdown_documents = registry_markdown_loader->load(agent::Value::object({
      {"type", "markdown"},
      {"path", knowledge_dir.string()},
  }));
