void InMemoryKnowledgeTextIndex::upsert(const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& chunk : chunks) {
    if (!chunk.id.empty()) {
      chunks_[chunk.id] = chunk;
    }
  }
}

std::vector<KnowledgeTextMatch> InMemoryKnowledgeTextIndex::search(const KnowledgeTextSearchOptions& options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (options.top_k == 0) {
    return {};
  }
  const auto query_tokens = tokenize_knowledge_text(options.query);
  if (query_tokens.empty()) {
    return {};
  }

  std::vector<KnowledgeChunkRecord> chunks;
  std::vector<std::vector<std::string>> documents;
  for (const auto& [_, chunk] : chunks_) {
    if (!chunk_matches_text_search(chunk, options)) {
      continue;
    }
    chunks.push_back(chunk);
    documents.push_back(tokenize_knowledge_text(knowledge_text_index_surface(chunk)));
  }
  if (chunks.empty()) {
    return {};
  }

  double total_length = 0;
  std::map<std::string, std::size_t> document_frequency;
  for (const auto& tokens : documents) {
    total_length += static_cast<double>(tokens.size());
    std::set<std::string> unique_tokens(tokens.begin(), tokens.end());
    for (const auto& token : unique_tokens) {
      ++document_frequency[token];
    }
  }
  const double average_length = total_length / std::max<std::size_t>(documents.size(), 1);

  std::vector<KnowledgeTextMatch> matches;
  matches.reserve(chunks.size());
  const double k1 = 1.2;
  const double b = 0.75;
  double max_score = 0;
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    const auto& tokens = documents[index];
    const auto frequencies = build_term_frequency(tokens);
    double score = 0;
    for (const auto& token : query_tokens) {
      const auto tf_found = frequencies.find(token);
      if (tf_found == frequencies.end() || tf_found->second == 0) {
        continue;
      }
      const double tf = static_cast<double>(tf_found->second);
      const double doc_frequency = static_cast<double>(document_frequency[token]);
      const double idf = std::log((static_cast<double>(documents.size()) - doc_frequency + 0.5) /
                                      (doc_frequency + 0.5) +
                                  1.0);
      const double length_norm = tf + k1 * (1.0 - b + b * (static_cast<double>(tokens.size()) /
                                                           std::max(average_length, 1.0)));
      score += idf * ((tf * (k1 + 1.0)) / length_norm);
    }
    max_score = std::max(max_score, score);
    matches.push_back(KnowledgeTextMatch{chunks[index].id, score});
  }

  if (max_score > 0) {
    for (auto& match : matches) {
      match.score /= max_score;
    }
  }
  matches.erase(std::remove_if(matches.begin(), matches.end(), [](const auto& match) {
                  return match.score <= 0;
                }),
                matches.end());
  std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
    if (left.score == right.score) {
      return left.chunk_id < right.chunk_id;
    }
    return left.score > right.score;
  });
  if (matches.size() > options.top_k) {
    matches.resize(options.top_k);
  }
  return matches;
}

void InMemoryKnowledgeTextIndex::delete_chunks(const std::vector<std::string>& chunk_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& chunk_id : chunk_ids) {
    chunks_.erase(chunk_id);
  }
}

void InMemoryKnowledgeTextIndex::clear(const KnowledgeTextIndexClearOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (options.knowledge_base_id.empty() && options.tenant_id.empty()) {
    chunks_.clear();
    return;
  }
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    const auto& chunk = it->second;
    if (!options.knowledge_base_id.empty() && chunk.knowledge_base_id != options.knowledge_base_id) {
      ++it;
      continue;
    }
    if (!options.tenant_id.empty() && chunk.tenant_id != options.tenant_id) {
      ++it;
      continue;
    }
    it = chunks_.erase(it);
  }
}

KnowledgeTextIndexStats InMemoryKnowledgeTextIndex::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return KnowledgeTextIndexStats{chunks_.size()};
}

