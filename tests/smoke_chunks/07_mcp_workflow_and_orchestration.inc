  auto qwen_vectors = qwen_embedder.embed({"qwen text"});
  SMOKE_CHECK(qwen_embedding_called);
  SMOKE_CHECK(qwen_vectors.size() == 1);
  SMOKE_CHECK(qwen_vectors.front().size() == 2);

  bool ollama_embedding_called = false;
  agent::OllamaEmbeddingAdapter ollama_embedder(
      "embeddinggemma",
      [&](const agent::NativeProviderRequest& request) {
        ollama_embedding_called = true;
        SMOKE_CHECK(request.provider == "ollama");
        SMOKE_CHECK(request.endpoint == "/embed");
        SMOKE_CHECK(request.base_url == "http://127.0.0.1:11434/api");
        SMOKE_CHECK(request.body.at("truncate").as_bool());
        SMOKE_CHECK(request.body.at("dimensions").as_integer() == 2);
        return agent::Value::object({
            {"embeddings", agent::Value::array({
                agent::Value::array({1.0, 0.0}),
                agent::Value::array({0.0, 1.0}),
            })},
        });
      },
      "http://127.0.0.1:11434/",
      2);
  auto ollama_vectors = ollama_embedder.embed({"left", "right"});
  SMOKE_CHECK(ollama_embedding_called);
  SMOKE_CHECK(ollama_vectors.size() == 2);
  SMOKE_CHECK(ollama_vectors.back().back() == 1.0);

  int gemini_embedding_calls = 0;
  agent::GeminiEmbeddingAdapter gemini_embedder(
      "gemini-embedding-001",
      [&](const agent::NativeProviderRequest& request) {
        ++gemini_embedding_calls;
        SMOKE_CHECK(request.provider == "gemini");
        SMOKE_CHECK(request.endpoint == "/models/gemini-embedding-001:embedContent");
        SMOKE_CHECK(request.base_url == "https://generativelanguage.googleapis.com/v1beta");
        SMOKE_CHECK(request.headers.at("x-goog-api-key") == "gemini-embedding-key");
        SMOKE_CHECK(request.body.at("model").as_string() == "models/gemini-embedding-001");
        SMOKE_CHECK(request.body.at("taskType").as_string() == "RETRIEVAL_QUERY");
        SMOKE_CHECK(request.body.at("outputDimensionality").as_integer() == 2);
        return agent::Value::object({
            {"embedding", agent::Value::object({
                {"values", gemini_embedding_calls == 1
                               ? agent::Value::array({0.8, 0.1})
                               : agent::Value::array({0.2, 0.9})},
            })},
        });
      },
      "gemini-embedding-key",
      "https://generativelanguage.googleapis.com/v1beta/",
      2,
      "RETRIEVAL_QUERY");
  auto gemini_vectors = gemini_embedder.embed({"alpha", "beta"});
  SMOKE_CHECK(gemini_embedding_calls == 2);
  SMOKE_CHECK(gemini_vectors.size() == 2);
  SMOKE_CHECK(gemini_vectors.front().front() == 0.8);

  bool provider_called = false;
  agent::OpenAICompatibleChatModelAdapter provider_model("openai", "gpt-test",
      [&](const agent::NativeProviderRequest& request) {
        provider_called = true;
        SMOKE_CHECK(request.endpoint == "/v1/chat/completions");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer provider-key");
        SMOKE_CHECK(request.body.at("model").as_string() == "gpt-test");
        SMOKE_CHECK(!request.body.at("messages").as_array().empty());
        return agent::Value::object({
            {"id", "resp_1"},
            {"choices", agent::Value::array({
                agent::Value::object({
                    {"message", agent::Value::object({{"content", "provider ok"}})},
                    {"finish_reason", "stop"},
                }),
            })},
            {"usage", agent::Value::object({{"prompt_tokens", 3}, {"completion_tokens", 2}})},
        });
      },
      "/v1/chat/completions",
      "https://api.openai.test",
      "provider-key");
  auto provider_response = provider_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
  });
  SMOKE_CHECK(provider_called);
  SMOKE_CHECK(provider_response.text == "provider ok");

  bool missing_openai_key_called = false;
  agent::OpenAICompatibleChatModelAdapter missing_openai_key_model("openai", "gpt-missing-key",
      [&](const agent::NativeProviderRequest&) {
        missing_openai_key_called = true;
        return agent::Value::object({});
      });
  bool missing_openai_key_failed = false;
  try {
    (void)missing_openai_key_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
    });
  } catch (const agent::ConfigurationError& error) {
    missing_openai_key_failed =
        std::string(error.what()).find("OpenAI adapter requires apiKey or OPENAI_API_KEY.") != std::string::npos;
  }
  SMOKE_CHECK(missing_openai_key_failed);
  SMOKE_CHECK(!missing_openai_key_called);

  bool provider_cancellation_called = false;
  agent::CancellationToken provider_cancellation;
  agent::OpenAICompatibleChatModelAdapter provider_cancellation_model("openai", "gpt-cancel",
      [&](const agent::NativeProviderRequest& request) {
        provider_cancellation_called = true;
        SMOKE_CHECK(request.cancellation == &provider_cancellation);
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer provider-cancel-key");
        request.cancellation->cancel("provider abort");
        return agent::Value::object({
            {"id", "resp_cancel"},
            {"choices", agent::Value::array({
                agent::Value::object({
                    {"message", agent::Value::object({{"content", "late"}})},
                    {"finish_reason", "stop"},
                }),
            })},
        });
      },
      "/v1/chat/completions",
      "https://api.openai.test",
      "provider-cancel-key");
  try {
    (void)provider_cancellation_model.generate(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "cancel provider")},
        .cancellation = &provider_cancellation,
    });
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("provider abort") != std::string::npos);
  }
  SMOKE_CHECK(provider_cancellation_called);

  bool provider_argument_normalization_called = false;
  agent::OpenAICompatibleChatModelAdapter provider_argument_normalization_model("openai", "gpt-args",
      [&](const agent::NativeProviderRequest& request) {
        provider_argument_normalization_called = true;
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer provider-args-key");
        return agent::Value::object({
            {"id", "resp_args"},
            {"choices", agent::Value::array({
                agent::Value::object({
                    {"message", agent::Value::object({
                        {"content", ""},
                        {"tool_calls", agent::Value::array({
                            agent::Value::object({
                                {"id", "call_scalar"},
                                {"function", agent::Value::object({
                                    {"name", "scalar"},
                                    {"arguments", "42"},
                                })},
                            }),
                            agent::Value::object({
                                {"id", "call_array"},
                                {"function", agent::Value::object({
                                    {"name", "array"},
                                    {"arguments", "[\"x\"]"},
                                })},
                            }),
                            agent::Value::object({
                                {"id", "call_raw"},
                                {"function", agent::Value::object({
                                    {"name", "raw"},
                                    {"arguments", "not-json"},
                                })},
                            }),
                        })},
                    })},
                    {"finish_reason", "tool_calls"},
                }),
            })},
        });
      },
      "/v1/chat/completions",
      "https://api.openai.test",
      "provider-args-key");
  auto provider_argument_normalization = provider_argument_normalization_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "args")},
  });
  SMOKE_CHECK(provider_argument_normalization_called);
  SMOKE_CHECK(provider_argument_normalization.tool_calls.size() == 3);
  SMOKE_CHECK(provider_argument_normalization.tool_calls[0].arguments.is_object());
  SMOKE_CHECK(provider_argument_normalization.tool_calls[0].arguments.at("value").as_integer() == 42);
  SMOKE_CHECK(provider_argument_normalization.tool_calls[1].arguments.is_object());
  SMOKE_CHECK(provider_argument_normalization.tool_calls[1].arguments.at("value").as_array().front().as_string() == "x");
  SMOKE_CHECK(provider_argument_normalization.tool_calls[2].arguments.is_object());
  SMOKE_CHECK(provider_argument_normalization.tool_calls[2].arguments.at("raw").as_string() == "not-json");

  bool provider_stream_called = false;
  agent::OpenAICompatibleChatModelAdapter provider_stream_model(
      "openai",
      "gpt-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "/v1/chat/completions",
      "https://api.openai.test",
      "stream-key",
      [&](const agent::NativeProviderRequest& request) {
        provider_stream_called = true;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.endpoint == "/v1/chat/completions");
        SMOKE_CHECK(request.base_url == "https://api.openai.test");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer stream-key");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("model").as_string() == "gpt-stream");
        return std::vector<std::string>{
            "data: {\"id\":\"chatcmpl_stream\",\"model\":\"gpt-stream\",\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n",
            "data: {\"id\":\"chatcmpl_stream\",\"model\":\"gpt-stream\",\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n",
            "data: {\"id\":\"chatcmpl_stream\",\"model\":\"gpt-stream\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"q\\\"\"}}]}}]}\n\n",
            "data: {\"id\":\"chatcmpl_stream\",\"model\":\"gpt-stream\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\":1}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n",
            "data: [DONE]\n\n",
        };
      });
  auto provider_stream_events = provider_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
  });
  SMOKE_CHECK(provider_stream_called);
  SMOKE_CHECK(provider_stream_events.size() == 6);
  SMOKE_CHECK(provider_stream_events[0].type == agent::ModelStreamEventType::ResponseStart);
  SMOKE_CHECK(provider_stream_events[1].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(provider_stream_events[1].text == "hel");
  SMOKE_CHECK(provider_stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(provider_stream_events[2].text == "hello");
  SMOKE_CHECK(provider_stream_events[3].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(provider_stream_events[3].tool_call_id == "call_1");
  SMOKE_CHECK(provider_stream_events[3].tool_call_name == "lookup");
  SMOKE_CHECK(provider_stream_events[3].tool_call_args_delta == "{\"q\"");
  SMOKE_CHECK(provider_stream_events[3].tool_call_args_accumulated == "{\"q\"");
  SMOKE_CHECK(provider_stream_events[4].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(provider_stream_events[4].tool_call_id == "call_1");
  SMOKE_CHECK(provider_stream_events[4].tool_call_args_delta == ":1}");
  SMOKE_CHECK(provider_stream_events[4].tool_call_args_accumulated == "{\"q\":1}");
  SMOKE_CHECK(provider_stream_events[5].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(provider_stream_events[5].response.text == "hello");
  SMOKE_CHECK(provider_stream_events[5].response.tool_calls.size() == 1);
  SMOKE_CHECK(provider_stream_events[5].response.tool_calls.front().name == "lookup");
  SMOKE_CHECK(provider_stream_events[5].response.tool_calls.front().arguments.at("q").as_integer() == 1);
  SMOKE_CHECK(provider_stream_events[5].response.finish_reason == "tool_calls");

  bool provider_stream_cancellation_called = false;
  agent::CancellationToken provider_stream_cancellation;
  agent::OpenAICompatibleChatModelAdapter provider_stream_cancellation_model(
      "openai",
      "gpt-stream-cancel",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "/v1/chat/completions",
      "https://api.openai.test",
      "stream-cancel-key",
      [&](const agent::NativeProviderRequest& request) {
        provider_stream_cancellation_called = true;
        SMOKE_CHECK(request.cancellation == &provider_stream_cancellation);
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer stream-cancel-key");
        request.cancellation->cancel("provider stream abort");
        return std::vector<std::string>{
            "data: {\"choices\":[{\"delta\":{\"content\":\"late\"}}]}\n\n",
        };
      });
  try {
    (void)provider_stream_cancellation_model.stream(agent::GenerateParams{
        .messages = {agent::create_message(agent::MessageRole::User, "stream cancel")},
        .cancellation = &provider_stream_cancellation,
    });
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("provider stream abort") != std::string::npos);
  }
  SMOKE_CHECK(provider_stream_cancellation_called);

  agent::JsonSchema lookup_schema;
  lookup_schema.type = agent::JsonSchemaType::Object;
  lookup_schema.required = {"query"};
  lookup_schema.properties["query"].type = agent::JsonSchemaType::String;
  agent::ChatToolDescriptor lookup_tool{
      .name = "lookup",
      .description = "Lookup a value.",
      .input_schema = lookup_schema,
  };

  bool provider_responses_called = false;
  agent::OpenAICompatibleChatModelAdapter provider_responses_model(
      "openai",
      "gpt-responses",
      [&](const agent::NativeProviderRequest& request) {
        provider_responses_called = true;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.endpoint == "/v1/responses");
        SMOKE_CHECK(request.base_url == "https://api.openai.responses");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer responses-key");
        SMOKE_CHECK(request.body.at("model").as_string() == "gpt-responses");
        SMOKE_CHECK(request.body.at("instructions").as_string() == "responses system");
        SMOKE_CHECK(request.body.at("reasoning").at("effort").as_string() == "high");
        SMOKE_CHECK(request.body.at("reasoning").at("summary").as_string() == "detailed");
        SMOKE_CHECK(request.body.at("tools").as_array().front().at("parameters")
                   .at("properties").at("query").at("type").as_string() == "string");
        const auto& input = request.body.at("input").as_array();
        SMOKE_CHECK(input.size() == 1);
        SMOKE_CHECK(input.front().at("type").as_string() == "message");
        SMOKE_CHECK(input.front().at("role").as_string() == "user");
        SMOKE_CHECK(input.front().at("content").as_array().front().at("text").as_string() == "responses input");
        return agent::Value::object({
            {"id", "resp_responses"},
            {"model", "gpt-responses"},
            {"status", "completed"},
            {"output", agent::Value::array({
                agent::Value::object({
                    {"type", "reasoning"},
                    {"summary", agent::Value::array({
                        agent::Value::object({{"text", "summary reason"}}),
                    })},
                }),
                agent::Value::object({
                    {"type", "message"},
                    {"content", agent::Value::array({
                        agent::Value::object({{"type", "output_text"}, {"text", "responses ok"}}),
                    })},
                }),
                agent::Value::object({
                    {"type", "function_call"},
                    {"call_id", "call_lookup"},
                    {"name", "lookup"},
                    {"arguments", "{\"query\":\"native\"}"},
                }),
            })},
        });
      },
      "/v1/chat/completions",
      "https://api.openai.responses",
      "responses-key");
  auto provider_responses = provider_responses_model.generate(agent::GenerateParams{
      .messages = {
          agent::create_message(agent::MessageRole::System, "responses system"),
          agent::create_message(agent::MessageRole::User, "responses input"),
      },
      .tools = {lookup_tool},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{
              .enabled = true,
              .budget = std::string("high"),
              .include_thoughts = true,
          },
      },
  });
  SMOKE_CHECK(provider_responses_called);
  SMOKE_CHECK(provider_responses.text == "responses ok");
  SMOKE_CHECK(provider_responses.reasoning);
  SMOKE_CHECK(provider_responses.reasoning->text == "summary reason");
  SMOKE_CHECK(provider_responses.reasoning->format == "summary");
  SMOKE_CHECK(provider_responses.tool_calls.size() == 1);
  SMOKE_CHECK(provider_responses.tool_calls.front().id == "call_lookup");
  SMOKE_CHECK(provider_responses.tool_calls.front().arguments.at("query").as_string() == "native");
  SMOKE_CHECK(provider_responses.finish_reason == "tool_calls");

  bool provider_responses_stream_called = false;
  agent::OpenAICompatibleChatModelAdapter provider_responses_stream_model(
      "openai",
      "gpt-responses-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "/v1/chat/completions",
      "https://api.openai.responses",
      "responses-stream-key",
      [&](const agent::NativeProviderRequest& request) {
        provider_responses_stream_called = true;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.endpoint == "/v1/responses");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("reasoning").at("effort").as_string() == "medium");
        return std::vector<std::string>{
            "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"stream reason\"}\n\n",
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"stream \"}\n\n",
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"answer\"}\n\n",
            "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\",\"call_id\":\"call_stream\",\"name\":\"lookup\",\"arguments\":\"{\\\"query\\\"\"}}\n\n",
            "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\":\\\"stream\\\"}\"}\n\n",
            "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_stream\",\"model\":\"gpt-responses-stream\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"stream answer\"}]},{\"type\":\"reasoning\",\"summary\":[{\"text\":\"completed reason\"}]},{\"type\":\"function_call\",\"call_id\":\"call_stream\",\"name\":\"lookup\",\"arguments\":\"{\\\"query\\\":\\\"stream\\\"}\"}]}}\n\n",
            "data: [DONE]\n\n",
        };
      });
  auto provider_responses_stream = provider_responses_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "stream input")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(provider_responses_stream_called);
  SMOKE_CHECK(provider_responses_stream.size() == 7);
  SMOKE_CHECK(provider_responses_stream[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(provider_responses_stream[1].reasoning == "stream reason");
  SMOKE_CHECK(provider_responses_stream[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(provider_responses_stream[2].text == "stream ");
  SMOKE_CHECK(provider_responses_stream[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(provider_responses_stream[3].text == "stream answer");
  SMOKE_CHECK(provider_responses_stream[4].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(provider_responses_stream[4].tool_call_id == "call_stream");
  SMOKE_CHECK(provider_responses_stream[4].tool_call_name == "lookup");
  SMOKE_CHECK(provider_responses_stream[4].tool_call_args_delta == "{\"query\"");
  SMOKE_CHECK(provider_responses_stream[4].tool_call_args_accumulated == "{\"query\"");
  SMOKE_CHECK(provider_responses_stream[5].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(provider_responses_stream[5].tool_call_args_delta == ":\"stream\"}");
  SMOKE_CHECK(provider_responses_stream[5].tool_call_args_accumulated == "{\"query\":\"stream\"}");
  SMOKE_CHECK(provider_responses_stream[6].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(provider_responses_stream[6].response.text == "stream answer");
  SMOKE_CHECK(provider_responses_stream[6].response.reasoning);
  SMOKE_CHECK(provider_responses_stream[6].response.reasoning->text == "stream reason");
  SMOKE_CHECK(provider_responses_stream[6].response.tool_calls.size() == 1);
  SMOKE_CHECK(provider_responses_stream[6].response.tool_calls.front().arguments.at("query").as_string() == "stream");

  bool configured_provider_stream_called = false;
  auto configured_provider_stream_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", agent::Value::object({
                      {"provider", "openai"},
                      {"model", "gpt-config-stream"},
                      {"apiKey", "config-stream-key"},
                      {"baseUrl", "https://api.openai.config"},
                  })},
              })},
          })},
      }),
      "main",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      {},
      {},
      {},
      {},
      [&](const agent::NativeProviderRequest& request) {
        configured_provider_stream_called = true;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer config-stream-key");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        return std::vector<std::string>{
            "data: {\"id\":\"cfg_stream\",\"model\":\"gpt-config-stream\",\"choices\":[{\"delta\":{\"content\":\"config stream\"},\"finish_reason\":\"stop\"}]}\n\n",
            "data: [DONE]\n\n",
        };
      });
  auto configured_provider_stream = configured_provider_stream_app.runner->stream(
      "config stream input",
      "config-stream-session");
  SMOKE_CHECK(configured_provider_stream_called);
  SMOKE_CHECK(configured_provider_stream.result.text == "config stream");

  bool configured_ollama_stream_called = false;
  auto configured_ollama_stream_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", agent::Value::object({
                      {"provider", "ollama"},
                      {"model", "llama-config-stream"},
                      {"baseUrl", "http://127.0.0.1:11434/"},
                  })},
              })},
          })},
      }),
      "main",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      {},
      {},
      {},
      {},
      [&](const agent::NativeProviderRequest& request) {
        configured_ollama_stream_called = true;
        SMOKE_CHECK(request.provider == "ollama");
        SMOKE_CHECK(request.endpoint == "/chat");
        SMOKE_CHECK(request.base_url == "http://127.0.0.1:11434/api");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        return std::vector<std::string>{
            "{\"model\":\"llama-config-stream\",\"message\":{\"content\":\"config ollama\"}}\n",
            "{\"model\":\"llama-config-stream\",\"created_at\":\"ollama_cfg\",\"done\":true,\"done_reason\":\"stop\",\"message\":{}}\n",
        };
      });
  auto configured_ollama_stream = configured_ollama_stream_app.runner->stream(
      "config ollama stream input",
      "ollama-config-stream-session");
  SMOKE_CHECK(configured_ollama_stream_called);
  SMOKE_CHECK(configured_ollama_stream.result.text == "config ollama");

  bool configured_gemini_stream_called = false;
  auto configured_gemini_stream_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", agent::Value::object({
                      {"provider", "gemini"},
                      {"model", "gemini-config-stream"},
                      {"apiKey", "gemini-config-stream-key"},
                      {"baseUrl", "https://generativelanguage.googleapis.com/v1beta/"},
                  })},
              })},
          })},
      }),
      "main",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      {},
      {},
      {},
      {},
      [&](const agent::NativeProviderRequest& request) {
        configured_gemini_stream_called = true;
        SMOKE_CHECK(request.provider == "gemini");
        SMOKE_CHECK(request.endpoint == "/models/gemini-config-stream:streamGenerateContent?alt=sse");
        SMOKE_CHECK(request.base_url == "https://generativelanguage.googleapis.com/v1beta");
        SMOKE_CHECK(request.headers.at("x-goog-api-key") == "gemini-config-stream-key");
        return std::vector<std::string>{
            "data: {\"responseId\":\"gemini_cfg\",\"modelVersion\":\"gemini-config-stream\",\"candidates\":[{\"finishReason\":\"STOP\",\"content\":{\"parts\":[{\"text\":\"config gemini\"}]}}]}\n\n",
        };
      });
  auto configured_gemini_stream = configured_gemini_stream_app.runner->stream(
      "config gemini stream input",
      "gemini-config-stream-session");
	  SMOKE_CHECK(configured_gemini_stream_called);
	  SMOKE_CHECK(configured_gemini_stream.result.text == "config gemini");
	
	  bool missing_qwen_key_called = false;
	  agent::QwenChatModelAdapter missing_qwen_key_model("qwen-missing-key",
	      [&](const agent::NativeProviderRequest&) {
	        missing_qwen_key_called = true;
	        return agent::Value::object({});
	      });
	  bool missing_qwen_key_failed = false;
	  try {
	    (void)missing_qwen_key_model.generate(agent::GenerateParams{
	        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
	    });
	  } catch (const agent::ConfigurationError& error) {
	    missing_qwen_key_failed =
	        std::string(error.what()).find("Qwen adapter requires apiKey or QWEN_API_KEY.") != std::string::npos;
	  }
	  SMOKE_CHECK(missing_qwen_key_failed);
	  SMOKE_CHECK(!missing_qwen_key_called);
	
	  bool qwen_called = false;
	  agent::QwenChatModelAdapter qwen_model("qwen-plus",
      [&](const agent::NativeProviderRequest& request) {
        qwen_called = true;
        SMOKE_CHECK(request.provider == "qwen");
        SMOKE_CHECK(request.endpoint == "/chat/completions");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer qwen-key");
        SMOKE_CHECK(request.body.at("model").as_string() == "qwen-plus");
        SMOKE_CHECK(request.body.at("enable_thinking").as_bool());
        SMOKE_CHECK(request.body.at("thinking_budget").as_integer() == 32);
        return agent::Value::object({
            {"id", "qwen_1"},
            {"choices", agent::Value::array({
                agent::Value::object({
                    {"message", agent::Value::object({
                        {"content", "qwen ok"},
                        {"reasoning_content", "qwen reasoning"},
                    })},
                    {"finish_reason", "stop"},
                }),
            })},
            {"usage", agent::Value::object({{"prompt_tokens", 4}, {"completion_tokens", 3}})},
        });
      },
      "qwen-key");
  auto qwen_response = qwen_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true, .budget = 32.0},
      },
  });
  SMOKE_CHECK(qwen_called);
  SMOKE_CHECK(qwen_response.text == "qwen ok");
  SMOKE_CHECK(qwen_response.reasoning && qwen_response.reasoning->text == "qwen reasoning");

  bool qwen_stream_called = false;
  agent::QwenChatModelAdapter qwen_stream_model(
      "qwen-plus",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "qwen-stream-key",
      "https://dashscope.aliyuncs.com/compatible-mode/v1/",
      [&](const agent::NativeProviderRequest& request) {
        qwen_stream_called = true;
        SMOKE_CHECK(request.provider == "qwen");
        SMOKE_CHECK(request.endpoint == "/chat/completions");
        SMOKE_CHECK(request.base_url == "https://dashscope.aliyuncs.com/compatible-mode/v1");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer qwen-stream-key");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("enable_thinking").as_bool());
        return std::vector<std::string>{
            "data: {\"id\":\"qwen_stream\",\"model\":\"qwen-plus\",\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n",
            "data: {\"id\":\"qwen_stream\",\"model\":\"qwen-plus\",\"choices\":[{\"delta\":{\"content\":\" answer\"},\"finish_reason\":\"stop\"}]}\n\n",
            "data: [DONE]\n\n",
        };
      });
  auto qwen_stream_events = qwen_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(qwen_stream_called);
  SMOKE_CHECK(qwen_stream_events.size() == 4);
  SMOKE_CHECK(qwen_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(qwen_stream_events[1].reasoning == "think");
  SMOKE_CHECK(qwen_stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(qwen_stream_events[2].text == " answer");
  SMOKE_CHECK(qwen_stream_events[3].response.reasoning);
  SMOKE_CHECK(qwen_stream_events[3].response.reasoning->text == "think");
  SMOKE_CHECK(qwen_stream_events[3].response.text == " answer");

  bool qwen_responses_called = false;
  agent::QwenChatModelAdapter qwen_responses_model(
      "qwen-vl",
      [&](const agent::NativeProviderRequest& request) {
        qwen_responses_called = true;
        SMOKE_CHECK(request.provider == "qwen");
        SMOKE_CHECK(request.endpoint == "/responses");
        SMOKE_CHECK(request.base_url == "https://dashscope.aliyuncs.com/compatible-mode/v1");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer qwen-responses-key");
        const auto& input = request.body.at("input").as_array();
        SMOKE_CHECK(input.size() == 1);
        const auto& content = input.front().at("content").as_array();
        SMOKE_CHECK(content.size() == 2);
        SMOKE_CHECK(content.front().at("type").as_string() == "input_text");
        SMOKE_CHECK(content.front().at("text").as_string() == "describe image");
        SMOKE_CHECK(content.back().at("type").as_string() == "input_image");
        SMOKE_CHECK(content.back().at("image_url").as_string() == "https://example.test/image.png");
        SMOKE_CHECK(content.back().at("detail").as_string() == "low");
        return agent::Value::object({
            {"id", "qwen_responses"},
            {"model", "qwen-vl"},
            {"status", "completed"},
            {"output", agent::Value::array({
                agent::Value::object({
                    {"type", "message"},
                    {"content", agent::Value::array({
                        agent::Value::object({{"type", "output_text"}, {"text", "qwen image ok"}}),
                    })},
                }),
            })},
        });
      },
      "qwen-responses-key",
      "https://dashscope.aliyuncs.com/compatible-mode/v1/");
  auto qwen_responses = qwen_responses_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, {
          agent::text_part("describe image"),
          agent::image_part(agent::MediaSource{
              .kind = agent::MediaSourceKind::Url,
              .url = "https://example.test/image.png",
              .mime_type = "image/png",
          }, "sample", "low"),
      })},
  });
  SMOKE_CHECK(qwen_responses_called);
  SMOKE_CHECK(qwen_responses.text == "qwen image ok");

  bool qwen_reasoning_responses_called = false;
  agent::QwenChatModelAdapter qwen_reasoning_responses_model(
      "qwen-reasoning",
      [&](const agent::NativeProviderRequest& request) {
        qwen_reasoning_responses_called = true;
        SMOKE_CHECK(request.provider == "qwen");
        SMOKE_CHECK(request.endpoint == "/responses");
        SMOKE_CHECK(request.body.at("reasoning").at("effort").as_string() == "low");
        return agent::Value::object({
            {"id", "qwen_reasoning_responses"},
            {"model", "qwen-reasoning"},
            {"status", "completed"},
            {"output", agent::Value::array({
                agent::Value::object({
                    {"type", "reasoning"},
                    {"content", agent::Value::array({
                        agent::Value::object({{"text", "qwen responses reasoning"}}),
                    })},
                }),
                agent::Value::object({
                    {"type", "message"},
                    {"content", agent::Value::array({
                        agent::Value::object({{"type", "output_text"}, {"text", "qwen responses ok"}}),
                    })},
                }),
            })},
        });
      },
      "qwen-reasoning-key",
      "https://dashscope.aliyuncs.com/compatible-mode/v1",
      agent::NativeProviderStreamTransport{},
      "responses");
  auto qwen_reasoning_responses = qwen_reasoning_responses_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "reasoning input")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{
              .enabled = true,
              .budget = std::string("low"),
          },
      },
  });
  SMOKE_CHECK(qwen_reasoning_responses_called);
  SMOKE_CHECK(qwen_reasoning_responses.text == "qwen responses ok");
	  SMOKE_CHECK(qwen_reasoning_responses.reasoning);
	  SMOKE_CHECK(qwen_reasoning_responses.reasoning->text == "qwen responses reasoning");
	  SMOKE_CHECK(qwen_reasoning_responses.reasoning->format == "provider-visible");

	  bool missing_mimo_key_called = false;
	  agent::MiMoChatModelAdapter missing_mimo_key_model("mimo-missing-key",
	      [&](const agent::NativeProviderRequest&) {
	        missing_mimo_key_called = true;
	        return agent::Value::object({});
	      });
	  bool missing_mimo_key_failed = false;
	  try {
	    (void)missing_mimo_key_model.generate(agent::GenerateParams{
	        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
	    });
	  } catch (const agent::ConfigurationError& error) {
	    missing_mimo_key_failed =
	        std::string(error.what()).find("MiMo adapter requires apiKey or MIMO_API_KEY.") != std::string::npos;
	  }
	  SMOKE_CHECK(missing_mimo_key_failed);
	  SMOKE_CHECK(!missing_mimo_key_called);

	  bool mimo_called = false;
	  agent::MiMoChatModelAdapter mimo_model("mimo-v2-flash",
	      [&](const agent::NativeProviderRequest& request) {
	        mimo_called = true;
	        SMOKE_CHECK(request.provider == "mimo");
	        SMOKE_CHECK(request.endpoint == "/chat/completions");
	        SMOKE_CHECK(request.base_url == "https://api.xiaomimimo.com/v1");
	        SMOKE_CHECK(request.headers.at("authorization") == "Bearer mimo-key");
	        SMOKE_CHECK(request.body.at("model").as_string() == "mimo-v2-flash");
	        SMOKE_CHECK(!request.body.contains("enable_thinking"));
	        SMOKE_CHECK(!request.body.contains("thinking_budget"));
	        SMOKE_CHECK(request.body.at("messages").as_array().size() == 1);
	        return agent::Value::object({
	            {"id", "mimo_1"},
	            {"choices", agent::Value::array({
	                agent::Value::object({
	                    {"message", agent::Value::object({
	                        {"content", "mimo ok"},
	                    })},
	                    {"finish_reason", "stop"},
	                }),
	            })},
	            {"usage", agent::Value::object({{"prompt_tokens", 3}, {"completion_tokens", 2}})},
	        });
	      },
	      "mimo-key");
	  auto mimo_response = mimo_model.generate(agent::GenerateParams{
	      .messages = {agent::create_message(agent::MessageRole::User, "hello mimo")},
	  });
	  SMOKE_CHECK(mimo_called);
	  SMOKE_CHECK(mimo_response.text == "mimo ok");
	  SMOKE_CHECK(mimo_response.provider == "mimo");
	  SMOKE_CHECK(mimo_response.model == "mimo-v2-flash");

	  bool mimo_stream_called = false;
	  agent::MiMoChatModelAdapter mimo_stream_model(
	      "mimo-v2-flash",
	      [](const agent::NativeProviderRequest&) -> agent::Value {
	        SMOKE_CHECK(false);
	        return agent::Value::object({});
	      },
	      "mimo-stream-key",
	      "https://api.xiaomimimo.com/v1/",
	      [&](const agent::NativeProviderRequest& request) {
	        mimo_stream_called = true;
	        SMOKE_CHECK(request.provider == "mimo");
	        SMOKE_CHECK(request.endpoint == "/chat/completions");
	        SMOKE_CHECK(request.base_url == "https://api.xiaomimimo.com/v1");
	        SMOKE_CHECK(request.headers.at("authorization") == "Bearer mimo-stream-key");
	        SMOKE_CHECK(request.body.at("stream").as_bool());
	        SMOKE_CHECK(request.body.at("stream_options").at("include_usage").as_bool());
	        SMOKE_CHECK(!request.body.contains("enable_thinking"));
	        return std::vector<std::string>{
	            "data: {\"id\":\"mimo_stream\",\"model\":\"mimo-v2-flash\",\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n",
	            "data: [DONE]\n\n",
	        };
	      });
	  auto mimo_stream_events = mimo_stream_model.stream(agent::GenerateParams{
	      .messages = {agent::create_message(agent::MessageRole::User, "hello stream")},
	  });
	  SMOKE_CHECK(mimo_stream_called);
	  SMOKE_CHECK(!mimo_stream_events.empty());
	  SMOKE_CHECK(mimo_stream_events.back().response.text == "hi");
	  SMOKE_CHECK(mimo_stream_events.back().response.provider == "mimo");

	  bool missing_anthropic_key_called = false;
	  agent::AnthropicChatModelAdapter missing_anthropic_key_model("claude-missing-key",
	      [&](const agent::NativeProviderRequest&) {
	        missing_anthropic_key_called = true;
	        return agent::Value::object({});
	      });
	  bool missing_anthropic_key_failed = false;
	  try {
	    (void)missing_anthropic_key_model.generate(agent::GenerateParams{
	        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
	    });
	  } catch (const agent::ConfigurationError& error) {
	    missing_anthropic_key_failed =
	        std::string(error.what()).find("Anthropic adapter requires apiKey or ANTHROPIC_API_KEY.") !=
	        std::string::npos;
	  }
	  SMOKE_CHECK(missing_anthropic_key_failed);
	  SMOKE_CHECK(!missing_anthropic_key_called);
	
	  bool anthropic_called = false;
	  agent::AnthropicChatModelAdapter anthropic_model("claude-test",
      [&](const agent::NativeProviderRequest& request) {
        anthropic_called = true;
        SMOKE_CHECK(request.provider == "anthropic");
        SMOKE_CHECK(request.endpoint == "/messages");
        SMOKE_CHECK(request.headers.at("x-api-key") == "anthropic-key");
        SMOKE_CHECK(request.headers.at("anthropic-version") == "2023-06-01");
        SMOKE_CHECK(request.body.at("model").as_string() == "claude-test");
        SMOKE_CHECK(request.body.at("system").as_string() == "system prompt");
        SMOKE_CHECK(!request.body.at("messages").as_array().empty());
        SMOKE_CHECK(request.body.at("thinking").at("budget_tokens").as_integer() == 64);
        return agent::Value::object({
            {"id", "anthropic_1"},
            {"model", "claude-test"},
            {"stop_reason", "tool_use"},
            {"content", agent::Value::array({
                agent::Value::object({{"type", "thinking"}, {"thinking", "anthropic reasoning"}}),
                agent::Value::object({{"type", "text"}, {"text", "anthropic ok"}}),
                agent::Value::object({
                    {"type", "tool_use"},
                    {"id", "tool_1"},
                    {"name", "lookup"},
                    {"input", agent::Value::object({{"query", "native"}})},
                }),
            })},
            {"usage", agent::Value::object({{"input_tokens", 5}, {"output_tokens", 4}})},
        });
      },
      "anthropic-key");
  auto anthropic_response = anthropic_model.generate(agent::GenerateParams{
      .messages = {
          agent::create_message(agent::MessageRole::System, "system prompt"),
          agent::create_message(agent::MessageRole::User, "hello"),
      },
      .tools = {agent::ChatToolDescriptor{
          .name = "lookup",
          .description = "Lookup data",
      }},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true, .budget = 64.0},
      },
  });
  SMOKE_CHECK(anthropic_called);
  SMOKE_CHECK(anthropic_response.text == "anthropic ok");
  SMOKE_CHECK(anthropic_response.reasoning && anthropic_response.reasoning->text == "anthropic reasoning");
  SMOKE_CHECK(anthropic_response.tool_calls.size() == 1);
  SMOKE_CHECK(anthropic_response.tool_calls.front().name == "lookup");

  bool anthropic_stream_called = false;
  agent::AnthropicChatModelAdapter anthropic_stream_model(
      "claude-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "anthropic-stream-key",
      "https://api.anthropic.test/v1/",
      [&](const agent::NativeProviderRequest& request) {
        anthropic_stream_called = true;
        SMOKE_CHECK(request.provider == "anthropic");
        SMOKE_CHECK(request.endpoint == "/messages");
        SMOKE_CHECK(request.base_url == "https://api.anthropic.test/v1");
        SMOKE_CHECK(request.headers.at("x-api-key") == "anthropic-stream-key");
        SMOKE_CHECK(request.headers.at("anthropic-version") == "2023-06-01");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("thinking").at("budget_tokens").as_integer() == 4096);
        return std::vector<std::string>{
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_stream\",\"model\":\"claude-stream\"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"reason\"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"anthropic \"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"stream\"}}\n\n",
            "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"tool_stream\",\"name\":\"lookup\",\"input\":{}}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"query\\\"\"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\":\\\"native\\\"}\"}}\n\n",
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n",
            "data: {\"type\":\"message_stop\"}\n\n",
        };
      });
  auto anthropic_stream_events = anthropic_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .tools = {agent::ChatToolDescriptor{.name = "lookup", .description = "Lookup data"}},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(anthropic_stream_called);
  SMOKE_CHECK(anthropic_stream_events.size() == 7);
  SMOKE_CHECK(anthropic_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(anthropic_stream_events[1].reasoning == "reason");
  SMOKE_CHECK(anthropic_stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(anthropic_stream_events[2].text == "anthropic ");
  SMOKE_CHECK(anthropic_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(anthropic_stream_events[3].text == "anthropic stream");
  SMOKE_CHECK(anthropic_stream_events[4].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(anthropic_stream_events[4].tool_call_id == "tool_stream");
  SMOKE_CHECK(anthropic_stream_events[4].tool_call_name == "lookup");
  SMOKE_CHECK(anthropic_stream_events[4].tool_call_args_delta == "{\"query\"");
  SMOKE_CHECK(anthropic_stream_events[4].tool_call_args_accumulated == "{\"query\"");
  SMOKE_CHECK(anthropic_stream_events[5].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(anthropic_stream_events[5].tool_call_args_delta == ":\"native\"}");
  SMOKE_CHECK(anthropic_stream_events[5].tool_call_args_accumulated == "{\"query\":\"native\"}");
  SMOKE_CHECK(anthropic_stream_events[6].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(anthropic_stream_events[6].response.text == "anthropic stream");
  SMOKE_CHECK(anthropic_stream_events[6].response.reasoning);
  SMOKE_CHECK(anthropic_stream_events[6].response.reasoning->text == "reason");
  SMOKE_CHECK(anthropic_stream_events[6].response.tool_calls.size() == 1);
	  SMOKE_CHECK(anthropic_stream_events[6].response.tool_calls.front().arguments.at("query").as_string() == "native");
	  SMOKE_CHECK(anthropic_stream_events[6].response.finish_reason == "tool_calls");
	
	  bool missing_deepseek_key_called = false;
	  agent::DeepSeekChatModelAdapter missing_deepseek_key_model("deepseek-missing-key",
	      [&](const agent::NativeProviderRequest&) {
	        missing_deepseek_key_called = true;
	        return agent::Value::object({});
	      });
	  bool missing_deepseek_key_failed = false;
	  try {
	    (void)missing_deepseek_key_model.generate(agent::GenerateParams{
	        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
	    });
	  } catch (const agent::ConfigurationError& error) {
	    missing_deepseek_key_failed =
	        std::string(error.what()).find("DeepSeek adapter requires apiKey or DEEPSEEK_API_KEY.") !=
	        std::string::npos;
	  }
	  SMOKE_CHECK(missing_deepseek_key_failed);
	  SMOKE_CHECK(!missing_deepseek_key_called);
	
	  bool deepseek_called = false;
	  agent::DeepSeekChatModelAdapter deepseek_model("deepseek-reasoner",
      [&](const agent::NativeProviderRequest& request) {
        deepseek_called = true;
        SMOKE_CHECK(request.provider == "deepseek");
        SMOKE_CHECK(request.endpoint == "/messages");
        SMOKE_CHECK(request.headers.at("x-api-key") == "deepseek-key");
        SMOKE_CHECK(request.headers.find("anthropic-version") == request.headers.end());
        SMOKE_CHECK(request.body.at("thinking").at("type").as_string() == "enabled");
        return agent::Value::object({
            {"id", "deepseek_1"},
            {"model", "deepseek-reasoner"},
            {"stop_reason", "end_turn"},
            {"content", agent::Value::array({
                agent::Value::object({{"type", "thinking"}, {"thinking", "deepseek reasoning"}}),
                agent::Value::object({{"type", "text"}, {"text", "<think>tagged hidden</think>deepseek ok"}}),
            })},
            {"usage", agent::Value::object({{"input_tokens", 6}, {"output_tokens", 5}})},
        });
      },
      "deepseek-key");
  auto deepseek_response = deepseek_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(deepseek_called);
  SMOKE_CHECK(deepseek_response.text == "deepseek ok");
  SMOKE_CHECK(deepseek_response.reasoning && deepseek_response.reasoning->text == "deepseek reasoning");

  bool deepseek_stream_called = false;
  agent::DeepSeekChatModelAdapter deepseek_stream_model(
      "deepseek-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "deepseek-stream-key",
      "https://api.deepseek.test/anthropic/",
      [&](const agent::NativeProviderRequest& request) {
        deepseek_stream_called = true;
        SMOKE_CHECK(request.provider == "deepseek");
        SMOKE_CHECK(request.endpoint == "/messages");
        SMOKE_CHECK(request.base_url == "https://api.deepseek.test/anthropic");
        SMOKE_CHECK(request.headers.at("x-api-key") == "deepseek-stream-key");
        SMOKE_CHECK(request.headers.find("anthropic-version") == request.headers.end());
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("thinking").at("type").as_string() == "enabled");
        SMOKE_CHECK(!request.body.at("thinking").contains("budget_tokens"));
        return std::vector<std::string>{
            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"ds_stream\",\"model\":\"deepseek-stream\"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"deep\"}}\n\n",
            "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"<think>tagged stream</think>seek stream\"}}\n\n",
            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
            "data: {\"type\":\"message_stop\"}\n\n",
        };
      });
  auto deepseek_stream_events = deepseek_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(deepseek_stream_called);
  SMOKE_CHECK(deepseek_stream_events.size() == 5);
  SMOKE_CHECK(deepseek_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(deepseek_stream_events[1].reasoning == "deep");
  SMOKE_CHECK(deepseek_stream_events[2].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(deepseek_stream_events[2].delta == "tagged stream");
  SMOKE_CHECK(deepseek_stream_events[2].reasoning == "tagged stream");
  SMOKE_CHECK(deepseek_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(deepseek_stream_events[3].text == "seek stream");
  SMOKE_CHECK(deepseek_stream_events[4].response.reasoning);
  SMOKE_CHECK(deepseek_stream_events[4].response.reasoning->text == "deep");
  SMOKE_CHECK(deepseek_stream_events[4].response.text == "seek stream");
  SMOKE_CHECK(deepseek_stream_events[4].response.finish_reason == "stop");

  bool ollama_called = false;
  agent::OllamaChatModelAdapter ollama_model("llama3.1",
      [&](const agent::NativeProviderRequest& request) {
        ollama_called = true;
        SMOKE_CHECK(request.provider == "ollama");
        SMOKE_CHECK(request.endpoint == "/chat");
        SMOKE_CHECK(request.base_url == "http://127.0.0.1:11434/api");
        SMOKE_CHECK(!request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("think").as_bool());
        SMOKE_CHECK(request.body.at("options").at("temperature").as_number() == 0.2);
        return agent::Value::object({
            {"model", "llama3.1"},
            {"done", true},
            {"done_reason", "stop"},
            {"message", agent::Value::object({
                {"content", "ollama ok"},
                {"thinking", "ollama reasoning"},
                {"tool_calls", agent::Value::array({
                    agent::Value::object({
                        {"function", agent::Value::object({
                            {"name", "echo"},
                            {"arguments", agent::Value::object({{"value", 1}})},
                        })},
                    }),
                })},
            })},
        });
      });
  auto ollama_response = ollama_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .temperature = 0.2,
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(ollama_called);
  SMOKE_CHECK(ollama_response.text == "ollama ok");
  SMOKE_CHECK(ollama_response.reasoning && ollama_response.reasoning->text == "ollama reasoning");
  SMOKE_CHECK(ollama_response.tool_calls.size() == 1);
  SMOKE_CHECK(ollama_response.tool_calls.front().name == "echo");

  bool ollama_tagged_called = false;
  agent::OllamaChatModelAdapter ollama_tagged_model("llama-tagged",
      [&](const agent::NativeProviderRequest&) {
        ollama_tagged_called = true;
        return agent::Value::object({
            {"model", "llama-tagged"},
            {"done", true},
            {"done_reason", "stop"},
            {"message", agent::Value::object({
                {"content", "<think>tagged reasoning</think>tagged answer"},
            })},
        });
      });
  auto ollama_tagged_response = ollama_tagged_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(ollama_tagged_called);
  SMOKE_CHECK(ollama_tagged_response.text == "tagged answer");
  SMOKE_CHECK(ollama_tagged_response.content.size() == 1);
  SMOKE_CHECK(ollama_tagged_response.content.front().text == "tagged answer");
  SMOKE_CHECK(ollama_tagged_response.reasoning);
  SMOKE_CHECK(ollama_tagged_response.reasoning->text == "tagged reasoning");
  SMOKE_CHECK(ollama_tagged_response.reasoning->format == "tagged");

  bool ollama_tagged_stream_called = false;
  agent::OllamaChatModelAdapter ollama_tagged_stream_model(
      "llama-tagged-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "http://127.0.0.1:11434/",
      [&](const agent::NativeProviderRequest& request) {
        ollama_tagged_stream_called = true;
        SMOKE_CHECK(request.body.at("think").as_bool());
        return std::vector<std::string>{
            "{\"model\":\"llama-tagged-stream\",\"message\":{\"content\":\"<think>先\"}}\n",
            "{\"model\":\"llama-tagged-stream\",\"message\":{\"content\":\"分析</think>最后\"}}\n",
            "{\"model\":\"llama-tagged-stream\",\"done\":true,\"done_reason\":\"stop\",\"message\":{\"content\":\"答案\"}}\n",
        };
      });
  auto ollama_tagged_stream_events = ollama_tagged_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(ollama_tagged_stream_called);
  SMOKE_CHECK(ollama_tagged_stream_events.size() == 6);
  SMOKE_CHECK(ollama_tagged_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(ollama_tagged_stream_events[1].delta == "先");
  SMOKE_CHECK(ollama_tagged_stream_events[2].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(ollama_tagged_stream_events[2].delta == "分析");
  SMOKE_CHECK(ollama_tagged_stream_events[2].reasoning == "先分析");
  SMOKE_CHECK(ollama_tagged_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(ollama_tagged_stream_events[3].text == "最后");
  SMOKE_CHECK(ollama_tagged_stream_events[4].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(ollama_tagged_stream_events[4].text == "最后答案");
  SMOKE_CHECK(ollama_tagged_stream_events[5].response.text == "最后答案");
  SMOKE_CHECK(ollama_tagged_stream_events[5].response.reasoning);
  SMOKE_CHECK(ollama_tagged_stream_events[5].response.reasoning->text == "先分析");
  SMOKE_CHECK(ollama_tagged_stream_events[5].response.reasoning->format == "tagged");

  bool ollama_stream_called = false;
  agent::OllamaChatModelAdapter ollama_stream_model(
      "llama-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "http://127.0.0.1:11434/",
      [&](const agent::NativeProviderRequest& request) {
        ollama_stream_called = true;
        SMOKE_CHECK(request.provider == "ollama");
        SMOKE_CHECK(request.endpoint == "/chat");
        SMOKE_CHECK(request.base_url == "http://127.0.0.1:11434/api");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        SMOKE_CHECK(request.body.at("think").as_bool());
        return std::vector<std::string>{
            "{\"model\":\"llama-stream\",\"message\":{\"thinking\":\"ollama \"}}\n",
            "{\"model\":\"llama-stream\",\"message\":{\"content\":\"stream \"}}\n",
            "{\"model\":\"llama-stream\",\"message\":{\"content\":\"ok\"}}\n",
            "{\"model\":\"llama-stream\",\"created_at\":\"ollama_done\",\"done\":true,\"done_reason\":\"stop\",\"message\":{\"tool_calls\":[{\"function\":{\"name\":\"echo\",\"arguments\":{\"value\":2}}}]}}\n",
        };
      });
  auto ollama_stream_events = ollama_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{.enabled = true},
      },
  });
  SMOKE_CHECK(ollama_stream_called);
  // Six events: ResponseStart, ReasoningDelta, TextDelta x2, ToolCallDelta
  // (one-shot from the tool_calls block), Response.
  SMOKE_CHECK(ollama_stream_events.size() == 6);
  SMOKE_CHECK(ollama_stream_events[0].type == agent::ModelStreamEventType::ResponseStart);
  SMOKE_CHECK(ollama_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(ollama_stream_events[1].reasoning == "ollama ");
  SMOKE_CHECK(ollama_stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(ollama_stream_events[2].text == "stream ");
  SMOKE_CHECK(ollama_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(ollama_stream_events[3].text == "stream ok");
  SMOKE_CHECK(ollama_stream_events[4].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(ollama_stream_events[4].tool_call_name == "echo");
  SMOKE_CHECK(ollama_stream_events[4].tool_call_args_delta == ollama_stream_events[4].tool_call_args_accumulated);
  SMOKE_CHECK(ollama_stream_events[4].tool_call_args_accumulated.find("\"value\"") != std::string::npos);
  SMOKE_CHECK(ollama_stream_events[5].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(ollama_stream_events[5].response.text == "stream ok");
  SMOKE_CHECK(ollama_stream_events[5].response.reasoning);
  SMOKE_CHECK(ollama_stream_events[5].response.reasoning->text == "ollama");
	  SMOKE_CHECK(ollama_stream_events[5].response.tool_calls.size() == 1);
	  SMOKE_CHECK(ollama_stream_events[5].response.tool_calls.front().name == "echo");
	  SMOKE_CHECK(ollama_stream_events[5].response.tool_calls.front().arguments.at("value").as_integer() == 2);
	  SMOKE_CHECK(ollama_stream_events[5].response.finish_reason == "tool_calls");

  auto slow_ollama_stream_model = std::make_shared<agent::OllamaChatModelAdapter>(
      "llama-slow-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "http://127.0.0.1:11434/",
      agent::NativeProviderStreamTransport{},
      [](const agent::NativeProviderRequest& request, agent::NativeProviderStreamingChunkHandler on_chunk) {
        SMOKE_CHECK(request.provider == "ollama");
        SMOKE_CHECK(request.body.at("stream").as_bool());
        const std::vector<std::string> chunks{
            "{\"model\":\"llama-slow-stream\",\"message\":{\"content\":\"one \"}}\n",
            "{\"model\":\"llama-slow-stream\",\"message\":{\"content\":\"two \"}}\n",
            "{\"model\":\"llama-slow-stream\",\"message\":{\"content\":\"three\"}}\n",
            "{\"model\":\"llama-slow-stream\",\"done\":true,\"done_reason\":\"stop\",\"message\":{}}\n",
        };
        for (const auto& chunk : chunks) {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          on_chunk(chunk);
        }
      });
  agent::AgentRunner slow_ollama_runner(agent::AgentRunnerConfig{
      .adapter = slow_ollama_stream_model,
  });
  std::vector<std::chrono::steady_clock::time_point> slow_ollama_delta_times;
  const auto slow_ollama_sink_id =
      slow_ollama_runner.register_event_sink([&](const agent::FrameworkEvent& event) {
        if (event.category == "model.delta") {
          slow_ollama_delta_times.push_back(std::chrono::steady_clock::now());
        }
      });
  const auto slow_ollama_result = slow_ollama_runner.stream("hello", "slow-ollama-stream");
  slow_ollama_runner.unregister_event_sink(slow_ollama_sink_id);
  SMOKE_CHECK(slow_ollama_result.result.text == "one two three");
  SMOKE_CHECK(slow_ollama_delta_times.size() >= 3);
  for (std::size_t index = 1; index < slow_ollama_delta_times.size(); ++index) {
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
        slow_ollama_delta_times[index] - slow_ollama_delta_times[index - 1]);
    SMOKE_CHECK(gap.count() >= 25);
  }
		
		  bool missing_gemini_key_called = false;
		  agent::GeminiChatModelAdapter missing_gemini_key_model("gemini-missing-key",
	      [&](const agent::NativeProviderRequest&) {
	        missing_gemini_key_called = true;
	        return agent::Value::object({});
	      });
	  bool missing_gemini_key_failed = false;
	  try {
	    (void)missing_gemini_key_model.generate(agent::GenerateParams{
	        .messages = {agent::create_message(agent::MessageRole::User, "missing key")},
	    });
	  } catch (const agent::ConfigurationError& error) {
	    missing_gemini_key_failed =
	        std::string(error.what()).find("Gemini adapter requires apiKey or GEMINI_API_KEY.") != std::string::npos;
	  }
	  SMOKE_CHECK(missing_gemini_key_failed);
	  SMOKE_CHECK(!missing_gemini_key_called);
	
	  bool gemini_called = false;
	  agent::GeminiChatModelAdapter gemini_model("gemini-test",
      [&](const agent::NativeProviderRequest& request) {
        gemini_called = true;
        SMOKE_CHECK(request.provider == "gemini");
        SMOKE_CHECK(request.endpoint == "/models/gemini-test:generateContent");
        SMOKE_CHECK(request.headers.at("x-goog-api-key") == "gemini-key");
        SMOKE_CHECK(!request.body.at("contents").as_array().empty());
        SMOKE_CHECK(request.body.at("generationConfig").at("temperature").as_number() == 0.4);
        return agent::Value::object({
            {"responseId", "gemini_1"},
            {"modelVersion", "gemini-test"},
            {"candidates", agent::Value::array({
                agent::Value::object({
                    {"finishReason", "STOP"},
                    {"content", agent::Value::object({
                        {"parts", agent::Value::array({
                            agent::Value::object({{"text", "gemini reasoning"}, {"thought", true}}),
                            agent::Value::object({{"text", "gemini ok"}}),
                            agent::Value::object({
                                {"functionCall", agent::Value::object({
                                    {"id", "gemini_tool_1"},
                                    {"name", "lookup"},
                                    {"args", agent::Value::object({{"query", "native"}})},
                                })},
                            }),
                        })},
                    })},
                }),
            })},
            {"usageMetadata", agent::Value::object({
                {"promptTokenCount", 7},
                {"candidatesTokenCount", 6},
                {"totalTokenCount", 13},
            })},
        });
      },
      "gemini-key");
  auto gemini_response = gemini_model.generate(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{.temperature = 0.4},
      .tools = {agent::ChatToolDescriptor{.name = "lookup", .description = "Lookup data"}},
  });
  SMOKE_CHECK(gemini_called);
  SMOKE_CHECK(gemini_response.text == "gemini ok");
  SMOKE_CHECK(gemini_response.reasoning && gemini_response.reasoning->text == "gemini reasoning");
  SMOKE_CHECK(gemini_response.tool_calls.size() == 1);
  SMOKE_CHECK(gemini_response.tool_calls.front().name == "lookup");

  bool gemini_stream_called = false;
  agent::GeminiChatModelAdapter gemini_stream_model(
      "gemini-stream",
      [](const agent::NativeProviderRequest&) -> agent::Value {
        SMOKE_CHECK(false);
        return agent::Value::object({});
      },
      "gemini-stream-key",
      "https://generativelanguage.googleapis.com/v1beta/",
      [&](const agent::NativeProviderRequest& request) {
        gemini_stream_called = true;
        SMOKE_CHECK(request.provider == "gemini");
        SMOKE_CHECK(request.endpoint == "/models/gemini-stream:streamGenerateContent?alt=sse");
        SMOKE_CHECK(request.base_url == "https://generativelanguage.googleapis.com/v1beta");
        SMOKE_CHECK(request.headers.at("x-goog-api-key") == "gemini-stream-key");
        SMOKE_CHECK(!request.body.at("contents").as_array().empty());
        SMOKE_CHECK(!request.body.at("generationConfig").at("thinkingConfig").at("includeThoughts").as_bool());
        SMOKE_CHECK(request.body.at("generationConfig").at("thinkingConfig").at("thinkingBudget").as_integer() == 24576);
        return std::vector<std::string>{
            "data: {\"responseId\":\"gemini_stream\",\"modelVersion\":\"gemini-stream\",\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini thought\",\"thought\":true}]}}]}\n\n",
            "data: {\"responseId\":\"gemini_stream\",\"modelVersion\":\"gemini-stream\",\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini \"},{\"text\":\"stream\"}]}}]}\n\n",
            "data: {\"responseId\":\"gemini_stream\",\"modelVersion\":\"gemini-stream\",\"candidates\":[{\"finishReason\":\"STOP\",\"content\":{\"parts\":[{\"functionCall\":{\"id\":\"gem_tool\",\"name\":\"lookup\",\"args\":{\"query\":\"native\"}}}]}}]}\n\n",
        };
      });
  auto gemini_stream_events = gemini_stream_model.stream(agent::GenerateParams{
      .messages = {agent::create_message(agent::MessageRole::User, "hello")},
      .settings = agent::ModelSettings{
          .reasoning = agent::ReasoningSettings{
              .enabled = true,
              .budget = std::string("high"),
              .include_thoughts = false,
          },
      },
      .tools = {agent::ChatToolDescriptor{.name = "lookup", .description = "Lookup data"}},
  });
  SMOKE_CHECK(gemini_stream_called);
  // Six events: ResponseStart, ReasoningDelta, TextDelta x2, ToolCallDelta
  // (one-shot from the functionCall part), Response.
  SMOKE_CHECK(gemini_stream_events.size() == 6);
  SMOKE_CHECK(gemini_stream_events[0].type == agent::ModelStreamEventType::ResponseStart);
  SMOKE_CHECK(gemini_stream_events[1].type == agent::ModelStreamEventType::ReasoningDelta);
  SMOKE_CHECK(gemini_stream_events[1].reasoning == "gemini thought");
  SMOKE_CHECK(gemini_stream_events[2].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(gemini_stream_events[2].text == "gemini ");
  SMOKE_CHECK(gemini_stream_events[3].type == agent::ModelStreamEventType::TextDelta);
  SMOKE_CHECK(gemini_stream_events[3].text == "gemini stream");
  SMOKE_CHECK(gemini_stream_events[4].type == agent::ModelStreamEventType::ToolCallDelta);
  SMOKE_CHECK(gemini_stream_events[4].tool_call_id == "gem_tool");
  SMOKE_CHECK(gemini_stream_events[4].tool_call_name == "lookup");
  SMOKE_CHECK(gemini_stream_events[4].tool_call_args_delta == gemini_stream_events[4].tool_call_args_accumulated);
  SMOKE_CHECK(gemini_stream_events[4].tool_call_args_accumulated.find("\"query\"") != std::string::npos);
  SMOKE_CHECK(gemini_stream_events[4].tool_call_args_accumulated.find("\"native\"") != std::string::npos);
  SMOKE_CHECK(gemini_stream_events[5].type == agent::ModelStreamEventType::Response);
  SMOKE_CHECK(gemini_stream_events[5].response.text == "gemini stream");
  SMOKE_CHECK(gemini_stream_events[5].response.reasoning);
  SMOKE_CHECK(gemini_stream_events[5].response.reasoning->text == "gemini thought");
  SMOKE_CHECK(gemini_stream_events[5].response.tool_calls.size() == 1);
  SMOKE_CHECK(gemini_stream_events[5].response.tool_calls.front().id == "gem_tool");
  SMOKE_CHECK(gemini_stream_events[5].response.tool_calls.front().arguments.at("query").as_string() == "native");
  SMOKE_CHECK(gemini_stream_events[5].response.finish_reason == "tool_calls");

  agent::InMemoryRealtimeBus realtime(agent::InMemoryRealtimeBusOptions{.replay_limit = 8});
  int realtime_deliveries = 0;
  realtime.publish("agent.events.model", agent::Value::object({{"phase", "before"}}));
  auto sub = realtime.subscribe("agent.events.*", [&](const agent::RealtimeMessage& message) {
    SMOKE_CHECK(message.topic.rfind("agent.events.", 0) == 0);
    realtime_deliveries += 1;
  }, 1);
  realtime.publish("agent.events.tool", agent::Value::object({{"phase", "after"}}));
  realtime.unsubscribe(sub);
  SMOKE_CHECK(realtime_deliveries == 2);

  agent::InMemoryRealtimeBus replay_options_realtime(agent::InMemoryRealtimeBusOptions{.replay_limit = 2});
  replay_options_realtime.publish("runs.one", 1);
  replay_options_realtime.publish("runs.two", 2);
  replay_options_realtime.publish("runs.three", 3);
  std::vector<int> replay_true_payloads;
  auto replay_true_sub = replay_options_realtime.subscribe(
      "runs.*",
      [&](const agent::RealtimeMessage& message) {
        replay_true_payloads.push_back(static_cast<int>(message.payload.as_integer()));
      },
      agent::RealtimeSubscribeOptions{.replay = true});
  SMOKE_CHECK((replay_true_payloads == std::vector<int>{2, 3}));
  replay_options_realtime.unsubscribe(replay_true_sub);
  std::vector<int> replay_count_payloads;
  auto replay_count_sub = replay_options_realtime.subscribe(
      "runs.*",
      [&](const agent::RealtimeMessage& message) {
        replay_count_payloads.push_back(static_cast<int>(message.payload.as_integer()));
      },
      agent::RealtimeSubscribeOptions{.replay_count = 1});
  SMOKE_CHECK((replay_count_payloads == std::vector<int>{3}));
  replay_options_realtime.unsubscribe(replay_count_sub);
  agent::InMemoryRealtimeBus no_replay_realtime(agent::InMemoryRealtimeBusOptions{.replay_limit = 0});
  no_replay_realtime.publish("none.one", 1);
  SMOKE_CHECK(no_replay_realtime.replay("none.*").empty());

  agent::InMemoryRealtimeBus concurrent_realtime(64);
  std::atomic<int> concurrent_realtime_deliveries{0};
  auto concurrent_realtime_sub = concurrent_realtime.subscribe(
      "concurrent.*",
      [&](const agent::RealtimeMessage& message) {
        SMOKE_CHECK(message.topic.rfind("concurrent.", 0) == 0);
        concurrent_realtime_deliveries += 1;
        SMOKE_CHECK(concurrent_realtime.replay("concurrent.*", 3).size() <= 3);
      });
  std::vector<std::thread> concurrent_realtime_threads;
  for (int thread_index = 0; thread_index < 8; ++thread_index) {
    concurrent_realtime_threads.emplace_back([&, thread_index]() {
      for (int message_index = 0; message_index < 20; ++message_index) {
        concurrent_realtime.publish(
            "concurrent." + std::to_string(thread_index),
            agent::Value::object({{"thread", thread_index}, {"message", message_index}}));
      }
    });
  }
  for (auto& thread : concurrent_realtime_threads) {
    thread.join();
  }
  SMOKE_CHECK(concurrent_realtime_deliveries == 160);
  auto concurrent_replay = concurrent_realtime.replay("concurrent.*");
  SMOKE_CHECK(concurrent_replay.size() == 64);
  std::atomic<int> concurrent_replay_deliveries{0};
  auto concurrent_replay_sub = concurrent_realtime.subscribe(
      "concurrent.*",
      [&](const agent::RealtimeMessage&) {
        concurrent_replay_deliveries += 1;
      },
      7);
  SMOKE_CHECK(concurrent_replay_deliveries == 7);
  concurrent_realtime.unsubscribe(concurrent_replay_sub.id);
  concurrent_realtime.unsubscribe(concurrent_realtime_sub.id);

  std::vector<std::string> configured_event_calls;
  agent::EventBus configured_event_bus(agent::EventBus::Options{
      .sinks = {
          [&](const agent::FrameworkEvent& event) {
            configured_event_calls.push_back(event.category);
            throw std::runtime_error("configured sink failed");
          },
          [&](const agent::FrameworkEvent& event) {
            configured_event_calls.push_back(event.category + ":second");
          },
      },
      .include_raw = true,
  });
  SMOKE_CHECK(configured_event_bus.include_raw());
  auto configured_event = configured_event_bus.publish(
      "run.started",
      agent::ExecutionTarget::Run,
      agent::Value::object({{"ok", true}}));
  SMOKE_CHECK(configured_event.category == "run.started");
  SMOKE_CHECK((configured_event_calls == std::vector<std::string>{"run.started", "run.started:second"}));

  agent::InMemoryRealtimeBus event_realtime(4);
  std::vector<agent::RealtimeMessage> event_realtime_messages;
  event_realtime.subscribe("agent.events.*", [&](const agent::RealtimeMessage& message) {
    event_realtime_messages.push_back(message);
  });
  agent::EventBus event_realtime_source;
  agent::EventBusRealtimeSink event_realtime_sink(
      event_realtime,
      agent::EventBusRealtimeSinkOptions{
          .topic_mapper = [](const agent::FrameworkEvent& event) {
            return "agent.events." + event.category;
          },
          .metadata_mapper = [](const agent::FrameworkEvent& event) {
            return agent::Value::object({{"category", event.category},
                                        {"target", agent::to_string(event.target)}});
          },
      });
  const auto event_realtime_sink_id = event_realtime_sink.attach(event_realtime_source);
  event_realtime_source.publish("tool.completed",
                                agent::ExecutionTarget::Tool,
                                agent::Value::object({{"toolName", "remote.echo"}}),
                                agent::TraceContext{
                                    .trace_id = "trace-realtime",
                                    .span_id = "span-realtime",
                                    .span_name = "tool.exec",
                                });
  SMOKE_CHECK(event_realtime_messages.size() == 1);
  SMOKE_CHECK(event_realtime_messages.front().topic == "agent.events.tool.completed");
  SMOKE_CHECK(event_realtime_messages.front().payload.at("category").as_string() == "tool.completed");
  SMOKE_CHECK(event_realtime_messages.front().payload.at("traceId").as_string() == "trace-realtime");
  SMOKE_CHECK(event_realtime_messages.front().payload.at("spanName").as_string() == "tool.exec");
  SMOKE_CHECK(event_realtime_messages.front().metadata.at("category").as_string() == "tool.completed");
  event_realtime_source.unregister_sink(event_realtime_sink_id);
  auto event_without_trace = event_realtime_source.publish("tool.completed", agent::ExecutionTarget::Tool);
  SMOKE_CHECK(event_without_trace.trace.trace_id.empty());
  SMOKE_CHECK(event_without_trace.trace.span_id.empty());
  SMOKE_CHECK(event_realtime_messages.size() == 1);

  agent::InMemoryRealtimeBus pipe_realtime(4);
  std::vector<agent::RealtimeMessage> pipe_received;
  pipe_realtime.subscribe("pipe.*", [&](const agent::RealtimeMessage& message) {
    pipe_received.push_back(message);
  });
  auto pipe_messages = agent::pipe_values_to_realtime(
      std::vector<agent::Value>{agent::Value::object({{"text", "first"}}),
                                agent::Value::object({{"text", "second"}})},
      pipe_realtime,
      "pipe.default",
      [](const agent::Value& item, std::size_t index) {
        return agent::RealtimePipeEnvelope{
            .topic = index == 0 ? "pipe.first" : "pipe.second",
            .payload = agent::Value::object({{"index", static_cast<int>(index)},
                                            {"item", item}}),
            .metadata = agent::Value::object({{"mapped", true}}),
        };
      });
  SMOKE_CHECK(pipe_messages.size() == 2);
  SMOKE_CHECK(pipe_received.size() == 2);
  SMOKE_CHECK(pipe_received.front().topic == "pipe.first");
  SMOKE_CHECK(pipe_received.back().payload.at("item").at("text").as_string() == "second");
  SMOKE_CHECK(pipe_received.back().metadata.at("mapped").as_bool());

  auto redis = std::make_shared<FakeRedisRealtimeClient>();
  agent::RedisRealtimeBus redis_realtime(agent::RedisRealtimeBusOptions{
      .publisher = redis,
      .subscriber = redis,
      .channel_prefix = "test:",
      .replay_limit = 4,
  });
  std::vector<agent::Value> redis_received;
  redis_realtime.subscribe("runs.*", [&](const agent::RealtimeMessage& message) {
    redis_received.push_back(message.payload);
  });
  agent::RealtimePublishOptions redis_publish_options;
  redis_publish_options.id = "message-1";
  auto redis_message = redis_realtime.publish(
      "runs.created",
      agent::Value::object({{"runId", "run-1"}}),
      std::move(redis_publish_options));
  SMOKE_CHECK(redis_received.size() == 1);
  SMOKE_CHECK(redis_received.front().at("runId").as_string() == "run-1");
  SMOKE_CHECK(redis_message.id == "message-1");
  SMOKE_CHECK(redis->published.size() == 1);
  SMOKE_CHECK(redis->published.front().first == "test:runs.created");
  SMOKE_CHECK(redis->patterns.size() == 1);
  SMOKE_CHECK(redis->patterns.front() == "test:runs.*");
  std::vector<agent::Value> redis_replayed;
  redis_realtime.subscribe("runs.*", [&](const agent::RealtimeMessage& message) {
    redis_replayed.push_back(message.payload);
  }, 4);
  SMOKE_CHECK(redis_replayed.size() == 1);
  SMOKE_CHECK(redis_replayed.front().at("runId").as_string() == "run-1");

  auto redis_replay_client = std::make_shared<FakeRedisRealtimeClient>();
  agent::RedisRealtimeBus redis_replay_options(agent::RedisRealtimeBusOptions{
      .publisher = redis_replay_client,
      .subscriber = redis_replay_client,
      .channel_prefix = "replay:",
      .replay_limit = 2,
  });
  redis_replay_options.publish("runs.one", 1);
  redis_replay_options.publish("runs.two", 2);
  redis_replay_options.publish("runs.three", 3);
  std::vector<int> redis_replay_true_payloads;
  auto redis_replay_true_sub = redis_replay_options.subscribe(
      "runs.*",
      [&](const agent::RealtimeMessage& message) {
        redis_replay_true_payloads.push_back(static_cast<int>(message.payload.as_integer()));
      },
      agent::RealtimeSubscribeOptions{.replay = true});
  SMOKE_CHECK((redis_replay_true_payloads == std::vector<int>{2, 3}));
  redis_replay_options.unsubscribe(redis_replay_true_sub);
  std::vector<int> redis_replay_count_payloads;
  auto redis_replay_count_sub = redis_replay_options.subscribe(
      "runs.*",
      [&](const agent::RealtimeMessage& message) {
        redis_replay_count_payloads.push_back(static_cast<int>(message.payload.as_integer()));
      },
      agent::RealtimeSubscribeOptions{.replay_count = 1});
  SMOKE_CHECK((redis_replay_count_payloads == std::vector<int>{3}));
  redis_replay_options.unsubscribe(redis_replay_count_sub);

  auto redis_remote = std::make_shared<FakeRedisRealtimeClient>();
  agent::RedisRealtimeBus redis_remote_realtime(agent::RedisRealtimeBusOptions{
      .publisher = redis_remote,
      .subscriber = redis_remote,
      .channel_prefix = "test:",
      .replay_limit = 4,
  });
  std::vector<std::string> redis_topics;
  redis_remote_realtime.subscribe("runs.*", [&](const agent::RealtimeMessage& message) {
    redis_topics.push_back(message.topic);
  });
  auto redis_local = redis_remote_realtime.publish("runs.local", agent::Value::object({}));
  redis_remote->emit(
      "test:runs.*",
      "test:runs.local",
      agent::Value::object({{"id", redis_local.id},
                            {"topic", redis_local.topic},
                            {"timestamp", redis_local.timestamp},
                            {"payload", redis_local.payload},
                            {"metadata", redis_local.metadata}})
          .stringify(0));
  redis_remote->emit(
      "test:runs.*",
      "test:runs.remote",
      agent::Value::object({{"id", "remote-1"},
                            {"topic", "runs.remote"},
                            {"timestamp", "2026-05-21T00:00:00.000Z"},
                            {"payload", agent::Value::object({})}})
          .stringify(0));
  SMOKE_CHECK((redis_topics == std::vector<std::string>{"runs.local", "runs.remote"}));

  auto memory_observability = agent::create_memory_observability_pipeline();
  agent::EventBus observed_bus;
  memory_observability.pipeline->attach(observed_bus);
  observed_bus.publish("tool.completed", agent::ExecutionTarget::Tool,
                       agent::Value::object({{"toolName", "time.now"}}),
                       agent::TraceContext{
                           .trace_id = "trace-1",
                           .span_id = "span-1",
                           .span_name = "tool.exec",
                       });
  SMOKE_CHECK(memory_observability.collector->logs.size() == 1);
  SMOKE_CHECK(memory_observability.collector->metrics.size() == 1);
  SMOKE_CHECK(memory_observability.collector->traces.size() == 1);
  SMOKE_CHECK(memory_observability.collector->logs.front().attributes.at("traceId").as_string() == "trace-1");
  SMOKE_CHECK(memory_observability.collector->traces.front().trace_context->span_name == "tool.exec");
  auto untraced_observability = agent::create_memory_observability_pipeline();
  agent::EventBus untraced_bus;
  untraced_observability.pipeline->attach(untraced_bus);
  auto untraced_event = untraced_bus.publish("run.started", agent::ExecutionTarget::Run);
  SMOKE_CHECK(untraced_event.trace.trace_id.empty());
  SMOKE_CHECK(untraced_observability.collector->logs.front().attributes.at("traceId").is_null());
  SMOKE_CHECK(!untraced_observability.collector->traces.front().trace_context);

  auto root_trace = agent::create_trace_context(agent::TraceContext{
      .trace_id = "trace-root",
      .span_id = "span-root",
      .run_id = "run-1",
  });
  auto child_trace = agent::derive_child_trace_context(root_trace, "tool.execute");
  auto nested_trace = agent::create_child_trace_context(child_trace, agent::TraceContext{
      .span_name = "tool.execute.retry",
  });
  SMOKE_CHECK(child_trace.trace_id == "trace-root");
  SMOKE_CHECK(child_trace.parent_span_id == "span-root");
  SMOKE_CHECK(child_trace.span_name == "tool.execute");
  SMOKE_CHECK(nested_trace.parent_span_id == child_trace.span_id);
  SMOKE_CHECK(nested_trace.run_id == "run-1");

  auto normalized_trace = agent::normalize_framework_event_trace(
      agent::TraceContext{},
      agent::TraceContext{
          .trace_id = "trace-normalized",
          .span_id = "span-normalized",
          .span_name = "run",
      });
  SMOKE_CHECK(normalized_trace.trace_context);
  SMOKE_CHECK(normalized_trace.trace_id == "trace-normalized");
  SMOKE_CHECK(normalized_trace.span_id == "span-normalized");
  SMOKE_CHECK(normalized_trace.span_name == "run");

  agent::TracePropagationPolicy no_span_policy;
  no_span_policy.require_span_id = false;
  auto trace_without_span = agent::normalize_framework_event_trace(
      agent::TraceContext{.trace_id = "trace-only"},
      std::nullopt,
      no_span_policy);
  SMOKE_CHECK(trace_without_span.trace_context);
  SMOKE_CHECK(trace_without_span.span_id.empty());

  auto nested_trace_value = agent::Value::object({
      {"traceContext", agent::Value::object({
          {"traceId", "trace-nested"},
          {"spanName", "nested.span"},
      })},
  });
  auto extracted_trace = agent::get_trace_context(nested_trace_value);
  SMOKE_CHECK(extracted_trace);
  SMOKE_CHECK(extracted_trace->trace_id == "trace-nested");
  SMOKE_CHECK(!extracted_trace->span_id.empty());
  SMOKE_CHECK(extracted_trace->span_name == "nested.span");
  auto invalid_nested_trace = agent::get_trace_context(agent::Value::object({
      {"traceContext", "invalid"},
      {"traceId", "trace-outer"},
  }));
  SMOKE_CHECK(!invalid_nested_trace);
  auto null_nested_trace = agent::get_trace_context(agent::Value::object({
      {"traceContext", agent::Value()},
      {"traceId", "trace-outer"},
  }));
  SMOKE_CHECK(null_nested_trace);
  SMOKE_CHECK(null_nested_trace->trace_id == "trace-outer");

  auto span_only_record = agent::create_trace_record(agent::FrameworkEvent{
      .event_id = "event-span-only",
      .timestamp = agent::now_iso8601(),
      .category = "workflow.node.completed",
      .trace = agent::TraceContext{.span_name = "workflow.node"},
      .payload = agent::Value::object({}),
  });
  SMOKE_CHECK(span_only_record.name == "workflow.node");
  SMOKE_CHECK(!span_only_record.trace_context);

  auto concurrent_observability = agent::create_memory_observability_pipeline();
  agent::EventBus concurrent_observed_bus;
  concurrent_observability.pipeline->attach(concurrent_observed_bus);
  std::vector<std::thread> observability_threads;
  for (int thread_index = 0; thread_index < 8; ++thread_index) {
    observability_threads.emplace_back([&, thread_index]() {
      for (int event_index = 0; event_index < 20; ++event_index) {
        concurrent_observed_bus.publish(
            "run.completed",
            agent::ExecutionTarget::Run,
            agent::Value::object({{"thread", thread_index}, {"event", event_index}}),
            agent::TraceContext{
                .trace_id = "trace-concurrent-" + std::to_string(thread_index),
                .span_id = "span-concurrent-" + std::to_string(event_index),
            });
      }
    });
  }
  for (auto& thread : observability_threads) {
    thread.join();
  }
  auto concurrent_logs = concurrent_observability.collector->log_records();
  auto concurrent_metrics = concurrent_observability.collector->metric_records();
  auto concurrent_traces = concurrent_observability.collector->trace_records();
  SMOKE_CHECK(concurrent_logs.size() == 160);
  SMOKE_CHECK(concurrent_metrics.size() == 160);
  SMOKE_CHECK(concurrent_traces.size() == 160);
  concurrent_observability.collector->reset();
  SMOKE_CHECK(concurrent_observability.collector->log_records().empty());

  int adapter_errors = 0;
  std::vector<std::string> observability_calls;
  agent::ObservabilityPipeline pipeline([&](const agent::ObservabilityAdapterError& error) {
    adapter_errors += 1;
    SMOKE_CHECK(error.adapter_type == "log");
  });
  pipeline.register_logger([](const agent::StructuredLogRecord&) {
    throw std::runtime_error("boom");
  });
  pipeline.register_metrics([&](const agent::MetricRecord& record) {
    SMOKE_CHECK(record.name == "node_agent_events_total");
    observability_calls.push_back("metrics");
  });
  pipeline.register_trace([&](const agent::TraceRecord& record) {
    SMOKE_CHECK(record.status == agent::TraceStatus::Ok);
    observability_calls.push_back("trace");
  });
  pipeline.publish(agent::FrameworkEvent{
      .event_id = "event-observe",
      .timestamp = agent::now_iso8601(),
      .category = "run.completed",
      .payload = agent::Value::object({}),
  });
  SMOKE_CHECK(adapter_errors == 1);
  SMOKE_CHECK((observability_calls == std::vector<std::string>{"metrics", "trace"}));
  int configured_adapter_errors = 0;
  std::vector<std::string> configured_observability_calls;
  agent::ObservabilityPipeline configured_pipeline(agent::ObservabilityPipelineConfig{
      .loggers = {[](const agent::StructuredLogRecord& record) {
        SMOKE_CHECK(record.level == agent::StructuredLogLevel::Warn);
        throw std::runtime_error("configured boom");
      }},
      .metrics = {[&](const agent::MetricRecord& record) {
        SMOKE_CHECK(record.name == "node_agent_events_total");
        configured_observability_calls.push_back("metrics");
      }},
      .traces = {[&](const agent::TraceRecord& record) {
        SMOKE_CHECK(record.status == agent::TraceStatus::Ok);
        configured_observability_calls.push_back("trace");
      }},
      .on_adapter_error = [&](const agent::ObservabilityAdapterError& error) {
        configured_adapter_errors += 1;
        SMOKE_CHECK(error.adapter_type == "log");
        SMOKE_CHECK(error.event.category == "retry.scheduled");
      },
  });
  configured_pipeline.publish(agent::FrameworkEvent{
      .event_id = "event-configured-observe",
      .timestamp = agent::now_iso8601(),
      .category = "retry.scheduled",
      .payload = agent::Value::object({}),
  });
  SMOKE_CHECK(configured_adapter_errors == 1);
  SMOKE_CHECK((configured_observability_calls == std::vector<std::string>{"metrics", "trace"}));

  auto failed_event = agent::FrameworkEvent{
      .event_id = "event-failed",
      .timestamp = agent::now_iso8601(),
      .category = "workflow.node.failed",
      .target = agent::ExecutionTarget::WorkflowNode,
      .trace = agent::TraceContext{
          .trace_id = "trace-otel",
          .span_id = "span-otel",
          .parent_span_id = "span-parent",
          .span_name = "workflow.node",
      },
      .payload = agent::Value::object({}),
  };
  auto log_record = agent::create_structured_log_record(failed_event);
  SMOKE_CHECK(log_record.level == agent::StructuredLogLevel::Error);
  auto trace_record = agent::create_trace_record(failed_event);
  SMOKE_CHECK(trace_record.status == agent::TraceStatus::Error);
  SMOKE_CHECK(trace_record.trace_context->parent_span_id == "span-parent");
  auto configured_memory_collector = std::make_shared<agent::MemoryObservabilityCollector>();
  auto configured_memory_observability = agent::create_memory_observability_pipeline(agent::MemoryAdapterConfig{
      .collector = configured_memory_collector,
  });
  configured_memory_observability.pipeline->publish(failed_event);
  SMOKE_CHECK(configured_memory_observability.collector == configured_memory_collector);
  SMOKE_CHECK(configured_memory_collector->logs.size() == 1);
  SMOKE_CHECK(configured_memory_collector->metrics.size() == 1);
  SMOKE_CHECK(configured_memory_collector->traces.size() == 1);

  std::ostringstream info_output;
  std::ostringstream warn_output;
  std::ostringstream error_output;
  auto console_pipeline = agent::create_console_observability_pipeline(info_output, warn_output, error_output);
  console_pipeline->publish(failed_event);
  SMOKE_CHECK(error_output.str().find("workflow.node.failed") != std::string::npos);
  SMOKE_CHECK(info_output.str().find("node_agent_events_total") != std::string::npos);

  RecordingTelemetryBridge telemetry_bridge;
  auto telemetry_pipeline = agent::create_opentelemetry_observability_pipeline(agent::OpenTelemetryAdapterConfig{
      .bridge = &telemetry_bridge,
  });
  telemetry_pipeline->publish(failed_event);
  SMOKE_CHECK((telemetry_bridge.calls == std::vector<std::string>{
                                       "log:workflow.node.failed",
                                       "counter:node_agent_events_total",
                                       "span:workflow.node:span-parent",
                                       "end:error",
                                   }));
  bool missing_telemetry_bridge_failed = false;
  try {
    (void)agent::create_opentelemetry_observability_pipeline(agent::OpenTelemetryAdapterConfig{});
  } catch (const agent::ConfigurationError& error) {
    missing_telemetry_bridge_failed =
        std::string(error.what()).find("requires a bridge") != std::string::npos;
  }
  SMOKE_CHECK(missing_telemetry_bridge_failed);

  agent::InMemoryTaskStore task_store;
  auto task = task_store.create_task(agent::CreateTaskInput{
      .type = "uppercase",
      .input = agent::Value::object({{"text", "task value"}}),
      .idempotency_key = "task-1",
  });
  agent::InMemoryTaskQueue task_queue(agent::InMemoryTaskQueueOptions{.store = &task_store});
  task_queue.enqueue(task.id);
  agent::AgentTaskWorker worker(agent::AgentTaskWorkerOptions{
      .store = &task_store,
      .queue = &task_queue,
      .handler = [](const agent::AgentTask& task, agent::TaskHandlerContext& context) {
        context.store->append_checkpoint(task.id, context.run.id, "input", task.input);
        return agent::Value::object({{"ok", true}, {"text", task.input.at("text").as_string()}});
      },
  });
  SMOKE_CHECK(worker.run_once());
  auto task_snapshot = task_store.get_task(task.id);
  SMOKE_CHECK(task_snapshot);
  SMOKE_CHECK(task_snapshot->task.status == agent::TaskStatus::Completed);
  SMOKE_CHECK(!task_snapshot->events.empty());
  SMOKE_CHECK(!task_snapshot->checkpoints.empty());

  agent::InMemoryTaskStore active_cancel_store;
  auto active_cancel_task = active_cancel_store.create_task(agent::CreateTaskInput{
      .type = "cancel-active",
      .input = agent::Value::object({{"text", "cancel me"}}),
      .idempotency_key = "task-active-cancel",
  });
  agent::InMemoryTaskQueue active_cancel_queue(agent::InMemoryTaskQueueOptions{
      .store = &active_cancel_store,
  });
  active_cancel_queue.enqueue(active_cancel_task.id);
  std::atomic<bool> active_cancel_handler_started{false};
  std::atomic<bool> active_cancel_handler_saw_cancel{false};
  agent::AgentTaskWorker active_cancel_worker(agent::AgentTaskWorkerOptions{
      .store = &active_cancel_store,
      .queue = &active_cancel_queue,
      .handler = [&](const agent::AgentTask&, agent::TaskHandlerContext& context) {
        SMOKE_CHECK(context.cancellation);
        active_cancel_handler_started = true;
        while (!context.cancellation->cancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        active_cancel_handler_saw_cancel = true;
        context.cancellation->throw_if_cancelled(agent::ExecutionTarget::Run);
        return agent::Value::object({{"ok", true}});
      },
  });
  std::thread active_cancel_thread([&]() { SMOKE_CHECK(active_cancel_worker.run_once()); });
  for (int i = 0; i < 100 && !active_cancel_handler_started.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SMOKE_CHECK(active_cancel_handler_started.load());
  SMOKE_CHECK(active_cancel_worker.cancel(active_cancel_task.id));
  active_cancel_thread.join();
  SMOKE_CHECK(active_cancel_handler_saw_cancel.load());
  auto active_cancel_snapshot = active_cancel_store.get_task(active_cancel_task.id);
  SMOKE_CHECK(active_cancel_snapshot);
  SMOKE_CHECK(active_cancel_snapshot->task.status == agent::TaskStatus::Cancelled);
  SMOKE_CHECK(active_cancel_snapshot->runs.size() == 1);
  SMOKE_CHECK(active_cancel_snapshot->runs.front().status == agent::TaskStatus::Cancelled);
  std::vector<std::string> active_cancel_event_types;
  for (const auto& event : active_cancel_snapshot->events) {
    active_cancel_event_types.push_back(event.type);
  }
  SMOKE_CHECK(contains_string(active_cancel_event_types, "task.started"));
  SMOKE_CHECK(contains_string(active_cancel_event_types, "task.cancelled"));
  SMOKE_CHECK(!contains_string(active_cancel_event_types, "task.completed"));
  SMOKE_CHECK(!contains_string(active_cancel_event_types, "task.failed"));

  auto bullmq_queue_client = std::make_shared<FakeBullMQQueueClient>();
  agent::InMemoryTaskStore bullmq_store;
  agent::BullMQTaskQueue bullmq_queue(agent::BullMQTaskQueueOptions{
      .store = &bullmq_store,
      .queue = bullmq_queue_client,
  });
  auto bullmq_task = bullmq_store.create_task(agent::CreateTaskInput{
      .type = "bullmq-unit",
      .input = agent::Value::object({{"value", "run"}}),
  });
  bullmq_queue.enqueue(bullmq_task.id);
  SMOKE_CHECK(bullmq_queue_client->added_jobs.size() == 1);
  SMOKE_CHECK(bullmq_queue_client->added_jobs.front().name == "agent-task");
  SMOKE_CHECK(bullmq_queue_client->added_jobs.front().task_id == bullmq_task.id);
  SMOKE_CHECK(bullmq_queue_client->added_jobs.front().options.at("jobId").as_string() == bullmq_task.id);
  auto bullmq_queued_snapshot = bullmq_store.get_task(bullmq_task.id);
  SMOKE_CHECK(bullmq_queued_snapshot);
  SMOKE_CHECK(bullmq_queued_snapshot->task.status == agent::TaskStatus::Queued);
  bool bullmq_claim_threw = false;
  try {
    (void)bullmq_queue.claim();
  } catch (const agent::AgentFrameworkError& error) {
    bullmq_claim_threw = std::string(error.what()).find("push-driven") != std::string::npos;
  }
  SMOKE_CHECK(bullmq_claim_threw);

  agent::BullMQTaskWorker bullmq_worker(agent::BullMQTaskWorkerOptions{
      .store = &bullmq_store,
      .task_queue = &bullmq_queue,
      .handler = [](const agent::AgentTask& task, agent::TaskHandlerContext& context) {
        context.event("custom.event", agent::Value::object({{"taskId", task.id}}));
        context.checkpoint("custom.checkpoint", agent::Value::object({{"input", task.input}}));
        return agent::Value::object({{"value", "ok"}});
      },
  });
  auto bullmq_output = bullmq_worker.process(agent::BullMQJobLike{
      .data = agent::Value::object({{"taskId", bullmq_task.id}}),
  });
  SMOKE_CHECK(bullmq_output.at("value").as_string() == "ok");
  auto bullmq_completed = bullmq_store.get_task(bullmq_task.id);
  SMOKE_CHECK(bullmq_completed);
  SMOKE_CHECK(bullmq_completed->task.status == agent::TaskStatus::Completed);
  SMOKE_CHECK(bullmq_completed->task.output.at("value").as_string() == "ok");
  std::vector<std::string> bullmq_event_types;
  for (const auto& event : bullmq_completed->events) {
    bullmq_event_types.push_back(event.type);
  }
  SMOKE_CHECK((bullmq_event_types == std::vector<std::string>{"task.started", "custom.event", "task.completed"}));
  std::vector<std::string> bullmq_checkpoint_names;
  for (const auto& checkpoint : bullmq_completed->checkpoints) {
    bullmq_checkpoint_names.push_back(checkpoint.name);
  }
  SMOKE_CHECK((bullmq_checkpoint_names == std::vector<std::string>{"custom.checkpoint", "output"}));

  auto bullmq_failed_task = bullmq_store.create_task(agent::CreateTaskInput{
      .type = "bullmq-unit",
      .input = agent::Value("fail"),
  });
  agent::BullMQTaskWorker bullmq_fail_worker(agent::BullMQTaskWorkerOptions{
      .store = &bullmq_store,
      .task_queue = &bullmq_queue,
      .handler = [](const agent::AgentTask&, agent::TaskHandlerContext&) -> agent::Value {
        throw std::runtime_error("boom");
      },
  });
  auto bullmq_failed_output = bullmq_fail_worker.process(agent::BullMQJobLike{
      .data = agent::Value::object({{"taskId", bullmq_failed_task.id}}),
  });
  SMOKE_CHECK(bullmq_failed_output.at("failed").as_bool());
  SMOKE_CHECK(bullmq_failed_output.at("error").as_string() == "boom");
  auto bullmq_failed_snapshot = bullmq_store.get_task(bullmq_failed_task.id);
  SMOKE_CHECK(bullmq_failed_snapshot);
  SMOKE_CHECK(bullmq_failed_snapshot->task.status == agent::TaskStatus::Failed);
  SMOKE_CHECK(bullmq_failed_snapshot->task.error == "boom");
  SMOKE_CHECK(bullmq_failed_snapshot->runs.front().status == agent::TaskStatus::Failed);
  SMOKE_CHECK(bullmq_failed_snapshot->events.back().type == "task.failed");

  auto bullmq_cancel_task = bullmq_store.create_task(agent::CreateTaskInput{
      .type = "bullmq-unit",
      .input = agent::Value("cancel"),
  });
  bullmq_queue.enqueue(bullmq_cancel_task.id);
  bullmq_queue.cancel(bullmq_cancel_task.id);
  auto bullmq_cancelled = bullmq_store.get_task(bullmq_cancel_task.id);
  SMOKE_CHECK(bullmq_cancelled);
  SMOKE_CHECK(bullmq_cancelled->task.status == agent::TaskStatus::Cancelled);
  SMOKE_CHECK(contains_string(bullmq_queue_client->removed_job_ids, bullmq_cancel_task.id));

  {
    auto concurrent_bullmq_client = std::make_shared<FakeBullMQQueueClient>();
    agent::InMemoryTaskStore concurrent_bullmq_store;
    agent::BullMQTaskQueue concurrent_bullmq_queue(agent::BullMQTaskQueueOptions{
        .store = &concurrent_bullmq_store,
        .queue = concurrent_bullmq_client,
        .close_queue_on_close = true,
    });
    constexpr int concurrent_bullmq_thread_count = 8;
    std::atomic<int> concurrent_bullmq_ready{0};
    std::atomic<bool> concurrent_bullmq_start{false};
    std::vector<std::thread> concurrent_bullmq_workers;
    for (int thread_index = 0; thread_index < concurrent_bullmq_thread_count; ++thread_index) {
      concurrent_bullmq_workers.emplace_back([&, thread_index]() {
        concurrent_bullmq_ready.fetch_add(1);
        while (!concurrent_bullmq_start.load()) {
          std::this_thread::yield();
        }
        const auto task_id = "bullmq-concurrent-" + std::to_string(thread_index);
        concurrent_bullmq_store.create_task(agent::CreateTaskInput{
            .id = task_id,
            .type = "bullmq-concurrent",
            .input = agent::Value::object({{"thread", thread_index}}),
        });
        concurrent_bullmq_queue.enqueue(task_id);
        if (thread_index % 2 == 0) {
          concurrent_bullmq_queue.cancel(task_id);
        }
        if (thread_index % 3 == 0) {
          concurrent_bullmq_queue.close();
        }
      });
    }
    while (concurrent_bullmq_ready.load() < concurrent_bullmq_thread_count) {
      std::this_thread::yield();
    }
    concurrent_bullmq_start.store(true);
    for (auto& worker : concurrent_bullmq_workers) {
      worker.join();
    }
    concurrent_bullmq_queue.close();
    SMOKE_CHECK(concurrent_bullmq_client->added_jobs.size() == static_cast<std::size_t>(concurrent_bullmq_thread_count));
    SMOKE_CHECK(concurrent_bullmq_client->removed_job_ids.size() ==
           static_cast<std::size_t>(concurrent_bullmq_thread_count / 2));
    SMOKE_CHECK(concurrent_bullmq_client->closed);
    SMOKE_CHECK(concurrent_bullmq_client->close_count == 1);
  }

  auto pg_task_client = std::make_shared<FakePgTaskClient>();
  agent::PgTaskStore pg_task_store(agent::PgTaskStoreConfig{.client = pg_task_client});
  auto pg_task = pg_task_store.create_task(agent::CreateTaskInput{
      .id = "task-1",
      .type = "agent.run",
      .input = agent::Value::object({{"prompt", "hello"}}),
      .idempotency_key = "idem-1",
      .owner_api_key_id = "key-a",
      .tenant_id = "tenant-a",
      .metadata = agent::Value::object({{"source", "test"}}),
  });
  SMOKE_CHECK(pg_task.id == "task-1");

  // ===== Tool call argument delta streaming =====
  // Test A: OpenAI streaming emits ToolCallDelta events with progressive args.
  {
    const std::vector<std::string> chunks{
        "data: {\"id\":\"sa\",\"model\":\"gpt-tcd\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_X\",\"function\":{\"name\":\"fs.writeText\",\"arguments\":\"\"}}]}}]}\n\n",
        "data: {\"id\":\"sa\",\"model\":\"gpt-tcd\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"path\\\":\\\"x.txt\\\"\"}}]}}]}\n\n",
        "data: {\"id\":\"sa\",\"model\":\"gpt-tcd\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\",\\\"content\\\":\\\"hello\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n",
        "data: [DONE]\n\n",
    };
    const auto events = agent::parse_openai_chat_stream_events(chunks, "openai", "gpt-tcd");
    std::vector<std::size_t> delta_indices;
    for (std::size_t i = 0; i < events.size(); ++i) {
      if (events[i].type == agent::ModelStreamEventType::ToolCallDelta) {
        delta_indices.push_back(i);
      }
    }
    SMOKE_CHECK(delta_indices.size() >= 2);
    std::string last_accum;
    for (auto idx : delta_indices) {
      const auto& e = events[idx];
      SMOKE_CHECK(e.tool_call_id == "call_X");
      SMOKE_CHECK(e.tool_call_args_accumulated.size() >= last_accum.size());
      SMOKE_CHECK(e.tool_call_args_accumulated.compare(0, last_accum.size(), last_accum) == 0);
      last_accum = e.tool_call_args_accumulated;
    }
    SMOKE_CHECK(last_accum == "{\"path\":\"x.txt\",\"content\":\"hello\"}");
    const auto& final_event = events.back();
    SMOKE_CHECK(final_event.type == agent::ModelStreamEventType::Response);
    SMOKE_CHECK(final_event.response.tool_calls.size() == 1);
    SMOKE_CHECK(final_event.response.tool_calls.front().arguments.at("path").as_string() == "x.txt");
    SMOKE_CHECK(final_event.response.tool_calls.front().arguments.at("content").as_string() == "hello");
  }

  // Test B: Anthropic streaming emits ToolCallDelta events with name resolved
  // on content_block_start and progressive partial_json accumulated.
  {
    const std::vector<std::string> chunks{
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_tcd\",\"model\":\"claude-tcd\"}}\n\n",
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_Y\",\"name\":\"fs.writeText\",\"input\":{}}}\n\n",
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"y.txt\\\"\"}}\n\n",
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\",\\\"content\\\":\\\"world\\\"}\"}}\n\n",
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n",
        "data: {\"type\":\"message_stop\"}\n\n",
    };
    const auto events = agent::parse_anthropic_messages_stream_events(chunks, "anthropic", "claude-tcd");
    std::vector<std::size_t> delta_indices;
    for (std::size_t i = 0; i < events.size(); ++i) {
      if (events[i].type == agent::ModelStreamEventType::ToolCallDelta) {
        delta_indices.push_back(i);
      }
    }
    SMOKE_CHECK(delta_indices.size() >= 2);
    std::string last_accum;
    for (auto idx : delta_indices) {
      const auto& e = events[idx];
      SMOKE_CHECK(e.tool_call_id == "toolu_Y");
      SMOKE_CHECK(e.tool_call_name == "fs.writeText");
      SMOKE_CHECK(e.tool_call_args_accumulated.size() >= last_accum.size());
      SMOKE_CHECK(e.tool_call_args_accumulated.compare(0, last_accum.size(), last_accum) == 0);
      last_accum = e.tool_call_args_accumulated;
    }
    SMOKE_CHECK(last_accum == "{\"path\":\"y.txt\",\"content\":\"world\"}");
  }

  // Test C: runtime layer surfaces ToolCallArgumentDelta events before ToolStart
  // and publishes a model.tool_call_delta EventBus event with camelCase payload.
  {
    class StreamingToolCallModel final : public agent::ChatModelAdapter {
     public:
      StreamingToolCallModel()
          : ChatModelAdapter("test", "tcd-runtime", 0.0, 64, {"input.text"}) {}
      agent::ModelResponse generate(const agent::GenerateParams&) override {
        agent::ModelResponse response;
        response.text = "";
        response.finish_reason = "tool_calls";
        response.tool_calls.push_back(agent::ToolCall{
            .id = "call_R",
            .name = "noop",
            .arguments = agent::Value::object({{"k", std::string("v")}}),
        });
        return build_response(response);
      }
      void stream(const agent::GenerateParams& params,
                  StreamEventHandler on_event) override {
        if (called_) {
          // Second iteration: short circuit to a plain text response.
          on_event(agent::ModelStreamEvent{
              .type = agent::ModelStreamEventType::ResponseStart,
              .provider = "test",
              .model = "tcd-runtime",
          });
          agent::ModelResponse final_response;
          final_response.text = "done";
          final_response.finish_reason = "stop";
          on_event(agent::ModelStreamEvent{
              .type = agent::ModelStreamEventType::Response,
              .response = build_response(final_response),
          });
          return;
        }
        called_ = true;
        on_event(agent::ModelStreamEvent{
            .type = agent::ModelStreamEventType::ResponseStart,
            .provider = "test",
            .model = "tcd-runtime",
        });
        const std::vector<std::string> pieces{"{\"k\":", "\"v", "\"}"};
        std::string accum;
        for (const auto& piece : pieces) {
          accum += piece;
          on_event(agent::ModelStreamEvent{
              .type = agent::ModelStreamEventType::ToolCallDelta,
              .provider = "test",
              .model = "tcd-runtime",
              .tool_call_id = "call_R",
              .tool_call_name = "noop",
              .tool_call_args_delta = piece,
              .tool_call_args_accumulated = accum,
          });
        }
        agent::ModelResponse response;
        response.finish_reason = "tool_calls";
        response.tool_calls.push_back(agent::ToolCall{
            .id = "call_R",
            .name = "noop",
            .arguments = agent::Value::object({{"k", std::string("v")}}),
        });
        on_event(agent::ModelStreamEvent{
            .type = agent::ModelStreamEventType::Response,
            .response = build_response(response),
        });
        (void)params;
      }

     private:
      bool called_ = false;
    };

    agent::ToolDefinition noop_def;
    noop_def.name = "noop";
    noop_def.description = "no-op";
    noop_def.input_schema = agent::JsonSchema{.type = agent::JsonSchemaType::Object};
    noop_def.execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
      return agent::Value::object({{"ok", true}});
    };
    agent::ToolRegistry tcd_registry({noop_def});
    agent::ToolExecutor tcd_executor(tcd_registry);
    agent::EmbeddedContextManager tcd_context;
    agent::EventBus tcd_bus;
    std::vector<agent::FrameworkEvent> tcd_bus_events;
    tcd_bus.register_sink([&](const agent::FrameworkEvent& event) {
      if (event.category == "model.tool_call_delta") {
        tcd_bus_events.push_back(event);
      }
    });
    agent::AgentLoop tcd_loop(agent::AgentLoopConfig{
        .model = std::make_shared<StreamingToolCallModel>(),
        .tool_registry = &tcd_registry,
        .tool_executor = &tcd_executor,
        .context_manager = &tcd_context,
        .max_iterations = 2,
        .event_bus = &tcd_bus,
    });
    agent::SessionMemory tcd_session(agent::SessionMemoryOptions{.storage = {.session_id = "tcd"}});
    auto tcd_stream = tcd_loop.stream(tcd_session, std::string("go"));

    std::size_t first_delta_index = static_cast<std::size_t>(-1);
    std::size_t first_tool_start_index = static_cast<std::size_t>(-1);
    std::size_t delta_count = 0;
    std::string last_accum;
    for (std::size_t i = 0; i < tcd_stream.events.size(); ++i) {
      const auto& event = tcd_stream.events[i];
      if (event.type == agent::AgentLoopStreamEventType::ToolCallArgumentDelta) {
        if (first_delta_index == static_cast<std::size_t>(-1)) {
          first_delta_index = i;
        }
        ++delta_count;
        SMOKE_CHECK(event.tool_call_id == "call_R");
        SMOKE_CHECK(event.tool_call_name == "noop");
        SMOKE_CHECK(event.tool_call_args_accumulated.compare(0, last_accum.size(), last_accum) == 0);
        last_accum = event.tool_call_args_accumulated;
      }
      if (event.type == agent::AgentLoopStreamEventType::ToolStart &&
          event.tool_call.id == "call_R" &&
          first_tool_start_index == static_cast<std::size_t>(-1)) {
        first_tool_start_index = i;
      }
    }
    SMOKE_CHECK(delta_count == 3);
    SMOKE_CHECK(last_accum == "{\"k\":\"v\"}");
    SMOKE_CHECK(first_delta_index != static_cast<std::size_t>(-1));
    SMOKE_CHECK(first_tool_start_index != static_cast<std::size_t>(-1));
    SMOKE_CHECK(first_delta_index < first_tool_start_index);

    SMOKE_CHECK(tcd_bus_events.size() == 3);
    std::string bus_last_accum;
    for (std::size_t i = 0; i < tcd_bus_events.size(); ++i) {
      const auto& payload = tcd_bus_events[i].payload;
      SMOKE_CHECK(payload.at("provider").as_string() == "test");
      SMOKE_CHECK(payload.at("model").as_string() == "tcd-runtime");
      SMOKE_CHECK(payload.at("toolCallId").as_string() == "call_R");
      SMOKE_CHECK(payload.at("toolName").as_string() == "noop");
      SMOKE_CHECK(payload.at("argsDelta").as_string().size() > 0);
      const auto accum = payload.at("argsAccumulated").as_string();
      SMOKE_CHECK(accum.size() > bus_last_accum.size());
      bus_last_accum = accum;
      SMOKE_CHECK(payload.at("iteration").as_integer() == 0);
    }
    SMOKE_CHECK(bus_last_accum == "{\"k\":\"v\"}");
  }

  // Test D: Ollama streaming emits a one-shot ToolCallDelta event for each
  // tool call (Ollama serializes tool_calls as one complete block, not
  // character-by-character like OpenAI).
  {
    const std::vector<std::string> chunks{
        "{\"created_at\":\"2026-01-01T00:00:00Z\",\"model\":\"qwen-tcd\",\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"function\":{\"name\":\"fs.writeText\",\"arguments\":{\"path\":\"x.txt\",\"content\":\"hi\"}}}]},\"done\":false}\n",
        "{\"created_at\":\"2026-01-01T00:00:01Z\",\"model\":\"qwen-tcd\",\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\"}\n",
    };
    const auto events = agent::parse_ollama_chat_stream_events(chunks, "qwen-tcd");
    std::size_t delta_count = 0;
    const agent::ModelStreamEvent* delta_event = nullptr;
    for (const auto& e : events) {
      if (e.type == agent::ModelStreamEventType::ToolCallDelta) {
        ++delta_count;
        delta_event = &e;
      }
    }
    SMOKE_CHECK(delta_count == 1);
    SMOKE_CHECK(delta_event != nullptr);
    SMOKE_CHECK(delta_event->provider == "ollama");
    SMOKE_CHECK(delta_event->tool_call_name == "fs.writeText");
    SMOKE_CHECK(delta_event->tool_call_args_delta == delta_event->tool_call_args_accumulated);
    const auto accum_ollama = delta_event->tool_call_args_accumulated;
    SMOKE_CHECK(accum_ollama.find("\"path\"") != std::string::npos);
    SMOKE_CHECK(accum_ollama.find("\"x.txt\"") != std::string::npos);
    SMOKE_CHECK(accum_ollama.find("\"content\"") != std::string::npos);
    SMOKE_CHECK(accum_ollama.find("\"hi\"") != std::string::npos);
    SMOKE_CHECK(!delta_event->tool_call_id.empty());
    const auto& final_event = events.back();
    SMOKE_CHECK(final_event.type == agent::ModelStreamEventType::Response);
    SMOKE_CHECK(final_event.response.tool_calls.size() == 1);
    SMOKE_CHECK(final_event.response.tool_calls.front().id == delta_event->tool_call_id);
    SMOKE_CHECK(final_event.response.tool_calls.front().name == "fs.writeText");
  }

  // Test E: Gemini streaming emits a one-shot ToolCallDelta for each
  // functionCall part. Gemini parts arrive whole, not character-by-character.
  {
    const std::vector<std::string> chunks{
        "data: {\"responseId\":\"resp_g\",\"modelVersion\":\"gemini-tcd\",\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"fs.readText\",\"args\":{\"path\":\"doc.txt\"}}}]},\"finishReason\":\"STOP\"}]}\n\n",
    };
    const auto events = agent::parse_gemini_generate_content_stream_events(chunks, "gemini-tcd");
    std::size_t delta_count = 0;
    const agent::ModelStreamEvent* delta_event = nullptr;
    for (const auto& e : events) {
      if (e.type == agent::ModelStreamEventType::ToolCallDelta) {
        ++delta_count;
        delta_event = &e;
      }
    }
    SMOKE_CHECK(delta_count == 1);
    SMOKE_CHECK(delta_event != nullptr);
    SMOKE_CHECK(delta_event->provider == "gemini");
    SMOKE_CHECK(delta_event->tool_call_name == "fs.readText");
    SMOKE_CHECK(delta_event->tool_call_args_delta == delta_event->tool_call_args_accumulated);
    const auto accum_gemini = delta_event->tool_call_args_accumulated;
    SMOKE_CHECK(accum_gemini.find("\"path\"") != std::string::npos);
    SMOKE_CHECK(accum_gemini.find("\"doc.txt\"") != std::string::npos);
    SMOKE_CHECK(!delta_event->tool_call_id.empty());
    const auto& final_event = events.back();
    SMOKE_CHECK(final_event.type == agent::ModelStreamEventType::Response);
    SMOKE_CHECK(final_event.response.tool_calls.size() == 1);
    SMOKE_CHECK(final_event.response.tool_calls.front().id == delta_event->tool_call_id);
    SMOKE_CHECK(final_event.response.tool_calls.front().name == "fs.readText");
  }
