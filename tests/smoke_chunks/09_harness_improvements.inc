  // ===== Harness improvements smoke =====

  // (1) Auto-compaction at configurable token threshold.
  {
    agent::SessionMemoryOptions options;
    options.storage.session_id = "auto-compact";
    options.compaction.compaction_budget.max_messages = 2;
    options.compaction.token_budget = 200;
    options.compaction.auto_compact_at = 0.5;
    options.compaction.compact_on_append = true;
    int summarizer_calls = 0;
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      ++summarizer_calls;
      std::string summary = input.previous_summary;
      if (!summary.empty()) summary += "\n";
      summary += "compacted:" + std::to_string(input.archived_messages.size());
      return summary;
    };
    auto session = std::make_shared<agent::SessionMemory>(std::move(options));
    for (int i = 0; i < 20; ++i) {
      std::string text(40, 'x');
      text += std::to_string(i);
      session->add(agent::create_message(agent::MessageRole::User, text));
    }

    class MarkerModel final : public agent::ChatModelAdapter {
     public:
      MarkerModel() : ChatModelAdapter("test", "marker", 0.0, 64) {}
      agent::AgentOutput generate(const agent::GenerateParams&) override {
        return build_output(agent::AgentOutput{.text = "ok"});
      }
    };

    agent::ToolRegistry empty_registry({});
    agent::ToolExecutor empty_executor(empty_registry);
    agent::EmbeddedContextManager context_manager;
    agent::EventBus auto_compact_bus;
    std::atomic<int> auto_compact_events{0};
    auto_compact_bus.register_sink([&](const agent::FrameworkEvent& event) {
      if (event.category == "session.auto_compact") ++auto_compact_events;
    });
    agent::internal::AgentLoop loop(agent::internal::AgentLoopConfig{
        .model = std::make_shared<MarkerModel>(),
        .tool_registry = &empty_registry,
        .tool_executor = &empty_executor,
        .context_manager = &context_manager,
        .max_iterations = 1,
        .event_bus = &auto_compact_bus,
    });
    const bool was_triggered_before_run = session->should_auto_compact();
    assert(was_triggered_before_run);
    loop.run(*session, std::string("trigger"));
    assert(summarizer_calls >= 1);
    assert(auto_compact_events.load() >= 1);
  }

  // (1b) CompactionBudget — the unit-alignment bug repro that motivated the
  // CompactionBudget refactor. Previously `auto_compact_at_ * token_budget`
  // triggered should_auto_compact() but compact() truncated by message count,
  // so a token-shaped cap silently never dropped anything.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-token-cap";
    opts.compaction.token_budget = 200000;
    opts.compaction.auto_compact_at = 0.8;
    opts.compaction.compaction_budget.max_tokens = 100000;
    agent::SessionMemory session(std::move(opts));
    // Push messages until estimated tokens cross 160k (80% of 200k). Each
    // 1000-char message is ~250 tokens, so we need ~640 messages — push 800
    // to be safe.
    const std::string filler(1000, 'x');
    for (int i = 0; i < 800; ++i) {
      session.add(agent::create_message(agent::MessageRole::User, filler + std::to_string(i)));
    }
    assert(session.should_auto_compact());
    const std::size_t tokens_before = session.estimated_token_count();
    assert(tokens_before >= 160000);
    session.compact();
    const std::size_t tokens_after = session.estimated_token_count();
    assert(tokens_after <= 100000);
    assert(tokens_after < tokens_before);
  }

  // (1c) Empty budget throws ConfigurationError. The error message MUST
  // contain "no CompactionBudget" for grep-ability.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-empty";
    agent::SessionMemory session(std::move(opts));
    // add() silently no-ops when no budget — only explicit compact() / the
    // auto-compaction path throws on misconfiguration.
    session.add(agent::create_message(agent::MessageRole::User, "hello"));
    session.add(agent::create_message(agent::MessageRole::User, "world"));
    bool threw = false;
    std::string error_text;
    try {
      session.compact();
    } catch (const agent::ConfigurationError& e) {
      threw = true;
      error_text = e.what();
    }
    assert(threw);
    assert(error_text.find("no CompactionBudget") != std::string::npos);
  }

  // (1d) Both caps applied simultaneously — the message-count cap is
  // satisfied first but the token cap forces further dropping.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-both-caps";
    opts.compaction.compaction_budget.max_messages = 10;
    opts.compaction.compaction_budget.max_tokens = 5000;
    opts.compaction.compact_on_append = true;
    agent::SessionMemory session(std::move(opts));
    // Each message ~4000 chars => ~1000 tokens. 10 messages would be ~10000
    // tokens (over the 5000 token cap), forcing extra drop.
    const std::string filler(4000, 'y');
    for (int i = 0; i < 20; ++i) {
      session.add(agent::create_message(agent::MessageRole::User, filler + std::to_string(i)));
    }
    const auto messages = session.get_messages(false);
    assert(messages.size() < 10);
    assert(session.estimated_token_count() <= 5000);
  }

  // (1e) Count-only mode behaves like the old API — truncates to N latest.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-count-only";
    opts.compaction.compaction_budget.max_messages = 5;
    opts.compaction.compact_on_append = true;
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 12; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("msg-") + std::to_string(i)));
    }
    const auto messages = session.get_messages(false);
    assert(messages.size() == 5);
    assert(agent::extract_text_content(messages.front().content) == "msg-7");
    assert(agent::extract_text_content(messages.back().content) == "msg-11");
  }

  // (1f) Token-only mode — drops oldest until estimated tokens <= cap.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-token-only";
    opts.compaction.compaction_budget.max_tokens = 1000;
    opts.compaction.compact_on_append = true;
    agent::SessionMemory session(std::move(opts));
    const std::string filler(400, 'z');
    for (int i = 0; i < 50; ++i) {
      session.add(agent::create_message(agent::MessageRole::User, filler + std::to_string(i)));
    }
    assert(session.estimated_token_count() <= 1000);
    // Some messages must remain.
    assert(!session.get_messages(false).empty());
  }

  // (1g) Summarizer receives overflow batch on compaction.
  {
    std::vector<agent::SessionMemorySummaryInput> captured;
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "budget-summarizer";
    opts.compaction.compaction_budget.max_messages = 2;
    opts.compaction.compact_on_append = true;
    opts.storage.summary_label = "Notes";
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      captured.push_back(input);
      return std::string("summarized:") + std::to_string(input.archived_messages.size());
    };
    agent::SessionMemory session(std::move(opts));
    session.add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "alpha"),
        agent::create_message(agent::MessageRole::User, "beta"),
        agent::create_message(agent::MessageRole::User, "gamma"),
        agent::create_message(agent::MessageRole::User, "delta"),
    });
    assert(captured.size() == 1);
    assert(captured.front().archived_messages.size() == 2);
    assert(agent::extract_text_content(captured.front().archived_messages.front().content) == "alpha");
    assert(agent::extract_text_content(captured.front().archived_messages.back().content) == "beta");
    const auto messages = session.get_messages(true);
    assert(messages.size() == 3);  // summary system message + 2 live messages
    assert(agent::extract_text_content(messages.front().content).find("Notes:\nsummarized:2") !=
           std::string::npos);
  }

  // (2) Token-aware truncation helper.
  {
    std::string big;
    for (int i = 1; i <= 500; ++i) {
      big += "line";
      big += std::to_string(i);
      big += "\n";
    }
    const auto truncated = agent::truncate_for_model(big);
    assert(truncated.truncated);
    assert(truncated.total_lines == 500);
    assert(truncated.text.find("line1\n") != std::string::npos);
    assert(truncated.text.find("line500\n") != std::string::npos);
    assert(truncated.text.find("line250\n") == std::string::npos);
    const auto v = agent::truncated_output_to_value(truncated);
    assert(v.at("truncated").as_bool());
    assert(v.at("totalLines").as_integer() == 500);
    assert(v.at("kept_bytes").as_integer() > 0);
  }

  // (3) Stale-read detection on fs.writeText.
  {
    const auto base = std::filesystem::temp_directory_path() / "agent_native_smoke_stale";
    std::filesystem::create_directories(base);
    const auto path = base / "watched.txt";
    {
      std::ofstream out(path);
      out << "v1";
    }
    const auto original_sha = agent::sha256_hex("v1");
    // Mutate externally so the agent's expected sha is now stale.
    {
      std::ofstream out(path);
      out << "v2-by-other";
    }

    auto fs_tools = agent::create_local_builtin_tools({}, true);
    agent::ToolRegistry fs_registry(fs_tools);
    agent::ToolExecutor fs_executor(fs_registry);
    auto stale_write = fs_executor.execute_tool_call(agent::ToolCall{
        .id = "stale_write",
        .name = "fs.writeText",
        .arguments = agent::Value::object({
            {"path", path.string()},
            {"content", std::string("v3-from-agent")},
            {"expectedSha256", original_sha},
        }),
    });
    assert(stale_write.ok);
    assert(stale_write.output.find("stale_read") != std::string::npos);
    {
      std::ifstream in(path);
      std::stringstream ss;
      ss << in.rdbuf();
      assert(ss.str() == "v2-by-other");
    }
    // A write without expectedSha256 still works.
    auto plain_write = fs_executor.execute_tool_call(agent::ToolCall{
        .id = "plain_write",
        .name = "fs.writeText",
        .arguments = agent::Value::object({
            {"path", path.string()},
            {"content", std::string("v4-clean")},
        }),
    });
    assert(plain_write.ok);
    std::filesystem::remove_all(base);
  }

  // (3b) fs.writeText returns unified diff + line counts for UI rendering.
	{
	    const auto base = std::filesystem::temp_directory_path() / "agent_native_smoke_diff";
	    std::filesystem::create_directories(base);
	    const auto path = base / "doc.txt";

	    auto fs_tools = agent::create_local_builtin_tools({}, true);
	    agent::ToolRegistry fs_registry(fs_tools);
	    agent::EventBus fs_progress_bus;
	    std::vector<agent::FrameworkEvent> fs_progress_events;
	    fs_progress_bus.register_sink([&](const agent::FrameworkEvent& event) {
	      if (event.category == "tool.progress") {
	        fs_progress_events.push_back(event);
	      }
	    });
	    agent::ToolExecutor fs_executor(fs_registry, {}, {}, &fs_progress_bus);

    auto do_write = [&](const std::string& content) {
      return fs_executor.execute_tool_call(agent::ToolCall{
          .id = "diff_write",
          .name = "fs.writeText",
          .arguments = agent::Value::object({
              {"path", path.string()},
              {"content", content},
          }),
      });
    };

    // Case A: brand-new file, 3 lines → all additions.
    auto r1 = do_write("alpha\nbeta\ngamma\n");
    assert(r1.ok);
    assert(r1.message.content.size() == 1);
    auto payload1 = agent::parse_json(r1.output);
    assert(payload1.at("ok").as_bool());
    assert(payload1.at("newFile").as_bool());
	    assert(payload1.at("linesAdded").as_integer() == 3);
	    assert(payload1.at("linesRemoved").as_integer() == 0);
	    assert(payload1.at("diff").as_string().find("+++ b/") != std::string::npos);
	    assert(payload1.at("diff").as_string().find("+alpha") != std::string::npos);
	    assert(std::any_of(
	        fs_progress_events.begin(), fs_progress_events.end(),
	        [&](const agent::FrameworkEvent& event) {
	          return event.payload.at("kind").as_string() == "file.edit.preview"
	                 && event.payload.at("payload").at("path").as_string() == path.string()
	                 && event.payload.at("payload").at("linesAdded").as_integer() == 3;
	        }));
	    assert(std::any_of(
	        fs_progress_events.begin(), fs_progress_events.end(),
	        [&](const agent::FrameworkEvent& event) {
	          return event.payload.at("kind").as_string() == "file.edit.applied"
	                 && event.payload.at("payload").at("path").as_string() == path.string();
	        }));

    // Case B: rewrite identical content → no diff, zero counts.
    auto r2 = do_write("alpha\nbeta\ngamma\n");
    assert(r2.ok);
    auto payload2 = agent::parse_json(r2.output);
    assert(!payload2.at("newFile").as_bool());
    assert(payload2.at("linesAdded").as_integer() == 0);
    assert(payload2.at("linesRemoved").as_integer() == 0);
    assert(payload2.at("diff").as_string().empty());

    // Case C: 5 lines → 4 lines (delete middle two, add one) → asymmetric.
    do_write("one\ntwo\nthree\nfour\nfive\n");  // reset baseline to 5 lines
    auto r3 = do_write("one\ntwo\nNEW\nfive\n");
    assert(r3.ok);
    auto payload3 = agent::parse_json(r3.output);
    assert(payload3.at("linesAdded").as_integer() >= 1);
    assert(payload3.at("linesRemoved").as_integer() >= 1);
    assert(payload3.at("diff").as_string().find("@@") != std::string::npos);

    // Case D: 1.5MB content → skip inline diff, placeholder text.
    std::string huge;
    huge.reserve(1500000);
    while (huge.size() < 1500000) huge += "padding-line-for-large-write\n";
    auto r4 = do_write(huge);
    assert(r4.ok);
    auto payload4 = agent::parse_json(r4.output);
    assert(payload4.at("ok").as_bool());
    assert(payload4.at("diff").as_string() == "(file too large for inline diff)");

    // Case E: binary content (embedded NUL) → "(binary file)" placeholder.
    std::string binary_content("hello\0world\nmore\nbytes", 22);
    auto r5 = do_write(binary_content);
    assert(r5.ok);
    auto payload5 = agent::parse_json(r5.output);
    assert(payload5.at("diff").as_string() == "(binary file)");
    assert(payload5.at("binary").as_bool());

    std::filesystem::remove_all(base);
  }

  // (4) Thinking + critique adapters wired through eval.
  {
    class EchoMain final : public agent::ChatModelAdapter {
     public:
      EchoMain() : ChatModelAdapter("test", "echo-main", 0.0, 64) {}
      agent::AgentOutput generate(const agent::GenerateParams& params) override {
        std::string text = "main:";
        if (!params.messages.empty()) {
          text += agent::extract_text_content(params.messages.back().content);
        }
        return build_output(agent::AgentOutput{.text = text});
      }
    };
    class CritiquePrefix final : public agent::ChatModelAdapter {
     public:
      CritiquePrefix() : ChatModelAdapter("test", "critique-prefix", 0.0, 64) {}
      int calls = 0;
      agent::AgentOutput generate(const agent::GenerateParams&) override {
        ++calls;
        return build_output(agent::AgentOutput{.text = "PASS [critique] graded"});
      }
    };

    auto critique = std::make_shared<CritiquePrefix>();
    agent::AgentRunnerConfig runner_config;
    runner_config.model_runtime.adapter = std::make_shared<EchoMain>();
    runner_config.model_runtime.critique_adapter = critique;
    runner_config.memory_runtime.session_store = std::make_shared<agent::InMemorySessionStore>();
    agent::AgentRunner critique_runner(runner_config);

    agent::EvalSuite suite;
    suite.agent = "critique-suite";
    suite.cases.push_back(agent::EvalCase{
        .id = "judge-case",
        .input = "hello",
        .expect = {agent::expect_llm_judge("judge", "Must start with PASS.")},
    });
    auto report = agent::run_eval_suite(suite, critique_runner);
    assert(report.total_cases == 1);
    assert(report.passed_cases == 1);
    assert(critique->calls >= 1);

    // Without a critique adapter, falls back + emits the warning event.
    agent::AgentRunnerConfig fallback_config;
    fallback_config.model_runtime.adapter = std::make_shared<EchoMain>();
    fallback_config.memory_runtime.session_store = std::make_shared<agent::InMemorySessionStore>();
    agent::AgentRunner fallback_runner(fallback_config);
    std::atomic<int> self_grade_events{0};
    fallback_runner.events().register_sink([&](const agent::FrameworkEvent& event) {
      if (event.category == "eval.self_grading_detected") ++self_grade_events;
    });
    agent::EvalSuite fallback_suite;
    fallback_suite.agent = "fallback-suite";
    fallback_suite.cases.push_back(agent::EvalCase{
        .id = "fallback-case",
        .input = "hi",
        .expect = {agent::expect_llm_judge("judge", "Must start with PASS.")},
    });
    auto fallback_report = agent::run_eval_suite(fallback_suite, fallback_runner);
    assert(fallback_report.total_cases == 1);
    assert(self_grade_events.load() >= 1);
  }

  // (5) Lazy tool discovery.
  {
    auto core = agent::create_core_builtin_tools();
    agent::ToolRegistry lazy_registry(core);
    lazy_registry.set_lazy_mode(true);
    // Visible list should only include tool.* in lazy mode.
    auto visible = lazy_registry.list();
    bool found_tool_search = false;
    bool found_tool_describe = false;
    bool found_json_query = false;
    for (const auto& tool : visible) {
      if (tool.name == "tool.search") found_tool_search = true;
      if (tool.name == "tool.describe") found_tool_describe = true;
      if (tool.name == "json.query") found_json_query = true;
    }
    assert(found_tool_search);
    assert(found_tool_describe);
    assert(!found_json_query);
    // list_all should still include hidden tools.
    auto all = lazy_registry.list_all();
    assert(std::any_of(all.begin(), all.end(),
                       [](const auto& t) { return t.name == "json.query"; }));

    agent::ToolExecutor lazy_executor(lazy_registry);
    auto search_result = lazy_executor.execute_tool_call(agent::ToolCall{
        .id = "search_call",
        .name = "tool.search",
        .arguments = agent::Value::object({{"query", std::string("json")}, {"limit", 5LL}}),
    });
    assert(search_result.ok);
    assert(search_result.output.find("json.query") != std::string::npos);

    // Hidden tools still executable when called directly.
    auto direct_result = lazy_executor.execute_tool_call(agent::ToolCall{
        .id = "direct_call",
        .name = "json.query",
        .arguments = agent::Value::object({
            {"value", agent::Value::object({{"k", "v"}})},
            {"path", std::string("k")},
        }),
    });
    assert(direct_result.ok);
    assert(direct_result.output.find("v") != std::string::npos);

    auto describe_result = lazy_executor.execute_tool_call(agent::ToolCall{
        .id = "describe_call",
        .name = "tool.describe",
        .arguments = agent::Value::object({{"name", std::string("json.query")}}),
    });
    assert(describe_result.ok);
    assert(describe_result.output.find("json.query") != std::string::npos);
  }

  // (7) Process hook adapter — exit code 2 blocks, 0 allows.
