  bool configured_browser_called = false;
  auto configured_browser_renderer = std::make_shared<agent::BrowserCallbackRenderer>(
      [&](const agent::BrowserRenderRequest& request) {
        configured_browser_called = true;
        SMOKE_CHECK(request.url == "https://example.test/config-browser");
        SMOKE_CHECK(request.selector == "#content");
        SMOKE_CHECK(request.wait_until == "domcontentloaded");
        SMOKE_CHECK(request.timeout_ms == 1500);
        agent::BrowserRenderResult render_result;
        render_result.url = request.url;
        render_result.title = "Configured Browser";
        render_result.text = "configured browser renderer output";
        return render_result;
      },
      agent::BrowserRendererMetadata{.name = "configured-browser", .tier = "test"});
  agent::NativeBrowserAdapters browser_adapters;
  browser_adapters.renderer = configured_browser_renderer;
  auto config_with_browser = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"browser"})}})},
              {"browser", agent::Value::object({
                  {"renderer", agent::Value::object({{"kind", "playwright"}})},
              })},
          })},
      })},
  });
  auto browser_app = agent::resolve_native_agent_app(
      config_with_browser,
      native_resolve_options("main", {}, {}, {}, {}, browser_adapters));
  SMOKE_CHECK(browser_app.browser);
  SMOKE_CHECK(browser_app.browser->renderer == configured_browser_renderer);
  agent::ToolRegistry configured_browser_tools(browser_app.tools);
  agent::ToolExecutor configured_browser_executor(configured_browser_tools);
  auto configured_browser_extract = configured_browser_executor.execute_tool_call(agent::ToolCall{
      .id = "config_browser_extract_1",
      .name = "browser.extract",
      .arguments = agent::Value::object({
          {"url", "https://example.test/config-browser"},
          {"selector", "#content"},
          {"waitUntil", "domcontentloaded"},
          {"timeoutMs", 1500},
      }),
  });
  SMOKE_CHECK(configured_browser_called);
  SMOKE_CHECK(configured_browser_extract.ok);
  SMOKE_CHECK(configured_browser_extract.output.find("configured browser renderer output") != std::string::npos);
  bool missing_config_browser_adapter = false;
  try {
    (void)agent::resolve_native_agent_app(config_with_browser, "main");
  } catch (const agent::ConfigurationError& error) {
    missing_config_browser_adapter =
        std::string(error.what()).find("NativeBrowserAdapters.renderer") != std::string::npos;
  }
  SMOKE_CHECK(missing_config_browser_adapter);

  auto custom_provider_factory_called = std::make_shared<bool>(false);
  class CustomDescriptorReasoningMapper final : public agent::ProviderReasoningMapper {
   public:
    bool matches(const agent::ProviderRequestProfile& profile) const override {
      return profile.provider == "custom-descriptor-smoke";
    }

    agent::ProviderReasoningApplyResult apply(
        agent::NativeProviderRequest& request,
        const agent::ProviderReasoningContext& context) const override {
      request.metadata["customReasoningMapper"] = true;
      return agent::ProviderReasoningApplyResult{
          .requested = context.reasoning && context.reasoning->enabled.value_or(false),
          .applied = true,
          .mode = agent::ProviderReasoningMode::None,
          .disable_strategy = agent::ProviderReasoningDisableStrategy::Unsupported,
          .metadata = agent::Value::object({{"provider", context.provider}}),
      };
    }
  };
  auto custom_provider_reasoning_mapper =
      std::make_shared<CustomDescriptorReasoningMapper>();
  agent::register_native_chat_provider_descriptor(agent::NativeChatProviderDescriptor{
      .provider = "custom-descriptor-smoke",
      .create_adapter = [custom_provider_factory_called](
                            const agent::Value& model,
                            agent::NativeChatProviderBuildContext) -> std::shared_ptr<agent::ChatModelAdapter> {
        *custom_provider_factory_called = true;
        SMOKE_CHECK(model.at("model").as_string() == "custom-model");
        return std::make_shared<agent::EchoChatModelAdapter>();
      },
      .resolve_protocol = [](const agent::Value&) {
        return agent::ProviderRequestProtocol::OpenAIResponses;
      },
      .default_capabilities = {"input.text", "custom.capability", "reasoning"},
      .resolve_reasoning_mapper = [custom_provider_reasoning_mapper](const agent::Value&) {
        return custom_provider_reasoning_mapper;
      },
      .extract_usage = [](const agent::NativeProviderUsageExtractionContext& context) {
        agent::ModelUsage usage;
        usage.provider = context.provider;
        usage.input_tokens = 13;
        usage.output_tokens = 5;
        usage.total_tokens = 18;
        usage.input_tokens_source = agent::TokenUsageSource::Provider;
        usage.output_tokens_source = agent::TokenUsageSource::Provider;
        usage.total_tokens_source = agent::TokenUsageSource::Provider;
        usage.quality = agent::TokenUsageQuality::Provider;
        return usage;
      },
      .apply_extra = [](agent::NativeProviderRequest& request,
                        const agent::Value&,
                        const agent::ModelSettings&) {
        request.body["customExtraApplied"] = true;
      },
      .media_input_marshaller = agent::NativeChatProviderMediaInputMarshaller{
          .name = "custom-media",
          .supported_parts = {agent::ContentPartType::Image},
          .accepted_sources = {agent::MediaSourceKind::Inline, agent::MediaSourceKind::Path},
          .requires_resolver_for_non_inline = true,
      },
      .resolve_stream_parser_metadata = [](const agent::Value&) {
        return agent::NativeChatProviderStreamParserMetadata{
            .parser = "custom-stream",
            .frame_boundary = agent::ModelStreamFrameBoundary::SseEvent,
            .events = {agent::ModelStreamEventType::ResponseStart,
                       agent::ModelStreamEventType::TextDelta,
                       agent::ModelStreamEventType::Response},
            .includes_terminal_usage = true,
            .reasoning_parser = agent::NativeProviderReasoningParser::None,
        };
      },
  });
  const auto custom_provider_model = agent::Value::object({
      {"provider", "custom-descriptor-smoke"},
      {"model", "custom-model"},
  });
  SMOKE_CHECK(agent::resolve_native_chat_provider_protocol(custom_provider_model) ==
              agent::ProviderRequestProtocol::OpenAIResponses);
  SMOKE_CHECK(agent::resolve_native_chat_provider_reasoning_mapper(custom_provider_model) ==
              custom_provider_reasoning_mapper);
  auto custom_usage_extractor = agent::resolve_native_chat_provider_usage_extractor(custom_provider_model);
  SMOKE_CHECK(static_cast<bool>(custom_usage_extractor));
  const auto custom_usage = custom_usage_extractor(agent::NativeProviderUsageExtractionContext{
      .provider = "custom-descriptor-smoke",
      .model = "custom-model",
  });
  SMOKE_CHECK(custom_usage.total_tokens == 18);
  auto custom_extra_mapper = agent::resolve_native_chat_provider_extra_mapper(custom_provider_model);
  SMOKE_CHECK(static_cast<bool>(custom_extra_mapper));
  agent::NativeProviderRequest custom_provider_request;
  custom_provider_request.body = agent::Value::object({});
  custom_extra_mapper(custom_provider_request, custom_provider_model, agent::ModelSettings{});
  SMOKE_CHECK(custom_provider_request.body.at("customExtraApplied").as_bool());
  const auto custom_media_marshaller =
      agent::resolve_native_chat_provider_media_input_marshaller(custom_provider_model);
  SMOKE_CHECK(custom_media_marshaller.has_value());
  SMOKE_CHECK(custom_media_marshaller->name == "custom-media");
  const auto custom_stream_metadata =
      agent::resolve_native_chat_provider_stream_parser_metadata(custom_provider_model);
  SMOKE_CHECK(custom_stream_metadata.has_value());
  SMOKE_CHECK(custom_stream_metadata->parser == "custom-stream");
  auto custom_provider_app = agent::resolve_native_agent_app(
      agent::Value::object({
          {"agents", agent::Value::object({
              {"main", agent::Value::object({
                  {"model", custom_provider_model},
              })},
          })},
      }),
      "main");
  SMOKE_CHECK(*custom_provider_factory_called);
  const auto custom_provider_result =
      custom_provider_app.runner->execution().run("custom descriptor run", "custom-provider-session");
  SMOKE_CHECK(custom_provider_result.text.find("custom descriptor run") != std::string::npos);

  const auto config_knowledge_store = persistent_dir / "configured-knowledge.json";
  const auto config_knowledge_image = persistent_dir / "configured-knowledge-image.png";
  {
    std::ofstream image_file(config_knowledge_image, std::ios::binary);
    image_file << "configured image bytes";
  }
  auto config_with_knowledge_base = agent::Value::object({
      {"resources", agent::Value::object({
          {"knowledgeBases", agent::Value::object({
              {"docs", agent::Value::object({
                  {"id", "docs"},
                  {"title", "Configured Docs"},
                  {"embedder", agent::Value::object({
                      {"provider", "hash"},
                      {"model", "configured-hash-text"},
                      {"dimensions", 32},
                      {"spaceId", "configured-shared-space"},
                  })},
                  {"imageEmbedder", agent::Value::object({
                      {"provider", "hash"},
                      {"model", "configured-hash-image"},
                      {"dimensions", 32},
                      {"spaceId", "configured-shared-space"},
                  })},
                  {"store", agent::Value::object({
                      {"kind", "file"},
                      {"filePath", config_knowledge_store.string()},
                  })},
                  {"sources", agent::Value::array({
                      agent::Value::object({
                          {"type", "text"},
                          {"uri", "memory://configured-doc"},
                          {"title", "Configured Knowledge"},
                          {"text", "Configured native knowledge base answers retrieval questions."},
                      }),
                      agent::Value::object({
                          {"type", "file"},
                          {"path", config_knowledge_image.string()},
                          {"title", "Configured Knowledge Image"},
                          {"metadata", agent::Value::object({
                              {"altText", "Configured image retrieval diagram."},
                          })},
                      }),
                  })},
                  {"ingestOptions", agent::Value::object({
                      {"documentBatchSize", 1},
                      {"embeddingBatchSize", 1},
                      {"maxAttempts", 1},
                      {"retryDelayMs", 0},
                  })},
              })},
          })},
      })},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"agent"})}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({{"use", "docs"}})},
              })},
          })},
      })},
  });
  auto knowledge_base_app = agent::resolve_native_agent_app(config_with_knowledge_base, "main");
  SMOKE_CHECK(knowledge_base_app.knowledge);
  SMOKE_CHECK(knowledge_base_app.knowledge->base);
  SMOKE_CHECK(knowledge_base_app.knowledge->base->stats().document_count == 2);
  SMOKE_CHECK(std::filesystem::exists(config_knowledge_store));
  auto configured_embedding_hits = knowledge_base_app.knowledge->base->search(
      "configured image retrieval questions",
      agent::KnowledgeSearchOptions{.top_k = 4, .min_score = 0.0});
  SMOKE_CHECK(!configured_embedding_hits.empty());
  bool configured_embedding_space_seen = false;
  bool configured_image_hit_seen = false;
  for (const auto& hit : configured_embedding_hits) {
    SMOKE_CHECK(hit.chunk.embedding_space_id == "configured-shared-space");
    SMOKE_CHECK(hit.chunk.embedding.size() == 32);
    configured_embedding_space_seen = true;
    if (hit.chunk.asset_type == agent::KnowledgeAssetType::Image) {
      configured_image_hit_seen = true;
    }
  }
  SMOKE_CHECK(configured_embedding_space_seen);
  SMOKE_CHECK(configured_image_hit_seen);
  agent::ToolRegistry configured_knowledge_tools(knowledge_base_app.tools);
  agent::ToolExecutor configured_knowledge_executor(configured_knowledge_tools);
  auto configured_knowledge_search = configured_knowledge_executor.execute_tool_call(agent::ToolCall{
      .id = "config_knowledge_search_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "retrieval questions"}, {"topK", 1}}),
  });
  SMOKE_CHECK(configured_knowledge_search.ok);
  SMOKE_CHECK(configured_knowledge_search.output.find("Configured Knowledge") != std::string::npos);

  auto config_with_knowledge_search_defaults = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "search-default-docs"},
                      {"title", "Search Defaults"},
                      {"description", "Config search defaults description"},
                      {"contextTitle", "Configured search default context"},
                      {"searchDefaults", agent::Value::object({
                          {"topK", 1},
                          {"minScore", 0.0},
                      })},
                      {"sources", agent::Value::array({
                          agent::Value::object({
                              {"type", "text"},
                              {"uri", "memory://search-default-a"},
                              {"title", "Search Default A"},
                              {"text", "alpha beta search default first."},
                          }),
                          agent::Value::object({
                              {"type", "text"},
                              {"uri", "memory://search-default-b"},
                              {"title", "Search Default B"},
                              {"text", "alpha beta search default second."},
                          }),
                      })},
                  })},
              })},
          })},
      })},
  });
  auto search_default_app = agent::resolve_native_agent_app(config_with_knowledge_search_defaults, "main");
  SMOKE_CHECK(search_default_app.knowledge->base->description() == "Config search defaults description");
  auto configured_default_hits = search_default_app.knowledge->base->search("alpha beta");
  SMOKE_CHECK(configured_default_hits.size() == 1);
  SMOKE_CHECK(configured_default_hits.front().citation.knowledge_base_title == "Search Defaults");
  auto configured_default_hit_value = agent::knowledge_search_hit_to_value(configured_default_hits.front());
  SMOKE_CHECK(!configured_default_hit_value.at("document").at("createdAt").as_string().empty());
  SMOKE_CHECK(!configured_default_hit_value.at("document").at("updatedAt").as_string().empty());
  SMOKE_CHECK(!configured_default_hit_value.at("chunk").at("createdAt").as_string().empty());
  SMOKE_CHECK(!configured_default_hit_value.at("chunk").at("updatedAt").as_string().empty());
  SMOKE_CHECK(!configured_default_hit_value.at("chunk").at("embedding").as_array().empty());
  auto configured_default_context = search_default_app.knowledge->base->build_context_message("alpha beta");
  SMOKE_CHECK(configured_default_context.message);
  SMOKE_CHECK(agent::extract_text_content(configured_default_context.message->content).find(
             "Configured search default context") != std::string::npos);
  SMOKE_CHECK(configured_default_context.message->metadata.at("knowledgeBaseId").as_string() == "search-default-docs");
  auto configured_default_override_hits = search_default_app.knowledge->base->search(
      "alpha beta", agent::KnowledgeSearchOptions{.top_k = 2, .min_score = 0.0});
  SMOKE_CHECK(configured_default_override_hits.size() == 2);

  const auto production_preset_dir = persistent_dir / ("production-preset-" + agent::generate_uuid());
  agent::Value::Array production_sources;
  for (int index = 0; index < 7; ++index) {
    production_sources.push_back(agent::Value::object({
        {"type", "text"},
        {"uri", "memory://production-" + std::to_string(index)},
        {"title", "Production " + std::to_string(index)},
        {"text", "alpha production preset document " + std::to_string(index)},
    }));
  }
  auto production_preset_app = agent::resolve_native_agent_app(agent::Value::object({
      {"_cwd", production_preset_dir.string()},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "production-docs"},
                      {"tenantId", "tenant-a"},
                      {"preset", "production"},
                      {"persistent", true},
                      {"sources", agent::Value(production_sources)},
                  })},
              })},
          })},
      })},
  }), "main");
  const auto production_sync_state =
      production_preset_dir / ".node-agent" / "knowledge-sync" / "tenant-a" / "production-docs.json";
  const auto production_vectors =
      production_preset_dir / ".node-agent" / "knowledge" / "tenant-a" / "production-docs" / "vectors.sqlite";
  SMOKE_CHECK(std::filesystem::exists(production_sync_state));
  SMOKE_CHECK(std::filesystem::exists(production_vectors));
  auto production_default_hits = production_preset_app.knowledge->base->search(
      "alpha", agent::KnowledgeSearchOptions{.min_score = 0.0});
  SMOKE_CHECK(production_default_hits.size() == 7);

  const auto sync_delete_state = production_preset_dir / "explicit-sync-state.json";
  auto sync_delete_base_config = [&](agent::Value::Array sources) {
    return agent::Value::object({
        {"_cwd", production_preset_dir.string()},
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"knowledge", agent::Value::object({
                    {"base", agent::Value::object({
                        {"id", "sync-delete-docs"},
                        {"tenantId", "tenant-a"},
                        {"preset", "production"},
                        {"persistent", true},
                        {"sync", agent::Value::object({
                            {"enabled", true},
                            {"stateFilePath", sync_delete_state.string()},
                            {"deleteMissing", true},
                        })},
                        {"sources", agent::Value(sources)},
                    })},
                })},
            })},
        })},
    });
  };
  agent::Value::Array sync_delete_initial_sources{
      agent::Value::object({
          {"type", "text"},
          {"uri", "memory://sync-delete-a"},
          {"title", "Sync Delete A"},
          {"text", "sync delete alpha retained"},
      }),
      agent::Value::object({
          {"type", "text"},
          {"uri", "memory://sync-delete-b"},
          {"title", "Sync Delete B"},
          {"text", "sync delete beta removed"},
      }),
  };
  (void)agent::resolve_native_agent_app(sync_delete_base_config(sync_delete_initial_sources), "main");
  agent::Value::Array sync_delete_next_sources{
      agent::Value::object({
          {"type", "text"},
          {"uri", "memory://sync-delete-a"},
          {"title", "Sync Delete A"},
          {"text", "sync delete alpha retained"},
      }),
  };
  auto sync_delete_app = agent::resolve_native_agent_app(sync_delete_base_config(sync_delete_next_sources), "main");
  auto sync_delete_documents = sync_delete_app.knowledge->base->store()->list_documents();
  SMOKE_CHECK(sync_delete_documents.size() == 1);
  SMOKE_CHECK(sync_delete_documents.front().uri == "memory://sync-delete-a");
  SMOKE_CHECK(std::filesystem::exists(sync_delete_state));

  auto config_with_clip_embedder = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "clip-config-docs"},
                      {"embedder", agent::Value::object({
                          {"provider", "clip"},
                          {"model", "configured-clip"},
                          {"dimensions", 3},
                          {"spaceId", "configured-clip-space"},
                      })},
                      {"imageEmbedder", agent::Value::object({
                          {"provider", "clip"},
                          {"model", "configured-clip"},
                          {"dimensions", 3},
                          {"spaceId", "configured-clip-space"},
                      })},
                  })},
              })},
          })},
      })},
  });
  auto clip_embedder_app = agent::resolve_native_agent_app(config_with_clip_embedder, "main");
  SMOKE_CHECK(clip_embedder_app.knowledge);
  SMOKE_CHECK(clip_embedder_app.knowledge->base->stats().document_count == 0);
  bool saw_config_clip_batch_error = false;
  try {
    (void)clip_embedder_app.knowledge->base->search("clip query");
  } catch (const agent::ConfigurationError& error) {
    saw_config_clip_batch_error = std::string(error.what()).find("ClipTextEmbeddingAdapter") != std::string::npos;
  }
  SMOKE_CHECK(saw_config_clip_batch_error);

  int configured_provider_embedding_calls = 0;
  auto config_with_provider_embedder = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "provider-embedder-docs"},
                      {"embedder", agent::Value::object({
                          {"provider", "openai"},
                          {"model", "openai-config-embedding"},
                          {"apiKey", "openai-config-key"},
                          {"baseUrl", "https://api.openai.test/v1/"},
                          {"organization", "org_config"},
                          {"dimensions", 3},
                      })},
                      {"sources", agent::Value::array({
                          agent::Value::object({
                              {"type", "text"},
                              {"uri", "memory://provider-embedder-doc"},
                              {"title", "Provider Embedder Config"},
                              {"text", "Provider configured embedding source."},
                          }),
                      })},
                  })},
              })},
          })},
      })},
  });
  auto provider_embedder_app = agent::resolve_native_agent_app(
      config_with_provider_embedder,
      native_resolve_options("main", [&](const agent::NativeProviderRequest& request) {
        ++configured_provider_embedding_calls;
        SMOKE_CHECK(request.provider == "openai");
        SMOKE_CHECK(request.endpoint == "/embeddings");
        SMOKE_CHECK(request.base_url == "https://api.openai.test/v1");
        SMOKE_CHECK(request.headers.at("authorization") == "Bearer openai-config-key");
        SMOKE_CHECK(request.headers.at("openai-organization") == "org_config");
        SMOKE_CHECK(request.body.at("model").as_string() == "openai-config-embedding");
        SMOKE_CHECK(request.body.at("dimensions").as_integer() == 3);
        agent::Value::Array data;
        std::size_t index = 0;
        for (const auto& input : request.body.at("input").as_array()) {
          (void)input;
          data.push_back(agent::Value::object({
              {"index", index++},
              {"embedding", agent::Value::array({1.0, 0.0, 0.0})},
          }));
        }
        return agent::Value::object({{"data", agent::Value(std::move(data))}});
      }));
  SMOKE_CHECK(provider_embedder_app.knowledge && provider_embedder_app.knowledge->base);
  SMOKE_CHECK(configured_provider_embedding_calls == 1);
  auto provider_embedding_hits = provider_embedder_app.knowledge->base->search(
      "provider configured embedding", agent::KnowledgeSearchOptions{.top_k = 1, .min_score = 0.0});
  SMOKE_CHECK(configured_provider_embedding_calls == 2);
  SMOKE_CHECK(!provider_embedding_hits.empty());
  SMOKE_CHECK(provider_embedding_hits.front().chunk.embedding_space_id == "openai:openai-config-embedding");
  SMOKE_CHECK(provider_embedding_hits.front().chunk.embedding.size() == 3);

  auto config_with_rendered_knowledge = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"agent"})}})},
              {"browser", agent::Value::object({
                  {"renderer", agent::Value::object({{"kind", "playwright"}})},
              })},
              {"web", agent::Value::object({
                  {"fetcher", agent::Value::object({{"kind", "default"}})},
                  {"crawler", agent::Value::object({{"kind", "default"}, {"maxDepth", 0}, {"maxPages", 1}})},
              })},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "rendered-docs"},
                      {"sources", agent::Value::array({
                          agent::Value::object({
                              {"type", "website"},
                              {"url", "https://rendered-knowledge.example.test/"},
                              {"render", true},
                              {"maxDepth", 0},
                              {"maxPages", 1},
                          }),
                      })},
                  })},
              })},
          })},
      })},
  });
  agent::NativeWebAdapters rendered_knowledge_web_adapters;
  rendered_knowledge_web_adapters.fetch_transport = [](const agent::NativeWebFetchTransportRequest& request) {
    if (request.request.url == "https://rendered-knowledge.example.test/robots.txt" ||
        request.request.url == "https://rendered-knowledge.example.test/sitemap.xml") {
      return agent::WebFetchedPage{
          .url = request.request.url,
          .final_url = request.request.url,
          .status = 200,
          .content_type = "text/plain",
      };
    }
    SMOKE_CHECK(request.request.url == "https://rendered-knowledge.example.test/");
    return agent::WebFetchedPage{
        .url = request.request.url,
        .final_url = request.request.url,
        .status = 200,
        .content_type = "text/html",
        .title = "Unrendered Knowledge",
        .html = "<html><body><p>unrendered crawler knowledge</p></body></html>",
    };
  };
  bool config_knowledge_browser_called = false;
  agent::NativeBrowserAdapters rendered_knowledge_browser_adapters;
  rendered_knowledge_browser_adapters.renderer = std::make_shared<agent::BrowserCallbackRenderer>(
      [&](const agent::BrowserRenderRequest& request) {
        config_knowledge_browser_called = true;
        SMOKE_CHECK(request.url == "https://rendered-knowledge.example.test/");
        return agent::BrowserRenderResult{
            .url = request.url,
            .title = "Rendered Knowledge Config",
            .html = "<html><body>configured rendered website knowledge</body></html>",
            .text = "configured rendered website knowledge",
        };
      });
  auto rendered_knowledge_app = agent::resolve_native_agent_app(
      config_with_rendered_knowledge,
      native_resolve_options("main", {}, {}, rendered_knowledge_web_adapters, {},
                             rendered_knowledge_browser_adapters));
  SMOKE_CHECK(config_knowledge_browser_called);
  SMOKE_CHECK(rendered_knowledge_app.knowledge && rendered_knowledge_app.knowledge->base);
  auto rendered_knowledge_hits = rendered_knowledge_app.knowledge->base->search(
      "configured rendered website", agent::KnowledgeSearchOptions{.top_k = 1, .min_score = 0.0});
  SMOKE_CHECK(!rendered_knowledge_hits.empty());
  SMOKE_CHECK(rendered_knowledge_hits.front().document.title == "Rendered Knowledge Config");

  const auto config_knowledge_manager_dir = persistent_dir / "configured-knowledge-manager";
  auto config_with_knowledge_manager = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"agent"})}})},
              {"knowledge", agent::Value::object({
                  {"manager", agent::Value::object({
                      {"baseDir", config_knowledge_manager_dir.string()},
                      {"bases", agent::Value::array({
                          agent::Value::object({
                              {"id", "managed"},
                              {"title", "Managed Knowledge"},
                              {"persistent", true},
                              {"sources", agent::Value::array({
                                  agent::Value::object({
                                      {"type", "text"},
                                      {"uri", "memory://managed-doc"},
                                      {"title", "Managed Config"},
                                      {"text", "Managed native knowledge manager can search configured bases."},
                                  }),
                              })},
                          }),
                      })},
                  })},
              })},
          })},
      })},
  });
  auto knowledge_manager_app = agent::resolve_native_agent_app(config_with_knowledge_manager, "main");
  SMOKE_CHECK(knowledge_manager_app.knowledge);
  SMOKE_CHECK(knowledge_manager_app.knowledge->manager);
  SMOKE_CHECK(std::filesystem::exists(config_knowledge_manager_dir / "knowledge-bases.json"));
  agent::ToolRegistry configured_manager_tools(knowledge_manager_app.tools);
  agent::ToolExecutor configured_manager_executor(configured_manager_tools);
  auto configured_manager_search = configured_manager_executor.execute_tool_call(agent::ToolCall{
      .id = "config_knowledge_manager_search_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "configured bases"}, {"topK", 1}}),
  });
  SMOKE_CHECK(configured_manager_search.ok);
  SMOKE_CHECK(configured_manager_search.output.find("Managed Config") != std::string::npos);
  auto configured_manager_zero_search = configured_manager_executor.execute_tool_call(agent::ToolCall{
      .id = "config_knowledge_manager_search_zero_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "configured bases"}, {"topK", 0}}),
  });
  SMOKE_CHECK(configured_manager_zero_search.ok);
  SMOKE_CHECK(configured_manager_zero_search.output.find("\"hits\": []") != std::string::npos);
  SMOKE_CHECK(configured_manager_zero_search.output.find("Managed Config") == std::string::npos);

  const auto config_workflow_dir = persistent_dir / "configured-workflow-runs";
  auto config_with_workflow = agent::Value::object({
      {"resources", agent::Value::object({
          {"workflowEngines", agent::Value::object({
              {"primary", agent::Value::object({
                  {"store", agent::Value::object({
                      {"kind", "file"},
                      {"baseDir", config_workflow_dir.string()},
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
  auto workflow_configured_app = agent::resolve_native_agent_app(config_with_workflow, "main");
  SMOKE_CHECK(workflow_configured_app.workflow);
  SMOKE_CHECK(workflow_configured_app.workflow->engine);
  auto configured_workflow = agent::WorkflowBuilder("config_wf", "Config Workflow")
                                 .start()
                                 .tool("eval", "math.eval",
                                       {{"expression", agent::workflow_literal("1+2")}})
                                 .end("end", agent::workflow_ref("nodes.eval.output"))
                                 .sequence({"start", "eval", "end"})
                                 .build();
  auto configured_workflow_result = workflow_configured_app.workflow->engine->run(configured_workflow);
  SMOKE_CHECK(configured_workflow_result.status == agent::WorkflowStatus::Completed);
  SMOKE_CHECK(configured_workflow_result.output.as_string().find("3") != std::string::npos);
  SMOKE_CHECK(std::filesystem::exists(config_workflow_dir / (configured_workflow_result.workflow_run_id + ".json")));
  agent::ToolRegistry configured_workflow_tools(workflow_configured_app.tools);
  agent::ToolExecutor configured_workflow_executor(configured_workflow_tools);
  auto configured_workflow_get = configured_workflow_executor.execute_tool_call(agent::ToolCall{
      .id = "config_workflow_get_1",
      .name = "workflow.getRun",
      .arguments = agent::Value::object({{"workflowRunId", configured_workflow_result.workflow_run_id}}),
  });
  SMOKE_CHECK(configured_workflow_get.ok);
  SMOKE_CHECK(configured_workflow_get.output.find(configured_workflow_result.workflow_run_id) != std::string::npos);
  auto configured_workflow_checkpoints = configured_workflow_executor.execute_tool_call(agent::ToolCall{
      .id = "config_workflow_checkpoints_1",
      .name = "workflow.listCheckpoints",
      .arguments = agent::Value::object({{"workflowRunId", configured_workflow_result.workflow_run_id}}),
  });
  SMOKE_CHECK(configured_workflow_checkpoints.ok);
  SMOKE_CHECK(configured_workflow_checkpoints.output.find("workflow.node.completed") != std::string::npos);

  auto config_with_web = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"web"})}})},
              {"web", agent::Value::object({
                  {"search", agent::Value::array({
                      agent::Value::object({
                          {"kind", "brave"},
                          {"apiKey", "test-key"},
                          {"default", true},
                      }),
                  })},
                  {"fetcher", agent::Value::object({{"kind", "default"}})},
                  {"crawler", agent::Value::object({
                      {"kind", "default"},
                      {"maxDepth", 1},
                      {"maxPages", 3},
                      {"allowedDomains", agent::Value::array({"example.test"})},
                      {"obeyRobots", false},
                      {"discoverSitemap", false},
                  })},
              })},
          })},
      })},
  });
  agent::NativeWebAdapters web_adapters;
  bool web_search_called = false;
  web_adapters.search_transport = [&](const agent::NativeWebSearchRequest& request) {
    web_search_called = true;
    SMOKE_CHECK(request.provider == "brave");
    SMOKE_CHECK(request.endpoint == "/web/search");
    SMOKE_CHECK(request.url == "https://api.search.brave.com/res/v1/web/search");
    SMOKE_CHECK(request.headers.at("x-subscription-token") == "test-key");
    SMOKE_CHECK(request.query_params.at("q").as_string() == "native docs");
    SMOKE_CHECK(request.query_params.at("count").as_integer() == 2);
    SMOKE_CHECK(request.query_params.at("safesearch").as_string() == "strict");
    return agent::Value::object({
        {"web", agent::Value::object({
            {"results", agent::Value::array({
                agent::Value::object({
                    {"title", "Native docs"},
                    {"url", "https://example.test/docs"},
                    {"description", "C++ docs"},
                    {"age", "2026-05-20"},
                }),
                agent::Value::object({
                    {"title", "Other"},
                    {"url", "https://other.test/docs"},
                    {"description", "Filtered"},
                }),
            })},
        })},
    });
  };
  bool web_fetch_called = false;
  web_adapters.fetch_transport = [&](const agent::NativeWebFetchTransportRequest& request) {
    web_fetch_called = true;
    if (request.request.url == "https://example.test/") {
      return agent::WebFetchedPage{
          .url = request.request.url,
          .final_url = request.request.url,
          .status = 200,
          .content_type = "text/html",
          .title = "Home",
          .html = "<html><body><a href=\"/docs\">Docs</a><p>home</p></body></html>",
      };
    }
    if (request.request.url == "https://example.test/docs") {
      return agent::WebFetchedPage{
          .url = request.request.url,
          .final_url = request.request.url,
          .status = 200,
          .content_type = "text/html",
          .title = "Docs",
          .html = "<html><body><p>configured web docs</p></body></html>",
      };
    }
    throw agent::ConfigurationError("Unexpected fetch URL " + request.request.url);
  };
  auto web_configured_app = agent::resolve_native_agent_app(
      config_with_web,
      native_resolve_options("main", {}, {}, web_adapters));
  SMOKE_CHECK(web_configured_app.web);
  SMOKE_CHECK(web_configured_app.web->fetcher);
  SMOKE_CHECK(web_configured_app.web->crawler);
  agent::ToolRegistry configured_web_tools(web_configured_app.tools);
  agent::ToolExecutor configured_web_executor(configured_web_tools);
  auto configured_web_search = configured_web_executor.execute_tool_call(agent::ToolCall{
      .id = "config_web_search_1",
      .name = "web.search",
      .arguments = agent::Value::object({
          {"query", "native docs"},
          {"topK", 2},
          {"safeSearch", "strict"},
          {"domains", agent::Value::array({"example.test"})},
      }),
  });
  SMOKE_CHECK(configured_web_search.ok);
  SMOKE_CHECK(web_search_called);
  SMOKE_CHECK(configured_web_search.output.find("Native docs") != std::string::npos);
  SMOKE_CHECK(configured_web_search.output.find("Other") == std::string::npos);
