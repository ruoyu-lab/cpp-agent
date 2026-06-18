InMemoryVectorStore::InMemoryVectorStore(std::string namespace_id) : namespace_id_(std::move(namespace_id)) {}

InMemoryVectorStore::InMemoryVectorStore(InMemoryVectorStoreConfig config)
    : InMemoryVectorStore(std::move(config.namespace_id)) {}

std::vector<VectorMemoryRecord> InMemoryVectorStore::upsert(const std::vector<VectorMemoryUpsertInput>& items) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = now_iso8601();
  std::vector<VectorMemoryRecord> records;
  records.reserve(items.size());
  for (const auto& item : items) {
    const std::string id = item.id.empty() ? generate_uuid() : item.id;
    const auto existing = records_.find(id);
    VectorMemoryRecord record;
    record.id = id;
    record.content = item.content;
    record.embedding = normalize_vector(item.embedding);
    record.metadata = item.metadata;
    record.namespace_id = item.namespace_id.empty() ? namespace_id_ : item.namespace_id;
    record.created_at = existing == records_.end() ? now : existing->second.created_at;
    record.updated_at = now;
    records_[id] = record;
    records.push_back(std::move(record));
  }
  return records;
}

std::vector<RetrievedMemory> InMemoryVectorStore::query(const VectorMemoryQuery& query) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto normalized = normalize_vector(query.embedding);
  std::vector<RetrievedMemory> hits;
  for (const auto& [_, record] : records_) {
    if (!query.namespace_id.empty() && record.namespace_id != query.namespace_id) {
      continue;
    }
    const double score = dot_product(normalized, record.embedding);
    if (score >= query.min_score) {
      hits.push_back(RetrievedMemory{record.id, record.content, score, record.metadata, record.namespace_id});
    }
  }
  const std::size_t limit = std::min(query.top_k, hits.size());
  if (limit < hits.size()) {
    std::nth_element(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(limit), hits.end(),
                     [](const auto& left, const auto& right) { return left.score > right.score; });
    hits.resize(limit);
  }
  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    return left.score > right.score;
  });
  return hits;
}

std::size_t InMemoryVectorStore::erase(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += records_.erase(id);
  }
  return deleted;
}

void InMemoryVectorStore::clear(const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (namespace_id.empty()) {
    records_.clear();
    return;
  }
  for (auto it = records_.begin(); it != records_.end();) {
    if (it->second.namespace_id == namespace_id) {
      it = records_.erase(it);
    } else {
      ++it;
    }
  }
}

std::size_t InMemoryVectorStore::count(const std::string& namespace_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (namespace_id.empty()) {
    return records_.size();
  }
  return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [&](const auto& entry) {
    return entry.second.namespace_id == namespace_id;
  }));
}

FileVectorStore::FileVectorStore(std::filesystem::path file_path, std::string namespace_id)
    : file_path_(std::move(file_path)), namespace_id_(std::move(namespace_id)) {}

FileVectorStore::FileVectorStore(FileVectorStoreConfig config)
    : FileVectorStore(std::move(config.file_path), std::move(config.namespace_id)) {}

std::vector<VectorMemoryRecord> FileVectorStore::upsert(const std::vector<VectorMemoryUpsertInput>& items) {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  const auto now = now_iso8601();
  std::vector<VectorMemoryRecord> records;
  for (const auto& item : items) {
    const std::string id = item.id.empty() ? generate_uuid() : item.id;
    const auto existing = records_.find(id);
    VectorMemoryRecord record{id,
                              item.content,
                              normalize_vector(item.embedding),
                              item.metadata,
                              item.namespace_id.empty() ? namespace_id_ : item.namespace_id,
                              existing == records_.end() ? now : existing->second.created_at,
                              now};
    records_[id] = record;
    records.push_back(record);
  }
  persist();
  return records;
}

std::vector<RetrievedMemory> FileVectorStore::query(const VectorMemoryQuery& query) {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  const auto normalized = normalize_vector(query.embedding);
  std::vector<RetrievedMemory> hits;
  for (const auto& [_, record] : records_) {
    if (!query.namespace_id.empty() && record.namespace_id != query.namespace_id) {
      continue;
    }
    const double score = dot_product(normalized, record.embedding);
    if (score >= query.min_score) {
      hits.push_back(RetrievedMemory{record.id, record.content, score, record.metadata, record.namespace_id});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    return left.score > right.score;
  });
  const std::size_t limit = std::min(query.top_k, hits.size());
  if (limit < hits.size()) {
    hits.resize(limit);
  }
  return hits;
}