#ifndef _WIN32
  {
    const auto script_path = std::filesystem::temp_directory_path() / "agent_native_smoke_process_hook.sh";
    {
      std::ofstream out(script_path);
      out << "#!/bin/sh\n"
             "input=$(cat)\n"
             "case \"$input\" in\n"
             "  *allowme*) exit 0 ;;\n"
             "  *) echo blocked 1>&2; exit 2 ;;\n"
             "esac\n";
    }
    std::filesystem::permissions(script_path,
                                 std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read);
    agent::ProcessHookConfig config;
    config.executable = script_path;
    config.timeout = std::chrono::milliseconds(5000);

    auto allow_result = agent::run_process_hook(config, agent::Value::object({{"msg", std::string("allowme please")}}));
    assert(allow_result.decision == agent::ProcessHookDecision::Allow);
    assert(allow_result.exit_code == 0);

    auto block_result = agent::run_process_hook(config, agent::Value::object({{"msg", std::string("nope")}}));
    assert(block_result.decision == agent::ProcessHookDecision::Block);
    assert(block_result.exit_code == 2);
    assert(block_result.stderr_text.find("blocked") != std::string::npos);

    std::filesystem::remove(script_path);
  }
  {
    const auto script_path = std::filesystem::temp_directory_path() / "agent_native_smoke_process_hook_early_exit.sh";
    {
      std::ofstream out(script_path);
      out << "#!/bin/sh\n"
             "exit 2\n";
    }
    std::filesystem::permissions(script_path,
                                 std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read);
    agent::ProcessHookConfig config;
    config.executable = script_path;
    config.timeout = std::chrono::milliseconds(2000);

    auto early_exit_result = agent::run_process_hook(
        config,
        agent::Value::object({{"msg", std::string(1024 * 1024, 'x')}}));
    assert(early_exit_result.decision == agent::ProcessHookDecision::Block);
    assert(early_exit_result.exit_code == 2);

    std::filesystem::remove(script_path);
  }