#ifndef _WIN32
  const char* old_brave_key = std::getenv("BRAVE_SEARCH_API_KEY");
  const char* old_brave_base = std::getenv("BRAVE_SEARCH_BASE_URL");
  const bool had_brave_key = old_brave_key != nullptr;
  const bool had_brave_base = old_brave_base != nullptr;
  const std::string old_brave_key_value = had_brave_key ? old_brave_key : "";
  const std::string old_brave_base_value = had_brave_base ? old_brave_base : "";
  SMOKE_CHECK(::setenv("BRAVE_SEARCH_API_KEY", "env-brave-key", 1) == 0);
  SMOKE_CHECK(::setenv("BRAVE_SEARCH_BASE_URL", "https://env-brave.example.test/root", 1) == 0);
  auto config_with_default_brave_env = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"web"})}})},
              {"web", agent::Value::object({
                  {"search", agent::Value::array({
                      agent::Value::object({{"kind", "brave"}, {"default", true}}),
                  })},
              })},
          })},
      })},
  });
  bool default_env_brave_called = false;
  agent::NativeWebAdapters default_env_web_adapters;
  default_env_web_adapters.search_transport = [&](const agent::NativeWebSearchRequest& request) {
    default_env_brave_called = true;
    SMOKE_CHECK(request.provider == "brave");
    SMOKE_CHECK(request.url == "https://env-brave.example.test/root/web/search");
    SMOKE_CHECK(request.headers.at("x-subscription-token") == "env-brave-key");
    return agent::Value::object({
        {"web", agent::Value::object({
            {"results", agent::Value::array({
                agent::Value::object({
                    {"title", "Env Brave"},
                    {"url", "https://env.example.test/result"},
                }),
            })},
        })},
    });
  };
  auto default_env_web_app =
      agent::resolve_native_agent_app(config_with_default_brave_env,
                                      native_resolve_options("main", {}, {}, default_env_web_adapters));
  agent::ToolRegistry default_env_web_tools(default_env_web_app.tools);
  agent::ToolExecutor default_env_web_executor(default_env_web_tools);
  auto default_env_web_search = default_env_web_executor.execute_tool_call(agent::ToolCall{
      .id = "config_web_brave_env_search_1",
      .name = "web.search",
      .arguments = agent::Value::object({{"query", "env brave"}}),
  });
  SMOKE_CHECK(default_env_web_search.ok);
  SMOKE_CHECK(default_env_brave_called);
  SMOKE_CHECK(default_env_web_search.output.find("Env Brave") != std::string::npos);
  if (had_brave_key) {
    SMOKE_CHECK(::setenv("BRAVE_SEARCH_API_KEY", old_brave_key_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("BRAVE_SEARCH_API_KEY") == 0);
  }
  if (had_brave_base) {
    SMOKE_CHECK(::setenv("BRAVE_SEARCH_BASE_URL", old_brave_base_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("BRAVE_SEARCH_BASE_URL") == 0);
  }
#endif
  auto configured_web_fetch = configured_web_executor.execute_tool_call(agent::ToolCall{
      .id = "config_web_fetch_1",
      .name = "web.fetch",
      .arguments = agent::Value::object({{"url", "https://example.test/docs"}}),
  });
  SMOKE_CHECK(configured_web_fetch.ok);
  SMOKE_CHECK(web_fetch_called);
  SMOKE_CHECK(configured_web_fetch.output.find("configured web docs") != std::string::npos);
  auto config_with_web_fallback = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"web"})}})},
              {"browser", agent::Value::object({
                  {"renderer", agent::Value::object({{"kind", "playwright"}})},
              })},
              {"web", agent::Value::object({
                  {"fetcher", agent::Value::object({
                      {"kind", "default"},
                      {"browserFallback", agent::Value::object({
                          {"minimumTextLength", 40},
                          {"selector", "main"},
                          {"waitUntil", "load"},
                          {"timeoutMs", 2200},
                      })},
                  })},
              })},
          })},
      })},
  });
  agent::NativeWebAdapters web_fallback_adapters;
  bool web_fallback_primary_called = false;
  web_fallback_adapters.fetch_transport = [&](const agent::NativeWebFetchTransportRequest& request) {
    web_fallback_primary_called = true;
    SMOKE_CHECK(request.request.url == "https://example.test/browser-fallback");
    return agent::WebFetchedPage{
        .url = request.request.url,
        .final_url = request.request.url,
        .status = 200,
        .content_type = "text/html",
        .title = "Short Config Page",
        .text = "short",
        .markdown = "short",
    };
  };
  bool web_fallback_browser_called = false;
  agent::NativeBrowserAdapters web_fallback_browser_adapters;
  web_fallback_browser_adapters.renderer = std::make_shared<agent::BrowserCallbackRenderer>(
      [&](const agent::BrowserRenderRequest& request) {
        web_fallback_browser_called = true;
        SMOKE_CHECK(request.url == "https://example.test/browser-fallback");
        SMOKE_CHECK(request.selector == "main");
        SMOKE_CHECK(request.wait_until == "load");
        SMOKE_CHECK(request.timeout_ms == 2200);
        return agent::BrowserRenderResult{
            .url = "https://example.test/browser-fallback/rendered",
            .title = "Rendered Config Page",
            .html = "<html><body><main>configured browser fallback text</main></body></html>",
            .text = "configured browser fallback text",
        };
      });
  auto web_fallback_app = agent::resolve_native_agent_app(
      config_with_web_fallback,
      native_resolve_options("main", {}, {}, web_fallback_adapters, {}, web_fallback_browser_adapters));
  agent::ToolRegistry web_fallback_tools(web_fallback_app.tools);
  agent::ToolExecutor web_fallback_executor(web_fallback_tools);
  auto web_fallback_fetch = web_fallback_executor.execute_tool_call(agent::ToolCall{
      .id = "config_web_fallback_fetch_1",
      .name = "web.fetch",
      .arguments = agent::Value::object({{"url", "https://example.test/browser-fallback"}}),
  });
  SMOKE_CHECK(web_fallback_fetch.ok);
  SMOKE_CHECK(web_fallback_primary_called);
  SMOKE_CHECK(web_fallback_browser_called);
  SMOKE_CHECK(web_fallback_fetch.output.find("configured browser fallback text") != std::string::npos);
  bool missing_web_fallback_browser_failed = false;
  try {
    (void)agent::resolve_native_agent_app(
        agent::Value::object({
            {"agents", agent::Value::object({
                {"main", agent::Value::object({
                    {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                    {"tools", agent::Value::object({{"bundles", agent::Value::array({"web"})}})},
                    {"web", agent::Value::object({
                        {"fetcher", agent::Value::object({
                            {"browserFallback", agent::Value::object({{"minimumTextLength", 1}})},
                        })},
                    })},
                })},
            })},
        }),
        "main");
  } catch (const agent::ConfigurationError& error) {
    missing_web_fallback_browser_failed =
        std::string(error.what()).find("web.fetcher.browserFallback") != std::string::npos;
  }
  SMOKE_CHECK(missing_web_fallback_browser_failed);

  const auto config_mcp_dir = persistent_dir / "agent-mcp";
  std::filesystem::create_directories(config_mcp_dir);
  agent::write_json_file(config_mcp_dir / ".mcp.json", agent::Value::object({
      {"mcpServers", agent::Value::object({
          {"fixture", agent::Value::object({{"command", "./server"}})},
          {"ignored", agent::Value::object({{"command", "./ignored"}})},
      })},
  }));
  auto config_with_mcp = agent::Value::object({
      {"_cwd", config_mcp_dir.string()},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"core"})}})},
              {"mcp", agent::Value::object({
                  {"anthropic", agent::Value::object({
                      {"servers", agent::Value::array({"fixture"})},
                  })},
              })},
          })},
      })},
  });
  bool mcp_factory_called = false;
  auto mcp_configured_app = agent::resolve_native_agent_app(
      config_with_mcp,
      native_resolve_options("main", {},
      [&](const agent::AnthropicMcpServerConfig& server) -> std::shared_ptr<agent::MCPTransport> {
        mcp_factory_called = true;
        SMOKE_CHECK(server.name == "fixture");
        SMOKE_CHECK(std::filesystem::path(server.command).is_absolute());
        return std::make_shared<agent::MCPCallbackTransport>([](const agent::Value& message) {
          const auto method = message.at("method").as_string();
          const auto id = message.at("id").as_string();
          if (method == "initialize") {
            return agent::json_rpc_success_to_value(id, agent::Value::object({
                {"protocolVersion", agent::MCP_PROTOCOL_VERSION},
                {"capabilities", agent::Value::object({{"tools", agent::Value::object({{"listChanged", false}})}})},
                {"serverInfo", agent::Value::object({{"name", "fixture-server"}, {"version", "1.0.0"}})},
            }));
          }
          if (method == "notifications/initialized") {
            return agent::Value();
          }
          if (method == "tools/list") {
            return agent::json_rpc_success_to_value(id, agent::Value::object({
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
                })},
            }));
          }
          if (method == "tools/call") {
            return agent::json_rpc_success_to_value(id, agent::Value::object({
                {"content", agent::Value::array({
                    agent::Value::object({{"type", "text"},
                                          {"text", "config:" + message.at("params").at("arguments").at("text").as_string()}}),
                })},
                {"structuredContent", agent::Value::object({{"ok", true}})},
            }));
          }
          return agent::json_rpc_error_to_value(id, -32601, "Method not found");
        });
      }));
  SMOKE_CHECK(mcp_factory_called);
  SMOKE_CHECK(mcp_configured_app.mcp_clients.size() == 1);
  bool found_config_mcp_tool = false;
  for (const auto& tool : mcp_configured_app.tools) {
    found_config_mcp_tool = found_config_mcp_tool || tool.name == "fixture.echo";
    SMOKE_CHECK(tool.name != "ignored.echo");
  }
  SMOKE_CHECK(found_config_mcp_tool);
  agent::ToolRegistry config_mcp_registry(mcp_configured_app.tools);
  agent::ToolExecutor config_mcp_executor(config_mcp_registry);
  auto config_mcp_result = config_mcp_executor.execute_tool_call(agent::ToolCall{
      .id = "config_mcp_1",
      .name = "fixture.echo",
      .arguments = agent::Value::object({{"text", "hello"}}),
  });
  SMOKE_CHECK(config_mcp_result.ok);
  SMOKE_CHECK(agent::extract_text_content(config_mcp_result.message.content) == "config:hello");
