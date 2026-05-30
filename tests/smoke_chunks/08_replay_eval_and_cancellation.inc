  SMOKE_CHECK(pg_task.input.at("prompt").as_string() == "hello");
  SMOKE_CHECK(pg_task.idempotency_key == "idem-1");
  SMOKE_CHECK(!pg_task_client->queries.empty());
  SMOKE_CHECK(pg_task_client->queries.front().sql.find("INSERT INTO") != std::string::npos);
  SMOKE_CHECK(pg_task_client->queries.front().params.at(2).as_string() == "{\"prompt\":\"hello\"}");

  auto pg_claim = pg_task_store.claim_task(agent::TaskScopeFilter{
      .type = "agent.run",
      .tenant_id = std::optional<std::string>("tenant-a"),
  }, 10000);
  SMOKE_CHECK(pg_claim);
  SMOKE_CHECK(pg_claim->task.status == agent::TaskStatus::Running);
  SMOKE_CHECK(pg_claim->run.task_id == "task-claim");
  SMOKE_CHECK(pg_claim->run.attempt == 3);
  SMOKE_CHECK(pg_task_client->queries.size() >= 2);
  const auto first_claim_query = std::find_if(
      pg_task_client->queries.begin(),
      pg_task_client->queries.end(),
      [](const FakePgTaskClient::QueryRecord& record) { return record.sql == "BEGIN"; });
  SMOKE_CHECK(first_claim_query != pg_task_client->queries.end());
  SMOKE_CHECK(pg_task_client->queries.back().sql == "COMMIT");
  const auto has_skip_locked = std::any_of(
      pg_task_client->queries.begin(),
      pg_task_client->queries.end(),
      [](const FakePgTaskClient::QueryRecord& record) {
        return record.sql.find("FOR UPDATE SKIP LOCKED") != std::string::npos;
      });
  SMOKE_CHECK(has_skip_locked);

  {
    auto pg_ready_client = std::make_shared<FakePgTaskClient>();
    agent::PgTaskStore pg_ready_store(agent::PgTaskStoreConfig{
        .client = pg_ready_client,
        .create_tables = true,
    });
    std::atomic<int> pg_ready_threads{0};
    std::atomic<bool> pg_ready_start{false};
    constexpr int pg_ready_thread_count = 8;
    std::vector<std::thread> pg_ready_workers;
    for (int thread_index = 0; thread_index < pg_ready_thread_count; ++thread_index) {
      pg_ready_workers.emplace_back([&, thread_index]() {
        pg_ready_threads.fetch_add(1);
        while (!pg_ready_start.load()) {
          std::this_thread::yield();
        }
        auto tasks = pg_ready_store.list_tasks(agent::TaskScopeFilter{
            .type = thread_index % 2 == 0 ? "agent.run" : "",
        });
        SMOKE_CHECK(tasks.empty());
      });
    }
    while (pg_ready_threads.load() < pg_ready_thread_count) {
      std::this_thread::yield();
    }
    pg_ready_start.store(true);
    for (auto& worker : pg_ready_workers) {
      worker.join();
    }
    const auto create_schema_count = std::count_if(
        pg_ready_client->queries.begin(),
        pg_ready_client->queries.end(),
        [](const FakePgTaskClient::QueryRecord& record) {
          return record.sql.find("CREATE SCHEMA") != std::string::npos;
        });
    const auto create_table_count = std::count_if(
        pg_ready_client->queries.begin(),
        pg_ready_client->queries.end(),
        [](const FakePgTaskClient::QueryRecord& record) {
          return record.sql.find("CREATE TABLE") != std::string::npos;
        });
    SMOKE_CHECK(create_schema_count == 1);
    SMOKE_CHECK(create_table_count == 5);
  }

  agent::InMemoryArtifactStore artifacts;
  artifacts.set("answer", agent::Value::object({{"text", "stored"}}));
  SMOKE_CHECK(artifacts.get("answer"));
  artifacts.clear();
  SMOKE_CHECK(!artifacts.get("answer"));
  SMOKE_CHECK(!artifacts.history().empty());
  SMOKE_CHECK(artifacts.history().back().key == "*");
  SMOKE_CHECK(artifacts.history().back().event == "clear-all");
  agent::InMemorySharedStateStore shared_state;
  shared_state.merge("worker:a", agent::Value::object({{"count", 1}}));
  shared_state.merge("worker:a", agent::Value::object({{"done", true}}));
  SMOKE_CHECK(shared_state.get("worker:a")->value.at("done").as_bool());
  agent::InMemoryMailbox mailbox;
  auto envelope = mailbox.publish("worker.assignments", "supervisor",
                                  agent::Value::object({{"input", "route me"}}), "worker-a", "assignment");
  auto messages = mailbox.receive(agent::MailboxReadOptions{.channel = "worker.assignments", .limit = 1});
  SMOKE_CHECK(messages.size() == 1);
  mailbox.ack({envelope.id});
  SMOKE_CHECK(mailbox.receive(agent::MailboxReadOptions{.channel = "worker.assignments"}).empty());
  agent::RuleBasedTaskRouter router({agent::RoutingRule{.worker_id = "worker-a", .include = {"code"}}},
                                    "fallback");
  SMOKE_CHECK(router.route("please write code") == "worker-a");

  auto skill = agent::parse_anthropic_skill_markdown(
      "---\nname: demo\ndescription: Demo skill\nuser-invocable: true\nallowed-tools: math.eval\nmodel: skill-model\neffort: low\n---\nUse $ARGUMENTS in ${CLAUDE_SKILL_DIR}.",
      (tmp_dir / "demo" / "SKILL.md").string());
  auto rendered_skill_prompt = agent::render_skill_prompt(
      agent::parse_anthropic_skill_markdown(
          "---\nname: render\ndescription: Render skill\n---\nDeploy $1 then $ARGUMENTS[1] for ${CUSTOM_ENV}/${MISSING_ENV}.",
          (tmp_dir / "render" / "SKILL.md").string()),
      agent::SkillRenderContext{
          .arguments_text = "Prod(us-east-1) Feature_X",
          .env = {{"CUSTOM_ENV", "custom-value"}},
      });
  SMOKE_CHECK(rendered_skill_prompt == "Deploy Prod(us-east-1) then Feature_X for custom-value/.");
  agent::SkillRegistry skill_registry({skill});
  agent::SkillResolveOptions demo_opts;
  demo_opts.input_text = "/demo alpha beta";
  demo_opts.session_id = "session-1";
  auto skill_state = agent::resolve_skills_state(&skill_registry, demo_opts);
  SMOKE_CHECK(skill_state.active_skills.size() == 1);
  SMOKE_CHECK(skill_state.active_messages.front().content.front().text.find("alpha beta") != std::string::npos);
  SMOKE_CHECK(skill_state.allowed_tools.front() == "math.eval");
  SMOKE_CHECK(skill_state.model_settings.model == "skill-model");

  auto migration_skill = agent::parse_anthropic_skill_markdown(
      "---\nname: migration-audit\ndescription: Review database migrations and deployment safety\nallowed-tools: fs.read*\n---\nInspect migration risk.",
      (tmp_dir / "migration-audit" / "SKILL.md").string());
  auto disabled_migration_skill = agent::parse_anthropic_skill_markdown(
      "---\nname: disabled-migration\ndescription: Review database migrations and deployment safety\ndisable-model-invocation: true\n---\nDisabled.",
      (tmp_dir / "disabled-migration" / "SKILL.md").string());
  agent::SkillRegistry auto_skill_registry({skill, migration_skill, disabled_migration_skill});
  agent::SkillResolveOptions auto_opts;
  auto_opts.input_text = "please review the database migration safety before deploy";
  auto_opts.session_id = "session-2";
  auto auto_skill_state = agent::resolve_skills_state(&auto_skill_registry, auto_opts);
  SMOKE_CHECK((auto_skill_state.auto_selected_skills == std::vector<std::string>{"migration-audit"}));
  SMOKE_CHECK(auto_skill_state.active_skills.size() == 1);
  SMOKE_CHECK(auto_skill_state.active_skills.front().skill.manifest.name == "migration-audit");
  SMOKE_CHECK(auto_skill_state.allowed_tools.front() == "fs.read*");

  // === Multi-skill activation API: structural smoke coverage ===========
  {
    auto skill_a = agent::parse_anthropic_skill_markdown(
        "---\nname: act-a\ndescription: Activation A skill\nuser-invocable: true\nallowed-tools: tool.a\n---\nBody A.",
        (tmp_dir / "act-a" / "SKILL.md").string());
    auto skill_b = agent::parse_anthropic_skill_markdown(
        "---\nname: act-b\ndescription: Activation B skill\nuser-invocable: true\nallowed-tools: tool.b\n---\nBody B.",
        (tmp_dir / "act-b" / "SKILL.md").string());
    auto skill_b_not_user = agent::parse_anthropic_skill_markdown(
        "---\nname: act-b-noui\ndescription: Activation B (not user invocable)\nallowed-tools: tool.b\n---\nBody B.",
        (tmp_dir / "act-b-noui" / "SKILL.md").string());
    agent::SkillRegistry reg({skill_a, skill_b});

    // 1. Multi-activation rendering: two user-source activations both fire.
    {
      agent::SkillResolveOptions opts;
      opts.activations = {
          agent::SkillActivation{"act-a", "", agent::SkillActivationSource::User, 0},
          agent::SkillActivation{"act-b", "", agent::SkillActivationSource::User, 0},
      };
      auto state = agent::resolve_skills_state(&reg, opts);
      SMOKE_CHECK(state.active_skills.size() == 2);
      SMOKE_CHECK(state.active_messages.size() == 2);
      SMOKE_CHECK(state.allowed_tools.size() == 2);
      SMOKE_CHECK(state.allowed_tools[0] == "tool.a");
      SMOKE_CHECK(state.allowed_tools[1] == "tool.b");
    }

    // 2. Multi-slash parsing: only the user-invocable prefix is consumed.
    {
      agent::SkillRegistry reg_mixed({skill_a, skill_b_not_user});
      auto parsed = agent::parse_slash_activations("/act-a /act-b-noui shared text", reg_mixed);
      SMOKE_CHECK(parsed.activations.size() == 1);
      SMOKE_CHECK(parsed.activations.front().name == "act-a");
      // With a single consumed token + tail, the tail becomes BOTH args and stripped_input.
      SMOKE_CHECK(parsed.activations.front().arguments_text == "/act-b-noui shared text");
      SMOKE_CHECK(parsed.stripped_input == "/act-b-noui shared text");
    }

    // 2b. Multi-slash parsing: two user-invocable consumed, args remain empty.
    {
      auto parsed = agent::parse_slash_activations("/act-a /act-b shared text", reg);
      SMOKE_CHECK(parsed.activations.size() == 2);
      SMOKE_CHECK(parsed.activations[0].name == "act-a");
      SMOKE_CHECK(parsed.activations[1].name == "act-b");
      SMOKE_CHECK(parsed.activations[0].arguments_text.empty());
      SMOKE_CHECK(parsed.activations[1].arguments_text.empty());
      SMOKE_CHECK(parsed.stripped_input == "shared text");
    }

    // 3. Single-slash with args: arguments_text and stripped_input mirror remainder.
    {
      auto parsed = agent::parse_slash_activations("/act-a hello world", reg);
      SMOKE_CHECK(parsed.activations.size() == 1);
      SMOKE_CHECK(parsed.activations.front().arguments_text == "hello world");
      SMOKE_CHECK(parsed.stripped_input == "hello world");
    }

    // 4. User-source enforcement: activating a non-user-invocable as User must throw.
    {
      agent::SkillRegistry reg_noui({skill_b_not_user});
      bool threw = false;
      try {
        agent::SkillResolveOptions opts;
        opts.activations = {agent::SkillActivation{"act-b-noui", "", agent::SkillActivationSource::User, 0}};
        (void)agent::resolve_skills_state(&reg_noui, opts);
      } catch (const agent::ConfigurationError& error) {
        threw = true;
        const std::string what(error.what());
        SMOKE_CHECK(what.find("not user-invocable") != std::string::npos);
      }
      SMOKE_CHECK(threw);
    }

    // 5. Host source bypasses user_invocable gating.
    {
      agent::SkillRegistry reg_noui({skill_b_not_user});
      agent::SkillResolveOptions opts;
      opts.activations = {agent::SkillActivation{"act-b-noui", "", agent::SkillActivationSource::Host, 0}};
      auto state = agent::resolve_skills_state(&reg_noui, opts);
      SMOKE_CHECK(state.active_skills.size() == 1);
      SMOKE_CHECK(state.active_skills.front().skill.manifest.name == "act-b-noui");
    }

    // Build a registry with conflicting models / efforts for policy tests.
    auto skill_m1 = agent::parse_anthropic_skill_markdown(
        "---\nname: m1\ndescription: model conflict A\nuser-invocable: true\nmodel: gpt-4o\neffort: low\n---\nM1.",
        (tmp_dir / "m1" / "SKILL.md").string());
    auto skill_m2 = agent::parse_anthropic_skill_markdown(
        "---\nname: m2\ndescription: model conflict B\nuser-invocable: true\nmodel: claude-3-opus\neffort: high\n---\nM2.",
        (tmp_dir / "m2" / "SKILL.md").string());
    agent::SkillRegistry reg_mm({skill_m1, skill_m2});

    // 6. Conflict policy Error throws with "conflicting models".
    {
      agent::SkillResolveOptions opts;
      opts.activations = {
          agent::SkillActivation{"m1", "", agent::SkillActivationSource::Host, 1},
          agent::SkillActivation{"m2", "", agent::SkillActivationSource::Host, 1},
      };
      bool threw = false;
      try {
        (void)agent::resolve_skills_state(&reg_mm, opts);
      } catch (const agent::ConfigurationError& error) {
        threw = true;
        const std::string what(error.what());
        SMOKE_CHECK(what.find("conflicting models") != std::string::npos);
      }
      SMOKE_CHECK(threw);
    }

    // 7. Conflict policy HighestPriority picks the higher-priority skill's model.
    {
      agent::SkillResolveOptions opts;
      opts.model_conflict = agent::SkillConflictPolicy::HighestPriority;
      opts.effort_conflict = agent::SkillConflictPolicy::HighestPriority;
      opts.activations = {
          agent::SkillActivation{"m1", "", agent::SkillActivationSource::Host, 10},
          agent::SkillActivation{"m2", "", agent::SkillActivationSource::Host, 1},
      };
      auto state = agent::resolve_skills_state(&reg_mm, opts);
      SMOKE_CHECK(state.model_settings.model == "gpt-4o");
      SMOKE_CHECK(state.model_settings.reasoning);
      SMOKE_CHECK(std::holds_alternative<std::string>(state.model_settings.reasoning->budget));
      SMOKE_CHECK(std::get<std::string>(state.model_settings.reasoning->budget) == "low");
    }

    // 8. FirstWins / LastWins.
    {
      agent::SkillResolveOptions opts;
      opts.model_conflict = agent::SkillConflictPolicy::FirstWins;
      opts.effort_conflict = agent::SkillConflictPolicy::FirstWins;
      opts.activations = {
          agent::SkillActivation{"m1", "", agent::SkillActivationSource::Host, 0},
          agent::SkillActivation{"m2", "", agent::SkillActivationSource::Host, 0},
      };
      auto state = agent::resolve_skills_state(&reg_mm, opts);
      SMOKE_CHECK(state.model_settings.model == "gpt-4o");
    }
    {
      agent::SkillResolveOptions opts;
      opts.model_conflict = agent::SkillConflictPolicy::LastWins;
      opts.effort_conflict = agent::SkillConflictPolicy::LastWins;
      opts.activations = {
          agent::SkillActivation{"m1", "", agent::SkillActivationSource::Host, 0},
          agent::SkillActivation{"m2", "", agent::SkillActivationSource::Host, 0},
      };
      auto state = agent::resolve_skills_state(&reg_mm, opts);
      SMOKE_CHECK(state.model_settings.model == "claude-3-opus");
    }

    // 9. Caller's explicit model wins — even with conflict between skills.
    // The effort field uses the default Error policy; m1 and m2 disagree on
    // effort (low vs high) so we use HighestPriority for effort to avoid an
    // unrelated throw. Both priorities are zero so first-in-list (m1 → low)
    // wins on ties. The caller did NOT set reasoning explicitly.
    {
      agent::SkillResolveOptions opts;
      opts.model_settings.model = "gpt-4o-mini";
      opts.effort_conflict = agent::SkillConflictPolicy::HighestPriority;
      opts.activations = {
          agent::SkillActivation{"m1", "", agent::SkillActivationSource::Host, 0},
          agent::SkillActivation{"m2", "", agent::SkillActivationSource::Host, 0},
      };
      // Should NOT throw and should NOT overwrite the caller's choice.
      auto state = agent::resolve_skills_state(&reg_mm, opts);
      SMOKE_CHECK(state.model_settings.model == "gpt-4o-mini");
    }
  }

  const auto skill_project = tmp_dir / "skill-project";
  const auto audit_dir = skill_project / ".claude" / "skills" / "audit";
  std::filesystem::create_directories(audit_dir);
  {
    std::ofstream skill_file(audit_dir / "SKILL.md");
    skill_file << "---\n"
               << "name: audit\n"
               << "description: Audit the project\n"
               << "user-invocable: true\n"
               << "allowed-tools: [fs.read*, web.search]\n"
               << "paths: src, tests/unit\n"
               << "owner: platform\n"
               << "enabled: true\n"
               << "reviewers: [platform, \"security\"]\n"
               << "quotedFlag: \"true\"\n"
               << "---\n"
               << "Audit $ARGUMENTS.";
  }
  const auto loaded_skill = agent::load_skill_file(audit_dir / "SKILL.md", "project");
  SMOKE_CHECK(loaded_skill.manifest.name == "audit");
  SMOKE_CHECK(loaded_skill.manifest.allowed_tools.size() == 2);
  SMOKE_CHECK(loaded_skill.manifest.paths.size() == 2);
  SMOKE_CHECK(loaded_skill.manifest.paths[0] == "src");
  SMOKE_CHECK(loaded_skill.manifest.paths[1] == "tests/unit");
  SMOKE_CHECK(loaded_skill.manifest.metadata.at("owner").as_string() == "platform");
  SMOKE_CHECK(loaded_skill.manifest.metadata.at("enabled").as_bool());
  SMOKE_CHECK(loaded_skill.manifest.metadata.at("reviewers").as_array().size() == 2);
  SMOKE_CHECK(loaded_skill.manifest.metadata.at("reviewers").as_array()[1].as_string() == "security");
  SMOKE_CHECK(loaded_skill.manifest.metadata.at("quotedFlag").as_string() == "true");
  const auto directory_skills = agent::load_skill_directory(skill_project / ".claude" / "skills", "project");
  SMOKE_CHECK(directory_skills.size() == 1);
  const auto project_dirs = agent::collect_project_skill_directories(skill_project / "nested");
  SMOKE_CHECK(project_dirs.size() >= 2);
  agent::AnthropicSkillRegistryLoadOptions skill_load_options;
  skill_load_options.cwd = skill_project / "nested";
  skill_load_options.include_user = false;
  auto loaded_registry = agent::load_anthropic_skill_registry(skill_load_options);
  SMOKE_CHECK(loaded_registry.has("audit"));

  auto runtime_skill = agent::parse_anthropic_skill_markdown(
      "---\nname: audit\ndescription: Audit the project\nuser-invocable: true\nallowed-tools: fs.read*\n---\nAudit $ARGUMENTS.",
      (tmp_dir / "audit" / "SKILL.md").string());
  agent::AgentRunnerConfig skill_runner_config;
  skill_runner_config.adapter = std::make_shared<SkillRuntimeModel>();
  skill_runner_config.max_iterations = 3;
  skill_runner_config.skills = std::make_shared<agent::SkillRegistry>(
      std::vector<agent::SkillDefinition>{runtime_skill});
  skill_runner_config.permission_policy = [](const agent::PermissionRequest&) {
    return agent::PermissionDecision{agent::PermissionDecisionKind::Deny, "deny by default"};
  };
  agent::JsonSchema read_schema;
  read_schema.type = agent::JsonSchemaType::Object;
  read_schema.properties["path"].type = agent::JsonSchemaType::String;
  skill_runner_config.tools.push_back(agent::define_tool(agent::ToolDefinition{
      .name = "fs.readText",
      .description = "Read a text file.",
      .input_schema = read_schema,
      .capabilities = {"fs.read"},
      .risk_level = agent::ToolRiskLevel::Medium,
      .execute = [](const agent::Value& input, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        SMOKE_CHECK(context.services.at("skills").at("allowedTools").as_array().size() == 1);
        return agent::Value::object({{"content", "allowed " + input.at("path").as_string()}});
      },
  }));
  agent::AgentRunner skill_runner(std::move(skill_runner_config));
  auto skill_run = skill_runner.run("/audit auth module", "skill-session");
  SMOKE_CHECK(skill_run.text == "skill runtime ok");
  SMOKE_CHECK(agent::extract_text_content(skill_run.messages.front().content) == "auth module");

  auto fork_skill = agent::parse_anthropic_skill_markdown(
      "---\nname: fork-audit\ndescription: Fork audit with subagent\nuser-invocable: true\ncontext: fork\nagent: reviewer\nallowed-tools: fork.inspect\n---\nFork audit $ARGUMENTS.",
      (tmp_dir / "fork-audit" / "SKILL.md").string());
  agent::AgentRunner fork_child_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<SkillForkChildModel>(),
      .max_iterations = 1,
  });
  agent::AgentRunnerConfig fork_runner_config;
  fork_runner_config.adapter = std::make_shared<SkillForkMainModel>();
  fork_runner_config.max_iterations = 3;
  fork_runner_config.skills = std::make_shared<agent::SkillRegistry>(
      std::vector<agent::SkillDefinition>{fork_skill});
  fork_runner_config.skill_subagents["reviewer"] = &fork_child_runner;
  fork_runner_config.tools.push_back(agent::define_tool(agent::ToolDefinition{
      .name = "fork.inspect",
      .description = "Inspect fork skill service state.",
      .execute = [](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        const auto& skills = context.services.at("skills");
        SMOKE_CHECK(skills.at("names").as_array().size() == 1);
        SMOKE_CHECK(skills.at("forkResults").as_array().size() == 1);
        SMOKE_CHECK(skills.at("forkResults").as_array().front().at("name").as_string() == "fork-audit");
        SMOKE_CHECK(skills.at("forkResults").as_array().front().at("agent").as_string() == "reviewer");
        SMOKE_CHECK(skills.at("forkResults").as_array().front().at("text").as_string() == "child fork summary");
        return agent::Value::object({{"content", "fork-service-ok"}});
      },
  }));
  agent::AgentRunner fork_runner(std::move(fork_runner_config));
  auto fork_run = fork_runner.run("/fork-audit auth module", "fork-session");
  SMOKE_CHECK(fork_run.text == "main consumed fork");

  agent::Value config_with_skills = agent::Value::object({
      {"_cwd", skill_project.string()},
      {"agents", agent::Value::object({
          {"default", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({})}})},
              {"skills", agent::Value::object({
                  {"anthropic", agent::Value::object({{"cwd", "."}, {"includeUser", false}})},
                  {"defaultSkills", agent::Value::array({"audit"})},
                  {"advertiseSkills", false},
              })},
          })},
      })},
  });
  auto configured_app = agent::resolve_native_agent_app(config_with_skills);
  SMOKE_CHECK(configured_app.skills && configured_app.skills->has("audit"));

  agent::InProcessMCPServer mcp_server;
  mcp_server.register_tool(agent::define_tool(agent::ToolDefinition{
      .name = "echo",
      .description = "Echo text",
      .execute = [](const agent::Value& input, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value::object({{"text", input.at("text").as_string()}});
      },
  }));
  mcp_server.register_resource(agent::MCPResourceDefinition{.uri = "memory://one", .name = "One"},
                               agent::MCPResourceReadResult{.uri = "memory://one", .mime_type = "text/plain", .text = "resource text"});
  mcp_server.register_prompt(agent::MCPPromptDefinition{
      .name = "hello",
      .description = "Hello prompt",
      .arguments = {"name"},
      .argument_definitions = {agent::MCPPromptArgument{.name = "name",
                                                        .description = "Person to greet",
                                                        .required = true}},
      .render = [](const agent::Value& args) {
        return std::vector<agent::AgentMessage>{
            agent::create_message(agent::MessageRole::User, "Hello " + args.at("name").as_string("world"))};
      },
  });
  agent::InProcessMCPClient mcp_client(mcp_server);
  auto init = mcp_client.connect();
  SMOKE_CHECK(init.at("protocolVersion").as_string() == agent::MCP_PROTOCOL_VERSION);
  auto mcp_tool = mcp_client.call_tool("echo", agent::Value::object({{"text", "mcp"}}));
  SMOKE_CHECK(!mcp_tool.is_error);
  auto in_process_resource = mcp_client.read_resource("memory://one");
  SMOKE_CHECK(in_process_resource.text == "resource text");
  SMOKE_CHECK(in_process_resource.contents.size() == 1);
  SMOKE_CHECK(in_process_resource.contents.front().uri == "memory://one");
  SMOKE_CHECK(in_process_resource.contents.front().text == "resource text");
  auto in_process_prompts = mcp_client.list_prompts();
  SMOKE_CHECK(in_process_prompts.size() == 1);
  SMOKE_CHECK(in_process_prompts.front().arguments.size() == 1);
  SMOKE_CHECK(in_process_prompts.front().argument_definitions.size() == 1);
  SMOKE_CHECK(in_process_prompts.front().argument_definitions.front().description == "Person to greet");
  SMOKE_CHECK(in_process_prompts.front().argument_definitions.front().required);
  SMOKE_CHECK(agent::extract_text_content(mcp_client.get_prompt("hello", agent::Value::object({{"name", "C++"}})).front().content).find("C++") != std::string::npos);

  agent::InProcessMCPServer concurrent_mcp_server;
  std::atomic<int> concurrent_mcp_ready{0};
  std::atomic<bool> concurrent_mcp_start{false};
  std::atomic<int> concurrent_mcp_prompt_calls{0};
  constexpr int concurrent_mcp_thread_count = 8;
  std::vector<std::thread> concurrent_mcp_workers;
  for (int thread_index = 0; thread_index < concurrent_mcp_thread_count; ++thread_index) {
    concurrent_mcp_workers.emplace_back([&, thread_index]() {
      concurrent_mcp_ready.fetch_add(1);
      while (!concurrent_mcp_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(thread_index);
      const auto resource_uri = "memory://concurrent/" + suffix;
      const auto prompt_name = "prompt.concurrent." + suffix;
      concurrent_mcp_server.register_resource(
          agent::MCPResourceDefinition{.uri = resource_uri, .name = "Resource " + suffix},
          agent::MCPResourceReadResult{.uri = resource_uri, .mime_type = "text/plain", .text = "resource:" + suffix});
      concurrent_mcp_server.register_prompt(agent::MCPPromptDefinition{
          .name = prompt_name,
          .description = "Concurrent prompt",
          .arguments = {"name"},
          .render = [prompt_name, &concurrent_mcp_prompt_calls](const agent::Value& args) {
            concurrent_mcp_prompt_calls.fetch_add(1);
            return std::vector<agent::AgentMessage>{
                agent::create_message(agent::MessageRole::User,
                                      prompt_name + ":" + args.at("name").as_string())};
          },
      });
      auto resource = concurrent_mcp_server.read_resource(resource_uri);
      SMOKE_CHECK(resource.text == "resource:" + suffix);
      SMOKE_CHECK(resource.contents.size() == 1);
      SMOKE_CHECK(resource.contents.front().uri == resource_uri);
      auto resources = concurrent_mcp_server.list_resources();
      SMOKE_CHECK(std::any_of(resources.begin(), resources.end(), [&](const agent::MCPResourceDefinition& resource) {
        return resource.uri == resource_uri;
      }));
      auto prompts = concurrent_mcp_server.list_prompts();
      SMOKE_CHECK(std::any_of(prompts.begin(), prompts.end(), [&](const agent::MCPPromptDefinition& prompt) {
        return prompt.name == prompt_name;
      }));
      auto rendered = concurrent_mcp_server.get_prompt(prompt_name,
                                                       agent::Value::object({{"name", "worker-" + suffix}}));
      SMOKE_CHECK(agent::extract_text_content(rendered.front().content) == prompt_name + ":worker-" + suffix);
    });
  }
  while (concurrent_mcp_ready.load() < concurrent_mcp_thread_count) {
    std::this_thread::yield();
  }
  concurrent_mcp_start.store(true);
  for (auto& worker : concurrent_mcp_workers) {
    worker.join();
  }
  SMOKE_CHECK(concurrent_mcp_prompt_calls.load() == concurrent_mcp_thread_count);

  const auto parsed_mcp_config = agent::parse_anthropic_mcp_config(
      "{\"mcpServers\":{\"local\":{\"command\":\"${HOME}/bin/server\",\"args\":[\"--token\",\"${TOKEN:-fallback}\"],\"env\":{\"API_KEY\":\"${API_KEY:-dev}\"}}}}",
      {{"HOME", "/tmp/home"}});
  SMOKE_CHECK(parsed_mcp_config.mcp_servers.at("local").command == "/tmp/home/bin/server");
  SMOKE_CHECK((parsed_mcp_config.mcp_servers.at("local").args == std::vector<std::string>{"--token", "fallback"}));
  SMOKE_CHECK(parsed_mcp_config.mcp_servers.at("local").env.at("API_KEY") == "dev");
  const auto parsed_http_mcp_config = agent::parse_anthropic_mcp_config(
      "{\"mcpServers\":{\"remote\":{\"url\":\"${MCP_HTTP_URL}\",\"headers\":{\"authorization\":\"Bearer ${TOKEN}\"}}}}",
      {{"MCP_HTTP_URL", "http://127.0.0.1:9999/mcp"}, {"TOKEN", "dev-token"}});
  SMOKE_CHECK(parsed_http_mcp_config.mcp_servers.at("remote").type == "http");
  SMOKE_CHECK(parsed_http_mcp_config.mcp_servers.at("remote").url == "http://127.0.0.1:9999/mcp");
  SMOKE_CHECK(parsed_http_mcp_config.mcp_servers.at("remote").headers.at("authorization") == "Bearer dev-token");
  const auto mcp_config_dir = tmp_dir / "mcp-config";
  std::filesystem::create_directories(mcp_config_dir / "nested");
  const auto mcp_config_path = mcp_config_dir / ".mcp.json";
  agent::write_json_file(mcp_config_path, agent::Value::object({
      {"mcpServers", agent::Value::object({
          {"repo", agent::Value::object({{"command", "./server"}, {"args", agent::Value::array({"--stdio"})}})},
      })},
  }));
  auto found_mcp_config = agent::find_anthropic_mcp_config_file(mcp_config_dir / "nested");
  SMOKE_CHECK(found_mcp_config && *found_mcp_config == mcp_config_path);
  auto loaded_mcp_config = agent::load_anthropic_mcp_config_file(mcp_config_path);
  SMOKE_CHECK(loaded_mcp_config.mcp_servers.at("repo").command.find("server") != std::string::npos);
  SMOKE_CHECK(std::filesystem::path(loaded_mcp_config.mcp_servers.at("repo").command).is_absolute());

  auto numeric_rpc_request = agent::json_rpc_request_from_value(agent::Value::object({
      {"jsonrpc", "2.0"},
      {"id", 42},
      {"method", "ping"},
      {"params", agent::Value::object({{"ok", true}})},
  }));
  SMOKE_CHECK(numeric_rpc_request.id == "42");
  auto numeric_rpc_response = agent::json_rpc_response_from_value(agent::Value::object({
      {"jsonrpc", "2.0"},
      {"id", 43},
      {"result", agent::Value::object({{"ok", true}})},
  }));
  SMOKE_CHECK(numeric_rpc_response.id == "43");
  auto numeric_rpc_error = agent::json_rpc_response_from_value(agent::Value::object({
      {"jsonrpc", "2.0"},
      {"id", 44},
      {"error", agent::Value::object({{"code", -32000}, {"message", "numeric-id-error"}})},
  }));
  SMOKE_CHECK(numeric_rpc_error.id == "44");
  SMOKE_CHECK(numeric_rpc_error.error_code == -32000);

  auto transport = std::make_shared<agent::MCPCallbackTransport>([](const agent::Value& message) {
    auto numeric_success = [](const agent::Value& request, agent::Value result) {
      const auto id = static_cast<int>(std::stoll(request.at("id").as_string("0")));
      return agent::Value::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    auto numeric_error = [](const agent::Value& request, int code, const std::string& text) {
      const auto id = static_cast<int>(std::stoll(request.at("id").as_string("0")));
      return agent::Value::object({
          {"jsonrpc", "2.0"},
          {"id", id},
          {"error", agent::Value::object({{"code", code}, {"message", text}})},
      });
    };
    const auto method = message.at("method").as_string();
    if (method == "initialize") {
      return numeric_success(message, agent::Value::object({
          {"protocolVersion", agent::MCP_PROTOCOL_VERSION},
          {"capabilities", agent::Value::object({{"tools", agent::Value::object({{"listChanged", false}})},
                                                 {"resources", agent::Value::object({{"listChanged", false}})},
                                                 {"prompts", agent::Value::object({{"listChanged", false}})}})},
          {"serverInfo", agent::Value::object({{"name", "fixture-server"}, {"version", "1.0.0"}})},
      }));
    }
    if (method == "notifications/initialized") {
      return agent::Value();
    }
    if (method == "tools/list") {
      return numeric_success(message, agent::Value::object({
          {"tools", agent::Value::array({
              agent::Value::object({
                  {"name", "echo"},
                  {"description", "Echo input"},
                  {"inputSchema", agent::Value::object({
                      {"type", "object"},
                      {"properties", agent::Value::object({
                          {"text", agent::Value::object({{"type", "string"}})},
                      })},
                  })},
              }),
              agent::Value::object({
                  {"name", "fail"},
                  {"description", "Always fails"},
              }),
          })},
      }));
    }
    if (method == "tools/call") {
      if (message.at("params").at("name").as_string() == "fail") {
        return numeric_success(message, agent::Value::object({
            {"content", agent::Value::array({
                agent::Value::object({{"type", "image"},
                                      {"data", "aW1hZ2U="},
                                      {"mimeType", "image/png"}}),
            })},
            {"isError", true},
            {"_meta", agent::Value::object({{"reason", "denied"}})},
        }));
      }
      return numeric_success(message, agent::Value::object({
          {"content", agent::Value::array({
              agent::Value::object({{"type", "text"},
                                    {"text", "echo:" + message.at("params").at("arguments").at("text").as_string()}}),
          })},
          {"structuredContent", agent::Value::object({{"ok", true}})},
      }));
    }
    if (method == "resources/list") {
      return numeric_success(message, agent::Value::object({
          {"resources", agent::Value::array({
              agent::Value::object({{"uri", "resource://one"},
                                    {"name", "One"},
                                    {"mimeType", "text/plain"}}),
          })},
      }));
    }
    if (method == "resources/read") {
      return numeric_success(message, agent::Value::object({
          {"contents", agent::Value::array({
              agent::Value::object({{"uri", message.at("params").at("uri").as_string() + "#text"},
                                    {"mimeType", "text/plain"},
                                    {"text", "resource-body"}}),
              agent::Value::object({{"uri", message.at("params").at("uri").as_string() + "/two.bin"},
                                    {"mimeType", "application/octet-stream"},
                                    {"blob", "Ymlu"}}),
          })},
      }));
    }
    if (method == "prompts/list") {
      return numeric_success(message, agent::Value::object({
          {"prompts", agent::Value::array({
              agent::Value::object({
                  {"name", "brief"},
                  {"description", "Brief prompt"},
                  {"arguments", agent::Value::array({
                      agent::Value::object({{"name", "topic"},
                                            {"description", "Topic to brief"},
                                            {"required", true}}),
                      agent::Value::object({{"name", "tone"},
                                            {"description", "Tone hint"},
                                            {"required", false}}),
                  })},
              }),
          })},
      }));
    }
    if (method == "prompts/get") {
      return numeric_success(message, agent::Value::object({
          {"messages", agent::Value::array({
              agent::Value::object({{"role", "user"},
                                    {"content", agent::Value::object({{"type", "text"},
                                                                      {"text", "prompt-body"}})}}),
          })},
      }));
    }
    return numeric_error(message, -32601, "Method not found");
  });
  agent::MCPClient remote_mcp("fixture", transport);
  auto remote_init = remote_mcp.connect();
  SMOKE_CHECK(remote_init.at("serverInfo").at("name").as_string() == "fixture-server");
  auto remote_tools = agent::create_mcp_tool_definitions(remote_mcp);
  SMOKE_CHECK(remote_tools.size() == 2);
  auto remote_echo_tool = std::find_if(remote_tools.begin(), remote_tools.end(), [](const agent::ToolDefinition& tool) {
    return tool.name == "fixture.echo";
  });
  auto remote_fail_tool = std::find_if(remote_tools.begin(), remote_tools.end(), [](const agent::ToolDefinition& tool) {
    return tool.name == "fixture.fail";
  });
  SMOKE_CHECK(remote_echo_tool != remote_tools.end());
  SMOKE_CHECK(remote_fail_tool != remote_tools.end());
  SMOKE_CHECK(remote_fail_tool->input_schema.type == agent::JsonSchemaType::Object);
  auto remote_tool_summary = agent::render_mcp_tool_summary(remote_tools);
  SMOKE_CHECK(remote_tool_summary.find("- fixture.echo: Echo input") != std::string::npos);
  SMOKE_CHECK(remote_tool_summary.find("- fixture.fail: Always fails") != std::string::npos);
  SMOKE_CHECK(agent::render_mcp_structured_value(agent::Value("plain")) == "plain");
  SMOKE_CHECK(agent::render_mcp_structured_value(agent::Value::object({{"ok", true}})).find("\"ok\": true") !=
         std::string::npos);
  auto remote_prompt_definitions = remote_mcp.list_prompts();
  SMOKE_CHECK(remote_prompt_definitions.size() == 1);
  SMOKE_CHECK((remote_prompt_definitions.front().arguments == std::vector<std::string>{"topic", "tone"}));
  SMOKE_CHECK(remote_prompt_definitions.front().argument_definitions.size() == 2);
  SMOKE_CHECK(remote_prompt_definitions.front().argument_definitions.front().description == "Topic to brief");
  SMOKE_CHECK(remote_prompt_definitions.front().argument_definitions.front().required);
  SMOKE_CHECK(!remote_prompt_definitions.front().argument_definitions.back().required);
  agent::ToolRegistry remote_tool_registry(remote_tools);
  agent::ToolExecutor remote_tool_executor(remote_tool_registry);
  auto remote_tool_result = remote_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "remote_mcp_1",
      .name = "fixture.echo",
      .arguments = agent::Value::object({{"text", "hello"}}),
  });
  SMOKE_CHECK(remote_tool_result.ok);
  SMOKE_CHECK(agent::extract_text_content(remote_tool_result.message.content) == "echo:hello");
  SMOKE_CHECK(remote_tool_result.output.find("ok") != std::string::npos);
  agent::ToolExecutionContext remote_fail_context;
  bool saw_mcp_error = false;
  try {
    (void)remote_fail_tool->invoke(agent::Value::object({}), remote_fail_context);
  } catch (const agent::AgentFrameworkError& error) {
    saw_mcp_error = true;
    SMOKE_CHECK(std::string(error.what()) == "image");
    SMOKE_CHECK(error.details().find("denied") != std::string::npos);
  }
  SMOKE_CHECK(saw_mcp_error);
  auto remote_resource = remote_mcp.read_resource("resource://one");
  SMOKE_CHECK(remote_resource.contents.size() == 2);
  SMOKE_CHECK(remote_resource.contents.front().uri == "resource://one#text");
  SMOKE_CHECK(remote_resource.contents.front().text == "resource-body");
  SMOKE_CHECK(remote_resource.contents.back().uri == "resource://one/two.bin");
  SMOKE_CHECK(remote_resource.contents.back().blob.size() == 3);
  SMOKE_CHECK(remote_resource.text == "resource-body");
  SMOKE_CHECK(agent::read_mcp_resource_text(remote_mcp, "resource://one") ==
         "resource-body\n\n[binary:application/octet-stream]");
  auto remote_resource_message = agent::create_mcp_resource_message(remote_mcp, "resource://one");
  SMOKE_CHECK(remote_resource_message.content.size() == 2);
  SMOKE_CHECK(remote_resource_message.content.front().metadata.at("uri").as_string() == "resource://one#text");
  SMOKE_CHECK(remote_resource_message.content.back().type == agent::ContentPartType::File);
  SMOKE_CHECK(remote_resource_message.content.back().source.filename == "two.bin");
  SMOKE_CHECK(agent::extract_text_content(remote_resource_message.content) == "resource-body");
  auto remote_prompt_messages = agent::create_mcp_prompt_messages(remote_mcp, "brief");
  SMOKE_CHECK(remote_prompt_messages.size() == 1);
  SMOKE_CHECK(agent::extract_text_content(remote_prompt_messages.front().content) == "prompt-body");
  remote_mcp.close();

  class AsyncMCPTransport final : public agent::MCPTransport {
   public:
    ~AsyncMCPTransport() override {
      close();
    }

    void start(agent::MCPMessageHandler handler) override {
      std::lock_guard<std::mutex> lock(mutex_);
      handler_ = std::move(handler);
      started_ = true;
    }

    void send(const agent::Value& message) override {
      agent::MCPMessageHandler handler;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        SMOKE_CHECK(started_);
        handler = handler_;
        sent_methods.push_back(message.at("method").as_string());
        threads_.emplace_back([handler, message]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          if (!handler) {
            return;
          }
          const auto method = message.at("method").as_string();
          if (method == "notifications/initialized") {
            return;
          }
          auto success = [&](agent::Value result) {
            handler(agent::Value::object({
                {"jsonrpc", "2.0"},
                {"id", message.at("id")},
                {"result", std::move(result)},
            }));
          };
          if (method == "initialize") {
            success(agent::Value::object({
                {"protocolVersion", agent::MCP_PROTOCOL_VERSION},
                {"capabilities", agent::Value::object({{"tools", agent::Value::object({})}})},
                {"serverInfo", agent::Value::object({{"name", "async-server"}, {"version", "1.0.0"}})},
            }));
            return;
          }
          if (method == "async/echo") {
            success(agent::Value::object({
                {"index", message.at("params").at("index")},
                {"ok", true},
            }));
            return;
          }
          handler(agent::Value::object({
              {"jsonrpc", "2.0"},
              {"id", message.at("id")},
              {"error", agent::Value::object({{"code", -32601}, {"message", "Method not found"}})},
          }));
        });
      }
    }

    void close() override {
      std::vector<std::thread> threads;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        handler_ = {};
        threads.swap(threads_);
      }
      for (auto& thread : threads) {
        if (thread.joinable()) {
          thread.join();
        }
      }
    }

    std::vector<std::string> sent_methods;

   private:
    std::mutex mutex_;
    agent::MCPMessageHandler handler_;
    bool started_ = false;
    std::vector<std::thread> threads_;
  };

  auto async_transport = std::make_shared<AsyncMCPTransport>();
  agent::MCPClient async_mcp("async-fixture",
                             async_transport,
                             "native-async-client",
                             "0.1.0",
                             agent::Value::object({}),
                             1000);
  auto async_init = async_mcp.connect();
  SMOKE_CHECK(async_init.at("serverInfo").at("name").as_string() == "async-server");
  std::atomic<int> async_mcp_responses{0};
  std::vector<std::thread> async_mcp_threads;
  for (int index = 0; index < 8; ++index) {
    async_mcp_threads.emplace_back([&, index]() {
      auto result = async_mcp.request("async/echo", agent::Value::object({{"index", index}}));
      SMOKE_CHECK(result.at("index").as_integer() == index);
      SMOKE_CHECK(result.at("ok").as_bool());
      async_mcp_responses += 1;
    });
  }
  for (auto& thread : async_mcp_threads) {
    thread.join();
  }
  SMOKE_CHECK(async_mcp_responses == 8);
  async_mcp.close();

  std::istringstream mcp_line_input("{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"result\":{\"ok\":true}}\n");
  std::ostringstream mcp_line_output;
  agent::MCPLineTransport line_transport(mcp_line_input, mcp_line_output);
  bool line_received = false;
  line_transport.start([&](const agent::Value& message) {
    line_received = message.at("result").at("ok").as_bool();
  });
  line_transport.send(agent::json_rpc_request_to_value(agent::JsonRpcRequest{"1", "ping", agent::Value::object({})}));
  line_transport.poll();
  SMOKE_CHECK(line_received);
  SMOKE_CHECK(mcp_line_output.str().find("\"method\":\"ping\"") != std::string::npos);

  agent::WorkflowBuilder builder("wf_smoke", "Workflow Smoke");
  auto workflow = builder.start()
                      .tool("eval", "math.eval",
                            {{"expression", agent::workflow_literal("2+3")}})
                      .end("end", agent::workflow_ref("nodes.eval.output"))
                      .sequence({"start", "eval", "end"})
                      .build();
  agent::WorkflowEngine engine(&registry, &executor);
  auto workflow_result = engine.run(workflow);
  SMOKE_CHECK(workflow_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(workflow_result.output.as_string().find("5") != std::string::npos);
  auto workflow_roundtrip = agent::workflow_definition_from_value(agent::workflow_definition_to_value(workflow));
  SMOKE_CHECK(workflow_roundtrip.nodes.size() == workflow.nodes.size());
  agent::FileWorkflowStore workflow_store(agent::FileWorkflowStoreConfig{
      .base_dir = persistent_dir / "workflow-runs",
  });
  agent::WorkflowEngine stored_engine(&registry, &executor, &workflow_store);
  auto stored_workflow_result = stored_engine.run(workflow);
  auto stored_workflow = workflow_store.get_run(stored_workflow_result.workflow_run_id);
  SMOKE_CHECK(stored_workflow);
  SMOKE_CHECK(stored_workflow->status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(stored_workflow->node_states.at("eval").visits == 1);
  auto restored_workflow_state =
      agent::workflow_run_state_from_value(agent::workflow_run_state_to_value(*stored_workflow));
  SMOKE_CHECK(restored_workflow_state.workflow_run_id == stored_workflow->workflow_run_id);
  SMOKE_CHECK(!restored_workflow_state.checkpoints.empty());
  SMOKE_CHECK(std::filesystem::exists(workflow_store.file_path(stored_workflow_result.workflow_run_id)));
  auto manual_workflow_state = agent::initialize_workflow_run_state(
      workflow,
      "manual:run/with space",
      agent::Value::object({{"manual", true}}));
  workflow_store.save_run(manual_workflow_state);
  SMOKE_CHECK(std::filesystem::exists((persistent_dir / "workflow-runs" / "manual%3Arun%2Fwith%20space.json")));
  auto loaded_manual_workflow = workflow_store.get_run("manual:run/with space");
  SMOKE_CHECK(loaded_manual_workflow);
  SMOKE_CHECK(loaded_manual_workflow->input.at("manual").as_bool());

  agent::InMemoryWorkflowStore object_workflow_store;
  agent::WorkflowEngine object_workflow_engine(agent::WorkflowEngineConfig{
      .store = &object_workflow_store,
  });
  auto object_human_workflow = agent::create_workflow("wf_object_human")
                                   .start()
                                   .human_wait("approve", agent::WorkflowHumanWaitOptions{
                                                              .prompt = "Approve object input",
                                                          })
                                   .end("end", agent::workflow_ref("nodes.approve.output.value"))
                                   .sequence({"start", "approve", "end"})
                                   .build();
  auto object_human_waiting = object_workflow_engine.run(agent::WorkflowRunInput{
      .definition = object_human_workflow,
      .input = agent::Value::object({{"ticket", "T-42"}}),
      .context = agent::Value::object({{"source", "object-style"}}),
  });
  SMOKE_CHECK(object_human_waiting.status == agent::WorkflowStatus::Waiting);
  auto object_human_resumed = object_workflow_engine.resume(agent::WorkflowResumeInput{
      .workflow_run_id = object_human_waiting.workflow_run_id,
      .definition = object_human_workflow,
  });
  SMOKE_CHECK(object_human_resumed.status == agent::WorkflowStatus::Waiting);
  SMOKE_CHECK(object_human_resumed.state.context.at("source").as_string() == "object-style");
  auto object_human_completed = object_workflow_engine.submit_human_response(agent::WorkflowHumanResponseInput{
      .workflow_run_id = object_human_waiting.workflow_run_id,
      .node_id = "approve",
      .payload = "approved via object",
  });
  SMOKE_CHECK(object_human_completed.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(object_human_completed.output.as_string() == "approved via object");

  auto object_webhook_workflow = agent::create_workflow("wf_object_webhook")
                                     .start()
                                     .webhook_wait("hook", agent::WorkflowWebhookWaitOptions{
                                                               .prompt = "Wait for webhook",
                                                           })
                                     .end("end", agent::workflow_ref("nodes.hook.output.value.ok"))
                                     .sequence({"start", "hook", "end"})
                                     .build();
  auto object_webhook_waiting = object_workflow_engine.run(agent::WorkflowRunInput{
      .definition = object_webhook_workflow,
      .input = agent::Value::object({{"kind", "webhook"}}),
  });
  SMOKE_CHECK(object_webhook_waiting.status == agent::WorkflowStatus::Waiting);
  auto object_webhook_completed = object_workflow_engine.submit_webhook_payload(agent::WorkflowWebhookPayloadInput{
      .workflow_run_id = object_webhook_waiting.workflow_run_id,
      .node_id = "hook",
      .payload = agent::Value::object({{"ok", true}}),
  });
  SMOKE_CHECK(object_webhook_completed.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(object_webhook_completed.output.as_bool());

  std::vector<agent::FrameworkEvent> workflow_events;
  agent::EventBus workflow_event_bus;
  workflow_event_bus.register_sink([&](const agent::FrameworkEvent& event) {
    workflow_events.push_back(event);
  });
  int flaky_workflow_attempts = 0;
  agent::WorkflowNodeRegistry retry_node_registry({
      agent::WorkflowNodeHandler{
          .type = "flaky",
          .execute = [&](const agent::WorkflowNode&, agent::WorkflowNodeHandlerContext&) -> agent::WorkflowNodeHandlerResult {
            flaky_workflow_attempts += 1;
            if (flaky_workflow_attempts == 1) {
              throw agent::AgentFrameworkError("retry workflow node");
            }
            return agent::Value::object({{"ok", true}});
          },
      },
  });
  agent::ExecutionPolicies workflow_execution_policies;
  workflow_execution_policies.retry[agent::ExecutionTarget::WorkflowNode] = agent::RetryPolicy{
      .max_attempts = 2,
      .strategy = agent::RetryStrategy::None,
      .retry_on = [](const agent::RetryContext&) { return true; },
  };
  auto retry_workflow = agent::WorkflowBuilder("wf_retry_events")
                            .start()
                            .add_node(agent::create_node(agent::WorkflowNode{.id = "flaky", .type = "flaky"}))
                            .end()
                            .sequence({"start", "flaky", "end"})
                            .build();
  agent::WorkflowEngine retry_event_engine(agent::WorkflowEngineConfig{
      .node_registry = &retry_node_registry,
      .event_bus = &workflow_event_bus,
      .execution_policies = workflow_execution_policies,
  });
  auto retry_event_result = retry_event_engine.run(retry_workflow);
  SMOKE_CHECK(retry_event_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(flaky_workflow_attempts == 2);
  auto workflow_event_seen = [&](const std::string& category) {
    return std::any_of(workflow_events.begin(), workflow_events.end(), [&](const auto& event) {
      return event.category == category;
    });
  };
  SMOKE_CHECK(workflow_event_seen("workflow.started"));
  SMOKE_CHECK(workflow_event_seen("retry.scheduled"));
  SMOKE_CHECK(workflow_event_seen("workflow.node.completed"));
  SMOKE_CHECK(workflow_event_seen("workflow.completed"));
  const auto retry_event = std::find_if(workflow_events.begin(), workflow_events.end(), [](const auto& event) {
    return event.category == "retry.scheduled";
  });
  SMOKE_CHECK(retry_event != workflow_events.end());
  SMOKE_CHECK(retry_event->payload.at("nodeId").as_string() == "flaky");
  SMOKE_CHECK(retry_event->trace.workflow_run_id == retry_event_result.workflow_run_id);

  agent::InMemoryWorkflowStore concurrent_workflow_store;
  std::vector<std::string> concurrent_workflow_run_ids;
  std::mutex concurrent_workflow_ids_mutex;
  std::vector<std::thread> concurrent_workflow_threads;
  for (int thread_index = 0; thread_index < 8; ++thread_index) {
    concurrent_workflow_threads.emplace_back([&, thread_index]() {
      for (int run_index = 0; run_index < 10; ++run_index) {
        auto state = concurrent_workflow_store.create_run(
            workflow,
            agent::Value::object({{"thread", thread_index}, {"run", run_index}}));
        state.context["savedBy"] = thread_index;
        concurrent_workflow_store.save_run(state);
        auto loaded = concurrent_workflow_store.get_run(state.workflow_run_id);
        SMOKE_CHECK(loaded);
        SMOKE_CHECK(loaded->context.at("savedBy").as_integer() == thread_index);
        std::lock_guard<std::mutex> lock(concurrent_workflow_ids_mutex);
        concurrent_workflow_run_ids.push_back(state.workflow_run_id);
      }
    });
  }
  for (auto& thread : concurrent_workflow_threads) {
    thread.join();
  }
  SMOKE_CHECK(concurrent_workflow_run_ids.size() == 80);
  for (const auto& workflow_run_id : concurrent_workflow_run_ids) {
    SMOKE_CHECK(concurrent_workflow_store.get_run(workflow_run_id));
  }

  agent::FileWorkflowStore concurrent_file_workflow_store(persistent_dir / "workflow-runs-concurrent");
  std::vector<std::thread> concurrent_file_workflow_threads;
  for (int thread_index = 0; thread_index < 4; ++thread_index) {
    concurrent_file_workflow_threads.emplace_back([&, thread_index]() {
      for (int run_index = 0; run_index < 5; ++run_index) {
        auto state = concurrent_file_workflow_store.create_run(
            workflow,
            agent::Value::object({{"thread", thread_index}, {"run", run_index}}));
        state.output = agent::Value::object({{"saved", true}, {"thread", thread_index}});
        concurrent_file_workflow_store.save_run(state);
        auto loaded = concurrent_file_workflow_store.get_run(state.workflow_run_id);
        SMOKE_CHECK(loaded);
        SMOKE_CHECK(loaded->output.at("thread").as_integer() == thread_index);
      }
    });
  }
  for (auto& thread : concurrent_file_workflow_threads) {
    thread.join();
  }

  agent::ToolRegistry workflow_tool_registry(agent::create_workflow_builtin_tools(&stored_engine));
  agent::ToolExecutor workflow_tool_executor(workflow_tool_registry);
  auto workflow_get_run = workflow_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "workflow_get_1",
      .name = "workflow.getRun",
      .arguments = agent::Value::object({{"workflowRunId", stored_workflow_result.workflow_run_id}}),
  });
  SMOKE_CHECK(workflow_get_run.ok);
  SMOKE_CHECK(workflow_get_run.output.find(stored_workflow_result.workflow_run_id) != std::string::npos);
  auto workflow_checkpoints = workflow_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "workflow_checkpoints_1",
      .name = "workflow.listCheckpoints",
      .arguments = agent::Value::object({{"workflowRunId", stored_workflow_result.workflow_run_id}}),
  });
  SMOKE_CHECK(workflow_checkpoints.ok);
  SMOKE_CHECK(workflow_checkpoints.output.find("workflow.started") != std::string::npos);
  auto workflow_resume = workflow_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "workflow_resume_1",
      .name = "workflow.resume",
      .arguments = agent::Value::object({{"workflowRunId", stored_workflow_result.workflow_run_id}}),
  });
  SMOKE_CHECK(workflow_resume.ok);
  SMOKE_CHECK(workflow_resume.output.find("workflow.resumed") != std::string::npos);
  agent::ToolRegistry workflow_bundle_registry(agent::create_builtin_tools({"workflow"}));
  SMOKE_CHECK(workflow_bundle_registry.get("workflow.getRun") != nullptr);
  agent::ToolExecutor workflow_bundle_executor(workflow_bundle_registry);
  agent::ToolExecutionContext workflow_service_context;
  workflow_service_context.service_refs.workflow_engine = &stored_engine;
  auto service_workflow_get = workflow_bundle_executor.execute_tool_call(agent::ToolCall{
      .id = "workflow_service_get_1",
      .name = "workflow.getRun",
      .arguments = agent::Value::object({{"workflowRunId", stored_workflow_result.workflow_run_id}}),
  }, workflow_service_context);
  SMOKE_CHECK(service_workflow_get.ok);
  SMOKE_CHECK(service_workflow_get.output.find(stored_workflow_result.workflow_run_id) != std::string::npos);

  bool saw_workflow_tool_context = false;
  auto workflow_context_tool = agent::define_tool(agent::ToolDefinition{
      .name = "workflow.context",
      .description = "Inspect workflow tool context services.",
      .execute = [&](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        saw_workflow_tool_context = true;
        SMOKE_CHECK(context.service_refs.workflow_engine != nullptr);
        SMOKE_CHECK(context.service_refs.workflow_definition != nullptr);
        SMOKE_CHECK(context.service_refs.workflow_definition->id == "wf_context_services");
        SMOKE_CHECK(context.services.at("workflow").at("configured").as_string() == "yes");
        SMOKE_CHECK(context.services.at("workflow").at("workflowId").as_string() == "wf_context_services");
        SMOKE_CHECK(context.services.at("workflow").at("nodeId").as_string() == "inspect");
        SMOKE_CHECK(!context.services.at("workflowRunId").as_string().empty());
        SMOKE_CHECK(context.attributes.at("workflowId").as_string() == "wf_context_services");
        SMOKE_CHECK(context.attributes.at("nodeId").as_string() == "inspect");
        return agent::Value::object({{"workflowId", context.services.at("workflow").at("workflowId")}});
      },
  });
  agent::ToolRegistry workflow_context_registry({workflow_context_tool});
  agent::ToolExecutor workflow_context_executor(workflow_context_registry);
  agent::ToolExecutionServices workflow_configured_services;
  workflow_configured_services.values = agent::Value::object({
      {"workflow", agent::Value::object({{"configured", "yes"}})},
  });
  agent::WorkflowEngine workflow_context_engine(&workflow_context_registry,
                                                &workflow_context_executor,
                                                nullptr,
                                                nullptr,
                                                nullptr,
                                                workflow_configured_services);
  auto workflow_context_definition = agent::WorkflowBuilder("wf_context_services")
                                         .start()
                                         .tool("inspect", "workflow.context")
                                         .end("end", agent::workflow_ref("nodes.inspect.output"))
                                         .sequence({"start", "inspect", "end"})
                                         .build();
  auto workflow_context_result = workflow_context_engine.run(workflow_context_definition);
  SMOKE_CHECK(workflow_context_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(saw_workflow_tool_context);
  SMOKE_CHECK(workflow_context_result.output.as_string().find("wf_context_services") != std::string::npos);

  agent::WorkflowNodeRegistry custom_node_registry;
  custom_node_registry.register_handler(agent::WorkflowNodeHandler{
      .type = "custom.echo",
      .execute = [](const agent::WorkflowNode& node,
                    agent::WorkflowNodeHandlerContext& context) -> agent::WorkflowNodeHandlerResult {
        return agent::Value::object({{"nodeType", node.type}, {"echoed", context.input.at("value")}});
      },
  });
  agent::WorkflowNode custom_node;
  custom_node.id = "custom";
  custom_node.type = "custom.echo";
  custom_node.inputs["value"] = agent::workflow_literal("hello");
  auto custom_workflow = agent::WorkflowBuilder("wf_custom")
                             .start()
                             .add_node(custom_node)
                             .end("end", agent::workflow_ref("nodes.custom.output.echoed"))
                             .sequence({"start", "custom", "end"})
                             .build();
  agent::WorkflowEngine custom_engine(nullptr, nullptr, nullptr, &custom_node_registry);
  auto custom_result = custom_engine.run(custom_workflow);
  SMOKE_CHECK(custom_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(custom_result.output.as_string() == "hello");

  agent::WorkflowEngine concurrent_workflow_registry_engine;
  std::atomic<int> concurrent_workflow_ready{0};
  std::atomic<bool> concurrent_workflow_start{false};
  std::atomic<int> concurrent_workflow_node_calls{0};
  std::atomic<int> concurrent_workflow_agent_calls{0};
  constexpr int concurrent_workflow_registry_thread_count = 8;
  std::vector<std::thread> concurrent_workflow_workers;
  for (int index = 0; index < concurrent_workflow_registry_thread_count; ++index) {
    concurrent_workflow_workers.emplace_back([&, index]() {
      concurrent_workflow_ready.fetch_add(1);
      while (!concurrent_workflow_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(index);
      const auto node_id = "node" + suffix;
      const auto node_type = "custom.concurrent." + suffix;
      const auto agent_node_id = "agent" + suffix;
      const auto agent_id = "agent.concurrent." + suffix;
      concurrent_workflow_registry_engine.register_node(agent::WorkflowNodeHandler{
          .type = node_type,
          .execute = [&concurrent_workflow_node_calls](
                         const agent::WorkflowNode&,
                         agent::WorkflowNodeHandlerContext& context) -> agent::WorkflowNodeHandlerResult {
            concurrent_workflow_node_calls.fetch_add(1);
            return agent::Value::object({{"value", context.input.at("value")}});
          },
      });
      concurrent_workflow_registry_engine.register_agent(agent::WorkflowAgentDefinition{
          .id = agent_id,
          .run = [&concurrent_workflow_agent_calls](const agent::Value& input, const agent::WorkflowRunState&) {
            concurrent_workflow_agent_calls.fetch_add(1);
            return agent::Value::object({{"text", "agent:" + input.at("prompt").as_string()}});
          },
      });
      agent::WorkflowNode concurrent_node;
      concurrent_node.id = node_id;
      concurrent_node.type = node_type;
      concurrent_node.inputs["value"] = agent::workflow_literal("node:" + suffix);
      auto concurrent_workflow = agent::WorkflowBuilder("wf_concurrent_registry_" + suffix)
                                     .start()
                                     .add_node(concurrent_node)
                                     .agent(agent_node_id, agent_id,
                                            {{"prompt", agent::workflow_literal("prompt:" + suffix)}})
                                     .end("end", agent::workflow_ref("nodes." + agent_node_id + ".output.result.text"))
                                     .sequence({"start", node_id, agent_node_id, "end"})
                                     .build();
      auto concurrent_result = concurrent_workflow_registry_engine.run(concurrent_workflow);
      SMOKE_CHECK(concurrent_result.status == agent::WorkflowStatus::Completed);
      SMOKE_CHECK(concurrent_result.state.node_states.at(node_id).output.at("value").as_string() == "node:" + suffix);
      SMOKE_CHECK(concurrent_result.output.as_string() == "agent:prompt:" + suffix);
    });
  }
  while (concurrent_workflow_ready.load() < concurrent_workflow_registry_thread_count) {
    std::this_thread::yield();
  }
  concurrent_workflow_start.store(true);
  for (auto& worker : concurrent_workflow_workers) {
    worker.join();
  }
  SMOKE_CHECK(concurrent_workflow_node_calls.load() == concurrent_workflow_registry_thread_count);
  SMOKE_CHECK(concurrent_workflow_agent_calls.load() == concurrent_workflow_registry_thread_count);

  bool saw_direct_human = false;
  agent::WorkflowEngine direct_human_engine;
  direct_human_engine.register_node(agent::create_human_workflow_node_handler(
      [&](const agent::HumanWorkflowNodeProviderParams& params) {
        saw_direct_human = true;
        SMOKE_CHECK(params.node.type == "human");
        SMOKE_CHECK(params.node.prompt == "Approve request");
        SMOKE_CHECK(params.node.message == "Review before release");
        SMOKE_CHECK(params.input.at("requestId").as_string() == "req-42");
        SMOKE_CHECK(params.state.workflow_id == "wf_direct_human");
        return agent::Value::object({{"approved", true}, {"message", params.node.message}});
      }));
  agent::WorkflowNode direct_human_node;
  direct_human_node.id = "approve";
  direct_human_node.type = "human";
  direct_human_node.prompt = "Approve request";
  direct_human_node.message = "Review before release";
  direct_human_node.inputs["requestId"] = agent::workflow_ref("input.requestId");
  auto direct_human_roundtrip = agent::workflow_node_from_value(agent::workflow_node_to_value(direct_human_node));
  SMOKE_CHECK(direct_human_roundtrip.message == "Review before release");
  auto direct_human_workflow = agent::WorkflowBuilder("wf_direct_human")
                                   .start()
                                   .add_node(direct_human_node)
                                   .end("end", agent::workflow_ref("nodes.approve.output.approved"))
                                   .sequence({"start", "approve", "end"})
                                   .build();
  auto direct_human_result = direct_human_engine.run(
      direct_human_workflow,
      agent::Value::object({{"requestId", "req-42"}}));
  SMOKE_CHECK(direct_human_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(direct_human_result.output.as_bool());
  SMOKE_CHECK(saw_direct_human);

  std::vector<agent::HttpRequest> direct_webhook_requests;
  agent::WorkflowEngine direct_webhook_engine;
  direct_webhook_engine.register_node(agent::create_webhook_workflow_node_handler(
      [&](const agent::HttpRequest& request) {
        direct_webhook_requests.push_back(request);
        return agent::HttpResponse{.status = 202, .body = "accepted"};
      }));
  agent::WorkflowNode direct_webhook_node;
  direct_webhook_node.id = "notify";
  direct_webhook_node.type = "webhook";
  direct_webhook_node.url = "http://example.test/hook";
  direct_webhook_node.method = "POST";
  direct_webhook_node.inputs["payload"] = agent::workflow_literal("ready");
  auto direct_webhook_roundtrip =
      agent::workflow_node_from_value(agent::workflow_node_to_value(direct_webhook_node));
  SMOKE_CHECK(direct_webhook_roundtrip.url == "http://example.test/hook");
  SMOKE_CHECK(direct_webhook_roundtrip.method == "POST");
  auto direct_webhook_workflow = agent::WorkflowBuilder("wf_direct_webhook")
                                     .start()
                                     .add_node(direct_webhook_node)
                                     .end("end", agent::workflow_ref("nodes.notify.output.status"))
                                     .sequence({"start", "notify", "end"})
                                     .build();
  auto direct_webhook_result = direct_webhook_engine.run(direct_webhook_workflow);
  SMOKE_CHECK(direct_webhook_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(direct_webhook_result.output.as_integer() == 202);
  SMOKE_CHECK(direct_webhook_requests.size() == 1);
  SMOKE_CHECK(direct_webhook_requests.front().url == "http://example.test/hook");
  SMOKE_CHECK(direct_webhook_requests.front().method == "POST");
  SMOKE_CHECK(direct_webhook_requests.front().headers.at("content-type") == "application/json");
  SMOKE_CHECK(agent::parse_json(direct_webhook_requests.front().body).at("payload").as_string() == "ready");

  std::vector<agent::HttpRequest> direct_webhook_get_requests;
  agent::WorkflowEngine direct_webhook_get_engine;
  direct_webhook_get_engine.register_node(agent::create_webhook_workflow_node_handler(
      [&](const agent::HttpRequest& request) {
        direct_webhook_get_requests.push_back(request);
        return agent::HttpResponse{.status = 204, .body = ""};
      }));
  agent::WorkflowNode direct_webhook_get_node;
  direct_webhook_get_node.id = "notify-get";
  direct_webhook_get_node.type = "webhook";
  direct_webhook_get_node.method = "GET";
  direct_webhook_get_node.inputs["url"] = agent::workflow_literal("http://example.test/from-input");
  direct_webhook_get_node.inputs["payload"] = agent::workflow_literal("ignored");
  auto direct_webhook_get_workflow = agent::WorkflowBuilder("wf_direct_webhook_get")
                                         .start()
                                         .add_node(direct_webhook_get_node)
                                         .end("end", agent::workflow_ref("nodes.notify-get.output.status"))
                                         .sequence({"start", "notify-get", "end"})
                                         .build();
  auto direct_webhook_get_result = direct_webhook_get_engine.run(direct_webhook_get_workflow);
  SMOKE_CHECK(direct_webhook_get_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(direct_webhook_get_result.output.as_integer() == 204);
  SMOKE_CHECK(direct_webhook_get_requests.size() == 1);
  SMOKE_CHECK(direct_webhook_get_requests.front().url == "http://example.test/from-input");
  SMOKE_CHECK(direct_webhook_get_requests.front().method == "GET");
  SMOKE_CHECK(direct_webhook_get_requests.front().body.empty());

  agent::WorkflowEngine direct_webhook_missing_url_engine;
  direct_webhook_missing_url_engine.register_node(agent::create_webhook_workflow_node_handler(
      [](const agent::HttpRequest&) {
        SMOKE_CHECK(false);
        return agent::HttpResponse{};
      }));
  agent::WorkflowNode missing_url_node;
  missing_url_node.id = "missing-url";
  missing_url_node.type = "webhook";
  auto missing_url_workflow = agent::WorkflowBuilder("wf_direct_webhook_missing_url")
                                  .start()
                                  .add_node(missing_url_node)
                                  .end()
                                  .sequence({"start", "missing-url", "end"})
                                  .build();
  auto missing_url_result = direct_webhook_missing_url_engine.run(missing_url_workflow);
  SMOKE_CHECK(missing_url_result.status == agent::WorkflowStatus::Failed);
  SMOKE_CHECK(missing_url_result.output.at("error").as_string().find("requires a url") != std::string::npos);
  SMOKE_CHECK(missing_url_result.state.node_states.at("missing-url").error.find("requires a url") != std::string::npos);

  agent::WorkflowEngine agent_node_engine;
  agent_node_engine.register_agent(agent::WorkflowAgentDefinition{
      .id = "writer",
      .run = [](const agent::Value& input, const agent::WorkflowRunState&) {
        return agent::Value::object({{"text", "child: " + input.at("prompt").as_string()}});
      },
  });
  auto agent_workflow = agent::WorkflowBuilder("wf_agent")
                            .start()
                            .agent("writer-node", "writer",
                                   {{"prompt", agent::workflow_literal("draft")}})
                            .end("end", agent::workflow_ref("nodes.writer-node.output.result.text"))
                            .sequence({"start", "writer-node", "end"})
                            .build();
  auto agent_result = agent_node_engine.run(agent_workflow);
  SMOKE_CHECK(agent_result.output.as_string() == "child: draft");
  SMOKE_CHECK(std::any_of(agent_result.state.checkpoints.begin(), agent_result.state.checkpoints.end(),
                     [](const auto& checkpoint) {
                       return checkpoint.type == "workflow.child-agent.completed";
                     }));

  bool saw_runner_child_context = false;
  auto child_context_tool = agent::define_tool(agent::ToolDefinition{
      .name = "child.context",
      .description = "Inspect workflow child runner context.",
      .execute = [&](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        saw_runner_child_context = true;
        SMOKE_CHECK(context.service_refs.session != nullptr);
        SMOKE_CHECK(context.services.at("context").at("workflowId").as_string() == "wf_runner_agent");
        SMOKE_CHECK(!context.services.at("context").at("workflowRunId").as_string().empty());
        SMOKE_CHECK(context.services.at("context").at("nodeId").as_string() == "runner-node");
        SMOKE_CHECK(context.services.at("context").at("agentId").as_string() == "runner-writer");
        SMOKE_CHECK(context.services.at("context").at("tenant").as_string() == "tenant-runner");
        return agent::Value::object({{"ok", true}});
      },
  });
  auto workflow_child_runner = std::make_shared<agent::AgentRunner>(agent::AgentRunnerConfig{
      .adapter = std::make_shared<WorkflowChildAgentModel>(),
      .tools = {child_context_tool},
      .max_iterations = 3,
  });
  std::vector<std::string> workflow_hook_calls;
  agent::HookSet workflow_hooks;
  workflow_hooks.before_workflow = [&](const agent::WorkflowHookContext& hook_context) {
    SMOKE_CHECK(hook_context.workflow_id == "wf_runner_agent");
    workflow_hook_calls.push_back("before-workflow");
  };
  workflow_hooks.before_workflow_node = [&](const agent::WorkflowNodeHookContext& hook_context) {
    if (hook_context.node_id == "runner-node") {
      SMOKE_CHECK(hook_context.node_type == "agent");
      workflow_hook_calls.push_back("before-agent-node");
    }
  };
  workflow_hooks.before_child_agent = [&](const agent::ChildAgentHookContext& hook_context) {
    SMOKE_CHECK(hook_context.agent_id == "runner-writer");
    SMOKE_CHECK(hook_context.input.at("prompt").as_string() == "runner draft");
    workflow_hook_calls.push_back("before-child-agent");
  };
  workflow_hooks.after_child_agent = [&](const agent::ChildAgentHookContext& hook_context) {
    SMOKE_CHECK(hook_context.result.at("text").as_string() == "runner child context ok");
    workflow_hook_calls.push_back("after-child-agent");
  };
  workflow_hooks.after_workflow_node = [&](const agent::WorkflowNodeHookContext& hook_context) {
    if (hook_context.node_id == "runner-node") {
      SMOKE_CHECK(hook_context.result.at("result").at("text").as_string() == "runner child context ok");
      workflow_hook_calls.push_back("after-agent-node");
    }
  };
  workflow_hooks.after_workflow = [&](const agent::WorkflowHookContext& hook_context) {
    SMOKE_CHECK(hook_context.workflow_id == "wf_runner_agent");
    SMOKE_CHECK(hook_context.result.at("status").as_string() == "completed");
    workflow_hook_calls.push_back("after-workflow");
  };
  agent::WorkflowEngine runner_agent_engine(nullptr, nullptr, nullptr, nullptr, nullptr, {}, workflow_hooks);
  runner_agent_engine.register_agent_runner("runner-writer", workflow_child_runner);
  auto runner_agent_workflow = agent::WorkflowBuilder("wf_runner_agent")
                                   .start()
                                   .agent("runner-node", "runner-writer",
                                          {{"prompt", agent::workflow_literal("runner draft")}})
                                   .end("end", agent::workflow_ref("nodes.runner-node.output.result.text"))
                                   .sequence({"start", "runner-node", "end"})
                                   .build();
  auto runner_agent_result = runner_agent_engine.run(
      runner_agent_workflow,
      agent::Value::object({}),
      agent::Value::object({{"tenant", "tenant-runner"}}));
  SMOKE_CHECK(runner_agent_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(runner_agent_result.output.as_string() == "runner child context ok");
  SMOKE_CHECK(saw_runner_child_context);
  SMOKE_CHECK(contains_string(workflow_hook_calls, "before-workflow"));
  SMOKE_CHECK(contains_string(workflow_hook_calls, "before-agent-node"));
  SMOKE_CHECK(contains_string(workflow_hook_calls, "before-child-agent"));
  SMOKE_CHECK(contains_string(workflow_hook_calls, "after-child-agent"));
  SMOKE_CHECK(contains_string(workflow_hook_calls, "after-agent-node"));
  SMOKE_CHECK(contains_string(workflow_hook_calls, "after-workflow"));
  SMOKE_CHECK(std::any_of(runner_agent_result.state.checkpoints.begin(), runner_agent_result.state.checkpoints.end(),
                     [](const auto& checkpoint) {
                       return checkpoint.type == "workflow.child-agent.completed";
                     }));

  agent::WorkflowNodeRoute gold_route;
  gold_route.key = "gold";
  gold_route.condition = agent::workflow_equals(agent::workflow_ref("$.input.tier"),
                                                agent::workflow_literal("gold"));
  gold_route.has_condition = true;
  agent::WorkflowNodeRoute fallback_route;
  fallback_route.key = "known";
  fallback_route.condition = agent::workflow_not_empty(agent::workflow_ref("$.input.tier"));
  fallback_route.has_condition = true;
  auto artifact_router_workflow = agent::create_workflow("wf_artifact_router")
                                      .start()
                                      .artifact("set-user", "user", agent::WorkflowArtifactOptions{
                                                                      .mode = "set",
                                                                      .value = agent::workflow_literal(
                                                                          agent::Value::object({{"name", "ada"}})),
                                                                  })
                                      .artifact("merge-user", "user", agent::WorkflowArtifactOptions{
                                                                        .mode = "merge",
                                                                        .value = agent::workflow_literal(
                                                                            agent::Value::object({{"role", "engineer"}})),
                                                                    })
                                      .artifact("get-user", "user", agent::WorkflowArtifactOptions{
                                                                      .mode = "get",
                                                                      .output_key = "user",
                                                                  })
                                      .router("route", agent::WorkflowRouterOptions{
                                                           .routes = {gold_route, fallback_route},
                                                           .default_route = "anonymous",
                                                       })
                                      .end("end", agent::WorkflowEndOptions{
                                                      .result = agent::workflow_ref("nodes.get-user.output.user"),
                                                  })
                                      .connect("start", "set-user")
                                      .connect("set-user", "merge-user")
                                      .connect("merge-user", "get-user")
                                      .parallel("get-user", {"route", "end"})
                                      .build();
  agent::WorkflowEngine artifact_router_engine;
  auto artifact_router_result = artifact_router_engine.run(
      artifact_router_workflow, agent::Value::object({{"tier", "gold"}}));
  SMOKE_CHECK(artifact_router_result.output.at("role").as_string() == "engineer");
  SMOKE_CHECK(artifact_router_result.state.node_states.at("route").output.at("route").as_string() == "gold");
  SMOKE_CHECK(artifact_router_result.state.context.at("artifacts").at("user").at("name").as_string() == "ada");

  agent::InMemoryWorkflowStore human_wait_store;
  agent::WorkflowEngine human_wait_engine(nullptr, nullptr, &human_wait_store);
  auto human_wait_workflow = agent::WorkflowBuilder("wf_human_wait")
                                 .start()
                                 .human_wait("approve", "Continue?")
                                 .end("end", agent::workflow_ref("nodes.approve.output.value"))
                                 .sequence({"start", "approve", "end"})
                                 .build();
  auto waiting_human = human_wait_engine.run(human_wait_workflow);
  SMOKE_CHECK(waiting_human.status == agent::WorkflowStatus::Waiting);
  SMOKE_CHECK(waiting_human.state.node_states.at("approve").waiting);
  SMOKE_CHECK(waiting_human.state.node_states.at("approve").waiting->kind == "human");
  auto resumed_human = human_wait_engine.submit_human_response(
      waiting_human.workflow_run_id, "approve", agent::Value("approved"));
  SMOKE_CHECK(resumed_human.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(resumed_human.output.as_string() == "approved");
  SMOKE_CHECK(std::any_of(resumed_human.state.checkpoints.begin(), resumed_human.state.checkpoints.end(),
                     [](const auto& checkpoint) {
                       return checkpoint.type == "workflow.human-response.received";
                     }));

  agent::InMemoryWorkflowStore webhook_wait_store;
  agent::WorkflowEngine webhook_wait_engine(nullptr, nullptr, &webhook_wait_store);
  auto webhook_wait_workflow = agent::WorkflowBuilder("wf_webhook_wait")
                                   .start()
                                   .webhook_wait("hook", "Await webhook")
                                   .end("end", agent::workflow_ref("nodes.hook.output.value"))
                                   .sequence({"start", "hook", "end"})
                                   .build();
  auto waiting_webhook = webhook_wait_engine.run(webhook_wait_workflow);
  SMOKE_CHECK(waiting_webhook.status == agent::WorkflowStatus::Waiting);
  auto resumed_webhook = webhook_wait_engine.submit_webhook_payload(
      waiting_webhook.workflow_run_id, "hook", agent::Value::object({{"ok", true}}));
  SMOKE_CHECK(resumed_webhook.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(resumed_webhook.output.at("ok").as_bool());
  auto webhook_roundtrip = agent::workflow_run_state_from_value(
      agent::workflow_run_state_to_value(resumed_webhook.state));
  SMOKE_CHECK(webhook_roundtrip.node_states.at("hook").visits == 2);

  std::vector<std::string> retrieval_hook_calls;
  agent::HookSet retrieval_hooks;
  retrieval_hooks.before_knowledge_retrieval = [&](const agent::RetrievalHookContext& hook_context) {
    SMOKE_CHECK(!hook_context.query.empty());
    SMOKE_CHECK(hook_context.source == "long-term-memory");
    retrieval_hook_calls.push_back("before-retrieval");
  };
  retrieval_hooks.after_knowledge_retrieval = [&](const agent::RetrievalHookContext& hook_context) {
    SMOKE_CHECK(!hook_context.query.empty());
    SMOKE_CHECK(hook_context.source == "long-term-memory");
    SMOKE_CHECK(hook_context.result.at("hitCount").as_integer() >= 1);
    retrieval_hook_calls.push_back("after-retrieval");
  };
  agent::AgentRunner runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<ToolCallingModel>(),
      .tools = tools,
      .system_prompt = "You are a test agent.",
      .max_iterations = 4,
      .long_term_memory = memory,
      .retrieval_options = agent::RunnerRetrievalOptions{.enabled = true, .top_k = 2, .min_score = 0.0},
      .writeback_options = agent::RunnerWritebackOptions{.enabled = true},
      .hooks = retrieval_hooks,
  });
  std::vector<std::string> retrieval_events;
  const auto retrieval_sink_id = runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.target == agent::ExecutionTarget::Retrieval) {
      retrieval_events.push_back(event.category);
    }
  });

  auto run = runner.run("evaluate a tool expression");
  SMOKE_CHECK(run.termination_reason == agent::AgentLoopTerminationReason::Completed);
  SMOKE_CHECK(run.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(!run.memory_hits.empty());
  SMOKE_CHECK(contains_string(retrieval_hook_calls, "before-retrieval"));
  SMOKE_CHECK(contains_string(retrieval_hook_calls, "after-retrieval"));
  SMOKE_CHECK(contains_string(retrieval_events, "retrieval.started"));
  SMOKE_CHECK(contains_string(retrieval_events, "retrieval.completed"));
  runner.unregister_event_sink(retrieval_sink_id);

  auto default_retrieval_memory = std::make_shared<agent::LongTermMemory>();
  default_retrieval_memory->remember("Default runner retrieval should find this memory context.");
  agent::AgentRunner default_retrieval_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .long_term_memory = default_retrieval_memory,
  });
  auto default_retrieval_run = default_retrieval_runner.run("find default runner retrieval",
                                                           "default-retrieval-session");
  SMOKE_CHECK(!default_retrieval_run.memory_hits.empty());

  auto default_writeback_memory = std::make_shared<agent::LongTermMemory>();
  agent::AgentRunner default_writeback_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .long_term_memory = default_writeback_memory,
  });
  auto default_writeback_run = default_writeback_runner.run("default writeback memory turn",
                                                           "default-writeback-session");
  SMOKE_CHECK(default_writeback_run.text == "stream text");
  auto default_writeback_hits =
      default_writeback_memory->search("default writeback memory turn stream text", 2, 0.0);
  SMOKE_CHECK(!default_writeback_hits.empty());
  SMOKE_CHECK(default_writeback_hits.front().metadata.at("sessionId").as_string() ==
         "default-writeback-session");
  SMOKE_CHECK(default_writeback_hits.front().metadata.at("type").as_string() ==
         "conversation-turn");

  auto disabled_writeback_memory = std::make_shared<agent::LongTermMemory>();
  agent::AgentRunner disabled_writeback_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .long_term_memory = disabled_writeback_memory,
      .writeback_options = agent::RunnerWritebackOptions{.enabled = false},
  });
  auto disabled_writeback_run = disabled_writeback_runner.run("disabled writeback memory turn",
                                                             "disabled-writeback-session");
  SMOKE_CHECK(disabled_writeback_run.text == "stream text");
  SMOKE_CHECK(disabled_writeback_memory->search("disabled writeback memory turn stream text", 2, 0.0).empty());

  auto forced_writeback_memory = std::make_shared<agent::LongTermMemory>(
      std::make_shared<agent::HashEmbeddingAdapter>(),
      std::make_shared<agent::InMemoryVectorStore>(),
      "default",
      4,
      0.0,
      false);
  agent::AgentRunner forced_writeback_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .long_term_memory = forced_writeback_memory,
  });
  auto forced_writeback_run = forced_writeback_runner.run(
      "forced writeback memory turn",
      "forced-writeback-session",
      agent::ModelSettings{},
      agent::RunnerRetrievalOptions{.enabled = false},
      agent::RunnerWritebackOptions{
          .enabled = true,
          .namespace_id = "forced-writeback",
          .metadata = agent::Value::object({{"source", "per-call"}}),
      });
  SMOKE_CHECK(forced_writeback_run.text == "stream text");
  SMOKE_CHECK(forced_writeback_memory->search("forced writeback memory turn stream text", 2, 0.0).empty());
  auto forced_writeback_hits = forced_writeback_memory->search(
      "forced writeback memory turn stream text", 2, 0.0, "forced-writeback");
  SMOKE_CHECK(!forced_writeback_hits.empty());
  SMOKE_CHECK(forced_writeback_hits.front().metadata.at("source").as_string() == "per-call");

  auto planned_writeback_memory = std::make_shared<agent::LongTermMemory>(
      std::make_shared<agent::HashEmbeddingAdapter>(),
      std::make_shared<agent::InMemoryVectorStore>(),
      "default",
      4,
      0.0);
  auto planned_writeback_planner = std::make_shared<agent::StaticPlanner>(agent::ExecutionPlan{
      .goal = "Remember planned work",
      .steps = {
          agent::PlanStep{.id = "inspect", .title = "Inspect writeback"},
          agent::PlanStep{.id = "reply", .title = "Reply with memory"},
      },
  });
  agent::AgentRunner planned_writeback_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .long_term_memory = planned_writeback_memory,
      .planner = planned_writeback_planner,
  });
  auto planned_writeback_run = planned_writeback_runner.run("planned writeback memory turn",
                                                           "planned-writeback-session");
  SMOKE_CHECK(planned_writeback_run.plan);
  auto planned_writeback_hits = planned_writeback_memory->search(
      "planned writeback memory turn Inspect writeback Reply with memory", 2, 0.0);
  SMOKE_CHECK(!planned_writeback_hits.empty());
  SMOKE_CHECK(planned_writeback_hits.front().content.find(
             "Plan: Inspect writeback -> Reply with memory") != std::string::npos);

  agent::CoordinatorContext coordinator_context;
  coordinator_context.primary_agent = &runner;
  coordinator_context.artifact_store = &artifacts;
  coordinator_context.shared_state = &shared_state;
  coordinator_context.mailbox = &mailbox;
  coordinator_context.state = agent::create_shared_task_state("coordinate");
  agent::PlanActObserveCoordinator coordinator(1);
  auto coordinated = coordinator.run("coordinate with primary", coordinator_context);
  SMOKE_CHECK(!coordinated.text.empty());
  SMOKE_CHECK(artifacts.get("round:1"));

  auto replan_model = std::make_shared<SequentialTextModel>(
      std::vector<std::string>{"draft needs revision", "approved final"});
  agent::AgentRunner replan_runner(agent::AgentRunnerConfig{
      .adapter = replan_model,
      .max_iterations = 1,
  });
  agent::InMemoryArtifactStore replan_artifacts;
  agent::InMemorySharedStateStore replan_state;
  agent::CoordinatorContext replan_context;
  replan_context.primary_agent = &replan_runner;
  replan_context.artifact_store = &replan_artifacts;
  replan_context.shared_state = &replan_state;
  replan_context.state = agent::create_shared_task_state("replan");
  auto replan_strategy = std::make_shared<agent::RuleBasedReplanStrategy>(
      agent::RuleBasedReplanStrategyConfig{
          .required_keywords = {"approved"},
          .rejected_keywords = {"revision"},
          .next_prompt = "Return an approved final answer.",
      });
  agent::PlanActObserveCoordinator replan_coordinator(agent::PlanActObserveCoordinatorOptions{
      .max_rounds = 3,
      .replan_strategy = replan_strategy,
  });
  auto replanned = replan_coordinator.run("coordinate until accepted", replan_context);
  SMOKE_CHECK(replanned.text == "approved final");
  SMOKE_CHECK(replan_model->generate_calls() == 2);
  SMOKE_CHECK(replan_artifacts.get("round:1"));
  SMOKE_CHECK(replan_artifacts.get("round:2"));
  SMOKE_CHECK(replan_state.get("plan-act-observe")->value.at("lastRound").as_integer() == 2);
  SMOKE_CHECK(replan_context.state.output.as_string() == "approved final");

  auto evaluator_model = std::make_shared<SequentialTextModel>(
      std::vector<std::string>{"missing approval", "approved evaluator output"});
  agent::AgentRunner evaluator_runner(agent::AgentRunnerConfig{
      .adapter = evaluator_model,
      .max_iterations = 1,
  });
  agent::InMemoryArtifactStore evaluator_artifacts;
  agent::CoordinatorContext evaluator_context;
  evaluator_context.primary_agent = &evaluator_runner;
  evaluator_context.artifact_store = &evaluator_artifacts;
  evaluator_context.state = agent::create_shared_task_state("evaluate");
  auto keyword_evaluation = std::make_shared<agent::KeywordEvaluationStrategy>(
      agent::KeywordEvaluationStrategyConfig{
          .required_keywords = {"approved"},
          .feedback = "Include approved in the answer.",
      });
  agent::EvaluatorLoopCoordinator evaluator_coordinator(agent::EvaluatorLoopCoordinatorOptions{
      .max_attempts = 3,
      .evaluation_strategy = keyword_evaluation,
  });
  auto evaluated = evaluator_coordinator.run("coordinate and evaluate", evaluator_context);
  SMOKE_CHECK(evaluated.accepted);
  SMOKE_CHECK(evaluated.attempts == 2);
  SMOKE_CHECK(evaluated.evaluation && evaluated.evaluation->accepted);
  SMOKE_CHECK(evaluator_model->generate_calls() == 2);
  SMOKE_CHECK(evaluator_artifacts.get("evaluation:1")->value.at("accepted").as_bool() == false);
  SMOKE_CHECK(evaluator_artifacts.get("evaluation:2")->value.at("accepted").as_bool());
  SMOKE_CHECK(evaluator_context.state.output.as_string() == "approved evaluator output");

  auto supervisor_worker_model = std::make_shared<SequentialTextModel>(
      std::vector<std::string>{"worker completed assignment"});
  agent::AgentRunner supervisor_worker_agent(agent::AgentRunnerConfig{
      .adapter = supervisor_worker_model,
      .max_iterations = 1,
  });
  agent::InMemoryArtifactStore supervisor_artifacts;
  agent::InMemorySharedStateStore supervisor_state;
  agent::InMemoryMailbox supervisor_mailbox;
  agent::CoordinatorContext supervisor_context;
  supervisor_context.worker_agents["worker-a"] = &supervisor_worker_agent;
  supervisor_context.artifact_store = &supervisor_artifacts;
  supervisor_context.shared_state = &supervisor_state;
  supervisor_context.mailbox = &supervisor_mailbox;
  supervisor_context.state = agent::create_shared_task_state("supervise");
  auto supervisor_router = std::make_shared<agent::RuleBasedTaskRouter>(
      std::vector<agent::RoutingRule>{agent::RoutingRule{.worker_id = "worker-a", .include = {"code"}}},
      "fallback");
  agent::SupervisorWorkerCoordinator supervisor_worker(agent::SupervisorWorkerCoordinatorOptions{
      .router = supervisor_router,
  });
  auto supervisor_worker_result = supervisor_worker.run("please write code", supervisor_context);
  SMOKE_CHECK(supervisor_worker_result.text == "worker completed assignment");
  SMOKE_CHECK(supervisor_worker_model->generate_calls() == 1);
  SMOKE_CHECK(supervisor_artifacts.get("worker:worker-a")->value.at("text").as_string() == "worker completed assignment");
  SMOKE_CHECK(supervisor_state.get("worker:worker-a")->value.at("output").as_string() == "worker completed assignment");
  auto supervisor_assignments = supervisor_mailbox.receive(agent::MailboxReadOptions{
      .channel = agent::DEFAULT_WORKER_ASSIGNMENT_CHANNEL,
      .recipient = "worker-a",
  });
  SMOKE_CHECK(supervisor_assignments.size() == 1);
  SMOKE_CHECK(supervisor_assignments.front().type == "assignment");
  SMOKE_CHECK(supervisor_assignments.front().payload.at("input").as_string() == "please write code");

  supervisor_mailbox.publish(agent::DEFAULT_SUPERVISOR_INBOX_CHANNEL,
                             "operator",
                             agent::Value::object({{"workerId", "worker-a"}}));
  agent::MailboxTaskRouter mailbox_router(agent::MailboxTaskRouterOptions{
      .fallback_worker_id = "fallback",
  });
  SMOKE_CHECK(mailbox_router.route("ignored", supervisor_context) == "worker-a");
  SMOKE_CHECK(supervisor_mailbox.receive(agent::MailboxReadOptions{
      .channel = agent::DEFAULT_SUPERVISOR_INBOX_CHANNEL,
  }).empty());

  auto supervisor_eval_model = std::make_shared<SequentialTextModel>(
      std::vector<std::string>{"needs another pass", "approved supervised output"});
  agent::AgentRunner supervisor_eval_worker(agent::AgentRunnerConfig{
      .adapter = supervisor_eval_model,
      .max_iterations = 1,
  });
  agent::InMemoryArtifactStore supervisor_eval_artifacts;
  agent::CoordinatorContext supervisor_eval_context;
  supervisor_eval_context.worker_agents["worker-a"] = &supervisor_eval_worker;
  supervisor_eval_context.artifact_store = &supervisor_eval_artifacts;
  supervisor_eval_context.state = agent::create_shared_task_state("supervisor-eval");
  agent::SupervisorEvaluatorCoordinator supervisor_evaluator(agent::SupervisorEvaluatorCoordinatorOptions{
      .router = supervisor_router,
      .evaluation_strategy = keyword_evaluation,
      .max_attempts = 3,
      .lock_worker = true,
  });
  auto supervisor_eval_result = supervisor_evaluator.run("please write code", supervisor_eval_context);
  SMOKE_CHECK(supervisor_eval_result.accepted);
  SMOKE_CHECK(supervisor_eval_result.attempts == 2);
  SMOKE_CHECK(supervisor_eval_model->generate_calls() == 2);
  SMOKE_CHECK(supervisor_eval_artifacts.get("supervisor-evaluator:worker:1")->value.at("workerId").as_string() == "worker-a");
  SMOKE_CHECK(supervisor_eval_artifacts.get("supervisor-evaluator:evaluation:1")->value.at("accepted").as_bool() == false);
  SMOKE_CHECK(supervisor_eval_artifacts.get("supervisor-evaluator:evaluation:2")->value.at("accepted").as_bool());
  SMOKE_CHECK(supervisor_eval_context.state.output.as_string() == "approved supervised output");

  auto orchestration_workflow = agent::WorkflowBuilder("orchestration_wf")
                                    .start()
                                    .transform("shape",
                                               {{"message", agent::workflow_ref("input.prompt")},
                                                {"taskId", agent::workflow_ref("context.orchestrationTaskId")},
                                                {"phase", agent::workflow_ref("context.orchestrationMetadata.phase")}})
                                    .end("end", agent::workflow_ref("nodes.shape.output"))
                                    .sequence({"start", "shape", "end"})
                                    .build();
  agent::InMemoryWorkflowStore orchestration_workflow_store;
  agent::WorkflowEngine orchestration_workflow_engine(nullptr, nullptr, &orchestration_workflow_store);
  agent::InMemoryArtifactStore workflow_backed_artifacts;
  agent::CoordinatorContext workflow_backed_context;
  workflow_backed_context.workflow_engine = &orchestration_workflow_engine;
  workflow_backed_context.artifact_store = &workflow_backed_artifacts;
  workflow_backed_context.state = agent::create_shared_task_state(
      "workflow-backed",
      agent::Value::object({{"phase", "orchestration"}}));
  agent::WorkflowBackedCoordinator workflow_backed(agent::WorkflowBackedCoordinatorOptions{
      .definition = orchestration_workflow,
  });
  auto workflow_backed_result = workflow_backed.run("run workflow coordinator", workflow_backed_context);
  SMOKE_CHECK(workflow_backed_result.workflow.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(workflow_backed_result.workflow.output.at("message").as_string() == "run workflow coordinator");
  SMOKE_CHECK(workflow_backed_result.workflow.output.at("taskId").as_string() == workflow_backed_context.state.task_id);
  SMOKE_CHECK(workflow_backed_result.workflow.output.at("phase").as_string() == "orchestration");
  SMOKE_CHECK(workflow_backed_result.text.find("run workflow coordinator") != std::string::npos);
  SMOKE_CHECK(workflow_backed_artifacts.get("workflow:last")->value.at("status").as_string() == "completed");
  SMOKE_CHECK(workflow_backed_context.state.output.as_string().find("run workflow coordinator") != std::string::npos);

  auto supervisor_workflow_definition = agent::WorkflowBuilder("supervisor_workflow_wf")
                                            .start()
                                            .transform("shape",
                                                       {{"workerId", agent::workflow_ref("input.workerId")},
                                                        {"workerOutput", agent::workflow_ref("input.workerOutput")},
                                                        {"originalInput", agent::workflow_ref("input.originalInput")}})
                                            .end("end", agent::workflow_ref("nodes.shape.output"))
                                            .sequence({"start", "shape", "end"})
                                            .build();
  auto supervisor_workflow_model = std::make_shared<SequentialTextModel>(
      std::vector<std::string>{"workflow worker output"});
  agent::AgentRunner supervisor_workflow_worker(agent::AgentRunnerConfig{
      .adapter = supervisor_workflow_model,
      .max_iterations = 1,
  });
  agent::InMemoryWorkflowStore supervisor_workflow_store;
  agent::WorkflowEngine supervisor_workflow_engine(nullptr, nullptr, &supervisor_workflow_store);
  agent::InMemoryArtifactStore supervisor_workflow_artifacts;
  agent::InMemorySharedStateStore supervisor_workflow_state;
  agent::CoordinatorContext supervisor_workflow_context;
  supervisor_workflow_context.worker_agents["worker-a"] = &supervisor_workflow_worker;
  supervisor_workflow_context.workflow_engine = &supervisor_workflow_engine;
  supervisor_workflow_context.artifact_store = &supervisor_workflow_artifacts;
  supervisor_workflow_context.shared_state = &supervisor_workflow_state;
  supervisor_workflow_context.state = agent::create_shared_task_state("supervisor-workflow");
  agent::SupervisorWorkflowCoordinator supervisor_workflow(agent::SupervisorWorkflowCoordinatorOptions{
      .router = supervisor_router,
      .definition = supervisor_workflow_definition,
  });
  auto supervisor_workflow_result = supervisor_workflow.run("please write code", supervisor_workflow_context);
  SMOKE_CHECK(supervisor_workflow_result.workflow.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(supervisor_workflow_result.workflow.output.at("workerId").as_string() == "worker-a");
  SMOKE_CHECK(supervisor_workflow_result.workflow.output.at("workerOutput").as_string() == "workflow worker output");
  SMOKE_CHECK(supervisor_workflow_artifacts.get("worker:worker-a:latest")->value.at("text").as_string() ==
         "workflow worker output");
  SMOKE_CHECK(supervisor_workflow_artifacts.get("workflow:last")->value.at("status").as_string() == "completed");
  SMOKE_CHECK(supervisor_workflow_state.get("supervisor-workflow")->value.at("workerId").as_string() == "worker-a");
  SMOKE_CHECK(supervisor_workflow_context.state.output.as_string().find("workflow worker output") != std::string::npos);

  auto manifest = agent::write_run_replay(tmp_dir, "session-1", agent::Value::object({{"input", "hello"}}),
                                          agent::Value::object({{"text", run.text}}),
                                          {agent::Value::object({{"type", "tool-start"}})},
                                          "run-1", "agent-native");
  SMOKE_CHECK(manifest.event_count == 1);
  SMOKE_CHECK(std::filesystem::exists(tmp_dir / "session-1" / "run-1" / "replay.html"));
  auto loaded_replay = agent::load_run_replay(tmp_dir / "session-1" / "run-1");
  SMOKE_CHECK(loaded_replay.manifest.run_id == "run-1");
  SMOKE_CHECK(loaded_replay.events.size() == 1);
  auto replay_runs = agent::list_session_replays(tmp_dir, "session-1");
  SMOKE_CHECK(!replay_runs.empty());
  agent::WriteRunReplayOptions timed_replay_options;
  timed_replay_options.base_dir = tmp_dir;
  timed_replay_options.session_id = "timed session";
  timed_replay_options.run_id = "timed run";
  timed_replay_options.input = agent::Value::object({{"prompt", "timed"}});
  timed_replay_options.result = agent::Value::object({{"text", "timed result"}});
  timed_replay_options.started_at = "2025-01-01T00:00:00.250Z";
  timed_replay_options.finished_at = "2025-01-01T00:00:02.750Z";
  auto timed_replay = agent::write_run_replay(timed_replay_options);
  SMOKE_CHECK(timed_replay.manifest.duration_ms == 2500);
  SMOKE_CHECK(timed_replay.manifest.started_at == "2025-01-01T00:00:00.250Z");
  SMOKE_CHECK(timed_replay.manifest.finished_at == "2025-01-01T00:00:02.750Z");
  SMOKE_CHECK(timed_replay.dir_path == tmp_dir / "timed-session" / "timed-run");
  SMOKE_CHECK(timed_replay.manifest_path == timed_replay.dir_path / "manifest.json");
  SMOKE_CHECK(timed_replay.html_path == timed_replay.dir_path / "replay.html");
  auto loaded_timed_replay = agent::load_run_replay(timed_replay.dir_path);
  SMOKE_CHECK(loaded_timed_replay.manifest.duration_ms == 2500);
  auto loaded_timed_replay_from_html = agent::load_run_replay(timed_replay.html_path);
  SMOKE_CHECK(loaded_timed_replay_from_html.manifest.run_id == loaded_timed_replay.manifest.run_id);
  auto loaded_timed_replay_from_manifest = agent::load_run_replay(timed_replay.manifest_path);
  SMOKE_CHECK(loaded_timed_replay_from_manifest.manifest.duration_ms ==
         loaded_timed_replay.manifest.duration_ms);
  agent::WriteRunReplayOptions offset_replay_options = timed_replay_options;
  offset_replay_options.run_id = "offset run";
  offset_replay_options.started_at = "2025-01-01T01:00:00+01:00";
  offset_replay_options.finished_at = "2025-01-01T00:00:01Z";
  auto offset_replay = agent::write_run_replay(offset_replay_options);
  SMOKE_CHECK(offset_replay.manifest.duration_ms == 1000);
  agent::WriteRunReplayOptions negative_duration_replay_options = timed_replay_options;
  negative_duration_replay_options.run_id = "negative duration run";
  negative_duration_replay_options.started_at = "2025-01-01T00:00:02Z";
  negative_duration_replay_options.finished_at = "2025-01-01T00:00:01Z";
  auto negative_duration_replay = agent::write_run_replay(negative_duration_replay_options);
  SMOKE_CHECK(negative_duration_replay.manifest.duration_ms == 0);
  auto replay_without_result = agent::write_run_replay(
      tmp_dir,
      "session missing result",
      agent::Value::object({{"input", "<unsafe>"}}),
      agent::Value(),
      {agent::Value::object({{"type", "status"},
                            {"status", agent::Value::object({{"stage", "model"}})}}),
       agent::Value::object({{"type", "loop"}, {"event", "tool-start"}}),
       agent::Value::object({{"type", "loop"},
                            {"event", agent::Value::object({{"type", "tool-start"}})}})},
      "run missing result",
      "",
      "interrupted");
  const auto replay_without_result_dir = tmp_dir / "session-missing-result" / "run-missing-result";
  SMOKE_CHECK(replay_without_result.status == "error");
  SMOKE_CHECK(replay_without_result.tool_call_count == 2);
  SMOKE_CHECK(replay_without_result.result_file.empty());
  SMOKE_CHECK(!std::filesystem::exists(replay_without_result_dir / "result.json"));
  auto replay_without_result_manifest =
      agent::run_replay_manifest_to_value(replay_without_result);
  SMOKE_CHECK(!replay_without_result_manifest.contains("resultFile"));
  auto loaded_replay_without_result = agent::load_run_replay(replay_without_result_dir);
  SMOKE_CHECK(loaded_replay_without_result.result.is_null());
  SMOKE_CHECK(loaded_replay_without_result.manifest.error == "interrupted");
  const auto replay_without_result_html =
      agent::render_run_replay_html(loaded_replay_without_result);
  SMOKE_CHECK(replay_without_result_html.find("Status Stages") != std::string::npos);
  SMOKE_CHECK(replay_without_result_html.find("model") != std::string::npos);
  SMOKE_CHECK(replay_without_result_html.find("interrupted") != std::string::npos);
  SMOKE_CHECK(replay_without_result_html.find("&lt;unsafe&gt;") != std::string::npos);

  agent::EvalSuite eval_suite = agent::define_eval_suite(agent::EvalSuite{
      .cases = {agent::EvalCase{.id = "case-1", .input = "eval input", .expect_contains = {"tool result"}}},
  });
  eval_suite.cases.front().expect_contains_matchers = {agent::EvalTextMatcher{
      .value = "tool result",
      .regex = true,
      .flags = "i",
  }};
  const auto eval_suite_file = persistent_dir / "eval-suite.json";
  agent::write_json_file(eval_suite_file, agent::eval_suite_to_value(eval_suite));
  auto loaded_suite = agent::load_eval_suite(eval_suite_file);
  SMOKE_CHECK(loaded_suite.cases.size() == 1);
  SMOKE_CHECK(loaded_suite.cases.front().expect_contains_matchers.size() == 1);
  SMOKE_CHECK(loaded_suite.cases.front().expect_contains_matchers.front().flags == "i");
  loaded_suite.pricing = agent::EvalPricing{.input_per_1k_tokens = 0.01, .output_per_1k_tokens = 0.03};
  loaded_suite.replay_dir = persistent_dir / "eval-runs";
  loaded_suite.cases.front().expect_not_contains = {"not-present-in-output"};
  loaded_suite.cases.front().expect_not_contains_matchers = {agent::EvalTextMatcher{
      .value = "forbidden\\s+phrase",
      .regex = true,
      .flags = "i",
  }};
  loaded_suite.cases.front().min_output_length = 12;
  loaded_suite.cases.front().tags = {"smoke", "runtime"};
  loaded_suite.cases.front().metadata = agent::Value::object({{"owner", "native-eval"}});
  loaded_suite.cases.front().expect = {
      agent::expect_output_matches("tool result", "i"),
      agent::expect_output_not_matches("forbidden", "i"),
      agent::expect_tool_called("math.eval"),
      agent::expect_tool_call_count("math.eval", 1),
      agent::expect_status_stage_seen("model"),
      agent::expect_status_stage_seen("tool"),
      agent::expect_token_under(100),
      agent::expect_cost_under(0.001),
      agent::expect_latency_under(10000),
      agent::expect_citation_count_at_least(1),
      agent::expect_custom("custom-output-check", [](const agent::EvalAssertionContext& context) {
        const bool passed = context.result && context.result->text.find("tool result") != std::string::npos &&
                            context.usage.total_tokens == 30 &&
                            context.citation_count >= 1;
        return agent::EvalAssertionResult{"custom-output-check", passed,
                                          passed ? "custom assertion passed" : "custom assertion failed"};
      }),
  };
  auto serialized_eval_suite = agent::eval_suite_to_value(loaded_suite);
  const auto roundtripped_eval_suite = agent::eval_suite_from_value(serialized_eval_suite);
  SMOKE_CHECK(roundtripped_eval_suite.cases.front().expect.size() == loaded_suite.cases.front().expect.size());
  SMOKE_CHECK(roundtripped_eval_suite.cases.front().expect.front().name.find("/tool result/i") != std::string::npos);
  SMOKE_CHECK(roundtripped_eval_suite.cases.front().expect.back().name == "custom-output-check");
  SMOKE_CHECK(!roundtripped_eval_suite.cases.front().expect.back().evaluate);

  agent::ReasoningSettings eval_reasoning_settings;
  eval_reasoning_settings.enabled = true;
  eval_reasoning_settings.budget = 24.0;
  eval_reasoning_settings.include_thoughts = true;
  eval_reasoning_settings.tag_name = "reason";
  agent::EvalSuite settings_eval_suite{
      .cases = {agent::EvalCase{.id = "settings-case", .input = "settings eval", .expect_contains = {"settings ok"}}},
  };
  settings_eval_suite.cases.front().model_settings = agent::ModelSettings{
      .model = "eval-case-model",
      .temperature = 0.42,
      .max_output_tokens = 77,
      .reasoning = eval_reasoning_settings,
      .extra = agent::Value::object({{"vendor", "case-extra"}}),
  };
  auto serialized_settings_suite = agent::eval_suite_to_value(settings_eval_suite);
  SMOKE_CHECK(serialized_settings_suite.at("cases").as_array().front().at("modelSettings").at("model").as_string() ==
         "eval-case-model");
  SMOKE_CHECK(serialized_settings_suite.at("cases").as_array().front().at("modelSettings").at("reasoning")
             .at("includeThoughts").as_bool());
  auto roundtripped_settings_suite = agent::eval_suite_from_value(serialized_settings_suite);
  SMOKE_CHECK(roundtripped_settings_suite.cases.front().model_settings.temperature &&
         *roundtripped_settings_suite.cases.front().model_settings.temperature == 0.42);
  auto settings_model = std::make_shared<EvalSettingsModel>();
  agent::AgentRunner settings_eval_runner(agent::AgentRunnerConfig{
      .adapter = settings_model,
      .max_iterations = 1,
  });
  auto settings_report = agent::run_eval_suite(roundtripped_settings_suite, settings_eval_runner);
  SMOKE_CHECK(settings_report.passed_cases == 1);
  SMOKE_CHECK(settings_model->last_settings().model == "eval-case-model");
  SMOKE_CHECK(settings_model->last_settings().temperature && *settings_model->last_settings().temperature == 0.42);
  SMOKE_CHECK(settings_model->last_settings().max_output_tokens && *settings_model->last_settings().max_output_tokens == 77);
  SMOKE_CHECK(settings_model->last_settings().reasoning);
  SMOKE_CHECK(settings_model->last_settings().reasoning->enabled == true);
  SMOKE_CHECK(std::holds_alternative<double>(settings_model->last_settings().reasoning->budget));
  SMOKE_CHECK(std::get<double>(settings_model->last_settings().reasoning->budget) == 24.0);
  SMOKE_CHECK(settings_model->last_settings().reasoning->include_thoughts == true);
  SMOKE_CHECK(settings_model->last_settings().reasoning->tag_name == "reason");
  SMOKE_CHECK(settings_model->last_settings().extra.at("vendor").as_string() == "case-extra");

  auto message_eval_input = agent::Value::object({
      {"role", "user"},
      {"content", agent::Value::array({
                      agent::Value::object({
                          {"type", "text"},
                          {"text", "message eval"},
                          {"metadata", agent::Value::object({{"part", "text"}})},
                      }),
                      agent::Value::object({
                          {"type", "image"},
                          {"source", agent::Value::object({
                                         {"kind", "inline"},
                                         {"data", "aW1hZ2U="},
                                         {"mimeType", "image/png"},
                                         {"filename", "eval.png"},
                                     })},
                          {"altText", "eval image"},
                          {"detail", "high"},
                          {"metadata", agent::Value::object({{"part", "image"}})},
                      }),
                  })},
      {"metadata", agent::Value::object({{"source", "eval-message-input"}})},
  });
  agent::EvalSuite message_eval_suite{
      .cases = {agent::EvalCase{
          .id = "message-input-case",
          .input_value = message_eval_input,
          .expect_contains = {"captured stream"},
      }},
  };
  auto serialized_message_eval_suite = agent::eval_suite_to_value(message_eval_suite);
  SMOKE_CHECK(serialized_message_eval_suite.at("cases").as_array().front().at("input").is_object());
  auto roundtripped_message_eval_suite = agent::eval_suite_from_value(serialized_message_eval_suite);
  SMOKE_CHECK(roundtripped_message_eval_suite.cases.front().input == "message eval");
  SMOKE_CHECK(roundtripped_message_eval_suite.cases.front().input_value.at("metadata").at("source").as_string() ==
         "eval-message-input");
  auto message_eval_model = std::make_shared<CapturingMultimodalModel>();
  int message_runner_factory_calls = 0;
  auto message_eval_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = roundtripped_message_eval_suite,
      .create_runner = [&](const agent::EvalCase& test_case) {
        SMOKE_CHECK(test_case.id == "message-input-case");
        ++message_runner_factory_calls;
        agent::AgentRunnerConfig config;
        config.adapter = message_eval_model;
        return std::make_shared<agent::AgentRunner>(std::move(config));
      },
  });
  SMOKE_CHECK(message_runner_factory_calls == 1);
  SMOKE_CHECK(message_eval_report.passed_cases == 1);
  const auto message_capture = std::find_if(message_eval_model->messages().begin(),
                                            message_eval_model->messages().end(),
                                            [](const agent::AgentMessage& message) {
                                              return message.role == agent::MessageRole::User;
                                            });
  SMOKE_CHECK(message_capture != message_eval_model->messages().end());
  SMOKE_CHECK(message_capture->metadata.at("source").as_string() == "eval-message-input");
  SMOKE_CHECK(message_capture->content.size() == 2);
  SMOKE_CHECK(message_capture->content.front().text == "message eval");
  SMOKE_CHECK(message_capture->content.back().type == agent::ContentPartType::Image);
  SMOKE_CHECK(message_capture->content.back().alt_text == "eval image");
  SMOKE_CHECK(message_capture->content.back().source.mime_type == "image/png");

  auto eval_config = agent::Value::object({
      {"agents", agent::Value::object({
          {"configured", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
          })},
      })},
  });
  agent::EvalSuite config_eval_suite{
      .agent = "configured",
      .cases = {agent::EvalCase{
          .id = "config-eval-case",
          .input = "config eval input",
          .expect_contains = {"config eval input"},
      }},
  };
  auto config_eval_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = config_eval_suite,
      .config = eval_config,
  });
  SMOKE_CHECK(config_eval_report.passed_cases == 1);
  SMOKE_CHECK(config_eval_report.results.front().session_id == "eval:config-eval-case");

  const auto eval_config_file = persistent_dir / "eval-config.json";
  agent::write_json_file(eval_config_file, eval_config);
  agent::EvalSuite config_path_eval_suite{
      .cases = {agent::EvalCase{
          .id = "config-path-eval-case",
          .input = "config path eval input",
          .expect_contains = {"config path eval input"},
      }},
  };
  auto config_path_eval_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = config_path_eval_suite,
      .config_path = eval_config_file,
      .agent = "configured",
  });
  SMOKE_CHECK(config_path_eval_report.passed_cases == 1);

  const auto eval_loaded_config_root = persistent_dir / "eval-loaded-config-root";
  std::filesystem::create_directories(eval_loaded_config_root);
  auto eval_loaded_config = agent::define_native_loaded_agent_config(agent::Value::object({
      {"agents", agent::Value::object({
          {"loadedEval", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "eval-loaded-sessions"}})},
              })},
          })},
      })},
  }), eval_loaded_config_root);
  auto loaded_config_eval_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = agent::EvalSuite{
          .cases = {agent::EvalCase{
              .id = "loaded-config-eval-case",
              .input = "loaded config eval input",
              .session_id = "loaded-config-eval-session",
              .expect_contains = {"loaded config eval input"},
          }},
      },
      .loaded_config = eval_loaded_config,
      .agent = "loadedEval",
  });
  SMOKE_CHECK(loaded_config_eval_report.passed_cases == 1);
  SMOKE_CHECK(std::filesystem::exists(eval_loaded_config_root / "eval-loaded-sessions" /
                                 "loaded-config-eval-session.json"));

  int eval_provider_stream_calls = 0;
  auto provider_eval_config = agent::Value::object({
      {"agents", agent::Value::object({
          {"providerEval", agent::Value::object({
              {"model", agent::Value::object({
                            {"provider", "openai"},
                            {"model", "gpt-eval"},
                            {"apiKey", "eval-key"},
                        })},
          })},
      })},
  });
  auto provider_eval_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = agent::EvalSuite{
          .cases = {agent::EvalCase{
              .id = "provider-stream-eval-case",
              .input = "provider stream eval input",
              .expect_contains = {"provider transport eval"},
          }},
      },
      .config = provider_eval_config,
      .agent = "providerEval",
      .provider_stream_transport = [&](const agent::NativeProviderRequest& request) {
        ++eval_provider_stream_calls;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer eval-key");
        return std::vector<std::string>{
            "data: {\"id\":\"chatcmpl-eval\",\"model\":\"gpt-eval\",\"choices\":[{\"delta\":{\"content\":\"provider transport eval\"}}]}\n\n",
            "data: {\"id\":\"chatcmpl-eval\",\"model\":\"gpt-eval\",\"choices\":[{\"finish_reason\":\"stop\",\"delta\":{}}]}\n\n",
            "data: [DONE]\n\n",
        };
      },
  });
  SMOKE_CHECK(eval_provider_stream_calls == 1);
  SMOKE_CHECK(provider_eval_report.passed_cases == 1);
  SMOKE_CHECK(provider_eval_report.results.front().output == "provider transport eval");

  const auto eval_report_file = persistent_dir / "eval-report.json";
  const auto eval_report_markdown = persistent_dir / "eval-report.md";
  const auto eval_baseline_file = persistent_dir / "eval-baseline.json";
  auto report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
      .suite = loaded_suite,
      .runner = &runner,
      .tags = {"smoke"},
      .report_path = eval_report_file,
      .markdown_report_path = eval_report_markdown,
      .baseline_path = eval_baseline_file,
      .update_baseline = true,
  });
  SMOKE_CHECK(report.total_cases == 1);
  SMOKE_CHECK(report.passed_cases == 1);
  SMOKE_CHECK(report.metadata.is_object());
  SMOKE_CHECK(report.results.front().tool_calls.size() == 1);
  SMOKE_CHECK(report.results.front().tool_calls.front() == "math.eval");
  SMOKE_CHECK(std::find(report.results.front().status_stages.begin(), report.results.front().status_stages.end(),
                   "model") != report.results.front().status_stages.end());
  SMOKE_CHECK(std::find(report.results.front().status_stages.begin(), report.results.front().status_stages.end(),
                   "tool") != report.results.front().status_stages.end());
  SMOKE_CHECK(report.results.front().usage.total_tokens == 30);
  SMOKE_CHECK(report.results.front().usage.estimated_cost && *report.results.front().usage.estimated_cost > 0);
  SMOKE_CHECK(report.results.front().knowledge_hit_count > 0);
  SMOKE_CHECK(report.results.front().citation_count >= 1);
  SMOKE_CHECK(!report.results.front().replay_path.empty());
  SMOKE_CHECK(std::filesystem::exists(report.results.front().replay_path));
  SMOKE_CHECK(std::filesystem::path(report.results.front().replay_path).filename() == "replay.html");
  SMOKE_CHECK(report.results.front().tags.size() == 2);
  SMOKE_CHECK(report.results.front().metadata.at("owner").as_string() == "native-eval");
  auto eval_replay = agent::load_run_replay(report.results.front().replay_path);
  SMOKE_CHECK(eval_replay.manifest.session_id == "eval:case-1");
  SMOKE_CHECK(eval_replay.events.size() > 0);
  auto eval_replay_from_dir =
      agent::load_run_replay(std::filesystem::path(report.results.front().replay_path).parent_path());
  SMOKE_CHECK(eval_replay_from_dir.manifest.run_id == eval_replay.manifest.run_id);
  agent::write_eval_report(report, eval_report_file);
  agent::write_eval_report(report, eval_report_markdown);
  auto loaded_report = agent::load_eval_report(eval_report_file);
  auto loaded_baseline = agent::load_eval_baseline(eval_baseline_file);
  SMOKE_CHECK(loaded_report.passed_cases == 1);
  SMOKE_CHECK(loaded_baseline.passed_cases == 1);
  SMOKE_CHECK(loaded_report.results.front().usage.total_tokens == 30);
  SMOKE_CHECK(!loaded_report.results.front().status_stages.empty());
  SMOKE_CHECK(!loaded_report.results.front().replay_path.empty());
  SMOKE_CHECK(agent::compare_eval_baseline(report, loaded_report).changed == false);
  SMOKE_CHECK(std::filesystem::exists(eval_report_markdown));
  std::ifstream eval_markdown_stream(eval_report_markdown);
  const std::string eval_markdown_text((std::istreambuf_iterator<char>(eval_markdown_stream)),
                                       std::istreambuf_iterator<char>());
  SMOKE_CHECK(eval_markdown_text.find("Total Tokens") != std::string::npos);
  SMOKE_CHECK(eval_markdown_text.find("Status Stages") != std::string::npos);

  agent::JsonSchema eval_output_schema;
  eval_output_schema.type = agent::JsonSchemaType::Object;
  eval_output_schema.required = {"message"};
  eval_output_schema.additional_properties = false;
  eval_output_schema.properties["message"].type = agent::JsonSchemaType::String;
  eval_output_schema.properties["message"].min_length = 5;
  agent::EvalSuite schema_eval_suite{
      .cases = {agent::EvalCase{.id = "schema-case", .input = "schema-governance"}},
  };
  schema_eval_suite.cases.front().expect = {agent::expect_json_schema(eval_output_schema)};
  auto serialized_schema_suite = agent::eval_suite_to_value(schema_eval_suite);
  SMOKE_CHECK(serialized_schema_suite.at("cases").as_array().front().at("expect").as_array().front()
             .at("schema").at("properties").at("message").at("type").as_string() == "string");
  SMOKE_CHECK(!serialized_schema_suite.at("cases").as_array().front().at("expect").as_array().front()
              .at("schema").at("additionalProperties").as_bool(true));
  auto roundtripped_schema_suite = agent::eval_suite_from_value(serialized_schema_suite);
  SMOKE_CHECK(roundtripped_schema_suite.cases.front().expect.front().name == "jsonSchema");
  SMOKE_CHECK(roundtripped_schema_suite.cases.front().expect.front().evaluate);
  agent::AgentRunner schema_eval_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<GovernanceModel>(),
      .max_iterations = 1,
  });
  auto schema_eval_report = agent::run_eval_suite(roundtripped_schema_suite, schema_eval_runner);
  SMOKE_CHECK(schema_eval_report.total_cases == 1);
  SMOKE_CHECK(schema_eval_report.passed_cases == 1);
  SMOKE_CHECK(schema_eval_report.results.front().assertions.size() == 1);
  SMOKE_CHECK(schema_eval_report.results.front().assertions.front().name == "jsonSchema");

  auto pre_cancel_eval_model = std::make_shared<ToolCallingModel>();
  agent::AgentRunner pre_cancel_eval_runner(agent::AgentRunnerConfig{
      .adapter = pre_cancel_eval_model,
      .tools = tools,
      .max_iterations = 2,
  });
  agent::EvalSuite pre_cancel_eval_suite{
      .cases = {agent::EvalCase{.id = "pre-cancel", .input = "should not run"}},
  };
  agent::CancellationToken pre_eval_cancellation;
  pre_eval_cancellation.cancel("eval cancelled before start");
  bool pre_eval_cancelled = false;
  try {
    (void)agent::run_eval_suite(pre_cancel_eval_suite, pre_cancel_eval_runner, {}, {}, false,
                                &pre_eval_cancellation);
  } catch (const std::exception& error) {
    pre_eval_cancelled = true;
    SMOKE_CHECK(std::string(error.what()).find("eval cancelled before start") != std::string::npos);
  }
  SMOKE_CHECK(pre_eval_cancelled);
  SMOKE_CHECK(pre_cancel_eval_model->generate_calls() == 0);

  auto eval_cancel_model = std::make_shared<CancellingToolModel>();
  auto eval_cancel_tool = agent::define_tool(agent::ToolDefinition{
      .name = "runtime.cancel",
      .description = "Cancel eval runtime",
      .input_schema = agent::JsonSchema{.type = agent::JsonSchemaType::Object},
      .execute = [](const agent::Value&, agent::ToolExecutionContext& context) {
        SMOKE_CHECK(context.cancellation != nullptr);
        context.cancellation->cancel("eval tool requested cancellation");
        return agent::Value::object({{"cancelled", true}});
      },
  });
  auto eval_cancel_tools = tools;
  eval_cancel_tools.push_back(eval_cancel_tool);
  agent::AgentRunner eval_cancel_runner(agent::AgentRunnerConfig{
      .adapter = eval_cancel_model,
      .tools = eval_cancel_tools,
      .max_iterations = 3,
  });
  agent::EvalSuite active_cancel_eval_suite{
      .cases = {agent::EvalCase{.id = "active-cancel", .input = "cancel eval"}},
  };
  agent::CancellationToken active_eval_cancellation;
  bool active_eval_cancelled = false;
  try {
    (void)agent::run_eval_suite(active_cancel_eval_suite, eval_cancel_runner, {}, {}, false,
                                &active_eval_cancellation);
  } catch (const std::exception& error) {
    active_eval_cancelled = true;
    SMOKE_CHECK(std::string(error.what()).find("eval tool requested cancellation") != std::string::npos);
  }
  SMOKE_CHECK(active_eval_cancelled);
  SMOKE_CHECK(active_eval_cancellation.cancelled());
  SMOKE_CHECK(eval_cancel_model->generate_calls() == 1);

  agent::AgentRunner permission_eval_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<ToolCallingModel>(),
      .tools = tools,
      .max_iterations = 3,
      .permission_policy = [](const agent::PermissionRequest&) {
        return agent::PermissionDecision{agent::PermissionDecisionKind::Ask, "approval required"};
      },
      .approval_handler = agent::create_static_approval_handler(
          agent::PermissionDecision{agent::PermissionDecisionKind::Deny, "eval denied"}),
  });
  agent::EvalSuite permission_eval_suite{
      .cases = {agent::EvalCase{.id = "permission-case", .input = "permission eval"}},
  };
  permission_eval_suite.cases.front().expect = {
      agent::expect_status_stage_seen("tool"),
      agent::expect_approval_requested("math.eval"),
      agent::expect_approval_denied("math.eval"),
  };
  auto permission_report = agent::run_eval_suite(permission_eval_suite, permission_eval_runner);
  SMOKE_CHECK(permission_report.passed_cases == 1);
  SMOKE_CHECK(permission_report.results.front().permission_events.size() == 2);
  SMOKE_CHECK(permission_report.results.front().permission_events.front().category == "permission.approval_requested");
  SMOKE_CHECK(permission_report.results.front().permission_events.back().category == "permission.denied");
  const auto permission_report_value = agent::eval_report_to_value(permission_report);
  const auto reloaded_permission_report = agent::eval_report_from_value(permission_report_value);
  SMOKE_CHECK(reloaded_permission_report.results.front().permission_events.size() == 2);

  std::cout << "agent_native smoke passed\n";
  return 0;
