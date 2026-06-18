namespace {

std::filesystem::path normalized_absolute_path(const std::filesystem::path& path) {
  std::error_code ec;
  auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    normalized = std::filesystem::absolute(path, ec);
    if (ec) {
      normalized = path;
    }
  }
  return normalized.lexically_normal();
}

bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& root) {
  auto candidate_it = candidate.begin();
  auto root_it = root.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *candidate_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::filesystem::path require_path_within_base(const std::filesystem::path& base_dir,
                                               const std::filesystem::path& candidate,
                                               const std::string& label) {
  if (base_dir.empty()) {
    return candidate;
  }
  const auto root = normalized_absolute_path(base_dir);
  const auto resolved = normalized_absolute_path(candidate);
  if (!path_is_within(resolved, root)) {
    throw ConfigurationError(label + " is outside the knowledge manager base directory.");
  }
  return resolved;
}

std::string knowledge_path_segment(const std::string& value, const std::string& fallback) {
  auto segment = sanitize_segment(value.empty() ? fallback : value);
  if (segment == "." || segment == "..") {
    segment = "_" + segment;
  }
  return segment;
}

}  // namespace

KnowledgeBaseManager::KnowledgeBaseManager(std::filesystem::path base_dir,
                                           std::shared_ptr<TextEmbeddingAdapter> embedder,
                                           std::shared_ptr<ImageEmbeddingAdapter> image_embedder,
                                           std::shared_ptr<KnowledgeSourceLoader> loader,
                                           std::shared_ptr<KnowledgeReranker> reranker,
                                           std::shared_ptr<KnowledgeTextIndex> text_index,
                                           std::shared_ptr<KnowledgeVectorIndex> vector_index,
                                           RecursiveTextChunker chunker)
    : base_dir_(std::move(base_dir)),
      embedder_(std::move(embedder)),
      image_embedder_(std::move(image_embedder)),
      loader_(std::move(loader)),
      reranker_(std::move(reranker)),
      text_index_(std::move(text_index)),
      vector_index_(std::move(vector_index)),
      chunker_(std::move(chunker)) {
  if (!embedder_) {
    embedder_ = std::make_shared<HashEmbeddingAdapter>();
  }
  if (!image_embedder_) {
    image_embedder_ = std::make_shared<HashImageEmbeddingAdapter>();
  }
  if (!loader_) {
    loader_ = std::make_shared<CompositeKnowledgeLoader>();
  }
  if (!reranker_) {
    reranker_ = std::make_shared<HeuristicKnowledgeReranker>();
  }
  if (!text_index_) {
    text_index_ = std::make_shared<InMemoryKnowledgeTextIndex>();
  }
}

std::filesystem::path KnowledgeBaseManager::manifest_path() const {
  return base_dir_ / "knowledge-bases.json";
}

std::vector<KnowledgeBaseDefinition> KnowledgeBaseManager::load_definitions() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!base_dir_.empty() && std::filesystem::exists(manifest_path())) {
    const auto raw = read_json_file(manifest_path());
    for (const auto& item : raw.at("knowledgeBases").as_array()) {
      auto definition = knowledge_base_definition_from_value(item);
      if (!definition.id.empty()) {
        definitions_[key(definition.tenant_id, definition.id)] = std::move(definition);
      }
    }
  }

  std::vector<KnowledgeBaseDefinition> definitions;
  definitions.reserve(definitions_.size());
  for (const auto& [_, definition] : definitions_) {
    definitions.push_back(definition);
  }
  return definitions;
}

std::vector<KnowledgeBaseDefinition> KnowledgeBaseManager::list_knowledge_bases(std::string tenant_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto definitions = load_definitions();
  if (tenant_id.empty()) {
    return definitions;
  }
  std::vector<KnowledgeBaseDefinition> filtered;
  for (auto& definition : definitions) {
    if (definition.tenant_id == tenant_id) {
      filtered.push_back(std::move(definition));
    }
  }
  return filtered;
}