#ifndef _WIN32
  const auto native_mcp_dir = persistent_dir / "agent-mcp-native";
  std::filesystem::create_directories(native_mcp_dir);
  agent::write_json_file(native_mcp_dir / ".mcp.json", agent::Value::object({
      {"mcpServers", agent::Value::object({
          {"native", agent::Value::object({
              {"command", smoke_executable.string()},
              {"args", agent::Value::array({"--mcp-fixture-server"})},
              {"env", agent::Value::object({{"MCP_FIXTURE_PREFIX", "native-config"}})},
          })},
      })},
  }));
  auto native_config_with_mcp = agent::Value::object({
      {"_cwd", native_mcp_dir.string()},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"mcp", agent::Value::object({
                  {"anthropic", agent::Value::object({
                      {"servers", agent::Value::array({"native"})},
                  })},
              })},
          })},
      })},
  });
  auto native_mcp_app = agent::resolve_native_agent_app(native_config_with_mcp, "main");
  SMOKE_CHECK(native_mcp_app.mcp_clients.size() == 1);
  agent::ToolRegistry native_mcp_registry(native_mcp_app.tools);
  agent::ToolExecutor native_mcp_executor(native_mcp_registry);
  auto native_mcp_result = native_mcp_executor.execute_tool_call(agent::ToolCall{
      .id = "native_mcp_1",
      .name = "native.echo",
      .arguments = agent::Value::object({{"text", "hello"}}),
  });
  SMOKE_CHECK(native_mcp_result.ok);
  SMOKE_CHECK(agent::extract_text_content(native_mcp_result.message.content) == "native-config:hello");
  for (const auto& client : native_mcp_app.mcp_clients) {
    client->close();
  }