std::vector<KnowledgeTextMatch> MiniSearchKnowledgeTextIndex::search(const KnowledgeTextSearchOptions& options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (options.top_k == 0) {
    return {};
  }
  const auto query_tokens = tokenize_knowledge_text(options.query);
  if (query_tokens.empty()) {
    return {};
  }

  std::vector<KnowledgeTextMatch> matches;
  for (const auto& [_, chunk] : chunks_) {
    if (!chunk_matches_text_search(chunk, options)) {
      continue;
    }
    const auto haystack_tokens = tokenize_knowledge_text(knowledge_text_index_surface(chunk));
    std::set<std::string> haystack(haystack_tokens.begin(), haystack_tokens.end());
    std::size_t matched = 0;
    for (const auto& token : query_tokens) {
      if (haystack.contains(token)) {
        ++matched;
      }
    }
    if (matched == 0) {
      continue;
    }
    matches.push_back(KnowledgeTextMatch{chunk.id, static_cast<double>(matched) / query_tokens.size()});
  }
  std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
    if (left.score == right.score) {
      return left.chunk_id < right.chunk_id;
    }
    return left.score > right.score;
  });
  if (matches.size() > options.top_k) {
    matches.resize(options.top_k);
  }
  return matches;
}

KnowledgeTextIndexRegistry::KnowledgeTextIndexRegistry(std::map<std::string, Factory> factories) {
  for (auto& [name, factory] : factories) {
    register_index(std::move(name), std::move(factory));
  }
}

KnowledgeTextIndexRegistry::KnowledgeTextIndexRegistry(std::vector<KnowledgeTextIndexProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeTextIndexRegistry::KnowledgeTextIndexRegistry(const KnowledgeTextIndexRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeTextIndexRegistry& KnowledgeTextIndexRegistry::operator=(const KnowledgeTextIndexRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeTextIndexRegistry::KnowledgeTextIndexRegistry(KnowledgeTextIndexRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeTextIndexRegistry& KnowledgeTextIndexRegistry::operator=(KnowledgeTextIndexRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeTextIndexProvider& KnowledgeTextIndexRegistry::register_provider(KnowledgeTextIndexProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge text index provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge text index provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

void KnowledgeTextIndexRegistry::register_index(std::string name, Factory factory) {
  if (name.empty()) {
    throw ConfigurationError("Knowledge text index name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Knowledge text index factory is required: " + name);
  }
  const auto provider_name = name;
  register_provider(KnowledgeTextIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = provider_name,
          .tier = "core-safe",
          .title = provider_name,
          .tags = {"text-index"},
      },
      .create = [factory = std::move(factory)](const Value&, const KnowledgeTextIndexRegistry&) {
        return factory();
      },
  });
}

const KnowledgeTextIndexProvider* KnowledgeTextIndexRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeTextIndex> KnowledgeTextIndexRegistry::create(const std::string& name,
                                                                       const Value& options) const {
  KnowledgeTextIndexProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      return nullptr;
    }
    factory = found->second.create;
  }
  return factory(options, *this);
}

std::vector<KnowledgeTextIndexProvider> KnowledgeTextIndexRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeTextIndexProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    providers.push_back(providers_.at(name));
  }
  return providers;
}

std::vector<std::string> KnowledgeTextIndexRegistry::list_names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    if (providers_.contains(name)) {
      names.push_back(name);
    }
  }
  return names;
}

KnowledgeTextIndexRegistry create_default_knowledge_text_index_registry() {
  KnowledgeTextIndexRegistry registry;
  registry.register_provider(KnowledgeTextIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "memory",
          .tier = "core-safe",
          .title = "In-Memory Text Index",
          .tags = {"text-index", "memory"},
      },
      .create = [](const Value&, const KnowledgeTextIndexRegistry&) {
        return std::make_shared<InMemoryKnowledgeTextIndex>();
      },
  });
  registry.register_provider(KnowledgeTextIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "minisearch",
          .tier = "portable",
          .title = "MiniSearch-style Text Index",
          .description = "Portable lexical text index for hybrid retrieval.",
          .tags = {"text-index", "lexical", "portable"},
      },
      .create = [](const Value&, const KnowledgeTextIndexRegistry&) {
        return std::make_shared<MiniSearchKnowledgeTextIndex>();
      },
  });
  return registry;
}
