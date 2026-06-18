std::string hash_knowledge_content(const std::string& content) {
  return sha256_hex(content);
}

std::vector<LoadedKnowledgeDocument> dedupe_loaded_knowledge_documents(
    const std::vector<LoadedKnowledgeDocument>& documents,
    bool replace_existing) {
  std::set<std::string> seen_pairs;
  if (!replace_existing) {
    std::vector<LoadedKnowledgeDocument> deduped;
    for (const auto& document : documents) {
      const auto key = document.uri + "::" + hash_knowledge_content(document.content);
      if (!seen_pairs.insert(key).second) {
        continue;
      }
      deduped.push_back(document);
    }
    return deduped;
  }

  std::vector<std::string> uri_order;
  std::map<std::string, LoadedKnowledgeDocument> latest_by_uri;
  for (const auto& document : documents) {
    const auto key = document.uri + "::" + hash_knowledge_content(document.content);
    if (!seen_pairs.insert(key).second) {
      continue;
    }
    if (!latest_by_uri.contains(document.uri)) {
      uri_order.push_back(document.uri);
    }
    latest_by_uri[document.uri] = document;
  }

  std::vector<LoadedKnowledgeDocument> deduped;
  for (const auto& uri : uri_order) {
    deduped.push_back(latest_by_uri.at(uri));
  }
  return deduped;
}

RecursiveTextChunker::RecursiveTextChunker(std::size_t chunk_size, std::size_t chunk_overlap,
                                           std::size_t code_chunk_lines,
                                           std::size_t code_chunk_overlap_lines)
    : chunk_size_(chunk_size),
      chunk_overlap_(chunk_overlap),
      code_chunk_lines_(code_chunk_lines),
      code_chunk_overlap_lines_(code_chunk_overlap_lines) {}

std::vector<ChunkDraft> RecursiveTextChunker::chunk(const LoadedKnowledgeDocument& document) const {
  if (document.asset_type == KnowledgeAssetType::Image) {
    std::vector<std::string> parts = {document.title,
                                      document.alt_text,
                                      document.caption,
                                      document.ocr_text,
                                      document.text_hint,
                                      document.content,
                                      document.uri};
    std::string content;
    for (const auto& part : parts) {
      if (part.empty()) {
        continue;
      }
      if (!content.empty()) {
        content += "\n";
      }
      content += part;
    }
    content = trim_copy(content);
    if (content.empty()) {
      return {};
    }
    return {ChunkDraft{content, 0, 0, content.size(), 0, 0,
                       Value::object({{"assetType", "image"}})}};
  }

  if (document.content.empty()) {
    return {};
  }

  const auto is_code_like = [&]() {
    static const std::set<std::string> code_extensions = {
        ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".py", ".java", ".go", ".rs", ".cpp",
        ".c",  ".h",   ".hpp", ".cs", ".php", ".rb",  ".swift", ".kt", ".scala", ".sh",
        ".sql", ".yaml", ".yml", ".json", ".toml", ".mdx", ".vue", ".svelte"};
    if (document.source_type == "repository" || document.source_type == "github") {
      return true;
    }
    const auto dot = document.uri.find_last_of('.');
    if (dot == std::string::npos) {
      return false;
    }
    std::string ext = document.uri.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return code_extensions.find(ext) != code_extensions.end();
  };

  std::vector<ChunkDraft> chunks;
  if (is_code_like()) {
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= document.content.size(); ++index) {
      if (index == document.content.size() || document.content[index] == '\n') {
        lines.push_back({start, index == document.content.size() ? index : index + 1});
        start = index + 1;
      }
    }
    std::size_t start_line = 0;
    std::size_t chunk_index = 0;
    while (start_line < lines.size()) {
      const std::size_t end_line = std::min(lines.size(), start_line + code_chunk_lines_);
      const std::size_t start_offset = lines[start_line].first;
      const std::size_t end_offset = lines[end_line - 1].second;
      const std::string content = trim_copy(document.content.substr(start_offset, end_offset - start_offset));
      if (!content.empty()) {
        chunks.push_back(ChunkDraft{content, chunk_index++, start_offset, end_offset,
                                    start_line + 1, end_line, Value::object({})});
      }
      if (end_line >= lines.size()) {
        break;
      }
      start_line = std::max(end_line - code_chunk_overlap_lines_, start_line + 1);
    }
    return chunks;
  }

  std::size_t start = 0;
  std::size_t chunk_index = 0;
  while (start < document.content.size()) {
    const std::size_t hard_end = std::min(document.content.size(), start + chunk_size_);
    std::size_t end = hard_end;
    if (hard_end < document.content.size()) {
      const std::size_t search_start = std::max(start + chunk_size_ / 2, start);
      const std::string window = document.content.substr(search_start, hard_end - search_start);
      for (const auto& candidate : std::vector<std::string>{"\n\n", "\n", ". ", "。", "！", "？", " "}) {
        const auto found = window.rfind(candidate);
        if (found != std::string::npos) {
          end = search_start + found + candidate.size();
          break;
        }
      }
    }
    const std::string content = trim_copy(document.content.substr(start, end - start));
    if (!content.empty()) {
      chunks.push_back(ChunkDraft{content, chunk_index++, start, end});
    }
    if (end >= document.content.size()) {
      break;
    }
    start = std::max(end - chunk_overlap_, start + 1);
  }
  return chunks;
}

KnowledgeChunkerRegistry::KnowledgeChunkerRegistry(std::vector<KnowledgeChunkerProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeChunkerRegistry::KnowledgeChunkerRegistry(const KnowledgeChunkerRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeChunkerRegistry& KnowledgeChunkerRegistry::operator=(const KnowledgeChunkerRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeChunkerRegistry::KnowledgeChunkerRegistry(KnowledgeChunkerRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeChunkerRegistry& KnowledgeChunkerRegistry::operator=(KnowledgeChunkerRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeChunkerProvider& KnowledgeChunkerRegistry::register_provider(KnowledgeChunkerProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge chunker provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge chunker provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

const KnowledgeChunkerProvider* KnowledgeChunkerRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeChunker> KnowledgeChunkerRegistry::create(const std::string& name,
                                                                   const Value& options) const {
  KnowledgeChunkerProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      throw ConfigurationError("Unknown knowledge chunker provider: " + name);
    }
    factory = found->second.create;
  }
  auto chunker = factory(options, *this);
  if (!chunker) {
    throw ConfigurationError("Knowledge chunker provider returned null: " + name);
  }
  return chunker;
}

std::vector<KnowledgeChunkerProvider> KnowledgeChunkerRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeChunkerProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<std::string> KnowledgeChunkerRegistry::list_names() const {
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

KnowledgeChunkerRegistry create_default_knowledge_chunker_registry() {
  KnowledgeChunkerRegistry registry;
  registry.register_provider(KnowledgeChunkerProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "recursive-text",
          .tier = "core-safe",
          .title = "Recursive Text Chunker",
          .tags = {"chunker", "text"},
      },
      .create = [](const Value& options, const KnowledgeChunkerRegistry&) {
        return std::make_shared<RecursiveTextChunker>(
            size_option_from_value(options, "chunkSize", 1200),
            size_option_from_value(options, "chunkOverlap", 180),
            size_option_from_value(options, "codeChunkLines", 80),
            size_option_from_value(options, "codeChunkOverlapLines", 12));
      },
  });
  return registry;
}
