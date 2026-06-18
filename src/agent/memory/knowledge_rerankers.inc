std::vector<KnowledgeSearchHit> HeuristicKnowledgeReranker::rerank(
    const std::string& query,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  for (auto& hit : hits) {
    const auto overlap = token_overlap_count(query, hit.document.title + " " + hit.chunk.uri);
    const double bonus = overlap == 0 ? 0 : std::min(0.2, static_cast<double>(overlap) * 0.05);
    hit.rerank_score = hit.score + bonus;
  }
  sort_hits_by_rerank_score(hits);
  if (top_k > 0 && hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

std::vector<KnowledgeSearchHit> BasicKnowledgeReranker::rerank(
    const std::string& query,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  for (auto& hit : hits) {
    const auto overlap = token_overlap_count(query, hit.citation.title + " " + hit.citation.snippet);
    hit.rerank_score = hit.score + std::min(0.3, static_cast<double>(overlap) * 0.03);
  }
  sort_hits_by_rerank_score(hits);
  if (top_k > 0 && hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

std::vector<KnowledgeSearchHit> OverlapKnowledgeReranker::rerank(
    const std::string& query,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  for (auto& hit : hits) {
    const auto overlap =
        token_overlap_count(query, hit.citation.title + "\n" + hit.citation.snippet + "\n" + hit.chunk.content);
    hit.rerank_score = hit.score + std::min(0.4, static_cast<double>(overlap) * 0.04);
  }
  sort_hits_by_rerank_score(hits);
  if (top_k > 0 && hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

HybridKnowledgeReranker::HybridKnowledgeReranker(HybridKnowledgeRerankerConfig config)
    : config_(config) {}

std::vector<KnowledgeSearchHit> HybridKnowledgeReranker::rerank(
    const std::string&,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  for (auto& hit : hits) {
    const auto ocr_text = hit.chunk.metadata.at("ocrText").as_string();
    const double asset_bias = hit.chunk.asset_type == KnowledgeAssetType::Image ? config_.image_asset_bias : 0;
    const double score = hit.vector_score * config_.vector_weight +
                         hit.lexical_score * config_.lexical_weight +
                         asset_bias +
                         (!ocr_text.empty() ? config_.ocr_boost : 0);
    hit.score = score;
    hit.rerank_score = score;
    hit.citation.score = score;
  }
  sort_hits_by_rerank_score(hits);
  if (top_k > 0 && hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

std::vector<KnowledgeSearchHit> RecencyKnowledgeReranker::rerank(
    const std::string&,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  double newest = 0;
  for (const auto& hit : hits) {
    newest = std::max(newest, std::max(sortable_timestamp_key(hit.chunk.updated_at),
                                       sortable_timestamp_key(hit.document.updated_at)));
  }
  for (auto& hit : hits) {
    const double updated_at = std::max(sortable_timestamp_key(hit.chunk.updated_at),
                                       sortable_timestamp_key(hit.document.updated_at));
    const double boost = newest > 0 && updated_at > 0 ? std::max(0.0, 0.15 * (updated_at / newest)) : 0;
    hit.rerank_score = hit.score + boost;
  }
  sort_hits_by_rerank_score(hits);
  if (top_k > 0 && hits.size() > top_k) {
    hits.resize(top_k);
  }
  return hits;
}

MmrKnowledgeReranker::MmrKnowledgeReranker(double lambda) : lambda_(lambda) {}

std::vector<KnowledgeSearchHit> MmrKnowledgeReranker::rerank(
    const std::string&,
    std::vector<KnowledgeSearchHit> hits,
    std::size_t top_k) const {
  if (top_k == 0) {
    return {};
  }
  if (top_k > hits.size()) {
    top_k = hits.size();
  }
  std::vector<KnowledgeSearchHit> selected;
  selected.reserve(top_k);
  while (!hits.empty() && selected.size() < top_k) {
    std::size_t best_index = 0;
    double best_score = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < hits.size(); ++index) {
      double diversity_penalty = 0;
      for (const auto& selected_hit : selected) {
        diversity_penalty =
            std::max(diversity_penalty, dot_product(hits[index].chunk.embedding, selected_hit.chunk.embedding));
      }
      const double rerank_score = lambda_ * hits[index].score - (1.0 - lambda_) * diversity_penalty;
      if (rerank_score > best_score) {
        best_score = rerank_score;
        best_index = index;
      }
    }
    auto chosen = std::move(hits[best_index]);
    hits.erase(hits.begin() + static_cast<std::ptrdiff_t>(best_index));
    chosen.rerank_score = best_score;
    selected.push_back(std::move(chosen));
  }
  return selected;
}

KnowledgeRerankerRegistry::KnowledgeRerankerRegistry(
    std::map<std::string, std::shared_ptr<KnowledgeReranker>> rerankers)
{
  for (auto& [name, reranker] : rerankers) {
    register_reranker(std::move(name), std::move(reranker));
  }
}

KnowledgeRerankerRegistry::KnowledgeRerankerRegistry(std::vector<KnowledgeRerankerProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeRerankerRegistry::KnowledgeRerankerRegistry(const KnowledgeRerankerRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeRerankerRegistry& KnowledgeRerankerRegistry::operator=(const KnowledgeRerankerRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeRerankerRegistry::KnowledgeRerankerRegistry(KnowledgeRerankerRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeRerankerRegistry& KnowledgeRerankerRegistry::operator=(KnowledgeRerankerRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeRerankerProvider& KnowledgeRerankerRegistry::register_provider(KnowledgeRerankerProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge reranker provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge reranker provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

void KnowledgeRerankerRegistry::register_reranker(std::string name, std::shared_ptr<KnowledgeReranker> reranker) {
  if (name.empty()) {
    throw ConfigurationError("Knowledge reranker name is required.");
  }
  if (!reranker) {
    throw ConfigurationError("Knowledge reranker is required: " + name);
  }
  const auto provider_name = name;
  register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = provider_name,
          .tier = "core-safe",
          .title = provider_name,
          .tags = {"reranker"},
      },
      .create = [reranker = std::move(reranker)](const Value&, const KnowledgeRerankerRegistry&) {
        return reranker;
      },
  });
}

const KnowledgeRerankerProvider* KnowledgeRerankerRegistry::provider(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeReranker> KnowledgeRerankerRegistry::create(const std::string& name,
                                                                     const Value& options) const {
  KnowledgeRerankerProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      throw ConfigurationError("Unknown knowledge reranker provider: " + name);
    }
    factory = found->second.create;
  }
  auto reranker = factory(options, *this);
  if (!reranker) {
    throw ConfigurationError("Knowledge reranker provider returned null: " + name);
  }
  return reranker;
}

std::shared_ptr<KnowledgeReranker> KnowledgeRerankerRegistry::get(const std::string& name) const {
  KnowledgeRerankerProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      return nullptr;
    }
    factory = found->second.create;
  }
  return factory(Value::object({}), *this);
}

std::vector<KnowledgeRerankerProvider> KnowledgeRerankerRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeRerankerProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<std::string> KnowledgeRerankerRegistry::list_names() const {
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

KnowledgeRerankerRegistry create_default_knowledge_reranker_registry() {
  KnowledgeRerankerRegistry registry;
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "heuristic",
          .tier = "core-safe",
          .title = "Heuristic Reranker",
          .tags = {"reranker", "basic"},
      },
      .create = [](const Value&, const KnowledgeRerankerRegistry&) {
        return std::make_shared<HeuristicKnowledgeReranker>();
      },
  });
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "basic",
          .tier = "core-safe",
          .title = "Basic Token Overlap Reranker",
          .description = "A lightweight token-overlap reranker for retrieved knowledge hits.",
          .tags = {"reranker", "basic"},
      },
      .create = [](const Value&, const KnowledgeRerankerRegistry&) {
        return std::make_shared<BasicKnowledgeReranker>();
      },
  });
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "overlap",
          .tier = "core-safe",
          .title = "Overlap Reranker",
          .description = "Boosts hits with stronger token overlap across title, snippet, and chunk content.",
          .tags = {"reranker", "overlap"},
      },
      .create = [](const Value&, const KnowledgeRerankerRegistry&) {
        return std::make_shared<OverlapKnowledgeReranker>();
      },
  });
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "hybrid",
          .tier = "portable",
          .title = "Hybrid Reranker",
          .description = "Combines vector, lexical, OCR and asset priors for hybrid retrieval.",
          .tags = {"reranker", "hybrid", "multimodal"},
      },
      .create = [](const Value& options, const KnowledgeRerankerRegistry&) {
        HybridKnowledgeRerankerConfig config;
        config.vector_weight = options.at("vectorWeight").as_number(config.vector_weight);
        config.lexical_weight = options.at("lexicalWeight").as_number(config.lexical_weight);
        config.image_asset_bias = options.at("imageAssetBias").as_number(config.image_asset_bias);
        config.ocr_boost = options.at("ocrBoost").as_number(config.ocr_boost);
        return std::make_shared<HybridKnowledgeReranker>(config);
      },
  });
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "recency",
          .tier = "core-safe",
          .title = "Recency Reranker",
          .description = "Biases retrieved hits toward more recently updated documents.",
          .tags = {"reranker", "recency"},
      },
      .create = [](const Value&, const KnowledgeRerankerRegistry&) {
        return std::make_shared<RecencyKnowledgeReranker>();
      },
  });
  registry.register_provider(KnowledgeRerankerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "mmr",
          .tier = "core-safe",
          .title = "MMR Reranker",
          .description = "Diversifies final hits with a lightweight max marginal relevance strategy.",
          .tags = {"reranker", "mmr"},
      },
      .create = [](const Value& options, const KnowledgeRerankerRegistry&) {
        return std::make_shared<MmrKnowledgeReranker>(options.at("lambda").as_number(0.75));
      },
  });
  return registry;
}
