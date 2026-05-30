  SMOKE_CHECK(saw_absolute_allowed_link);
  SMOKE_CHECK(!saw_raw_allowed_link);
  const auto policy_crawl_value = agent::web_crawl_result_to_value(policy_crawl).stringify();
  SMOKE_CHECK(policy_crawl_value.find("blockedUrls") != std::string::npos);
  SMOKE_CHECK(policy_crawl_value.find("discoveredUrls") != std::string::npos);
  SMOKE_CHECK(policy_crawl_value.find("\"skipped\"") == std::string::npos);
  SMOKE_CHECK(policy_crawl_value.find("https://crawl.example.test/allowed") != std::string::npos);
  SMOKE_CHECK(policy_crawl_value.find("\"/allowed\"") == std::string::npos);
  agent::NativeWebPageFetcher failing_crawl_fetcher;
  failing_crawl_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://failing-crawl.example.test/",
      .content_type = "text/html",
      .title = "Failing Crawl Home",
      .html = "<html><body><a href=\"/missing\">Missing</a></body></html>",
  });
  agent::NativeWebCrawler failing_crawler(&failing_crawl_fetcher);
  bool failing_crawl_threw = false;
  try {
    (void)failing_crawler.crawl(agent::WebCrawlRequest{
        .url = "https://failing-crawl.example.test/",
        .max_depth = 1,
        .max_pages = 3,
    });
  } catch (const agent::ConfigurationError& error) {
    failing_crawl_threw = std::string(error.what()).find("No registered page") != std::string::npos;
  }
  SMOKE_CHECK(failing_crawl_threw);

  auto web_bundle_tools = agent::create_builtin_tools({"web"});
  agent::ToolRegistry web_bundle_registry(web_bundle_tools);
  const auto* web_search_tool = web_bundle_registry.get("web.search");
  SMOKE_CHECK(web_search_tool != nullptr);
  SMOKE_CHECK(web_search_tool->bundle == "web");
  SMOKE_CHECK(contains_string(web_search_tool->tags, "builtin"));
  SMOKE_CHECK(contains_string(web_search_tool->tags, "search"));
  SMOKE_CHECK(contains_string(web_search_tool->tags, "bundle:web"));
  SMOKE_CHECK(contains_string(web_search_tool->capabilities, "network.search"));
  SMOKE_CHECK(contains_string(web_search_tool->capabilities, "network.http.read"));
  {
    agent::WebSearchProviderRegistry concurrent_web_search_registry;
    agent::ToolRegistry concurrent_web_tools(
        agent::create_web_builtin_tools(&concurrent_web_search_registry));
    agent::ToolExecutor concurrent_web_executor(concurrent_web_tools);
    std::atomic<int> concurrent_web_ready{0};
    std::atomic<bool> concurrent_web_start{false};
    std::atomic<int> concurrent_web_search_calls{0};
    constexpr int concurrent_web_thread_count = 8;
    std::vector<std::thread> concurrent_web_workers;
    for (int thread_index = 0; thread_index < concurrent_web_thread_count; ++thread_index) {
      concurrent_web_workers.emplace_back([&, thread_index]() {
        concurrent_web_ready.fetch_add(1);
        while (!concurrent_web_start.load()) {
          std::this_thread::yield();
        }
        const auto suffix = std::to_string(thread_index);
        const auto provider_name = "web.concurrent." + suffix;
        concurrent_web_search_registry.register_provider(agent::StaticWebSearchProvider(
            provider_name,
            [provider_name, &concurrent_web_search_calls](const agent::WebSearchQuery& query) {
              concurrent_web_search_calls.fetch_add(1);
              return std::vector<agent::WebSearchResult>{agent::WebSearchResult{
                  .title = "Result " + query.query,
                  .url = "https://example.test/" + provider_name,
                  .snippet = "ok",
                  .source = provider_name,
                  .rank = 1,
              }};
            }));
        auto search_result = concurrent_web_executor.execute_tool_call(agent::ToolCall{
            .id = "web-search-" + suffix,
            .name = "web.search",
            .arguments = agent::Value::object({
                {"provider", provider_name},
                {"query", "query-" + suffix},
                {"topK", 1},
            }),
        });
        SMOKE_CHECK(search_result.ok);
        SMOKE_CHECK(search_result.output.find(provider_name) != std::string::npos);
        SMOKE_CHECK(search_result.output.find("query-" + suffix) != std::string::npos);
      });
    }
    while (concurrent_web_ready.load() < concurrent_web_thread_count) {
      std::this_thread::yield();
    }
    concurrent_web_start.store(true);
    for (auto& worker : concurrent_web_workers) {
      worker.join();
    }
    SMOKE_CHECK(concurrent_web_search_calls.load() == concurrent_web_thread_count);
  }
  {
    agent::ToolRegistry concurrent_tools;
    agent::ToolExecutor concurrent_executor(concurrent_tools);
    agent::EmbeddedContextManager concurrent_contexts;
    agent::SkillRegistry concurrent_skills;
    constexpr int thread_count = 6;
    constexpr int iterations = 8;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto suffix = std::to_string(thread_index) + "-" + std::to_string(iteration);
          const auto tool_name = "concurrent.tool." + suffix;
          concurrent_tools.register_tool(agent::ToolDefinition{
              .name = tool_name,
              .description = "Concurrent registry tool",
              .execute = [tool_name](const agent::Value&, agent::ToolExecutionContext&) {
                return agent::Value::object({{"tool", tool_name}});
              },
          });
          auto tool_result = concurrent_executor.execute_tool_call(agent::ToolCall{
              .id = "call-" + suffix,
              .name = tool_name,
              .arguments = agent::Value::object({}),
          });
          SMOKE_CHECK(tool_result.ok);
          SMOKE_CHECK(tool_result.output.find(tool_name) != std::string::npos);

          concurrent_contexts.register_source(agent::ContextSource{
              .id = "ctx-" + suffix,
              .title = "Context " + suffix,
              .resolve = [suffix](const agent::Value&) {
                return std::vector<agent::EmbeddedContextBlock>{agent::EmbeddedContextBlock{
                    .title = "Context " + suffix,
                    .content = "content " + suffix,
                    .priority = 1,
                }};
              },
          });
          auto blocks = concurrent_contexts.resolve_blocks();
          SMOKE_CHECK(!blocks.empty());

          concurrent_skills.register_skill(agent::SkillDefinition{
              .manifest = agent::SkillManifest{
                  .name = "skill-" + suffix,
                  .description = "Concurrent skill " + suffix,
                  .user_invocable = true,
                  .allowed_tools = {tool_name},
              },
              .prompt = "Use concurrent skill " + suffix,
          });
          agent::SkillResolveOptions concurrent_opts;
          concurrent_opts.input_text = "run concurrent skill";
          concurrent_opts.session_id = "session-" + suffix;
          concurrent_opts.activations.push_back(agent::SkillActivation{
              "skill-" + suffix, "", agent::SkillActivationSource::Host, 0});
          auto skill_state = agent::resolve_skills_state(&concurrent_skills, concurrent_opts);
          SMOKE_CHECK(!skill_state.active_skills.empty());
          SMOKE_CHECK(contains_string(skill_state.allowed_tools, tool_name));
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    SMOKE_CHECK(concurrent_tools.list().size() == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(concurrent_skills.list().size() == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(concurrent_contexts.resolve_blocks().size() == static_cast<std::size_t>(thread_count * iterations));
  }

  auto usage = agent::extract_model_usage(agent::Value::object({
      {"usage", agent::Value::object({{"prompt_tokens", 12}, {"completion_tokens", 8}})},
  }), "openai");
  SMOKE_CHECK(usage.input_tokens == 12);
  SMOKE_CHECK(usage.output_tokens == 8);
  SMOKE_CHECK(usage.total_tokens == 20);
  SMOKE_CHECK(usage.provider == "openai");
  auto gemini_usage = agent::extract_model_usage(agent::Value::object({
      {"usageMetadata", agent::Value::object({
                            {"promptTokenCount", 24},
                            {"candidatesTokenCount", 12},
                            {"totalTokenCount", 36},
                        })},
  }));
  SMOKE_CHECK(gemini_usage.input_tokens == 24);
  SMOKE_CHECK(gemini_usage.output_tokens == 12);
  SMOKE_CHECK(gemini_usage.total_tokens == 36);
  auto raw_response_usage = agent::extract_model_usage(agent::Value::object({
      {"provider", "qwen"},
      {"model", "qwen-plus"},
      {"raw", agent::Value::object({
                  {"usage", agent::Value::object({
                                {"prompt_tokens", 7},
                                {"completion_tokens", 3},
                                {"total_tokens", 10},
                            })},
              })},
  }), "qwen");
  SMOKE_CHECK(raw_response_usage.input_tokens == 7);
  SMOKE_CHECK(raw_response_usage.output_tokens == 3);
  SMOKE_CHECK(raw_response_usage.total_tokens == 10);
  SMOKE_CHECK(raw_response_usage.provider == "qwen");
  auto explicit_zero_total_usage = agent::extract_model_usage(agent::Value::object({
      {"inputTokens", 9},
      {"outputTokens", 4},
      {"totalTokens", 0},
  }));
  SMOKE_CHECK(explicit_zero_total_usage.input_tokens == 9);
  SMOKE_CHECK(explicit_zero_total_usage.output_tokens == 4);
  SMOKE_CHECK(explicit_zero_total_usage.total_tokens == 0);
  auto merged_usage = agent::merge_model_usage({
      usage,
      agent::ModelUsage{.input_tokens = 4, .output_tokens = 9, .total_tokens = 13},
  });
  SMOKE_CHECK(merged_usage.input_tokens == 16);
  SMOKE_CHECK(merged_usage.output_tokens == 17);
  SMOKE_CHECK(merged_usage.total_tokens == 33);
  SMOKE_CHECK(merged_usage.provider == "openai");

  agent::JsonSchema default_object_schema;
  default_object_schema.type = agent::JsonSchemaType::Object;
  default_object_schema.properties["known"].type = agent::JsonSchemaType::String;
  auto normalized_default_object_schema = agent::normalize_json_schema(default_object_schema);
  SMOKE_CHECK(normalized_default_object_schema.additional_properties.is_set());
  SMOKE_CHECK(!normalized_default_object_schema.additional_properties);
  auto raw_default_object_value = agent::json_schema_to_value(default_object_schema);
  SMOKE_CHECK(!raw_default_object_value.contains("additionalProperties"));
  auto normalized_default_object_value = agent::json_schema_to_value(normalized_default_object_schema);
  SMOKE_CHECK(!normalized_default_object_value.at("additionalProperties").as_bool(true));
  auto valid_default_object_issues = agent::validate_json_schema(
      default_object_schema,
      agent::Value::object({{"known", "ok"}}));
  SMOKE_CHECK(valid_default_object_issues.empty());
  auto extra_default_object_issues = agent::validate_json_schema(
      default_object_schema,
      agent::Value::object({{"known", "ok"}, {"extra", "value"}}));
  SMOKE_CHECK(extra_default_object_issues.size() == 1);
  SMOKE_CHECK(extra_default_object_issues.front().path == "$.extra");

  agent::JsonSchema explicit_open_object_schema = default_object_schema;
  explicit_open_object_schema.additional_properties = true;
  auto valid_open_object_issues = agent::validate_json_schema(
      explicit_open_object_schema,
      agent::Value::object({{"known", "ok"}, {"extra", "value"}}));
  SMOKE_CHECK(valid_open_object_issues.empty());

  agent::JsonSchema nested_default_object_schema;
  nested_default_object_schema.type = agent::JsonSchemaType::Object;
  nested_default_object_schema.properties["payload"].type = agent::JsonSchemaType::Object;
  auto nested_default_object_issues = agent::validate_json_schema(
      nested_default_object_schema,
      agent::Value::object({{"payload", agent::Value::object({{"extra", "allowed"}})}}));
  SMOKE_CHECK(nested_default_object_issues.empty());

  auto strict_schema_tool = agent::define_tool(agent::ToolDefinition{
      .name = "schema.strict",
      .description = "Validate Node-style strict object schema defaults.",
      .input_schema = default_object_schema,
      .execute = [](const agent::Value& input, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return input;
      },
  });
  SMOKE_CHECK(strict_schema_tool.input_schema.additional_properties.is_set());
  SMOKE_CHECK(!strict_schema_tool.input_schema.additional_properties);
  bool rejected_extra_tool_argument = false;
  try {
    agent::ToolExecutionContext context;
    (void)strict_schema_tool.invoke(
        agent::Value::object({{"known", "ok"}, {"extra", "value"}}),
        context);
  } catch (const agent::SchemaValidationError& error) {
    rejected_extra_tool_argument = true;
    SMOKE_CHECK(error.issues().size() == 1);
    SMOKE_CHECK(error.issues().front().path == "$.extra");
  }
  SMOKE_CHECK(rejected_extra_tool_argument);

  agent::JsonSchema additional_property_schema;
  additional_property_schema.type = agent::JsonSchemaType::Object;
  additional_property_schema.properties["known"].type = agent::JsonSchemaType::String;
  additional_property_schema.additional_properties_schema = std::make_shared<agent::JsonSchema>();
  additional_property_schema.additional_properties_schema->type = agent::JsonSchemaType::Integer;
  auto valid_additional_property_issues = agent::validate_json_schema(
      additional_property_schema,
      agent::Value::object({{"known", "ok"}, {"count", 2}}));
  SMOKE_CHECK(valid_additional_property_issues.empty());
  auto invalid_additional_property_issues = agent::validate_json_schema(
      additional_property_schema,
      agent::Value::object({{"known", "ok"}, {"count", "two"}}));
  SMOKE_CHECK(invalid_additional_property_issues.size() == 1);
  SMOKE_CHECK(invalid_additional_property_issues.front().path == "$.count");
  auto serialized_additional_property_schema = agent::json_schema_to_value(additional_property_schema);
  SMOKE_CHECK(serialized_additional_property_schema.at("additionalProperties").at("type").as_string() == "integer");
  auto roundtripped_additional_property_schema = agent::json_schema_from_value(serialized_additional_property_schema);
  SMOKE_CHECK(roundtripped_additional_property_schema.additional_properties_schema);
  SMOKE_CHECK(roundtripped_additional_property_schema.additional_properties_schema->type == agent::JsonSchemaType::Integer);

  auto cost = agent::estimate_usage_cost(
      usage,
      agent::UsagePricing{.input_per_1k_tokens = 0.01, .output_per_1k_tokens = 0.03},
      "openai",
      "test-model");
  SMOKE_CHECK(cost.total_cost > 0);

  auto parsed_plan = agent::parse_execution_plan_text(
      "Plan:\n```json\n{\"steps\":[{\"title\":\"Only step\"}],\"notes\":[\"parsed\"]}\n```");
  SMOKE_CHECK(parsed_plan);
  SMOKE_CHECK(parsed_plan->goal == "Execute the task safely and efficiently.");
  SMOKE_CHECK(parsed_plan->steps.size() == 1);
  SMOKE_CHECK(parsed_plan->steps.front().id == "step_1");
  SMOKE_CHECK(parsed_plan->notes.front() == "parsed");

  auto planner_model = std::make_shared<RuntimePlannerModel>();
  agent::ToolRegistry planner_tool_registry(tools);
  agent::ModelPlanner model_planner(agent::ModelPlannerConfig{
      .model = planner_model,
      .max_steps = 3,
  });
  auto model_plan = model_planner.plan(agent::PlannerParams{
      .input = "Plan native runtime",
      .tools = &planner_tool_registry,
      .memory_hits = {agent::RetrievedMemory{.id = "planner-memory",
                                             .content = "Memory says keep the plan short.",
                                             .score = 0.8}},
  });
  SMOKE_CHECK(model_plan);
  SMOKE_CHECK(model_plan->goal == "Ship native planner");
  SMOKE_CHECK(model_plan->steps.size() == 2);
  SMOKE_CHECK(model_plan->steps.front().id == "step_1");
  SMOKE_CHECK(model_plan->steps.front().tool_name == "math.eval");
  SMOKE_CHECK(model_plan->steps.back().depends_on.front() == "step_1");
  SMOKE_CHECK(planner_model->planning_calls() == 1);

  auto runner_planner_model = std::make_shared<RuntimePlannerModel>();
  auto runtime_planner = std::make_shared<agent::ModelPlanner>(agent::ModelPlannerConfig{
      .model = runner_planner_model,
      .max_steps = 2,
  });
  auto planned_runner = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = runner_planner_model,
      .tools = tools,
      .max_iterations = 1,
      .planner = runtime_planner,
  });
  std::vector<std::string> planning_events;
  const auto planned_sink_id = planned_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category.find("planning.") == 0) {
      planning_events.push_back(event.category);
    }
  });
  auto planned_run = planned_runner.run("Use planning before answering", "planned-run-session");
  SMOKE_CHECK(planned_run.text == "planned response");
  SMOKE_CHECK(planned_run.plan);
  SMOKE_CHECK(planned_run.plan->steps.size() == 2);
  SMOKE_CHECK(runner_planner_model->saw_plan_message());
  SMOKE_CHECK(contains_string(planning_events, "planning.started"));
  SMOKE_CHECK(contains_string(planning_events, "planning.completed"));

  auto planned_stream = planned_runner.stream("Use planning in stream", "planned-stream-session");
  bool saw_planning_event = false;
  bool saw_planning_status_start = false;
  bool saw_planning_status_complete = false;
  for (const auto& event : planned_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Status &&
        event.status.stage == "planning") {
      saw_planning_status_start = saw_planning_status_start || event.status.state == "start";
      saw_planning_status_complete = saw_planning_status_complete || event.status.state == "complete";
    }
    if (event.type == agent::AgentRunnerStreamEventType::Planning) {
      saw_planning_event = true;
      SMOKE_CHECK(event.plan);
      SMOKE_CHECK(event.plan->goal == "Ship native planner");
    }
  }
  SMOKE_CHECK(saw_planning_event);
  SMOKE_CHECK(saw_planning_status_start);
  SMOKE_CHECK(saw_planning_status_complete);
  SMOKE_CHECK(planned_stream.result.plan);
  SMOKE_CHECK(agent::to_string(agent::AgentRunnerStreamEventType::Planning) == "plan");
  planned_runner.unregister_event_sink(planned_sink_id);

  int empty_planning_calls = 0;
  auto empty_static_planner = std::make_shared<agent::StaticPlanner>(
      agent::PlannerHandler([&](const agent::PlannerParams&) -> std::optional<agent::ExecutionPlan> {
        ++empty_planning_calls;
        return std::nullopt;
      }));
  agent::AgentRunner empty_planning_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .max_iterations = 1,
      .planner = empty_static_planner,
  });
  auto empty_planning_stream = empty_planning_runner.stream("Planner returns null",
                                                           "planning-empty-stream-session");
  bool saw_empty_planning_event = false;
  bool saw_empty_planning_status_start = false;
  bool saw_empty_planning_status_complete = false;
  bool saw_empty_planning_has_plan_detail = true;
  for (const auto& event : empty_planning_stream.events) {
    saw_empty_planning_event =
        saw_empty_planning_event || event.type == agent::AgentRunnerStreamEventType::Planning;
    if (event.type == agent::AgentRunnerStreamEventType::Status &&
        event.status.stage == "planning") {
      saw_empty_planning_status_start =
          saw_empty_planning_status_start || event.status.state == "start";
      saw_empty_planning_status_complete =
          saw_empty_planning_status_complete || event.status.state == "complete";
      if (event.status.state == "complete") {
        saw_empty_planning_has_plan_detail =
            event.status.details.at("hasPlan").as_bool();
      }
    }
  }
  SMOKE_CHECK(empty_planning_calls == 1);
  SMOKE_CHECK(!empty_planning_stream.result.plan);
  SMOKE_CHECK(!saw_empty_planning_event);
  SMOKE_CHECK(saw_empty_planning_status_start);
  SMOKE_CHECK(saw_empty_planning_status_complete);
  SMOKE_CHECK(!saw_empty_planning_has_plan_detail);

  int disabled_planning_calls = 0;
  auto disabled_static_planner = std::make_shared<agent::StaticPlanner>(
      agent::PlannerHandler([&](const agent::PlannerParams&) -> std::optional<agent::ExecutionPlan> {
        ++disabled_planning_calls;
        return agent::ExecutionPlan{
            .goal = "Should not run",
            .steps = {agent::PlanStep{.id = "skip", .title = "Skip disabled planning"}},
        };
      }));
  agent::AgentRunner disabled_planning_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .max_iterations = 1,
      .planner = disabled_static_planner,
  });
  auto disabled_planning_run = disabled_planning_runner.run(
      "Skip planning for run",
      "planning-disabled-run-session",
      agent::ModelSettings{},
      agent::RunnerRetrievalOptions{},
      agent::RunnerWritebackOptions{},
      {},
      agent::Value::object({}),
      std::nullopt,
      agent::AgentRunnerDurableOptions{},
      nullptr,
      false);
  SMOKE_CHECK(disabled_planning_run.text == "stream text");
  SMOKE_CHECK(!disabled_planning_run.plan);
  SMOKE_CHECK(disabled_planning_calls == 0);

  auto disabled_planning_stream = disabled_planning_runner.stream(
      "Skip planning for stream",
      "planning-disabled-stream-session",
      agent::ModelSettings{},
      agent::RunnerRetrievalOptions{},
      agent::RunnerWritebackOptions{},
      {},
      agent::Value::object({}),
      std::nullopt,
      nullptr,
      false);
  bool saw_disabled_planning_event = false;
  bool saw_disabled_planning_status = false;
  for (const auto& event : disabled_planning_stream.events) {
    saw_disabled_planning_event =
        saw_disabled_planning_event || event.type == agent::AgentRunnerStreamEventType::Planning;
    saw_disabled_planning_status =
        saw_disabled_planning_status ||
        (event.type == agent::AgentRunnerStreamEventType::Status &&
         event.status.stage == "planning");
  }
  SMOKE_CHECK(disabled_planning_stream.result.text == "stream text");
  SMOKE_CHECK(!disabled_planning_stream.result.plan);
  SMOKE_CHECK(!saw_disabled_planning_event);
  SMOKE_CHECK(!saw_disabled_planning_status);
  SMOKE_CHECK(disabled_planning_calls == 0);

  auto runner_knowledge = std::make_shared<agent::KnowledgeBase>("runner-kb", "default", "Runner KB");
  runner_knowledge->ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "kb://runner",
          .title = "Runner Knowledge",
          .content = "Native runner knowledge context should be injected before answering.",
          .source_type = "text",
      },
  });
  auto knowledge_model = std::make_shared<KnowledgeRunnerModel>();
  agent::AgentRunner knowledge_runner(agent::AgentRunnerConfig{
      .adapter = knowledge_model,
      .max_iterations = 1,
      .knowledge_base = runner_knowledge.get(),
      .knowledge_retrieval_options = agent::RunnerKnowledgeRetrievalOptions{
          .enabled = true,
          .top_k = 2,
          .min_score = 0.0,
      },
  });
  std::vector<std::string> knowledge_events;
  const auto knowledge_sink_id = knowledge_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.payload.at("source").as_string() == "knowledge-base") {
      knowledge_events.push_back(event.category);
    }
  });
  auto knowledge_run = knowledge_runner.run("native runner knowledge", "knowledge-runner-session");
  SMOKE_CHECK(knowledge_run.text == "knowledge response");
  SMOKE_CHECK(!knowledge_run.knowledge_hits.empty());
  SMOKE_CHECK(knowledge_run.knowledge_hits.front().citation.title == "Runner Knowledge");
  SMOKE_CHECK(knowledge_model->saw_knowledge_context());
  SMOKE_CHECK(contains_string(knowledge_events, "retrieval.started"));
  SMOKE_CHECK(contains_string(knowledge_events, "retrieval.completed"));
  auto knowledge_stream = knowledge_runner.stream("native runner knowledge", "knowledge-stream-session");
  bool saw_knowledge_stream_event = false;
  bool saw_knowledge_status_start = false;
  bool saw_knowledge_status_complete = false;
  for (const auto& event : knowledge_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Status &&
        event.status.stage == "knowledge-retrieval") {
      saw_knowledge_status_start = saw_knowledge_status_start || event.status.state == "start";
      saw_knowledge_status_complete = saw_knowledge_status_complete || event.status.state == "complete";
    }
    if (event.type == agent::AgentRunnerStreamEventType::KnowledgeRetrieval) {
      saw_knowledge_stream_event = true;
      SMOKE_CHECK(!event.knowledge_hits.empty());
      SMOKE_CHECK(event.knowledge_message);
    }
  }
  SMOKE_CHECK(saw_knowledge_stream_event);
  SMOKE_CHECK(saw_knowledge_status_start);
  SMOKE_CHECK(saw_knowledge_status_complete);
  SMOKE_CHECK(!knowledge_stream.result.knowledge_hits.empty());
  knowledge_runner.unregister_event_sink(knowledge_sink_id);

  agent::MediaSource runner_image_query_source;
  runner_image_query_source.kind = agent::MediaSourceKind::Inline;
  runner_image_query_source.data = "iVBORw0KGgo=";
  runner_image_query_source.mime_type = "image/png";
  runner_image_query_source.filename = "runner-query.png";
  auto runner_image_query_kb = std::make_shared<agent::KnowledgeBase>(
      "runner-image-query-kb",
      "default",
      "Runner Image Query KB",
      std::make_shared<agent::HashEmbeddingAdapter>(256, "hash-embedding", "runner-mm-v1"),
      agent::RecursiveTextChunker{},
      std::make_shared<agent::HeuristicKnowledgeReranker>(),
      std::make_shared<agent::InMemoryKnowledgeStore>(),
      std::make_shared<agent::HashImageEmbeddingAdapter>(256, "hash-image-embedding", "runner-mm-v1"));
  runner_image_query_kb->ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "doc://runner-image-query",
          .title = "Runner Knowledge Image Query",
          .content = "Sunset dashboard orange visual retrieval context.",
          .source_type = "text",
      },
  });
  agent::KnowledgeBaseManager runner_image_query_manager;
  runner_image_query_manager.register_knowledge_base(runner_image_query_kb);
  auto runner_image_query_model = std::make_shared<KnowledgeRunnerModel>();
  agent::AgentRunner runner_image_query_runner(agent::AgentRunnerConfig{
      .adapter = runner_image_query_model,
      .max_iterations = 1,
      .knowledge_base_manager = &runner_image_query_manager,
      .knowledge_retrieval_options = agent::RunnerKnowledgeRetrievalOptions{
          .enabled = true,
          .top_k = 1,
          .min_score = 0.0,
      },
  });
  std::vector<agent::FrameworkEvent> runner_image_query_events;
  runner_image_query_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.payload.at("source").as_string() == "knowledge-base-manager") {
      runner_image_query_events.push_back(event);
    }
  });
  auto runner_image_only_input = agent::create_message(
      agent::MessageRole::User,
      std::vector<agent::MessageContentPart>{
          agent::image_part(runner_image_query_source, "sunset dashboard", "high"),
      },
      agent::Value::object({{"title", "Runner Knowledge Image Query"},
                            {"textHint", "sunset dashboard orange"}}));
  auto runner_image_query_result = runner_image_query_runner.run(runner_image_only_input,
                                                                 "knowledge-image-query-session");
  SMOKE_CHECK(runner_image_query_result.text == "knowledge response");
  SMOKE_CHECK(!runner_image_query_result.knowledge_hits.empty());
  SMOKE_CHECK(runner_image_query_result.knowledge_hits.front().citation.uri == "doc://runner-image-query");
  SMOKE_CHECK(runner_image_query_model->saw_knowledge_context());
  SMOKE_CHECK(!runner_image_query_events.empty());
  SMOKE_CHECK(runner_image_query_events.front().payload.at("query").as_string().find(
             "Runner Knowledge Image Query") != std::string::npos);
  SMOKE_CHECK(runner_image_query_events.front().payload.at("query").as_string().find(
             "sunset dashboard") != std::string::npos);

  StreamingFixtureModel streaming_model;
  auto stream_events = streaming_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "stream")},
  });
  SMOKE_CHECK(stream_events.size() == 5);
  SMOKE_CHECK(stream_events[0].type == agent::ModelStreamEventType::ResponseStart);
  SMOKE_CHECK(stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(stream_events[1].delta == "thinking");
  SMOKE_CHECK(stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(stream_events[2].text == "stream text");
  SMOKE_CHECK(stream_events[3].type == agent::ModelStreamEventType::ContentPart);
  SMOKE_CHECK(stream_events[3].part.type == agent::ContentPartType::Image);
  SMOKE_CHECK(stream_events[4].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(stream_events[4].response.text == "stream text");
  std::vector<std::string> stream_lifecycle_hook_calls;
  agent::HookSet stream_lifecycle_hooks;
  stream_lifecycle_hooks.before_run = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.input.as_string() == "runner stream");
    SMOKE_CHECK(hook_context.metadata.at("sessionId").as_string() == "runner-stream-session");
    SMOKE_CHECK(hook_context.run_id == hook_context.metadata.at("runId").as_string());
    stream_lifecycle_hook_calls.push_back("before-run");
  };
  stream_lifecycle_hooks.after_run = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.input.as_string() == "runner stream");
    SMOKE_CHECK(hook_context.result.at("text").as_string() == "stream text");
    SMOKE_CHECK(hook_context.result.at("terminationReason").as_string() == "completed");
    SMOKE_CHECK(hook_context.metadata.at("sessionId").as_string() == "runner-stream-session");
    stream_lifecycle_hook_calls.push_back("after-run");
  };
  agent::AgentRunner stream_event_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<StreamingFixtureModel>(),
      .hooks = stream_lifecycle_hooks,
  });
  std::vector<agent::FrameworkEvent> stream_run_events;
  const auto stream_event_sink_id = stream_event_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category.find("run.") == 0) {
      stream_run_events.push_back(event);
    }
  });
  auto runner_stream = stream_event_runner.stream("runner stream", "runner-stream-session");
  bool saw_runner_text_delta = false;
  bool saw_runner_done = false;
  bool saw_empty_knowledge_event = false;
  bool saw_empty_memory_event = false;
  bool saw_noop_knowledge_status = false;
  bool saw_noop_memory_status = false;
  for (const auto& event : runner_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::KnowledgeRetrieval) {
      saw_empty_knowledge_event = event.knowledge_hits.empty() && !event.knowledge_message;
    }
    if (event.type == agent::AgentRunnerStreamEventType::MemoryRetrieval) {
      saw_empty_memory_event = event.memory_hits.empty() && !event.memory_message;
    }
    if (event.type == agent::AgentRunnerStreamEventType::Status &&
        event.status.stage == "knowledge-retrieval") {
      saw_noop_knowledge_status = true;
    }
    if (event.type == agent::AgentRunnerStreamEventType::Status &&
        event.status.stage == "memory-retrieval") {
      saw_noop_memory_status = true;
    }
    if (event.type == agent::AgentRunnerStreamEventType::Loop &&
        event.loop_event.type == agent::AgentLoopStreamEventType::ModelTextDelta) {
      saw_runner_text_delta = saw_runner_text_delta || event.loop_event.delta == "stream text";
    }
    if (event.type == agent::AgentRunnerStreamEventType::Done) {
      saw_runner_done = true;
      SMOKE_CHECK(event.result.text == "stream text");
    }
  }
  SMOKE_CHECK(saw_empty_knowledge_event);
  SMOKE_CHECK(saw_empty_memory_event);
  SMOKE_CHECK(!saw_noop_knowledge_status);
  SMOKE_CHECK(!saw_noop_memory_status);
  SMOKE_CHECK(saw_runner_text_delta);
  SMOKE_CHECK(saw_runner_done);
  SMOKE_CHECK(runner_stream.result.text == "stream text");
  SMOKE_CHECK(contains_string(stream_lifecycle_hook_calls, "before-run"));
  SMOKE_CHECK(contains_string(stream_lifecycle_hook_calls, "after-run"));
  stream_event_runner.unregister_event_sink(stream_event_sink_id);
  SMOKE_CHECK(stream_run_events.size() == 2);
  SMOKE_CHECK(stream_run_events[0].category == "run.started");
  SMOKE_CHECK(stream_run_events[0].target == agent::ExecutionTarget::Run);
  SMOKE_CHECK(stream_run_events[0].payload.at("sessionId").as_string() == "runner-stream-session");
  SMOKE_CHECK(!stream_run_events[0].payload.at("runId").as_string().empty());
  SMOKE_CHECK(stream_run_events[0].trace.span_name == "agent.stream");
  SMOKE_CHECK(stream_run_events[0].trace.run_id == stream_run_events[0].payload.at("runId").as_string());
  SMOKE_CHECK(stream_run_events[1].category == "run.completed");
  SMOKE_CHECK(stream_run_events[1].payload.at("iterationCount").as_integer() == 1);
  SMOKE_CHECK(stream_run_events[1].trace.trace_id == stream_run_events[0].trace.trace_id);

  auto multimodal_capture_model = std::make_shared<CapturingMultimodalModel>();
  agent::AgentRunner multimodal_runner(agent::AgentRunnerConfig{
      .adapter = multimodal_capture_model,
  });
  agent::MediaSource multimodal_image_source;
  multimodal_image_source.kind = agent::MediaSourceKind::Inline;
  multimodal_image_source.data = "iVBORw0KGgo=";
  multimodal_image_source.mime_type = "image/png";
  multimodal_image_source.filename = "screenshot.png";
  std::vector<agent::MessageContentPart> multimodal_input_parts{
      agent::text_part("describe this image"),
      agent::image_part(multimodal_image_source, "screenshot", "high"),
  };
  auto multimodal_stream = multimodal_runner.stream(std::move(multimodal_input_parts),
                                                    "runner-multimodal-stream-session");
  SMOKE_CHECK(multimodal_stream.result.text == "captured stream");
  SMOKE_CHECK(!multimodal_capture_model->messages().empty());
  const auto& multimodal_user_message = multimodal_capture_model->messages().back();
  SMOKE_CHECK(multimodal_user_message.role == agent::MessageRole::User);
  SMOKE_CHECK(multimodal_user_message.content.size() == 2);
  SMOKE_CHECK(multimodal_user_message.content[0].type == agent::ContentPartType::Text);
  SMOKE_CHECK(multimodal_user_message.content[0].text == "describe this image");
  SMOKE_CHECK(multimodal_user_message.content[1].type == agent::ContentPartType::Image);
  SMOKE_CHECK(multimodal_user_message.content[1].source.kind == agent::MediaSourceKind::Inline);
  SMOKE_CHECK(multimodal_user_message.content[1].source.data == "iVBORw0KGgo=");
  SMOKE_CHECK(multimodal_user_message.content[1].source.mime_type == "image/png");
  SMOKE_CHECK(multimodal_user_message.content[1].source.filename == "screenshot.png");

  auto multimodal_skill_capture_model = std::make_shared<CapturingMultimodalModel>();
  auto multimodal_skill_registry = std::make_shared<agent::SkillRegistry>();
  multimodal_skill_registry->register_skill(agent::SkillDefinition{
      .manifest = agent::SkillManifest{
          .name = "vision",
          .description = "Inspect image input.",
          .user_invocable = true,
      },
      .prompt = "Use the provided image.",
  });
  agent::AgentRunner multimodal_skill_runner(agent::AgentRunnerConfig{
      .adapter = multimodal_skill_capture_model,
      .skills = multimodal_skill_registry,
      .advertise_skills = false,
  });
  std::vector<agent::MessageContentPart> multimodal_skill_input_parts{
      agent::text_part("/vision describe this image"),
      agent::image_part(multimodal_image_source, "screenshot", "high"),
  };
  auto multimodal_skill_stream = multimodal_skill_runner.stream(
      std::move(multimodal_skill_input_parts), "runner-multimodal-skill-session");
  SMOKE_CHECK(multimodal_skill_stream.result.text == "captured stream");
  SMOKE_CHECK(!multimodal_skill_capture_model->messages().empty());
  const auto& multimodal_skill_user_message = multimodal_skill_capture_model->messages().back();
  SMOKE_CHECK(multimodal_skill_user_message.role == agent::MessageRole::User);
  SMOKE_CHECK(multimodal_skill_user_message.content.size() == 2);
  SMOKE_CHECK(multimodal_skill_user_message.content[0].type == agent::ContentPartType::Text);
  SMOKE_CHECK(multimodal_skill_user_message.content[0].text == "describe this image");
  SMOKE_CHECK(multimodal_skill_user_message.content[1].type == agent::ContentPartType::Image);
  SMOKE_CHECK(multimodal_skill_user_message.content[1].source.data == "iVBORw0KGgo=");

  auto agent_message_capture_model = std::make_shared<CapturingMultimodalModel>();
  std::vector<agent::Value> agent_message_hook_inputs;
  agent::HookSet agent_message_hooks;
  agent_message_hooks.before_run = [&](const agent::RunHookContext& hook_context) {
    agent_message_hook_inputs.push_back(hook_context.input);
  };
  agent::AgentRunner agent_message_runner(agent::AgentRunnerConfig{
      .adapter = agent_message_capture_model,
      .hooks = agent_message_hooks,
  });
  auto agent_message_input = agent::create_message(
      agent::MessageRole::Assistant,
      std::vector<agent::MessageContentPart>{
          agent::text_part("preserve assistant message",
                           agent::Value::object({{"part", "text"}})),
          agent::image_part(multimodal_image_source, "assistant screenshot", "low"),
      },
      agent::Value::object({{"source", "agent-message-input"}}));
  auto agent_message_result = agent_message_runner.run(agent_message_input,
                                                       "runner-agent-message-session");
  SMOKE_CHECK(agent_message_result.text == "captured");
  SMOKE_CHECK(!agent_message_hook_inputs.empty());
  SMOKE_CHECK(agent_message_hook_inputs.front().at("role").as_string() == "assistant");
  SMOKE_CHECK(agent_message_hook_inputs.front().at("metadata").at("source").as_string() ==
         "agent-message-input");
  SMOKE_CHECK(!agent_message_capture_model->messages().empty());
  const auto& captured_agent_message = agent_message_capture_model->messages().back();
  SMOKE_CHECK(captured_agent_message.role == agent::MessageRole::Assistant);
  SMOKE_CHECK(captured_agent_message.metadata.at("source").as_string() == "agent-message-input");
  SMOKE_CHECK(captured_agent_message.content.size() == 2);
  SMOKE_CHECK(captured_agent_message.content[0].text == "preserve assistant message");
  SMOKE_CHECK(captured_agent_message.content[0].metadata.at("part").as_string() == "text");
  SMOKE_CHECK(captured_agent_message.content[1].type == agent::ContentPartType::Image);

  auto agent_message_skill_capture_model = std::make_shared<CapturingMultimodalModel>();
  agent::AgentRunner agent_message_skill_runner(agent::AgentRunnerConfig{
      .adapter = agent_message_skill_capture_model,
      .skills = multimodal_skill_registry,
      .advertise_skills = false,
  });
  auto agent_message_skill_input = agent::create_message(
      agent::MessageRole::User,
      std::vector<agent::MessageContentPart>{
          agent::text_part("/vision keep the command text"),
          agent::image_part(multimodal_image_source, "screenshot", "high"),
      },
      agent::Value::object({{"source", "message-input-skill"}}));
  auto agent_message_skill_stream = agent_message_skill_runner.stream(
      agent_message_skill_input, "runner-agent-message-skill-session");
  SMOKE_CHECK(agent_message_skill_stream.result.text == "captured stream");
  SMOKE_CHECK(!agent_message_skill_capture_model->messages().empty());
  bool saw_agent_message_skill_preface = false;
  for (const auto& message : agent_message_skill_capture_model->messages()) {
    if (message.metadata.at("source").as_string() == "skill" &&
        message.metadata.at("skill").as_string() == "vision" &&
        message.metadata.at("argumentsText").as_string() == "keep the command text") {
      saw_agent_message_skill_preface = true;
    }
  }
  SMOKE_CHECK(saw_agent_message_skill_preface);
  const auto& captured_agent_message_skill =
      agent_message_skill_capture_model->messages().back();
  SMOKE_CHECK(captured_agent_message_skill.role == agent::MessageRole::User);
  SMOKE_CHECK(captured_agent_message_skill.metadata.at("source").as_string() ==
         "message-input-skill");
  SMOKE_CHECK(captured_agent_message_skill.content.size() == 2);
  SMOKE_CHECK(captured_agent_message_skill.content[0].text == "/vision keep the command text");
  SMOKE_CHECK(captured_agent_message_skill.content[1].type == agent::ContentPartType::Image);

  agent::ExecutionPolicies stream_retry_policies;
  agent::RetryPolicy stream_retry_policy;
  stream_retry_policy.max_attempts = 2;
  stream_retry_policy.strategy = agent::RetryStrategy::Fixed;
  stream_retry_policy.base_delay_ms = 0;
  stream_retry_policy.retry_on = [](const agent::RetryContext& retry) {
    return retry.target == agent::ExecutionTarget::Model
           && retry.error.find("temporary stream failure") != std::string::npos;
  };
  stream_retry_policies.retry[agent::ExecutionTarget::Model] = stream_retry_policy;
  auto flaky_stream_model = std::make_shared<FlakyBeforeStartStreamingModel>();
  agent::AgentRunner flaky_stream_runner(agent::AgentRunnerConfig{
      .adapter = flaky_stream_model,
      .max_iterations = 1,
      .execution_policies = stream_retry_policies,
  });
  std::vector<agent::FrameworkEvent> stream_retry_events;
  const auto flaky_stream_sink_id = flaky_stream_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category == "retry.scheduled") {
      stream_retry_events.push_back(event);
    }
  });
  auto flaky_stream_result = flaky_stream_runner.stream("retry stream", "runner-flaky-stream-session");
  flaky_stream_runner.unregister_event_sink(flaky_stream_sink_id);
  SMOKE_CHECK(flaky_stream_model->stream_calls() == 2);
  SMOKE_CHECK(flaky_stream_result.result.text == "retried stream");
  SMOKE_CHECK(stream_retry_events.size() == 1);
  SMOKE_CHECK(stream_retry_events.front().target == agent::ExecutionTarget::Model);
  SMOKE_CHECK(stream_retry_events.front().payload.at("stream").as_bool());

  agent::ExecutionPolicies no_replay_policies;
  agent::RetryPolicy no_replay_policy;
  no_replay_policy.max_attempts = 3;
  no_replay_policy.strategy = agent::RetryStrategy::Fixed;
  no_replay_policy.base_delay_ms = 0;
  no_replay_policy.retry_on = [](const agent::RetryContext& retry) {
    return retry.target == agent::ExecutionTarget::Model;
  };
  no_replay_policies.retry[agent::ExecutionTarget::Model] = no_replay_policy;
  auto after_delta_stream_model = std::make_shared<FailingAfterDeltaStreamingModel>();
  agent::AgentRunner after_delta_stream_runner(agent::AgentRunnerConfig{
      .adapter = after_delta_stream_model,
      .max_iterations = 1,
      .execution_policies = no_replay_policies,
  });
  std::vector<agent::FrameworkEvent> no_replay_retry_events;
  const auto after_delta_sink_id = after_delta_stream_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category == "retry.scheduled") {
      no_replay_retry_events.push_back(event);
    }
  });
  bool after_delta_stream_threw = false;
  try {
    (void)after_delta_stream_runner.stream("no replay stream", "runner-after-delta-stream-session");
    SMOKE_CHECK(false);
  } catch (const std::runtime_error& error) {
    after_delta_stream_threw = std::string(error.what()).find("stream failed after output") != std::string::npos;
  }
  after_delta_stream_runner.unregister_event_sink(after_delta_sink_id);
  SMOKE_CHECK(after_delta_stream_threw);
  SMOKE_CHECK(after_delta_stream_model->stream_calls() == 1);
  SMOKE_CHECK(after_delta_stream_model->emitted_delta());
  SMOKE_CHECK(no_replay_retry_events.empty());

  auto tool_stream = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<ToolCallingModel>(),
      .tools = tools,
      .max_iterations = 4,
  }).stream("stream tool", "runner-tool-stream-session");
  bool saw_tool_start = false;
  bool saw_tool_complete = false;
  bool saw_tools_aggregate = false;
  bool saw_tool_status_start = false;
  bool saw_model_status_complete = false;
  bool saw_final_tool_text_delta = false;
  for (const auto& event : tool_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Status) {
      if (event.status.stage == "tool" && event.status.state == "start") {
        saw_tool_status_start = saw_tool_status_start || event.status.tool_name == "math.eval";
      }
      if (event.status.stage == "model" && event.status.state == "complete") {
        saw_model_status_complete = true;
      }
    }
    if (event.type != agent::AgentRunnerStreamEventType::Loop) {
      continue;
    }
    saw_tool_start = saw_tool_start || event.loop_event.type == agent::AgentLoopStreamEventType::ToolStart;
    saw_tool_complete = saw_tool_complete || event.loop_event.type == agent::AgentLoopStreamEventType::ToolComplete;
    if (event.loop_event.type == agent::AgentLoopStreamEventType::Tools) {
      saw_tools_aggregate = saw_tools_aggregate || event.loop_event.tool_results.size() == 1;
    }
    if (event.loop_event.type == agent::AgentLoopStreamEventType::ModelTextDelta) {
      saw_final_tool_text_delta =
          saw_final_tool_text_delta || event.loop_event.delta.find("tool result") != std::string::npos;
    }
  }
  SMOKE_CHECK(saw_tool_start);
  SMOKE_CHECK(saw_tool_complete);
  SMOKE_CHECK(saw_tools_aggregate);
  SMOKE_CHECK(saw_tool_status_start);
  SMOKE_CHECK(saw_model_status_complete);
  SMOKE_CHECK(saw_final_tool_text_delta);
  SMOKE_CHECK(tool_stream.result.text.find("tool result") != std::string::npos);

  auto throwing_tool_model = std::make_shared<ThrowingToolRecoveryModel>();
  auto throwing_tool = agent::define_tool(agent::ToolDefinition{
      .name = "explode",
      .description = "Always throws for stream failure recovery.",
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        throw std::runtime_error("boom");
      },
  });
  auto throwing_tool_stream = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = throwing_tool_model,
      .tools = {throwing_tool},
      .max_iterations = 3,
  }).stream("stream tool failure", "runner-tool-failure-stream-session");
  std::vector<agent::ToolExecutionResult> failed_tool_stream_results;
  std::vector<agent::ToolExecutionResult> failed_tool_aggregate_results;
  bool saw_tool_failure_done = false;
  for (const auto& event : throwing_tool_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Loop &&
        event.loop_event.type == agent::AgentLoopStreamEventType::ToolComplete) {
      failed_tool_stream_results.push_back(event.loop_event.tool_result);
    }
    if (event.type == agent::AgentRunnerStreamEventType::Loop &&
        event.loop_event.type == agent::AgentLoopStreamEventType::Tools) {
      failed_tool_aggregate_results = event.loop_event.tool_results;
    }
    if (event.type == agent::AgentRunnerStreamEventType::Done) {
      saw_tool_failure_done = true;
    }
  }
  SMOKE_CHECK(saw_tool_failure_done);
  SMOKE_CHECK(failed_tool_stream_results.size() == 1);
  SMOKE_CHECK(!failed_tool_stream_results.front().ok);
  SMOKE_CHECK(failed_tool_stream_results.front().tool_call.name == "explode");
  SMOKE_CHECK(failed_tool_stream_results.front().error == "Tool \"explode\" failed.");
  SMOKE_CHECK(failed_tool_stream_results.front().output.find("boom") != std::string::npos);
  SMOKE_CHECK(failed_tool_aggregate_results.size() == 1);
  SMOKE_CHECK(!failed_tool_aggregate_results.front().ok);
  SMOKE_CHECK(throwing_tool_stream.result.text == "recovered after tool failure");
  SMOKE_CHECK(throwing_tool_model->generate_calls() == 2);
  SMOKE_CHECK(throwing_tool_model->saw_failed_tool_message());

  auto executor_failure_model = std::make_shared<ExecutorFailureRecoveryModel>();
  auto permission_gated_tool = agent::define_tool(agent::ToolDefinition{
      .name = "permission.gated",
      .description = "Needs permission hook processing.",
      .capabilities = {"process.exec"},
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value("should not run");
      },
  });
  agent::HookSet executor_failure_hooks;
  executor_failure_hooks.before_permission_check = [](const agent::PermissionHookContext&) {
    throw std::runtime_error("permission hook crashed");
  };
  auto executor_failure_stream = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = executor_failure_model,
      .tools = {permission_gated_tool},
      .max_iterations = 3,
      .hooks = executor_failure_hooks,
      .permission_policy = [](const agent::PermissionRequest&) {
        return agent::PermissionDecision{agent::PermissionDecisionKind::Allow, {}};
      },
  }).stream("stream executor failure", "runner-executor-failure-stream-session");
  std::vector<agent::ToolExecutionResult> synthetic_tool_stream_results;
  for (const auto& event : executor_failure_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Loop &&
        event.loop_event.type == agent::AgentLoopStreamEventType::ToolComplete) {
      synthetic_tool_stream_results.push_back(event.loop_event.tool_result);
    }
  }
  SMOKE_CHECK(synthetic_tool_stream_results.size() == 1);
  SMOKE_CHECK(!synthetic_tool_stream_results.front().ok);
  SMOKE_CHECK(synthetic_tool_stream_results.front().tool_call.name == "permission.gated");
  SMOKE_CHECK(synthetic_tool_stream_results.front().error ==
         "Tool \"permission.gated\" threw an unexpected error.");
  SMOKE_CHECK(executor_failure_stream.result.text == "recovered after executor failure");
  SMOKE_CHECK(executor_failure_model->generate_calls() == 2);
  SMOKE_CHECK(executor_failure_model->saw_synthetic_tool_message());

  auto incomplete_stream = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<IncompleteFinishStreamingModel>(),
      .max_iterations = 1,
  }).stream("long stream", "runner-incomplete-stream-session");
  bool saw_incomplete_done = false;
  for (const auto& event : incomplete_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Done) {
      saw_incomplete_done = true;
      SMOKE_CHECK(event.result.termination_reason == agent::AgentLoopTerminationReason::IncompleteResponse);
      SMOKE_CHECK(event.result.text == "partial answer that got cut off");
    }
  }
  SMOKE_CHECK(saw_incomplete_done);
  SMOKE_CHECK(incomplete_stream.result.termination_reason == agent::AgentLoopTerminationReason::IncompleteResponse);
  SMOKE_CHECK(incomplete_stream.result.text == "partial answer that got cut off");

  auto counting_tool_loop_model = std::make_shared<CountingToolLoopModel>();
  auto noop_tool = agent::define_tool(agent::ToolDefinition{
      .name = "noop",
      .description = "Returns a stable value for max-iteration stream tests.",
      .execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value("ok");
      },
  });
  auto max_iteration_stream = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = counting_tool_loop_model,
      .tools = {noop_tool},
      .max_iterations = 3,
  }).stream("loop forever", "runner-max-iteration-stream-session");
  bool saw_max_iteration_done = false;
  for (const auto& event : max_iteration_stream.events) {
    if (event.type == agent::AgentRunnerStreamEventType::Done) {
      saw_max_iteration_done = true;
      SMOKE_CHECK(event.result.termination_reason == agent::AgentLoopTerminationReason::MaxIterations);
      SMOKE_CHECK(event.result.iteration_count == 3);
    }
  }
  SMOKE_CHECK(saw_max_iteration_done);
  SMOKE_CHECK(max_iteration_stream.result.termination_reason == agent::AgentLoopTerminationReason::MaxIterations);
  SMOKE_CHECK(max_iteration_stream.result.iteration_count == 3);
  SMOKE_CHECK(max_iteration_stream.result.text == "progress 3");
  SMOKE_CHECK(counting_tool_loop_model->generate_calls() == 3);

  std::vector<std::string> crashing_stream_hook_calls;
  agent::HookSet crashing_stream_hooks;
  crashing_stream_hooks.before_run = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.input.as_string() == "crash stream");
    SMOKE_CHECK(hook_context.metadata.at("sessionId").as_string() == "runner-crashing-stream-session");
    crashing_stream_hook_calls.push_back("before-run");
  };
  crashing_stream_hooks.on_run_error = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.input.as_string() == "crash stream");
    SMOKE_CHECK(hook_context.error.find("model adapter crashed mid-stream") != std::string::npos);
    SMOKE_CHECK(hook_context.metadata.at("sessionId").as_string() == "runner-crashing-stream-session");
    crashing_stream_hook_calls.push_back("on-run-error");
  };
  agent::AgentRunner crashing_stream_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<CrashingStreamModel>(),
      .max_iterations = 1,
      .hooks = crashing_stream_hooks,
  });
  std::vector<agent::FrameworkEvent> crashing_stream_events;
  const auto crashing_stream_sink_id = crashing_stream_runner.register_event_sink(
      [&](const agent::FrameworkEvent& event) {
        if (event.category == "model.failed" || event.category == "run.failed") {
          crashing_stream_events.push_back(event);
        }
      });
  bool crashing_stream_threw = false;
  try {
    (void)crashing_stream_runner.stream("crash stream", "runner-crashing-stream-session");
    SMOKE_CHECK(false);
  } catch (const std::runtime_error& error) {
    crashing_stream_threw = std::string(error.what()).find("model adapter crashed mid-stream") != std::string::npos;
  }
  crashing_stream_runner.unregister_event_sink(crashing_stream_sink_id);
  SMOKE_CHECK(crashing_stream_threw);
  SMOKE_CHECK(contains_string(crashing_stream_hook_calls, "before-run"));
  SMOKE_CHECK(contains_string(crashing_stream_hook_calls, "on-run-error"));
  const auto crashing_stream_snapshot = crashing_stream_runner.last_stream_events();
  SMOKE_CHECK(std::find_if(crashing_stream_snapshot.begin(), crashing_stream_snapshot.end(),
                      [](const agent::AgentRunnerStreamEvent& event) {
                        return event.type == agent::AgentRunnerStreamEventType::Error
                               && event.error.at("message").as_string().find("model adapter crashed mid-stream")
                                      != std::string::npos
                               && event.error.at("name").as_string() == "Error";
                      })
         != crashing_stream_snapshot.end());
  SMOKE_CHECK(std::find_if(crashing_stream_events.begin(), crashing_stream_events.end(),
                      [](const agent::FrameworkEvent& event) {
                        return event.category == "model.failed"
                               && event.payload.at("error").as_string().find("model adapter crashed mid-stream")
                                      != std::string::npos;
                      })
         != crashing_stream_events.end());
  SMOKE_CHECK(std::find_if(crashing_stream_events.begin(), crashing_stream_events.end(),
                      [](const agent::FrameworkEvent& event) {
                        return event.category == "run.failed"
                               && event.payload.at("error").as_string().find("model adapter crashed mid-stream")
                                      != std::string::npos;
                      })
         != crashing_stream_events.end());

  std::atomic<bool> cancellable_stream_started{false};
  auto cancellable_stream_model = std::make_shared<CancellableStreamingModel>(cancellable_stream_started);
  agent::AgentRunner cancellable_stream_runner(agent::AgentRunnerConfig{
      .adapter = cancellable_stream_model,
      .max_iterations = 1,
  });
  agent::CancellationToken stream_cancellation;
  bool cancellable_stream_threw = false;
  std::string cancellable_stream_error;
  std::thread cancellable_stream_thread([&]() {
    try {
      (void)cancellable_stream_runner.stream("abort stream", "runner-cancellable-stream-session",
                                             {}, {}, {}, {}, agent::Value::object({}), std::nullopt,
                                             &stream_cancellation);
    } catch (const std::exception& error) {
      cancellable_stream_threw = true;
      cancellable_stream_error = error.what();
    }
  });
  for (int attempt = 0; attempt < 100 && !cancellable_stream_started.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  SMOKE_CHECK(cancellable_stream_started.load());
  stream_cancellation.cancel("caller aborted");
  cancellable_stream_thread.join();
  SMOKE_CHECK(cancellable_stream_threw);
  SMOKE_CHECK(cancellable_stream_error.find("caller aborted") != std::string::npos);
  const auto cancellable_stream_snapshot = cancellable_stream_runner.last_stream_events();
  SMOKE_CHECK(std::find_if(cancellable_stream_snapshot.begin(), cancellable_stream_snapshot.end(),
                      [](const agent::AgentRunnerStreamEvent& event) {
                        return event.type == agent::AgentRunnerStreamEventType::Error
                               && event.error.at("message").as_string().find("caller aborted") != std::string::npos;
                      })
         != cancellable_stream_snapshot.end());

  agent::ToolRegistry durable_tool_registry(tools);
  agent::ToolExecutor durable_tool_executor(durable_tool_registry);
  agent::EmbeddedContextManager durable_context_manager;
  auto durable_model = std::make_shared<ToolCallingModel>();
  agent::AgentLoop durable_loop(agent::AgentLoopConfig{
      .model = durable_model,
      .tool_registry = &durable_tool_registry,
      .tool_executor = &durable_tool_executor,
      .context_manager = &durable_context_manager,
      .max_iterations = 4,
  });
  agent::SessionMemory durable_session(agent::SessionMemoryOptions{.storage = {.session_id = "loop-durable-session"}});
  std::vector<agent::AgentLoopDurableState> durable_checkpoints;
  auto durable_result = durable_loop.run(
      durable_session,
      "checkpoint runtime",
      {},
      {},
      {},
      agent::Value::object({{"durable", true}}),
      agent::AgentLoopDurableOptions{
          .on_checkpoint = [&](const agent::AgentLoopDurableState& state) {
            durable_checkpoints.push_back(state);
          },
      });
  SMOKE_CHECK(durable_result.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(durable_model->generate_calls() == 2);
  SMOKE_CHECK(durable_checkpoints.size() >= 6);
  SMOKE_CHECK(durable_checkpoints.front().phase == agent::AgentLoopDurablePhase::BeforeModel);
  const auto model_completed = std::find_if(
      durable_checkpoints.begin(), durable_checkpoints.end(), [](const agent::AgentLoopDurableState& state) {
        return state.phase == agent::AgentLoopDurablePhase::ModelCompleted
               && state.next_iteration == 0
               && state.last_response
               && !state.last_response->tool_calls.empty();
      });
  SMOKE_CHECK(model_completed != durable_checkpoints.end());
  SMOKE_CHECK(std::find_if(durable_checkpoints.begin(), durable_checkpoints.end(),
                      [](const agent::AgentLoopDurableState& state) {
                        return state.phase == agent::AgentLoopDurablePhase::ToolsCompleted
                               && state.next_iteration == 1;
                      })
         != durable_checkpoints.end());
  SMOKE_CHECK(durable_checkpoints.back().phase == agent::AgentLoopDurablePhase::Completed);
  SMOKE_CHECK(durable_checkpoints.back().termination_reason == agent::AgentLoopTerminationReason::Completed);
  auto durable_state_value = agent::agent_loop_durable_state_to_value(durable_checkpoints.back());
  SMOKE_CHECK(durable_state_value.at("version").as_integer() == 1);
  SMOKE_CHECK(durable_state_value.at("phase").as_string() == "completed");
  SMOKE_CHECK(durable_state_value.at("session").at("messages").as_array().size() >= 4);
  SMOKE_CHECK(durable_state_value.at("trace").as_array().size() >= 3);

  auto resumed_model = std::make_shared<ToolCallingModel>();
  agent::ToolExecutor resumed_tool_executor(durable_tool_registry);
  agent::EmbeddedContextManager resumed_context_manager;
  agent::AgentLoop resumed_loop(agent::AgentLoopConfig{
      .model = resumed_model,
      .tool_registry = &durable_tool_registry,
      .tool_executor = &resumed_tool_executor,
      .context_manager = &resumed_context_manager,
      .max_iterations = 4,
  });
  agent::SessionMemory resumed_session(agent::SessionMemoryOptions{.storage = {.session_id = "loop-durable-session"}});
  std::vector<agent::AgentLoopDurableState> resumed_checkpoints;
  auto resumed_result = resumed_loop.run(
      resumed_session,
      "checkpoint runtime",
      {},
      {},
      {},
      agent::Value::object({{"durable", true}}),
      agent::AgentLoopDurableOptions{
          .resume_state = *model_completed,
          .on_checkpoint = [&](const agent::AgentLoopDurableState& state) {
            resumed_checkpoints.push_back(state);
          },
      });
  SMOKE_CHECK(resumed_result.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(resumed_model->generate_calls() == 1);
  SMOKE_CHECK(!resumed_checkpoints.empty());
  SMOKE_CHECK(resumed_checkpoints.front().phase == agent::AgentLoopDurablePhase::BeforeModel);
  SMOKE_CHECK(resumed_checkpoints.back().phase == agent::AgentLoopDurablePhase::Completed);

  auto runner_durable_model = std::make_shared<ToolCallingModel>();
  agent::AgentRunner durable_runner(agent::AgentRunnerConfig{
      .adapter = runner_durable_model,
      .tools = tools,
      .max_iterations = 4,
  });
  std::vector<agent::AgentRunnerDurableState> runner_checkpoints;
  auto runner_durable_result = durable_runner.run(
      "runner checkpoint runtime",
      "runner-durable-session",
      {},
      {},
      {},
      {},
      agent::Value::object({{"durable", "runner"}}),
      std::nullopt,
      agent::AgentRunnerDurableOptions{
          .on_checkpoint = [&](const agent::AgentRunnerDurableState& state) {
            runner_checkpoints.push_back(state);
          },
      });
  SMOKE_CHECK(runner_durable_result.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(runner_durable_model->generate_calls() == 2);
  SMOKE_CHECK(!runner_checkpoints.empty());
  SMOKE_CHECK(runner_checkpoints.front().status == agent::AgentRunnerDurableStatus::Running);
  SMOKE_CHECK(!runner_checkpoints.front().loop);
  SMOKE_CHECK(runner_checkpoints.back().status == agent::AgentRunnerDurableStatus::Completed);
  SMOKE_CHECK(runner_checkpoints.back().loop);
  SMOKE_CHECK(runner_checkpoints.back().loop->phase == agent::AgentLoopDurablePhase::Completed);
  const auto runner_model_completed = std::find_if(
      runner_checkpoints.begin(), runner_checkpoints.end(), [](const agent::AgentRunnerDurableState& state) {
        return state.status == agent::AgentRunnerDurableStatus::Running
               && state.loop
               && state.loop->phase == agent::AgentLoopDurablePhase::ModelCompleted
               && state.loop->last_response
               && !state.loop->last_response->tool_calls.empty();
      });
  SMOKE_CHECK(runner_model_completed != runner_checkpoints.end());
  auto runner_durable_value = agent::agent_runner_durable_state_to_value(runner_checkpoints.back());
  SMOKE_CHECK(runner_durable_value.at("version").as_integer() == 1);
  SMOKE_CHECK(runner_durable_value.at("status").as_string() == "completed");
  SMOKE_CHECK(runner_durable_value.at("runId").as_string() == runner_checkpoints.back().run_id);
  SMOKE_CHECK(runner_durable_value.at("prefaceMessages").is_array());
  SMOKE_CHECK(runner_durable_value.at("loop").at("phase").as_string() == "completed");
  agent::AgentRunnerDurableState durable_memory_state;
  durable_memory_state.memory_hits = {agent::RetrievedMemory{.id = "durable-memory",
                                                              .content = "Durable memory hit",
                                                              .score = 0.9,
                                                              .metadata = agent::Value::object({{"kind", "smoke"}}),
                                                              .namespace_id = "durable-ns"}};
  auto durable_memory_value = agent::agent_runner_durable_state_to_value(durable_memory_state);
  const auto& durable_memory_hit = durable_memory_value.at("memoryHits").as_array().front();
  SMOKE_CHECK(durable_memory_hit.at("namespace").as_string() == "durable-ns");
  SMOKE_CHECK(!durable_memory_hit.contains("namespaceId"));

  auto resumed_runner_model = std::make_shared<ToolCallingModel>();
  agent::AgentRunner resumed_runner(agent::AgentRunnerConfig{
      .adapter = resumed_runner_model,
      .tools = tools,
      .max_iterations = 4,
  });
  std::vector<agent::AgentRunnerDurableState> resumed_runner_checkpoints;
  auto resumed_runner_result = resumed_runner.run(
      "runner checkpoint runtime",
      "ignored-session-on-resume",
      {},
      {},
      {},
      {},
      agent::Value::object({{"durable", "runner"}}),
      std::nullopt,
      agent::AgentRunnerDurableOptions{
          .resume_state = *runner_model_completed,
          .on_checkpoint = [&](const agent::AgentRunnerDurableState& state) {
            resumed_runner_checkpoints.push_back(state);
          },
      });
  SMOKE_CHECK(resumed_runner_result.session_id == "runner-durable-session");
  SMOKE_CHECK(resumed_runner_result.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(resumed_runner_model->generate_calls() == 1);
  SMOKE_CHECK(!resumed_runner_checkpoints.empty());
  SMOKE_CHECK(resumed_runner_checkpoints.front().run_id == runner_model_completed->run_id);
  SMOKE_CHECK(resumed_runner_checkpoints.back().status == agent::AgentRunnerDurableStatus::Completed);

  agent::WebSearchProviderRegistry service_web_registry({agent::StaticWebSearchProvider("svc")});
  agent::ToolExecutionServices configured_services;
  configured_services.web_search_registry = &service_web_registry;
  configured_services.values = agent::Value::object({
      {"web", agent::Value::object({{"configuredLayer", "runner"}})},
      {"custom", agent::Value::object({{"fromConfigured", true}})},
      {"session", agent::Value::object({{"configuredKey", "cfg"}})},
  });
  auto context_inspect_tool = agent::define_tool(agent::ToolDefinition{
      .name = "context.inspect",
      .description = "Inspect the injected tool execution context.",
      .risk_level = agent::ToolRiskLevel::Low,
      .execute = [&](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        SMOKE_CHECK(context.service_refs.session != nullptr);
        SMOKE_CHECK(context.service_refs.long_term_memory == memory.get());
        SMOKE_CHECK(context.service_refs.knowledge_base == runner_knowledge.get());
        SMOKE_CHECK(context.service_refs.web_search_registry == &service_web_registry);
        SMOKE_CHECK(context.services.at("sessionId").as_string() == "context-service-session");
        SMOKE_CHECK(context.services.at("session").at("sessionId").as_string() == "context-service-session");
        SMOKE_CHECK(context.services.at("session").at("configuredKey").as_string() == "cfg");
        SMOKE_CHECK(context.services.at("session").at("contextKey").as_string() == "ctx");
        SMOKE_CHECK(context.services.at("knowledge").at("longTermMemoryAvailable").as_bool());
        SMOKE_CHECK(context.services.at("knowledge").at("knowledgeBaseAvailable").as_bool());
        SMOKE_CHECK(context.services.at("web").at("configuredLayer").as_string() == "runner");
        SMOKE_CHECK(context.services.at("web").at("contextLayer").as_string() == "request");
        SMOKE_CHECK(context.services.at("custom").at("fromConfigured").as_bool());
        SMOKE_CHECK(context.services.at("custom").at("fromContext").as_bool());
        SMOKE_CHECK(context.attributes.at("context").at("requestId").as_string() == "ctx-1");
        SMOKE_CHECK(context.attributes.at("iteration").as_integer() == 0);
        return agent::Value::object({{"sessionId", context.services.at("sessionId")},
                                    {"webContext", context.services.at("web").at("contextLayer")}});
      },
  });
  auto context_runner = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<ContextServiceModel>(),
      .tools = {context_inspect_tool},
      .max_iterations = 3,
      .long_term_memory = memory,
      .knowledge_base = runner_knowledge.get(),
      .knowledge_retrieval_options = agent::RunnerKnowledgeRetrievalOptions{.enabled = false},
      .tool_services = configured_services,
  });
  auto context_result = context_runner.run(
      "inspect context",
      "context-service-session",
      {},
      {},
      {},
      {},
      agent::Value::object({
          {"requestId", "ctx-1"},
          {"services", agent::Value::object({
                           {"web", agent::Value::object({{"contextLayer", "request"}})},
                           {"custom", agent::Value::object({{"fromContext", true}})},
                           {"session", agent::Value::object({{"contextKey", "ctx"}})},
                       })},
      }));
  SMOKE_CHECK(context_result.text.find("context-service-session") != std::string::npos);
  SMOKE_CHECK(context_result.text.find("request") != std::string::npos);

  std::vector<std::string> runtime_hook_calls;
  agent::HookSet runtime_hooks;
  runtime_hooks.before_run = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.input.as_string() == "hooked runtime");
    runtime_hook_calls.push_back("before-run");
  };
  runtime_hooks.after_run = [&](const agent::RunHookContext& hook_context) {
    SMOKE_CHECK(hook_context.result.at("text").as_string().find("tool result") != std::string::npos);
    runtime_hook_calls.push_back("after-run");
  };
  runtime_hooks.before_model = [&](const agent::ModelHookContext& hook_context) {
    SMOKE_CHECK(hook_context.request.at("messageCount").as_integer() >= 1);
    runtime_hook_calls.push_back("before-model");
  };
  runtime_hooks.after_model = [&](const agent::ModelHookContext& hook_context) {
    SMOKE_CHECK(!hook_context.response.at("finishReason").as_string().empty()
           || !hook_context.response.at("text").as_string().empty());
    runtime_hook_calls.push_back("after-model");
  };
  runtime_hooks.before_permission_check = [&](const agent::PermissionHookContext& hook_context) {
    SMOKE_CHECK(hook_context.tool_name == "math.eval");
    runtime_hook_calls.push_back("before-permission");
  };
  runtime_hooks.after_permission_check = [&](const agent::PermissionHookContext& hook_context) {
    SMOKE_CHECK(hook_context.decision == "allow");
    runtime_hook_calls.push_back("after-permission");
  };
  runtime_hooks.before_tool = [&](const agent::ToolHookContext& hook_context) {
    SMOKE_CHECK(hook_context.tool_name == "math.eval");
    runtime_hook_calls.push_back("before-tool");
  };
  runtime_hooks.after_tool = [&](const agent::ToolHookContext& hook_context) {
    SMOKE_CHECK(hook_context.result.as_string().find("3") != std::string::npos);
    runtime_hook_calls.push_back("after-tool");
  };
  auto runtime_hook_runner = agent::AgentRunner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<ToolCallingModel>(),
      .tools = tools,
      .max_iterations = 4,
      .hooks = runtime_hooks,
      .permission_policy = [](const agent::PermissionRequest&) {
        return agent::PermissionDecision{agent::PermissionDecisionKind::Allow, "hook allow"};
      },
  });
  std::vector<agent::FrameworkEvent> runtime_events;
  std::vector<agent::FrameworkEvent> runtime_run_events;
  const auto runtime_event_sink_id = runtime_hook_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    runtime_events.push_back(event);
    if (event.category.find("run.") == 0) {
      runtime_run_events.push_back(event);
    }
  });
  auto runtime_hook_result = runtime_hook_runner.run("hooked runtime", "runtime-hook-session");
  runtime_hook_runner.unregister_event_sink(runtime_event_sink_id);
  SMOKE_CHECK(runtime_hook_result.text.find("tool result") != std::string::npos);
  SMOKE_CHECK(contains_string(runtime_hook_calls, "before-run"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "after-run"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "before-model"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "after-model"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "before-permission"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "after-permission"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "before-tool"));
  SMOKE_CHECK(contains_string(runtime_hook_calls, "after-tool"));
  SMOKE_CHECK(runtime_run_events.size() == 2);
  SMOKE_CHECK(runtime_run_events[0].category == "run.started");
  SMOKE_CHECK(runtime_run_events[0].target == agent::ExecutionTarget::Run);
  SMOKE_CHECK(runtime_run_events[0].payload.at("input").as_string() == "hooked runtime");
  SMOKE_CHECK(runtime_run_events[1].category == "run.completed");
  SMOKE_CHECK(runtime_run_events[1].payload.at("sessionId").as_string() == runtime_hook_result.session_id);
  SMOKE_CHECK(runtime_run_events[1].payload.at("memoryHitCount").as_integer() == 0);
  SMOKE_CHECK(!runtime_run_events[0].trace.trace_id.empty());
  SMOKE_CHECK(!runtime_run_events[0].trace.span_id.empty());
  SMOKE_CHECK(runtime_run_events[0].trace.span_name == "agent.run");
  SMOKE_CHECK(runtime_run_events[0].trace.run_id == runtime_run_events[0].payload.at("runId").as_string());
  SMOKE_CHECK(runtime_run_events[1].trace.trace_id == runtime_run_events[0].trace.trace_id);
  SMOKE_CHECK(runtime_run_events[1].trace.span_id == runtime_run_events[0].trace.span_id);
  const auto runtime_model_started = std::find_if(
      runtime_events.begin(), runtime_events.end(),
      [](const agent::FrameworkEvent& event) { return event.category == "model.started"; });
  SMOKE_CHECK(runtime_model_started != runtime_events.end());
  SMOKE_CHECK(runtime_model_started->trace.trace_id == runtime_run_events[0].trace.trace_id);
  SMOKE_CHECK(runtime_model_started->trace.parent_span_id == runtime_run_events[0].trace.span_id);
  SMOKE_CHECK(!runtime_model_started->trace.span_id.empty());
  const auto runtime_tool_completed = std::find_if(
      runtime_events.begin(), runtime_events.end(),
      [](const agent::FrameworkEvent& event) { return event.category == "tool.completed"; });
  SMOKE_CHECK(runtime_tool_completed != runtime_events.end());
  SMOKE_CHECK(runtime_tool_completed->trace.trace_id == runtime_run_events[0].trace.trace_id);
  SMOKE_CHECK(runtime_tool_completed->trace.parent_span_id == runtime_run_events[0].trace.span_id);
  SMOKE_CHECK(!runtime_tool_completed->trace.span_id.empty());

  agent::AgentRunner failing_runner(agent::AgentRunnerConfig{
      .adapter = std::make_shared<FailingRuntimeModel>(),
  });
  std::vector<agent::FrameworkEvent> failed_run_events;
  const auto failed_event_sink_id = failing_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category.find("run.") == 0) {
      failed_run_events.push_back(event);
    }
  });
  try {
    (void)failing_runner.run("fail runtime", "runtime-failed-session");
    SMOKE_CHECK(false);
  } catch (const std::runtime_error& error) {
    SMOKE_CHECK(std::string(error.what()).find("runtime model failed") != std::string::npos);
  }
  failing_runner.unregister_event_sink(failed_event_sink_id);
  SMOKE_CHECK(failed_run_events.size() == 2);
  SMOKE_CHECK(failed_run_events[0].category == "run.started");
  SMOKE_CHECK(failed_run_events[1].category == "run.failed");
  SMOKE_CHECK(failed_run_events[1].target == agent::ExecutionTarget::Run);
  SMOKE_CHECK(failed_run_events[1].payload.at("sessionId").as_string() == "runtime-failed-session");
  SMOKE_CHECK(failed_run_events[1].payload.at("error").as_string().find("runtime model failed") != std::string::npos);

  agent::RetryPolicy retry_delay_policy;
  retry_delay_policy.strategy = agent::RetryStrategy::None;
  retry_delay_policy.base_delay_ms = 25;
  retry_delay_policy.max_delay_ms = 100;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 0);
  retry_delay_policy.strategy = agent::RetryStrategy::Fixed;
  retry_delay_policy.max_delay_ms = 5;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 25);
  retry_delay_policy.strategy = agent::RetryStrategy::Exponential;
  retry_delay_policy.max_delay_ms = 0;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 25);
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 3) == 25);
  retry_delay_policy.max_delay_ms = 100;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 25);
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 2) == 50);
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 3) == 100);
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 4) == 100);
  retry_delay_policy.strategy = agent::RetryStrategy::Fixed;
  retry_delay_policy.jitter = true;
  retry_delay_policy.base_delay_ms = 1;
  retry_delay_policy.max_delay_ms = 0;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 0);
  retry_delay_policy.base_delay_ms = 2;
  SMOKE_CHECK(agent::resolve_retry_delay_ms(retry_delay_policy, 1) == 1);
  agent::TimeoutPolicy retry_timeout_policy;
  retry_timeout_policy.timeout_ms[agent::ExecutionTarget::Permission] = 1;
  retry_timeout_policy.timeout_ms[agent::ExecutionTarget::Model] = 1;
  SMOKE_CHECK(agent::timeout_ms_for_target(retry_timeout_policy, agent::ExecutionTarget::Permission) == 0);
  SMOKE_CHECK(agent::timeout_ms_for_target(retry_timeout_policy, agent::ExecutionTarget::Model) == 1);

  agent::ExecutionPolicies retry_policies;
  agent::RetryPolicy model_retry_policy;
  model_retry_policy.max_attempts = 2;
  model_retry_policy.strategy = agent::RetryStrategy::Fixed;
  model_retry_policy.base_delay_ms = 0;
  model_retry_policy.retry_on = [](const agent::RetryContext& retry) {
    return retry.target == agent::ExecutionTarget::Model
           && retry.attempt == 1
           && retry.error.find("transient model failure") != std::string::npos;
  };
  retry_policies.retry[agent::ExecutionTarget::Model] = model_retry_policy;
  auto retry_model = std::make_shared<RetryingRuntimeModel>();
  agent::AgentRunner retry_runner(agent::AgentRunnerConfig{
      .adapter = retry_model,
      .max_iterations = 1,
      .execution_policies = retry_policies,
  });
  std::vector<agent::FrameworkEvent> retry_events;
  const auto retry_sink_id = retry_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category == "retry.scheduled") {
      retry_events.push_back(event);
    }
  });
  auto retry_result = retry_runner.run("retry runtime", "runtime-retry-session");
  retry_runner.unregister_event_sink(retry_sink_id);
  SMOKE_CHECK(retry_result.text == "retried response");
  SMOKE_CHECK(retry_model->generate_calls() == 2);
  SMOKE_CHECK(retry_events.size() == 1);
  SMOKE_CHECK(retry_events.front().target == agent::ExecutionTarget::Model);
  SMOKE_CHECK(retry_events.front().payload.at("attempt").as_integer() == 1);
  SMOKE_CHECK(retry_events.front().payload.at("delayMs").as_integer() == 0);
  SMOKE_CHECK(retry_events.front().payload.at("iteration").as_integer() == 0);
  SMOKE_CHECK(retry_events.front().payload.at("error").as_string().find("transient model failure") != std::string::npos);

  agent::ExecutionPolicies timeout_policies;
  timeout_policies.timeout.timeout_ms[agent::ExecutionTarget::Model] = 1;
  auto slow_model = std::make_shared<SlowRuntimeModel>();
  agent::AgentRunner timeout_runner(agent::AgentRunnerConfig{
      .adapter = slow_model,
      .max_iterations = 1,
      .execution_policies = timeout_policies,
  });
  std::vector<agent::FrameworkEvent> timeout_events;
  const auto timeout_sink_id = timeout_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category.find("run.") == 0) {
      timeout_events.push_back(event);
    }
  });
  try {
    (void)timeout_runner.run("timeout runtime", "runtime-timeout-session");
    SMOKE_CHECK(false);
  } catch (const agent::TimeoutError& error) {
    SMOKE_CHECK(error.target() == "model");
    SMOKE_CHECK(error.timeout_ms() == 1);
  }
  timeout_runner.unregister_event_sink(timeout_sink_id);
  SMOKE_CHECK(slow_model->generate_calls() == 1);
  SMOKE_CHECK(timeout_events.size() == 2);
  SMOKE_CHECK(timeout_events.back().category == "run.failed");
  SMOKE_CHECK(timeout_events.back().payload.at("error").as_string().find("timed out") != std::string::npos);

  auto cancelling_model = std::make_shared<CancellingToolModel>();
  auto cancelling_tool = agent::define_tool(agent::ToolDefinition{
      .name = "runtime.cancel",
      .description = "Cancel the active runtime token.",
      .risk_level = agent::ToolRiskLevel::Low,
      .execute = [](const agent::Value&, agent::ToolExecutionContext& context) -> agent::ToolInvokeResult {
        SMOKE_CHECK(context.cancellation != nullptr);
        context.cancellation->cancel("tool requested cancellation");
        return agent::Value::object({{"cancelled", true}});
      },
  });
  agent::AgentRunner cancelling_runner(agent::AgentRunnerConfig{
      .adapter = cancelling_model,
      .tools = {cancelling_tool},
      .max_iterations = 3,
  });
  agent::CancellationToken runtime_cancellation;
  std::vector<agent::FrameworkEvent> cancellation_events;
  const auto cancellation_sink_id = cancelling_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
    if (event.category.find("run.") == 0 || event.category == "tool.failed") {
      cancellation_events.push_back(event);
    }
  });
  try {
    (void)cancelling_runner.run("cancel runtime", "runtime-cancel-session",
                                {}, {}, {}, {}, agent::Value::object({}), std::nullopt, {}, &runtime_cancellation);
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("tool requested cancellation") != std::string::npos);
  }
  cancelling_runner.unregister_event_sink(cancellation_sink_id);
  SMOKE_CHECK(runtime_cancellation.cancelled());
  SMOKE_CHECK(cancelling_model->generate_calls() == 1);
  SMOKE_CHECK(std::find_if(cancellation_events.begin(), cancellation_events.end(),
                      [](const agent::FrameworkEvent& event) { return event.category == "run.failed"; })
         != cancellation_events.end());
  SMOKE_CHECK(std::find_if(cancellation_events.begin(), cancellation_events.end(),
                      [](const agent::FrameworkEvent& event) { return event.category == "tool.failed"; })
         == cancellation_events.end());

  auto cancellation_aware_model = std::make_shared<CancellationAwareModel>();
  agent::AgentRunner cancellation_aware_runner(agent::AgentRunnerConfig{
      .adapter = cancellation_aware_model,
      .max_iterations = 1,
  });
  agent::CancellationToken model_cancellation;
  auto cancellation_aware_result = cancellation_aware_runner.run(
      "model cancellation propagation",
      "model-cancellation-session",
      {},
      {},
      {},
      {},
      agent::Value::object({}),
      std::nullopt,
      {},
      &model_cancellation);
  SMOKE_CHECK(cancellation_aware_result.text == "model cancellation propagated");
  SMOKE_CHECK(cancellation_aware_model->saw_cancellation());

  agent::ChatProviderRegistry chat_registry;
  chat_registry.register_provider("echo", [](const agent::Value& config) {
    SMOKE_CHECK(config.at("ok").as_bool());
    return std::make_shared<agent::EchoChatModelAdapter>();
  });
  SMOKE_CHECK(chat_registry.has("echo"));
  auto registry_model = chat_registry.create("echo", agent::Value::object({{"ok", true}}));
  auto registry_response = registry_model->generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "registry text")},
  });
  SMOKE_CHECK(registry_response.text == "registry text");
  try {
    (void)chat_registry.create("missing");
    SMOKE_CHECK(false);
  } catch (const agent::ConfigurationError& error) {
    SMOKE_CHECK(std::string(error.what()).find("Unknown chat provider") != std::string::npos);
  }

  auto llama_binding = std::make_shared<agent::LlamaCppNativeBinding>();
  agent::LlamaCppNativeRuntimeConfig seen_llama_config;
  agent::LlamaCppNativeChatRequest seen_llama_request;
  llama_binding->generate_chat =
      [&](const agent::LlamaCppNativeRuntimeConfig& config,
          const agent::LlamaCppNativeChatRequest& request,
          agent::LlamaCppNativeBinding::ChatDeltaHandler on_delta) {
        seen_llama_config = config;
        seen_llama_request = request;
        if (on_delta) {
          on_delta(agent::LlamaCppNativeChatDelta{.type = "text", .delta = "<think>pla"});
          on_delta(agent::LlamaCppNativeChatDelta{.type = "text", .delta = "n</think>fi"});
          on_delta(agent::LlamaCppNativeChatDelta{.type = "text", .delta = "nal"});
        }
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "<think>plan</think>final",
            .finish_reason = "stop",
        };
      };
  agent::LlamaCppNativeChatModelAdapterConfig llama_chat_config;
  llama_chat_config.model_path = "/models/qwen.gguf";
  llama_chat_config.library_dir = "/llama/build/bin";
  llama_chat_config.top_k = 12;
  llama_chat_config.top_p = 0.8;
  llama_chat_config.min_p = 0.05;
  llama_chat_config.repeat_penalty = 1.1;
  llama_chat_config.seed = 7;
  llama_chat_config.reasoning = agent::ReasoningSettings{.enabled = true};
  llama_chat_config.binding = llama_binding;
  agent::LlamaCppNativeChatModelAdapter llama_chat_model(llama_chat_config);
  auto llama_response = llama_chat_model.generate(agent::GenerateParams{
      .messages = {
          agent::create_message(agent::MessageRole::System, "be brief"),
          agent::create_message(agent::MessageRole::User, "hello"),
      },
      .settings = agent::ModelSettings{
          .model = "custom-name",
          .temperature = 0,
          .max_output_tokens = 8,
      },
  });
  SMOKE_CHECK(llama_response.provider == "llamacpp-native");
  SMOKE_CHECK(llama_response.text == "final");
  SMOKE_CHECK(llama_response.reasoning && llama_response.reasoning->text == "plan");
  SMOKE_CHECK(seen_llama_config.model_path == "/models/qwen.gguf");
  SMOKE_CHECK(seen_llama_config.library_dir == "/llama/build/bin");
  SMOKE_CHECK(seen_llama_request.model == "custom-name");
  SMOKE_CHECK(seen_llama_request.messages.size() == 2);
  SMOKE_CHECK(seen_llama_request.messages.front().role == "system");
  SMOKE_CHECK(seen_llama_request.messages.back().content == "hello");
  SMOKE_CHECK(seen_llama_request.temperature && *seen_llama_request.temperature == 0);
  SMOKE_CHECK(seen_llama_request.max_output_tokens && *seen_llama_request.max_output_tokens == 8);
  SMOKE_CHECK(seen_llama_request.top_k && *seen_llama_request.top_k == 12);
  SMOKE_CHECK(seen_llama_request.top_p && *seen_llama_request.top_p == 0.8);
  SMOKE_CHECK(seen_llama_request.min_p && *seen_llama_request.min_p == 0.05);
  SMOKE_CHECK(seen_llama_request.repeat_penalty && *seen_llama_request.repeat_penalty == 1.1);
  SMOKE_CHECK(seen_llama_request.seed && *seen_llama_request.seed == 7);
  auto llama_stream_events = llama_chat_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "stream")},
  });
  SMOKE_CHECK(llama_stream_events.size() == 6);
  SMOKE_CHECK(llama_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(llama_stream_events[1].delta == "pla");
  SMOKE_CHECK(llama_stream_events[2].reasoning == "plan");
  SMOKE_CHECK(llama_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(llama_stream_events[3].text == "fi");
  SMOKE_CHECK(llama_stream_events[4].text == "final");
  SMOKE_CHECK(llama_stream_events[5].response.text == "final");
  SMOKE_CHECK(llama_stream_events[5].response.reasoning &&
         llama_stream_events[5].response.reasoning->text == "plan");
  agent::LlamaCppNativeChatModelAdapterConfig strict_llama_config = llama_chat_config;
  strict_llama_config.tool_mode = "error";
  agent::LlamaCppNativeChatModelAdapter strict_llama_chat_model(strict_llama_config);
  try {
    (void)strict_llama_chat_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "hello")},
        .tools = {agent::ChatToolDescriptor{.name = "lookup"}},
    });
    SMOKE_CHECK(false);
  } catch (const agent::AdapterError& error) {
    SMOKE_CHECK(std::string(error.what()).find("does not support tool calling") != std::string::npos);
  }
  try {
    (void)llama_chat_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(
            agent::MessageRole::User,
            std::vector<agent::MessageContentPart>{agent::image_part(agent::MediaSource{
                .kind = agent::MediaSourceKind::Inline,
                .data = "",
                .mime_type = "image/png",
            })})},
    });
    SMOKE_CHECK(false);
  } catch (const agent::AdapterError& error) {
    const std::string llama_image_error = error.what();
    SMOKE_CHECK(llama_image_error.find("requires mmproj_path") != std::string::npos);
  }

  auto llama_multimodal_binding = std::make_shared<agent::LlamaCppNativeBinding>();
  agent::LlamaCppNativeChatRequest seen_llama_multimodal_request;
  llama_multimodal_binding->generate_chat =
      [&](const agent::LlamaCppNativeRuntimeConfig&,
          const agent::LlamaCppNativeChatRequest& request,
          agent::LlamaCppNativeBinding::ChatDeltaHandler) {
        seen_llama_multimodal_request = request;
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "saw image",
            .finish_reason = "stop",
        };
      };
  agent::LlamaCppNativeChatModelAdapterConfig llama_multimodal_config;
  llama_multimodal_config.model_path = "/models/vision.gguf";
  llama_multimodal_config.mmproj_path = "/models/mmproj.gguf";
  llama_multimodal_config.binding = llama_multimodal_binding;
  agent::LlamaCppNativeChatModelAdapter llama_multimodal_model(llama_multimodal_config);
  SMOKE_CHECK(llama_multimodal_model.supports("input.image"));
  auto llama_multimodal_response = llama_multimodal_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(
          agent::MessageRole::User,
          std::vector<agent::MessageContentPart>{
              agent::text_part("describe "),
              agent::image_part(agent::MediaSource{
                  .kind = agent::MediaSourceKind::Inline,
                  .data = "aGVsbG8=",
                  .mime_type = "image/png",
                  .filename = "tiny.png",
              }),
          })},
  });
  SMOKE_CHECK(llama_multimodal_response.text == "saw image");
  SMOKE_CHECK(seen_llama_multimodal_request.media.size() == 1);
  SMOKE_CHECK(!seen_llama_multimodal_request.media.front().bytes.empty());
  SMOKE_CHECK(seen_llama_multimodal_request.media.front().mime_type == "image/png");
  SMOKE_CHECK(seen_llama_multimodal_request.messages.front().content.find("<__media__>") != std::string::npos);

  auto llama_cancel_binding = std::make_shared<agent::LlamaCppNativeBinding>();
  std::string seen_llama_cancel_request_id;
  std::string cancelled_llama_request_id;
  agent::CancellationToken llama_model_cancellation;
  llama_cancel_binding->generate_chat =
      [&](const agent::LlamaCppNativeRuntimeConfig&,
          const agent::LlamaCppNativeChatRequest& request,
          agent::LlamaCppNativeBinding::ChatDeltaHandler) {
        seen_llama_cancel_request_id = request.request_id;
        llama_model_cancellation.cancel("llama abort");
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "cancelled",
            .finish_reason = "stop",
        };
      };
  llama_cancel_binding->cancel = [&](const std::string& request_id) {
    cancelled_llama_request_id = request_id;
  };
  agent::LlamaCppNativeChatModelAdapterConfig llama_cancel_config;
  llama_cancel_config.model_path = "/models/qwen.gguf";
  llama_cancel_config.binding = llama_cancel_binding;
  agent::LlamaCppNativeChatModelAdapter llama_cancel_model(llama_cancel_config);
  try {
    (void)llama_cancel_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "cancel")},
        .cancellation = &llama_model_cancellation,
    });
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("llama abort") != std::string::npos);
  }
  SMOKE_CHECK(!seen_llama_cancel_request_id.empty());
  SMOKE_CHECK(cancelled_llama_request_id == seen_llama_cancel_request_id);

  auto llama_tool_binding = std::make_shared<agent::LlamaCppNativeBinding>();
  agent::LlamaCppNativeRuntimeConfig seen_llama_tool_config;
  agent::LlamaCppNativeChatRequest seen_llama_tool_request;
  llama_tool_binding->generate_chat =
      [&](const agent::LlamaCppNativeRuntimeConfig& config,
          const agent::LlamaCppNativeChatRequest& request,
          agent::LlamaCppNativeBinding::ChatDeltaHandler) {
        seen_llama_tool_config = config;
        seen_llama_tool_request = request;
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "{\"type\":\"tool_call\",\"name\":\"lookup\",\"arguments\":{\"query\":\"repo\"}}",
            .finish_reason = "stop",
        };
      };
  agent::JsonSchema llama_lookup_schema;
  llama_lookup_schema.type = agent::JsonSchemaType::Object;
  llama_lookup_schema.properties["query"].type = agent::JsonSchemaType::String;
  llama_lookup_schema.required = {"query"};
  llama_lookup_schema.additional_properties = false;
  agent::ChatToolDescriptor llama_lookup_tool{
      .name = "lookup",
      .description = "Lookup docs",
      .input_schema = llama_lookup_schema,
  };
  agent::LlamaCppNativeChatModelAdapterConfig llama_tool_config;
  llama_tool_config.model_path = "/models/qwen.gguf";
  llama_tool_config.session_id = "main-session";
  llama_tool_config.lora_adapters = {agent::LlamaCppNativeLoraAdapter{.path = "/loras/domain.gguf", .scale = 0.5}};
  llama_tool_config.binding = llama_tool_binding;
  agent::LlamaCppNativeChatModelAdapter llama_tool_model(llama_tool_config);
  auto llama_assistant_history = agent::create_message(agent::MessageRole::Assistant, "");
  llama_assistant_history.tool_calls = {agent::ToolCall{
      .id = "lookup_1",
      .name = "lookup",
      .arguments = agent::Value::object({{"query", "old"}}),
  }};
  auto llama_tool_response = llama_tool_model.generate(agent::GenerateParams{
      .messages = {
          agent::create_message(agent::MessageRole::User, "lookup repo"),
          llama_assistant_history,
          agent::create_tool_result_message("lookup_1", "lookup", "old result"),
      },
      .tools = {llama_lookup_tool},
  });
  SMOKE_CHECK(llama_tool_response.text.empty());
  SMOKE_CHECK(llama_tool_response.finish_reason == "tool_calls");
  SMOKE_CHECK(llama_tool_response.tool_calls.size() == 1);
  SMOKE_CHECK(llama_tool_response.tool_calls.front().id == "lookup_1");
  SMOKE_CHECK(llama_tool_response.tool_calls.front().name == "lookup");
  SMOKE_CHECK(llama_tool_response.tool_calls.front().arguments.at("query").as_string() == "repo");
  SMOKE_CHECK(seen_llama_tool_config.lora_adapters.size() == 1);
  SMOKE_CHECK(seen_llama_tool_config.lora_adapters.front().path == "/loras/domain.gguf");
  SMOKE_CHECK(seen_llama_tool_request.session_id == "main-session");
  SMOKE_CHECK(seen_llama_tool_request.tool_mode == "auto");
  SMOKE_CHECK(seen_llama_tool_request.tools.size() == 1);
  SMOKE_CHECK(seen_llama_tool_request.grammar.find("tool_call") != std::string::npos);
  SMOKE_CHECK(seen_llama_tool_request.messages.front().content.find("Tool call envelope") != std::string::npos);
  SMOKE_CHECK(seen_llama_tool_request.messages[2].content.find("Assistant tool calls") != std::string::npos);
  SMOKE_CHECK(seen_llama_tool_request.messages[3].content.find("Tool result from lookup") != std::string::npos);

  llama_tool_binding->generate_chat =
      [](const agent::LlamaCppNativeRuntimeConfig&,
         const agent::LlamaCppNativeChatRequest& request,
         agent::LlamaCppNativeBinding::ChatDeltaHandler on_delta) {
        if (on_delta) {
          on_delta(agent::LlamaCppNativeChatDelta{.type = "text", .delta = "{\"type\":\"tool_call\","});
          on_delta(agent::LlamaCppNativeChatDelta{.type = "text", .delta = "\"name\":\"lookup\",\"arguments\":{\"query\":\"stream\"}}"});
        }
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "{\"type\":\"tool_call\",\"name\":\"lookup\",\"arguments\":{\"query\":\"stream\"}}",
            .finish_reason = "stop",
        };
      };
  auto llama_tool_stream = llama_tool_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "lookup stream")},
      .tools = {llama_lookup_tool},
  });
  bool saw_llama_tool_text_delta = false;
  for (const auto& event : llama_tool_stream) {
    saw_llama_tool_text_delta = saw_llama_tool_text_delta ||
                                event.type == agent::ModelStreamEventType::TextDelta;
  }
  SMOKE_CHECK(!saw_llama_tool_text_delta);
  SMOKE_CHECK(llama_tool_stream.back().response.tool_calls.size() == 1);
  SMOKE_CHECK(llama_tool_stream.back().response.tool_calls.front().arguments.at("query").as_string() == "stream");

  agent::JsonSchema answer_schema;
  answer_schema.type = agent::JsonSchemaType::Object;
  answer_schema.properties["answer"].type = agent::JsonSchemaType::String;
  answer_schema.required = {"answer"};
  answer_schema.additional_properties = false;
  agent::LlamaCppNativeChatRequest seen_llama_structured_request;
  llama_tool_binding->generate_chat =
      [&](const agent::LlamaCppNativeRuntimeConfig&,
          const agent::LlamaCppNativeChatRequest& request,
          agent::LlamaCppNativeBinding::ChatDeltaHandler) {
        seen_llama_structured_request = request;
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "{\"answer\":\"ok\"}",
            .finish_reason = "stop",
        };
      };
  auto llama_structured_response = llama_tool_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "answer")},
      .settings = agent::ModelSettings{
          .extra = agent::Value::object({{"outputSchema", agent::json_schema_to_value(answer_schema)}}),
      },
  });
  SMOKE_CHECK(llama_structured_response.text == "{\"answer\":\"ok\"}");
  SMOKE_CHECK(seen_llama_structured_request.output_schema);
  SMOKE_CHECK(seen_llama_structured_request.grammar.find("root ::= space object-1 space") != std::string::npos);

  llama_tool_binding->generate_chat =
      [](const agent::LlamaCppNativeRuntimeConfig&,
         const agent::LlamaCppNativeChatRequest& request,
         agent::LlamaCppNativeBinding::ChatDeltaHandler) {
        return agent::LlamaCppNativeChatResult{
            .id = request.request_id,
            .model = request.model,
            .text = "{\"wrong\":\"no\"}",
            .finish_reason = "stop",
        };
      };
  bool llama_structured_invalid_failed = false;
  try {
    (void)llama_tool_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "answer")},
        .settings = agent::ModelSettings{
            .extra = agent::Value::object({{"outputSchema", agent::json_schema_to_value(answer_schema)}}),
        },
    });
  } catch (const agent::SchemaValidationError&) {
    llama_structured_invalid_failed = true;
  }
  SMOKE_CHECK(llama_structured_invalid_failed);

  int configured_llama_embedding_calls = 0;
  llama_binding->embed_texts =
      [&](const agent::LlamaCppNativeRuntimeConfig& config,
          const agent::LlamaCppNativeEmbeddingRequest& request) {
        ++configured_llama_embedding_calls;
        SMOKE_CHECK(config.model_path == "/models/config-embed.gguf");
        SMOKE_CHECK(request.dimensions && *request.dimensions == 3);
        std::vector<agent::EmbeddingVector> embeddings;
        for (std::size_t index = 0; index < request.texts.size(); ++index) {
          embeddings.push_back({1.0, 0.0, 0.0});
        }
        return agent::LlamaCppNativeEmbeddingResult{
            .model = request.model,
            .embeddings = std::move(embeddings),
            .dimensions = 3,
        };
      };
  auto configured_llama_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", agent::Value::object({
                      {"provider", "llamacpp-native"},
                      {"modelPath", "/models/config-chat.gguf"},
                      {"libraryDir", "/llama/config/bin"},
                      {"topK", 9},
                      {"sessionId", "config-llama-session"},
                      {"grammarRoot", "root"},
                      {"loraAdapters", agent::Value::array({
                          agent::Value::object({{"path", "/loras/config-domain.gguf"}, {"scale", 0.75}}),
                      })},
                      {"reasoning", agent::Value::object({{"enabled", true}})},
                  })},
                  {"knowledge", agent::Value::object({
                      {"base", agent::Value::object({
                          {"id", "llama-config-docs"},
                          {"embedder", agent::Value::object({
                              {"provider", "llamacpp-native"},
                              {"modelPath", "/models/config-embed.gguf"},
                              {"dimensions", 3},
                              {"pooling", "mean"},
                          })},
                          {"sources", agent::Value::array({
                              agent::Value::object({
                                  {"type", "text"},
                                  {"uri", "memory://llama-config-doc"},
                                  {"title", "llama config"},
                                  {"text", "llama native config embedding source"},
                              }),
                          })},
                      })},
                  })},
              })},
          })},
      }),
      "main",
      {},
      {},
      {},
      {},
      {},
      {},
      agent::NativeLlamaCppAdapters{.binding = llama_binding});
  SMOKE_CHECK(configured_llama_embedding_calls == 1);
  SMOKE_CHECK(configured_llama_app.knowledge && configured_llama_app.knowledge->base);
  auto configured_llama_hits = configured_llama_app.knowledge->base->search(
      "llama native config embedding", agent::KnowledgeSearchOptions{.top_k = 1, .min_score = 0.0});
  SMOKE_CHECK(configured_llama_embedding_calls == 2);
  SMOKE_CHECK(!configured_llama_hits.empty());
  auto configured_llama_run = configured_llama_app.runner->run("configured llama", "llama-config-session");
  SMOKE_CHECK(configured_llama_run.text == "final");
  SMOKE_CHECK(seen_llama_config.lora_adapters.size() == 1);
  SMOKE_CHECK(seen_llama_config.lora_adapters.front().path == "/loras/config-domain.gguf");
  SMOKE_CHECK(seen_llama_config.lora_adapters.front().scale == 0.75);
  SMOKE_CHECK(seen_llama_request.session_id == "config-llama-session");
  SMOKE_CHECK(seen_llama_request.grammar_root == "root");

  auto llama_structured_capability_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", agent::Value::object({
                      {"provider", "ollama"},
                      {"model", "primary"},
                      {"requireCapabilities", agent::Value::array({"output.structuredContent"})},
                      {"fallbacks", agent::Value::array({
                          agent::Value::object({
                              {"provider", "llamacpp-native"},
                              {"model", "llama-structured"},
                              {"modelPath", "/models/structured.gguf"},
                              {"reasoning", agent::Value::object({{"enabled", true}})},
                          }),
                      })},
                  })},
              })},
          })},
      }),
      "main",
      {},
      {},
      {},
      {},
      {},
      {},
      agent::NativeLlamaCppAdapters{.binding = llama_binding});
  auto llama_structured_capability_run =
      llama_structured_capability_app.runner->run("structured fallback", "llama-capability-session");
  SMOKE_CHECK(llama_structured_capability_run.text == "final");
  SMOKE_CHECK(seen_llama_config.model_path == "/models/structured.gguf");
  SMOKE_CHECK(seen_llama_request.model == "llama-structured");

  agent::EmbeddingProviderRegistry embedding_registry;
  embedding_registry.register_provider("hash", [](const agent::Value& config) {
    return std::make_shared<agent::HashEmbeddingAdapter>(
        static_cast<int>(config.at("dimensions").as_integer(16)));
  });
  auto text_embedder = embedding_registry.create("hash", agent::Value::object({{"dimensions", 16}}));
  auto text_space = text_embedder->space();
  SMOKE_CHECK(text_space.id == "hash-shared-v1");
  SMOKE_CHECK(text_space.modalities.front() == "text");
  auto small_text_vector = text_embedder->embed_one("native registry", agent::Value::object({{"dimensions", 8}}));
  SMOKE_CHECK(small_text_vector.size() == 8);
  agent::LlamaCppNativeEmbeddingRequest seen_llama_embedding_request;
  llama_binding->embed_texts =
      [&](const agent::LlamaCppNativeRuntimeConfig& config,
          const agent::LlamaCppNativeEmbeddingRequest& request) {
        SMOKE_CHECK(config.model_path == "/models/embed.gguf");
        seen_llama_embedding_request = request;
        return agent::LlamaCppNativeEmbeddingResult{
            .model = request.model,
            .embeddings = {{0, 1, 2}, {3, 4, 5}},
            .dimensions = 3,
        };
      };
  agent::LlamaCppNativeEmbeddingAdapterConfig llama_embedding_config;
  llama_embedding_config.model_path = "/models/embed.gguf";
  llama_embedding_config.dimensions = 3;
  llama_embedding_config.pooling = "cls";
  llama_embedding_config.normalize = false;
  llama_embedding_config.binding = llama_binding;
  agent::LlamaCppNativeTextEmbeddingAdapter llama_embedder(llama_embedding_config);
  auto llama_vectors = llama_embedder.embed({"left", "right"},
                                            agent::Value::object({{"model", "embed-override"},
                                                                 {"pooling", "last"},
                                                                 {"normalize", true}}));
  SMOKE_CHECK(llama_vectors.size() == 2);
  SMOKE_CHECK(llama_vectors.back().back() == 5);
  SMOKE_CHECK(seen_llama_embedding_request.model == "embed-override");
  SMOKE_CHECK(seen_llama_embedding_request.pooling == "last");
  SMOKE_CHECK(seen_llama_embedding_request.normalize);
  SMOKE_CHECK(seen_llama_embedding_request.dimensions && *seen_llama_embedding_request.dimensions == 3);
  llama_binding->embed_texts =
      [](const agent::LlamaCppNativeRuntimeConfig&,
         const agent::LlamaCppNativeEmbeddingRequest& request) {
        return agent::LlamaCppNativeEmbeddingResult{
            .model = request.model,
            .embeddings = {{1, 2}},
            .dimensions = 2,
        };
      };
  try {
    (void)llama_embedder.embed({"bad"});
    SMOKE_CHECK(false);
  } catch (const agent::AdapterError& error) {
    SMOKE_CHECK(std::string(error.what()).find("dimensions mismatch") != std::string::npos);
  }
  std::string seen_llama_embedding_cancel_request_id;
  std::string cancelled_llama_embedding_request_id;
  agent::CancellationToken llama_embedding_cancellation;
  llama_binding->embed_texts =
      [&](const agent::LlamaCppNativeRuntimeConfig&,
          const agent::LlamaCppNativeEmbeddingRequest& request) {
        seen_llama_embedding_cancel_request_id = request.request_id;
        llama_embedding_cancellation.cancel("llama embedding abort");
        return agent::LlamaCppNativeEmbeddingResult{
            .model = request.model,
            .embeddings = {{1, 2, 3}},
            .dimensions = 3,
        };
      };
  llama_binding->cancel = [&](const std::string& request_id) {
    cancelled_llama_embedding_request_id = request_id;
  };
  try {
    (void)llama_embedder.embed({"cancel embedding"}, agent::Value::object({}), &llama_embedding_cancellation);
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("llama embedding abort") != std::string::npos);
  }
  SMOKE_CHECK(!seen_llama_embedding_cancel_request_id.empty());
  SMOKE_CHECK(cancelled_llama_embedding_request_id == seen_llama_embedding_cancel_request_id);

  agent::ImageEmbeddingProviderRegistry image_embedding_registry;
  image_embedding_registry.register_provider("hash-image", [](const agent::Value& config) {
    return std::make_shared<agent::HashImageEmbeddingAdapter>(
        static_cast<int>(config.at("dimensions").as_integer(16)));
  });
  auto image_embedder = image_embedding_registry.create("hash-image", agent::Value::object({{"dimensions", 16}}));
  auto image_space = image_embedder->space();
  SMOKE_CHECK(image_space.modalities.front() == "image");
  auto image_vector = image_embedder->embed_one(agent::ImageEmbeddingInput{
      .source = agent::MediaSource{
          .kind = agent::MediaSourceKind::Path,
          .path = "charts/revenue.png",
          .mime_type = "image/png",
      },
      .alt_text = "Revenue chart",
      .title = "Q4 Revenue",
      .text_hint = "growth trend",
  });
  SMOKE_CHECK(image_vector.size() == 16);
  double image_magnitude = 0;
  for (const auto value : image_vector) {
    image_magnitude += value * value;
  }
  SMOKE_CHECK(image_magnitude > 0.99 && image_magnitude < 1.01);

  agent::ChatProviderRegistry concurrent_chat_registry;
  agent::EmbeddingProviderRegistry concurrent_embedding_registry;
  agent::ImageEmbeddingProviderRegistry concurrent_image_embedding_registry;
  std::atomic<int> concurrent_model_registry_ready{0};
  std::atomic<bool> concurrent_model_registry_start{false};
  std::atomic<int> concurrent_chat_factory_calls{0};
  std::atomic<int> concurrent_embedding_factory_calls{0};
  std::atomic<int> concurrent_image_embedding_factory_calls{0};
  constexpr int concurrent_model_registry_thread_count = 8;
  std::vector<std::thread> concurrent_model_registry_workers;
  for (int index = 0; index < concurrent_model_registry_thread_count; ++index) {
    concurrent_model_registry_workers.emplace_back([&, index]() {
      concurrent_model_registry_ready.fetch_add(1);
      while (!concurrent_model_registry_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(index);
      const auto chat_provider = "chat.concurrent." + suffix;
      const auto embedding_provider = "embedding.concurrent." + suffix;
      const auto image_embedding_provider = "image-embedding.concurrent." + suffix;
      concurrent_chat_registry.register_provider(
          chat_provider,
          [&concurrent_chat_factory_calls](const agent::Value&) {
            concurrent_chat_factory_calls.fetch_add(1);
            return std::make_shared<agent::EchoChatModelAdapter>();
          });
      concurrent_embedding_registry.register_provider(
          embedding_provider,
          [&concurrent_embedding_factory_calls](const agent::Value& config) {
            concurrent_embedding_factory_calls.fetch_add(1);
            return std::make_shared<agent::HashEmbeddingAdapter>(
                static_cast<int>(config.at("dimensions").as_integer(8)));
          });
      concurrent_image_embedding_registry.register_provider(
          image_embedding_provider,
          [&concurrent_image_embedding_factory_calls](const agent::Value& config) {
            concurrent_image_embedding_factory_calls.fetch_add(1);
            return std::make_shared<agent::HashImageEmbeddingAdapter>(
                static_cast<int>(config.at("dimensions").as_integer(8)));
          });
      SMOKE_CHECK(concurrent_chat_registry.has(chat_provider));
      SMOKE_CHECK(concurrent_embedding_registry.has(embedding_provider));
      SMOKE_CHECK(concurrent_image_embedding_registry.has(image_embedding_provider));
      auto concurrent_chat_model = concurrent_chat_registry.create(chat_provider);
      auto concurrent_chat_response = concurrent_chat_model->generate(agent::GenerateParams{
          .messages = {agent::create_message(agent::MessageRole::User, "chat:" + suffix)},
      });
      SMOKE_CHECK(concurrent_chat_response.text == "chat:" + suffix);
      auto concurrent_text_embedder =
          concurrent_embedding_registry.create(embedding_provider,
                                               agent::Value::object({{"dimensions", 8}}));
      SMOKE_CHECK(concurrent_text_embedder->embed_one("text:" + suffix).size() == 8);
      auto concurrent_image_embedder =
          concurrent_image_embedding_registry.create(image_embedding_provider,
                                                     agent::Value::object({{"dimensions", 8}}));
      SMOKE_CHECK(concurrent_image_embedder->embed_one(agent::ImageEmbeddingInput{
                 .source = agent::MediaSource{
                     .kind = agent::MediaSourceKind::Path,
                     .path = "image-" + suffix + ".png",
                     .mime_type = "image/png",
                 },
             }).size() == 8);
    });
  }
  while (concurrent_model_registry_ready.load() < concurrent_model_registry_thread_count) {
    std::this_thread::yield();
  }
  concurrent_model_registry_start.store(true);
  for (auto& worker : concurrent_model_registry_workers) {
    worker.join();
  }
  SMOKE_CHECK(concurrent_chat_factory_calls.load() == concurrent_model_registry_thread_count);
  SMOKE_CHECK(concurrent_embedding_factory_calls.load() == concurrent_model_registry_thread_count);
  SMOKE_CHECK(concurrent_image_embedding_factory_calls.load() == concurrent_model_registry_thread_count);

  agent::ClipTextEmbeddingAdapter clip_text_embedder(
      [](const std::vector<std::string>& texts, const agent::EmbeddingSettings& settings) {
        SMOKE_CHECK((texts == std::vector<std::string>{"hello clip"}));
        SMOKE_CHECK(settings.model == "custom-clip");
        SMOKE_CHECK(settings.dimensions && *settings.dimensions == 3);
        SMOKE_CHECK(settings.space_id == "custom-clip-space");
        return std::vector<agent::EmbeddingVector>{{1, 2, 3}};
      });
  agent::ClipImageEmbeddingAdapter clip_image_embedder(
      [](const std::vector<agent::ImageEmbeddingInput>& images, const agent::EmbeddingSettings& settings) {
        SMOKE_CHECK(images.size() == 1);
        SMOKE_CHECK(images.front().source.kind == agent::MediaSourceKind::Inline);
        SMOKE_CHECK(images.front().source.data == "AA==");
        SMOKE_CHECK(settings.model == "Xenova/clip-vit-base-patch32");
        SMOKE_CHECK(settings.space_id == "clip-shared-v1");
        return std::vector<agent::EmbeddingVector>{{4, 5, 6}};
      });
  SMOKE_CHECK(clip_text_embedder.space().id == clip_image_embedder.space().id);
  SMOKE_CHECK(clip_text_embedder.embed_one("hello clip",
                                      agent::Value::object({{"model", "custom-clip"},
                                                           {"dimensions", 3},
                                                           {"spaceId", "custom-clip-space"}})) ==
         agent::EmbeddingVector({1, 2, 3}));
  SMOKE_CHECK(clip_image_embedder.embed_one(agent::ImageEmbeddingInput{
             .source = agent::MediaSource{
                 .kind = agent::MediaSourceKind::Inline,
                 .data = "AA==",
                 .mime_type = "image/png",
             },
         }) == agent::EmbeddingVector({4, 5, 6}));
  bool saw_missing_clip_batch = false;
  try {
    agent::ClipTextEmbeddingAdapter missing_clip_batch;
    (void)missing_clip_batch.embed_one("missing");
  } catch (const agent::ConfigurationError& error) {
    saw_missing_clip_batch = true;
    SMOKE_CHECK(std::string(error.what()).find("injected batch function") != std::string::npos);
  }
  SMOKE_CHECK(saw_missing_clip_batch);

  bool openai_embedding_called = false;
  agent::OpenAIEmbeddingAdapter openai_embedder(
      "text-embedding-3-small",
      [&](const agent::NativeProviderRequest& request) {
        openai_embedding_called = true;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.endpoint == "/embeddings");
        SMOKE_CHECK(request.base_url == "https://api.openai.com/v1");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer openai-embedding-key");
        SMOKE_CHECK(request.headers.at("openai-organization") == "org_123");
        SMOKE_CHECK(request.body.at("model").as_string() == "text-embedding-3-large");
        SMOKE_CHECK(request.body.at("input").as_array().size() == 2);
        SMOKE_CHECK(request.body.at("dimensions").as_integer() == 3);
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 1}, {"embedding", agent::Value::array({0.3, 0.4, 0.5})}}),
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.1, 0.2, 0.3})}}),
            })},
        });
      },
      "openai-embedding-key",
      "https://api.openai.com/v1/",
      "org_123",
      12,
      "openai-space");
  auto openai_vectors = openai_embedder.embed({"first", "second"},
      agent::Value::object({{"model", "text-embedding-3-large"}, {"dimensions", 3}}));
  SMOKE_CHECK(openai_embedding_called);
  SMOKE_CHECK(openai_vectors.size() == 2);
  SMOKE_CHECK(openai_vectors.front().front() == 0.1);

  bool openai_embedding_cancellation_called = false;
  agent::CancellationToken openai_embedding_cancellation;
  agent::OpenAIEmbeddingAdapter openai_cancellable_embedder(
      "text-embedding-3-small",
      [&](const agent::NativeProviderRequest& request) {
        openai_embedding_cancellation_called = true;
        SMOKE_CHECK(request.cancellation == &openai_embedding_cancellation);
        request.cancellation->cancel("embedding provider abort");
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.1, 0.2})}}),
            })},
        });
      },
      "openai-embedding-key");
  try {
    (void)openai_cancellable_embedder.embed({"cancel"}, agent::Value::object({}), &openai_embedding_cancellation);
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("embedding provider abort") != std::string::npos);
  }
  SMOKE_CHECK(openai_embedding_cancellation_called);

  bool memory_embedding_cancellation_called = false;
  agent::CancellationToken memory_embedding_cancellation;
  auto memory_cancellable_embedder = std::make_shared<agent::OpenAIEmbeddingAdapter>(
      "text-embedding-3-small",
      [&](const agent::NativeProviderRequest& request) {
        memory_embedding_cancellation_called = true;
        SMOKE_CHECK(request.cancellation == &memory_embedding_cancellation);
        request.cancellation->cancel("memory embedding abort");
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.1, 0.2})}}),
            })},
        });
      },
      "openai-embedding-key");
  agent::LongTermMemory memory_cancellable(memory_cancellable_embedder,
                                           std::make_shared<agent::InMemoryVectorStore>());
  try {
    (void)memory_cancellable.search("cancel memory", 1, 0.0, {}, &memory_embedding_cancellation);
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("memory embedding abort") != std::string::npos);
  }
  SMOKE_CHECK(memory_embedding_cancellation_called);

  bool qwen_embedding_called = false;
  agent::QwenEmbeddingAdapter qwen_embedder(
      "text-embedding-v4",
      [&](const agent::NativeProviderRequest& request) {
        qwen_embedding_called = true;
        SMOKE_CHECK(request.provider == "qwen");
        SMOKE_CHECK(request.endpoint == "/embeddings");
        SMOKE_CHECK(request.base_url == "https://dashscope.aliyuncs.com/compatible-mode/v1");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer qwen-embedding-key");
        SMOKE_CHECK(request.body.at("encoding_format").as_string() == "float");
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.6, 0.7})}}),
            })},
        });
      },
      "qwen-embedding-key");