#endif

  const auto media_path = tmp_dir / "media.txt";
  {
    std::ofstream media_file(media_path);
    media_file << "media text";
  }
  agent::DefaultMediaResolver media_resolver;
  agent::MediaSource inline_source;
  inline_source.kind = agent::MediaSourceKind::Inline;
  inline_source.data = "SGVsbG8=";
  inline_source.mime_type = "text/plain";
  auto inline_media = media_resolver.resolve(inline_source);
  SMOKE_CHECK(inline_media.text == "Hello");
  agent::MediaSource path_source;
  path_source.kind = agent::MediaSourceKind::Path;
  path_source.path = media_path.string();
  auto path_media = media_resolver.resolve(path_source);
  SMOKE_CHECK(path_media.text == "media text");
  agent::MediaSource artifact_source;
  artifact_source.kind = agent::MediaSourceKind::Artifact;
  artifact_source.key = "artifact-media";
  auto artifact_media = media_resolver.resolve(artifact_source, [](const std::string&) {
    return agent::Value::object({{"mimeType", "text/plain"}, {"text", "artifact text"}});
  });
  SMOKE_CHECK(artifact_media.text == "artifact text");

  agent::OCRProviderRegistry ocr_registry;
  auto fixture_ocr = ocr_registry.register_provider(std::make_shared<FixtureOCRProvider>());
  SMOKE_CHECK(agent::require_ocr_provider(&ocr_registry, "fixture-ocr") == fixture_ocr);
  agent::DocumentRasterizerRegistry rasterizer_registry;
  auto fixture_rasterizer = rasterizer_registry.register_rasterizer(std::make_shared<FixturePdfRasterizer>());
  SMOKE_CHECK(fixture_rasterizer->supports(agent::ResolvedMedia{
      .mime_type = "application/pdf",
  }));

  agent::DocumentPreprocessorRegistry document_preprocessors({
      std::make_shared<agent::DefaultDocumentPreprocessor>(),
  });
  auto text_document = agent::preprocess_document_part(
      &document_preprocessors,
      agent::file_part(path_source, "Media Text"),
      agent::DocumentPreprocessContext{});
  SMOKE_CHECK(text_document.used_native_text);
  SMOKE_CHECK(text_document.extracted_text == "media text");
  SMOKE_CHECK(agent::extract_text_content(text_document.content) == "media text");

  agent::MediaSource image_file_source;
  image_file_source.kind = agent::MediaSourceKind::Inline;
  image_file_source.data = "iVBORw0KGgo=";
  image_file_source.mime_type = "image/png";
  image_file_source.filename = "scan.png";
  agent::DocumentPreprocessContext ocr_context;
  ocr_context.ocr_registry = &ocr_registry;
  ocr_context.default_ocr_provider_name = "fixture-ocr";
  auto image_document = agent::preprocess_document_part(
      &document_preprocessors,
      agent::file_part(image_file_source, "scan", "scan text hint"),
      ocr_context);
  SMOKE_CHECK(image_document.used_ocr);
  SMOKE_CHECK(image_document.ocr_provider == "fixture-ocr");
  SMOKE_CHECK(image_document.content.size() == 2);
  SMOKE_CHECK(agent::extract_text_content(image_document.content) == "ocr page one");

  agent::MediaSource pdf_source;
  pdf_source.kind = agent::MediaSourceKind::Inline;
  pdf_source.data = "JVBERi0xLjQKJSBmaXh0dXJl";
  pdf_source.mime_type = "application/pdf";
  pdf_source.filename = "fixture.pdf";
  agent::DocumentPreprocessContext pdf_context;
  pdf_context.ocr_registry = &ocr_registry;
  pdf_context.default_ocr_provider_name = "fixture-ocr";
  pdf_context.document_rasterizers = &rasterizer_registry;
  pdf_context.ocr_mode = agent::OCRMode::Force;
  auto pdf_document = agent::preprocess_document_part(
      &document_preprocessors,
      agent::file_part(pdf_source, "Fixture PDF"),
      pdf_context);
  SMOKE_CHECK(pdf_document.used_ocr);
  SMOKE_CHECK(pdf_document.rasterizer == "fixture-pdf-rasterizer");
  SMOKE_CHECK(pdf_document.ocr_text == "ocr page one");

  bool missing_preprocessor_failed = false;
  try {
    agent::DocumentPreprocessorRegistry empty_preprocessors;
    (void)agent::preprocess_document_part(
        &empty_preprocessors,
        agent::file_part(path_source),
        agent::DocumentPreprocessContext{});
  } catch (const agent::ConfigurationError&) {
    missing_preprocessor_failed = true;
  }
  SMOKE_CHECK(missing_preprocessor_failed);

  class ConcurrentOCRProvider final : public agent::OCRProvider {
   public:
    ConcurrentOCRProvider(std::string name, std::atomic<int>& calls) : calls_(calls) {
      metadata_.name = std::move(name);
      metadata_.tier = "portable";
      metadata_.title = "Concurrent OCR";
    }

    const agent::OCRProviderMetadata& metadata() const override {
      return metadata_;
    }

    agent::OCRResult recognize(const agent::OCRRequest& request) override {
      SMOKE_CHECK(!request.images.empty());
      calls_.fetch_add(1);
      return agent::OCRResult{.text = metadata_.name};
    }

   private:
    agent::OCRProviderMetadata metadata_;
    std::atomic<int>& calls_;
  };

  class ConcurrentDocumentRasterizer final : public agent::DocumentRasterizer {
   public:
    ConcurrentDocumentRasterizer(std::string name, std::string filename)
        : filename_(std::move(filename)) {
      metadata_.name = std::move(name);
      metadata_.tier = "portable";
    }

    const agent::DocumentRasterizerMetadata& metadata() const override {
      return metadata_;
    }

    bool supports(const agent::ResolvedMedia& document) const override {
      return document.filename == filename_;
    }

    std::vector<agent::RasterizedDocumentPage> rasterize(
        const agent::RasterizeDocumentRequest& request) override {
      agent::MediaSource source;
      source.kind = agent::MediaSourceKind::Inline;
      source.mime_type = "image/png";
      source.filename = "page-" + filename_;
      agent::ResolvedMedia page;
      page.source = source;
      page.mime_type = "image/png";
      page.filename = source.filename;
      return {agent::RasterizedDocumentPage{.page_number = 1, .media = page, .source = request.source}};
    }

   private:
    agent::DocumentRasterizerMetadata metadata_;
    std::string filename_;
  };

  class ConcurrentDocumentPreprocessor final : public agent::DocumentPreprocessor {
   public:
    explicit ConcurrentDocumentPreprocessor(std::string filename) : filename_(std::move(filename)) {}

    bool supports(const agent::MessageContentPart& part) const override {
      return part.source.filename == filename_;
    }

    agent::PreprocessedDocument process(const agent::MessageContentPart& part,
                                        const agent::DocumentPreprocessContext&) override {
      return agent::PreprocessedDocument{
          .source = part.source,
          .content = {agent::text_part("processed:" + filename_)},
          .filename = filename_,
          .extracted_text = "processed:" + filename_,
          .used_native_text = true,
      };
    }

   private:
    std::string filename_;
  };

  agent::OCRProviderRegistry concurrent_ocr_registry;
  agent::DocumentRasterizerRegistry concurrent_rasterizer_registry;
  agent::DocumentPreprocessorRegistry concurrent_preprocessor_registry;
  std::atomic<int> concurrent_media_ready{0};
  std::atomic<bool> concurrent_media_start{false};
  std::atomic<int> concurrent_ocr_calls{0};
  constexpr int concurrent_media_thread_count = 8;
  std::vector<std::thread> concurrent_media_workers;
  for (int thread_index = 0; thread_index < concurrent_media_thread_count; ++thread_index) {
    concurrent_media_workers.emplace_back([&, thread_index]() {
      concurrent_media_ready.fetch_add(1);
      while (!concurrent_media_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(thread_index);
      const auto ocr_name = "ocr.concurrent." + suffix;
      const auto document_name = "document-" + suffix + ".pdf";
      concurrent_ocr_registry.register_provider(
          std::make_shared<ConcurrentOCRProvider>(ocr_name, concurrent_ocr_calls));
      concurrent_rasterizer_registry.register_rasterizer(
          std::make_shared<ConcurrentDocumentRasterizer>("raster.concurrent." + suffix, document_name));
      concurrent_preprocessor_registry.register_preprocessor(
          std::make_shared<ConcurrentDocumentPreprocessor>(document_name));

      auto ocr_provider = agent::require_ocr_provider(&concurrent_ocr_registry, ocr_name);
      auto ocr_result = ocr_provider->recognize(agent::OCRRequest{
          .images = {agent::OCRRequestImage{.media = agent::ResolvedMedia{.filename = "scan-" + suffix + ".png"}}},
      });
      SMOKE_CHECK(ocr_result.text == ocr_name);

      agent::ResolvedMedia concurrent_document;
      concurrent_document.mime_type = "application/pdf";
      concurrent_document.filename = document_name;
      auto rasterizer = concurrent_rasterizer_registry.get(concurrent_document);
      SMOKE_CHECK(rasterizer);
      SMOKE_CHECK(rasterizer->metadata().name == "raster.concurrent." + suffix);

      agent::MediaSource concurrent_source;
      concurrent_source.kind = agent::MediaSourceKind::Inline;
      concurrent_source.mime_type = "application/pdf";
      concurrent_source.filename = document_name;
      auto preprocessed = agent::preprocess_document_part(
          &concurrent_preprocessor_registry,
          agent::file_part(concurrent_source),
          agent::DocumentPreprocessContext{});
      SMOKE_CHECK(preprocessed.extracted_text == "processed:" + document_name);
    });
  }
  while (concurrent_media_ready.load() < concurrent_media_thread_count) {
    std::this_thread::yield();
  }
  concurrent_media_start.store(true);
  for (auto& worker : concurrent_media_workers) {
    worker.join();
  }
  SMOKE_CHECK(concurrent_ocr_calls.load() == concurrent_media_thread_count);

  agent::StaticWebSearchProvider search_provider("static");
  search_provider.add_result(agent::WebSearchResult{
      .title = "Native Agent Docs",
      .url = "https://example.test/native-agent",
      .snippet = "Pure C++ native agent framework",
      .source = "fixture",
  });
  search_provider.add_result(agent::WebSearchResult{
      .title = "Subdomain Agent Docs",
      .url = "https://docs.example.test/native-agent",
      .snippet = "Host filtered agent docs",
      .source = "fixture",
  });
  search_provider.add_result(agent::WebSearchResult{
      .title = "Spoofed Agent Docs",
      .url = "https://docs.example.test.evil.test/native-agent",
      .snippet = "Should not match domain filter",
      .source = "fixture",
  });
  auto search_results = search_provider.search(agent::WebSearchQuery{.query = "pure framework", .top_k = 3});
  SMOKE_CHECK(search_results.size() == 1);
  auto static_zero_results = search_provider.search(agent::WebSearchQuery{.query = "agent", .top_k = 0});
  SMOKE_CHECK(static_zero_results.empty());
  agent::WebSearchQuery static_domain_query;
  static_domain_query.query = "agent docs";
  static_domain_query.top_k = 5;
  static_domain_query.domains = {"example.test"};
  auto static_domain_results = search_provider.search(static_domain_query);
  SMOKE_CHECK(static_domain_results.size() == 2);
  SMOKE_CHECK(static_domain_results[0].url == "https://example.test/native-agent");
  SMOKE_CHECK(static_domain_results[1].url == "https://docs.example.test/native-agent");
  agent::NativeWebSearchRequest captured_brave_request;
  auto brave_provider = agent::create_brave_web_search_provider(
      [&](const agent::NativeWebSearchRequest& request) {
        captured_brave_request = request;
        return agent::Value::object({{"web",
                                      agent::Value::object({{"results",
                                                            agent::Value::array({
                                                                agent::Value::object({
                                                                    {"title", "Other"},
                                                                    {"url", "https://other.example.com/post"},
                                                                    {"description", "ignored"},
                                                                }),
                                                                agent::Value::object({
                                                                    {"title", "Docs"},
                                                                    {"url", "https://docs.example.com/agent"},
                                                                    {"description", "agent docs"},
                                                                }),
                                                            })}})}});
      },
      "brave-key",
      "https://brave.example.test");
  agent::WebSearchQuery brave_query;
  brave_query.query = "agent";
  brave_query.locale = "en";
  brave_query.recency_days = 3;
  brave_query.domains = {"docs.example.com"};
  brave_query.safe_search = "strict";
  auto brave_results = brave_provider.search(brave_query);
  SMOKE_CHECK(brave_results.size() == 1);
  SMOKE_CHECK(brave_results.front().title == "Docs");
  SMOKE_CHECK(brave_results.front().rank == 2);
  SMOKE_CHECK(captured_brave_request.query_params.at("count").as_integer() == 8);
  SMOKE_CHECK(captured_brave_request.query_params.at("search_lang").as_string() == "en");
  SMOKE_CHECK(captured_brave_request.query_params.at("freshness").as_string() == "3d");
  SMOKE_CHECK(captured_brave_request.query_params.at("safesearch").as_string() == "strict");
  SMOKE_CHECK(!captured_brave_request.query_params.contains("domains"));
  SMOKE_CHECK(!captured_brave_request.query_params.contains("safeSearch"));
  SMOKE_CHECK(!captured_brave_request.query_params.contains("recencyDays"));
  SMOKE_CHECK(!captured_brave_request.query_params.contains("locale"));
  agent::WebSearchQuery brave_zero_query;
  brave_zero_query.query = "agent";
  brave_zero_query.top_k = 0;
  auto brave_zero_results = brave_provider.search(brave_zero_query);
  SMOKE_CHECK(brave_zero_results.empty());
  SMOKE_CHECK(captured_brave_request.query_params.at("count").as_integer() == 0);
  bool missing_brave_key_failed = false;
  auto missing_key_brave_provider = agent::create_brave_web_search_provider(
      [](const agent::NativeWebSearchRequest&) -> agent::Value {
        SMOKE_CHECK(false && "Brave search must not call transport without an API key.");
        return agent::Value::object({});
      },
      "",
      "https://brave.example.test");
  try {
    (void)missing_key_brave_provider.search(agent::WebSearchQuery{.query = "agent"});
  } catch (const agent::ConfigurationError& error) {
    missing_brave_key_failed =
        std::string(error.what()).find("apiKey or BRAVE_SEARCH_API_KEY") != std::string::npos;
  }
  SMOKE_CHECK(missing_brave_key_failed);

  agent::NativeWebSearchRequest captured_searxng_request;
  auto searxng_provider = agent::create_searxng_web_search_provider(
      [&](const agent::NativeWebSearchRequest& request) {
        captured_searxng_request = request;
        return agent::Value::object({{"results",
                                      agent::Value::array({
                                          agent::Value::object({
                                              {"title", "Other"},
                                              {"url", "https://other.example.com/post"},
                                              {"content", "ignored"},
                                              {"engine", "duckduckgo"},
                                          }),
                                          agent::Value::object({
                                              {"title", "Docs One"},
                                              {"url", "https://docs.example.com/one"},
                                              {"content", "one"},
                                              {"engine", "google"},
                                          }),
                                          agent::Value::object({
                                              {"title", "Docs Two"},
                                              {"url", "https://docs.example.com/two"},
                                              {"content", "two"},
                                              {"engine", "google"},
                                          }),
                                      })}});
      },
      "https://search.example.test");
  agent::WebSearchQuery searxng_query;
  searxng_query.query = "agent";
  searxng_query.top_k = 1;
  searxng_query.locale = "en";
  searxng_query.recency_days = 7;
  searxng_query.domains = {"docs.example.com"};
  searxng_query.safe_search = "off";
  auto searxng_results = searxng_provider.search(searxng_query);
  SMOKE_CHECK(searxng_results.size() == 1);
  SMOKE_CHECK(searxng_results.front().title == "Docs One");
  SMOKE_CHECK(searxng_results.front().rank == 2);
  SMOKE_CHECK(captured_searxng_request.query_params.at("format").as_string() == "json");
  SMOKE_CHECK(captured_searxng_request.query_params.at("language").as_string() == "en");
  SMOKE_CHECK(captured_searxng_request.query_params.at("time_range").as_string() == "week");
  SMOKE_CHECK(captured_searxng_request.query_params.at("safesearch").as_string() == "0");
  SMOKE_CHECK(!captured_searxng_request.query_params.contains("count"));
  SMOKE_CHECK(!captured_searxng_request.query_params.contains("domains"));
  SMOKE_CHECK(!captured_searxng_request.query_params.contains("safeSearch"));
  SMOKE_CHECK(!captured_searxng_request.query_params.contains("recencyDays"));
  agent::WebSearchQuery searxng_zero_query;
  searxng_zero_query.query = "agent";
  searxng_zero_query.top_k = 0;
  auto searxng_zero_results = searxng_provider.search(searxng_zero_query);
  SMOKE_CHECK(searxng_zero_results.empty());
  agent::NativeWebPageFetcher fetcher;
  fetcher.register_page(agent::WebFetchedPage{
      .url = "fixture://page",
      .content_type = "text/html",
      .html = "<html><title>Fixture</title><body>Fetched text</body></html>",
  });
  auto page = fetcher.fetch(agent::WebFetchRequest{.url = "fixture://page"});
  SMOKE_CHECK(page.text.find("Fetched text") != std::string::npos);

  agent::NativeWebPageFetcher concurrent_fetcher;
  agent::NativeWebCrawler concurrent_crawler(&concurrent_fetcher);
  std::atomic<int> concurrent_fetch_ready{0};
  std::atomic<bool> concurrent_fetch_start{false};
  constexpr int concurrent_fetch_thread_count = 8;
  std::vector<std::thread> concurrent_fetch_workers;
  for (int thread_index = 0; thread_index < concurrent_fetch_thread_count; ++thread_index) {
    concurrent_fetch_workers.emplace_back([&, thread_index]() {
      concurrent_fetch_ready.fetch_add(1);
      while (!concurrent_fetch_start.load()) {
        std::this_thread::yield();
      }
      const auto suffix = std::to_string(thread_index);
      const auto root_url = "https://concurrent-fetch.example.test/page-" + suffix;
      const auto child_url = "https://concurrent-fetch.example.test/child-" + suffix;
      concurrent_fetcher.register_page(agent::WebFetchedPage{
          .url = root_url,
          .content_type = "text/html",
          .title = "Concurrent " + suffix,
          .html = "<html><body>root " + suffix + " <a href=\"/child-" + suffix + "\">child</a></body></html>",
      });
      concurrent_fetcher.register_page(agent::WebFetchedPage{
          .url = child_url,
          .content_type = "text/html",
          .title = "Child " + suffix,
          .html = "<html><body>child " + suffix + "</body></html>",
      });
      auto fetched = concurrent_fetcher.fetch(agent::WebFetchRequest{.url = root_url});
      SMOKE_CHECK(fetched.text.find("root " + suffix) != std::string::npos);
      auto crawl = concurrent_crawler.crawl(agent::WebCrawlRequest{
          .url = root_url,
          .max_depth = 1,
          .max_pages = 2,
          .allowed_domains = {"concurrent-fetch.example.test"},
      });
      SMOKE_CHECK(crawl.pages.size() >= 1);
      SMOKE_CHECK(crawl.discovered_urls.front() == root_url);
      SMOKE_CHECK(std::any_of(crawl.discovered_urls.begin(), crawl.discovered_urls.end(), [&](const std::string& url) {
        return url == child_url;
      }));
    });
  }
  while (concurrent_fetch_ready.load() < concurrent_fetch_thread_count) {
    std::this_thread::yield();
  }
  concurrent_fetch_start.store(true);
  for (auto& worker : concurrent_fetch_workers) {
    worker.join();
  }

  bool fallback_browser_called = false;
  auto fallback_primary = std::make_shared<agent::NativeWebPageFetcher>();
  fallback_primary->register_page(agent::WebFetchedPage{
      .url = "https://fallback.example.test/page",
      .final_url = "https://fallback.example.test/page",
      .status = 200,
      .content_type = "text/html",
      .title = "Short",
      .text = "short",
      .markdown = "short",
  });
  auto fallback_browser = std::make_shared<agent::BrowserCallbackRenderer>(
      [&](const agent::BrowserRenderRequest& request) {
        fallback_browser_called = true;
        SMOKE_CHECK(request.url == "https://fallback.example.test/page");
        SMOKE_CHECK(request.selector == "main");
        SMOKE_CHECK(request.wait_until == "load");
        agent::BrowserRenderResult render_result;
        render_result.url = "https://fallback.example.test/rendered";
        render_result.title = "Rendered";
        render_result.html = "<html><body>rendered content</body></html>";
        render_result.text = "rendered content from browser fallback";
        return render_result;
      });
  agent::BrowserRenderRequest fallback_browser_request;
  fallback_browser_request.selector = "main";
  fallback_browser_request.wait_until = "load";
  agent::BrowserBackedWebPageFetcher fallback_fetcher(agent::BrowserBackedWebPageFetcherConfig{
      .fetcher = fallback_primary,
      .browser = fallback_browser,
      .minimum_text_length = 50,
      .browser_request = fallback_browser_request,
  });
  auto fallback_page = fallback_fetcher.fetch(agent::WebFetchRequest{.url = "https://fallback.example.test/page"});
  SMOKE_CHECK(fallback_browser_called);
  SMOKE_CHECK(fallback_page.final_url == "https://fallback.example.test/rendered");
  SMOKE_CHECK(fallback_page.title == "Rendered");
  SMOKE_CHECK(fallback_page.text.find("browser fallback") != std::string::npos);

  bool enough_browser_called = false;
  auto enough_primary = std::make_shared<agent::NativeWebPageFetcher>();
  enough_primary->register_page(agent::WebFetchedPage{
      .url = "https://fallback.example.test/enough",
      .final_url = "https://fallback.example.test/enough",
      .status = 200,
      .content_type = "text/html",
      .title = "Enough",
      .text = "this content is long enough",
      .markdown = "this content is long enough",
  });
  auto enough_browser = std::make_shared<agent::BrowserCallbackRenderer>(
      [&](const agent::BrowserRenderRequest&) {
        enough_browser_called = true;
        return agent::BrowserRenderResult{.url = "https://fallback.example.test/enough"};
      });
  agent::BrowserBackedWebPageFetcher enough_fetcher(agent::BrowserBackedWebPageFetcherConfig{
      .fetcher = enough_primary,
      .browser = enough_browser,
      .minimum_text_length = 10,
  });
  auto enough_page = enough_fetcher.fetch(agent::WebFetchRequest{.url = "https://fallback.example.test/enough"});
  SMOKE_CHECK(!enough_browser_called);
  SMOKE_CHECK(enough_page.title == "Enough");

  agent::StaticWebSearchProvider tool_search_provider("fixture-search");
  tool_search_provider.add_result(agent::WebSearchResult{
      .title = "Workflow Native Docs",
      .url = "https://docs.example.test/workflow",
      .snippet = "Workflow and tool documentation",
      .source = "fixture",
  });
  agent::CancellationToken web_tool_cancellation;
  agent::StaticWebSearchProvider cancellation_search_provider(
      "fixture-cancel",
      [&](const agent::WebSearchQuery& query) {
        SMOKE_CHECK(query.cancellation == &web_tool_cancellation);
        return std::vector<agent::WebSearchResult>{agent::WebSearchResult{
            .title = "Cancellable Search",
            .url = "https://docs.example.test/cancel",
            .rank = 1,
        }};
      });
  agent::WebSearchProviderRegistry web_registry({tool_search_provider, cancellation_search_provider});
  agent::NativeWebPageFetcher tool_fetcher;
  tool_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://docs.example.test/",
      .content_type = "text/html",
      .title = "Docs Home",
      .html = "<html><body>Home <a href=\"/workflow\">Workflow</a></body></html>",
  });
  tool_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://docs.example.test/workflow",
      .content_type = "text/html",
      .title = "Workflow",
      .html = "<html><body>Workflow page</body></html>",
  });
  agent::NativeWebCrawler web_crawler(&tool_fetcher);
  auto web_tools = agent::create_web_builtin_tools(&web_registry, "fixture-search", &tool_fetcher);
  agent::ToolRegistry web_tool_registry(web_tools);
  agent::ToolExecutor web_tool_executor(web_tool_registry);
  agent::ToolExecutionContext web_context;
  web_context.cancellation = &web_tool_cancellation;
  auto web_search_result = web_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "web_search_1",
      .name = "web.search",
      .arguments = agent::Value::object({{"query", "workflow"}, {"topK", 2}}),
  });
  SMOKE_CHECK(web_search_result.ok);
  SMOKE_CHECK(web_search_result.output.find("fixture-search") != std::string::npos);
  SMOKE_CHECK(web_search_result.output.find("Workflow Native Docs") != std::string::npos);
  auto web_search_cancellation_result = web_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "web_search_cancel_1",
      .name = "web.search",
      .arguments = agent::Value::object({{"query", "workflow"}, {"provider", "fixture-cancel"}}),
  }, web_context);
  SMOKE_CHECK(web_search_cancellation_result.ok);
  SMOKE_CHECK(web_search_cancellation_result.output.find("Cancellable Search") != std::string::npos);
  auto web_search_zero_result = web_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "web_search_zero_1",
      .name = "web.search",
      .arguments = agent::Value::object({{"query", "workflow"}, {"topK", 0}}),
  });
  SMOKE_CHECK(web_search_zero_result.ok);
  SMOKE_CHECK(agent::parse_json(web_search_zero_result.output).at("results").as_array().empty());
  auto web_fetch_result = web_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "web_fetch_1",
      .name = "web.fetch",
      .arguments = agent::Value::object({{"url", "https://docs.example.test/workflow"}, {"extract", "text"}}),
  });
  SMOKE_CHECK(web_fetch_result.ok);
  SMOKE_CHECK(web_fetch_result.output.find("Workflow page") != std::string::npos);
  int web_fetch_default_timeout = -1;
  agent::NativeWebPageFetcher timeout_fetcher([&](const agent::NativeWebFetchTransportRequest& request) {
    web_fetch_default_timeout = request.request.timeout_ms;
    SMOKE_CHECK(request.request.cancellation == &web_tool_cancellation);
    return agent::WebFetchedPage{
        .url = request.request.url,
        .final_url = request.request.url,
        .status = 200,
        .content_type = "text/html",
        .title = "Timeout Default",
        .html = "<html><body>timeout default</body></html>",
    };
  });
  auto timeout_web_tools = agent::create_web_builtin_tools(nullptr, {}, &timeout_fetcher);
  agent::ToolRegistry timeout_web_registry(timeout_web_tools);
  agent::ToolExecutor timeout_web_executor(timeout_web_registry);
  auto timeout_web_fetch = timeout_web_executor.execute_tool_call(agent::ToolCall{
      .id = "web_fetch_timeout_default_1",
      .name = "web.fetch",
      .arguments = agent::Value::object({{"url", "https://timeout.example.test/"}}),
  }, web_context);
  SMOKE_CHECK(timeout_web_fetch.ok);
  SMOKE_CHECK(web_fetch_default_timeout == 15000);
  agent::NativeWebPageFetcher extraction_fetcher(agent::create_native_web_fetch_transport(
      [](const agent::HttpRequest& request) -> agent::HttpResponse {
        if (request.url.find("/html") != std::string::npos) {
          return agent::HttpResponse{
              .status = 200,
              .headers = {{"content-type", "text/html; charset=utf-8"}},
              .body = "<html><body>Hello <strong>native</strong> fetch</body></html>",
              .final_url = request.url,
          };
        }
        return agent::HttpResponse{
            .status = 200,
            .headers = {{"content-type", "text/plain"}},
            .body = "plain native fetch",
            .final_url = request.url,
        };
      }));
  const auto html_text_page =
      extraction_fetcher.fetch(agent::WebFetchRequest{.url = "https://extract.example.test/html",
                                                      .extract = "text"});
  SMOKE_CHECK(html_text_page.text.find("Hello") != std::string::npos);
  SMOKE_CHECK(html_text_page.markdown.find("Hello") != std::string::npos);
  SMOKE_CHECK(!html_text_page.html.empty());
  const auto html_markdown_page =
      extraction_fetcher.fetch(agent::WebFetchRequest{.url = "https://extract.example.test/html",
                                                      .extract = "markdown"});
  SMOKE_CHECK(html_markdown_page.text.empty());
  SMOKE_CHECK(html_markdown_page.markdown.find("Hello") != std::string::npos);
  SMOKE_CHECK(!html_markdown_page.html.empty());
  const auto plain_markdown_page =
      extraction_fetcher.fetch(agent::WebFetchRequest{.url = "https://extract.example.test/plain",
                                                      .extract = "markdown"});
  SMOKE_CHECK(plain_markdown_page.text.empty());
  SMOKE_CHECK(plain_markdown_page.markdown == "plain native fetch");
  SMOKE_CHECK(plain_markdown_page.html.empty());
  const auto plain_html_page =
      extraction_fetcher.fetch(agent::WebFetchRequest{.url = "https://extract.example.test/plain",
                                                      .extract = "html"});
  SMOKE_CHECK(plain_html_page.text.empty());
  SMOKE_CHECK(plain_html_page.markdown.empty());
  SMOKE_CHECK(plain_html_page.html.empty());
  // Exercise the NativeWebCrawler API directly.
  auto direct_web_crawl = web_crawler.crawl(agent::WebCrawlRequest{
      .url = "https://docs.example.test/",
      .max_depth = 1,
      .max_pages = 4,
      .allowed_domains = {"docs.example.test"},
  });
  bool saw_workflow_page = false;
  for (const auto& crawled_page : direct_web_crawl.pages) {
    saw_workflow_page = saw_workflow_page || crawled_page.page.url == "https://docs.example.test/workflow";
  }
  SMOKE_CHECK(saw_workflow_page);
  agent::NativeWebPageFetcher default_limit_fetcher;
  std::string default_limit_links;
  for (int index = 0; index < 12; ++index) {
    default_limit_links += "<a href=\"/page-" + std::to_string(index) + "\">Page</a>";
    default_limit_fetcher.register_page(agent::WebFetchedPage{
        .url = "https://limit.example.test/page-" + std::to_string(index),
        .content_type = "text/html",
        .title = "Limit Page " + std::to_string(index),
        .html = "<html><body>limit page</body></html>",
    });
  }
  default_limit_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://limit.example.test/",
      .content_type = "text/html",
      .title = "Limit Home",
      .html = "<html><body>" + default_limit_links + "</body></html>",
  });
  agent::NativeWebCrawler default_limit_crawler(&default_limit_fetcher);
  auto default_limit_crawl = default_limit_crawler.crawl(agent::WebCrawlRequest{
      .url = "https://limit.example.test/",
      .max_depth = 1,
  });
  SMOKE_CHECK(default_limit_crawl.pages.size() == 10);
  auto direct_zero_limit_crawl = default_limit_crawler.crawl(agent::WebCrawlRequest{
      .url = "https://limit.example.test/",
      .max_depth = 1,
      .max_pages = 0,
  });
  SMOKE_CHECK(direct_zero_limit_crawl.pages.empty());

  agent::NativeWebPageFetcher policy_fetcher;
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/robots.txt",
      .content_type = "text/plain",
      .text = "User-agent: *\nDisallow: /blocked\n",
  });
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/sitemap.xml",
      .content_type = "application/xml",
      .text = "<urlset><url><loc>https://crawl.example.test/from-sitemap</loc></url></urlset>",
  });
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/",
      .content_type = "text/html",
      .title = "Crawl Home",
      .html = "<html><body><a href=\"/allowed\">Allowed</a><a href=\"/blocked/page\">Blocked</a>"
              "<a href=\"https://external.example.test/page\">External</a></body></html>",
  });
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/allowed",
      .content_type = "text/html",
      .title = "Allowed",
      .html = "<html><body>allowed page</body></html>",
  });
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/from-sitemap",
      .content_type = "text/html",
      .title = "Sitemap Page",
      .html = "<html><body>sitemap page</body></html>",
  });
  policy_fetcher.register_page(agent::WebFetchedPage{
      .url = "https://crawl.example.test/blocked/page",
      .content_type = "text/html",
      .title = "Blocked",
      .html = "<html><body>blocked page should not be fetched</body></html>",
  });
  agent::NativeWebCrawler policy_crawler(&policy_fetcher);
  auto policy_crawl = policy_crawler.crawl(agent::WebCrawlRequest{
      .url = "https://crawl.example.test/",
      .max_depth = 1,
      .max_pages = 5,
      .allowed_domains = {"crawl.example.test"},
  });
  SMOKE_CHECK(std::find(policy_crawl.blocked_urls.begin(), policy_crawl.blocked_urls.end(),
                   "https://crawl.example.test/blocked/page") != policy_crawl.blocked_urls.end());
  SMOKE_CHECK(std::find(policy_crawl.blocked_urls.begin(), policy_crawl.blocked_urls.end(),
                   "https://external.example.test/page") != policy_crawl.blocked_urls.end());
  SMOKE_CHECK(std::find(policy_crawl.discovered_urls.begin(), policy_crawl.discovered_urls.end(),
                   "https://crawl.example.test/from-sitemap") != policy_crawl.discovered_urls.end());
  bool saw_sitemap_page = false;
  bool saw_blocked_page = false;
  bool saw_external_page = false;
  bool saw_absolute_allowed_link = false;
  bool saw_raw_allowed_link = false;
  for (const auto& crawled_page : policy_crawl.pages) {
    saw_sitemap_page = saw_sitemap_page || crawled_page.page.url == "https://crawl.example.test/from-sitemap";
    saw_blocked_page = saw_blocked_page || crawled_page.page.url == "https://crawl.example.test/blocked/page";
    saw_external_page = saw_external_page || crawled_page.page.url == "https://external.example.test/page";
    saw_absolute_allowed_link =
        saw_absolute_allowed_link ||
        std::find(crawled_page.links.begin(), crawled_page.links.end(),
                  "https://crawl.example.test/allowed") != crawled_page.links.end();
    saw_raw_allowed_link =
        saw_raw_allowed_link ||
        std::find(crawled_page.links.begin(), crawled_page.links.end(), "/allowed") != crawled_page.links.end();
  }
  SMOKE_CHECK(saw_sitemap_page);
  SMOKE_CHECK(!saw_blocked_page);
  SMOKE_CHECK(!saw_external_page);

  // Reasoning-token usage extraction across providers, plus per-run aggregation.
  {
    // (1) OpenAI non-stream: completion_tokens_details.reasoning_tokens.
    {
      auto raw = agent::Value::object({
          {"usage", agent::Value::object({
              {"prompt_tokens", 100},
              {"completion_tokens", 5200},
              {"completion_tokens_details", agent::Value::object({
                  {"reasoning_tokens", 4800},
              })},
          })},
      });
      const auto usage = agent::extract_model_usage(raw, "openai");
      SMOKE_CHECK(usage.reasoning_tokens == 4800);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Provider);
      SMOKE_CHECK(usage.output_tokens == 5200);
      SMOKE_CHECK(usage.input_tokens_source == agent::TokenUsageSource::Provider);
      SMOKE_CHECK(usage.output_tokens_source == agent::TokenUsageSource::Provider);
      SMOKE_CHECK(usage.total_tokens_source == agent::TokenUsageSource::Derived);
      SMOKE_CHECK(usage.quality == agent::TokenUsageQuality::Provider);
    }

    // (2) OpenAI stream: terminal chunk usage block propagates through stream parser.
    {
      std::vector<std::string> chunks;
      chunks.push_back("data: " + agent::Value::object({
          {"id", "resp_stream_reasoning"},
          {"model", "gpt-stream"},
          {"choices", agent::Value::array({
              agent::Value::object({
                  {"index", 0},
                  {"delta", agent::Value::object({{"content", "hi"}})},
                  {"finish_reason", agent::Value()},
              }),
          })},
      }).stringify() + "\n\n");
      chunks.push_back("data: " + agent::Value::object({
          {"id", "resp_stream_reasoning"},
          {"model", "gpt-stream"},
          {"choices", agent::Value::array({})},
          {"usage", agent::Value::object({
              {"prompt_tokens", 12},
              {"completion_tokens", 110},
              {"completion_tokens_details", agent::Value::object({
                  {"reasoning_tokens", 100},
              })},
          })},
      }).stringify() + "\n\n");
      chunks.push_back("data: [DONE]\n\n");
      const auto events = agent::parse_openai_chat_stream_events(chunks, "openai", "gpt-stream");
      const auto& response_event = events.back();
      SMOKE_CHECK(response_event.type == agent::ModelStreamEventType::Response);
      const auto usage = agent::extract_model_usage(response_event.response);
      SMOKE_CHECK(usage.reasoning_tokens == 100);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Provider);
      SMOKE_CHECK(usage.input_tokens == 12);
      SMOKE_CHECK(usage.total_tokens_source == agent::TokenUsageSource::Derived);
      SMOKE_CHECK(usage.quality == agent::TokenUsageQuality::Provider);
    }

    // (2b) OpenAI stream request opts into include_usage.
    {
      agent::GenerateParams params;
      params.messages.push_back(agent::create_message(agent::MessageRole::User, "hi"));
      const auto request = agent::build_openai_chat_stream_request(params, "gpt-4o");
      SMOKE_CHECK(request.body.at("stream").as_bool() == true);
      SMOKE_CHECK(request.body.at("stream_options").at("include_usage").as_bool() == true);
    }

    // (2c) Ollama JSON-line streaming can emit tool_calls before the final done chunk.
    {
      agent::JsonSchema read_schema;
      read_schema.type = agent::JsonSchemaType::Object;
      read_schema.required = {"path"};
      read_schema.properties["path"].type = agent::JsonSchemaType::String;

      agent::OllamaChatModelAdapter adapter(
          "qwen-test",
          [](const agent::NativeProviderRequest&) {
            return agent::Value::object({});
          },
          "http://127.0.0.1:11434",
          agent::NativeProviderStreamTransport{},
          [](const agent::NativeProviderRequest& request,
             agent::NativeProviderStreamingChunkHandler on_chunk) {
            SMOKE_CHECK(request.body.at("stream").as_bool() == true);
            const std::string tool_line = agent::Value::object({
                {"model", "qwen-test"},
                {"created_at", "2026-06-07T00:00:00Z"},
                {"message", agent::Value::object({
                    {"role", "assistant"},
                    {"content", ""},
                    {"tool_calls", agent::Value::array({
                        agent::Value::object({
                            {"id", "call_ollama_read"},
                            {"function", agent::Value::object({
                                {"index", 0},
                                {"name", "fs.readText"},
                                {"arguments", agent::Value::object({
                                    {"path", "/private/tmp/agent_react_instability_input.txt"},
                                })},
                            })},
                        }),
                    })},
                })},
                {"done", false},
            }).stringify() + "\n";
            const std::string done_line = agent::Value::object({
                {"model", "qwen-test"},
                {"created_at", "2026-06-07T00:00:01Z"},
                {"message", agent::Value::object({
                    {"role", "assistant"},
                    {"content", ""},
                })},
                {"done", true},
                {"done_reason", "stop"},
            }).stringify() + "\n";
            on_chunk(std::string_view(tool_line).substr(0, 13));
            on_chunk(std::string_view(tool_line).substr(13));
            on_chunk(done_line);
          });

      agent::GenerateParams params;
      params.messages.push_back(agent::create_message(agent::MessageRole::User, "read file"));
      params.tools.push_back(agent::ChatToolDescriptor{
          .name = "fs.readText",
          .description = "Read a text file.",
          .input_schema = read_schema,
      });

      std::vector<agent::ModelStreamEvent> events;
      adapter.stream(params, [&](const agent::ModelStreamEvent& event) {
        events.push_back(event);
      });
      bool saw_tool_delta = false;
      const agent::ModelStreamEvent* response_event = nullptr;
      for (const auto& event : events) {
        if (event.type == agent::ModelStreamEventType::ToolCallDelta) {
          saw_tool_delta = true;
          SMOKE_CHECK(event.tool_call_name == "fs.readText");
          SMOKE_CHECK(event.tool_call_args_accumulated.find("agent_react_instability_input.txt") != std::string::npos);
        }
        if (event.type == agent::ModelStreamEventType::Response) {
          response_event = &event;
        }
      }
      SMOKE_CHECK(saw_tool_delta);
      SMOKE_CHECK(response_event != nullptr);
      SMOKE_CHECK(response_event->response.finish_reason == "tool_calls");
      SMOKE_CHECK(response_event->response.tool_calls.size() == 1);
      SMOKE_CHECK(response_event->response.tool_calls.front().id == "fs.readText_1");
      SMOKE_CHECK(response_event->response.tool_calls.front().name == "fs.readText");
      SMOKE_CHECK(response_event->response.tool_calls.front().arguments.at("path").as_string() ==
                  "/private/tmp/agent_react_instability_input.txt");
      SMOKE_CHECK(response_event->response.raw.at("chunks").as_array().size() == 2);
    }

    // (2d) Local providers: Ollama native counters and LM Studio stats are real usage.
    {
      auto ollama_raw = agent::Value::object({
          {"prompt_eval_count", 27},
          {"eval_count", 9},
      });
      const auto usage = agent::extract_model_usage(ollama_raw, "ollama");
      SMOKE_CHECK(usage.input_tokens == 27);
      SMOKE_CHECK(usage.output_tokens == 9);
      SMOKE_CHECK(usage.total_tokens == 36);
      SMOKE_CHECK(usage.input_tokens_source == agent::TokenUsageSource::Provider);
      SMOKE_CHECK(usage.output_tokens_source == agent::TokenUsageSource::Provider);
      SMOKE_CHECK(usage.total_tokens_source == agent::TokenUsageSource::Derived);
      SMOKE_CHECK(usage.quality == agent::TokenUsageQuality::Provider);
    }

    {
      auto llama_native_raw = agent::Value::object({
          {"prompt_eval_count", 19},
          {"eval_count", 7},
      });
      const auto usage = agent::extract_model_usage(llama_native_raw, "llamacpp-native");
      SMOKE_CHECK(usage.input_tokens == 19);
      SMOKE_CHECK(usage.output_tokens == 7);
      SMOKE_CHECK(usage.total_tokens == 26);
    }

    {
      auto ollama_stream_raw = agent::Value::object({
          {"chunks", agent::Value::array({
              agent::Value::object({
                  {"model", "qwen-test"},
                  {"message", agent::Value::object({{"content", "hi"}})},
                  {"done", false},
              }),
              agent::Value::object({
                  {"model", "qwen-test"},
                  {"message", agent::Value::object({{"content", ""}})},
                  {"done", true},
                  {"prompt_eval_count", 31},
                  {"eval_count", 12},
              }),
          })},
      });
      const auto usage = agent::extract_model_usage(ollama_stream_raw, "ollama");
      SMOKE_CHECK(usage.input_tokens == 31);
      SMOKE_CHECK(usage.output_tokens == 12);
      SMOKE_CHECK(usage.total_tokens == 43);
    }

    {
      auto lmstudio_raw = agent::Value::object({
          {"stats", agent::Value::object({
              {"input_tokens", 646},
              {"total_output_tokens", 586},
              {"reasoning_output_tokens", 17},
          })},
      });
      const auto usage = agent::extract_model_usage(lmstudio_raw, "lmstudio");
      SMOKE_CHECK(usage.input_tokens == 646);
      SMOKE_CHECK(usage.output_tokens == 586);
      SMOKE_CHECK(usage.total_tokens == 1232);
      SMOKE_CHECK(usage.total_tokens_source == agent::TokenUsageSource::Derived);
      SMOKE_CHECK(usage.quality == agent::TokenUsageQuality::Provider);
      SMOKE_CHECK(usage.reasoning_tokens == 17);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Provider);
    }

    // (3) Anthropic with reasoning text → Estimated source, chars/4.
    {
      agent::AgentOutput response;
      response.provider = "anthropic";
      response.reasoning = agent::build_model_reasoning(std::string(400, 'r'), "provider-visible");
      response.raw = agent::Value::object({
          {"usage", agent::Value::object({
              {"input_tokens", 50},
              {"output_tokens", 80},
          })},
      });
      const auto usage = agent::extract_model_usage(response);
      SMOKE_CHECK(usage.reasoning_tokens == 100);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Estimated);
    }

    // (4) Gemini: usage_metadata thoughts add onto candidates for output_tokens.
    {
      auto raw = agent::Value::object({
          {"usage_metadata", agent::Value::object({
              {"prompt_token_count", 30},
              {"candidates_token_count", 200},
              {"thoughts_token_count", 4800},
          })},
      });
      const auto usage = agent::extract_model_usage(raw, "gemini");
      SMOKE_CHECK(usage.reasoning_tokens == 4800);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Provider);
      SMOKE_CHECK(usage.output_tokens == 5000);
    }

    // (5a) DeepSeek with OpenAI-compat reasoning_tokens → Provider.
    {
      auto raw = agent::Value::object({
          {"usage", agent::Value::object({
              {"input_tokens", 40},
              {"output_tokens", 600},
              {"completion_tokens_details", agent::Value::object({
                  {"reasoning_tokens", 555},
              })},
          })},
      });
      const auto usage = agent::extract_model_usage(raw, "deepseek");
      SMOKE_CHECK(usage.reasoning_tokens == 555);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Provider);
    }

    // (5b) DeepSeek with only reasoning_content text → Estimated.
    {
      agent::AgentOutput response;
      response.provider = "deepseek";
      response.reasoning = agent::build_model_reasoning(std::string(800, 'd'), "tagged");
      response.raw = agent::Value::object({
          {"usage", agent::Value::object({
              {"input_tokens", 20},
              {"output_tokens", 300},
          })},
      });
      const auto usage = agent::extract_model_usage(response);
      SMOKE_CHECK(usage.reasoning_tokens == 200);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Estimated);
    }

    // (6) Plain gpt-4o with no reasoning fields → Unknown, 0.
    {
      auto raw = agent::Value::object({
          {"usage", agent::Value::object({
              {"prompt_tokens", 20},
              {"completion_tokens", 30},
          })},
      });
      const auto usage = agent::extract_model_usage(raw, "openai");
      SMOKE_CHECK(usage.reasoning_tokens == 0);
      SMOKE_CHECK(usage.reasoning_source == agent::ReasoningSource::Unknown);
    }

    // (7) merge_model_usage source merge semantics.
    {
      const auto provider_a = agent::ModelUsage{.input_tokens = 10, .output_tokens = 20,
                                                 .reasoning_tokens = 5,
                                                 .reasoning_source = agent::ReasoningSource::Provider};
      const auto provider_b = agent::ModelUsage{.input_tokens = 10, .output_tokens = 20,
                                                 .reasoning_tokens = 5,
                                                 .reasoning_source = agent::ReasoningSource::Provider};
      const auto estimated = agent::ModelUsage{.input_tokens = 0, .output_tokens = 0,
                                                .reasoning_tokens = 3,
                                                .reasoning_source = agent::ReasoningSource::Estimated};
      const auto unknown = agent::ModelUsage{.input_tokens = 5, .output_tokens = 5,
                                              .reasoning_source = agent::ReasoningSource::Unknown};
      const auto merged_pp_e = agent::merge_model_usage({provider_a, provider_b, estimated});
      SMOKE_CHECK(merged_pp_e.reasoning_source == agent::ReasoningSource::Estimated);
      SMOKE_CHECK(merged_pp_e.reasoning_tokens == 13);
      const auto merged_pp_u = agent::merge_model_usage({provider_a, provider_b, unknown});
      SMOKE_CHECK(merged_pp_u.reasoning_source == agent::ReasoningSource::Unknown);
      const auto merged_empty = agent::merge_model_usage({});
      SMOKE_CHECK(merged_empty.reasoning_source == agent::ReasoningSource::Unknown);
      SMOKE_CHECK(merged_empty.reasoning_tokens == 0);
      // Round-trip helpers.
      SMOKE_CHECK(agent::to_string(agent::ReasoningSource::Provider) == "provider");
      SMOKE_CHECK(agent::to_string(agent::ReasoningSource::Estimated) == "estimated");
      SMOKE_CHECK(agent::to_string(agent::ReasoningSource::Unknown) == "unknown");
      SMOKE_CHECK(agent::to_string(agent::TokenUsageSource::Provider) == "provider");
      SMOKE_CHECK(agent::to_string(agent::TokenUsageSource::Derived) == "derived");
      SMOKE_CHECK(agent::to_string(agent::TokenUsageQuality::Provider) == "provider");
      SMOKE_CHECK(agent::merge_token_usage_source(agent::TokenUsageSource::Provider,
                                                  agent::TokenUsageSource::Derived) ==
                  agent::TokenUsageSource::Derived);
      SMOKE_CHECK(agent::reasoning_source_from_string("provider") == agent::ReasoningSource::Provider);
      SMOKE_CHECK(agent::reasoning_source_from_string("estimated") == agent::ReasoningSource::Estimated);
      SMOKE_CHECK(agent::reasoning_source_from_string("bogus", agent::ReasoningSource::Estimated) ==
             agent::ReasoningSource::Estimated);
    }

    // (8) End-to-end: AgentRunner aggregates usage across two iterations.
    {
      class ReasoningAggregateModel final : public agent::ChatModelAdapter {
       public:
        ReasoningAggregateModel()
            : ChatModelAdapter("openai", "reasoning-aggregate", 0.0, 256, {"input.text"}) {}
        agent::AgentOutput generate(const agent::GenerateParams&) override {
          ++call_;
	          agent::AgentOutput response;
	          if (call_ == 1) {
            response.text = "Thought: call the noop tool.\nActions:\n"
                            "[{\"tool\":\"noop\",\"input\":{}}]";
	            response.finish_reason = "stop";
	            response.raw = agent::Value::object({
                {"usage", agent::Value::object({
                    {"prompt_tokens", 100},
                    {"completion_tokens", 800},
                    {"completion_tokens_details", agent::Value::object({
                        {"reasoning_tokens", 700},
                    })},
                })},
	            });
	          } else {
	            response.text = "Thought: tool completed.\nFinal Answer: done";
	            response.finish_reason = "stop";
            response.raw = agent::Value::object({
                {"usage", agent::Value::object({
                    {"prompt_tokens", 120},
                    {"completion_tokens", 40},
                })},
            });
          }
          return build_output(std::move(response));
        }
       private:
        int call_ = 0;
      };

      auto reasoning_model = std::make_shared<ReasoningAggregateModel>();
      agent::ToolDefinition noop_tool;
      noop_tool.name = "noop";
      noop_tool.description = "Returns ok.";
      noop_tool.execute = [](const agent::Value&, agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value(std::string("ok"));
      };
      agent::AgentRunner reasoning_runner(agent::AgentRunnerConfig{
          .model_runtime = {
              .adapter = reasoning_model,
          },
          .tool_runtime = {
              .definitions = std::vector<agent::ToolDefinition>{noop_tool},
          },
          .context_runtime = {
              .max_iterations = 4,
          },
      });
      const auto run_result = reasoning_runner.execution().run("hello", "reasoning-session");
      SMOKE_CHECK(run_result.usage.input_tokens == 220);
      SMOKE_CHECK(run_result.usage.output_tokens == 840);
      SMOKE_CHECK(run_result.usage.reasoning_tokens == 700);
      // One iteration carried provider reasoning, the other had none → merged source is Unknown.
      SMOKE_CHECK(run_result.usage.reasoning_source == agent::ReasoningSource::Unknown);
    }
  }

  // Provider/runtime reasoning mapping is centralized behind ProviderReasoningMapper.
  {
    // (a) OpenAI Chat Completions: reasoning enabled + budget="high" → reasoning_effort="high".
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-5-mini");
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "openai",
              .model = "gpt-5-mini",
              .reasoning = agent::ReasoningSettings{.enabled = true, .budget = std::string("high")},
              .profile = agent::make_provider_request_profile(
                  "openai", "gpt-5-mini", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(result.mode == agent::ProviderReasoningMode::OpenAIReasoningEffort);
      SMOKE_CHECK(chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(chat_request.body.at("reasoning_effort").as_string() == "high");
    }

    // (b) Numeric budget routes via normalize_reasoning_budget_to_effort.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-5-mini");
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "openai",
              .model = "gpt-5-mini",
              .reasoning = agent::ReasoningSettings{.enabled = true, .budget = 1024.0},
              .profile = agent::make_provider_request_profile(
                  "openai", "gpt-5-mini", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(chat_request.body.at("reasoning_effort").as_string() == "low");
    }

    // (c) Reasoning disabled or absent → no native OpenAI field added.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-5-mini");
      agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "openai",
              .model = "gpt-5-mini",
              .reasoning = std::nullopt,
              .profile = agent::make_provider_request_profile(
                  "openai", "gpt-5-mini", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      const auto disabled_result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "openai",
              .model = "gpt-5-mini",
              .reasoning = agent::ReasoningSettings{.enabled = false, .budget = std::string("high")},
              .profile = agent::make_provider_request_profile(
                  "openai", "gpt-5-mini", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(disabled_result.disabled);
      SMOKE_CHECK(!disabled_result.applied);
      SMOKE_CHECK(disabled_result.degraded);
      SMOKE_CHECK(disabled_result.disable_strategy == agent::ProviderReasoningDisableStrategy::MinimizeOnly);
      SMOKE_CHECK(disabled_result.metadata.at("disableStrategy").as_string() == "minimize-only");
      SMOKE_CHECK(disabled_result.message.find("effort adjustment") != std::string::npos);
      SMOKE_CHECK(chat_request.metadata.at("reasoningRequest").at("degraded").as_bool());
    }

    // (d) Qwen Chat Completions can explicitly disable thinking.
    {
      auto request = agent::build_qwen_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "qwen-plus");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "qwen",
              .model = "qwen-plus",
              .reasoning = agent::ReasoningSettings{.enabled = false, .budget = std::string("high")},
              .profile = agent::make_provider_request_profile(
                  "qwen", "qwen-plus", agent::ProviderRequestProtocol::QwenChatCompletions),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(!result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::ExplicitParam);
      SMOKE_CHECK(request.body.contains("enable_thinking"));
      SMOKE_CHECK(!request.body.at("enable_thinking").as_bool(true));
      SMOKE_CHECK(!request.body.contains("thinking_budget"));
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("disableStrategy").as_string() == "explicit-param");
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("metadata").at("reasoningDisableStrategy").as_string() == "explicit-param");
    }

    // (e) DeepSeek can explicitly disable thinking with its provider-native toggle.
    {
      auto request = agent::build_deepseek_messages_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "deepseek-reasoner",
          "",
          "");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "deepseek",
              .model = "deepseek-reasoner",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "deepseek", "deepseek-reasoner", agent::ProviderRequestProtocol::DeepSeekMessages),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(!result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::ExplicitParam);
      SMOKE_CHECK(request.body.at("thinking").at("type").as_string() == "disabled");
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("disableStrategy").as_string() == "explicit-param");
    }

    // (f) Gemini Flash can explicitly disable thinking with thinkingBudget=0.
    {
      auto request = agent::build_gemini_generate_content_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gemini-2.5-flash",
          "",
          "");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "gemini",
              .model = "gemini-2.5-flash",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "gemini", "gemini-2.5-flash", agent::ProviderRequestProtocol::GeminiGenerateContent),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(!result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::ExplicitParam);
      SMOKE_CHECK(request.body.at("generationConfig").at("thinkingConfig").at("thinkingBudget").as_number() == 0.0);
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("metadata").at("reasoningDisableStrategy").as_string() == "explicit-param");
    }

    // (g) Gemini Pro/unknown disable support is not reported as applied.
    {
      auto request = agent::build_gemini_generate_content_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gemini-2.5-pro",
          "",
          "");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "gemini",
              .model = "gemini-2.5-pro",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "gemini", "gemini-2.5-pro", agent::ProviderRequestProtocol::GeminiGenerateContent),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(!result.applied);
      SMOKE_CHECK(result.degraded);
      SMOKE_CHECK(result.unsupported);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::Unsupported);
      SMOKE_CHECK(!request.body.at("generationConfig").contains("thinkingConfig"));
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("disableStrategy").as_string() == "unsupported");
    }

    // (h0) Ollama image path inputs are resolved into messages[].images base64.
    {
      const std::vector<std::uint8_t> image_bytes{
          0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0xff, 0xd9};
      const auto image_path = tmp_dir / ("ollama-path-image-" + agent::generate_uuid() + ".jpg");
      {
        std::ofstream image_file(image_path, std::ios::binary);
        image_file.write(reinterpret_cast<const char*>(image_bytes.data()),
                         static_cast<std::streamsize>(image_bytes.size()));
      }
      agent::MediaSource source;
      source.kind = agent::MediaSourceKind::Path;
      source.path = image_path.string();
      source.mime_type = "image/jpeg";
      source.filename = "test.jpg";
      auto request = agent::build_ollama_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(
                  agent::MessageRole::User,
                  std::vector<agent::MessageContentPart>{
                      agent::text_part("describe this image"),
                      agent::image_part(source, "attached image", "auto"),
                  })},
          },
          "llava");
      const auto& messages = request.body.at("messages").as_array();
      SMOKE_CHECK(messages.size() == 1);
      SMOKE_CHECK(messages.front().at("content").as_string() == "describe this image");
      const auto& images = messages.front().at("images").as_array();
      SMOKE_CHECK(images.size() == 1);
      SMOKE_CHECK(images.front().as_string() == agent::base64_encode(image_bytes));
    }

    // (h1) Ollama image inputs fail loudly when the media payload is empty.
    {
      bool empty_inline_image_failed = false;
      try {
        (void)agent::build_ollama_chat_request(
            agent::GenerateParams{
                .messages = {agent::create_message(
                    agent::MessageRole::User,
                    std::vector<agent::MessageContentPart>{
                        agent::image_part(agent::MediaSource{
                            .kind = agent::MediaSourceKind::Inline,
                            .data = "",
                            .mime_type = "image/jpeg",
                        }),
                    })},
            },
            "llava");
      } catch (const agent::ConfigurationError& error) {
        empty_inline_image_failed =
            std::string(error.what()).find("Inline media input is empty") != std::string::npos;
      }
      SMOKE_CHECK(empty_inline_image_failed);
    }

    // (h) Ollama boolean thinking can explicitly disable thinking.
    {
      auto request = agent::build_ollama_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "llama3.2");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "ollama",
              .model = "llama3.2",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "ollama",
                  "llama3.2",
                  agent::ProviderRequestProtocol::OllamaChat,
                  {},
                  agent::ProviderReasoningMode::OllamaThinkToggle),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(!result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::ExplicitParam);
      SMOKE_CHECK(request.body.at("think").is_bool());
      SMOKE_CHECK(!request.body.at("think").as_bool(true));
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("applied").as_bool());
    }

    // (i) Ollama GPT-OSS effort mode cannot truly disable; it minimizes and reports degraded.
    {
      auto request = agent::build_ollama_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-oss:20b");
      const auto result = agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "ollama",
              .model = "gpt-oss:20b",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "ollama",
                  "gpt-oss:20b",
                  agent::ProviderRequestProtocol::OllamaChat,
                  {},
                  agent::ProviderReasoningMode::OllamaThinkEffort),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(!result.applied);
      SMOKE_CHECK(result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::MinimizeOnly);
      SMOKE_CHECK(request.body.at("think").as_string() == "low");
      SMOKE_CHECK(request.metadata.at("reasoningRequest").at("message").as_string().find("minimized") != std::string::npos);
    }

    // (j) oMLX/GPT-OSS effort mode cannot truly disable; it minimizes and reports degraded.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-oss-20b");
      chat_request.provider = "omlx";
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "omlx",
              .model = "gpt-oss-20b",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "omlx",
                  "gpt-oss-20b",
                  agent::ProviderRequestProtocol::OpenAIChatCompletions,
                  {},
                  agent::ProviderReasoningMode::OmlxChatTemplateReasoningEffort),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(!result.applied);
      SMOKE_CHECK(result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::MinimizeOnly);
      SMOKE_CHECK(chat_request.body.at("chat_template_kwargs").at("reasoning_effort").as_string() == "low");
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(!chat_request.body.contains("think"));
      SMOKE_CHECK(chat_request.metadata.at("reasoningRequest").at("degraded").as_bool());
    }

    // (k) Unsupported disabled requests are surfaced through neutral metadata and a clear message.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "local-thinking-model");
      chat_request.provider = "vllm";
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "vllm",
              .model = "local-thinking-model",
              .reasoning = agent::ReasoningSettings{.enabled = false},
              .profile = agent::make_provider_request_profile(
                  "vllm", "local-thinking-model", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(result.disabled);
      SMOKE_CHECK(!result.applied);
      SMOKE_CHECK(result.degraded);
      SMOKE_CHECK(result.disable_strategy == agent::ProviderReasoningDisableStrategy::Unsupported);
      SMOKE_CHECK(result.message.find("no native reasoning disable parameter") != std::string::npos);
      SMOKE_CHECK(chat_request.metadata.at("reasoningRequest").at("disableStrategy").as_string() == "unsupported");
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(!chat_request.body.contains("chat_template_kwargs"));
    }

    // (l) oMLX GPT-OSS uses chat_template_kwargs, not OpenAI's reasoning_effort or Ollama's think.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "gpt-oss-20b");
      chat_request.provider = "omlx";
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "omlx",
              .model = "gpt-oss-20b",
              .reasoning = agent::ReasoningSettings{.enabled = true, .budget = std::string("high")},
              .profile = agent::make_provider_request_profile(
                  "omlx",
                  "gpt-oss-20b",
                  agent::ProviderRequestProtocol::OpenAIChatCompletions,
                  {},
                  agent::ProviderReasoningMode::OmlxChatTemplateReasoningEffort),
          });
      SMOKE_CHECK(result.applied);
      SMOKE_CHECK(result.mode == agent::ProviderReasoningMode::OmlxChatTemplateReasoningEffort);
      SMOKE_CHECK(chat_request.body.at("chat_template_kwargs").at("reasoning_effort").as_string() == "high");
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(!chat_request.body.contains("think"));
    }

    // (m) Ollama GPT-OSS uses string-valued think effort.
    {
      auto request = agent::build_ollama_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
              .settings = agent::ModelSettings{
                  .reasoning = agent::ReasoningSettings{.enabled = true, .budget = std::string("low")},
              },
          },
          "gpt-oss:20b");
      agent::apply_provider_reasoning(
          request,
          agent::ProviderReasoningContext{
              .provider = "ollama",
              .model = "gpt-oss:20b",
              .reasoning = agent::ReasoningSettings{.enabled = true, .budget = std::string("low")},
              .profile = agent::make_provider_request_profile(
                  "ollama",
                  "gpt-oss:20b",
                  agent::ProviderRequestProtocol::OllamaChat,
                  {},
                  agent::ProviderReasoningMode::OllamaThinkEffort),
          });
      SMOKE_CHECK(request.body.at("think").as_string() == "low");
    }

    // (n) Generic OpenAI-compatible runtimes do not receive guessed reasoning fields.
    {
      auto chat_request = agent::build_openai_chat_request(
          agent::GenerateParams{
              .messages = {agent::create_message(agent::MessageRole::User, "hi")},
          },
          "local-thinking-model");
      chat_request.provider = "vllm";
      const auto result = agent::apply_provider_reasoning(
          chat_request,
          agent::ProviderReasoningContext{
              .provider = "vllm",
              .model = "local-thinking-model",
              .reasoning = agent::ReasoningSettings{.enabled = true},
              .profile = agent::make_provider_request_profile(
                  "vllm", "local-thinking-model", agent::ProviderRequestProtocol::OpenAIChatCompletions),
          });
      SMOKE_CHECK(!result.applied);
      SMOKE_CHECK(result.degraded);
      SMOKE_CHECK(!chat_request.body.contains("reasoning_effort"));
      SMOKE_CHECK(!chat_request.body.contains("chat_template_kwargs"));
    }

    // (o) OpenAI adapter (chat-completions path: no reasoning, no image content) → no reasoning_effort.
    {
      agent::NativeProviderRequest captured;
      agent::OpenAICompatibleChatModelAdapter openai_no_reasoning(
          "openai", "gpt-4o-mini",
          [&](const agent::NativeProviderRequest& request) {
            captured = request;
            return agent::Value::object({
                {"id", "resp_no_reasoning"},
                {"choices", agent::Value::array({
                    agent::Value::object({
                        {"message", agent::Value::object({{"content", "ok"}})},
                        {"finish_reason", "stop"},
                    }),
                })},
                {"usage", agent::Value::object({{"prompt_tokens", 3}, {"completion_tokens", 1}})},
            });
          },
          "/v1/chat/completions", "https://api.openai.test", "openai-key",
          {}, {}, {}, agent::ProviderReasoningMode::None, openai_smoke_runtime_options());
      (void)openai_no_reasoning.generate(agent::GenerateParams{
          .messages = {agent::create_message(agent::MessageRole::User, "hello")},
          .settings = agent::ModelSettings{},
      });
      SMOKE_CHECK(captured.endpoint == "/v1/chat/completions");
      SMOKE_CHECK(!captured.body.contains("reasoning_effort"));
      SMOKE_CHECK(!captured.body.contains("reasoning"));
    }

    // (p) Qwen adapter (delegates to shared build_openai_chat_request) with reasoning enabled →
    //     body has enable_thinking + thinking_budget but NOT reasoning_effort (shared builder stayed clean).
    {
      agent::NativeProviderRequest captured;
      agent::QwenChatModelAdapter qwen_reasoning_clean(
          "qwen-plus",
          [&](const agent::NativeProviderRequest& request) {
            captured = request;
            return agent::Value::object({
                {"id", "qwen_clean"},
                {"choices", agent::Value::array({
                    agent::Value::object({
                        {"message", agent::Value::object({{"content", "ok"}})},
                        {"finish_reason", "stop"},
                    }),
                })},
                {"usage", agent::Value::object({{"prompt_tokens", 4}, {"completion_tokens", 1}})},
            });
          },
          "qwen-key");
      (void)qwen_reasoning_clean.generate(agent::GenerateParams{
          .messages = {agent::create_message(agent::MessageRole::User, "hello")},
          .settings = agent::ModelSettings{
              .reasoning = agent::ReasoningSettings{.enabled = true, .budget = 32.0},
          },
      });
      SMOKE_CHECK(captured.provider == "qwen");
      SMOKE_CHECK(captured.body.contains("enable_thinking"));
      SMOKE_CHECK(captured.body.at("enable_thinking").as_bool());
      SMOKE_CHECK(captured.body.contains("thinking_budget"));
      SMOKE_CHECK(captured.body.at("thinking_budget").as_integer() == 32);
      // The shared builder must not have leaked reasoning_effort into a non-OpenAI request.
      SMOKE_CHECK(!captured.body.contains("reasoning_effort"));
    }
  }
