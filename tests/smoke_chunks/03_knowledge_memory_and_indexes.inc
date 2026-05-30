  SMOKE_CHECK(registry_markdown_documents.size() == 1);
  SMOKE_CHECK(registry_markdown_documents.front().title == "Native Guide");
  auto registry_composite_loader = loader_registry.create("composite", agent::Value::object({
      {"loaders", agent::Value::array({"text", "markdown"})},
  }));
  auto registry_composite_text = registry_composite_loader->load(agent::Value::object({
      {"type", "text"},
      {"text", "Registry composite knowledge."},
      {"title", "Registry Text"},
  }));
  SMOKE_CHECK(registry_composite_text.size() == 1);
  SMOKE_CHECK(registry_composite_text.front().source_type == "text");
  bool unknown_loader_provider_failed = false;
  try {
    (void)loader_registry.create("missing-loader");
  } catch (const agent::ConfigurationError& error) {
    unknown_loader_provider_failed =
        std::string(error.what()).find("Unknown knowledge loader provider") != std::string::npos;
  }
  SMOKE_CHECK(unknown_loader_provider_failed);

  agent::KnowledgeLoaderRegistry override_loader_registry;
  override_loader_registry.register_provider(agent::KnowledgeLoaderProvider{
      .metadata = agent::KnowledgeProviderMetadata{
          .name = "text",
          .tier = "core-safe",
          .title = "first",
      },
      .create = [](const agent::Value&, const agent::KnowledgeLoaderRegistry&) {
        return std::make_shared<agent::TextKnowledgeSourceLoader>();
      },
  });
  override_loader_registry.register_provider(agent::KnowledgeLoaderProvider{
      .metadata = agent::KnowledgeProviderMetadata{
          .name = "text",
          .tier = "core-safe",
          .title = "second",
      },
      .create = [](const agent::Value&, const agent::KnowledgeLoaderRegistry&) {
        return std::make_shared<agent::TextKnowledgeSourceLoader>();
      },
  });
  SMOKE_CHECK(override_loader_registry.get("text")->metadata.title == "second");
  SMOKE_CHECK(override_loader_registry.list_names().size() == 1);

  auto chunker_registry = agent::create_default_knowledge_chunker_registry();
  const auto* recursive_chunker_provider = chunker_registry.get("recursive-text");
  SMOKE_CHECK(recursive_chunker_provider != nullptr);
  SMOKE_CHECK(recursive_chunker_provider->metadata.tier == "core-safe");
  SMOKE_CHECK(contains_string(recursive_chunker_provider->metadata.tags, "text"));
  auto registry_chunker = chunker_registry.create("recursive-text", agent::Value::object({
      {"codeChunkLines", 2},
      {"codeChunkOverlapLines", 0},
  }));
  auto registry_chunks = registry_chunker->chunk(agent::LoadedKnowledgeDocument{
      .uri = "repo://example/src/index.ts",
      .title = "index.ts",
      .content = "one\ntwo\nthree\nfour\nfive",
      .source_type = "repository",
  });
  SMOKE_CHECK(registry_chunks.size() == 3);
  SMOKE_CHECK(registry_chunks[0].line_start == 1);
  SMOKE_CHECK(registry_chunks[0].line_end == 2);
  SMOKE_CHECK(registry_chunks[2].line_start == 5);
  SMOKE_CHECK(registry_chunks[2].line_end == 5);
  bool unknown_chunker_provider_failed = false;
  try {
    (void)chunker_registry.create("missing-chunker");
  } catch (const agent::ConfigurationError& error) {
    unknown_chunker_provider_failed =
        std::string(error.what()).find("Unknown knowledge chunker provider") != std::string::npos;
  }
  SMOKE_CHECK(unknown_chunker_provider_failed);

  auto reranker_registry = agent::create_default_knowledge_reranker_registry();
  SMOKE_CHECK(reranker_registry.get("heuristic") != nullptr);
  SMOKE_CHECK(reranker_registry.get("basic") != nullptr);
  SMOKE_CHECK(reranker_registry.get("overlap") != nullptr);
  SMOKE_CHECK(reranker_registry.get("hybrid") != nullptr);
  SMOKE_CHECK(reranker_registry.get("recency") != nullptr);
  SMOKE_CHECK(reranker_registry.get("mmr") != nullptr);
  auto reranker_provider_names = reranker_registry.list_names();
  SMOKE_CHECK(contains_string(reranker_provider_names, "heuristic"));
  SMOKE_CHECK(contains_string(reranker_provider_names, "hybrid"));
  const auto* hybrid_reranker_provider = reranker_registry.provider("hybrid");
  SMOKE_CHECK(hybrid_reranker_provider != nullptr);
  SMOKE_CHECK(hybrid_reranker_provider->metadata.tier == "portable");
  SMOKE_CHECK(contains_string(hybrid_reranker_provider->metadata.tags, "multimodal"));
  bool unknown_reranker_provider_failed = false;
  try {
    (void)reranker_registry.create("missing-reranker");
  } catch (const agent::ConfigurationError& error) {
    unknown_reranker_provider_failed =
        std::string(error.what()).find("Unknown knowledge reranker provider") != std::string::npos;
  }
  SMOKE_CHECK(unknown_reranker_provider_failed);
  auto make_rerank_hit = [](const std::string& title,
                            const std::string& snippet,
                            const std::string& content,
                            double score,
                            agent::EmbeddingVector embedding) {
    agent::KnowledgeSearchHit hit;
    hit.score = score;
    hit.citation.title = title;
    hit.citation.snippet = snippet;
    hit.chunk.content = content;
    hit.chunk.embedding = embedding;
    return hit;
  };
  auto overlap_hits = reranker_registry.get("overlap")->rerank(
      "workflow native",
      {
          make_rerank_hit("Score", "unrelated ranking", "plain content", 0.55, {0.0, 1.0}),
          make_rerank_hit("Overlap",
                          "workflow workflow workflow native native",
                          "native workflow native memory workflow native",
                          0.20,
                          {1.0, 0.0}),
      },
      2);
  SMOKE_CHECK(overlap_hits.size() == 2);
  SMOKE_CHECK(overlap_hits.front().citation.title == "Overlap");
  SMOKE_CHECK(overlap_hits.front().rerank_score.has_value());
  SMOKE_CHECK(overlap_hits.front().rerank_score.value() > overlap_hits.front().score);
  agent::MmrKnowledgeReranker diversity_reranker(0.5);
  auto mmr_hits = diversity_reranker.rerank(
      "workflow native",
      {
          make_rerank_hit("A", "", "primary", 0.90, {1.0, 0.0}),
          make_rerank_hit("B", "", "similar", 0.85, {1.0, 0.0}),
          make_rerank_hit("C", "", "diverse", 0.70, {0.0, 1.0}),
      },
      2);
  SMOKE_CHECK(mmr_hits.size() == 2);
  SMOKE_CHECK(mmr_hits[0].citation.title == "A");
  SMOKE_CHECK(mmr_hits[1].citation.title == "C");
  SMOKE_CHECK(mmr_hits[1].rerank_score.has_value());
  auto vector_only_hybrid = reranker_registry.create("hybrid", agent::Value::object({
      {"vectorWeight", 1.0},
      {"lexicalWeight", 0.0},
      {"imageAssetBias", 0.0},
      {"ocrBoost", 0.0},
  }));
  auto hybrid_option_a = make_rerank_hit("Vector", "", "vector hit", 0.0, {1.0, 0.0});
  hybrid_option_a.vector_score = 0.9;
  hybrid_option_a.lexical_score = 0.1;
  auto hybrid_option_b = make_rerank_hit("Lexical", "", "lexical hit", 0.0, {0.0, 1.0});
  hybrid_option_b.vector_score = 0.2;
  hybrid_option_b.lexical_score = 1.0;
  auto vector_weighted_hits =
      vector_only_hybrid->rerank("workflow native", {hybrid_option_b, hybrid_option_a}, 2);
  SMOKE_CHECK(vector_weighted_hits.front().citation.title == "Vector");
  agent::KnowledgeRerankerRegistry legacy_reranker_registry;
  legacy_reranker_registry.register_reranker("basic", std::make_shared<agent::BasicKnowledgeReranker>());
  SMOKE_CHECK(legacy_reranker_registry.get("basic") != nullptr);
  SMOKE_CHECK(legacy_reranker_registry.list_names().size() == 1);

  auto text_index_registry = agent::create_default_knowledge_text_index_registry();
  const auto* memory_text_index_provider = text_index_registry.get("memory");
  SMOKE_CHECK(memory_text_index_provider != nullptr);
  SMOKE_CHECK(memory_text_index_provider->metadata.title == "In-Memory Text Index");
  SMOKE_CHECK(contains_string(memory_text_index_provider->metadata.tags, "memory"));
  const auto* minisearch_text_index_provider = text_index_registry.get("minisearch");
  SMOKE_CHECK(minisearch_text_index_provider != nullptr);
  SMOKE_CHECK(minisearch_text_index_provider->metadata.tier == "portable");
  SMOKE_CHECK(contains_string(minisearch_text_index_provider->metadata.tags, "lexical"));
  SMOKE_CHECK(contains_string(text_index_registry.list_names(), "minisearch"));
  SMOKE_CHECK(text_index_registry.create("memory") != nullptr);
  SMOKE_CHECK(text_index_registry.create("missing-text-index") == nullptr);
  auto minisearch_index = text_index_registry.create("minisearch");
  SMOKE_CHECK(minisearch_index != nullptr);
  agent::KnowledgeTextIndexRegistry legacy_text_index_registry;
  legacy_text_index_registry.register_index("custom", [] {
    return std::make_shared<agent::InMemoryKnowledgeTextIndex>();
  });
  SMOKE_CHECK(legacy_text_index_registry.get("custom") != nullptr);
  SMOKE_CHECK(legacy_text_index_registry.create("custom") != nullptr);
  legacy_text_index_registry.register_provider(agent::KnowledgeTextIndexProvider{
      .metadata = agent::KnowledgeProviderMetadata{
          .name = "custom",
          .tier = "portable",
          .title = "Custom Text Index",
      },
      .create = [](const agent::Value&, const agent::KnowledgeTextIndexRegistry&) {
        return std::make_shared<agent::MiniSearchKnowledgeTextIndex>();
      },
  });
  SMOKE_CHECK(legacy_text_index_registry.get("custom")->metadata.title == "Custom Text Index");
  SMOKE_CHECK(legacy_text_index_registry.list_names().size() == 1);
  agent::KnowledgeChunkRecord indexed_chunk;
  indexed_chunk.id = "chunk-1";
  indexed_chunk.document_id = "doc-1";
  indexed_chunk.knowledge_base_id = "kb-text";
  indexed_chunk.tenant_id = "tenant-a";
  indexed_chunk.source_type = "text";
  indexed_chunk.asset_type = agent::KnowledgeAssetType::Text;
  indexed_chunk.uri = "doc://indexed";
  indexed_chunk.title = "Hello";
  indexed_chunk.content = "world agent";
  indexed_chunk.metadata = agent::Value::object({{"altText", "diagram"}, {"scope", "native"}});
  minisearch_index->upsert({indexed_chunk});
  agent::KnowledgeTextSearchOptions text_search_options;
  text_search_options.knowledge_base_id = "kb-text";
  text_search_options.tenant_id = "tenant-a";
  text_search_options.query = "hello agent";
  text_search_options.top_k = 5;
  auto text_matches = minisearch_index->search(text_search_options);
  SMOKE_CHECK(text_matches.size() == 1);
  SMOKE_CHECK(text_matches.front().chunk_id == "chunk-1");
  text_search_options.query = "diagram";
  text_search_options.metadata = agent::Value::object({{"scope", "native"}});
  text_matches = minisearch_index->search(text_search_options);
  SMOKE_CHECK(text_matches.size() == 1);
  minisearch_index->delete_chunks({"chunk-1"});
  SMOKE_CHECK(minisearch_index->stats().chunk_count == 0);

  agent::KnowledgeBase text_index_kb("text-index-kb", "tenant-a", "Text Index KB",
                                     std::make_shared<agent::HashEmbeddingAdapter>(),
                                     agent::RecursiveTextChunker{},
                                     std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                     std::make_shared<agent::InMemoryKnowledgeStore>(),
                                     std::make_shared<agent::HashImageEmbeddingAdapter>(),
                                     std::make_shared<agent::MiniSearchKnowledgeTextIndex>());
  text_index_kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "doc://visual",
          .title = "Visual Notes",
          .content = "plain asset notes",
          .source_type = "text",
          .ocr_text = "diagram native retrieval",
      },
  });
  auto text_index_hits = text_index_kb.search("diagram", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .min_score = 0.0,
      .hybrid_alpha = 0.0,
  });
  SMOKE_CHECK(text_index_hits.size() == 1);
  SMOKE_CHECK(text_index_hits.front().chunk.metadata.at("ocrText").as_string() == "diagram native retrieval");
  SMOKE_CHECK(text_index_hits.front().lexical_score > 0);

  auto search_options_store = std::make_shared<agent::InMemoryKnowledgeStore>();
  auto search_options_text_index = std::make_shared<agent::InMemoryKnowledgeTextIndex>();
  auto search_options_vector_index = std::make_shared<agent::InMemoryKnowledgeVectorIndex>();
  agent::KnowledgeBase search_options_kb("search-options-kb", "tenant-a", "Search Options KB",
                                         std::make_shared<FixedSearchEmbeddingAdapter>(),
                                         agent::RecursiveTextChunker{},
                                         std::make_shared<PassthroughKnowledgeReranker>(),
                                         search_options_store,
                                         std::make_shared<agent::HashImageEmbeddingAdapter>(),
                                         search_options_text_index,
                                         search_options_vector_index,
                                         agent::KnowledgeSearchOptions{
                                             .top_k = 1,
                                             .vector_top_k = 1,
                                             .lexical_top_k = 1,
                                             .min_score = 0.0,
                                             .hybrid_alpha = 0.0,
                                             .retrieval_mode = "hybrid",
                                             .oversample_factor = 1.0,
                                             .fusion = "weighted",
                                         });
  auto make_search_document = [](std::string id,
                                 std::string uri,
                                 std::string title,
                                 agent::KnowledgeAssetType asset_type) {
    agent::KnowledgeDocumentRecord document;
    document.id = std::move(id);
    document.knowledge_base_id = "search-options-kb";
    document.tenant_id = "tenant-a";
    document.source_type = "text";
    document.asset_type = asset_type;
    document.uri = std::move(uri);
    document.title = std::move(title);
    document.metadata = agent::Value::object({});
    return document;
  };
  auto make_search_chunk = [](std::string id,
                              const agent::KnowledgeDocumentRecord& document,
                              std::string content,
                              agent::EmbeddingVector embedding) {
    agent::KnowledgeChunkRecord chunk;
    chunk.id = std::move(id);
    chunk.document_id = document.id;
    chunk.knowledge_base_id = document.knowledge_base_id;
    chunk.tenant_id = document.tenant_id;
    chunk.source_type = document.source_type;
    chunk.asset_type = document.asset_type;
    chunk.uri = document.uri;
    chunk.title = document.title;
    chunk.content = std::move(content);
    chunk.embedding_space_id = "fixed-search-v1";
    chunk.metadata = agent::Value::object({});
    chunk.embedding = std::move(embedding);
    return chunk;
  };
  auto search_vector_doc = make_search_document("search-vector-doc", "doc://search-vector", "Vector", agent::KnowledgeAssetType::Text);
  auto search_lexical_doc = make_search_document("search-lexical-doc", "doc://search-lexical", "Lexical", agent::KnowledgeAssetType::Text);
  auto search_image_doc = make_search_document("search-image-doc", "image://search-weighted", "Weighted Image", agent::KnowledgeAssetType::Image);
  auto search_vector_chunk = make_search_chunk("search-vector", search_vector_doc, "plain vector content", {1.0, 0.0});
  auto search_lexical_chunk = make_search_chunk("search-lexical", search_lexical_doc, "needle lexical content", {0.0, 1.0});
  auto search_image_chunk = make_search_chunk("search-zz-image", search_image_doc, "plain visual content", {0.0, 1.0});
  search_options_store->upsert_documents({search_vector_doc, search_lexical_doc, search_image_doc});
  search_options_store->upsert_chunks({search_vector_chunk, search_lexical_chunk, search_image_chunk});
  auto defaulted_search_hits = search_options_kb.search("needle");
  SMOKE_CHECK(defaulted_search_hits.size() == 1);
  SMOKE_CHECK(defaulted_search_hits.front().chunk.id == "search-lexical");
  auto defaulted_context = search_options_kb.build_context_message("needle");
  SMOKE_CHECK(defaulted_context.hits.size() == 1);
  SMOKE_CHECK(defaulted_context.message);
  SMOKE_CHECK(defaulted_context.debug.at("finalHits").as_array().size() == 1);
  SMOKE_CHECK(defaulted_context.message->metadata.at("knowledgeBaseId").as_string() == "search-options-kb");
  SMOKE_CHECK(defaulted_context.hits.front().citation.knowledge_base_title == "Search Options KB");
  auto vector_only_hits = search_options_kb.search("needle", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .vector_top_k = 1,
      .lexical_top_k = 1,
      .min_score = 0.0,
      .hybrid_alpha = 1.0,
      .retrieval_mode = "vector",
      .fusion = "weighted",
  });
  SMOKE_CHECK(vector_only_hits.size() == 1);
  SMOKE_CHECK(vector_only_hits.front().chunk.id == "search-vector");
  auto hybrid_weighted_hits = search_options_kb.search("needle", agent::KnowledgeSearchOptions{
      .top_k = 2,
      .vector_top_k = 1,
      .lexical_top_k = 1,
      .min_score = 0.0,
      .hybrid_alpha = 0.0,
      .retrieval_mode = "hybrid",
      .fusion = "weighted",
  });
  SMOKE_CHECK(hybrid_weighted_hits.size() == 2);
  SMOKE_CHECK(hybrid_weighted_hits.front().chunk.id == "search-lexical");
  SMOKE_CHECK(hybrid_weighted_hits.front().lexical_score > hybrid_weighted_hits.front().vector_score);
  auto rrf_hits = search_options_kb.search("needle", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .vector_top_k = 1,
      .lexical_top_k = 1,
      .min_score = 0.0,
      .hybrid_alpha = 1.0,
      .retrieval_mode = "hybrid",
      .oversample_factor = 2.0,
      .fusion = "rrf",
  });
  SMOKE_CHECK(rrf_hits.size() == 1);
  SMOKE_CHECK(rrf_hits.front().chunk.id == "search-lexical");
  SMOKE_CHECK(rrf_hits.front().score > 0.9);
  auto debug_search_result = search_options_kb.search_with_debug("needle", agent::KnowledgeSearchOptions{
      .top_k = 2,
      .vector_top_k = 1,
      .lexical_top_k = 1,
      .min_score = 0.0,
      .hybrid_alpha = 0.0,
      .retrieval_mode = "hybrid",
      .fusion = "weighted",
  });
  SMOKE_CHECK(debug_search_result.hits.size() == 2);
  SMOKE_CHECK(debug_search_result.debug.at("query").as_string() == "needle");
  SMOKE_CHECK(debug_search_result.debug.at("totalDocumentCount").as_integer() == 3);
  SMOKE_CHECK(debug_search_result.debug.at("filteredChunkCount").as_integer() == 3);
  SMOKE_CHECK(!debug_search_result.debug.at("vectorCandidates").as_array().empty());
  SMOKE_CHECK(!debug_search_result.debug.at("lexicalCandidates").as_array().empty());
  SMOKE_CHECK(!debug_search_result.debug.at("hybridCandidates").as_array().empty());
  SMOKE_CHECK(debug_search_result.debug.at("finalHits").as_array().size() == debug_search_result.hits.size());
  auto modality_weighted_hits = search_options_kb.search("needle", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .vector_top_k = 3,
      .min_score = 0.0,
      .hybrid_alpha = 1.0,
      .retrieval_mode = "vector",
      .fusion = "weighted",
      .modality_weights = {{agent::KnowledgeAssetType::Image, 3.0}},
  });
  SMOKE_CHECK(modality_weighted_hits.size() == 1);
  SMOKE_CHECK(modality_weighted_hits.front().chunk.id == "search-zz-image");
  SMOKE_CHECK(modality_weighted_hits.front().score > search_vector_chunk.embedding.front());

  auto vector_index_registry = agent::create_default_knowledge_vector_index_registry();
  const auto* memory_vector_index_provider = vector_index_registry.get("memory");
  SMOKE_CHECK(memory_vector_index_provider != nullptr);
  SMOKE_CHECK(memory_vector_index_provider->metadata.title == "In-Memory Vector Index");
  SMOKE_CHECK(contains_string(memory_vector_index_provider->metadata.tags, "vector-index"));
  SMOKE_CHECK(vector_index_registry.create("memory") != nullptr);
  const auto* sqlite_vector_index_provider = vector_index_registry.get("sqlite");
  SMOKE_CHECK(sqlite_vector_index_provider != nullptr);
  SMOKE_CHECK(sqlite_vector_index_provider->metadata.tier == "portable");
  SMOKE_CHECK(contains_string(sqlite_vector_index_provider->metadata.tags, "sqlite"));
  const auto provider_sqlite_vector_path = tmp_dir / ("provider-vectors-" + agent::generate_uuid() + ".sqlite");
  SMOKE_CHECK(vector_index_registry.create("sqlite", agent::Value::object({
      {"filePath", provider_sqlite_vector_path.string()},
      {"dimensions", 3},
      {"namespace", "provider-scope"},
  })) != nullptr);
  const auto* qdrant_vector_index_provider = vector_index_registry.get("qdrant");
  SMOKE_CHECK(qdrant_vector_index_provider != nullptr);
  SMOKE_CHECK(qdrant_vector_index_provider->metadata.tier == "external");
  SMOKE_CHECK(contains_string(qdrant_vector_index_provider->metadata.tags, "qdrant"));
  SMOKE_CHECK(vector_index_registry.create("qdrant", agent::Value::object({
      {"baseUrl", "http://127.0.0.1:6333"},
      {"collection", "agent-smoke"},
      {"headers", agent::Value::object({{"x-test", "yes"}})},
  })) != nullptr);
  SMOKE_CHECK(vector_index_registry.create("missing-vector-index") == nullptr);
  agent::KnowledgeVectorIndexRegistry legacy_vector_index_registry;
  legacy_vector_index_registry.register_index("custom", [] {
    return std::make_shared<agent::InMemoryKnowledgeVectorIndex>();
  });
  SMOKE_CHECK(legacy_vector_index_registry.get("custom") != nullptr);
  SMOKE_CHECK(legacy_vector_index_registry.create("custom") != nullptr);

  {
    agent::KnowledgeLoaderRegistry concurrent_loader_registry;
    agent::KnowledgeChunkerRegistry concurrent_chunker_registry;
    agent::KnowledgeTextIndexRegistry concurrent_text_index_registry;
    agent::KnowledgeVectorIndexRegistry concurrent_vector_index_registry;
    agent::KnowledgeRerankerRegistry concurrent_reranker_registry;
    std::atomic<int> concurrent_registry_ready{0};
    std::atomic<bool> concurrent_registry_start{false};
    std::atomic<int> loader_factory_calls{0};
    std::atomic<int> chunker_factory_calls{0};
    std::atomic<int> text_index_factory_calls{0};
    std::atomic<int> vector_index_factory_calls{0};
    std::atomic<int> reranker_factory_calls{0};
    constexpr int registry_thread_count = 6;
    std::vector<std::thread> registry_workers;
    for (int thread_index = 0; thread_index < registry_thread_count; ++thread_index) {
      registry_workers.emplace_back([&, thread_index]() {
        concurrent_registry_ready.fetch_add(1);
        while (!concurrent_registry_start.load()) {
          std::this_thread::yield();
        }

        const auto suffix = std::to_string(thread_index);
        const auto loader_name = "loader.concurrent." + suffix;
        const auto chunker_name = "chunker.concurrent." + suffix;
        const auto text_index_name = "text-index.concurrent." + suffix;
        const auto vector_index_name = "vector-index.concurrent." + suffix;
        const auto reranker_name = "reranker.concurrent." + suffix;

        concurrent_loader_registry.register_provider(agent::KnowledgeLoaderProvider{
            .metadata = agent::KnowledgeProviderMetadata{
                .name = loader_name,
                .tier = "core-safe",
                .title = "Concurrent Loader " + suffix,
            },
            .create = [&loader_factory_calls](const agent::Value&, const agent::KnowledgeLoaderRegistry&) {
              loader_factory_calls.fetch_add(1);
              return std::make_shared<agent::TextKnowledgeSourceLoader>();
            },
        });
        auto loader = concurrent_loader_registry.create(loader_name);
        auto documents = loader->load(agent::Value::object({
            {"type", "text"},
            {"text", "concurrent loader"},
        }));
        SMOKE_CHECK(documents.size() == 1);
        SMOKE_CHECK(!concurrent_loader_registry.list_names().empty());

        concurrent_chunker_registry.register_provider(agent::KnowledgeChunkerProvider{
            .metadata = agent::KnowledgeProviderMetadata{
                .name = chunker_name,
                .tier = "core-safe",
                .title = "Concurrent Chunker " + suffix,
            },
            .create = [&chunker_factory_calls](const agent::Value&, const agent::KnowledgeChunkerRegistry&) {
              chunker_factory_calls.fetch_add(1);
              return std::make_shared<agent::RecursiveTextChunker>(16, 0, 8, 0);
            },
        });
        auto chunker = concurrent_chunker_registry.create(chunker_name);
        auto chunks = chunker->chunk(agent::LoadedKnowledgeDocument{
            .uri = "doc://concurrent/chunker/" + suffix,
            .content = "alpha beta gamma delta",
            .source_type = "text",
        });
        SMOKE_CHECK(!chunks.empty());
        SMOKE_CHECK(!concurrent_chunker_registry.list().empty());

        concurrent_text_index_registry.register_index(text_index_name, [&text_index_factory_calls] {
          text_index_factory_calls.fetch_add(1);
          return std::make_shared<agent::InMemoryKnowledgeTextIndex>();
        });
        SMOKE_CHECK(concurrent_text_index_registry.create(text_index_name) != nullptr);
        SMOKE_CHECK(!concurrent_text_index_registry.list_names().empty());

        concurrent_vector_index_registry.register_index(vector_index_name, [&vector_index_factory_calls] {
          vector_index_factory_calls.fetch_add(1);
          return std::make_shared<agent::InMemoryKnowledgeVectorIndex>();
        });
        SMOKE_CHECK(concurrent_vector_index_registry.create(vector_index_name) != nullptr);
        SMOKE_CHECK(!concurrent_vector_index_registry.list().empty());

        concurrent_reranker_registry.register_provider(agent::KnowledgeRerankerProvider{
            .metadata = agent::KnowledgeProviderMetadata{
                .name = reranker_name,
                .tier = "core-safe",
                .title = "Concurrent Reranker " + suffix,
            },
            .create = [&reranker_factory_calls](const agent::Value&, const agent::KnowledgeRerankerRegistry&) {
              reranker_factory_calls.fetch_add(1);
              return std::make_shared<agent::BasicKnowledgeReranker>();
            },
        });
        auto reranker = concurrent_reranker_registry.create(reranker_name);
        SMOKE_CHECK(reranker != nullptr);
        SMOKE_CHECK(!concurrent_reranker_registry.list_names().empty());
      });
    }
    while (concurrent_registry_ready.load() < registry_thread_count) {
      std::this_thread::yield();
    }
    concurrent_registry_start.store(true);
    for (auto& worker : registry_workers) {
      worker.join();
    }
    SMOKE_CHECK(concurrent_loader_registry.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(concurrent_chunker_registry.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(concurrent_text_index_registry.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(concurrent_vector_index_registry.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(concurrent_reranker_registry.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(loader_factory_calls.load() == registry_thread_count);
    SMOKE_CHECK(chunker_factory_calls.load() == registry_thread_count);
    SMOKE_CHECK(text_index_factory_calls.load() == registry_thread_count);
    SMOKE_CHECK(vector_index_factory_calls.load() == registry_thread_count);
    SMOKE_CHECK(reranker_factory_calls.load() == registry_thread_count);

    auto loader_registry_copy = concurrent_loader_registry;
    auto chunker_registry_copy = concurrent_chunker_registry;
    auto text_index_registry_copy = concurrent_text_index_registry;
    auto vector_index_registry_copy = concurrent_vector_index_registry;
    auto reranker_registry_copy = concurrent_reranker_registry;
    SMOKE_CHECK(loader_registry_copy.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(chunker_registry_copy.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(text_index_registry_copy.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(vector_index_registry_copy.list_names().size() == static_cast<std::size_t>(registry_thread_count));
    SMOKE_CHECK(reranker_registry_copy.list_names().size() == static_cast<std::size_t>(registry_thread_count));
  }

  const auto sqlite_vector_path = tmp_dir / ("vectors-" + agent::generate_uuid() + ".sqlite");
  {
    agent::SqliteKnowledgeVectorIndex sqlite_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = sqlite_vector_path,
        .dimensions = 3,
    });
    agent::KnowledgeChunkRecord vector_chunk_a;
    vector_chunk_a.id = "vector-a";
    vector_chunk_a.document_id = "vector-doc-a";
    vector_chunk_a.knowledge_base_id = "vector-kb";
    vector_chunk_a.tenant_id = "tenant-a";
    vector_chunk_a.source_type = "file";
    vector_chunk_a.asset_type = agent::KnowledgeAssetType::Text;
    vector_chunk_a.uri = "file://a.md";
    vector_chunk_a.embedding_space_id = "hash-shared-v1";
    vector_chunk_a.metadata = agent::Value::object({{"topic", "a"}});
    vector_chunk_a.embedding = {1.0, 0.0, 0.0};
    auto vector_chunk_b = vector_chunk_a;
    vector_chunk_b.id = "vector-b";
    vector_chunk_b.document_id = "vector-doc-b";
    vector_chunk_b.uri = "file://b.md";
    vector_chunk_b.metadata = agent::Value::object({{"topic", "b"}});
    vector_chunk_b.embedding = {0.0, 1.0, 0.0};
    sqlite_index.upsert({vector_chunk_a, vector_chunk_b});
    auto vector_matches = sqlite_index.search(agent::KnowledgeVectorSearchOptions{
        .knowledge_base_id = "vector-kb",
        .tenant_id = "tenant-a",
        .embedding = {1.0, 0.0, 0.0},
        .top_k = 2,
        .metadata = agent::Value::object({{"topic", "a"}}),
    });
    SMOKE_CHECK(vector_matches.size() == 1);
    SMOKE_CHECK(vector_matches.front().chunk_id == "vector-a");
    auto vector_stats = sqlite_index.stats();
    SMOKE_CHECK(vector_stats.chunk_count == 2);
    SMOKE_CHECK(vector_stats.asset_type_counts.at("text") == 2);
  }
  {
    agent::SqliteKnowledgeVectorIndex reopened_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = sqlite_vector_path,
        .dimensions = 3,
    });
    SMOKE_CHECK(reopened_index.stats().chunk_count == 2);
    reopened_index.delete_chunks({"vector-a"});
    auto after_delete = reopened_index.search(agent::KnowledgeVectorSearchOptions{
        .knowledge_base_id = "vector-kb",
        .tenant_id = "tenant-a",
        .embedding = {1.0, 0.0, 0.0},
        .top_k = 2,
    });
    SMOKE_CHECK(std::none_of(after_delete.begin(), after_delete.end(), [](const auto& match) {
      return match.chunk_id == "vector-a";
    }));
  }
  const auto namespaced_vector_path = tmp_dir / ("vectors-ns-" + agent::generate_uuid() + ".sqlite");
  agent::KnowledgeChunkRecord namespace_chunk_blue;
  namespace_chunk_blue.id = "vector-blue";
  namespace_chunk_blue.document_id = "vector-doc-blue";
  namespace_chunk_blue.knowledge_base_id = "vector-kb";
  namespace_chunk_blue.tenant_id = "tenant-a";
  namespace_chunk_blue.source_type = "file";
  namespace_chunk_blue.asset_type = agent::KnowledgeAssetType::Text;
  namespace_chunk_blue.uri = "file://blue.md";
  namespace_chunk_blue.embedding_space_id = "hash-shared-v1";
  namespace_chunk_blue.metadata = agent::Value::object({{"scope", "blue"}});
  namespace_chunk_blue.embedding = {1.0, 0.0, 0.0};
  auto namespace_chunk_red = namespace_chunk_blue;
  namespace_chunk_red.id = "vector-red";
  namespace_chunk_red.document_id = "vector-doc-red";
  namespace_chunk_red.uri = "file://red.md";
  namespace_chunk_red.metadata = agent::Value::object({{"scope", "red"}});
  namespace_chunk_red.embedding = {0.0, 1.0, 0.0};
  {
    agent::SqliteKnowledgeVectorIndex blue_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
        .namespace_id = "blue",
    });
    blue_index.upsert({namespace_chunk_blue});
  }
  {
    agent::SqliteKnowledgeVectorIndex red_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
        .namespace_id = "red",
    });
    red_index.upsert({namespace_chunk_red});
  }
  {
    agent::SqliteKnowledgeVectorIndex blue_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
        .namespace_id = "blue",
    });
    auto blue_stats = blue_index.stats();
    SMOKE_CHECK(blue_stats.namespace_id == "blue");
    SMOKE_CHECK(blue_stats.chunk_count == 1);
    auto blue_matches = blue_index.search(agent::KnowledgeVectorSearchOptions{
        .knowledge_base_id = "vector-kb",
        .tenant_id = "tenant-a",
        .embedding = {1.0, 0.0, 0.0},
        .top_k = 2,
    });
    SMOKE_CHECK(blue_matches.size() == 1);
    SMOKE_CHECK(blue_matches.front().chunk_id == "vector-blue");
  }
  {
    agent::SqliteKnowledgeVectorIndex red_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
        .namespace_id = "red",
    });
    red_index.delete_chunks({"vector-blue"});
    SMOKE_CHECK(red_index.stats().chunk_count == 1);
  }
  {
    agent::SqliteKnowledgeVectorIndex blue_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
        .namespace_id = "blue",
    });
    SMOKE_CHECK(blue_index.stats().chunk_count == 1);
  }
  {
    agent::SqliteKnowledgeVectorIndex unscoped_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = namespaced_vector_path,
        .dimensions = 3,
    });
    SMOKE_CHECK(unscoped_index.stats().chunk_count == 2);
  }
  const auto kb_vector_path = tmp_dir / ("kb-vectors-" + agent::generate_uuid() + ".sqlite");
  auto sqlite_kb_index = std::make_shared<agent::SqliteKnowledgeVectorIndex>(agent::SqliteKnowledgeVectorIndexConfig{
      .file_path = kb_vector_path,
      .dimensions = 256,
  });
  agent::KnowledgeBase sqlite_vector_kb("sqlite-vector-kb", "tenant-a", "File Vector KB",
                                        std::make_shared<agent::HashEmbeddingAdapter>(),
                                        agent::RecursiveTextChunker{},
                                        std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                        std::make_shared<agent::InMemoryKnowledgeStore>(),
                                        std::make_shared<agent::HashImageEmbeddingAdapter>(),
                                        std::make_shared<agent::InMemoryKnowledgeTextIndex>(),
                                        sqlite_kb_index);
  sqlite_vector_kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "doc://alpha",
          .title = "Alpha",
          .content = "alpha vector retrieval",
          .source_type = "text",
      },
      agent::LoadedKnowledgeDocument{
          .uri = "doc://beta",
          .title = "Beta",
          .content = "beta unrelated",
          .source_type = "text",
      },
  });
  auto sqlite_vector_hits = sqlite_vector_kb.search("alpha vector", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .min_score = 0.0,
  });
  SMOKE_CHECK(sqlite_vector_hits.size() == 1);
  SMOKE_CHECK(sqlite_vector_hits.front().chunk.uri == "doc://alpha");

  std::vector<agent::HttpRequest> qdrant_requests;
  agent::HttpTransport qdrant_transport = [&](const agent::HttpRequest& request) {
    qdrant_requests.push_back(request);
    if (request.method == "GET" && request.url == "https://qdrant.test/collections/knowledge") {
      return agent::HttpResponse{.status = 404, .body = "{\"error\":\"missing\"}"};
    }
    if (request.method == "PUT" && request.url == "https://qdrant.test/collections/knowledge") {
      return agent::HttpResponse{.status = 200, .body = "{\"ok\":true}"};
    }
    if (request.method == "PUT" && request.url == "https://qdrant.test/collections/knowledge/points?wait=true") {
      return agent::HttpResponse{.status = 200, .body = "{\"status\":\"ok\"}"};
    }
    if (request.method == "POST" && request.url == "https://qdrant.test/collections/knowledge/points/search") {
      return agent::HttpResponse{.status = 200,
                                 .body = "{\"result\":["
                                         "{\"id\":\"chunk-1\",\"score\":0.92,\"payload\":{\"id\":\"chunk-1\","
                                         "\"sourceType\":\"file\",\"assetType\":\"text\","
                                         "\"embeddingSpaceId\":\"space-shared\",\"uri\":\"/docs/guide.md\","
                                         "\"documentId\":\"doc-1\",\"metadata\":{\"locale\":\"ko-KR\"}}},"
                                         "{\"id\":\"chunk-2\",\"score\":0.97,\"payload\":{\"id\":\"chunk-2\","
                                         "\"sourceType\":\"file\",\"assetType\":\"text\","
                                         "\"embeddingSpaceId\":\"space-shared\",\"uri\":\"/docs/other.md\","
                                         "\"documentId\":\"doc-2\",\"metadata\":{\"locale\":\"en-US\"}}}"
                                         "]}"};
    }
    if (request.method == "POST" && request.url == "https://qdrant.test/collections/knowledge/points/delete?wait=true") {
      return agent::HttpResponse{.status = 200, .body = "{\"status\":\"ok\"}"};
    }
    return agent::HttpResponse{.status = 500, .body = "{\"error\":\"unexpected\"}"};
  };
  agent::QdrantKnowledgeVectorIndex qdrant_index(agent::QdrantKnowledgeVectorIndexConfig{
      .base_url = "https://qdrant.test",
      .collection = "knowledge",
      .api_key = "secret",
      .vector_name = "content",
      .create_collection = true,
      .dimensions = 3,
      .wait = true,
      .oversample_factor = 3,
      .transport = qdrant_transport,
  });
  agent::KnowledgeChunkRecord qdrant_chunk;
  qdrant_chunk.id = "chunk-1";
  qdrant_chunk.document_id = "doc-1";
  qdrant_chunk.knowledge_base_id = "kb-1";
  qdrant_chunk.tenant_id = "tenant-1";
  qdrant_chunk.source_type = "file";
  qdrant_chunk.asset_type = agent::KnowledgeAssetType::Text;
  qdrant_chunk.uri = "/docs/guide.md";
  qdrant_chunk.title = "Guide";
  qdrant_chunk.embedding_space_id = "space-shared";
  qdrant_chunk.metadata = agent::Value::object({{"locale", "ko-KR"}});
  qdrant_chunk.embedding = {0.1, 0.2, 0.3};
  qdrant_index.upsert({qdrant_chunk});
  auto qdrant_matches = qdrant_index.search(agent::KnowledgeVectorSearchOptions{
      .knowledge_base_id = "kb-1",
      .tenant_id = "tenant-1",
      .embedding = {0.1, 0.2, 0.3},
      .top_k = 1,
      .source_types = {"file"},
      .asset_types = {agent::KnowledgeAssetType::Text},
      .uri_prefix = "/docs/guide",
      .document_ids = {"doc-1"},
      .space_id = "space-shared",
      .metadata = agent::Value::object({{"locale", "ko-KR"}}),
  });
  qdrant_index.delete_chunks({"chunk-1"});
  SMOKE_CHECK(qdrant_matches.size() == 1);
  SMOKE_CHECK(qdrant_matches.front().chunk_id == "chunk-1");
  SMOKE_CHECK(qdrant_matches.front().score == 0.92);
  SMOKE_CHECK(std::any_of(qdrant_requests.begin(), qdrant_requests.end(), [](const auto& request) {
    return request.method == "PUT" && request.url == "https://qdrant.test/collections/knowledge";
  }));
  SMOKE_CHECK(std::any_of(qdrant_requests.begin(), qdrant_requests.end(), [](const auto& request) {
    if (request.method != "POST" || request.url != "https://qdrant.test/collections/knowledge/points/search") {
      return false;
    }
    return agent::parse_json(request.body).at("limit").as_integer() == 3;
  }));
  SMOKE_CHECK(std::any_of(qdrant_requests.begin(), qdrant_requests.end(), [](const auto& request) {
    if (request.method != "POST" || request.url != "https://qdrant.test/collections/knowledge/points/delete?wait=true") {
      return false;
    }
    return agent::parse_json(request.body).at("points").as_array().size() == 1;
  }));
  SMOKE_CHECK(qdrant_requests.front().headers.at("api-key") == "secret");

  std::vector<agent::PgVectorQuery> pgvector_queries;
  agent::PgVectorQueryClient pgvector_client = [&](const agent::PgVectorQuery& query) {
    pgvector_queries.push_back(query);
    if (query.sql.find("SELECT COUNT(*)") != std::string::npos) {
      return agent::PgVectorQueryResult{.rows = {{{"count", "1"}}}};
    }
    if (query.sql.find("SELECT") != std::string::npos && query.sql.find("chunk_id") != std::string::npos) {
      return agent::PgVectorQueryResult{.rows = {{{"chunk_id", "chunk-1"}, {"score", 0.91}}}};
    }
    return agent::PgVectorQueryResult{};
  };
  agent::PgVectorKnowledgeVectorIndex pgvector_index(agent::PgVectorKnowledgeVectorIndexConfig{
      .client = pgvector_client,
      .create_table = true,
      .dimensions = 3,
  });
  agent::KnowledgeChunkRecord pgvector_chunk;
  pgvector_chunk.id = "chunk-1";
  pgvector_chunk.document_id = "doc-1";
  pgvector_chunk.knowledge_base_id = "kb-1";
  pgvector_chunk.tenant_id = "tenant-1";
  pgvector_chunk.source_type = "file";
  pgvector_chunk.asset_type = agent::KnowledgeAssetType::Text;
  pgvector_chunk.uri = "/tmp/file.md";
  pgvector_chunk.title = "File";
  pgvector_chunk.embedding_space_id = "hash-shared-v1";
  pgvector_chunk.metadata = agent::Value::object({{"locale", "ko-KR"}});
  pgvector_chunk.embedding = {0.1, 0.2, 0.3};
  pgvector_index.upsert({pgvector_chunk});
  auto pgvector_matches = pgvector_index.search(agent::KnowledgeVectorSearchOptions{
      .knowledge_base_id = "kb-1",
      .tenant_id = "tenant-1",
      .embedding = {0.1, 0.2, 0.3},
      .top_k = 4,
      .source_types = {"file"},
      .asset_types = {agent::KnowledgeAssetType::Text},
      .uri_prefix = "/tmp/",
      .document_ids = {"doc-1"},
      .chunk_ids = {"chunk-1"},
      .space_id = "hash-shared-v1",
      .metadata = agent::Value::object({{"locale", "ko-KR"}}),
  });
  auto pgvector_zero_matches = pgvector_index.search(agent::KnowledgeVectorSearchOptions{
      .knowledge_base_id = "kb-1",
      .tenant_id = "tenant-1",
      .embedding = {0.1, 0.2, 0.3},
      .top_k = 0,
      .min_score = 0.0,
  });
  auto pgvector_stats = pgvector_index.stats();
  pgvector_index.delete_chunks({"chunk-1"});
  pgvector_index.clear(agent::KnowledgeVectorIndexClearOptions{.knowledge_base_id = "kb-1", .tenant_id = "tenant-1"});
  SMOKE_CHECK(pgvector_matches.size() == 1);
  SMOKE_CHECK(pgvector_matches.front().chunk_id == "chunk-1");
  SMOKE_CHECK(pgvector_matches.front().score == 0.91);
  SMOKE_CHECK(pgvector_zero_matches.empty());
  SMOKE_CHECK(pgvector_stats.chunk_count == 1);
  SMOKE_CHECK(std::any_of(pgvector_queries.begin(), pgvector_queries.end(), [](const auto& query) {
    return query.sql.find("CREATE TABLE IF NOT EXISTS") != std::string::npos;
  }));
  SMOKE_CHECK(std::any_of(pgvector_queries.begin(), pgvector_queries.end(), [](const auto& query) {
    return query.sql.find("INSERT INTO") != std::string::npos && query.params.size() == 11;
  }));
  SMOKE_CHECK(std::any_of(pgvector_queries.begin(), pgvector_queries.end(), [](const auto& query) {
    return query.sql.find("ORDER BY embedding <=>") != std::string::npos &&
           query.sql.find("metadata @>") != std::string::npos &&
           query.params.size() == 10;
  }));
  SMOKE_CHECK(std::any_of(pgvector_queries.begin(), pgvector_queries.end(), [](const auto& query) {
    return query.sql.find("DELETE FROM") != std::string::npos &&
           query.sql.find("chunk_id = ANY") != std::string::npos;
  }));

  agent::KnowledgeSearchHit hybrid_image_hit;
  hybrid_image_hit.score = 0.60;
  hybrid_image_hit.vector_score = 0.50;
  hybrid_image_hit.lexical_score = 0.40;
  hybrid_image_hit.chunk.asset_type = agent::KnowledgeAssetType::Image;
  hybrid_image_hit.chunk.metadata = agent::Value::object({{"ocrText", "diagram"}});
  hybrid_image_hit.citation.title = "Image";
  agent::KnowledgeSearchHit hybrid_text_hit;
  hybrid_text_hit.score = 0.70;
  hybrid_text_hit.vector_score = 0.55;
  hybrid_text_hit.lexical_score = 0.20;
  hybrid_text_hit.chunk.asset_type = agent::KnowledgeAssetType::Text;
  hybrid_text_hit.chunk.metadata = agent::Value::object({});
  hybrid_text_hit.citation.title = "Text";
  auto hybrid_hits = reranker_registry.get("hybrid")->rerank(
      "diagram",
      {hybrid_text_hit, hybrid_image_hit},
      2);
  SMOKE_CHECK(hybrid_hits.size() == 2);
  SMOKE_CHECK(hybrid_hits.front().citation.title == "Image");
  SMOKE_CHECK(hybrid_hits.front().rerank_score.has_value());
  SMOKE_CHECK(hybrid_hits.front().score == hybrid_hits.front().rerank_score.value());
  agent::KnowledgeBase loader_kb("loader-docs", "default", "Loader Docs",
                                 std::make_shared<agent::HashEmbeddingAdapter>(),
                                 agent::RecursiveTextChunker{},
                                 reranker_registry.get("basic"));
  auto loader_ingestion = loader_kb.ingest_loaded_documents(loaded_knowledge);
  SMOKE_CHECK(loader_ingestion.document_count ==
         agent::dedupe_loaded_knowledge_documents(loaded_knowledge, true).size());
  auto loader_hits = loader_kb.search("workflow native knowledge", agent::KnowledgeSearchOptions{
      .top_k = 3,
      .min_score = 0.0,
      .rerank_top_k = 6,
  });
  SMOKE_CHECK(!loader_hits.empty());
  SMOKE_CHECK(loader_hits.front().rerank_score.has_value());
  auto recency_hits = reranker_registry.get("recency")->rerank("workflow native knowledge", loader_hits, 2);
  SMOKE_CHECK(!recency_hits.empty());
  SMOKE_CHECK(recency_hits.front().rerank_score.has_value());
  auto mmr_zero_hits = reranker_registry.get("mmr")->rerank("workflow native knowledge", loader_hits, 0);
  SMOKE_CHECK(mmr_zero_hits.empty());

  agent::MediaSource sunset_media;
  sunset_media.kind = agent::MediaSourceKind::Path;
  sunset_media.path = "/tmp/sunset.png";
  sunset_media.mime_type = "image/png";
  agent::KnowledgeBase multimodal_kb("multimodal-kb", "default", "Multimodal KB",
                                     std::make_shared<agent::HashEmbeddingAdapter>(256, "hash-embedding", "shared-mm-v1"),
                                     agent::RecursiveTextChunker{},
                                     std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                     std::make_shared<agent::InMemoryKnowledgeStore>(),
                                     std::make_shared<agent::HashImageEmbeddingAdapter>(256, "hash-image-embedding", "shared-mm-v1"));
  auto image_ingestion = multimodal_kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "image://sunset",
          .title = "Sunset Beach",
          .source_type = "file",
          .asset_type = agent::KnowledgeAssetType::Image,
          .media = sunset_media,
          .text_hint = "sunset beach orange sky",
      },
  });
  SMOKE_CHECK(image_ingestion.document_count == 1);
  SMOKE_CHECK(image_ingestion.chunk_count == 1);
  auto image_hits = multimodal_kb.search("sunset beach orange sky", agent::KnowledgeSearchOptions{
      .top_k = 1,
      .min_score = 0.0,
      .asset_types = {agent::KnowledgeAssetType::Image},
  });
  SMOKE_CHECK(!image_hits.empty());
  SMOKE_CHECK(image_hits.front().citation.asset_type == agent::KnowledgeAssetType::Image);
  SMOKE_CHECK(image_hits.front().citation.uri == "image://sunset");
  SMOKE_CHECK(image_hits.front().citation.embedding_space_id == "shared-mm-v1");
  auto image_context = multimodal_kb.create_context_message(image_hits);
  SMOKE_CHECK(image_context);
  SMOKE_CHECK(image_context->content.size() == 2);
  SMOKE_CHECK(image_context->content[1].type == agent::ContentPartType::Image);

  agent::KnowledgeBase image_query_kb("multimodal-kb-2", "default", "Image Query KB",
                                      std::make_shared<agent::HashEmbeddingAdapter>(256, "hash-embedding", "shared-mm-v1"),
                                      agent::RecursiveTextChunker{},
                                      std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                      std::make_shared<agent::InMemoryKnowledgeStore>(),
                                      std::make_shared<agent::HashImageEmbeddingAdapter>(256, "hash-image-embedding", "shared-mm-v1"));
  image_query_kb.ingest_loaded_documents({
      agent::LoadedKnowledgeDocument{
          .uri = "doc://sunset-guide",
          .title = "Sunset Travel Guide",
          .content = "sunset beach orange sky travel guide",
          .source_type = "text",
      },
  });
  auto text_hits_from_image = image_query_kb.search(agent::ImageEmbeddingInput{
      .source = sunset_media,
      .alt_text = "sunset beach orange sky",
      .title = "Sunset Beach",
      .text_hint = "travel guide",
  }, agent::KnowledgeSearchOptions{
      .top_k = 1,
      .min_score = 0.0,
      .asset_types = {agent::KnowledgeAssetType::Text},
  });
  SMOKE_CHECK(!text_hits_from_image.empty());
  SMOKE_CHECK(text_hits_from_image.front().citation.asset_type == agent::KnowledgeAssetType::Text);
  SMOKE_CHECK(text_hits_from_image.front().citation.uri == "doc://sunset-guide");
  SMOKE_CHECK(text_hits_from_image.front().citation.embedding_space_id == "shared-mm-v1");

  agent::TextKnowledgeSourceLoader text_knowledge_loader;
  agent::KnowledgeBase pipeline_kb("pipeline-kb", "default", "Pipeline KB");
  agent::KnowledgeIngestionPipeline knowledge_pipeline(pipeline_kb, text_knowledge_loader);
  auto initial_pipeline_ingest = knowledge_pipeline.ingest({
      agent::Value::object({{"type", "text"}, {"uri", "note://pipeline"}, {"title", "Pipeline"}, {"text", "same content"}}),
  });
  SMOKE_CHECK(initial_pipeline_ingest.document_count == 1);
  std::vector<std::string> pipeline_phases;
  auto repeated_pipeline_ingest = knowledge_pipeline.ingest({
      agent::Value::object({{"type", "text"}, {"uri", "note://pipeline"}, {"title", "Pipeline"}, {"text", "same content"}}),
  }, agent::KnowledgeIngestionPipelineOptions{
      .on_progress = [&](const agent::KnowledgeIngestionProgress& progress) {
        pipeline_phases.push_back(progress.phase);
      },
  });
  SMOKE_CHECK(repeated_pipeline_ingest.document_count == 0);
  SMOKE_CHECK(repeated_pipeline_ingest.chunk_count == 0);
  SMOKE_CHECK(!pipeline_phases.empty());
  SMOKE_CHECK(pipeline_phases.front() == "loading");
  SMOKE_CHECK(pipeline_phases.back() == "completed");
  bool pipeline_cancellation_called = false;
  agent::CancellationToken pipeline_cancellation;
  auto pipeline_cancel_embedder = std::make_shared<agent::OpenAIEmbeddingAdapter>(
      "text-embedding-3-small",
      [&](const agent::NativeProviderRequest& request) {
        pipeline_cancellation_called = true;
        SMOKE_CHECK(request.cancellation == &pipeline_cancellation);
        request.cancellation->cancel("pipeline ingest abort");
        return agent::Value::object({
            {"data", agent::Value::array({
                agent::Value::object({{"index", 0}, {"embedding", agent::Value::array({0.1, 0.2})}}),
            })},
        });
      },
      "openai-embedding-key");
  agent::KnowledgeBase pipeline_cancel_kb("pipeline-cancel",
                                          "default",
                                          "Pipeline Cancel",
                                          pipeline_cancel_embedder);
  agent::KnowledgeIngestionPipeline pipeline_cancel(pipeline_cancel_kb, text_knowledge_loader);
  try {
    (void)pipeline_cancel.ingest({
        agent::Value::object({{"type", "text"}, {"uri", "note://cancel"}, {"text", "cancel pipeline"}}),
    }, agent::KnowledgeIngestionPipelineOptions{.cancellation = &pipeline_cancellation});
    SMOKE_CHECK(false);
  } catch (const agent::AgentFrameworkError& error) {
    SMOKE_CHECK(std::string(error.what()).find("pipeline ingest abort") != std::string::npos);
  }
  SMOKE_CHECK(pipeline_cancellation_called);
  SMOKE_CHECK(pipeline_cancel_kb.stats().document_count == 0);
  struct SmokeDedupeStrategy : agent::KnowledgeDedupeStrategy {
    bool* called;
    explicit SmokeDedupeStrategy(bool* called_ref) : called(called_ref) {}
    std::vector<agent::LoadedKnowledgeDocument> dedupe(const agent::KnowledgeDedupeContext& context) const override {
      *called = true;
      SMOKE_CHECK(context.sources.size() == 2);
      SMOKE_CHECK(context.loaded_documents.size() == 2);
      return {context.loaded_documents.back()};
    }
  };
  struct SmokeIncrementalStrategy : agent::KnowledgeIncrementalStrategy {
    bool* called;
    explicit SmokeIncrementalStrategy(bool* called_ref) : called(called_ref) {}
    agent::KnowledgeIncrementalResult prepare(const agent::KnowledgeIncrementalContext& context) const override {
      *called = true;
      SMOKE_CHECK(context.loaded_documents.size() == 2);
      SMOKE_CHECK(context.documents.size() == 1);
      agent::KnowledgeIncrementalResult result;
      result.documents = context.documents;
      result.replace_existing = false;
      result.skip_if_unchanged = false;
      return result;
    }
  };
  bool custom_dedupe_called = false;
  bool custom_incremental_called = false;
  agent::KnowledgeBase strategy_pipeline_kb("strategy-pipeline-kb", "default", "Strategy Pipeline KB");
  agent::KnowledgeIngestionPipeline strategy_pipeline(strategy_pipeline_kb, text_knowledge_loader);
  auto strategy_ingest = strategy_pipeline.ingest({
      agent::Value::object({{"type", "text"}, {"uri", "note://strategy-a"}, {"title", "A"}, {"text", "ignored"}}),
      agent::Value::object({{"type", "text"}, {"uri", "note://strategy-b"}, {"title", "B"}, {"text", "kept"}}),
  }, agent::KnowledgeIngestionPipelineOptions{
      .dedupe_strategy = std::make_shared<SmokeDedupeStrategy>(&custom_dedupe_called),
      .incremental_strategy = std::make_shared<SmokeIncrementalStrategy>(&custom_incremental_called),
  });
  SMOKE_CHECK(custom_dedupe_called);
  SMOKE_CHECK(custom_incremental_called);
  SMOKE_CHECK(strategy_ingest.document_count == 1);
  SMOKE_CHECK(strategy_pipeline_kb.store()->list_documents().front().uri == "note://strategy-b");
  struct FlakyTextKnowledgeLoader : agent::KnowledgeSourceLoader {
    mutable int calls = 0;
    bool supports(const agent::Value& source) const override {
      return source.at("type").as_string() == "text";
    }
    std::vector<agent::LoadedKnowledgeDocument> load(const agent::Value& source) const override {
      ++calls;
      if (calls == 1) {
        throw agent::ConfigurationError("transient knowledge load");
      }
      return agent::TextKnowledgeSourceLoader().load(source);
    }
  };
  struct SmokeRetryStrategy : agent::KnowledgeRetryStrategy {
    int* retry_calls;
    std::vector<std::string>* phases;
    SmokeRetryStrategy(int* calls_ref, std::vector<std::string>* phases_ref)
        : retry_calls(calls_ref), phases(phases_ref) {}
    std::size_t max_attempts(const agent::KnowledgeRetryContext& context) const override {
      SMOKE_CHECK(context.knowledge_base.id() == "retry-pipeline-kb");
      return 2;
    }
    bool should_retry(const std::exception& error,
                      const agent::KnowledgeRetryContext& context,
                      std::size_t attempt,
                      std::size_t max_attempts) const override {
      ++(*retry_calls);
      phases->push_back(context.phase);
      SMOKE_CHECK(context.phase == "loading");
      SMOKE_CHECK(context.batch_index == 0);
      SMOKE_CHECK(context.total_batches == 0);
      SMOKE_CHECK(attempt == 1);
      SMOKE_CHECK(max_attempts == 2);
      return std::string(error.what()).find("transient knowledge load") != std::string::npos;
    }
    int retry_delay_ms(const std::exception&,
                       const agent::KnowledgeRetryContext& context,
                       std::size_t attempt) const override {
      SMOKE_CHECK(context.phase == "loading");
      SMOKE_CHECK(attempt == 1);
      return 0;
    }
  };
  FlakyTextKnowledgeLoader flaky_text_loader;
  agent::KnowledgeBase retry_pipeline_kb("retry-pipeline-kb", "default", "Retry Pipeline KB");
  agent::KnowledgeIngestionPipeline retry_pipeline(retry_pipeline_kb, flaky_text_loader);
  int retry_calls = 0;
  std::vector<std::string> retry_phases;
  auto retry_ingest = retry_pipeline.ingest({
      agent::Value::object({{"type", "text"}, {"uri", "note://retry"}, {"title", "Retry"}, {"text", "retry content"}}),
  }, agent::KnowledgeIngestionPipelineOptions{
      .retry_strategy = std::make_shared<SmokeRetryStrategy>(&retry_calls, &retry_phases),
  });
  SMOKE_CHECK(flaky_text_loader.calls == 2);
  SMOKE_CHECK(retry_calls == 1);
  SMOKE_CHECK(retry_phases.size() == 1 && retry_phases.front() == "loading");
  SMOKE_CHECK(retry_ingest.document_count == 1);

  agent::KnowledgeBase sync_kb("sync-kb", "default", "Sync KB");
  agent::InMemoryKnowledgeSyncStateStore memory_sync_state;
  agent::KnowledgeSyncJob sync_job(sync_kb, text_knowledge_loader, memory_sync_state);
  auto first_sync = sync_job.sync({
      agent::Value::object({{"type", "text"}, {"uri", "note://sync"}, {"title", "Sync"}, {"text", "content v1"}}),
  });
  auto second_sync = sync_job.sync({
      agent::Value::object({{"type", "text"}, {"uri", "note://sync"}, {"title", "Sync"}, {"text", "content v1"}}),
  });
  auto third_sync = sync_job.sync({
      agent::Value::object({{"type", "text"}, {"uri", "note://sync"}, {"title", "Sync"}, {"text", "content v2"}}),
  });
  SMOKE_CHECK(first_sync.changed_document_count == 1);
  SMOKE_CHECK(second_sync.changed_document_count == 0);
  SMOKE_CHECK(second_sync.skipped_document_count == 1);
  SMOKE_CHECK(third_sync.changed_document_count == 1);
  SMOKE_CHECK(sync_kb.stats().document_count == 1);

  const auto persistent_dir = tmp_dir / ("native-store-" + agent::generate_uuid());
  std::filesystem::create_directories(persistent_dir);

  agent::SessionMemory tool_session(agent::SessionMemoryOptions{.storage = {.session_id = "tool-session"}});
  tool_session.add(agent::create_message(agent::MessageRole::User, "remember tool session"));
  agent::ToolRegistry agent_tool_registry(agent::create_agent_builtin_tools(&tool_session, &kb));
  agent::ToolExecutor agent_tool_executor(agent_tool_registry);
  auto session_snapshot = agent_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "session_snapshot_1",
      .name = "session.snapshot",
      .arguments = agent::Value::object({}),
  });
  SMOKE_CHECK(session_snapshot.ok);
  SMOKE_CHECK(session_snapshot.output.find("tool-session") != std::string::npos);
  SMOKE_CHECK(session_snapshot.output.find("remember tool session") != std::string::npos);
  auto knowledge_tool_search = agent_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "knowledge_search_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "workflow rag"}, {"topK", 1}}),
  });
  SMOKE_CHECK(knowledge_tool_search.ok);
  SMOKE_CHECK(knowledge_tool_search.output.find("Native Port") != std::string::npos);
  SMOKE_CHECK(knowledge_tool_search.output.find("\"hits\"") != std::string::npos);
  auto knowledge_tool_zero_search = agent_tool_executor.execute_tool_call(agent::ToolCall{
      .id = "knowledge_search_zero_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "workflow rag"}, {"topK", 0}}),
  });
  SMOKE_CHECK(knowledge_tool_zero_search.ok);
  SMOKE_CHECK(knowledge_tool_zero_search.output.find("\"hits\": []") != std::string::npos);
  SMOKE_CHECK(knowledge_tool_zero_search.output.find("Native Port") == std::string::npos);
  agent::ToolRegistry empty_agent_registry(agent::create_builtin_tools({"agent"}));
  agent::ToolExecutor empty_agent_executor(empty_agent_registry);
  auto empty_knowledge = empty_agent_executor.execute_tool_call(agent::ToolCall{
      .id = "knowledge_empty_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "anything"}}),
  });
  SMOKE_CHECK(empty_knowledge.ok);
  SMOKE_CHECK(empty_knowledge.output.find("\"hits\": []") != std::string::npos);
  agent::ToolExecutionContext agent_service_context;
  agent_service_context.service_refs.session = &tool_session;
  agent_service_context.service_refs.knowledge_base = &kb;
  auto service_session_snapshot = empty_agent_executor.execute_tool_call(agent::ToolCall{
      .id = "session_service_1",
      .name = "session.snapshot",
      .arguments = agent::Value::object({}),
  }, agent_service_context);
  SMOKE_CHECK(service_session_snapshot.ok);
  SMOKE_CHECK(service_session_snapshot.output.find("tool-session") != std::string::npos);
  auto service_knowledge = empty_agent_executor.execute_tool_call(agent::ToolCall{
      .id = "knowledge_service_1",
      .name = "knowledge.search",
      .arguments = agent::Value::object({{"query", "workflow rag"}, {"topK", 1}}),
  }, agent_service_context);
  SMOKE_CHECK(service_knowledge.ok);
  SMOKE_CHECK(service_knowledge.output.find("Native Port") != std::string::npos);

  agent::AgentMessage rich_message = agent::create_message(
      agent::MessageRole::User,
      std::vector<agent::MessageContentPart>{
          agent::text_part("hello", agent::Value::object({{"kind", "greeting"}})),
          agent::image_part(agent::MediaSource{
              .kind = agent::MediaSourceKind::Url,
              .url = "https://example.test/image.png",
              .mime_type = "image/png",
          }, "sample image", "low"),
      },
      agent::Value::object({{"trace", "roundtrip"}}));
  rich_message.tool_calls.push_back(agent::ToolCall{
      .id = "call_roundtrip",
      .name = "math.eval",
      .arguments = agent::Value::object({{"expression", "1+2"}}),
  });
  auto restored_message = agent::agent_message_from_value(agent::agent_message_to_value(rich_message));
  SMOKE_CHECK(restored_message.role == agent::MessageRole::User);
  SMOKE_CHECK(restored_message.content.size() == 2);
  SMOKE_CHECK(restored_message.content[1].source.url == "https://example.test/image.png");
  SMOKE_CHECK(restored_message.tool_calls.front().name == "math.eval");

  auto normalized_string_message = agent::agent_message_from_value(agent::Value::object({
      {"role", "assistant"},
      {"content", "normalized text"},
      {"toolCalls", agent::Value::array({
          agent::Value::object({{"name", "lookup"}, {"arguments", R"({"query":"native"})"}}),
          agent::Value::object({{"id", "scalar_call"}, {"name", "rank"}, {"arguments", "7"}}),
          agent::Value::object({{"id", "raw_call"}, {"name", "raw"}, {"arguments", "not json"}}),
      })},
      {"metadata", agent::Value()},
  }));
  SMOKE_CHECK(normalized_string_message.role == agent::MessageRole::Assistant);
  SMOKE_CHECK(normalized_string_message.content.size() == 1);
  SMOKE_CHECK(agent::extract_text_content(normalized_string_message.content) == "normalized text");
  SMOKE_CHECK(normalized_string_message.tool_calls[0].id == "tool_call_1");
  SMOKE_CHECK(normalized_string_message.tool_calls[0].arguments.at("query").as_string() == "native");
  SMOKE_CHECK(normalized_string_message.tool_calls[1].arguments.at("value").as_integer() == 7);
  SMOKE_CHECK(normalized_string_message.tool_calls[2].arguments.at("raw").as_string() == "not json");
  SMOKE_CHECK(normalized_string_message.metadata.is_object());
  SMOKE_CHECK(normalized_string_message.metadata.as_object().empty());

  auto normalized_single_part_message = agent::agent_message_from_value(agent::Value::object({
      {"role", "user"},
      {"content", agent::Value::object({
          {"type", "image"},
          {"source", agent::Value::object({
              {"kind", "url"},
              {"url", "https://example.test/normalized.png"},
              {"mimeType", "image/png"},
          })},
          {"altText", "normalized image"},
          {"detail", "medium"},
      })},
  }));
  SMOKE_CHECK(normalized_single_part_message.content.size() == 1);
  SMOKE_CHECK(normalized_single_part_message.content.front().type == agent::ContentPartType::Image);
  SMOKE_CHECK(normalized_single_part_message.content.front().source.url == "https://example.test/normalized.png");
  SMOKE_CHECK(normalized_single_part_message.content.front().detail.empty());

  bool rejected_nameless_tool_message = false;
  try {
    (void)agent::agent_message_from_value(agent::Value::object({
        {"role", "tool"},
        {"content", "tool output"},
    }));
  } catch (const agent::ConfigurationError& error) {
    rejected_nameless_tool_message = std::string(error.what()).find("Tool messages require a name") != std::string::npos;
  }
  SMOKE_CHECK(rejected_nameless_tool_message);

  agent::ModelResponse assistant_reasoning_response;
  assistant_reasoning_response.provider = "fixture";
  assistant_reasoning_response.model = "reasoning-model";
  assistant_reasoning_response.text = "answer";
  assistant_reasoning_response.reasoning = agent::ModelReasoning{.text = "reasoned", .format = "summary"};
  auto assistant_reasoning_message = agent::assistant_message_from_response(assistant_reasoning_response);
  SMOKE_CHECK(assistant_reasoning_message.metadata.at("reasoning").at("text").as_string() == "reasoned");
  SMOKE_CHECK(assistant_reasoning_message.metadata.at("reasoning").at("format").as_string() == "summary");
  agent::ModelResponse assistant_plain_response;
  assistant_plain_response.text = "plain";
  auto assistant_plain_message = agent::assistant_message_from_response(assistant_plain_response);
  SMOKE_CHECK(assistant_plain_message.metadata.at("reasoning").is_null());
  SMOKE_CHECK(assistant_plain_message.metadata.at("provider").is_null());

  {
    std::vector<agent::SessionMemorySnapshot> value_add_changes;
    agent::SessionMemoryOptions options;
    options.storage.session_id = "value-input-session";
    options.storage.summary = "value-summary";
    options.on_change = [&](const agent::SessionMemorySnapshot& snapshot) {
      value_add_changes.push_back(snapshot);
    };
    agent::SessionMemory session(options);
    auto& value_add_return = session.add(agent::Value::object({
        {"role", "user"},
        {"content", "value add"},
        {"metadata", agent::Value::object({{"source", "value"}})},
    }));
    SMOKE_CHECK(&value_add_return == &session);
    auto& value_add_many_return = session.add_many(std::vector<agent::Value>{
        agent::Value::object({{"role", "assistant"}, {"content", "value many one"}}),
        agent::Value::object({{"role", "user"}, {"content", "value many two"}}),
    });
    SMOKE_CHECK(&value_add_many_return == &session);
    SMOKE_CHECK(value_add_changes.size() == 2);
    SMOKE_CHECK(value_add_changes.back().messages.size() == 3);
    agent::SessionMemoryGetMessagesOptions hidden_summary;
    hidden_summary.include_summary = false;
    auto value_messages = session.get_messages(hidden_summary);
    SMOKE_CHECK(value_messages.size() == 3);
    SMOKE_CHECK(value_messages.front().role == agent::MessageRole::User);
    SMOKE_CHECK(value_messages.front().metadata.at("source").as_string() == "value");
    SMOKE_CHECK(agent::extract_text_content(value_messages.back().content) == "value many two");
    auto value_messages_with_summary = session.get_messages(agent::SessionMemoryGetMessagesOptions{});
    SMOKE_CHECK(value_messages_with_summary.size() == 4);
    SMOKE_CHECK(value_messages_with_summary.front().role == agent::MessageRole::System);
    SMOKE_CHECK(agent::extract_text_content(value_messages_with_summary.front().content).find("value-summary") !=
           std::string::npos);
  }

  {
    std::vector<agent::SessionMemorySnapshot> restore_changes;
    agent::SessionMemoryOptions options;
    options.storage.session_id = "restore-session";
    options.on_change = [&](const agent::SessionMemorySnapshot& snapshot) {
      restore_changes.push_back(snapshot);
    };
    agent::SessionMemory session(options);
    session.add(agent::create_message(agent::MessageRole::User, "before restore"));
    restore_changes.clear();

    agent::SessionMemoryRestoreInput summary_only;
    summary_only.summary = "restored-summary";
    auto& summary_restore_return = session.restore(summary_only);
    SMOKE_CHECK(&summary_restore_return == &session);
    auto summary_messages = session.get_messages();
    SMOKE_CHECK(summary_messages.size() == 1);
    SMOKE_CHECK(summary_messages.front().role == agent::MessageRole::System);
    SMOKE_CHECK(agent::extract_text_content(summary_messages.front().content).find("restored-summary") !=
           std::string::npos);
    SMOKE_CHECK(restore_changes.size() == 1);
    SMOKE_CHECK(restore_changes.back().session_id == "restore-session");
    SMOKE_CHECK(restore_changes.back().summary == "restored-summary");
    SMOKE_CHECK(restore_changes.back().messages.empty());

    agent::SessionMemoryRestoreInput messages_only;
    messages_only.messages = std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::Assistant, "restored message"),
    };
    auto& messages_restore_return = session.restore(messages_only);
    SMOKE_CHECK(&messages_restore_return == &session);
    auto restored_messages = session.get_messages();
    SMOKE_CHECK(restored_messages.size() == 1);
    SMOKE_CHECK(restored_messages.front().role == agent::MessageRole::Assistant);
    SMOKE_CHECK(agent::extract_text_content(restored_messages.front().content) == "restored message");
    SMOKE_CHECK(restore_changes.size() == 2);
    SMOKE_CHECK(restore_changes.back().summary.empty());
    SMOKE_CHECK(restore_changes.back().messages.size() == 1);
  }

  {
    std::vector<agent::SessionMemorySummaryInput> summary_inputs;
    agent::SessionMemoryOptions summarized_options;
    summarized_options.storage.session_id = "summary-session";
    summarized_options.storage.summary_label = "Custom summary";
    summarized_options.compaction.compaction_budget.max_messages = 2;
    summarized_options.compaction.compact_on_append = true;
    summarized_options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      summary_inputs.push_back(input);
      SMOKE_CHECK(input.session_id == "summary-session");
      SMOKE_CHECK(input.previous_summary.empty());
      SMOKE_CHECK(input.archived_messages.size() == 1);
      return "custom:" + agent::extract_text_content(input.archived_messages.front().content);
    };
    agent::SessionMemory summarized_session(summarized_options);
    summarized_session.add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "one"),
        agent::create_message(agent::MessageRole::Assistant, "two"),
        agent::create_message(agent::MessageRole::User, "three"),
    });
    SMOKE_CHECK(summary_inputs.size() == 1);
    auto summarized_messages = summarized_session.get_messages();
    SMOKE_CHECK(summarized_messages.size() == 3);
    SMOKE_CHECK(summarized_messages.front().role == agent::MessageRole::System);
    SMOKE_CHECK(agent::extract_text_content(summarized_messages.front().content).find("Custom summary:\ncustom:one") !=
           std::string::npos);
    SMOKE_CHECK(summarized_messages.front().metadata.at("source").as_string() == "session-memory");
  }

  {
    std::vector<agent::SessionMemorySummaryInput> store_summary_inputs;
    agent::SessionMemoryOptions options;
    options.compaction.compaction_budget.max_messages = 1;
    options.compaction.compact_on_append = true;
    options.storage.summary_label = "Store summary";
    options.storage.summary = "seed-summary";
    options.storage.messages = {agent::create_message(agent::MessageRole::User, "seed-live")};
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      store_summary_inputs.push_back(input);
      SMOKE_CHECK(input.session_id == "option-session");
      SMOKE_CHECK(input.previous_summary == "seed-summary");
      SMOKE_CHECK(input.archived_messages.size() == 2);
      return "store:" + input.previous_summary + ":" + std::to_string(input.archived_messages.size());
    };
    agent::InMemorySessionStore sessions(options);
    auto session = sessions.get("option-session");
    auto initial_messages = session->get_messages();
    SMOKE_CHECK(initial_messages.size() == 2);
    SMOKE_CHECK(agent::extract_text_content(initial_messages.front().content).find("Store summary:\nseed-summary") !=
           std::string::npos);
    SMOKE_CHECK(agent::extract_text_content(initial_messages.back().content) == "seed-live");
    session->add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "first"),
        agent::create_message(agent::MessageRole::Assistant, "second"),
    });
    SMOKE_CHECK(store_summary_inputs.size() == 1);
    auto compacted_messages = session->get_messages();
    SMOKE_CHECK(compacted_messages.size() == 2);
    SMOKE_CHECK(agent::extract_text_content(compacted_messages.front().content)
               .find("Store summary:\nstore:seed-summary:2") != std::string::npos);
    SMOKE_CHECK(agent::extract_text_content(compacted_messages.back().content) == "second");
  }

  {
    std::vector<agent::SessionMemorySummaryInput> file_summary_inputs;
    agent::SessionMemoryOptions options;
    options.compaction.compaction_budget.max_messages = 1;
    options.compaction.compact_on_append = true;
    options.storage.summary_label = "Disk summary";
    options.storage.summary = "disk-seed";
    options.storage.messages = {agent::create_message(agent::MessageRole::User, "disk-live")};
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      file_summary_inputs.push_back(input);
      SMOKE_CHECK(input.session_id == "disk-option");
      SMOKE_CHECK(input.previous_summary == "disk-seed");
      SMOKE_CHECK(input.archived_messages.size() == 2);
      return "disk:" + input.previous_summary + ":" + std::to_string(input.archived_messages.size());
    };
    agent::FileSessionStore sessions(persistent_dir / "option-sessions", "json", options);
    auto session = sessions.get("disk-option");
    auto initial_messages = session->get_messages();
    SMOKE_CHECK(initial_messages.size() == 2);
    SMOKE_CHECK(agent::extract_text_content(initial_messages.front().content).find("Disk summary:\ndisk-seed") !=
           std::string::npos);
    SMOKE_CHECK(agent::extract_text_content(initial_messages.back().content) == "disk-live");
    session->add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "older"),
        agent::create_message(agent::MessageRole::Assistant, "newest"),
    });
    SMOKE_CHECK(file_summary_inputs.size() == 1);
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "option-sessions" / "disk-option.json"));
    auto compacted_messages = session->get_messages();
    SMOKE_CHECK(compacted_messages.size() == 2);
    SMOKE_CHECK(agent::extract_text_content(compacted_messages.front().content)
               .find("Disk summary:\ndisk:disk-seed:2") != std::string::npos);
    SMOKE_CHECK(agent::extract_text_content(compacted_messages.back().content) == "newest");
  }
  {
    agent::SessionMemoryOptions options;
    options.storage.summary_label = "Disk summary";
    agent::FileSessionStore sessions(persistent_dir / "option-sessions", "json", options);
    auto session = sessions.get("disk-option");
    auto messages = session->get_messages();
    SMOKE_CHECK(messages.size() == 2);
    SMOKE_CHECK(agent::extract_text_content(messages.front().content).find("Disk summary:\ndisk:disk-seed:2") !=
           std::string::npos);
    SMOKE_CHECK(agent::extract_text_content(messages.back().content) == "newest");
  }

  {
    agent::FileSessionStore sessions(persistent_dir / "sessions");
    auto session = sessions.get("disk-session");
    session->add(agent::create_message(agent::MessageRole::User, "persist me"));
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "sessions" / "disk-session.json"));
    auto encoded_session = sessions.get("srv:one/path");
    encoded_session->add(agent::create_message(agent::MessageRole::User, "encoded persist"));
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "sessions" / "srv%3Aone%2Fpath.json"));
    SMOKE_CHECK(contains_string(sessions.list_session_ids(), "srv:one/path"));
  }
  {
    agent::FileSessionStore sessions(persistent_dir / "sessions");
    auto session = sessions.get("disk-session");
    auto messages = session->get_messages(false);
    SMOKE_CHECK(messages.size() == 1);
    SMOKE_CHECK(agent::extract_text_content(messages.front().content) == "persist me");
    auto encoded_session = sessions.get("srv:one/path");
    auto encoded_messages = encoded_session->get_messages(false);
    SMOKE_CHECK(encoded_messages.size() == 1);
    SMOKE_CHECK(agent::extract_text_content(encoded_messages.front().content) == "encoded persist");
  }

  // === Compaction refactor regression coverage ===
  // Bug 1 — counter split: precise token counter trips the trigger but a
  // separate char/4 ruler used to leave compaction a silent no-op for
  // CJK-heavy content. The shared measure_remaining_tokens helper now
  // makes the planner and the trigger use the SAME ruler.
  {
    std::vector<agent::SessionMemorySummaryInput> bug1_summary_inputs;
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug1-counter-split";
    options.storage.summary_label = "Bug1";
    options.compaction.compaction_budget.max_tokens = 100000;
    options.compaction.token_budget = 200000;
    options.compaction.auto_compact_at = 0.8;
    // Precise counter — 4x the char/4 estimate. Mimics CJK precision
    // difference vs the cheap char/4 fallback.
    options.compaction.token_counter = [](const std::vector<agent::AgentMessage>& msgs) {
      std::size_t total_chars = 0;
      for (const auto& message : msgs) {
        total_chars += agent::extract_text_content(message.content).size();
      }
      return total_chars * 4;
    };
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      bug1_summary_inputs.push_back(input);
      return std::string("summary:") + std::to_string(input.archived_messages.size());
    };
    options.compaction.compact_on_append = false;  // drive compaction explicitly
    agent::SessionMemory session(options);
    // 10 messages * 5000 chars each = 50000 chars => char/4 ruler says
    // ~12500 tokens (under cap), but precise counter says 200000 tokens
    // (over cap). Pre-fix: trigger fires, drop=0. Post-fix: drop>0.
    std::vector<agent::AgentMessage> bulk;
    for (int i = 0; i < 10; ++i) {
      bulk.push_back(agent::create_message(agent::MessageRole::User,
                                           std::string(5000, 'x')));
    }
    session.add_many(bulk);
    SMOKE_CHECK(session.estimated_token_count() >= 160000);
    SMOKE_CHECK(session.should_auto_compact());
    const std::size_t before = session.snapshot().messages.size();
    session.compact();
    const std::size_t after = session.snapshot().messages.size();
    // The critical post-refactor assertion: compaction is no longer a
    // silent no-op when the precise counter sees overflow but the char/4
    // ruler does not. Pre-fix this was `after == before`.
    SMOKE_CHECK(after < before);
    // The planner uses the same precise ruler the trigger uses, so the
    // remaining messages (5000-char each, 20000 precise tokens) must be
    // bounded by max_tokens / 20000.
    SMOKE_CHECK(after <= 5);
    SMOKE_CHECK(bug1_summary_inputs.size() == 1);
  }

  // Bug 2 — summarizer reentry without deadlock: a summarizer that calls
  // back into SessionMemory must not block on mutex_, because the new
  // three-phase compact() runs the summarizer outside the lock.
  {
    std::atomic<std::size_t> reentry_observed{0};
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug2-reentry";
    options.storage.summary_label = "Bug2";
    options.compaction.compaction_budget.max_messages = 1;
    options.compaction.compact_on_append = false;  // drive compaction explicitly
    agent::SessionMemory* session_ptr = nullptr;
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      // Re-enter via reads. Pre-fix this would deadlock on mutex_.
      SMOKE_CHECK(session_ptr != nullptr);
      const std::size_t tokens = session_ptr->estimated_token_count();
      reentry_observed.fetch_add(1, std::memory_order_relaxed);
      return "reentry:" + std::to_string(input.archived_messages.size()) +
             ":seen=" + std::to_string(tokens);
    };
    agent::SessionMemory session(options);
    session_ptr = &session;
    session.add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "one"),
        agent::create_message(agent::MessageRole::Assistant, "two"),
        agent::create_message(agent::MessageRole::User, "three"),
    });
    session.compact();
    SMOKE_CHECK(reentry_observed.load() == 1);
  }

  // Bug 2 — summarizer doesn't block readers. A long-running summarizer
  // must not stall concurrent estimated_token_count() calls.
  {
    std::atomic<bool> summarizer_started{false};
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug2-nonblock";
    options.storage.summary_label = "Bug2NB";
    options.compaction.compaction_budget.max_messages = 1;
    options.compaction.compact_on_append = false;
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      summarizer_started.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return "slow:" + std::to_string(input.archived_messages.size());
    };
    auto session = std::make_shared<agent::SessionMemory>(options);
    session->add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "a"),
        agent::create_message(agent::MessageRole::Assistant, "b"),
        agent::create_message(agent::MessageRole::User, "c"),
    });
    std::thread compact_thread([&] { session->compact(); });
    // Wait for the summarizer to be in-flight.
    while (!summarizer_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const auto reader_start = std::chrono::steady_clock::now();
    (void)session->estimated_token_count();
    const auto reader_elapsed = std::chrono::steady_clock::now() - reader_start;
    SMOKE_CHECK(reader_elapsed < std::chrono::milliseconds(50));
    compact_thread.join();
  }

  // Bug 3 — summary included in truncation budget. The planner now folds
  // the (pre-existing) summary into the token cap, just like the trigger.
  // Pre-fix: planner sized messages only, leaving "summary + messages"
  // potentially over the cap.
  {
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug3-summary-budget";
    options.storage.summary_label = "Bug3";
    options.storage.summary = std::string(800, 'S');  // ~200 char/4 tokens
    options.compaction.compaction_budget.max_tokens = 1000;
    options.compaction.compact_on_append = false;
    // Use a controlled summarizer that yields a short summary so the test
    // measures the PLANNER's behavior, not the post-compaction summary
    // size. The pre-existing 800-char summary alone is ~200 tokens; the
    // planner must shrink the message list so that (existing_summary +
    // remaining_messages) <= 1000 tokens.
    int summarizer_calls = 0;
    options.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      ++summarizer_calls;
      SMOKE_CHECK(input.previous_summary.size() == 800);
      return std::string("s");
    };
    agent::SessionMemory session(options);
    std::vector<agent::AgentMessage> bulk;
    for (int i = 0; i < 20; ++i) {
      bulk.push_back(agent::create_message(agent::MessageRole::User,
                                           std::string(300, 'M')));
    }
    session.add_many(bulk);
    // Compute what the planner SHOULD see: with summary folded in, the
    // 200-token summary leaves at most 800 char/4 tokens = 3200 chars
    // of messages, i.e. ~10 of the 300-char messages.
    session.compact();
    SMOKE_CHECK(summarizer_calls == 1);
    const auto post = session.snapshot();
    // Each message is 300 chars (~75 char/4 tokens). 800 tokens budget
    // for messages => at most 10 messages of this size.
    SMOKE_CHECK(post.messages.size() <= 10);
    // Pre-fix the planner would have kept 1000 tokens of messages and
    // ignored the 200-token summary, leaving up to 13 messages.
  }

  // Bug 4 — compact_on_append=false suppresses per-append compaction.
  {
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug4-no-append-compact";
    options.storage.summary_label = "Bug4";
    options.compaction.compaction_budget.max_messages = 2;
    options.compaction.compact_on_append = false;
    agent::SessionMemory session(options);
    std::vector<agent::AgentMessage> bulk;
    for (int i = 0; i < 8; ++i) {
      bulk.push_back(agent::create_message(agent::MessageRole::User, "msg-" + std::to_string(i)));
    }
    session.add_many(bulk);
    // No compaction during append: full message list should be present.
    SMOKE_CHECK(session.snapshot().messages.size() == 8);
    // Explicit compact still works.
    session.compact();
    SMOKE_CHECK(session.snapshot().messages.size() == 2);
  }

  // Three-phase compact preserves the externally observable result of
  // the old single-phase free function (drop oldest, write summary,
  // notify), with the addition of a second on_change notification for
  // the post-compaction summary write.
  {
    std::vector<agent::SessionMemorySnapshot> bug_smoke_changes;
    agent::SessionMemoryOptions options;
    options.storage.session_id = "bug-smoke";
    options.storage.summary_label = "Smoke";
    options.compaction.compaction_budget.max_messages = 1;
    options.compaction.compact_on_append = true;
    options.compaction.summarizer = [](const agent::SessionMemorySummaryInput& input) {
      return "smoke:" + std::to_string(input.archived_messages.size());
    };
    options.on_change = [&](const agent::SessionMemorySnapshot& snapshot) {
      bug_smoke_changes.push_back(snapshot);
    };
    agent::SessionMemory session(options);
    session.add_many(std::vector<agent::AgentMessage>{
        agent::create_message(agent::MessageRole::User, "alpha"),
        agent::create_message(agent::MessageRole::Assistant, "beta"),
        agent::create_message(agent::MessageRole::User, "gamma"),
    });
    auto final_snapshot = session.snapshot();
    SMOKE_CHECK(final_snapshot.messages.size() == 1);
    SMOKE_CHECK(agent::extract_text_content(final_snapshot.messages.front().content) == "gamma");
    SMOKE_CHECK(final_snapshot.summary == "smoke:2");
    // append-side change + Phase 1 erase change + Phase 2 baseline change
    SMOKE_CHECK(bug_smoke_changes.size() == 3);
    SMOKE_CHECK(bug_smoke_changes.back().summary == "smoke:2");
    SMOKE_CHECK(bug_smoke_changes.back().messages.size() == 1);
  }

  // plan_compaction is a pure function — no lock, no callback. Verify
  // both branches: char/4 fallback and the precise token_counter path.
  {
    std::vector<agent::AgentMessage> source;
    for (int i = 0; i < 5; ++i) {
      source.push_back(agent::create_message(agent::MessageRole::User,
                                             std::string(100, 'q')));
    }
    agent::CompactionBudget budget;
    budget.max_messages = 2;
    auto plan = agent::plan_compaction(agent::PlanCompactionInput{
        .messages = std::span<const agent::AgentMessage>(source),
        .summary = "",
        .summary_label = "Label",
        .budget = budget,
        .token_counter = {},
    });
    SMOKE_CHECK(plan.drop_count == 3);
    SMOKE_CHECK(plan.overflow.size() == 3);
    // token_counter branch — make every message worth 1000 "tokens".
    agent::CompactionBudget token_budget;
    token_budget.max_tokens = 1500;
    auto precise = [](const std::vector<agent::AgentMessage>& msgs) {
      return msgs.size() * 1000;
    };
    auto plan2 = agent::plan_compaction(agent::PlanCompactionInput{
        .messages = std::span<const agent::AgentMessage>(source),
        .summary = "",
        .summary_label = "Label",
        .budget = token_budget,
        .token_counter = precise,
    });
    // remaining must be <= 1500 ==> at most 1 message left.
    SMOKE_CHECK(plan2.drop_count >= 4);
    // Empty budget => no drop, no throw at the planner level.
    auto plan3 = agent::plan_compaction(agent::PlanCompactionInput{
        .messages = std::span<const agent::AgentMessage>(source),
        .summary = "",
        .summary_label = "Label",
        .budget = agent::CompactionBudget{},
        .token_counter = {},
    });
    SMOKE_CHECK(plan3.drop_count == 0);
    SMOKE_CHECK(plan3.overflow.empty());
  }

  // === SessionMemory hardening — race + event + structured error ===

  // (R1) Race repro for compaction_mutex_: 4 threads call compact() with a
  // 50ms-sleeping summarizer that records a monotonic sequence number. The
  // final summary must equal the LAST sequence's output (no clobber).
  {
    std::atomic<int> seq{0};
    std::atomic<int> last_seen{-1};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "race-compaction";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      const int my_seq = seq.fetch_add(1, std::memory_order_relaxed) + 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      last_seen.store(my_seq, std::memory_order_relaxed);
      (void)input;
      return std::string("seq:") + std::to_string(my_seq);
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 8; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("m") + std::to_string(i)));
    }
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&] {
        try {
          session.compact();
        } catch (...) {
        }
      });
    }
    for (auto& th : threads) th.join();
    const auto snap = session.snapshot();
    // The compaction_mutex_ serializes summarizer runs. The final summary
    // must equal seq:N for some N, and since the last writer wins, the
    // observed final must match the last sequence number observed by ANY
    // summarizer invocation that ran.
    const int observed_seq = last_seen.load(std::memory_order_relaxed);
    SMOKE_CHECK(observed_seq >= 1);
    const std::string expected = std::string("seq:") + std::to_string(observed_seq);
    SMOKE_CHECK(snap.summary == expected);
  }

  // (R2) compact_on_append=false default: pushing 20 messages via add() with
  // a small max_messages cap should NOT compact during append. Explicit
  // compact() then drops to the cap.
  {
    agent::SessionMemoryOptions opts;
    opts.compaction.compaction_budget.max_messages = 5;
    // Intentionally no explicit compact_on_append — relies on the new false default.
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 20; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("rx-") + std::to_string(i)));
    }
    SMOKE_CHECK(session.snapshot().messages.size() == 20);
    session.compact();
    SMOKE_CHECK(session.snapshot().messages.size() == 5);
  }

  // (R3) Compaction event listener — Synchronous mode emits the full
  // taxonomy: Started, Phase1Completed, RefinementStarted, RefinementCompleted.
  {
    std::vector<agent::SessionCompactionEvent> events;
    std::mutex events_mutex;
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "event-bridge";
    opts.compaction.compaction_budget.max_messages = 2;
    opts.compaction.compact_on_append = false;
    opts.on_compaction = [&](const agent::SessionCompactionEvent& event) {
      std::lock_guard<std::mutex> g(events_mutex);
      events.push_back(event);
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("e") + std::to_string(i)));
    }
    session.compact();
    std::lock_guard<std::mutex> g(events_mutex);
    SMOKE_CHECK(events.size() == 4);
    SMOKE_CHECK(events[0].kind == agent::SessionCompactionEvent::Kind::Started);
    SMOKE_CHECK(events[1].kind == agent::SessionCompactionEvent::Kind::Phase1Completed);
    SMOKE_CHECK(events[2].kind == agent::SessionCompactionEvent::Kind::RefinementStarted);
    SMOKE_CHECK(events[3].kind == agent::SessionCompactionEvent::Kind::RefinementCompleted);
    SMOKE_CHECK(events[0].dropped_count == 4);
    SMOKE_CHECK(events[1].dropped_count == 4);
    SMOKE_CHECK(events[3].summarizer_duration.count() >= 0);
    SMOKE_CHECK(events[0].session_id == "event-bridge");
    // to_string PascalCase matches the brief
    SMOKE_CHECK(agent::to_string(agent::SessionCompactionEvent::Kind::Started) == "Started");
    SMOKE_CHECK(agent::to_string(agent::SessionCompactionEvent::Kind::Phase1Completed) == "Phase1Completed");
    SMOKE_CHECK(agent::to_string(agent::SessionCompactionEvent::Kind::RefinementStarted) == "RefinementStarted");
    SMOKE_CHECK(agent::to_string(agent::SessionCompactionEvent::Kind::RefinementCompleted) == "RefinementCompleted");
    SMOKE_CHECK(agent::to_string(agent::SessionCompactionEvent::Kind::Failed) == "Failed");
  }

  // (R4) Structured error on empty budget. ConfigurationError details() must
  // be a JSON-encoded string containing code + field.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "structured-error";
    // No budget configured.
    agent::SessionMemory session(std::move(opts));
    session.add(agent::create_message(agent::MessageRole::User, "hello"));
    bool threw = false;
    std::string what_text;
    std::string details_text;
    try {
      session.compact();
    } catch (const agent::ConfigurationError& e) {
      threw = true;
      what_text = e.what();
      details_text = e.details();
    }
    SMOKE_CHECK(threw);
    // Error message MUST still contain "no CompactionBudget" for grep-ability.
    SMOKE_CHECK(what_text.find("no CompactionBudget") != std::string::npos);
    // details() is a JSON-encoded string by AgentFrameworkError convention.
    SMOKE_CHECK(details_text.find("\"code\":\"no_compaction_budget\"") != std::string::npos);
    SMOKE_CHECK(details_text.find("\"field\":\"compaction.compaction_budget\"") != std::string::npos);
  }

  // (R5) summary_max_chars is configurable. With the built-in (no host
  // summarizer) summarize_messages fallback and a tight cap, the final
  // summary_ length must be <= the cap.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "summary-cap";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summary_max_chars = 100;
    // No host summarizer → built-in fallback summarize_messages runs.
    agent::SessionMemory session(std::move(opts));
    // Push enough content that the default summary would exceed 100 chars.
    for (int i = 0; i < 30; ++i) {
      session.add(agent::create_message(
          agent::MessageRole::User,
          std::string("verbose message ") + std::to_string(i) + std::string(50, 'P')));
    }
    session.compact();
    const auto snap = session.snapshot();
    SMOKE_CHECK(snap.summary.size() <= 100);
  }

  // ==========================================================================
  // Background summarizer mode upgrade — tests B1..B11 of the brief.
  // Public surface locked: SummarizerMode, SessionCompactionExecutor,
  // SessionMemory::await_refinements, CompactionPlan::truncated_tail_message,
  // SessionCompactionEvent::Kind {Started, Phase1Completed, RefinementStarted,
  // RefinementCompleted, Failed}.
  // ==========================================================================

  // (B1) Synchronous mode preserves existing behavior — summarizer runs on
  // the calling thread, compact() returns when refinement is done.
  {
    std::thread::id summarizer_thread;
    std::atomic<int> calls{0};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b1-sync";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Synchronous;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      summarizer_thread = std::this_thread::get_id();
      ++calls;
      return std::string("sync:") + std::to_string(input.archived_messages.size());
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("s") + std::to_string(i)));
    }
    const auto caller_thread = std::this_thread::get_id();
    session.compact();
    SMOKE_CHECK(calls == 1);
    SMOKE_CHECK(summarizer_thread == caller_thread);
    const auto snap = session.snapshot();
    SMOKE_CHECK(snap.messages.size() == 1);
    SMOKE_CHECK(snap.summary == "sync:5");
  }

  // (B2) Background mode dispatches Phase 2 off-thread. Use await_refinements
  // to synchronize. The captured thread id must NOT match the caller's.
  {
    std::thread::id summarizer_thread;
    std::atomic<int> calls{0};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b2-off-thread";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      summarizer_thread = std::this_thread::get_id();
      ++calls;
      return std::string("bg:") + std::to_string(input.archived_messages.size());
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("b") + std::to_string(i)));
    }
    const auto caller_thread = std::this_thread::get_id();
    session.compact();
    session.await_refinements();
    SMOKE_CHECK(calls == 1);
    SMOKE_CHECK(summarizer_thread != caller_thread);
    const auto snap = session.snapshot();
    SMOKE_CHECK(snap.summary == "bg:5");
  }

  // (B3) Background mode unblocks the caller. A summarizer that sleeps 200ms
  // must NOT block compact() — caller should observe a small Phase 1 latency.
  {
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b3-unblock";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [](const agent::SessionMemorySummaryInput& input) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return std::string("slow:") + std::to_string(input.archived_messages.size());
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("u") + std::to_string(i)));
    }
    const auto start = std::chrono::steady_clock::now();
    session.compact();
    const auto compact_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    SMOKE_CHECK(compact_duration.count() < 100);  // generous: Phase 1 only
    session.await_refinements();
    const auto snap = session.snapshot();
    SMOKE_CHECK(snap.summary == "slow:5");
  }

  // (B4) Accumulating refinement during in-flight. Two compact() calls,
  // second one happens while first refinement is sleeping. Exactly TWO
  // summarizer calls happen (one in-flight, one trailing). Final summary
  // covers all overflow.
  {
    std::atomic<int> calls{0};
    std::vector<std::size_t> overflow_sizes;
    std::mutex sizes_mutex;
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b4-accumulate";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      const int my = calls.fetch_add(1) + 1;
      {
        std::lock_guard<std::mutex> g(sizes_mutex);
        overflow_sizes.push_back(input.archived_messages.size());
      }
      if (my == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
      return std::string("r:") + std::to_string(my);
    };
    agent::SessionMemory session(std::move(opts));
    // First batch: 6 messages -> compact -> Phase 1 drops 5, Phase 2 dispatched.
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("a") + std::to_string(i)));
    }
    session.compact();
    // Give the executor a moment to start the in-flight refinement.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // Second batch — adds enough to trigger another Phase 1 drop. Cumulative
    // pending now has messages from the first AND second batch (because the
    // first refinement consumed a snapshot earlier).
    for (int i = 0; i < 5; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("b") + std::to_string(i)));
    }
    session.compact();
    session.await_refinements();
    SMOKE_CHECK(calls == 2);
    // Trailing refinement must have seen the appended overflow.
    {
      std::lock_guard<std::mutex> g(sizes_mutex);
      SMOKE_CHECK(overflow_sizes.size() == 2);
      SMOKE_CHECK(overflow_sizes[0] >= 1);
      SMOKE_CHECK(overflow_sizes[1] >= 1);
    }
    const auto snap = session.snapshot();
    SMOKE_CHECK(snap.summary == "r:2");
    SMOKE_CHECK(snap.pending_concat_tail.empty());
    SMOKE_CHECK(snap.pending_refinement_overflow.empty());
  }

  // (B5) Failed refinement preserves info; subsequent compact triggers a
  // new refinement that succeeds. refinement_pending_ is NOT a retry queue.
  {
    std::atomic<int> calls{0};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b5-failed";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      const int my = calls.fetch_add(1) + 1;
      if (my == 1) {
        throw std::runtime_error("boom");
      }
      return std::string("ok:") + std::to_string(input.archived_messages.size());
    };
    std::vector<agent::SessionCompactionEvent::Kind> kinds;
    std::mutex kinds_mutex;
    opts.on_compaction = [&](const agent::SessionCompactionEvent& e) {
      std::lock_guard<std::mutex> g(kinds_mutex);
      kinds.push_back(e.kind);
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("f") + std::to_string(i)));
    }
    session.compact();
    session.await_refinements();
    {
      auto snap = session.snapshot();
      // Failed: polished_baseline_ untouched, pending preserved.
      SMOKE_CHECK(snap.polished_baseline.empty());
      SMOKE_CHECK(!snap.pending_refinement_overflow.empty());
    }
    {
      std::lock_guard<std::mutex> g(kinds_mutex);
      bool saw_failed = false;
      for (auto k : kinds) {
        if (k == agent::SessionCompactionEvent::Kind::Failed) saw_failed = true;
      }
      SMOKE_CHECK(saw_failed);
    }
    // Host retries by triggering another compact. Even though no new overflow,
    // the existing pending state still drives refinement.
    session.compact();
    session.await_refinements();
    SMOKE_CHECK(calls == 2);
    auto snap = session.snapshot();
    SMOKE_CHECK(snap.polished_baseline.rfind("ok:", 0) == 0);
    SMOKE_CHECK(snap.pending_refinement_overflow.empty());
  }

  // (B6) Single-message overflow with truncate_oversized_message=true.
  // Push one message of 1000 chars. max_tokens=20 (i.e. ~80 chars).
  // After compact(), messages_.size() should be 1, the message should be
  // tail-truncated to <= 100 chars, and the event should report
  // single_message_truncated = true.
  {
    std::vector<agent::SessionCompactionEvent> events;
    std::mutex emu;
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b6-truncate";
    opts.compaction.compaction_budget.max_tokens = 20;
    opts.compaction.token_budget = 25;
    opts.compaction.auto_compact_at = 0.8;
    opts.compaction.compact_on_append = false;
    opts.compaction.truncate_oversized_message = true;
    opts.compaction.oversized_tail_chars = 100;
    opts.compaction.summarizer = [](const agent::SessionMemorySummaryInput&) {
      return std::string("trunc");
    };
    opts.on_compaction = [&](const agent::SessionCompactionEvent& e) {
      std::lock_guard<std::mutex> g(emu);
      events.push_back(e);
    };
    agent::SessionMemory session(std::move(opts));
    session.add(agent::create_message(agent::MessageRole::User,
                                      std::string(1000, 'X')));
    SMOKE_CHECK(session.should_auto_compact());
    session.compact();
    session.await_refinements();
    auto snap = session.snapshot();
    SMOKE_CHECK(snap.messages.size() == 1);
    const auto& tail_text = agent::extract_text_content(snap.messages.front().content);
    SMOKE_CHECK(tail_text.size() <= 100);
    bool saw_truncated_flag = false;
    {
      std::lock_guard<std::mutex> g(emu);
      for (const auto& e : events) {
        if (e.single_message_truncated) saw_truncated_flag = true;
      }
    }
    SMOKE_CHECK(saw_truncated_flag);
  }

  // (B7) Single-message overflow WITHOUT opt-in. The compaction warning
  // field reports the soft over-budget condition.
  {
    std::vector<agent::SessionCompactionEvent> events;
    std::mutex emu;
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b7-warning";
    opts.compaction.compaction_budget.max_tokens = 20;
    opts.compaction.token_budget = 25;
    opts.compaction.auto_compact_at = 0.8;
    opts.compaction.compact_on_append = false;
    opts.compaction.truncate_oversized_message = false;
    opts.compaction.summarizer = [](const agent::SessionMemorySummaryInput&) {
      return std::string{};
    };
    opts.on_compaction = [&](const agent::SessionCompactionEvent& e) {
      std::lock_guard<std::mutex> g(emu);
      events.push_back(e);
    };
    agent::SessionMemory session(std::move(opts));
    session.add(agent::create_message(agent::MessageRole::User,
                                      std::string(1000, 'Y')));
    SMOKE_CHECK(session.should_auto_compact());
    session.compact();
    session.await_refinements();
    bool warning_seen = false;
    {
      std::lock_guard<std::mutex> g(emu);
      for (const auto& e : events) {
        if (e.warning.find("single message exceeds max_tokens") != std::string::npos) {
          warning_seen = true;
        }
      }
    }
    SMOKE_CHECK(warning_seen);
  }

  // (B8) Persistence round-trip preserves polished + pending state. Trigger
  // Background compaction, push more messages, second Phase 1 accumulates.
  // Without waiting for refinement, serialize via SessionStore, deserialize
  // into a fresh SessionMemory, and verify the restored state.
  {
    std::atomic<bool> allow_finish{false};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b8-persistence";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput& input) {
      // Block until we explicitly allow finish, so the second Phase 1 can
      // accumulate while refinement is in flight.
      while (!allow_finish.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return std::string("p:") + std::to_string(input.archived_messages.size());
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("p") + std::to_string(i)));
    }
    session.compact();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    for (int i = 0; i < 4; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("q") + std::to_string(i)));
    }
    session.compact();
    // Snapshot includes pending state.
    const auto snap = session.snapshot();
    SMOKE_CHECK(!snap.pending_concat_tail.empty());
    SMOKE_CHECK(!snap.pending_refinement_overflow.empty());
    // Restore into a fresh SessionMemory and verify.
    agent::SessionMemoryOptions restore_opts;
    restore_opts.storage.session_id = "bg-b8-persistence";
    restore_opts.compaction.compaction_budget.max_messages = 1;
    agent::SessionMemory restored(std::move(restore_opts));
    restored.restore(snap);
    const auto restored_snap = restored.snapshot();
    SMOKE_CHECK(restored_snap.polished_baseline == snap.polished_baseline);
    SMOKE_CHECK(restored_snap.pending_concat_tail == snap.pending_concat_tail);
    SMOKE_CHECK(restored_snap.pending_refinement_overflow.size() ==
           snap.pending_refinement_overflow.size());
    allow_finish.store(true);
    session.await_refinements();
  }

  // (B9) Custom background_executor is invoked for every dispatched task.
  {
    std::atomic<int> dispatched{0};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b9-executor";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [](const agent::SessionMemorySummaryInput&) {
      return std::string("e");
    };
    opts.compaction.background_executor = [&](std::function<void()> work) {
      ++dispatched;
      std::thread(std::move(work)).detach();
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("z") + std::to_string(i)));
    }
    session.compact();
    session.await_refinements();
    SMOKE_CHECK(dispatched >= 1);
  }

  // (B10) await_refinements() blocks then returns after refinement completes.
  {
    std::atomic<bool> summarizer_started{false};
    std::atomic<bool> summarizer_done{false};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b10-await";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [&](const agent::SessionMemorySummaryInput&) {
      summarizer_started.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      summarizer_done.store(true);
      return std::string("aw");
    };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 6; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("w") + std::to_string(i)));
    }
    session.compact();
    // Block until refinement done.
    session.await_refinements();
    SMOKE_CHECK(summarizer_done.load());
  }

  // (B11) Cross-mode event sequence. Background mode fires the full taxonomy
  // (Started -> Phase1Completed -> RefinementStarted -> RefinementCompleted),
  // and on_change fires twice (Phase 1 mutation + refinement commit).
  {
    std::vector<agent::SessionCompactionEvent::Kind> kinds;
    std::mutex kmu;
    std::atomic<int> changes{0};
    agent::SessionMemoryOptions opts;
    opts.storage.session_id = "bg-b11-events";
    opts.compaction.compaction_budget.max_messages = 1;
    opts.compaction.compact_on_append = false;
    opts.compaction.summarizer_mode = agent::SummarizerMode::Background;
    opts.compaction.summarizer = [](const agent::SessionMemorySummaryInput&) {
      return std::string("done");
    };
    opts.on_compaction = [&](const agent::SessionCompactionEvent& e) {
      std::lock_guard<std::mutex> g(kmu);
      kinds.push_back(e.kind);
    };
    opts.on_change = [&](const agent::SessionMemorySnapshot&) { ++changes; };
    agent::SessionMemory session(std::move(opts));
    for (int i = 0; i < 5; ++i) {
      session.add(agent::create_message(agent::MessageRole::User,
                                        std::string("v") + std::to_string(i)));
    }
    const int changes_before_compact = changes.load();
    session.compact();
    session.await_refinements();
    {
      std::lock_guard<std::mutex> g(kmu);
      SMOKE_CHECK(kinds.size() == 4);
      SMOKE_CHECK(kinds[0] == agent::SessionCompactionEvent::Kind::Started);
      SMOKE_CHECK(kinds[1] == agent::SessionCompactionEvent::Kind::Phase1Completed);
      SMOKE_CHECK(kinds[2] == agent::SessionCompactionEvent::Kind::RefinementStarted);
      SMOKE_CHECK(kinds[3] == agent::SessionCompactionEvent::Kind::RefinementCompleted);
    }
    // Two on_change notifications fire during compact: Phase 1 + Phase 2 commit.
    SMOKE_CHECK(changes.load() - changes_before_compact == 2);
  }


  {
    agent::FileVectorStore vectors(persistent_dir / "vectors.json", "disk");
    vectors.upsert({agent::VectorMemoryUpsertInput{
        .id = "vec-1",
        .content = "persisted vector memory",
        .embedding = {1.0, 0.0, 0.0},
        .namespace_id = "disk",
    }});
  }
  {
    agent::FileVectorStore vectors(persistent_dir / "vectors.json", "disk");
    SMOKE_CHECK(vectors.count("disk") == 1);
    auto vector_hits = vectors.query(agent::VectorMemoryQuery{
        .embedding = {1.0, 0.0, 0.0},
        .top_k = 1,
        .min_score = 0.0,
        .namespace_id = "disk",
    });
    SMOKE_CHECK(vector_hits.size() == 1);
    SMOKE_CHECK(vector_hits.front().content == "persisted vector memory");
    SMOKE_CHECK(vectors.delete_ids({"vec-1"}) == 1);
    SMOKE_CHECK(vectors.count("disk") == 0);
  }
  {
    agent::InMemoryVectorStore vectors(agent::InMemoryVectorStoreConfig{.namespace_id = "cfg"});
    vectors.upsert({agent::VectorMemoryUpsertInput{
        .id = "cfg-1",
        .content = "configured in-memory vector",
        .embedding = {1.0, 0.0},
    }});
    SMOKE_CHECK(vectors.count("cfg") == 1);
    auto hits = vectors.query(agent::VectorMemoryQuery{
        .embedding = {1.0, 0.0},
        .top_k = 1,
        .min_score = 0.0,
        .namespace_id = "cfg",
    });
    SMOKE_CHECK(hits.size() == 1);
    SMOKE_CHECK(vectors.delete_ids({"cfg-1"}) == 1);
    SMOKE_CHECK(vectors.count("cfg") == 0);
  }
  {
    const auto configured_vector_file = persistent_dir / "vectors-configured.json";
    std::filesystem::remove(configured_vector_file);
    agent::FileVectorStore vectors(agent::FileVectorStoreConfig{
        .file_path = configured_vector_file,
        .namespace_id = "cfg-disk",
    });
    vectors.upsert({agent::VectorMemoryUpsertInput{
        .id = "cfg-disk-1",
        .content = "configured disk vector",
        .embedding = {1.0, 0.0},
    }});
    SMOKE_CHECK(vectors.count("cfg-disk") == 1);
    agent::FileVectorStore reloaded(agent::FileVectorStoreConfig{
        .file_path = configured_vector_file,
        .namespace_id = "cfg-disk",
    });
    SMOKE_CHECK(reloaded.count("cfg-disk") == 1);
    SMOKE_CHECK(reloaded.delete_ids({"cfg-disk-1"}) == 1);
    SMOKE_CHECK(reloaded.count("cfg-disk") == 0);
  }
  {
    agent::InMemoryVectorStore memory_vectors("concurrent");
    constexpr int thread_count = 6;
    constexpr int iterations = 12;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto id = "mem-vec-" + std::to_string(thread_index) + "-" + std::to_string(iteration);
          memory_vectors.upsert({agent::VectorMemoryUpsertInput{
              .id = id,
              .content = "memory vector " + id,
              .embedding = {1.0, static_cast<double>(thread_index), static_cast<double>(iteration)},
              .namespace_id = "concurrent",
          }});
          (void)memory_vectors.query(agent::VectorMemoryQuery{
              .embedding = {1.0, static_cast<double>(thread_index), static_cast<double>(iteration)},
              .top_k = 2,
              .min_score = 0.0,
              .namespace_id = "concurrent",
          });
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    SMOKE_CHECK(memory_vectors.count("concurrent") == static_cast<std::size_t>(thread_count * iterations));
  }
  {
    const auto concurrent_vector_file = persistent_dir / "vectors-concurrent.json";
    std::filesystem::remove(concurrent_vector_file);
    agent::FileVectorStore file_vectors(concurrent_vector_file, "disk-concurrent");
    constexpr int thread_count = 5;
    constexpr int iterations = 10;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto id = "file-vec-" + std::to_string(thread_index) + "-" + std::to_string(iteration);
          file_vectors.upsert({agent::VectorMemoryUpsertInput{
              .id = id,
              .content = "file vector " + id,
              .embedding = {1.0, static_cast<double>(thread_index), static_cast<double>(iteration)},
              .namespace_id = "disk-concurrent",
          }});
          SMOKE_CHECK(file_vectors.count("disk-concurrent") >= 1);
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    agent::FileVectorStore reloaded_vectors(concurrent_vector_file, "disk-concurrent");
    SMOKE_CHECK(reloaded_vectors.count("disk-concurrent") == static_cast<std::size_t>(thread_count * iterations));
  }
  {
    auto make_concurrent_document = [](const std::string& id) {
      return agent::KnowledgeDocumentRecord{
          .id = id,
          .knowledge_base_id = "kb-concurrent",
          .tenant_id = "tenant-a",
          .source_type = "text",
          .asset_type = agent::KnowledgeAssetType::Text,
          .uri = "doc://" + id,
          .title = "Concurrent " + id,
          .content = "native concurrent knowledge " + id,
          .metadata = agent::Value::object({{"id", id}}),
      };
    };
    auto make_concurrent_chunk = [](const agent::KnowledgeDocumentRecord& document, int ordinal) {
      return agent::KnowledgeChunkRecord{
          .id = document.id + "-chunk-" + std::to_string(ordinal),
          .document_id = document.id,
          .knowledge_base_id = document.knowledge_base_id,
          .tenant_id = document.tenant_id,
          .source_type = document.source_type,
          .asset_type = document.asset_type,
          .uri = document.uri,
          .title = document.title,
          .content = document.content + " token-" + std::to_string(ordinal),
          .chunk_index = static_cast<std::size_t>(ordinal),
          .embedding_space_id = "concurrent-space",
          .metadata = document.metadata,
          .embedding = {1.0, static_cast<double>(ordinal), 0.0},
      };
    };
    agent::InMemoryKnowledgeStore memory_store;
    agent::MiniSearchKnowledgeTextIndex text_index;
    agent::InMemoryKnowledgeVectorIndex vector_index;
    constexpr int thread_count = 5;
    constexpr int iterations = 10;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto id = "doc-" + std::to_string(thread_index) + "-" + std::to_string(iteration);
          auto document = make_concurrent_document(id);
          auto chunk = make_concurrent_chunk(document, iteration);
          memory_store.upsert_documents({document});
          memory_store.upsert_chunks({chunk});
          text_index.upsert({chunk});
          vector_index.upsert({chunk});
          (void)text_index.search(agent::KnowledgeTextSearchOptions{
              .knowledge_base_id = "kb-concurrent",
              .tenant_id = "tenant-a",
              .query = "native concurrent",
              .top_k = 3,
          });
          (void)vector_index.search(agent::KnowledgeVectorSearchOptions{
              .knowledge_base_id = "kb-concurrent",
              .tenant_id = "tenant-a",
              .embedding = {1.0, static_cast<double>(iteration), 0.0},
              .top_k = 3,
              .min_score = 0.0,
          });
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    SMOKE_CHECK(memory_store.stats().document_count == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(memory_store.stats().chunk_count == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(text_index.stats().chunk_count == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(vector_index.stats().chunk_count == static_cast<std::size_t>(thread_count * iterations));

    const auto concurrent_knowledge_file = persistent_dir / "knowledge-concurrent.json";
    std::filesystem::remove(concurrent_knowledge_file);
    agent::FileKnowledgeStore file_store(concurrent_knowledge_file);
    agent::SqliteKnowledgeVectorIndex sqlite_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = persistent_dir / "knowledge-vectors-concurrent.sqlite",
        .dimensions = 3,
        .namespace_id = "kb-concurrent",
    });
    std::filesystem::remove(persistent_dir / "knowledge-vectors-concurrent.sqlite");
    workers.clear();
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto id = "file-doc-" + std::to_string(thread_index) + "-" + std::to_string(iteration);
          auto document = make_concurrent_document(id);
          auto chunk = make_concurrent_chunk(document, iteration);
          file_store.upsert_documents({document});
          file_store.upsert_chunks({chunk});
          sqlite_index.upsert({chunk});
          SMOKE_CHECK(file_store.stats().document_count >= 1);
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    agent::FileKnowledgeStore reloaded_store(concurrent_knowledge_file);
    SMOKE_CHECK(reloaded_store.stats().document_count == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(reloaded_store.stats().chunk_count == static_cast<std::size_t>(thread_count * iterations));
    agent::SqliteKnowledgeVectorIndex reloaded_sqlite_index(agent::SqliteKnowledgeVectorIndexConfig{
        .file_path = persistent_dir / "knowledge-vectors-concurrent.sqlite",
        .dimensions = 3,
        .namespace_id = "kb-concurrent",
    });
    SMOKE_CHECK(reloaded_sqlite_index.stats().chunk_count == static_cast<std::size_t>(thread_count * iterations));

    agent::InMemoryKnowledgeSyncStateStore memory_sync;
    agent::FileKnowledgeSyncStateStore file_sync(persistent_dir / "knowledge-sync-concurrent.json");
    std::filesystem::remove(persistent_dir / "knowledge-sync-concurrent.json");
    workers.clear();
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        agent::KnowledgeSyncState state;
        state.document_hashes["sync://" + std::to_string(thread_index)] = std::to_string(thread_index);
        state.synced_at = "thread-" + std::to_string(thread_index);
        memory_sync.save(state);
        file_sync.save(state);
        SMOKE_CHECK(!memory_sync.load().synced_at.empty());
        SMOKE_CHECK(!file_sync.load().synced_at.empty());
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    SMOKE_CHECK(!memory_sync.load().document_hashes.empty());
    SMOKE_CHECK(!file_sync.load().document_hashes.empty());
  }
  {
    agent::KnowledgeBase concurrent_kb("kb-runtime-concurrent", "tenant-a", "Concurrent KB");
    constexpr int thread_count = 5;
    constexpr int iterations = 8;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto suffix = std::to_string(thread_index) + "-" + std::to_string(iteration);
          auto result = concurrent_kb.ingest_loaded_documents({
              agent::LoadedKnowledgeDocument{
                  .uri = "doc://runtime-concurrent/" + suffix,
                  .title = "Runtime Concurrent " + suffix,
                  .content = "runtime concurrent native knowledge " + suffix,
                  .source_type = "text",
              },
          });
          SMOKE_CHECK(result.document_count == 1);
          (void)concurrent_kb.search("runtime concurrent native", agent::KnowledgeSearchOptions{
              .top_k = 3,
              .min_score = 0.0,
          });
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    SMOKE_CHECK(concurrent_kb.stats().document_count == static_cast<std::size_t>(thread_count * iterations));
    auto concurrent_hits = concurrent_kb.search("runtime concurrent native", agent::KnowledgeSearchOptions{
        .top_k = 5,
        .min_score = 0.0,
    });
    SMOKE_CHECK(!concurrent_hits.empty());
  }
  {
    const auto concurrent_manager_dir = persistent_dir / "knowledge-manager-concurrent";
    std::filesystem::remove_all(concurrent_manager_dir);
    agent::KnowledgeBaseManager concurrent_manager(
        concurrent_manager_dir,
        std::make_shared<agent::HashEmbeddingAdapter>(),
        std::make_shared<agent::HashImageEmbeddingAdapter>(),
        std::make_shared<agent::TextKnowledgeSourceLoader>());
    constexpr int thread_count = 4;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        const auto id = "managed-concurrent-" + std::to_string(thread_index);
        concurrent_manager.create_knowledge_base(agent::ManagedKnowledgeBaseConfig{
            .id = id,
            .tenant_id = "tenant-a",
            .title = "Managed Concurrent " + std::to_string(thread_index),
        });
        concurrent_manager.ingest(
            id,
            {
                agent::Value::object({
                    {"type", "text"},
                    {"uri", "doc://managed-concurrent/" + std::to_string(thread_index)},
                    {"title", "Managed Concurrent"},
                    {"text", "managed concurrent native knowledge " + std::to_string(thread_index)},
                }),
            },
            "tenant-a");
        agent::ManagerSearchOptions search_options;
        search_options.tenant_id = "tenant-a";
        search_options.top_k = 3;
        search_options.min_score = 0.0;
        auto result = concurrent_manager.search_with_debug("managed concurrent native", search_options);
        SMOKE_CHECK(!result.debug.at("searchedKnowledgeBases").as_array().empty());
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    auto definitions = concurrent_manager.list_knowledge_bases("tenant-a");
    SMOKE_CHECK(definitions.size() == static_cast<std::size_t>(thread_count));
    agent::ManagerSearchOptions search_options;
    search_options.tenant_id = "tenant-a";
    search_options.top_k = 4;
    search_options.min_score = 0.0;
    auto manager_hits = concurrent_manager.search("managed concurrent native", search_options);
    SMOKE_CHECK(!manager_hits.empty());
  }

  const auto knowledge_store_file = persistent_dir / "knowledge.json";
  std::size_t persisted_knowledge_chunks = 0;
  {
    auto disk_knowledge_store = std::make_shared<agent::FileKnowledgeStore>(knowledge_store_file);
    agent::KnowledgeBase disk_kb("disk-kb", "default", "Disk Knowledge",
                                 std::make_shared<agent::HashEmbeddingAdapter>(),
                                 agent::RecursiveTextChunker{},
                                 std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                 disk_knowledge_store);
    auto disk_ingestion = disk_kb.ingest_loaded_documents({
        agent::LoadedKnowledgeDocument{
            .uri = "disk://knowledge",
            .title = "Persistent Knowledge",
            .content = "Persistent file knowledge store keeps chunks and documents on disk.",
        },
    });
    SMOKE_CHECK(disk_ingestion.document_count == 1);
    SMOKE_CHECK(disk_kb.stats().document_count == 1);
    persisted_knowledge_chunks = disk_kb.stats().chunk_count;
    SMOKE_CHECK(persisted_knowledge_chunks >= 1);
    SMOKE_CHECK(std::filesystem::exists(knowledge_store_file));
  }
  {
    auto disk_knowledge_store = std::make_shared<agent::FileKnowledgeStore>(knowledge_store_file);
    SMOKE_CHECK(disk_knowledge_store->stats().document_count == 1);
    SMOKE_CHECK(disk_knowledge_store->stats().chunk_count == persisted_knowledge_chunks);
    agent::KnowledgeBase disk_kb("disk-kb", "default", "Disk Knowledge",
                                 std::make_shared<agent::HashEmbeddingAdapter>(),
                                 agent::RecursiveTextChunker{},
                                 std::make_shared<agent::HeuristicKnowledgeReranker>(),
                                 disk_knowledge_store);
    auto disk_hits = disk_kb.search("persistent file knowledge chunks",
                                    agent::KnowledgeSearchOptions{.top_k = 1, .min_score = 0.0});
    SMOKE_CHECK(!disk_hits.empty());
    SMOKE_CHECK(disk_hits.front().document.title == "Persistent Knowledge");
    auto chunks = disk_knowledge_store->list_chunks();
    SMOKE_CHECK(!chunks.empty());
    SMOKE_CHECK(disk_knowledge_store->delete_chunks({chunks.front().id}) == 1);
    persisted_knowledge_chunks -= 1;
  }
  {
    agent::FileKnowledgeStore disk_knowledge_store(knowledge_store_file);
    SMOKE_CHECK(disk_knowledge_store.stats().document_count == 1);
    SMOKE_CHECK(disk_knowledge_store.stats().chunk_count == persisted_knowledge_chunks);
  }

  const auto sync_state_file = persistent_dir / "knowledge-sync-state.json";
  {
    agent::KnowledgeBase sync_delete_kb("sync-delete-kb", "default", "Sync Delete KB");
    agent::FileKnowledgeSyncStateStore file_sync_state(sync_state_file);
    agent::KnowledgeSyncJob sync_delete_job(sync_delete_kb, text_knowledge_loader, file_sync_state);
    sync_delete_job.sync({
        agent::Value::object({{"type", "text"}, {"uri", "note://keep"}, {"title", "Keep"}, {"text", "keep me"}}),
        agent::Value::object({{"type", "text"}, {"uri", "note://drop"}, {"title", "Drop"}, {"text", "drop me"}}),
    });
    auto delete_missing_result = sync_delete_job.sync({
        agent::Value::object({{"type", "text"}, {"uri", "note://keep"}, {"title", "Keep"}, {"text", "keep me"}}),
    }, agent::KnowledgeSyncOptions{.delete_missing = true});
    SMOKE_CHECK(delete_missing_result.deleted_document_count == 1);
    SMOKE_CHECK((delete_missing_result.deleted_uris == std::vector<std::string>{"note://drop"}));
    auto remaining_documents = sync_delete_kb.store()->list_documents();
    SMOKE_CHECK(remaining_documents.size() == 1);
    SMOKE_CHECK(remaining_documents.front().uri == "note://keep");
  }
  {
    agent::FileKnowledgeSyncStateStore file_sync_state(sync_state_file);
    auto state = file_sync_state.load();
    SMOKE_CHECK(state.document_hashes.contains("note://keep"));
    SMOKE_CHECK(!state.document_hashes.contains("note://drop"));
    SMOKE_CHECK(!state.synced_at.empty());
  }

  const auto knowledge_manager_dir = persistent_dir / "knowledge-manager";
  {
    agent::KnowledgeBaseManager manager(
        knowledge_manager_dir,
        std::make_shared<agent::HashEmbeddingAdapter>(),
        std::make_shared<agent::HashImageEmbeddingAdapter>(),
        std::make_shared<agent::TextKnowledgeSourceLoader>());
    auto managed = manager.create_knowledge_base(agent::ManagedKnowledgeBaseConfig{
        .id = "managed",
        .tenant_id = "tenant-a",
        .title = "Managed KB",
        .description = "Persistent managed knowledge base",
    });
    SMOKE_CHECK(managed->id() == "managed");
    SMOKE_CHECK(std::filesystem::exists(manager.manifest_path()));
    auto managed_ingestion = manager.ingest("managed", {
        agent::Value::object({{"type", "text"},
                             {"uri", "note://managed"},
                             {"title", "Managed Note"},
                             {"text", "Managed knowledge base search works."}}),
    }, "tenant-a");
    SMOKE_CHECK(managed_ingestion.document_count == 1);
    auto definitions = manager.list_knowledge_bases("tenant-a");
    SMOKE_CHECK(definitions.size() == 1);
    SMOKE_CHECK(!definitions.front().store_file_path.empty());
  }
  {
    agent::KnowledgeBaseManager reloaded_manager(
        knowledge_manager_dir,
        std::make_shared<agent::HashEmbeddingAdapter>(),
        std::make_shared<agent::HashImageEmbeddingAdapter>(),
        std::make_shared<agent::TextKnowledgeSourceLoader>());
    auto loaded_managed = reloaded_manager.get_knowledge_base("managed", "tenant-a");
    SMOKE_CHECK(loaded_managed);
    SMOKE_CHECK(loaded_managed->stats().document_count == 1);
    agent::ManagerSearchOptions manager_search_options;
    manager_search_options.tenant_id = "tenant-a";
    manager_search_options.top_k = 2;
    manager_search_options.min_score = 0.0;
    auto manager_search = reloaded_manager.search_with_debug("managed search", manager_search_options);
    SMOKE_CHECK(!manager_search.hits.empty());
    SMOKE_CHECK(manager_search.debug.at("searchedKnowledgeBases").as_array().size() == 1);
    auto manager_context = reloaded_manager.build_context_message("managed search", manager_search_options);
    SMOKE_CHECK(manager_context.message);
    SMOKE_CHECK(agent::extract_text_content(manager_context.message->content).find("Managed Note") != std::string::npos);
    SMOKE_CHECK(reloaded_manager.delete_knowledge_base("managed", "tenant-a"));
    SMOKE_CHECK(!reloaded_manager.get_knowledge_base("managed", "tenant-a"));
    SMOKE_CHECK(!std::filesystem::exists(knowledge_manager_dir / "tenant-a" / "managed"));
  }

  const auto task_file = persistent_dir / "tasks.json";
  std::string disk_task_id;
  {
    agent::FileTaskStore tasks(agent::FileTaskStoreConfig{.file_path = task_file});
    agent::InMemoryTaskQueue queue(agent::InMemoryTaskQueueOptions{.store = &tasks, .lease_ms = 45000});
    auto disk_task = tasks.create_task(agent::CreateTaskInput{
        .type = "disk-task",
        .input = agent::Value::object({{"text", "stored task"}}),
    });
    disk_task_id = disk_task.id;
    queue.enqueue(disk_task.id);
    auto claim = queue.claim();
    SMOKE_CHECK(claim);
    tasks.append_event(disk_task.id, claim->run.id, "claimed", agent::Value::object({{"ok", true}}));
    queue.complete(disk_task.id, claim->run.id, agent::Value::object({{"done", true}}));
  }
  {
    agent::FileTaskStore tasks(agent::FileTaskStoreConfig{.file_path = task_file});
    auto snapshot = tasks.get_task(disk_task_id);
    SMOKE_CHECK(snapshot);
    SMOKE_CHECK(snapshot->task.status == agent::TaskStatus::Completed);
    SMOKE_CHECK(!snapshot->runs.empty());
    SMOKE_CHECK(!snapshot->events.empty());
  }

  const auto autonomous_file = persistent_dir / "autonomous.json";
  {
    agent::FileAutonomousStore autonomous_store(autonomous_file);
    agent::StaticAutonomousPlanner autonomous_planner(agent::AutonomousPlan{
        .summary = "Autonomous smoke plan",
        .steps = {
            agent::AutonomousPlanStep{
                .id = "step-one",
                .title = "First step",
                .objective = "Produce first output",
                .metadata = agent::Value::object({{"keep", true}}),
            },
            agent::AutonomousPlanStep{
                .id = "step-two",
                .title = "Second step",
                .objective = "Use first output",
                .depends_on = {"step-one"},
            },
        },
    });
    std::vector<std::string> autonomous_executed;
    agent::CallbackAutonomousStepExecutor autonomous_executor(
        [&](const agent::AutonomousStepExecutionInput& input) {
          autonomous_executed.push_back(input.step.id);
          auto checkpoint = input.checkpoint(
              "executor.state",
              agent::Value::object({{"stepId", input.step.id}, {"previous", input.previous_steps.size()}}));
          SMOKE_CHECK(checkpoint.name == "executor.state");
          return agent::AutonomousStepExecutionResult{
              .output = agent::Value::object({{"text", input.step.title + " done"}}),
              .metadata = agent::Value::object({{"executed", true}}),
          };
        });
    agent::AutonomousRunManager autonomous_manager(agent::AutonomousRunManagerConfig{
        .store = &autonomous_store,
        .planner = &autonomous_planner,
        .executor = &autonomous_executor,
        .max_steps_per_run = 8,
    });
    auto autonomous_run = autonomous_manager.create_run(agent::CreateAutonomousRunInput{
        .id = "auto-run-1",
        .goal = "Run autonomous smoke",
        .input = agent::Value::object({{"mode", "test"}}),
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    SMOKE_CHECK(autonomous_run.status == agent::AutonomousRunStatus::Queued);
    auto autonomous_snapshot = autonomous_manager.run(autonomous_run.id);
    SMOKE_CHECK(autonomous_snapshot.run.status == agent::AutonomousRunStatus::Completed);
    SMOKE_CHECK((autonomous_executed == std::vector<std::string>{"step-one", "step-two"}));
    SMOKE_CHECK(autonomous_snapshot.steps.size() == 2);
    SMOKE_CHECK(autonomous_snapshot.steps.front().metadata.at("keep").as_bool());
    SMOKE_CHECK(autonomous_snapshot.steps.front().metadata.at("executed").as_bool());
    SMOKE_CHECK(!autonomous_snapshot.events.empty());
    SMOKE_CHECK(!autonomous_snapshot.checkpoints.empty());
    auto serialized_autonomous = agent::autonomous_run_snapshot_to_value(autonomous_snapshot);
    SMOKE_CHECK(serialized_autonomous.at("run").at("status").as_string() == "completed");
    auto filtered_runs = autonomous_store.list_runs(agent::AutonomousRunFilter{
        .status = agent::AutonomousRunStatus::Completed,
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    SMOKE_CHECK(filtered_runs.size() == 1);
  }
  {
    agent::FileAutonomousStore reloaded_autonomous_store(autonomous_file);
    auto reloaded_autonomous = reloaded_autonomous_store.get_run("auto-run-1");
    SMOKE_CHECK(reloaded_autonomous);
    SMOKE_CHECK(reloaded_autonomous->run.status == agent::AutonomousRunStatus::Completed);
    SMOKE_CHECK(reloaded_autonomous->steps.size() == 2);
  }
  {
    agent::InMemoryAutonomousStore concurrent_autonomous_store;
    auto concurrent_run = concurrent_autonomous_store.create_run(agent::CreateAutonomousRunInput{
        .id = "concurrent-auto",
        .goal = "Concurrent autonomous store",
        .metadata = agent::Value::object({{"kind", "concurrent"}}),
    });
    std::vector<std::thread> autonomous_threads;
    for (int thread_index = 0; thread_index < 8; ++thread_index) {
      autonomous_threads.emplace_back([&, thread_index]() {
        for (int step_index = 0; step_index < 10; ++step_index) {
          const std::string suffix = std::to_string(thread_index) + "-" + std::to_string(step_index);
          auto appended = concurrent_autonomous_store.append_steps(concurrent_run.id, {
              agent::AutonomousPlanStep{
                  .id = "concurrent-step-" + suffix,
                  .title = "Concurrent step " + suffix,
              },
          });
          SMOKE_CHECK(appended.size() == 1);
          concurrent_autonomous_store.append_event(agent::AutonomousEvent{
              .run_id = concurrent_run.id,
              .step_id = appended.front().id,
              .type = "autonomous.step.concurrent",
              .payload = agent::Value::object({{"suffix", suffix}}),
          });
          concurrent_autonomous_store.append_checkpoint(agent::AutonomousCheckpoint{
              .run_id = concurrent_run.id,
              .step_id = appended.front().id,
              .name = "concurrent.checkpoint",
              .state = agent::Value::object({{"suffix", suffix}}),
          });
        }
      });
    }
    for (auto& thread : autonomous_threads) {
      thread.join();
    }
    auto concurrent_snapshot = concurrent_autonomous_store.get_run(concurrent_run.id);
    SMOKE_CHECK(concurrent_snapshot);
    SMOKE_CHECK(concurrent_snapshot->steps.size() == 80);
    SMOKE_CHECK(concurrent_snapshot->events.size() == 80);
    SMOKE_CHECK(concurrent_snapshot->checkpoints.size() == 80);
    auto concurrent_filtered = concurrent_autonomous_store.list_runs(agent::AutonomousRunFilter{
        .metadata = agent::Value::object({{"kind", "concurrent"}}),
    });
    SMOKE_CHECK(concurrent_filtered.size() == 1);

    agent::FileAutonomousStore concurrent_file_autonomous_store(persistent_dir / "autonomous-concurrent.json");
    std::vector<std::string> file_autonomous_run_ids;
    std::mutex file_autonomous_ids_mutex;
    std::vector<std::thread> file_autonomous_threads;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
      file_autonomous_threads.emplace_back([&, thread_index]() {
        for (int run_index = 0; run_index < 5; ++run_index) {
          const std::string run_id = "file-auto-" + std::to_string(thread_index) + "-" + std::to_string(run_index);
          auto run = concurrent_file_autonomous_store.create_run(agent::CreateAutonomousRunInput{
              .id = run_id,
              .goal = "Concurrent file autonomous",
              .metadata = agent::Value::object({{"thread", thread_index}}),
          });
          auto steps = concurrent_file_autonomous_store.replace_steps(run.id, {
              agent::AutonomousPlanStep{
                  .id = "file-step-" + std::to_string(thread_index) + "-" + std::to_string(run_index),
                  .title = "File step",
              },
          });
          SMOKE_CHECK(steps.size() == 1);
          concurrent_file_autonomous_store.update_run(run.id, agent::AutonomousRunPatch{
              .status = agent::AutonomousRunStatus::Completed,
              .output = agent::Value::object({{"thread", thread_index}, {"run", run_index}}),
              .has_output = true,
              .completed_at = agent::now_iso8601(),
              .has_completed_at = true,
          });
          auto loaded = concurrent_file_autonomous_store.get_run(run.id);
          SMOKE_CHECK(loaded);
          SMOKE_CHECK(loaded->run.status == agent::AutonomousRunStatus::Completed);
          SMOKE_CHECK(loaded->steps.size() == 1);
          std::lock_guard<std::mutex> lock(file_autonomous_ids_mutex);
          file_autonomous_run_ids.push_back(run.id);
        }
      });
    }
    for (auto& thread : file_autonomous_threads) {
      thread.join();
    }
    SMOKE_CHECK(file_autonomous_run_ids.size() == 20);
    agent::FileAutonomousStore reloaded_concurrent_file_autonomous_store(persistent_dir / "autonomous-concurrent.json");
    for (const auto& run_id : file_autonomous_run_ids) {
      auto loaded = reloaded_concurrent_file_autonomous_store.get_run(run_id);
      SMOKE_CHECK(loaded);
      SMOKE_CHECK(loaded->run.status == agent::AutonomousRunStatus::Completed);
    }
  }
  {
    auto pg_autonomous_client = std::make_shared<FakePgAutonomousClient>();
    agent::PgAutonomousStore pg_autonomous_store(agent::PgAutonomousStoreConfig{
        .client = pg_autonomous_client,
        .create_tables = true,
        .close_client_on_close = true,
    });
    auto pg_run = pg_autonomous_store.create_run(agent::CreateAutonomousRunInput{
        .id = "pg-auto-1",
        .goal = "Run pg autonomous smoke",
        .input = agent::Value::object({{"mode", "pg"}}),
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    SMOKE_CHECK(pg_run.id == "pg-auto-1");
    SMOKE_CHECK(pg_run.input.at("mode").as_string() == "pg");
    SMOKE_CHECK(pg_run.metadata.at("tenant").as_string() == "native");
    SMOKE_CHECK(pg_autonomous_client->queries.size() >= 7);
    SMOKE_CHECK(pg_autonomous_client->queries.front().sql.find("CREATE SCHEMA IF NOT EXISTS") != std::string::npos);

    auto pg_steps = pg_autonomous_store.replace_steps("pg-auto-1", {
        agent::AutonomousPlanStep{
            .id = "pg-step-1",
            .title = "PG Step",
            .objective = "Persist the first step",
            .input = agent::Value::object({{"n", 1}}),
            .metadata = agent::Value::object({{"kind", "first"}}),
        },
    });
    SMOKE_CHECK(pg_steps.size() == 1);
    SMOKE_CHECK(pg_steps.front().index == 0);
    SMOKE_CHECK(pg_steps.front().input.at("n").as_integer() == 1);
    const auto pg_begin = std::find_if(
        pg_autonomous_client->queries.begin(),
        pg_autonomous_client->queries.end(),
        [](const FakePgAutonomousClient::QueryRecord& record) { return record.sql == "BEGIN"; });
    SMOKE_CHECK(pg_begin != pg_autonomous_client->queries.end());
    SMOKE_CHECK(std::any_of(pg_autonomous_client->queries.begin(), pg_autonomous_client->queries.end(),
                       [](const FakePgAutonomousClient::QueryRecord& record) {
                         return record.sql == "COMMIT";
                       }));

    auto pg_more_steps = pg_autonomous_store.append_steps("pg-auto-1", {
        agent::AutonomousPlanStep{
            .id = "pg-step-2",
            .title = "PG Step 2",
            .objective = "Append after count",
            .depends_on = {"pg-step-1"},
        },
    });
    SMOKE_CHECK(pg_more_steps.size() == 1);
    SMOKE_CHECK(pg_more_steps.front().index == 1);
    SMOKE_CHECK((pg_more_steps.front().depends_on == std::vector<std::string>{"pg-step-1"}));

    auto pg_running_step = pg_autonomous_store.update_step("pg-auto-1", "pg-step-1", agent::AutonomousStepPatch{
        .status = agent::AutonomousStepStatus::Running,
        .started_at = "2026-05-24T00:00:00.000Z",
        .has_started_at = true,
        .attempts = 1,
    });
    SMOKE_CHECK(pg_running_step.status == agent::AutonomousStepStatus::Running);
    SMOKE_CHECK(pg_running_step.started_at == "2026-05-24T00:00:00.000Z");
    SMOKE_CHECK(pg_running_step.attempts == 1);

    auto pg_event = pg_autonomous_store.append_event(agent::AutonomousEvent{
        .run_id = "pg-auto-1",
        .step_id = "pg-step-1",
        .type = "autonomous.step.started",
        .payload = agent::Value::object({{"attempt", 1}}),
    });
    SMOKE_CHECK(pg_event.type == "autonomous.step.started");
    auto pg_checkpoint = pg_autonomous_store.append_checkpoint(agent::AutonomousCheckpoint{
        .run_id = "pg-auto-1",
        .step_id = "pg-step-1",
        .name = "step.started",
        .state = agent::Value::object({{"status", "running"}}),
    });
    SMOKE_CHECK(pg_checkpoint.state.at("status").as_string() == "running");

    auto pg_completed_run = pg_autonomous_store.update_run("pg-auto-1", agent::AutonomousRunPatch{
        .status = agent::AutonomousRunStatus::Completed,
        .output = agent::Value::object({{"ok", true}}),
        .has_output = true,
        .completed_at = "2026-05-24T00:00:01.000Z",
        .has_completed_at = true,
    });
    SMOKE_CHECK(pg_completed_run.status == agent::AutonomousRunStatus::Completed);
    SMOKE_CHECK(pg_completed_run.output.at("ok").as_bool());

    auto pg_snapshot = pg_autonomous_store.get_run("pg-auto-1");
    SMOKE_CHECK(pg_snapshot);
    SMOKE_CHECK(pg_snapshot->steps.size() == 2);
    SMOKE_CHECK(pg_snapshot->events.size() == 1);
    SMOKE_CHECK(pg_snapshot->checkpoints.size() == 1);

    auto pg_filtered = pg_autonomous_store.list_runs(agent::AutonomousRunFilter{
        .status = agent::AutonomousRunStatus::Completed,
        .metadata = agent::Value::object({{"tenant", "native"}}),
    });
    SMOKE_CHECK(pg_filtered.size() == 1);
    const auto has_metadata_filter = std::any_of(
        pg_autonomous_client->queries.begin(),
        pg_autonomous_client->queries.end(),
        [](const FakePgAutonomousClient::QueryRecord& record) {
          return record.sql.find("metadata @>") != std::string::npos;
        });
    SMOKE_CHECK(has_metadata_filter);
    pg_autonomous_store.close();
    SMOKE_CHECK(pg_autonomous_client->closed);
  }
  {
    auto pg_autonomous_ready_client = std::make_shared<FakePgAutonomousClient>();
    agent::PgAutonomousStore pg_autonomous_ready_store(agent::PgAutonomousStoreConfig{
        .client = pg_autonomous_ready_client,
        .create_tables = true,
        .close_client_on_close = true,
    });

    constexpr int autonomous_ready_thread_count = 8;
    std::atomic<int> autonomous_ready_threads_waiting{0};
    std::atomic<bool> autonomous_ready_start{false};
    std::vector<std::thread> autonomous_ready_threads;
    for (int i = 0; i < autonomous_ready_thread_count; ++i) {
      autonomous_ready_threads.emplace_back([&]() {
        autonomous_ready_threads_waiting.fetch_add(1, std::memory_order_acq_rel);
        while (!autonomous_ready_start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        auto runs = pg_autonomous_ready_store.list_runs(agent::AutonomousRunFilter{});
        SMOKE_CHECK(runs.empty());
      });
    }
    while (autonomous_ready_threads_waiting.load(std::memory_order_acquire) != autonomous_ready_thread_count) {
      std::this_thread::yield();
    }
    autonomous_ready_start.store(true, std::memory_order_release);
    for (auto& thread : autonomous_ready_threads) {
      thread.join();
    }

    const auto autonomous_schema_creates = std::count_if(
        pg_autonomous_ready_client->queries.begin(),
        pg_autonomous_ready_client->queries.end(),
        [](const FakePgAutonomousClient::QueryRecord& record) {
          return record.sql.find("CREATE SCHEMA IF NOT EXISTS") != std::string::npos;
        });
    const auto autonomous_table_creates = std::count_if(
        pg_autonomous_ready_client->queries.begin(),
        pg_autonomous_ready_client->queries.end(),
        [](const FakePgAutonomousClient::QueryRecord& record) {
          return record.sql.find("CREATE TABLE IF NOT EXISTS") != std::string::npos;
        });
    const auto autonomous_index_creates = std::count_if(
        pg_autonomous_ready_client->queries.begin(),
        pg_autonomous_ready_client->queries.end(),
        [](const FakePgAutonomousClient::QueryRecord& record) {
          return record.sql.find("CREATE INDEX IF NOT EXISTS") != std::string::npos;
        });
    SMOKE_CHECK(autonomous_schema_creates == 1);
    SMOKE_CHECK(autonomous_table_creates == 4);
    SMOKE_CHECK(autonomous_index_creates == 3);

    constexpr int autonomous_close_thread_count = 4;
    std::atomic<int> autonomous_close_threads_waiting{0};
    std::atomic<bool> autonomous_close_start{false};
    std::vector<std::thread> autonomous_close_threads;
    for (int i = 0; i < autonomous_close_thread_count; ++i) {
      autonomous_close_threads.emplace_back([&]() {
        autonomous_close_threads_waiting.fetch_add(1, std::memory_order_acq_rel);
        while (!autonomous_close_start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        pg_autonomous_ready_store.close();
      });
    }
    while (autonomous_close_threads_waiting.load(std::memory_order_acquire) != autonomous_close_thread_count) {
      std::this_thread::yield();
    }
    autonomous_close_start.store(true, std::memory_order_release);
    for (auto& thread : autonomous_close_threads) {
      thread.join();
    }
    pg_autonomous_ready_store.close();
    SMOKE_CHECK(pg_autonomous_ready_client->closed);
    SMOKE_CHECK(pg_autonomous_ready_client->close_count == 1);
  }