std::shared_ptr<KnowledgeBase> KnowledgeBaseManager::create_knowledge_base(ManagedKnowledgeBaseConfig config) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (config.id.empty()) {
    throw ConfigurationError("Knowledge base id is required.");
  }
  if (config.tenant_id.empty()) {
    config.tenant_id = "default";
  }

  const bool persistent = config.persistent.value_or(!base_dir_.empty());
  std::string store_path;
  std::shared_ptr<KnowledgeStore> store = config.store;
  if (!store && persistent && !base_dir_.empty()) {
    const auto path = store_file_path(config.tenant_id, config.id);
    store_path = path.string();
    store = std::make_shared<FileKnowledgeStore>(path);
  }
  if (!store) {
    store = std::make_shared<InMemoryKnowledgeStore>();
  }

  auto base = std::make_shared<KnowledgeBase>(
      config.id,
      config.tenant_id,
      config.title,
      config.embedder ? config.embedder : embedder_,
      config.chunker,
      config.reranker ? config.reranker : reranker_,
      store,
      config.image_embedder ? config.image_embedder : image_embedder_,
      config.text_index ? config.text_index : text_index_,
      config.vector_index ? config.vector_index : vector_index_,
      merge_knowledge_search_options(config.retrieval_config, config.search_defaults),
      config.description,
      config.context_title,
      config.ingestion_strategy,
      config.retrieval_strategy,
      config.context_renderer);

  const auto entry_key = key(config.tenant_id, config.id);
  bases_[entry_key] = base;
  definitions_[entry_key] = KnowledgeBaseDefinition{
      .id = config.id,
      .tenant_id = config.tenant_id,
      .title = base->title(),
      .description = config.description,
      .store_file_path = store_path,
  };
  persist_definitions();
  return base;
}

std::shared_ptr<KnowledgeBase> KnowledgeBaseManager::register_knowledge_base(std::shared_ptr<KnowledgeBase> base,
                                                                             std::string description) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!base) {
    throw ConfigurationError("Knowledge base is required.");
  }
  const auto entry_key = key(base->tenant_id(), base->id());
  bases_[entry_key] = base;
  definitions_[entry_key] = KnowledgeBaseDefinition{
      .id = base->id(),
      .tenant_id = base->tenant_id(),
      .title = base->title(),
      .description = description.empty() ? base->description() : std::move(description),
  };
  persist_definitions();
  return base;
}

std::shared_ptr<KnowledgeBase> KnowledgeBaseManager::get_knowledge_base(const std::string& id,
                                                                        std::string tenant_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (tenant_id.empty()) {
    tenant_id = "default";
  }
  const auto entry_key = key(tenant_id, id);
  if (auto found = bases_.find(entry_key); found != bases_.end()) {
    return found->second;
  }
  if (!definitions_.contains(entry_key)) {
    (void)load_definitions();
  }
  const auto definition = definitions_.find(entry_key);
  if (definition == definitions_.end()) {
    return nullptr;
  }

  std::shared_ptr<KnowledgeStore> store;
  if (!definition->second.store_file_path.empty()) {
    store = std::make_shared<FileKnowledgeStore>(
        require_path_within_base(base_dir_,
                                 std::filesystem::path(definition->second.store_file_path),
                                 "Knowledge store path"));
  } else {
    store = std::make_shared<InMemoryKnowledgeStore>();
  }
  auto base = std::make_shared<KnowledgeBase>(
      definition->second.id,
      definition->second.tenant_id,
      definition->second.title,
      embedder_,
      chunker_,
      reranker_,
      store,
      image_embedder_,
      text_index_,
      vector_index_,
      KnowledgeSearchOptions{},
      definition->second.description);
  bases_[entry_key] = base;
  return base;
}

bool KnowledgeBaseManager::delete_knowledge_base(const std::string& id, std::string tenant_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (tenant_id.empty()) {
    tenant_id = "default";
  }
  const auto entry_key = key(tenant_id, id);
  if (!definitions_.contains(entry_key)) {
    (void)load_definitions();
  }
  const auto definition = definitions_.find(entry_key);
  const bool existed = definition != definitions_.end();
  std::string store_path = existed ? definition->second.store_file_path : std::string{};
  bases_.erase(entry_key);
  if (existed) {
    definitions_.erase(entry_key);
    persist_definitions();
    if (!store_path.empty()) {
      const auto confined = require_path_within_base(base_dir_, std::filesystem::path(store_path),
                                                     "Knowledge store path");
      std::filesystem::remove_all(confined.parent_path());
    }
  }
  return existed;
}