std::size_t FileVectorStore::erase(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += records_.erase(id);
  }
  persist();
  return deleted;
}

void FileVectorStore::clear(const std::string& namespace_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  if (namespace_id.empty()) {
    records_.clear();
  } else {
    for (auto it = records_.begin(); it != records_.end();) {
      it = it->second.namespace_id == namespace_id ? records_.erase(it) : std::next(it);
    }
  }
  persist();
}

std::size_t FileVectorStore::count(const std::string& namespace_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  if (namespace_id.empty()) {
    return records_.size();
  }
  return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [&](const auto& item) {
    return item.second.namespace_id == namespace_id;
  }));
}

void FileVectorStore::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  if (!std::filesystem::exists(file_path_)) {
    return;
  }
  const auto raw = read_json_file(file_path_);
  for (const auto& item : raw.at("records").as_array()) {
    EmbeddingVector embedding;
    for (const auto& value : item.at("embedding").as_array()) {
      embedding.push_back(value.as_number());
    }
    VectorMemoryRecord record{item.at("id").as_string(),
                              item.at("content").as_string(),
                              embedding,
                              item.at("metadata").is_object() ? item.at("metadata") : Value::object({}),
                              item.at("namespace").as_string(namespace_id_),
                              item.at("createdAt").as_string(),
                              item.at("updatedAt").as_string()};
    const auto record_id = record.id;
    records_[record_id] = std::move(record);
  }
}

void FileVectorStore::persist() const {
  Value::Array records;
  for (const auto& [_, record] : records_) {
    Value::Array embedding;
    for (const auto value : record.embedding) {
      embedding.emplace_back(value);
    }
    records.push_back(Value::object({{"id", record.id},
                                     {"content", record.content},
                                     {"embedding", Value(embedding)},
                                     {"metadata", record.metadata},
                                     {"namespace", record.namespace_id},
                                     {"createdAt", record.created_at},
                                     {"updatedAt", record.updated_at}}));
  }
  write_json_file(file_path_, Value::object({{"records", Value(records)}}));
}

LongTermMemory::LongTermMemory(std::shared_ptr<TextEmbeddingAdapter> embedder, std::shared_ptr<VectorStore> store,
                               std::string namespace_id, std::size_t top_k, double min_score, bool auto_remember,
                               std::string context_title)
    : embedder_(std::move(embedder)),
      store_(std::move(store)),
      namespace_id_(std::move(namespace_id)),
      top_k_(top_k),
      min_score_(min_score),
      auto_remember_(auto_remember),
      context_title_(std::move(context_title)) {}

LongTermMemory::LongTermMemory(LongTermMemoryConfig config)
    : LongTermMemory(std::move(config.embedder),
                     std::move(config.store),
                     std::move(config.namespace_id),
                     config.top_k,
                     config.min_score,
                     config.auto_remember,
                     std::move(config.context_title)) {}

VectorMemoryRecord LongTermMemory::remember(std::string content, Value metadata, std::string namespace_id,
                                            CancellationToken* cancellation) {
  auto records = remember_many({RememberMemoryInput{"",
                                                    std::move(content),
                                                    std::move(metadata),
                                                    std::move(namespace_id)}},
                               cancellation);
  return records.front();
}

VectorMemoryRecord LongTermMemory::remember(const RememberMemoryInput& input,
                                            CancellationToken* cancellation) {
  auto records = remember_many({input}, cancellation);
  return records.front();
}

std::vector<VectorMemoryRecord> LongTermMemory::remember_many(const std::vector<RememberMemoryInput>& inputs,
                                                              CancellationToken* cancellation) {
  std::vector<std::string> texts;
  texts.reserve(inputs.size());
  for (const auto& input : inputs) {
    texts.push_back(input.content);
  }
  auto vectors = embedder_->embed(texts, Value::object({{"taskType", "RETRIEVAL_DOCUMENT"}}), cancellation);
  std::vector<VectorMemoryUpsertInput> upserts;
  upserts.reserve(inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const auto& input = inputs[index];
    upserts.push_back(VectorMemoryUpsertInput{
        input.id,
        input.content,
        index < vectors.size() ? std::move(vectors[index]) : EmbeddingVector{},
        input.metadata,
        input.namespace_id.empty() ? namespace_id_ : input.namespace_id,
    });
  }
  return store_->upsert(upserts);
}