#endif

  // (8) Atomic JSON/text writes leave complete files and clean up temp names.
  {
    const auto atomic_dir = std::filesystem::temp_directory_path() / "agent_native_smoke_atomic_write";
    const auto atomic_path = atomic_dir / "state.json";
    std::filesystem::remove_all(atomic_dir);
    std::filesystem::create_directories(atomic_dir);
    agent::write_json_file(atomic_path, agent::Value::object({{"version", 1}}));
    agent::write_json_file(atomic_path, agent::Value::object({{"version", 2}, {"ok", true}}));
    const auto loaded = agent::read_json_file(atomic_path);
    assert(loaded.at("version").as_integer() == 2);
    assert(loaded.at("ok").as_bool());
    for (const auto& entry : std::filesystem::directory_iterator(atomic_dir)) {
      assert(entry.path().filename().string().find(".tmp-") == std::string::npos);
    }
    std::filesystem::remove_all(atomic_dir);
  }

  // (9) Default logging hook set: success silent, failure verbose.
  {
    std::vector<agent::HookLogEntry> log_entries;
    auto sink = [&](const agent::HookLogEntry& entry) { log_entries.push_back(entry); };
    agent::HookSet defaults = agent::default_logging_hook_set(sink);
    assert(defaults.after_tool);
    assert(defaults.on_tool_error);

    agent::ToolHookContext success_ctx;
    success_ctx.tool_name = "noop";
    defaults.after_tool(success_ctx);
    agent::ToolHookContext failure_ctx;
    failure_ctx.tool_name = "explode";
    failure_ctx.error = "boom";
    defaults.on_tool_error(failure_ctx);

    bool saw_trace_success = false;
    bool saw_warn_failure = false;
    for (const auto& entry : log_entries) {
      if (entry.severity == agent::HookLogSeverity::Trace && entry.source == "after_tool") {
        saw_trace_success = true;
      }
      if (entry.severity == agent::HookLogSeverity::Warn && entry.source == "on_tool_error") {
        saw_warn_failure = true;
        assert(entry.details.at("error").as_string() == "boom");
      }
    }
    assert(saw_trace_success);
    assert(saw_warn_failure);
  }

  // (8) AgentRunnerConfig::lazy_tool_mode + forced_visible_tools.
  {
    class CaptureToolsModel final : public agent::ChatModelAdapter {
     public:
      CaptureToolsModel(std::vector<std::string>* captured_names, std::vector<std::string>* captured_descriptors)
          : ChatModelAdapter("test", "capture-tools", 0.0, 64),
            captured_names_(captured_names),
            captured_descriptors_(captured_descriptors) {}
      agent::AgentOutput generate(const agent::GenerateParams& params) override {
        captured_names_->clear();
        captured_descriptors_->clear();
        for (const auto& tool : params.tools) {
          captured_names_->push_back(tool.name);
          captured_descriptors_->push_back(tool.name);
        }
        return build_output(agent::AgentOutput{.text = "lazy-ok"});
      }

     private:
      std::vector<std::string>* captured_names_;
      std::vector<std::string>* captured_descriptors_;
    };

    auto core_tools = agent::create_core_builtin_tools();
    auto local_tools = agent::create_local_builtin_tools({}, true);
    auto http_tools = agent::create_http_builtin_tools();

    auto find_tool = [](const std::vector<agent::ToolDefinition>& pool, std::string_view name) {
      for (const auto& t : pool) {
        if (t.name == name) return t;
      }
      assert(false && "expected tool not found in builtin pool");
      return agent::ToolDefinition{};
    };

    std::vector<agent::ToolDefinition> tools = {
        find_tool(core_tools, "tool.search"),
        find_tool(core_tools, "tool.describe"),
        find_tool(local_tools, "fs.readText"),
        find_tool(local_tools, "shell.exec"),
        find_tool(http_tools, "web.fetch"),
    };
    assert(tools.size() == 5);

    std::vector<std::string> captured_names;
    std::vector<std::string> captured_descriptors;
    auto model = std::make_shared<CaptureToolsModel>(&captured_names, &captured_descriptors);

    agent::AgentRunnerConfig lazy_config;
    lazy_config.model_runtime.adapter = model;
    lazy_config.tool_runtime.definitions = tools;
    lazy_config.tool_runtime.lazy_mode = true;
    lazy_config.tool_runtime.forced_visible_tools = {"fs.readText"};
    lazy_config.context_runtime.max_iterations = 1;
    lazy_config.memory_runtime.session_store = std::make_shared<agent::InMemorySessionStore>();
    agent::AgentRunner lazy_runner(lazy_config);

    // get_session works under the new config.
    auto session = lazy_runner.sessions().get("lazy-session");
    assert(session);

    agent::SessionMemoryOptions manual_compact_options;
    manual_compact_options.compaction.compaction_budget.max_messages = 1;
    auto manual_compact_store =
        std::make_shared<agent::InMemorySessionStore>(manual_compact_options);
    agent::EventBus manual_compact_bus;
    std::atomic<int> manual_compact_events{0};
    manual_compact_bus.register_sink([&](const agent::FrameworkEvent& event) {
      if (event.category == "session.compact") ++manual_compact_events;
    });
    agent::AgentRunnerConfig manual_compact_config;
    manual_compact_config.model_runtime.adapter = std::make_shared<CaptureToolsModel>(&captured_names, &captured_descriptors);
    manual_compact_config.memory_runtime.session_store = manual_compact_store;
    manual_compact_config.observability.event_bus = &manual_compact_bus;
    agent::AgentRunner manual_compact_runner(manual_compact_config);
    auto manual_compact_session = manual_compact_runner.sessions().get("manual-compact");
    manual_compact_session->add(agent::create_message(agent::MessageRole::User, "first"));
    manual_compact_session->add(agent::create_message(agent::MessageRole::Assistant, "second"));
    manual_compact_session->add(agent::create_message(agent::MessageRole::User, "third"));
    auto manual_compact_snapshot = manual_compact_runner.sessions().compact("manual-compact");
    assert(manual_compact_snapshot.session_id == "manual-compact");
    assert(manual_compact_snapshot.messages.size() == 1);
    assert(manual_compact_snapshot.summary.find("first") != std::string::npos);
    assert(manual_compact_events.load() == 1);

    auto run_result = lazy_runner.execution().run("hi", "lazy-session");
    assert(run_result.text == "lazy-ok");

    // The adapter should have seen only tool.search + tool.describe + fs.readText.
    auto has = [&](std::string_view name) {
      return std::any_of(captured_names.begin(), captured_names.end(),
                         [&](const std::string& n) { return n == name; });
    };
    assert(has("tool.search"));
    assert(has("tool.describe"));
    assert(has("fs.readText"));
    assert(!has("shell.exec"));
    assert(!has("web.fetch"));

    // Default (no opt-in) → all 5 tools advertised.
    std::vector<std::string> default_names;
    std::vector<std::string> default_descriptors;
    auto default_model = std::make_shared<CaptureToolsModel>(&default_names, &default_descriptors);

    agent::AgentRunnerConfig default_config;
    default_config.model_runtime.adapter = default_model;
    default_config.tool_runtime.definitions = tools;
    default_config.context_runtime.max_iterations = 1;
    default_config.memory_runtime.session_store = std::make_shared<agent::InMemorySessionStore>();
    agent::AgentRunner default_runner(default_config);

    (void)default_runner.execution().run("hi", "default-session");
    auto default_has = [&](std::string_view name) {
      return std::any_of(default_names.begin(), default_names.end(),
                         [&](const std::string& n) { return n == name; });
    };
    assert(default_has("tool.search"));
    assert(default_has("tool.describe"));
    assert(default_has("fs.readText"));
    assert(default_has("shell.exec"));
    assert(default_has("web.fetch"));
  }

  {
    auto provider = std::make_shared<agent::DeterministicMediaGenerationProvider>(
        "fixture-media", "fixture-media-v1",
        std::vector<agent::MediaGenerationKind>{agent::MediaGenerationKind::Image,
                                                agent::MediaGenerationKind::Video,
                                                agent::MediaGenerationKind::Audio});
    agent::MediaGenerationProviderRegistry media_registry({provider});
    assert(media_registry.require_by_kind(agent::MediaGenerationKind::Image)->name() == "fixture-media");
    bool unsupported_kind_threw = false;
    try {
      auto image_only = std::make_shared<agent::DeterministicMediaGenerationProvider>(
          "image-only", "fixture-image-v1",
          std::vector<agent::MediaGenerationKind>{agent::MediaGenerationKind::Image});
      agent::MediaGenerationProviderRegistry image_registry({image_only});
      (void)image_registry.require_by_kind(agent::MediaGenerationKind::Video, "image-only");
    } catch (const agent::ConfigurationError& error) {
      unsupported_kind_threw = std::string(error.what()).find("does not support video") != std::string::npos;
    }
    assert(unsupported_kind_threw);

    agent::InMemoryArtifactStore media_artifacts;
    agent::MediaGenerationToolOptions media_options;
    media_options.default_provider = "fixture-media";
    media_options.output_options.artifact_writer =
        agent::create_in_memory_media_artifact_writer(&media_artifacts);
    media_options.output_options.artifact_key_prefix = "tool-media";
    auto media_tools = agent::create_media_generation_builtin_tools(&media_registry, media_options);
    assert(media_tools.size() == 3);
    for (auto& tool : media_tools) {
      agent::ToolExecutionContext tool_context;
      const auto invoked = tool.invoke(agent::Value::object({
          {"prompt", "make " + tool.name},
          {"count", 2},
      }), tool_context);
      const auto& envelope = std::get<agent::ToolResultEnvelope>(invoked);
      const auto& result = *envelope.value;
      const auto& artifacts = result.at("artifacts").as_array();
      assert(artifacts.size() == 2);
      assert(artifacts.front().at("source").at("kind").as_string() == "artifact");
      assert(artifacts.front().at("source").at("key").as_string().find("tool-media/") == 0);
      assert(media_artifacts.get(artifacts.front().at("source").at("key").as_string()).has_value());
    }

    auto inline_tools = agent::create_media_generation_builtin_tools(
        &media_registry,
        agent::MediaGenerationToolOptions{.default_provider = "fixture-media"});
    auto image_tool = std::find_if(inline_tools.begin(), inline_tools.end(), [](const auto& tool) {
      return tool.name == "media.generateImage";
    });
    assert(image_tool != inline_tools.end());
    agent::ToolExecutionContext inline_context;
    const auto inline_invoked = image_tool->invoke(agent::Value::object({
        {"prompt", "inline result"},
    }), inline_context);
    const auto& inline_envelope = std::get<agent::ToolResultEnvelope>(inline_invoked);
    const auto& inline_result = *inline_envelope.value;
    assert(inline_result.at("artifacts").as_array().front().at("source").at("kind").as_string() == "inline");
  }