KnowledgeIngestionResult KnowledgeBaseManager::ingest(const std::string& knowledge_base_id,
                                                      const std::vector<Value>& sources,
                                                      std::string tenant_id,
                                                      KnowledgeIngestionPipelineOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto base = get_knowledge_base(knowledge_base_id, std::move(tenant_id));
  if (!base) {
    throw ConfigurationError("Knowledge base not found: " + knowledge_base_id);
  }
  KnowledgeIngestionPipeline pipeline(*base, *loader_);
  return pipeline.ingest(sources, std::move(options));
}

KnowledgeDeleteResult KnowledgeBaseManager::delete_documents(const std::string& knowledge_base_id,
                                                             const KnowledgeDeleteOptions& options,
                                                             std::string tenant_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto base = get_knowledge_base(knowledge_base_id, std::move(tenant_id));
  if (!base) {
    throw ConfigurationError("Knowledge base not found: " + knowledge_base_id);
  }
  return base->delete_documents(options);
}

KnowledgeManagerSearchResult KnowledgeBaseManager::search_with_debug(const std::string& query,
                                                                     ManagerSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  KnowledgeManagerSearchResult result;
  if (!options.enabled) {
    result.debug = Value::object({{"query", query},
                                  {"searchedKnowledgeBases", Value::array({})},
                                  {"finalHits", Value::array({})}});
    return result;
  }

  auto definitions = load_definitions();
  Value::Array searched;
  for (const auto& definition : definitions) {
    if (!options.tenant_id.empty() && definition.tenant_id != options.tenant_id) {
      continue;
    }
    if (!options.knowledge_base_ids.empty() &&
        !vector_contains(options.knowledge_base_ids, definition.id)) {
      continue;
    }
    auto base = get_knowledge_base(definition.id, definition.tenant_id);
    if (!base) {
      continue;
    }
    auto search_result = base->search_with_debug(query, options);
    auto hits = std::move(search_result.hits);
    searched.push_back(Value::object({{"knowledgeBaseId", definition.id},
                                     {"tenantId", definition.tenant_id},
                                     {"title", definition.title},
                                     {"hitCount", hits.size()},
                                     {"debug", std::move(search_result.debug)}}));
    result.hits.insert(result.hits.end(), std::make_move_iterator(hits.begin()), std::make_move_iterator(hits.end()));
  }

  sort_hits_by_rerank_score(result.hits);
  const std::size_t top_k = options.top_k == 0 ? 6 : options.top_k;
  if (result.hits.size() > top_k) {
    result.hits.resize(top_k);
  }

  result.debug = Value::object({{"query", query},
                                {"searchedKnowledgeBases", Value(searched)},
                                {"finalHits", knowledge_hits_to_value(result.hits)}});
  return result;
}

KnowledgeManagerSearchResult KnowledgeBaseManager::search_with_debug(const ImageEmbeddingInput& query,
                                                                     ManagerSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string query_text = image_embedding_query_text(query);
  KnowledgeManagerSearchResult result;
  if (!options.enabled) {
    result.debug = Value::object({{"query", query_text},
                                  {"searchedKnowledgeBases", Value::array({})},
                                  {"finalHits", Value::array({})}});
    return result;
  }

  auto definitions = load_definitions();
  Value::Array searched;
  for (const auto& definition : definitions) {
    if (!options.tenant_id.empty() && definition.tenant_id != options.tenant_id) {
      continue;
    }
    if (!options.knowledge_base_ids.empty() &&
        !vector_contains(options.knowledge_base_ids, definition.id)) {
      continue;
    }
    auto base = get_knowledge_base(definition.id, definition.tenant_id);
    if (!base) {
      continue;
    }
    auto search_result = base->search_with_debug(query, options);
    auto hits = std::move(search_result.hits);
    searched.push_back(Value::object({{"knowledgeBaseId", definition.id},
                                     {"tenantId", definition.tenant_id},
                                     {"title", definition.title},
                                     {"hitCount", hits.size()},
                                     {"debug", std::move(search_result.debug)}}));
    result.hits.insert(result.hits.end(), std::make_move_iterator(hits.begin()), std::make_move_iterator(hits.end()));
  }

  sort_hits_by_rerank_score(result.hits);
  const std::size_t top_k = options.top_k == 0 ? 6 : options.top_k;
  if (result.hits.size() > top_k) {
    result.hits.resize(top_k);
  }

  result.debug = Value::object({{"query", query_text},
                                {"searchedKnowledgeBases", Value(searched)},
                                {"finalHits", knowledge_hits_to_value(result.hits)}});
  return result;
}

