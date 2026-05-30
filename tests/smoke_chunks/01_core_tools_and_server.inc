  if (argc > 1 && std::string(argv[1]) == "--mcp-fixture-server") {
    return run_mcp_fixture_stdio_server();
  }
  const auto smoke_executable = std::filesystem::absolute(argv[0]).lexically_normal();

  auto tools = agent::create_core_builtin_tools();
  agent::ToolRegistry registry(tools);
  assert(registry.get("time.now") != nullptr);
  assert(registry.get("uuid.generate") != nullptr);
  assert(registry.get("json.query") != nullptr);

  agent::ToolExecutor executor(registry);
  auto result = executor.execute_tool_call(agent::ToolCall{
      .id = "manual_1",
      .name = "json.query",
      .arguments = agent::Value::object({
          {"value", agent::Value::object({{"a", agent::Value::object({{"b", 42}})}})},
          {"path", "a.b"},
      }),
  });
  assert(result.ok);
  assert(result.output.find("42") != std::string::npos);
  auto array_query_result = executor.execute_tool_call(agent::ToolCall{
      .id = "manual_array_query",
      .name = "json.query",
      .arguments = agent::Value::object({
          {"value", agent::Value::object({
                        {"items", agent::Value::array({
                                      agent::Value::object({{"name", "first"}}),
                                      agent::Value::object({{"name", "second"}}),
                                  })},
                    })},
          {"path", "items.1.name"},
      }),
  });
  assert(array_query_result.ok);
  assert(array_query_result.output.find("second") != std::string::npos);
  auto out_of_range_array_query = executor.execute_tool_call(agent::ToolCall{
      .id = "manual_array_query_out_of_range",
      .name = "json.query",
      .arguments = agent::Value::object({
          {"value", agent::Value::object({{"items", agent::Value::array({"zero"})}})},
          {"path", "items.999999999999999999999999"},
      }),
  });
  assert(out_of_range_array_query.ok);
  assert(out_of_range_array_query.output.find("null") != std::string::npos);
  // JSON \uXXXX unicode escape decoding (regression: used to emit literal '?').
  // Raw-string literals so the backslash-u sequences reach parse_json verbatim.
  {
    auto ascii_escape = agent::parse_json(R"("<>&")");
    assert(ascii_escape.as_string() == "<>&");

    auto bmp_escape = agent::parse_json(R"("你好")");
    assert(bmp_escape.as_string() == "\xe4\xbd\xa0\xe5\xa5\xbd");  // 你好

    auto surrogate_pair = agent::parse_json(R"("😀")");
    assert(surrogate_pair.as_string() == "\xf0\x9f\x98\x80");  // 😀

    // Mixed plain + escape — the original code would corrupt the escape only.
    auto mixed = agent::parse_json(R"("a<b")");
    assert(mixed.as_string() == "a<b");

    // Realistic Ollama-shape payload: code fragment with multiple escapes.
    auto code = agent::parse_json(R"("#include <iostream>")");
    assert(code.as_string() == "#include <iostream>");
  }

  // ===== Prompt-cache observability + explicit markers =====
  {
    const std::vector<agent::AgentMessage> cache_messages = {
        agent::create_message(agent::MessageRole::System, "system prompt for cache"),
        agent::create_message(agent::MessageRole::User, "hello cache"),
    };
    const std::vector<agent::ChatToolDescriptor> cache_tools = {
        agent::ChatToolDescriptor{.name = "lookup", .description = "Lookup data"},
        agent::ChatToolDescriptor{.name = "search", .description = "Search docs"},
    };

    // (1) Default: no cache_strategy => no markers in either provider.
    {
      agent::NativeProviderRequest captured;
      bool called = false;
      agent::AnthropicChatModelAdapter anthropic_default(
          "claude-default",
          [&](const agent::NativeProviderRequest& request) {
            called = true;
            captured = request;
            return agent::Value::object({
                {"id", "msg_default"},
                {"model", "claude-default"},
                {"stop_reason", "end_turn"},
                {"content", agent::Value::array({
                    agent::Value::object({{"type", "text"}, {"text", "ok"}}),
                })},
                {"usage", agent::Value::object({{"input_tokens", 10}, {"output_tokens", 2}})},
            });
          },
          "anthropic-key");
      (void)anthropic_default.generate(agent::GenerateParams{
          .messages = cache_messages,
          .tools = cache_tools,
          .settings = agent::ModelSettings{},
      });
      assert(called);
      // System should be a plain string (legacy form, no cache_control).
      assert(captured.body.at("system").is_string());
      const auto serialized_default = captured.body.stringify();
      assert(serialized_default.find("cache_control") == std::string::npos);
      assert(serialized_default.find("prompt_cache_key") == std::string::npos);

      // OpenAI default — no prompt_cache_key.
      agent::NativeProviderRequest openai_captured;
      agent::OpenAICompatibleChatModelAdapter openai_default(
          "openai", "gpt-default",
          [&](const agent::NativeProviderRequest& request) {
            openai_captured = request;
            return agent::Value::object({
                {"id", "resp_default"},
                {"choices", agent::Value::array({
                    agent::Value::object({
                        {"message", agent::Value::object({{"content", "ok"}})},
                        {"finish_reason", "stop"},
                    }),
                })},
                {"usage", agent::Value::object({{"prompt_tokens", 12}, {"completion_tokens", 2}})},
            });
          },
          "/v1/chat/completions", "https://api.openai.test", "openai-key");
      (void)openai_default.generate(agent::GenerateParams{
          .messages = cache_messages,
          .tools = cache_tools,
          .settings = agent::ModelSettings{},
      });
      const auto serialized_openai = openai_captured.body.stringify();
      assert(serialized_openai.find("prompt_cache_key") == std::string::npos);
    }

    // (2) Anthropic explicit, scope = SystemAndTools.
    {
      agent::NativeProviderRequest captured;
      agent::AnthropicChatModelAdapter anthropic_explicit(
          "claude-explicit",
          [&](const agent::NativeProviderRequest& request) {
            captured = request;
            return agent::Value::object({
                {"id", "msg_explicit"},
                {"model", "claude-explicit"},
                {"stop_reason", "end_turn"},
                {"content", agent::Value::array({
                    agent::Value::object({{"type", "text"}, {"text", "ok"}}),
                })},
                {"usage", agent::Value::object({
                    {"input_tokens", 5},
                    {"output_tokens", 2},
                    {"cache_read_input_tokens", 80},
                    {"cache_creation_input_tokens", 15},
                })},
            });
          },
          "anthropic-key");
      auto response = anthropic_explicit.generate(agent::GenerateParams{
          .messages = cache_messages,
          .tools = cache_tools,
          .settings = agent::ModelSettings{
              .cache_strategy = agent::CacheStrategy::Explicit,
              .cache_scope = agent::CacheScope::SystemAndTools,
          },
      });
      // System should now be an array carrying a cache_control marker.
      assert(captured.body.at("system").is_array());
      const auto& sys_blocks = captured.body.at("system").as_array();
      assert(!sys_blocks.empty());
      assert(sys_blocks.back().at("cache_control").at("type").as_string() == "ephemeral");
      // Last tool entry must carry the marker.
      const auto& tools_array = captured.body.at("tools").as_array();
      assert(tools_array.size() == 2);
      assert(tools_array.back().at("cache_control").at("type").as_string() == "ephemeral");
      // First tool must NOT carry one (only the last is marked).
      assert(!tools_array.front().contains("cache_control") ||
             tools_array.front().at("cache_control").is_null());

      // Usage parse: input_tokens rolls cached + creation back into the prompt total.
      const auto usage = agent::extract_model_usage(response);
      assert(usage.cached_input_tokens == 80);
      assert(usage.input_tokens == 5 + 80 + 15);
    }

    // (3) OpenAI explicit — derived prompt_cache_key.
    {
      agent::NativeProviderRequest captured;
      agent::OpenAICompatibleChatModelAdapter openai_explicit(
          "openai", "gpt-explicit",
          [&](const agent::NativeProviderRequest& request) {
            captured = request;
            return agent::Value::object({
                {"id", "resp_explicit"},
                {"choices", agent::Value::array({
                    agent::Value::object({
                        {"message", agent::Value::object({{"content", "ok"}})},
                        {"finish_reason", "stop"},
                    }),
                })},
                {"usage", agent::Value::object({
                    {"prompt_tokens", 120},
                    {"completion_tokens", 8},
                    {"prompt_tokens_details", agent::Value::object({{"cached_tokens", 100}})},
                })},
            });
          },
          "/v1/chat/completions", "https://api.openai.test", "openai-key");
      auto response = openai_explicit.generate(agent::GenerateParams{
          .messages = cache_messages,
          .tools = cache_tools,
          .settings = agent::ModelSettings{
              .cache_strategy = agent::CacheStrategy::Explicit,
          },
      });
      assert(captured.body.contains("prompt_cache_key"));
      const auto key = captured.body.at("prompt_cache_key").as_string();
      assert(key.rfind("agent:", 0) == 0);
      assert(key.size() == std::string("agent:").size() + 16);

      const auto usage = agent::extract_model_usage(response);
      assert(usage.cached_input_tokens == 100);
      assert(usage.input_tokens == 120);
    }

    // (4) OpenAI explicit with custom cache_key — verbatim.
    {
      agent::NativeProviderRequest captured;
      agent::OpenAICompatibleChatModelAdapter openai_custom_key(
          "openai", "gpt-custom-key",
          [&](const agent::NativeProviderRequest& request) {
            captured = request;
            return agent::Value::object({
                {"id", "resp_custom"},
                {"choices", agent::Value::array({
                    agent::Value::object({
                        {"message", agent::Value::object({{"content", "ok"}})},
                        {"finish_reason", "stop"},
                    }),
                })},
                {"usage", agent::Value::object({{"prompt_tokens", 7}, {"completion_tokens", 1}})},
            });
          },
          "/v1/chat/completions", "https://api.openai.test", "openai-key");
      (void)openai_custom_key.generate(agent::GenerateParams{
          .messages = cache_messages,
          .tools = cache_tools,
          .settings = agent::ModelSettings{
              .cache_strategy = agent::CacheStrategy::Explicit,
              .cache_key = "my-fixed-key",
          },
      });
      assert(captured.body.at("prompt_cache_key").as_string() == "my-fixed-key");
    }

    // (5) Usage round-trip + model.cache_stats event publication via AgentLoop.
    {
      class CacheStatsModel final : public agent::ChatModelAdapter {
       public:
        CacheStatsModel() : ChatModelAdapter("openai", "gpt-stats", 0.0, 64) {}
        agent::ModelResponse generate(const agent::GenerateParams&) override {
          agent::ModelResponse response;
          response.provider = "openai";
          response.model = "gpt-stats";
          response.text = "stats ok";
          response.raw = agent::Value::object({
              {"usage", agent::Value::object({
                  {"prompt_tokens", 200},
                  {"completion_tokens", 5},
                  {"prompt_tokens_details", agent::Value::object({{"cached_tokens", 100}})},
              })},
          });
          return build_response(std::move(response));
        }
      };

      auto stats_model = std::make_shared<CacheStatsModel>();
      agent::ToolRegistry stats_registry({});
      agent::ToolExecutor stats_executor(stats_registry);
      agent::EmbeddedContextManager stats_context;
      agent::EventBus stats_bus;
      agent::Value last_cache_payload;
      std::atomic<int> cache_event_count{0};
      stats_bus.register_sink([&](const agent::FrameworkEvent& event) {
        if (event.category == "model.cache_stats") {
          ++cache_event_count;
          last_cache_payload = event.payload;
        }
      });

      agent::SessionMemoryOptions session_options;
      session_options.storage.session_id = "cache-stats";
      auto session = std::make_shared<agent::SessionMemory>(std::move(session_options));

      agent::AgentLoop stats_loop(agent::AgentLoopConfig{
          .model = stats_model,
          .tool_registry = &stats_registry,
          .tool_executor = &stats_executor,
          .context_manager = &stats_context,
          .max_iterations = 1,
          .event_bus = &stats_bus,
      });
      stats_loop.run(*session, std::string("hello"));

      assert(cache_event_count.load() == 1);
      assert(last_cache_payload.at("provider").as_string() == "openai");
      assert(last_cache_payload.at("model").as_string() == "gpt-stats");
      assert(last_cache_payload.at("totalInputTokens").as_integer() == 200);
      assert(last_cache_payload.at("cachedInputTokens").as_integer() == 100);
      assert(last_cache_payload.at("strategy").as_string() == "none");
      const double hit_rate = last_cache_payload.at("hitRate").as_number();
      assert(hit_rate > 0.499 && hit_rate < 0.501);
    }
  }

  // JsonSchema: pattern / const / allOf / anyOf / oneOf
  {
    agent::JsonSchema pattern_schema;
    pattern_schema.type = agent::JsonSchemaType::String;
    pattern_schema.pattern = "^sk-[A-Za-z0-9]{4,}$";
    assert(agent::validate_json_schema(pattern_schema, agent::Value("sk-abcd")).empty());
    assert(!agent::validate_json_schema(pattern_schema, agent::Value("nope")).empty());

    agent::JsonSchema const_schema;
    const_schema.const_value = agent::Value::object({{"kind", "envelope"}});
    assert(agent::validate_json_schema(const_schema,
                                       agent::Value::object({{"kind", "envelope"}}))
               .empty());
    assert(!agent::validate_json_schema(const_schema,
                                        agent::Value::object({{"kind", "other"}}))
                .empty());

    agent::JsonSchema all_a;
    all_a.type = agent::JsonSchemaType::String;
    all_a.min_length = 3;
    agent::JsonSchema all_b;
    all_b.type = agent::JsonSchemaType::String;
    all_b.pattern = "^[a-z]+$";
    agent::JsonSchema all_of_schema;
    all_of_schema.all_of = {all_a, all_b};
    assert(agent::validate_json_schema(all_of_schema, agent::Value("hello")).empty());
    assert(!agent::validate_json_schema(all_of_schema, agent::Value("Hi")).empty());
    assert(!agent::validate_json_schema(all_of_schema, agent::Value("HELLO")).empty());

    agent::JsonSchema any_a;
    any_a.type = agent::JsonSchemaType::String;
    agent::JsonSchema any_b;
    any_b.type = agent::JsonSchemaType::Integer;
    agent::JsonSchema any_of_schema;
    any_of_schema.any_of = {any_a, any_b};
    assert(agent::validate_json_schema(any_of_schema, agent::Value("hello")).empty());
    assert(agent::validate_json_schema(any_of_schema,
                                       agent::Value(static_cast<long long>(42)))
               .empty());
    assert(!agent::validate_json_schema(any_of_schema, agent::Value(true)).empty());

    agent::JsonSchema one_a;
    one_a.type = agent::JsonSchemaType::Integer;
    one_a.maximum = 10;
    agent::JsonSchema one_b;
    one_b.type = agent::JsonSchemaType::Integer;
    one_b.minimum = 5;
    agent::JsonSchema one_of_schema;
    one_of_schema.one_of = {one_a, one_b};
    assert(agent::validate_json_schema(one_of_schema, agent::Value(static_cast<long long>(2)))
               .empty());  // matches a only
    assert(agent::validate_json_schema(one_of_schema, agent::Value(static_cast<long long>(20)))
               .empty());  // matches b only
    assert(!agent::validate_json_schema(one_of_schema, agent::Value(static_cast<long long>(7)))
                .empty());  // matches both → fails oneOf

    // Round-trip through JSON to validate to_value / from_value parity.
    agent::JsonSchema composed;
    composed.type = agent::JsonSchemaType::Object;
    composed.properties["name"].type = agent::JsonSchemaType::String;
    composed.properties["name"].pattern = "^[A-Z][a-z]+$";
    composed.properties["kind"].const_value = agent::Value("user");
    composed.one_of = {one_a, one_b};
    composed.any_of = {any_a, any_b};
    composed.all_of = {all_a};
    const auto serialized = agent::json_schema_to_value(composed);
    const auto round_tripped = agent::json_schema_from_value(serialized);
    assert(round_tripped.properties.at("name").pattern.has_value());
    assert(round_tripped.properties.at("name").pattern.value() == "^[A-Z][a-z]+$");
    assert(round_tripped.properties.at("kind").const_value.has_value());
    assert(round_tripped.properties.at("kind").const_value.value() == agent::Value("user"));
    assert(round_tripped.one_of.size() == 2);
    assert(round_tripped.any_of.size() == 2);
    assert(round_tripped.all_of.size() == 1);
  }

  // math.eval / time.* / text.diff / text.patch / scratch.* / todo.* smoke tests.
  {
    auto state_tools = agent::create_state_builtin_tools();
    auto core_tools = agent::create_core_builtin_tools();
    std::vector<agent::ToolDefinition> all_tools;
    all_tools.reserve(state_tools.size() + core_tools.size());
    for (auto& tool : core_tools) all_tools.push_back(std::move(tool));
    for (auto& tool : state_tools) all_tools.push_back(std::move(tool));
    agent::ToolRegistry feature_registry(all_tools);
    agent::ToolExecutor feature_executor(feature_registry);

    agent::SessionMemory session(agent::SessionMemoryOptions{.storage = {.session_id = "smoke-state-session"}});
    agent::InMemoryScratchStore scratch_store;
    agent::ToolExecutionContext base_ctx;
    base_ctx.service_refs.session = &session;
    base_ctx.service_refs.scratch_store = &scratch_store;

    auto run_tool = [&](const std::string& name, agent::Value arguments) {
      return feature_executor.execute_tool_call(
          agent::ToolCall{.id = name + ":call", .name = name, .arguments = std::move(arguments)},
          base_ctx);
    };

    // --- math.eval ---
    {
      auto r = run_tool("math.eval", agent::Value::object({{"expression", "2+3*4"}}));
      assert(r.ok);
      assert(r.output.find("14") != std::string::npos);

      r = run_tool("math.eval", agent::Value::object({{"expression", "sqrt(16)"}}));
      assert(r.ok);
      assert(r.output.find("\"value\": 4") != std::string::npos);

      r = run_tool("math.eval", agent::Value::object({{"expression", "sin(0)"}}));
      assert(r.ok);
      assert(r.output.find("\"value\": 0") != std::string::npos);

      r = run_tool("math.eval", agent::Value::object({{"expression", "pi"}}));
      assert(r.ok);
      assert(r.output.find("3.14159") != std::string::npos);

      r = run_tool("math.eval", agent::Value::object({{"expression", "pow(2,10)"}}));
      assert(r.ok);
      assert(r.output.find("1024") != std::string::npos);

      r = run_tool("math.eval", agent::Value::object({{"expression", "1/0"}}));
      assert(!r.ok);
    }

    // --- text.diff round-trip text.patch ---
    {
      const std::string before = "a\nb\nc\n";
      const std::string after = "a\nB\nc\n";
      auto diff = run_tool("text.diff",
                           agent::Value::object({{"before", before}, {"after", after}}));
      assert(diff.ok);
      assert(diff.output.find("hasChanges") != std::string::npos);
      assert(diff.output.find("true") != std::string::npos);
      // Extract the diff string from the result envelope.
      assert(diff.result.has_value());
      const auto* diff_value = std::get_if<agent::Value>(&*diff.result);
      assert(diff_value != nullptr);
      const std::string patch_text = diff_value->at("diff").as_string();
      assert(!patch_text.empty());

      auto patched = run_tool("text.patch",
                              agent::Value::object({{"source", before}, {"patch", patch_text}}));
      assert(patched.ok);
      assert(patched.result.has_value());
      const auto* patched_value = std::get_if<agent::Value>(&*patched.result);
      assert(patched_value != nullptr);
      assert(patched_value->at("appliedCleanly").as_bool());
      assert(patched_value->at("conflicts").as_array().empty());
      assert(patched_value->at("text").as_string() == after);
    }

    // --- scratch.set / get / list / delete ---
    {
      auto s1 = run_tool("scratch.set",
                         agent::Value::object({{"key", "alpha"}, {"value", "one"}}));
      assert(s1.ok);
      auto s2 = run_tool("scratch.set",
                         agent::Value::object({{"key", "beta"}, {"value", 2}}));
      assert(s2.ok);
      auto s3 = run_tool("scratch.set",
                         agent::Value::object(
                             {{"key", "gamma"}, {"value", agent::Value::object({{"nested", true}})}}));
      assert(s3.ok);

      auto listed = run_tool("scratch.list", agent::Value::object({}));
      assert(listed.ok);
      assert(listed.result.has_value());
      const auto* listed_value = std::get_if<agent::Value>(&*listed.result);
      assert(listed_value != nullptr);
      assert(listed_value->at("entries").as_array().size() == 3);

      auto got = run_tool("scratch.get", agent::Value::object({{"key", "alpha"}}));
      assert(got.ok);
      const auto* got_value = std::get_if<agent::Value>(&*got.result);
      assert(got_value != nullptr);
      assert(got_value->at("found").as_bool());
      assert(got_value->at("value").as_string() == "one");

      auto deleted = run_tool("scratch.delete", agent::Value::object({{"key", "beta"}}));
      assert(deleted.ok);
      const auto* deleted_value = std::get_if<agent::Value>(&*deleted.result);
      assert(deleted_value != nullptr);
      assert(deleted_value->at("deleted").as_bool());

      auto missing = run_tool("scratch.get", agent::Value::object({{"key", "beta"}}));
      assert(missing.ok);
      const auto* missing_value = std::get_if<agent::Value>(&*missing.result);
      assert(missing_value != nullptr);
      assert(!missing_value->at("found").as_bool());
    }

    // --- todo.create / update / complete / list ---
    {
      auto created = run_tool("todo.create",
                              agent::Value::object({
                                  {"items", agent::Value::array({
                                                agent::Value::object({{"text", "first task"}}),
                                                agent::Value::object({{"text", "second task"}}),
                                            })},
                              }));
      assert(created.ok);
      const auto* created_value = std::get_if<agent::Value>(&*created.result);
      assert(created_value != nullptr);
      const auto& created_items = created_value->at("items").as_array();
      assert(created_items.size() == 2);
      const std::string first_id = created_items.at(0).at("id").as_string();
      assert(!first_id.empty());

      auto completed = run_tool("todo.complete", agent::Value::object({{"id", first_id}}));
      assert(completed.ok);
      const auto* completed_value = std::get_if<agent::Value>(&*completed.result);
      assert(completed_value != nullptr);
      assert(completed_value->at("item").at("status").as_string() == "completed");

      auto only_completed = run_tool("todo.list", agent::Value::object({{"status", "completed"}}));
      assert(only_completed.ok);
      const auto* only_value = std::get_if<agent::Value>(&*only_completed.result);
      assert(only_value != nullptr);
      assert(only_value->at("items").as_array().size() == 1);

      auto all_listed = run_tool("todo.list", agent::Value::object({}));
      assert(all_listed.ok);
      const auto* all_value = std::get_if<agent::Value>(&*all_listed.result);
      assert(all_value != nullptr);
      assert(all_value->at("items").as_array().size() == 2);
    }

    // --- ScratchStore injection: custom backend records every call ---
    {
      struct MockScratchStore final : agent::ScratchStore {
        std::vector<std::tuple<std::string, std::string, agent::Value>> sets;
        std::vector<std::pair<std::string, std::string>> gets;
        std::vector<std::pair<std::string, std::string>> removes;
        std::vector<std::pair<std::string, std::string>> entry_calls;
        agent::InMemoryScratchStore delegate;
        std::optional<agent::Value> get(const std::string& session, const std::string& key) override {
          gets.emplace_back(session, key);
          return delegate.get(session, key);
        }
        void set(const std::string& session, const std::string& key, agent::Value value) override {
          sets.emplace_back(session, key, value);
          delegate.set(session, key, std::move(value));
        }
        bool remove(const std::string& session, const std::string& key) override {
          removes.emplace_back(session, key);
          return delegate.remove(session, key);
        }
        std::map<std::string, agent::Value> entries(const std::string& session,
                                                     const std::string& prefix = "") override {
          entry_calls.emplace_back(session, prefix);
          return delegate.entries(session, prefix);
        }
        void clear(const std::string& session) override { delegate.clear(session); }
      };

      MockScratchStore mock;
      agent::SessionMemory mock_session(agent::SessionMemoryOptions{.storage = {.session_id = "mock-scratch-session"}});
      agent::ToolExecutionContext mock_ctx;
      mock_ctx.service_refs.session = &mock_session;
      mock_ctx.service_refs.scratch_store = &mock;

      auto set_call = feature_executor.execute_tool_call(
          agent::ToolCall{.id = "scratch.set:mock", .name = "scratch.set",
                          .arguments = agent::Value::object({{"key", "tracked"}, {"value", 42}})},
          mock_ctx);
      assert(set_call.ok);
      assert(mock.sets.size() == 1);
      assert(std::get<1>(mock.sets[0]) == "tracked");
      assert(std::get<2>(mock.sets[0]).as_integer() == 42);

      auto get_call = feature_executor.execute_tool_call(
          agent::ToolCall{.id = "scratch.get:mock", .name = "scratch.get",
                          .arguments = agent::Value::object({{"key", "tracked"}})},
          mock_ctx);
      assert(get_call.ok);
      assert(mock.gets.size() == 1);
      assert(mock.gets[0].second == "tracked");
    }

    // --- FileScratchStore persists across instances ---
    {
      const auto temp_dir = std::filesystem::temp_directory_path() /
                            ("agent_scratch_smoke_" + agent::generate_uuid());
      std::filesystem::create_directories(temp_dir);

      {
        agent::FileScratchStore file_store(temp_dir);
        agent::SessionMemory file_session(agent::SessionMemoryOptions{.storage = {.session_id = "file-scratch-session"}});
        agent::ToolExecutionContext file_ctx;
        file_ctx.service_refs.session = &file_session;
        file_ctx.service_refs.scratch_store = &file_store;

        auto stored = feature_executor.execute_tool_call(
            agent::ToolCall{.id = "scratch.set:file", .name = "scratch.set",
                            .arguments = agent::Value::object(
                                {{"key", "persists"}, {"value", "across-restart"}})},
            file_ctx);
        assert(stored.ok);
      }

      // Fresh FileScratchStore over the same dir should see the value.
      {
        agent::FileScratchStore file_store2(temp_dir);
        agent::SessionMemory file_session(agent::SessionMemoryOptions{.storage = {.session_id = "file-scratch-session"}});
        agent::ToolExecutionContext file_ctx2;
        file_ctx2.service_refs.session = &file_session;
        file_ctx2.service_refs.scratch_store = &file_store2;

        auto reread = feature_executor.execute_tool_call(
            agent::ToolCall{.id = "scratch.get:file", .name = "scratch.get",
                            .arguments = agent::Value::object({{"key", "persists"}})},
            file_ctx2);
        assert(reread.ok);
        const auto* reread_value = std::get_if<agent::Value>(&*reread.result);
        assert(reread_value != nullptr);
        assert(reread_value->at("found").as_bool());
        assert(reread_value->at("value").as_string() == "across-restart");
      }

      std::filesystem::remove_all(temp_dir);
    }

    // --- Missing scratch_store yields a clearly-grep-able ConfigurationError ---
    {
      agent::SessionMemory bare_session(agent::SessionMemoryOptions{.storage = {.session_id = "bare-session"}});
      agent::ToolExecutionContext bare_ctx;
      bare_ctx.service_refs.session = &bare_session;
      // Deliberately no scratch_store.

      auto failed = feature_executor.execute_tool_call(
          agent::ToolCall{.id = "scratch.set:bare", .name = "scratch.set",
                          .arguments = agent::Value::object({{"key", "x"}, {"value", 1}})},
          bare_ctx);
      assert(!failed.ok);
      // The exception message is preserved in the structured output payload;
      // verify both grep-able tokens land there for users diagnosing missing
      // injection.
      assert(failed.output.find("Scratch") != std::string::npos);
      assert(failed.output.find("scratch_store") != std::string::npos);
    }

    // --- fs.* tools are confined to allowed roots (secure by default) ---
    {
      const auto root_dir = std::filesystem::temp_directory_path() /
                            ("agent_fs_confine_root_" + agent::generate_uuid());
      std::filesystem::create_directories(root_dir);

      // Explicit roots: writes inside the root succeed; traversal/escape is
      // rejected with a clearly-grep-able message, and reads outside are blocked.
      {
        agent::ToolRegistry confined_registry(
            agent::create_local_builtin_tools({root_dir.string()}));
        agent::ToolExecutor confined_executor(confined_registry);
        agent::ToolExecutionContext confined_ctx;

        const auto inside = (root_dir / "note.txt").string();
        auto ok_write = confined_executor.execute_tool_call(
            agent::ToolCall{.id = "fs.writeText:inside", .name = "fs.writeText",
                            .arguments = agent::Value::object(
                                {{"path", inside}, {"content", std::string("hi")}})},
            confined_ctx);
        assert(ok_write.ok);

        const auto escape = (root_dir / ".." / "escape.txt").string();
        auto bad_write = confined_executor.execute_tool_call(
            agent::ToolCall{.id = "fs.writeText:escape", .name = "fs.writeText",
                            .arguments = agent::Value::object(
                                {{"path", escape}, {"content", std::string("nope")}})},
            confined_ctx);
        assert(!bad_write.ok);
        assert(bad_write.output.find("outside the allowed") != std::string::npos);

        auto bad_read = confined_executor.execute_tool_call(
            agent::ToolCall{.id = "fs.readText:escape", .name = "fs.readText",
                            .arguments = agent::Value::object(
                                {{"path", (root_dir / ".." / ".." / "etc-passwd").string()}})},
            confined_ctx);
        assert(!bad_read.ok);
        assert(bad_read.output.find("outside the allowed") != std::string::npos);
      }

      // Secure by default (no args): confinement falls back to the process
      // working directory, so a write to a temp path outside cwd is rejected.
      {
        agent::ToolRegistry default_registry(agent::create_local_builtin_tools());
        agent::ToolExecutor default_executor(default_registry);
        agent::ToolExecutionContext default_ctx;
        const auto outside = (root_dir / "outside.txt").string();
        auto denied = default_executor.execute_tool_call(
            agent::ToolCall{.id = "fs.writeText:default", .name = "fs.writeText",
                            .arguments = agent::Value::object(
                                {{"path", outside}, {"content", std::string("no")}})},
            default_ctx);
        assert(!denied.ok);
        assert(denied.output.find("outside the allowed") != std::string::npos);
      }

      // Explicit escape hatch (allow_unconfined): writes anywhere succeed.
      {
        agent::ToolRegistry open_registry(agent::create_local_builtin_tools({}, true));
        agent::ToolExecutor open_executor(open_registry);
        agent::ToolExecutionContext open_ctx;
        const auto anywhere = (root_dir /
                               ("agent_fs_open_" + agent::generate_uuid() + ".txt"))
                                  .string();
        auto open_write = open_executor.execute_tool_call(
            agent::ToolCall{.id = "fs.writeText:open", .name = "fs.writeText",
                            .arguments = agent::Value::object(
                                {{"path", anywhere}, {"content", std::string("ok")}})},
            open_ctx);
        assert(open_write.ok);
        std::filesystem::remove(std::filesystem::path(anywhere));
      }

      std::filesystem::remove_all(root_dir);
    }

    // --- Server is fail-closed: no auth configured throws ConfigurationError ---
    {
      bool threw = false;
      try {
        agent::AgentServerApp locked_app(agent::AgentServerOptions{});
        (void)locked_app;
      } catch (const agent::ConfigurationError& e) {
        threw = true;
        assert(std::string(e.what()).find("no authentication configured") != std::string::npos);
      }
      assert(threw);

      // Explicit opt-in constructs cleanly.
      agent::AgentServerApp public_app(agent::AgentServerOptions{.allow_unauthenticated = true});
      (void)public_app;
    }
  }

  agent::JsonSchema envelope_schema;
  envelope_schema.type = agent::JsonSchemaType::Object;
  auto envelope_tool = agent::define_tool(agent::ToolDefinition{
      .name = "envelope.result",
      .description = "Return a rich tool result envelope.",
      .input_schema = envelope_schema,
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::ToolResultEnvelope{
            .content = std::vector<agent::MessageContentPart>{agent::text_part("visible envelope content")},
            .value = agent::Value::object({{"hidden", true}}),
            .metadata = agent::Value::object({{"source", "envelope"}}),
        };
      },
  });
  agent::ToolRegistry envelope_registry({envelope_tool});
  agent::EventBus envelope_events;
  std::vector<agent::FrameworkEvent> envelope_published_events;
  std::vector<std::string> envelope_event_categories;
  std::vector<std::string> envelope_audit_lifecycles;
  envelope_events.register_sink([&](const agent::FrameworkEvent& event) {
    envelope_published_events.push_back(event);
    envelope_event_categories.push_back(event.category);
    if (event.category == "tool.audit") {
      envelope_audit_lifecycles.push_back(event.payload.at("lifecycle").as_string());
    }
  });
  agent::ToolExecutor envelope_executor(
      envelope_registry,
      [](const agent::PermissionRequest&) {
        return agent::PermissionDecision{agent::PermissionDecisionKind::Allow, {}};
      },
      {},
      &envelope_events);
  auto envelope_result = envelope_executor.execute_tool_call(agent::ToolCall{
      .id = "envelope_1",
      .name = "envelope.result",
      .arguments = agent::Value::object({}),
  });
  assert(envelope_result.ok);
  assert(envelope_result.output.find("\"hidden\": true") != std::string::npos);
  assert(agent::extract_text_content(envelope_result.message.content) == "visible envelope content");
  assert(envelope_result.message.metadata.at("ok").as_bool());
  assert(envelope_result.message.metadata.at("source").as_string() == "envelope");
  assert(contains_string(envelope_event_categories, "permission.checked"));
  assert(contains_string(envelope_audit_lifecycles, "requested"));
  assert(contains_string(envelope_audit_lifecycles, "approved"));
  assert(contains_string(envelope_audit_lifecycles, "started"));
  assert(contains_string(envelope_audit_lifecycles, "completed"));
  const auto envelope_tool_completed = std::find_if(
      envelope_published_events.begin(), envelope_published_events.end(),
      [](const agent::FrameworkEvent& event) { return event.category == "tool.completed"; });
  assert(envelope_tool_completed != envelope_published_events.end());
  assert(!envelope_tool_completed->trace.trace_id.empty());
  assert(!envelope_tool_completed->trace.span_id.empty());
  auto unknown_envelope_result = envelope_executor.execute_tool_call(agent::ToolCall{
      .id = "envelope_unknown",
      .name = "envelope.missing",
      .arguments = agent::Value::object({}),
  });
  assert(!unknown_envelope_result.ok);
  assert(contains_string(envelope_audit_lifecycles, "failed"));

  agent::CancellationToken developer_tool_cancellation;
  bool developer_exec_called = false;
  agent::ToolRegistry developer_registry(agent::create_developer_builtin_tools(
      [&](const agent::DeveloperProcessRequest& request) {
        developer_exec_called = true;
        assert(request.command == "fixture-command");
        assert((request.args == std::vector<std::string>{"one", "two"}));
        assert(request.cwd == "/tmp");
        assert(request.cancellation == &developer_tool_cancellation);
        return agent::DeveloperProcessResult{
            .stdout_text = "developer stdout",
            .stderr_text = "developer stderr",
            .exit_code = 0,
        };
      }));
  agent::ToolExecutor developer_executor(developer_registry);
  // shell.exec declares a sandbox policy whose required capabilities include
  // ProcessAccess::ChildOnly. Install a permissive test sandbox so the
  // existing developer flow can run; the dedicated sandbox-contract block
  // below covers the rejection paths.
  struct PermissiveTestSandbox final : agent::Sandbox {
    agent::SandboxCapabilities max_capabilities() const noexcept override {
      return agent::SandboxCapabilities{
          .fs = agent::FsAccess::Unrestricted,
          .net = agent::NetAccess::Unrestricted,
          .process = agent::ProcessAccess::Unrestricted,
          .syscall = agent::SyscallAccess::Unrestricted,
      };
    }
    std::unique_ptr<agent::SandboxScope> enter(const agent::SandboxRequest& request) override {
      struct InlineScope final : agent::SandboxScope {
        explicit InlineScope(agent::SandboxRequest req) : req_(std::move(req)) {}
        const agent::SandboxRequest& request() const noexcept override { return req_; }
        agent::SandboxCapabilities effective() const noexcept override { return req_.capabilities; }
        agent::SandboxRequest req_;
      };
      return std::make_unique<InlineScope>(request);
    }
  };
  PermissiveTestSandbox permissive_sandbox;
  agent::ToolExecutionContext developer_shell_context;
  developer_shell_context.cancellation = &developer_tool_cancellation;
  developer_shell_context.service_refs.sandbox = &permissive_sandbox;
  auto developer_shell = developer_executor.execute_tool_call(agent::ToolCall{
      .id = "developer_shell_1",
      .name = "shell.exec",
      .arguments = agent::Value::object({
          {"command", "fixture-command"},
          {"args", agent::Value::array({"one", "two"})},
          {"cwd", "/tmp"},
      }),
  }, developer_shell_context);
  assert(developer_exec_called);
  assert(developer_shell.ok);
  assert(developer_shell.output.find("developer stdout") != std::string::npos);
  assert(developer_shell.output.find("developer stderr") != std::string::npos);
  agent::ToolRegistry developer_bundle_registry(agent::create_builtin_tools({"developer"}));
  assert(developer_bundle_registry.get("shell.exec") != nullptr);
  agent::ToolExecutor developer_bundle_executor(developer_bundle_registry);
  agent::ToolExecutionContext missing_executor_context;
  // Same trick — install permissive sandbox so we reach the executor check.
  missing_executor_context.service_refs.sandbox = &permissive_sandbox;
  auto missing_developer_executor = developer_bundle_executor.execute_tool_call(agent::ToolCall{
      .id = "developer_shell_missing_1",
      .name = "shell.exec",
      .arguments = agent::Value::object({{"command", "fixture-command"}}),
  }, missing_executor_context);
  assert(!missing_developer_executor.ok);
  assert(missing_developer_executor.error == "Tool \"shell.exec\" failed.");
  assert(missing_developer_executor.output.find("DeveloperProcessExecutor") != std::string::npos);

  // Sandbox contract enforcement — exercise the preferred → required → reject
  // ladder, capability axes, event emission, and serialization round-trips.
  {
    // (1) Empty policy with no sandbox installed → succeeds (None capabilities
    // trivially satisfy themselves).
    bool empty_policy_ok = true;
    try {
      auto effective = agent::enforce_sandbox_contract(agent::ToolSandboxPolicy{}, nullptr);
      (void)effective;
    } catch (...) {
      empty_policy_ok = false;
    }
    assert(empty_policy_ok);

    // Wire an event-bus-aware executor so we can capture contract events.
    agent::EventBus sandbox_events_bus;
    std::vector<agent::FrameworkEvent> sandbox_events;
    sandbox_events_bus.register_sink([&](const agent::FrameworkEvent& event) {
      if (event.category == "sandbox.entered" || event.category == "sandbox.exited" ||
          event.category == "sandbox.violated") {
        sandbox_events.push_back(event);
      }
    });
    agent::ToolExecutor sandbox_audited_executor(developer_bundle_registry, {}, {},
                                                 &sandbox_events_bus);

    // (2) shell.exec's required policy, sandbox = nullptr → rejected.
    sandbox_events.clear();
    agent::ToolExecutionContext no_sandbox_ctx;
    auto no_sandbox_result = sandbox_audited_executor.execute_tool_call(agent::ToolCall{
        .id = "sandbox_contract_none",
        .name = "shell.exec",
        .arguments = agent::Value::object({{"command", "fixture-command"}}),
    }, no_sandbox_ctx);
    assert(!no_sandbox_result.ok);
    assert(no_sandbox_result.output.find("sandbox") != std::string::npos);
    bool saw_violated_no_sandbox = false;
    for (const auto& ev : sandbox_events) {
      if (ev.category == "sandbox.violated") {
        saw_violated_no_sandbox = true;
        const auto& payload = ev.payload.as_object();
        assert(payload.count("required") == 1);
        assert(payload.count("provided") == 1);
        assert(payload.count("reason") == 1);
        assert(payload.at("provided").is_null());
      }
    }
    assert(saw_violated_no_sandbox);

    // (3) shell.exec's required policy, sandbox = NoopSandbox (all None) → rejected.
    sandbox_events.clear();
    agent::NoopSandbox noop_sandbox;
    {
      const auto caps = noop_sandbox.max_capabilities();
      assert(caps.fs == agent::FsAccess::None);
      assert(caps.net == agent::NetAccess::None);
      assert(caps.process == agent::ProcessAccess::None);
    }
    agent::ToolExecutionContext noop_ctx;
    noop_ctx.service_refs.sandbox = &noop_sandbox;
    auto noop_result = sandbox_audited_executor.execute_tool_call(agent::ToolCall{
        .id = "sandbox_contract_noop",
        .name = "shell.exec",
        .arguments = agent::Value::object({{"command", "fixture-command"}}),
    }, noop_ctx);
    assert(!noop_result.ok);
    assert(noop_result.output.find("satisfy") != std::string::npos ||
           noop_result.output.find("below") != std::string::npos);

    // (4) shell.exec's required policy, sandbox = PermissiveTestSandbox →
    // succeeds end-to-end. Verify sandbox.entered + sandbox.exited fire.
    sandbox_events.clear();
    agent::ToolExecutionContext permissive_ctx;
    permissive_ctx.cancellation = &developer_tool_cancellation;
    permissive_ctx.service_refs.sandbox = &permissive_sandbox;
    developer_exec_called = false;
    auto permissive_result = sandbox_audited_executor.execute_tool_call(agent::ToolCall{
        .id = "sandbox_contract_permissive",
        .name = "shell.exec",
        .arguments = agent::Value::object({
            {"command", "fixture-command"},
            {"args", agent::Value::array({"only"})},
            {"cwd", "/tmp"},
        }),
    }, permissive_ctx);
    // shell.exec invokes the bundle's default executor which throws because
    // no DeveloperProcessExecutor was wired into create_builtin_tools. We
    // only care here that the contract accepted the call and that the scope
    // events fired around the invocation; use the previously-tested
    // `developer_executor` (which has the real exec) for the success path.
    assert(permissive_result.error.find("sandbox") == std::string::npos);
    bool saw_entered_required = false;
    bool saw_exited = false;
    for (const auto& ev : sandbox_events) {
      if (ev.category == "sandbox.entered") {
        saw_entered_required = true;
        const auto& payload = ev.payload.as_object();
        assert(payload.count("request") == 1);
        assert(payload.count("mode") == 1);
        assert(payload.at("mode").as_string() == "required");
      } else if (ev.category == "sandbox.exited") {
        saw_exited = true;
        const auto& payload = ev.payload.as_object();
        assert(payload.count("durationMs") == 1);
      }
    }
    assert(saw_entered_required);
    assert(saw_exited);

    // Run the same scenario against the original developer_executor (with a
    // real process executor wired in) so we observe a successful invocation
    // and `developer_exec_called == true`.
    developer_exec_called = false;
    agent::ToolExecutionContext live_permissive_ctx;
    live_permissive_ctx.cancellation = &developer_tool_cancellation;
    live_permissive_ctx.service_refs.sandbox = &permissive_sandbox;
    auto live_permissive_result = developer_executor.execute_tool_call(agent::ToolCall{
        .id = "sandbox_contract_live_permissive",
        .name = "shell.exec",
        .arguments = agent::Value::object({
            {"command", "fixture-command"},
            {"args", agent::Value::array({"one", "two"})},
            {"cwd", "/tmp"},
        }),
    }, live_permissive_ctx);
    assert(live_permissive_result.ok);
    assert(developer_exec_called);

    // (5) Preferred fallback.
    //   (5a) PermissiveTestSandbox satisfies fully_open → effective is
    //        fully_open (preferred).
    sandbox_events.clear();
    agent::ToolSandboxPolicy preferred_policy{
        .required = agent::sandbox_presets::shell_safe(),
        .preferred = agent::sandbox_presets::fully_open(),
    };
    auto pref_effective = agent::enforce_sandbox_contract(
        preferred_policy, &permissive_sandbox, &sandbox_events_bus);
    assert(pref_effective.capabilities.fs == agent::FsAccess::Unrestricted);
    assert(pref_effective.capabilities.net == agent::NetAccess::Unrestricted);
    assert(pref_effective.capabilities.process == agent::ProcessAccess::Unrestricted);
    bool saw_preferred_mode = false;
    for (const auto& ev : sandbox_events) {
      if (ev.category == "sandbox.entered" &&
          ev.payload.as_object().at("mode").as_string() == "preferred") {
        saw_preferred_mode = true;
      }
    }
    assert(saw_preferred_mode);

    //   (5b) Sandbox satisfies shell_safe but not fully_open → effective is
    //        shell_safe (required).
    struct ShellSafeOnlySandbox final : agent::Sandbox {
      agent::SandboxCapabilities max_capabilities() const noexcept override {
        return agent::SandboxCapabilities{
            .fs = agent::FsAccess::ReadWrite,
            .net = agent::NetAccess::None,
            .process = agent::ProcessAccess::ChildOnly,
            .syscall = agent::SyscallAccess::Standard,
        };
      }
      std::unique_ptr<agent::SandboxScope> enter(const agent::SandboxRequest& request) override {
        struct InlineScope final : agent::SandboxScope {
          explicit InlineScope(agent::SandboxRequest req) : req_(std::move(req)) {}
          const agent::SandboxRequest& request() const noexcept override { return req_; }
          agent::SandboxCapabilities effective() const noexcept override {
            return req_.capabilities;
          }
          agent::SandboxRequest req_;
        };
        return std::make_unique<InlineScope>(request);
      }
    };
    ShellSafeOnlySandbox shell_safe_sandbox;
    auto required_effective = agent::enforce_sandbox_contract(
        preferred_policy, &shell_safe_sandbox);
    assert(required_effective.capabilities.fs == agent::FsAccess::ReadWrite);
    assert(required_effective.capabilities.process == agent::ProcessAccess::ChildOnly);
    assert(required_effective.capabilities.net == agent::NetAccess::None);

    // (6) Round-trip a SandboxRequest through Value.
    agent::SandboxRequest roundtrip_input;
    roundtrip_input.capabilities = agent::SandboxCapabilities{
        .fs = agent::FsAccess::ReadWrite,
        .net = agent::NetAccess::Allowlist,
        .process = agent::ProcessAccess::ChildOnly,
        .syscall = agent::SyscallAccess::Standard,
    };
    roundtrip_input.fs_read_paths = {std::filesystem::path("/tmp/in")};
    roundtrip_input.fs_write_paths = {std::filesystem::path("/tmp/out")};
    roundtrip_input.net_hosts = {"example.com", "127.0.0.1"};
    roundtrip_input.allowed_subcommands = {"git status"};
    auto roundtrip_output = agent::sandbox_request_from_value(
        agent::sandbox_request_to_value(roundtrip_input));
    assert(roundtrip_output.capabilities.fs == roundtrip_input.capabilities.fs);
    assert(roundtrip_output.capabilities.net == roundtrip_input.capabilities.net);
    assert(roundtrip_output.capabilities.process == roundtrip_input.capabilities.process);
    assert(roundtrip_output.capabilities.syscall == roundtrip_input.capabilities.syscall);
    assert(roundtrip_output.fs_read_paths == roundtrip_input.fs_read_paths);
    assert(roundtrip_output.fs_write_paths == roundtrip_input.fs_write_paths);
    assert(roundtrip_output.net_hosts == roundtrip_input.net_hosts);
    assert(roundtrip_output.allowed_subcommands == roundtrip_input.allowed_subcommands);

    // (7) Axis ↔ string round-trip for all four enums.
    assert(agent::to_string(agent::FsAccess::ReadWrite) == "read-write");
    assert(agent::fs_access_from_string("read-write") == agent::FsAccess::ReadWrite);
    assert(agent::fs_access_from_string("unrestricted") == agent::FsAccess::Unrestricted);
    assert(agent::to_string(agent::NetAccess::Allowlist) == "allowlist");
    assert(agent::net_access_from_string("loopback") == agent::NetAccess::Loopback);
    assert(agent::to_string(agent::ProcessAccess::ChildOnly) == "child-only");
    assert(agent::process_access_from_string("unrestricted") == agent::ProcessAccess::Unrestricted);
    assert(agent::to_string(agent::SyscallAccess::Minimal) == "minimal");
    assert(agent::syscall_access_from_string("standard") == agent::SyscallAccess::Standard);
    assert(agent::syscall_access_from_string("garbled", agent::SyscallAccess::Unrestricted) ==
           agent::SyscallAccess::Unrestricted);
  }

  agent::CancellationToken browser_tool_cancellation;
  std::vector<agent::BrowserRenderRequest> browser_requests;
  agent::BrowserCallbackRenderer browser_renderer(
      [&](const agent::BrowserRenderRequest& request) {
        browser_requests.push_back(request);
        agent::BrowserRenderResult render_result;
        render_result.url = request.url;
        render_result.title = "Fixture Browser";
        render_result.html = "<html><body><main id=\"main\">rendered browser text</main></body></html>";
        render_result.text = request.selector.empty() ? "rendered browser text" : "selected browser text";
        if (request.screenshot) {
          render_result.screenshot_base64 = "c2NyZWVuc2hvdA==";
        }
        return render_result;
      },
      agent::BrowserRendererMetadata{.name = "fixture-browser", .tier = "test"});
  agent::ToolRegistry browser_registry(agent::create_browser_builtin_tools(&browser_renderer));
  assert(browser_registry.get("browser.render") != nullptr);
  assert(browser_registry.get("browser.extract") != nullptr);
  assert(browser_registry.get("browser.screenshot") != nullptr);
  agent::ToolExecutor browser_executor(browser_registry);
  agent::ToolExecutionContext browser_context;
  browser_context.cancellation = &browser_tool_cancellation;
  auto browser_render = browser_executor.execute_tool_call(agent::ToolCall{
      .id = "browser_render_1",
      .name = "browser.render",
      .arguments = agent::Value::object({
          {"url", "https://example.test/app"},
          {"waitUntil", "load"},
          {"timeoutMs", 1000},
      }),
  }, browser_context);
  assert(browser_render.ok);
  assert(browser_render.output.find("Fixture Browser") != std::string::npos);
  assert(browser_requests.back().url == "https://example.test/app");
  assert(browser_requests.back().wait_until == "load");
  assert(browser_requests.back().timeout_ms == 1000);
  assert(browser_requests.back().cancellation == &browser_tool_cancellation);
  auto browser_extract = browser_executor.execute_tool_call(agent::ToolCall{
      .id = "browser_extract_1",
      .name = "browser.extract",
      .arguments = agent::Value::object({
          {"url", "https://example.test/app"},
          {"selector", "#main"},
      }),
  });
  assert(browser_extract.ok);
  assert(browser_extract.output.find("selected browser text") != std::string::npos);
  assert(browser_requests.back().selector == "#main");
  auto browser_screenshot = browser_executor.execute_tool_call(agent::ToolCall{
      .id = "browser_screenshot_1",
      .name = "browser.screenshot",
      .arguments = agent::Value::object({{"url", "https://example.test/app"}}),
  });
  assert(browser_screenshot.ok);
  assert(browser_screenshot.output.find("c2NyZWVuc2hvdA==") != std::string::npos);
  assert(browser_requests.back().screenshot);
  agent::ToolRegistry browser_bundle_registry(agent::create_builtin_tools({"browser"}));
  assert(browser_bundle_registry.get("browser.render") != nullptr);
  agent::ToolExecutor browser_bundle_executor(browser_bundle_registry);
  auto missing_browser_renderer = browser_bundle_executor.execute_tool_call(agent::ToolCall{
      .id = "browser_missing_1",
      .name = "browser.render",
      .arguments = agent::Value::object({{"url", "https://example.test/app"}}),
  });
  assert(!missing_browser_renderer.ok);
  assert(missing_browser_renderer.error == "Tool \"browser.render\" failed.");
  assert(missing_browser_renderer.output.find("Browser renderer is not configured") != std::string::npos);
  agent::ToolExecutionContext browser_service_context;
  browser_service_context.service_refs.browser_renderer = &browser_renderer;
  auto browser_service_render = browser_bundle_executor.execute_tool_call(agent::ToolCall{
      .id = "browser_service_1",
      .name = "browser.render",
      .arguments = agent::Value::object({{"url", "https://example.test/app"}}),
  }, browser_service_context);
  assert(browser_service_render.ok);
  assert(browser_service_render.output.find("Fixture Browser") != std::string::npos);

  auto transport_pair = agent::create_in_memory_tool_client_transport_pair();
  agent::EventBus tool_client_events;
  std::vector<std::string> tool_client_lifecycles;
  tool_client_events.register_sink([&](const agent::FrameworkEvent& event) {
    if (event.category == "tool-client.audit") {
      tool_client_lifecycles.push_back(event.payload.at("lifecycle").as_string());
    }
  });
  agent::ToolClientBroker broker(
      transport_pair.server,
      {},
      agent::ToolClientSecurityPolicy{
          .allowed_client_ids = {"client-*"},
          .allowed_tools = {"remote.echo"},
          .allowed_capabilities = {"remote.exec"},
      },
      &tool_client_events);
  int remote_echo_calls = 0;
  agent::JsonSchema remote_echo_schema;
  remote_echo_schema.type = agent::JsonSchemaType::Object;
  remote_echo_schema.required = {"text"};
  remote_echo_schema.properties["text"].type = agent::JsonSchemaType::String;
  auto remote_echo = agent::define_tool(agent::ToolDefinition{
      .name = "remote.echo",
      .description = "Remote echo.",
      .input_schema = remote_echo_schema,
      .capabilities = {"remote.exec"},
      .risk_level = agent::ToolRiskLevel::Low,
      .execute = [&](const agent::Value& input, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        remote_echo_calls += 1;
        return agent::Value::object({{"text", "remote:" + input.at("text").as_string()}});
      },
  });
  agent::ToolClientRuntime runtime(agent::ToolClientRuntimeOptions{
      .client_id = "client-one",
      .tools = {remote_echo},
      .transport = transport_pair.client,
      .capabilities = {"remote.exec"},
      .event_bus = &tool_client_events,
  });
  runtime.start();
  assert(broker.registry().get("client-one"));
  auto remote_call_result = broker.call_tool(agent::ToolClientBrokerCallOptions{
      .tool_call = agent::ToolCall{
          .id = "remote_call_1",
          .name = "remote.echo",
          .arguments = agent::Value::object({{"text", "hello"}}),
      },
      .idempotency_key = "remote-key-1",
  });
  assert(remote_call_result.at("text").as_string() == "remote:hello");
  auto cached_remote_call_result = broker.call_tool(agent::ToolClientBrokerCallOptions{
      .tool_call = agent::ToolCall{
          .id = "remote_call_1_retry",
          .name = "remote.echo",
          .arguments = agent::Value::object({{"text", "ignored"}}),
      },
      .idempotency_key = "remote-key-1",
  });
  assert(cached_remote_call_result.at("text").as_string() == "remote:hello");
  assert(remote_echo_calls == 1);
  bool denied_remote_tool = false;
  try {
    (void)broker.call_tool(agent::ToolClientBrokerCallOptions{
        .tool_call = agent::ToolCall{
            .id = "remote_call_denied",
            .name = "remote.denied",
            .arguments = agent::Value::object({}),
        },
    });
  } catch (const agent::AgentFrameworkError& error) {
    denied_remote_tool = std::string(error.what()).find("not allowed") != std::string::npos;
  }
  assert(denied_remote_tool);
  runtime.stop("done");
  assert(!broker.registry().get("client-one"));
  assert(std::find(tool_client_lifecycles.begin(), tool_client_lifecycles.end(), "client.registered") !=
         tool_client_lifecycles.end());
  assert(std::find(tool_client_lifecycles.begin(), tool_client_lifecycles.end(), "call.completed") !=
         tool_client_lifecycles.end());

  auto socket_server = std::make_shared<FakeSocketIoToolClientServer>();
  auto socket_server_side = std::make_shared<FakeSocketIoToolClientSocket>();
  auto socket_client_side = std::make_shared<FakeSocketIoToolClientSocket>();
  socket_server_side->set_peer(socket_client_side);
  socket_client_side->set_peer(socket_server_side);
  auto socket_server_transport = std::make_shared<agent::SocketIoToolClientServerTransport>(socket_server);
  auto socket_client_transport = std::make_shared<agent::SocketIoToolClientClientTransport>(socket_client_side);
  socket_server->connect(socket_server_side);
  agent::ToolClientBroker socket_broker(
      socket_server_transport,
      {},
      agent::ToolClientSecurityPolicy{
          .allowed_client_ids = {"socket-client-*"},
          .allowed_tools = {"remote.echo"},
          .allowed_capabilities = {"remote.exec"},
      });
  int socket_remote_echo_calls = 0;
  auto socket_remote_echo = agent::define_tool(agent::ToolDefinition{
      .name = "remote.echo",
      .description = "Socket remote echo.",
      .input_schema = remote_echo_schema,
      .capabilities = {"remote.exec"},
      .risk_level = agent::ToolRiskLevel::Low,
      .execute = [&](const agent::Value& input, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        socket_remote_echo_calls += 1;
        return agent::Value::object({{"text", "socket:" + input.at("text").as_string()}});
      },
  });
  agent::ToolClientRuntime socket_runtime(agent::ToolClientRuntimeOptions{
      .client_id = "socket-client-one",
      .tools = {socket_remote_echo},
      .transport = socket_client_transport,
      .capabilities = {"remote.exec"},
  });
  socket_runtime.start();
  assert(socket_broker.registry().get("socket-client-one"));
  assert(socket_server_transport->get_socket("socket-client-one") == socket_server_side);
  auto socket_call_result = socket_broker.call_tool(agent::ToolClientBrokerCallOptions{
      .tool_call = agent::ToolCall{
          .id = "socket_remote_call_1",
          .name = "remote.echo",
          .arguments = agent::Value::object({{"text", "hello"}}),
      },
  });
  assert(socket_call_result.at("text").as_string() == "socket:hello");
  assert(socket_remote_echo_calls == 1);
  assert(!socket_server_side->emitted_events.empty());
  assert(socket_server_side->emitted_events.front() == agent::DEFAULT_TOOL_CLIENT_SOCKET_IO_EVENT);
  socket_client_side->disconnect("test disconnect");
  assert(!socket_broker.registry().get("socket-client-one"));
  assert(!socket_server_transport->get_socket("socket-client-one"));
  socket_runtime.stop("done");
  socket_broker.stop();
  socket_server_transport->stop();

  {
    agent::ToolClientRegistry concurrent_client_registry;
    std::atomic<int> concurrent_registry_ready{0};
    std::atomic<bool> concurrent_registry_start{false};
    constexpr int concurrent_registry_thread_count = 8;
    std::vector<std::thread> concurrent_registry_workers;
    for (int thread_index = 0; thread_index < concurrent_registry_thread_count; ++thread_index) {
      concurrent_registry_workers.emplace_back([&, thread_index]() {
        concurrent_registry_ready.fetch_add(1);
        while (!concurrent_registry_start.load()) {
          std::this_thread::yield();
        }
        const auto client_id = "concurrent-client-" + std::to_string(thread_index);
        concurrent_client_registry.register_client(client_id, {remote_echo.descriptor()}, {"remote.exec"});
        assert(concurrent_client_registry.touch(client_id));
        auto client = concurrent_client_registry.find_client_for_tool("remote.echo", client_id);
        assert(client);
        assert(client->client_id == client_id);
        assert(!concurrent_client_registry.list().empty());
      });
    }
    while (concurrent_registry_ready.load() < concurrent_registry_thread_count) {
      std::this_thread::yield();
    }
    concurrent_registry_start.store(true);
    for (auto& worker : concurrent_registry_workers) {
      worker.join();
    }
    assert(concurrent_client_registry.list().size() == static_cast<std::size_t>(concurrent_registry_thread_count));
    auto copied_client_registry = concurrent_client_registry;
    assert(copied_client_registry.list().size() == static_cast<std::size_t>(concurrent_registry_thread_count));
  }

  {
    auto concurrent_pair = agent::create_in_memory_tool_client_transport_pair();
    std::atomic<int> concurrent_transport_ready{0};
    std::atomic<bool> concurrent_transport_start{false};
    std::atomic<int> concurrent_transport_sent{0};
    std::atomic<int> concurrent_transport_received{0};
    constexpr int concurrent_transport_thread_count = 6;
    std::vector<std::thread> concurrent_transport_workers;
    for (int thread_index = 0; thread_index < concurrent_transport_thread_count; ++thread_index) {
      concurrent_transport_workers.emplace_back([&, thread_index]() {
        auto subscription = concurrent_pair.server->subscribe(
            [&](const agent::ToolClientMessage& message) {
              if (message.type == agent::ToolClientMessageType::Heartbeat &&
                  message.client_id.find("transport-concurrent-") == 0) {
                concurrent_transport_received.fetch_add(1);
              }
            });
        concurrent_transport_ready.fetch_add(1);
        while (!concurrent_transport_start.load()) {
          std::this_thread::yield();
        }
        auto message = agent::ToolClientMessage{};
        message.type = agent::ToolClientMessageType::Heartbeat;
        message.message_id = agent::generate_uuid();
        message.timestamp = agent::now_iso8601();
        message.client_id = "transport-concurrent-" + std::to_string(thread_index);
        concurrent_pair.client->send(message);
        concurrent_transport_sent.fetch_add(1);
        while (concurrent_transport_sent.load() < concurrent_transport_thread_count) {
          std::this_thread::yield();
        }
        concurrent_pair.server->unsubscribe(subscription);
      });
    }
    while (concurrent_transport_ready.load() < concurrent_transport_thread_count) {
      std::this_thread::yield();
    }
    concurrent_transport_start.store(true);
    for (auto& worker : concurrent_transport_workers) {
      worker.join();
    }
    assert(concurrent_transport_received.load() ==
           concurrent_transport_thread_count * concurrent_transport_thread_count);
  }

  {
    auto concurrent_socket_server = std::make_shared<FakeSocketIoToolClientServer>();
    auto concurrent_socket_transport =
        std::make_shared<agent::SocketIoToolClientServerTransport>(concurrent_socket_server);
    std::atomic<int> concurrent_socket_registered{0};
    std::atomic<int> concurrent_socket_tool_calls{0};
    auto concurrent_socket_subscription = concurrent_socket_transport->subscribe(
        [&](const agent::ToolClientMessage& message) {
          if (message.type == agent::ToolClientMessageType::Register) {
            concurrent_socket_registered.fetch_add(1);
          }
        });
    constexpr int concurrent_socket_thread_count = 6;
    std::vector<std::thread> concurrent_socket_workers;
    for (int thread_index = 0; thread_index < concurrent_socket_thread_count; ++thread_index) {
      concurrent_socket_workers.emplace_back([&, thread_index]() {
        auto server_side = std::make_shared<FakeSocketIoToolClientSocket>();
        auto client_side = std::make_shared<FakeSocketIoToolClientSocket>();
        server_side->set_peer(client_side);
        client_side->set_peer(server_side);
        concurrent_socket_server->connect(server_side);
        agent::SocketIoToolClientClientTransport client_transport(client_side);
        const auto client_id = "socket-concurrent-" + std::to_string(thread_index);
        auto client_subscription = client_transport.subscribe(
            [&](const agent::ToolClientMessage& message) {
              if (message.type == agent::ToolClientMessageType::ToolCall) {
                concurrent_socket_tool_calls.fetch_add(1);
              }
            });
        auto registration = agent::ToolClientMessage{};
        registration.type = agent::ToolClientMessageType::Register;
        registration.message_id = agent::generate_uuid();
        registration.timestamp = agent::now_iso8601();
        registration.client_id = client_id;
        registration.tools = {remote_echo.descriptor()};
        registration.capabilities = {"remote.exec"};
        client_transport.send(registration);
        assert(concurrent_socket_transport->get_socket(client_id) == server_side);
        auto call = agent::ToolClientMessage{};
        call.type = agent::ToolClientMessageType::ToolCall;
        call.message_id = agent::generate_uuid();
        call.timestamp = agent::now_iso8601();
        call.client_id = client_id;
        call.tool_call = agent::ToolCall{
            .id = "socket_concurrent_call_" + std::to_string(thread_index),
            .name = "remote.echo",
            .arguments = agent::Value::object({{"text", "parallel"}}),
        };
        concurrent_socket_transport->send(call);
        client_transport.unsubscribe(client_subscription);
        client_side->disconnect("parallel done");
        assert(!concurrent_socket_transport->get_socket(client_id));
      });
    }
    for (auto& worker : concurrent_socket_workers) {
      worker.join();
    }
    concurrent_socket_transport->unsubscribe(concurrent_socket_subscription);
    concurrent_socket_transport->stop();
    assert(concurrent_socket_registered.load() == concurrent_socket_thread_count);
    assert(concurrent_socket_tool_calls.load() == concurrent_socket_thread_count);
  }

  auto async_pair = agent::create_in_memory_tool_client_transport_pair();
  agent::ToolClientBroker async_broker(async_pair.server);
  async_broker.registry().register_client("async-client-one", {remote_echo.descriptor()}, {"remote.exec"});
  std::thread delayed_tool_result;
  auto async_subscription = async_pair.client->subscribe(
      [&](const agent::ToolClientMessage& message) {
        if (message.type != agent::ToolClientMessageType::ToolCall) {
          return;
        }
        delayed_tool_result = std::thread([client = async_pair.client, message]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          agent::ToolClientMessage reply;
          reply.type = agent::ToolClientMessageType::ToolResult;
          reply.client_id = message.client_id;
          reply.message_id = agent::generate_uuid();
          reply.timestamp = agent::now_iso8601();
          reply.request_id = message.message_id;
          reply.tool_call_id = message.tool_call.id;
          reply.result = agent::Value::object({{"text", "async:" + message.tool_call.arguments.at("text").as_string()}});
          client->send(reply);
        });
      });
  auto async_call_result = async_broker.call_tool(agent::ToolClientBrokerCallOptions{
      .tool_call = agent::ToolCall{
          .id = "async_remote_call_1",
          .name = "remote.echo",
          .arguments = agent::Value::object({{"text", "hello"}}),
      },
      .timeout_ms = 500,
  });
  if (delayed_tool_result.joinable()) {
    delayed_tool_result.join();
  }
  async_pair.client->unsubscribe(async_subscription);
  assert(async_call_result.at("text").as_string() == "async:hello");
  assert(async_broker.pending_count() == 0);

  auto timeout_pair = agent::create_in_memory_tool_client_transport_pair();
  agent::ToolClientBroker timeout_broker(timeout_pair.server);
  timeout_broker.registry().register_client("timeout-client-one", {remote_echo.descriptor()}, {"remote.exec"});
  std::atomic<bool> timeout_cancel_seen{false};
  auto timeout_subscription = timeout_pair.client->subscribe(
      [&](const agent::ToolClientMessage& message) {
        if (message.type == agent::ToolClientMessageType::Cancel &&
            message.reason.find("timed out") != std::string::npos) {
          timeout_cancel_seen = true;
        }
      });
  bool async_timeout_threw = false;
  try {
    (void)timeout_broker.call_tool(agent::ToolClientBrokerCallOptions{
        .tool_call = agent::ToolCall{
            .id = "timeout_remote_call_1",
            .name = "remote.echo",
            .arguments = agent::Value::object({{"text", "slow"}}),
        },
        .timeout_ms = 20,
    });
  } catch (const agent::AgentFrameworkError& error) {
    async_timeout_threw = std::string(error.what()).find("timed out") != std::string::npos;
  }
  timeout_pair.client->unsubscribe(timeout_subscription);
  assert(async_timeout_threw);
  assert(timeout_cancel_seen.load());
  assert(timeout_broker.pending_count() == 0);

  auto runtime_cancel_pair = agent::create_in_memory_tool_client_transport_pair();
  std::atomic<bool> runtime_cancel_tool_started{false};
  std::atomic<bool> runtime_cancel_observed{false};
  std::atomic<bool> runtime_cancel_reply_seen{false};
  std::string runtime_cancel_error;
  auto runtime_cancel_reply_subscription = runtime_cancel_pair.server->subscribe(
      [&](const agent::ToolClientMessage& message) {
        if (message.type == agent::ToolClientMessageType::ToolError &&
            message.request_id == "runtime_cancel_request") {
          runtime_cancel_error = message.error;
          runtime_cancel_reply_seen = true;
        }
      });
  auto cancellable_remote_tool = agent::define_tool(agent::ToolDefinition{
      .name = "remote.cancel",
      .description = "Cooperatively cancellable remote tool.",
      .input_schema = agent::JsonSchema{.type = agent::JsonSchemaType::Object},
      .capabilities = {"remote.exec"},
      .risk_level = agent::ToolRiskLevel::Low,
      .execute = [&](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        assert(context.cancellation != nullptr);
        runtime_cancel_tool_started = true;
        for (int attempt = 0; attempt < 100; ++attempt) {
          if (context.cancellation->cancelled()) {
            runtime_cancel_observed = true;
            context.cancellation->throw_if_cancelled(agent::ExecutionTarget::Tool);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return agent::Value::object({{"finished", true}});
      },
  });
  agent::ToolClientRuntime runtime_cancel_runtime(agent::ToolClientRuntimeOptions{
      .client_id = "runtime-cancel-client",
      .tools = {cancellable_remote_tool},
      .transport = runtime_cancel_pair.client,
      .capabilities = {"remote.exec"},
  });
  runtime_cancel_runtime.start();
  agent::ToolClientMessage runtime_cancel_call;
  runtime_cancel_call.type = agent::ToolClientMessageType::ToolCall;
  runtime_cancel_call.message_id = "runtime_cancel_request";
  runtime_cancel_call.client_id = "runtime-cancel-client";
  runtime_cancel_call.timestamp = agent::now_iso8601();
  runtime_cancel_call.tool_call = agent::ToolCall{
      .id = "runtime_cancel_tool_call",
      .name = "remote.cancel",
      .arguments = agent::Value::object({}),
  };
  std::thread runtime_cancel_thread([&]() {
    runtime_cancel_pair.server->send(runtime_cancel_call);
  });
  for (int attempt = 0; attempt < 100 && !runtime_cancel_tool_started.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(runtime_cancel_tool_started.load());
  agent::ToolClientMessage runtime_cancel_message;
  runtime_cancel_message.type = agent::ToolClientMessageType::Cancel;
  runtime_cancel_message.message_id = "runtime_cancel_message";
  runtime_cancel_message.client_id = "runtime-cancel-client";
  runtime_cancel_message.timestamp = agent::now_iso8601();
  runtime_cancel_message.request_id = "runtime_cancel_request";
  runtime_cancel_message.reason = "broker requested runtime cancellation";
  runtime_cancel_pair.server->send(runtime_cancel_message);
  if (runtime_cancel_thread.joinable()) {
    runtime_cancel_thread.join();
  }
  assert(runtime_cancel_observed.load());
  assert(runtime_cancel_reply_seen.load());
  assert(runtime_cancel_error.find("broker requested runtime cancellation") != std::string::npos);
  runtime_cancel_pair.server->unsubscribe(runtime_cancel_reply_subscription);
  runtime_cancel_runtime.stop("done");

  auto shell_tool = agent::define_tool(agent::ToolDefinition{
      .name = "shell.exec",
      .description = "Execute a process.",
      .capabilities = {"process.exec"},
      .risk_level = agent::ToolRiskLevel::High,
      .tags = {"developer"},
      .bundle = "developer",
      .builtin = true,
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value("ok");
      },
  });
  agent::ToolRegistry permission_registry({shell_tool});
  auto capability_policy = agent::create_capability_policy(agent::CapabilityPermissionPolicyConfig{
      .allow = {"process.exec"},
      .high_risk_mode = agent::PermissionDecisionKind::Ask,
  });
  agent::EventBus denied_permission_events_bus;
  std::vector<agent::FrameworkEvent> denied_permission_events;
  denied_permission_events_bus.register_sink([&](const agent::FrameworkEvent& event) {
    denied_permission_events.push_back(event);
  });
  agent::ToolExecutor denied_permission_executor(permission_registry, capability_policy, {},
                                                &denied_permission_events_bus);
  auto denied_permission_result = denied_permission_executor.execute_tool_call(agent::ToolCall{
      .id = "permission_1",
      .name = "shell.exec",
      .arguments = agent::Value::object({}),
  });
  assert(!denied_permission_result.ok);
  assert(denied_permission_result.error == "Tool \"shell.exec\" is not permitted.");
  assert(denied_permission_result.output.find("not permitted") != std::string::npos);
  const auto denied_permission_event = std::find_if(
      denied_permission_events.begin(), denied_permission_events.end(),
      [](const agent::FrameworkEvent& event) { return event.category == "permission.denied"; });
  assert(denied_permission_event != denied_permission_events.end());
  assert(denied_permission_event->payload.at("reason").as_string().find("Approval handler") != std::string::npos);
  const auto denied_audit_event = std::find_if(
      denied_permission_events.begin(), denied_permission_events.end(),
      [](const agent::FrameworkEvent& event) {
        return event.category == "tool.audit" && event.payload.at("lifecycle").as_string() == "denied";
      });
  assert(denied_audit_event != denied_permission_events.end());
  assert(denied_audit_event->payload.at("error").as_string() == "Tool \"shell.exec\" is not permitted.");
  assert(denied_audit_event->payload.at("reason").as_string().find("Approval handler") != std::string::npos);

  agent::MemoryApprovalRecorder approval_recorder(agent::PermissionDecision{agent::PermissionDecisionKind::Allow, {}});
  agent::ToolExecutor approved_permission_executor(permission_registry, capability_policy, approval_recorder.handler());
  auto approved_permission_result = approved_permission_executor.execute_tool_call(agent::ToolCall{
      .id = "permission_2",
      .name = "shell.exec",
      .arguments = agent::Value::object({}),
  });
  assert(approved_permission_result.ok);
  assert(approval_recorder.records().size() == 1);
  assert(approval_recorder.records().front().request.bundle == "developer");

  auto rule_policy = agent::create_rule_based_permission_policy(agent::RuleBasedPermissionPolicyConfig{
      .rules = {agent::ToolPermissionRule{
          .match = agent::ToolPermissionMatcher{
              .tags = {"filesystem"},
              .bundles = {"local"},
          },
          .decision = agent::PermissionDecisionKind::Ask,
          .reason = "Local filesystem reads require confirmation.",
      }},
  });
  auto rule_decision = rule_policy(agent::PermissionRequest{
      .tool_name = "fs.readText",
      .capabilities = {"fs.read"},
      .risk_level = agent::ToolRiskLevel::Medium,
      .tags = {"filesystem"},
      .bundle = "local",
      .builtin = true,
      .tool_call = agent::ToolCall{.id = "permission_3", .name = "fs.readText", .arguments = agent::Value::object({})},
  });
  assert(rule_decision.decision == agent::PermissionDecisionKind::Ask);

  std::istringstream cli_approval_input("yes\n");
  std::ostringstream cli_approval_output;
  auto cli_approval = agent::create_cli_approval_handler(agent::CliApprovalHandlerConfig{
      .input = &cli_approval_input,
      .output = &cli_approval_output,
  });
  auto cli_decision = cli_approval(approval_recorder.records().front().request,
                                   agent::PermissionDecision{agent::PermissionDecisionKind::Ask, "needs approval"});
  assert(cli_decision.decision == agent::PermissionDecisionKind::Allow);
  assert(cli_approval_output.str().find("shell.exec") != std::string::npos);

  auto exploding_tool = agent::define_tool(agent::ToolDefinition{
      .name = "explode.direct",
      .description = "Throw to exercise ToolExecutor failure wrapping.",
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        throw std::runtime_error("direct boom");
      },
  });
  agent::ToolRegistry exploding_registry({exploding_tool});
  agent::EventBus exploding_events_bus;
  std::vector<agent::FrameworkEvent> exploding_events;
  exploding_events_bus.register_sink([&](const agent::FrameworkEvent& event) {
    exploding_events.push_back(event);
  });
  agent::ToolExecutor exploding_executor(exploding_registry, {}, {}, &exploding_events_bus);
  auto exploding_result = exploding_executor.execute_tool_call(agent::ToolCall{
      .id = "explode_direct_1",
      .name = "explode.direct",
      .arguments = agent::Value::object({}),
  });
  assert(!exploding_result.ok);
  assert(exploding_result.error == "Tool \"explode.direct\" failed.");
  assert(exploding_result.output.find("direct boom") != std::string::npos);
  const auto exploding_failed_event = std::find_if(
      exploding_events.begin(), exploding_events.end(),
      [](const agent::FrameworkEvent& event) { return event.category == "tool.failed"; });
  assert(exploding_failed_event != exploding_events.end());
  assert(exploding_failed_event->payload.at("error").as_string() == "Tool \"explode.direct\" failed.");

  auto parsed = agent::parse_json("{\"a\":[1,true,\"x\"],\"b\":{\"c\":2}}");
  assert(parsed.at("a").as_array().size() == 3);
  assert(parsed.at("b").at("c").as_integer() == 2);
  agent::write_json_file(smoke_artifacts_root() / "value.json", parsed);
  assert(agent::read_json_file(smoke_artifacts_root() / "value.json").at("b").at("c").as_integer() == 2);

  std::vector<std::string> hook_calls;
  agent::HookSet hook_a;
  hook_a.before_run = [&](const agent::RunHookContext& context) {
    assert(context.target == agent::ExecutionTarget::Run);
    assert(context.input.as_string() == "hook-input");
    hook_calls.push_back("a");
  };
  agent::HookSet hook_b;
  hook_b.before_run = [&](const agent::RunHookContext&) {
    hook_calls.push_back("b");
  };
  auto merged_hooks = agent::merge_hooks({hook_a, hook_b});
  agent::RunHookContext run_hook_context;
  run_hook_context.target = agent::ExecutionTarget::Run;
  run_hook_context.input = "hook-input";
  run_hook_context.context = agent::Value::object({});
  merged_hooks.before_run(run_hook_context);
  assert((hook_calls == std::vector<std::string>{"a", "b"}));

  assert(agent::build_url("https://api.example.test/", "/v1/chat") == "https://api.example.test/v1/chat");
  assert(agent::build_url("https://api.example.test", "https://override.test/path") == "https://override.test/path");
  bool http_called = false;
  auto json_response = agent::request_json(
      agent::RequestJsonOptions{
          .base_url = "https://api.example.test/",
          .path = "/v1/data",
          .headers = {{"authorization", "Bearer test"}},
          .body = agent::Value::object({{"value", 7}}),
      },
      [&](const agent::HttpRequest& request) {
        http_called = true;
        assert(request.url == "https://api.example.test/v1/data");
        assert(request.method == "POST");
        assert(request.headers.at("content-type") == "application/json");
        assert(request.headers.at("authorization") == "Bearer test");
        assert(request.body.find("\"value\":7") != std::string::npos);
        return agent::HttpResponse{.status = 200, .body = "{\"ok\":true}"};
      });
  assert(http_called);
  assert(json_response.at("ok").as_bool());
  try {
    (void)agent::request_json(
        agent::RequestJsonOptions{.base_url = "https://api.example.test", .path = "/fail"},
        [](const agent::HttpRequest&) {
          return agent::HttpResponse{.status = 429, .body = "{\"error\":\"rate\"}"};
        });
    assert(false);
  } catch (const agent::AdapterError& error) {
    assert(std::string(error.what()).find("429") != std::string::npos);
    assert(error.details().find("rate") != std::string::npos);
  }
  auto stream_response = agent::request_stream(
      agent::RequestStreamOptions{.base_url = "https://api.example.test", .path = "/stream"},
      [](const agent::HttpRequest&) {
        return agent::HttpResponse{.status = 200, .body = "data: one\n\ndata: two\n"};
      });
  assert(stream_response.body.find("data: one") != std::string::npos);
  auto lines = agent::read_lines(std::vector<std::string>{"alpha\r", "\nbeta\npartial"});
  assert((lines == std::vector<std::string>{"alpha", "beta", "partial"}));
  auto sse_events = agent::read_sse_events(std::vector<std::string>{
      ": keepalive\n",
      "data: first\n",
      "data: line\n\n",
      "event: ignored\n",
      "data: second\n",
  });
  assert((sse_events == std::vector<std::string>{"first\nline", "second"}));

  auto server_model = std::make_shared<ServerRuntimeModel>();
  agent::AgentRunner server_runner(agent::AgentRunnerConfig{
      .adapter = server_model,
      .contexts = {
          agent::ContextSource{
              .id = "server-runtime",
              .title = "Server Runtime",
              .resolve = [](const agent::Value& runtime) {
                std::vector<agent::EmbeddedContextBlock> blocks;
                if (runtime.at("source").as_string().empty()) {
                  return blocks;
                }
                blocks.push_back(agent::EmbeddedContextBlock{
                    .title = "Server Runtime",
                    .content = "source=" + runtime.at("source").as_string(),
                    .priority = 20,
                });
                return blocks;
              },
          },
      },
      .max_iterations = 2,
  });
  const auto server_audit_file = smoke_artifacts_root() / "server-audit.jsonl";
  const auto server_replay_dir = smoke_artifacts_root() / "server-replays";
  std::filesystem::remove(server_audit_file);
  std::filesystem::remove_all(server_replay_dir);
  agent::AgentServerApp server_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .runner = &server_runner,
      .server_name = "native-smoke",
      .audit_log_path = server_audit_file,
      .replay_dir = server_replay_dir,
      .auth = agent::AgentServerAuthConfig{.bearer_tokens = {"server-token"}},
      .cors = agent::ServerCorsConfig{
          .origins = {"https://app.example"},
          .max_age_seconds = 600,
      },
      .session = agent::AgentServerSessionPolicy{.mode = "require", .id_prefix = "srv:"},
      .metrics_pricing = {{"fixture:server-runtime",
                           agent::UsagePricing{.input_per_1k_tokens = 0.001,
                                               .output_per_1k_tokens = 0.002}}},
  });
  server_app.add_route(agent::AgentServerRoute{
      .method = "GET",
      .pattern = "/items/:id",
      .handler = [](const agent::AgentServerRequestContext& context) {
        return agent::send_json(200, agent::Value::object({
                                         {"id", context.params.at("id")},
                                         {"filter", context.query.at("filter")},
                                     }));
      },
  });
  server_app.add_route(agent::AgentServerRoute{
      .method = "GET",
      .pattern = "/trace",
      .handler = [](const agent::AgentServerRequestContext& context) {
        return agent::send_json(200, agent::Value::object({
                                         {"requestId", context.access.request_id},
                                         {"traceId", context.access.trace_context.trace_id},
                                         {"spanName", context.access.trace_context.span_name},
                                     }));
      },
  });
  auto preflight_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "OPTIONS",
      .headers = {{"origin", "https://app.example"}},
  });
  assert(preflight_response.status == 204);
  assert(preflight_response.headers.at("access-control-allow-origin") == "https://app.example");
  assert(preflight_response.headers.at("access-control-max-age") == "600");
  assert(!preflight_response.headers.at("x-request-id").empty());
  auto blocked_preflight_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "OPTIONS",
      .headers = {{"origin", "https://blocked.example"}},
  });
  assert(blocked_preflight_response.status == 403);
  auto unauthorized_ready = server_app.handle(agent::HttpRequest{.url = "/ready", .method = "GET"});
  assert(unauthorized_ready.status == 401);
  assert(!unauthorized_ready.headers.at("x-trace-id").empty());
  auto health_response = server_app.handle(agent::HttpRequest{.url = "/health", .method = "GET"});
  assert(health_response.status == 200);
  assert(agent::parse_json(health_response.body).at("service").as_string() == "native-smoke");
  auto chat_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"},
                  {"origin", "https://app.example"},
                  {"x-request-id", "req-chat-1"},
                  {"x-trace-id", "trace-chat-1"}},
      .body = agent::Value::object({
          {"input", "server-input"},
          {"sessionId", "one"},
          {"context", agent::Value::object({{"source", "server-source"}})},
      }).stringify(0),
  });
  assert(chat_response.status == 200);
  assert(chat_response.headers.at("access-control-allow-origin") == "https://app.example");
  assert(chat_response.headers.at("x-request-id") == "req-chat-1");
  assert(chat_response.headers.at("x-trace-id") == "trace-chat-1");
  auto chat_payload = agent::parse_json(chat_response.body);
  assert(chat_payload.at("sessionId").as_string() == "srv:one");
  assert(chat_payload.at("requestId").as_string() == "req-chat-1");
  assert(chat_payload.at("traceId").as_string() == "trace-chat-1");
  assert(std::filesystem::exists(chat_payload.at("replayPath").as_string()));
  auto chat_replay = agent::load_run_replay(std::filesystem::path(chat_payload.at("replayPath").as_string()).parent_path());
  assert(chat_replay.manifest.session_id == "srv:one");
  assert(chat_replay.manifest.agent_id == "native-smoke");
  assert(chat_replay.manifest.metadata.at("source").as_string() == "server");
  assert(chat_replay.manifest.metadata.at("path").as_string() == "/chat");
  assert(chat_replay.result.at("response").at("model").as_string() == "server-runtime");
  assert(chat_payload.at("output").as_string().find("server-ok:server-input") != std::string::npos);
  assert(chat_payload.at("result").at("response").at("model").as_string() == "server-runtime");
  auto invalid_reasoning_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"}},
      .body = agent::Value::object({
          {"input", "server-input"},
          {"sessionId", "invalid-reasoning"},
          {"context", agent::Value::object({{"source", "server-source"}})},
          {"modelSettings", agent::Value::object({
                                {"reasoning", agent::Value::object({{"enabled", "yes"}})},
                            })},
      }).stringify(0),
  });
  assert(invalid_reasoning_response.status == 400);
  auto invalid_reasoning_payload = agent::parse_json(invalid_reasoning_response.body);
  assert(invalid_reasoning_payload.at("field").as_string() == "modelSettings.reasoning");
  assert(invalid_reasoning_payload.at("error").as_string().find("enabled must be a boolean") != std::string::npos);
  auto reasoning_chat_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"}},
      .body = agent::Value::object({
          {"input", "server-input"},
          {"sessionId", "reasoning"},
          {"context", agent::Value::object({{"source", "server-source"}})},
          {"modelSettings", agent::Value::object({
                                {"reasoning", agent::Value::object({
                                                  {"enabled", true},
                                                  {"budget", "medium"},
                                                  {"includeThoughts", true},
                                                  {"tagName", "think"},
                                              })},
                            })},
      }).stringify(0),
  });
  assert(reasoning_chat_response.status == 200);
  assert(server_model->last_settings().reasoning);
  assert(server_model->last_settings().reasoning->enabled == true);
  assert(std::holds_alternative<std::string>(server_model->last_settings().reasoning->budget));
  assert(std::get<std::string>(server_model->last_settings().reasoning->budget) == "medium");
  assert(server_model->last_settings().reasoning->include_thoughts == true);
  assert(server_model->last_settings().reasoning->tag_name == "think");
  auto trace_route_response = server_app.handle(agent::HttpRequest{
      .url = "/trace",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"},
                  {"x-request-id", "req-route-1"},
                  {"x-trace-id", "trace-route-1"}},
  });
  assert(trace_route_response.status == 200);
  auto trace_route_payload = agent::parse_json(trace_route_response.body);
  assert(trace_route_payload.at("requestId").as_string() == "req-route-1");
  assert(trace_route_payload.at("traceId").as_string() == "trace-route-1");
  assert(trace_route_payload.at("spanName").as_string() == "http.request");
  auto stream_server_response = server_app.handle(agent::HttpRequest{
      .url = "/stream",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"}},
      .body = agent::Value::object({
          {"input", "server-input-stream"},
          {"sessionId", "two"},
          {"context", agent::Value::object({{"source", "server-source"}})},
      }).stringify(0),
  });
  assert(stream_server_response.status == 200);
  assert(stream_server_response.headers.at("content-type").find("text/event-stream") != std::string::npos);
  assert(stream_server_response.body.find("event: replay") != std::string::npos);
  assert(stream_server_response.body.find("event: done") != std::string::npos);
  assert(!stream_server_response.headers.at("x-request-id").empty());
  auto route_response = server_app.handle(agent::HttpRequest{
      .url = "/items/native%20id?filter=active",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"}},
  });
  assert(route_response.status == 200);
  auto route_payload = agent::parse_json(route_response.body);
  assert(route_payload.at("id").as_string() == "native id");
  assert(route_payload.at("filter").as_string() == "active");
  {
    agent::AgentServerApp concurrent_route_app(agent::AgentServerOptions{.allow_unauthenticated = true});
    std::atomic<int> concurrent_route_ready{0};
    std::atomic<bool> concurrent_route_start{false};
    std::atomic<int> concurrent_route_hits{0};
    constexpr int concurrent_route_thread_count = 8;
    std::vector<std::thread> concurrent_route_workers;
    for (int thread_index = 0; thread_index < concurrent_route_thread_count; ++thread_index) {
      concurrent_route_workers.emplace_back([&, thread_index]() {
        concurrent_route_ready.fetch_add(1);
        while (!concurrent_route_start.load()) {
          std::this_thread::yield();
        }
        const auto suffix = std::to_string(thread_index);
        const auto pattern = "/concurrent-route-" + suffix + "/:id";
        concurrent_route_app.add_route(agent::AgentServerRoute{
            .method = "GET",
            .pattern = pattern,
            .handler = [suffix, &concurrent_route_hits](const agent::AgentServerRequestContext& context) {
              concurrent_route_hits.fetch_add(1);
              return agent::send_json(200, agent::Value::object({
                                               {"route", suffix},
                                               {"id", context.params.at("id")},
                                           }));
            },
        });
        auto specs = concurrent_route_app.route_specs();
        assert(std::find(specs.begin(), specs.end(), "GET " + pattern) != specs.end());
        auto response = concurrent_route_app.handle(agent::HttpRequest{
            .url = "/concurrent-route-" + suffix + "/value-" + suffix,
            .method = "GET",
        });
        assert(response.status == 200);
        auto payload = agent::parse_json(response.body);
        assert(payload.at("route").as_string() == suffix);
        assert(payload.at("id").as_string() == "value-" + suffix);
      });
    }
    while (concurrent_route_ready.load() < concurrent_route_thread_count) {
      std::this_thread::yield();
    }
    concurrent_route_start.store(true);
    for (auto& worker : concurrent_route_workers) {
      worker.join();
    }
    assert(concurrent_route_hits.load() == concurrent_route_thread_count);
    auto final_route_specs = concurrent_route_app.route_specs();
    for (int thread_index = 0; thread_index < concurrent_route_thread_count; ++thread_index) {
      const auto expected = "GET /concurrent-route-" + std::to_string(thread_index) + "/:id";
      assert(std::find(final_route_specs.begin(), final_route_specs.end(), expected) != final_route_specs.end());
    }
  }
  auto metrics_agent_chat_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"}},
      .body = agent::Value::object({
          {"input", "server-input-agent"},
          {"sessionId", "metrics-agent"},
          {"agent", "metrics-agent"},
          {"context", agent::Value::object({{"source", "server-source"}})},
      }).stringify(0),
  });
  assert(metrics_agent_chat_response.status == 200);
  agent::AgentServerApp no_trace_header_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .tracing = agent::AgentServerTracingConfig{.response_headers = false},
  });
  auto no_trace_header_response = no_trace_header_app.handle(agent::HttpRequest{
      .url = "/health",
      .method = "GET",
      .headers = {{"x-request-id", "hidden-request"}},
  });
  assert(no_trace_header_response.status == 200);
  assert(no_trace_header_response.headers.find("x-request-id") == no_trace_header_response.headers.end());

  auto factory_model = std::make_shared<ServerRuntimeModel>();
  auto factory_runner = std::make_shared<agent::AgentRunner>(agent::AgentRunnerConfig{
      .adapter = factory_model,
      .contexts = {
          agent::ContextSource{
              .id = "server-runtime",
              .title = "Server Runtime",
              .resolve = [](const agent::Value& runtime) {
                std::vector<agent::EmbeddedContextBlock> blocks;
                if (runtime.at("source").as_string().empty()) {
                  return blocks;
                }
                blocks.push_back(agent::EmbeddedContextBlock{
                    .title = "Server Runtime",
                    .content = "source=" + runtime.at("source").as_string(),
                    .priority = 20,
                });
                return blocks;
              },
          },
      },
      .max_iterations = 1,
  });
  int factory_runner_calls = 0;
  agent::AgentServerApp factory_server_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .create_runner = [&](const agent::Value& request_body) {
        ++factory_runner_calls;
        assert(request_body.at("input").as_string() == "server-input-factory");
        assert(request_body.at("sessionId").as_string() == "factory-session");
        assert(request_body.at("agent").as_string() == "factory-agent");
        assert(request_body.at("context").at("source").as_string() == "server-source");
        assert(request_body.at("context").at("traceContext").at("spanName").as_string() == "chat");
        assert(request_body.at("context").at("server").at("sessionId").as_string() == "factory-session");
        assert(request_body.at("context").at("server").at("agentId").as_string() == "factory-agent");
        return factory_runner;
      },
      .server_name = "factory-server",
  });
  auto factory_ready_response = factory_server_app.handle(agent::HttpRequest{
      .url = "/ready",
      .method = "GET",
  });
  assert(factory_ready_response.status == 200);
  assert(agent::parse_json(factory_ready_response.body).at("runner").as_bool());
  auto factory_chat_response = factory_server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .body = agent::Value::object({
          {"input", "server-input-factory"},
          {"sessionId", "factory-session"},
          {"agent", "factory-agent"},
          {"context", agent::Value::object({{"source", "server-source"}})},
      }).stringify(0),
  });
  assert(factory_chat_response.status == 200);
  assert(factory_runner_calls == 1);
  auto factory_chat_payload = agent::parse_json(factory_chat_response.body);
  assert(factory_chat_payload.at("sessionId").as_string() == "factory-session");
  assert(factory_chat_payload.at("output").as_string().find("server-ok:server-input-factory") !=
         std::string::npos);

  const auto config_server_dir = smoke_artifacts_root() / "server-config";
  const auto config_server_replay_dir =
      smoke_artifacts_root() / "server-config-replays";
  const auto config_server_sessions_dir =
      smoke_artifacts_root() / "server-config-sessions";
  std::filesystem::create_directories(config_server_dir);
  std::filesystem::remove_all(config_server_replay_dir);
  std::filesystem::remove_all(config_server_sessions_dir);
  agent::write_json_file(config_server_dir / "node-agent.config.json", agent::Value::object({
      {"defaultAgent", "main"},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"systemPrompt", "Config-backed native server."},
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({})}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({
                      {"kind", "file"},
                      {"baseDir", config_server_sessions_dir.string()},
                  })},
              })},
          })},
      })},
  }));
  agent::AgentServerApp config_server_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .config_path = config_server_dir,
      .server_name = "config-server",
      .replay_dir = config_server_replay_dir,
  });
  auto config_ready_response = config_server_app.handle(agent::HttpRequest{
      .url = "/ready",
      .method = "GET",
  });
  assert(config_ready_response.status == 200);
  auto config_ready_payload = agent::parse_json(config_ready_response.body);
  assert(config_ready_payload.at("runner").as_bool());
  assert(config_ready_payload.at("agent").as_string() == "main");
  auto config_chat_response = config_server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .body = agent::Value::object({
          {"input", "config server input"},
          {"sessionId", "config-session"},
      }).stringify(0),
  });
  assert(config_chat_response.status == 200);
  auto config_chat_payload = agent::parse_json(config_chat_response.body);
  assert(config_chat_payload.at("output").as_string() == "config server input");
  auto config_chat_replay = agent::load_run_replay(config_chat_payload.at("replayPath").as_string());
  assert(config_chat_replay.manifest.agent_id == "main");
  auto config_sessions_response = config_server_app.handle(agent::HttpRequest{
      .url = "/sessions",
      .method = "GET",
  });
  assert(config_sessions_response.status == 200);
  auto config_sessions_payload = agent::parse_json(config_sessions_response.body);
  assert(std::any_of(config_sessions_payload.at("items").as_array().begin(),
                     config_sessions_payload.at("items").as_array().end(),
                     [](const agent::Value& item) { return item.as_string() == "config-session"; }));
  const auto module_config_server_dir = smoke_artifacts_root() / "server-config-module";
  const auto module_config_server_sessions_dir = module_config_server_dir / "module-server-sessions";
  std::filesystem::create_directories(module_config_server_dir);
  std::filesystem::remove_all(module_config_server_sessions_dir);
  {
    std::ofstream module_config(module_config_server_dir / "node-agent.config.mjs");
    module_config << "export default { agents: {} };\n";
  }
  bool server_module_loader_called = false;
  agent::AgentServerApp module_config_server_app(agent::AgentServerOptions{.allow_unauthenticated = true,
      .config_path = module_config_server_dir,
      .config_module_loader = [&](const std::filesystem::path& path) {
        server_module_loader_called = true;
        assert(path.filename().string() == "node-agent.config.mjs");
        return agent::Value::object({
            {"defaultAgent", "module"},
            {"agents", agent::Value::object({
                {"module", agent::Value::object({
                    {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                    {"memory", agent::Value::object({
                        {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "module-server-sessions"}})},
                    })},
                })},
            })},
        });
      },
      .server_name = "module-config-server",
  });
  auto module_config_ready_response = module_config_server_app.handle(agent::HttpRequest{
      .url = "/ready",
      .method = "GET",
  });
  assert(module_config_ready_response.status == 200);
  assert(server_module_loader_called);
  auto module_config_ready_payload = agent::parse_json(module_config_ready_response.body);
  assert(module_config_ready_payload.at("agent").as_string() == "module");
  server_module_loader_called = false;
  auto module_config_chat_response = module_config_server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .body = agent::Value::object({
          {"input", "module server input"},
          {"sessionId", "module-server-session"},
      }).stringify(0),
  });
  assert(module_config_chat_response.status == 200);
  assert(!server_module_loader_called);
	  auto module_config_chat_payload = agent::parse_json(module_config_chat_response.body);
	  assert(module_config_chat_payload.at("output").as_string() == "module server input");
	  assert(std::filesystem::exists(module_config_server_sessions_dir / "module-server-session.json"));
	  const auto concurrent_config_server_dir =
	      smoke_artifacts_root() / "server-config-concurrent";
	  std::filesystem::create_directories(concurrent_config_server_dir);
	  {
	    std::ofstream module_config(concurrent_config_server_dir / "node-agent.config.mjs");
	    module_config << "export default { agents: {} };\n";
	  }
	  std::atomic<int> concurrent_module_loader_calls{0};
	  agent::AgentServerApp concurrent_config_server_app(agent::AgentServerOptions{.allow_unauthenticated = true,
	      .config_path = concurrent_config_server_dir,
	      .config_module_loader = [&](const std::filesystem::path& path) {
	        assert(path.filename().string() == "node-agent.config.mjs");
	        concurrent_module_loader_calls += 1;
	        std::this_thread::sleep_for(std::chrono::milliseconds(20));
	        return agent::Value::object({
	            {"defaultAgent", "main"},
	            {"agents", agent::Value::object({
	                {"main", agent::Value::object({
	                    {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
	                })},
	            })},
	        });
	      },
	      .server_name = "concurrent-config-server",
	  });
	  std::vector<std::thread> concurrent_config_threads;
	  std::vector<int> concurrent_config_statuses(12, 0);
	  std::vector<std::string> concurrent_config_outputs(12);
	  for (std::size_t index = 0; index < concurrent_config_statuses.size(); ++index) {
	    concurrent_config_threads.emplace_back([&, index]() {
	      auto response = concurrent_config_server_app.handle(agent::HttpRequest{
	          .url = "/chat",
	          .method = "POST",
	          .body = agent::Value::object({
	              {"input", "concurrent config " + std::to_string(index)},
	              {"sessionId", "concurrent-config-session-" + std::to_string(index)},
	          }).stringify(0),
	      });
	      concurrent_config_statuses[index] = response.status;
	      if (response.status == 200) {
	        concurrent_config_outputs[index] = agent::parse_json(response.body).at("output").as_string();
	      }
	    });
	  }
	  for (auto& thread : concurrent_config_threads) {
	    thread.join();
	  }
	  assert(concurrent_module_loader_calls == 1);
	  for (std::size_t index = 0; index < concurrent_config_statuses.size(); ++index) {
	    assert(concurrent_config_statuses[index] == 200);
	    assert(concurrent_config_outputs[index] == "concurrent config " + std::to_string(index));
	  }
  auto invalid_chat_response = server_app.handle(agent::HttpRequest{
      .url = "/chat",
      .method = "POST",
      .headers = {{"authorization", "Bearer server-token"}},
      .body = "{\"sessionId\":\"bad\"}",
  });
  assert(invalid_chat_response.status == 400);
  auto metrics_response = server_app.handle(agent::HttpRequest{
      .url = "/metrics",
      .method = "GET",
      .headers = {{"authorization", "Bearer server-token"}},
  });
  assert(metrics_response.status == 200);
  const auto metrics_payload = agent::parse_json(metrics_response.body);
  const auto& chat_metrics = metrics_payload.at("routes").at("POST /chat");
  assert(chat_metrics.at("requests").as_integer() >= 4);
  assert(chat_metrics.at("totalTokens").as_integer() >= 450);
  assert(chat_metrics.at("estimatedCostByCurrency").at("USD").as_number() > 0);
  assert(metrics_payload.at("routes").at("POST /stream").at("totalTokens").as_integer() == 150);
  assert(metrics_payload.at("agents").at("metrics-agent").at("totalTokens").as_integer() == 150);
  agent::AgentServerMetricsCollector direct_metrics(