std::vector<RetrievedMemory> LongTermMemory::search(const std::string& query,
                                                    std::optional<std::size_t> top_k,
                                                    std::optional<double> min_score,
                                                    std::string namespace_id,
                                                    CancellationToken* cancellation) {
  auto vector = embedder_->embed_one(query, Value::object({{"taskType", "RETRIEVAL_QUERY"}}), cancellation);
  return store_->query(VectorMemoryQuery{std::move(vector), top_k.value_or(top_k_),
                                         min_score.value_or(min_score_),
                                         namespace_id.empty() ? namespace_id_ : std::move(namespace_id)});
}

std::vector<RetrievedMemory> LongTermMemory::search(const std::string& query,
                                                    const SearchMemoryOptions& options,
                                                    CancellationToken* cancellation) {
  return search(query, options.top_k, options.min_score, options.namespace_id, cancellation);
}

std::optional<AgentMessage> LongTermMemory::create_context_message(const std::vector<RetrievedMemory>& hits) const {
  if (hits.empty()) {
    return std::nullopt;
  }
  std::ostringstream content;
  content << context_title_;
  for (std::size_t index = 0; index < hits.size(); ++index) {
    content << "\n" << index + 1 << ". score=" << std::fixed << std::setprecision(3) << hits[index].score
            << " | " << hits[index].content;
  }
  return create_message(MessageRole::System, content.str(),
                        Value::object({{"source", "long-term-memory"}, {"hits", hits.size()}}));
}

LongTermMemoryContextResult LongTermMemory::build_context_message(const std::string& query,
                                                                  std::optional<std::size_t> top_k,
                                                                  std::optional<double> min_score,
                                                                  std::string namespace_id,
                                                                  CancellationToken* cancellation) {
  LongTermMemoryContextResult result;
  result.hits = search(query, top_k, min_score, std::move(namespace_id), cancellation);
  result.message = create_context_message(result.hits);
  return result;
}

LongTermMemoryContextResult LongTermMemory::build_context_message(const std::string& query,
                                                                  const SearchMemoryOptions& options,
                                                                  CancellationToken* cancellation) {
  LongTermMemoryContextResult result;
  result.hits = search(query, options, cancellation);
  result.message = create_context_message(result.hits);
  return result;
}

bool LongTermMemory::auto_remember() const noexcept {
  return auto_remember_;
}

VectorMemoryRecord LongTermMemory::remember_conversation_turn(const std::string& session_id, const std::string& input,
                                                              const std::string& output, Value metadata,
                                                              std::string namespace_id,
                                                              std::vector<std::string> plan_steps) {
  metadata["sessionId"] = session_id;
  metadata["type"] = "conversation-turn";
  std::string plan_summary = "Plan: none";
  if (!plan_steps.empty()) {
    std::ostringstream out;
    out << "Plan: ";
    for (std::size_t index = 0; index < plan_steps.size(); ++index) {
      if (index > 0) {
        out << " -> ";
      }
      out << plan_steps[index];
    }
    plan_summary = out.str();
  }
  return remember("User: " + input + "\nAssistant: " + output + "\n" + plan_summary,
                  std::move(metadata), std::move(namespace_id));
}

VectorMemoryRecord LongTermMemory::remember_conversation_turn(const RememberConversationTurnInput& input) {
  std::vector<std::string> plan_steps = input.plan_steps;
  if (input.plan && !input.plan->steps.empty()) {
    plan_steps.clear();
    plan_steps.reserve(input.plan->steps.size());
    for (const auto& step : input.plan->steps) {
      plan_steps.push_back(step.title);
    }
  }
  return remember_conversation_turn(input.session_id,
                                    input.input,
                                    input.output,
                                    input.metadata,
                                    input.namespace_id,
                                    std::move(plan_steps));
}