std::vector<KnowledgeSearchHit> KnowledgeBaseManager::search(const std::string& query,
                                                             KnowledgeSearchOptions options) {
  ManagerSearchOptions manager_options;
  static_cast<KnowledgeSearchOptions&>(manager_options) = std::move(options);
  return search(query, std::move(manager_options));
}

std::vector<KnowledgeSearchHit> KnowledgeBaseManager::search(const std::string& query,
                                                             ManagerSearchOptions options) {
  return search_with_debug(query, std::move(options)).hits;
}

std::vector<KnowledgeSearchHit> KnowledgeBaseManager::search(const ImageEmbeddingInput& query,
                                                             ManagerSearchOptions options) {
  return search_with_debug(query, std::move(options)).hits;
}

std::optional<AgentMessage> KnowledgeBaseManager::create_context_message(
    const std::vector<KnowledgeSearchHit>& hits) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (hits.empty()) {
    return std::nullopt;
  }
  std::ostringstream content;
  content << "Knowledge base retrieval";
  for (std::size_t index = 0; index < hits.size(); ++index) {
    content << "\n" << index + 1 << ". [" << hits[index].citation.knowledge_base_title << "] "
            << hits[index].citation.title << " | " << hits[index].citation.uri
            << "\n   Snippet: " << hits[index].citation.snippet;
  }
  return create_message(MessageRole::System, content.str(),
                        Value::object({{"source", "knowledge-base-manager"}, {"hits", hits.size()}}));
}

KnowledgeContextResult KnowledgeBaseManager::build_context_message(const std::string& query,
                                                                   KnowledgeSearchOptions options) {
  ManagerSearchOptions manager_options;
  static_cast<KnowledgeSearchOptions&>(manager_options) = std::move(options);
  return build_context_message(query, std::move(manager_options));
}

KnowledgeContextResult KnowledgeBaseManager::build_context_message(const ImageEmbeddingInput& query,
                                                                   KnowledgeSearchOptions options) {
  ManagerSearchOptions manager_options;
  static_cast<KnowledgeSearchOptions&>(manager_options) = std::move(options);
  return build_context_message(query, std::move(manager_options));
}

KnowledgeManagerContextResult KnowledgeBaseManager::build_context_message(const std::string& query,
                                                                          ManagerSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeManagerContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

KnowledgeManagerContextResult KnowledgeBaseManager::build_context_message(const ImageEmbeddingInput& query,
                                                                          ManagerSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeManagerContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

std::string KnowledgeBaseManager::key(const std::string& tenant_id, const std::string& id) const {
  return tenant_id + "::" + id;
}

std::filesystem::path KnowledgeBaseManager::store_file_path(const std::string& tenant_id,
                                                            const std::string& id) const {
  const auto path = base_dir_
      / knowledge_path_segment(tenant_id, "default")
      / knowledge_path_segment(id, "knowledge-base")
      / "store.json";
  return require_path_within_base(base_dir_, path, "Knowledge store path");
}

void KnowledgeBaseManager::persist_definitions() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (base_dir_.empty()) {
    return;
  }
  Value::Array definitions;
  for (const auto& [_, definition] : definitions_) {
    definitions.push_back(knowledge_base_definition_to_value(definition));
  }
  write_json_file(manifest_path(), Value::object({{"knowledgeBases", Value(definitions)}}));
}
