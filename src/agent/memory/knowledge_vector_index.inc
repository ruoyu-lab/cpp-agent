void InMemoryKnowledgeVectorIndex::upsert(const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto chunk : chunks) {
    if (!chunk.id.empty()) {
      chunk.embedding = normalize_vector(std::move(chunk.embedding));
      const auto chunk_id = chunk.id;
      chunks_[chunk_id] = std::move(chunk);
    }
  }
}

std::vector<KnowledgeVectorMatch> InMemoryKnowledgeVectorIndex::search(
    const KnowledgeVectorSearchOptions& options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (options.top_k == 0) {
    return {};
  }
  const auto normalized_query = normalize_vector(options.embedding);
  std::vector<KnowledgeVectorMatch> matches;
  for (const auto& [_, chunk] : chunks_) {
    if (!chunk_matches_vector_search(chunk, options)) {
      continue;
    }
    const auto score = vector_index_score(normalized_query, chunk.embedding);
    if (score >= options.min_score) {
      matches.push_back(KnowledgeVectorMatch{chunk.id, score});
    }
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

void InMemoryKnowledgeVectorIndex::delete_chunks(const std::vector<std::string>& chunk_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& chunk_id : chunk_ids) {
    chunks_.erase(chunk_id);
  }
}

void InMemoryKnowledgeVectorIndex::clear(const KnowledgeVectorIndexClearOptions& options) {
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

KnowledgeVectorIndexStats InMemoryKnowledgeVectorIndex::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  KnowledgeVectorIndexStats stats;
  stats.chunk_count = chunks_.size();
  for (const auto& [_, chunk] : chunks_) {
    ++stats.asset_type_counts[knowledge_asset_type_label(chunk.asset_type)];
  }
  return stats;
}

SqliteKnowledgeVectorIndex::SqliteKnowledgeVectorIndex(SqliteKnowledgeVectorIndexConfig config)
    : config_(std::move(config)) {
  if (config_.file_path.empty()) {
    throw ConfigurationError("File-backed knowledge vector index requires file_path.");
  }
  if (config_.batch_size == 0) {
    config_.batch_size = 200;
  }
}

void SqliteKnowledgeVectorIndex::load() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  chunks_.clear();
  if (!std::filesystem::exists(config_.file_path)) {
    return;
  }
  const auto value = read_json_file(config_.file_path);
  const auto file_namespace = value.is_object() ? value.at("namespace").as_string() : std::string();
  const auto& items = value.at("chunks").is_array() ? value.at("chunks").as_array() : value.as_array();
  for (const auto& item : items) {
    const auto& chunk_value = item.is_object() && item.at("chunk").is_object() ? item.at("chunk") : item;
    auto chunk = knowledge_chunk_record_from_value(chunk_value);
    if (!chunk.id.empty()) {
      const auto chunk_id = chunk.id;
      chunks_[chunk_id] = StoredChunk{
          item.is_object() ? item.at("namespace").as_string(file_namespace) : file_namespace,
          std::move(chunk),
      };
    }
  }
}

void SqliteKnowledgeVectorIndex::save() const {
  if (config_.file_path.has_parent_path()) {
    std::filesystem::create_directories(config_.file_path.parent_path());
  }
  Value::Array chunks;
  for (const auto& [_, record] : chunks_) {
    auto value = knowledge_chunk_record_to_value(record.chunk);
    value["namespace"] = record.namespace_id;
    chunks.push_back(std::move(value));
  }
  write_json_file(config_.file_path, Value::object({
      {"kind", "file-backed-knowledge-vector-index"},
      {"tableName", config_.table_name},
      {"namespace", config_.namespace_id},
      {"chunks", Value(std::move(chunks))},
  }));
}

void SqliteKnowledgeVectorIndex::upsert(const std::vector<KnowledgeChunkRecord>& chunks) {
  if (chunks.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  load();
  for (auto chunk : chunks) {
    if (chunk.id.empty()) {
      continue;
    }
    chunk.embedding = normalize_vector(std::move(chunk.embedding));
    const auto chunk_id = chunk.id;
    chunks_[chunk_id] = StoredChunk{config_.namespace_id, std::move(chunk)};
  }
  save();
}

std::vector<KnowledgeVectorMatch> SqliteKnowledgeVectorIndex::search(
    const KnowledgeVectorSearchOptions& options) const {
  if (options.top_k == 0) {
    return {};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  load();
  std::vector<KnowledgeVectorMatch> matches;
  const auto normalized_query = normalize_vector(options.embedding);
  for (const auto& [_, record] : chunks_) {
    if (!sqlite_vector_namespace_matches(config_.namespace_id, record.namespace_id) ||
        !chunk_matches_vector_search(record.chunk, options)) {
      continue;
    }
    const auto score = vector_index_score(normalized_query, record.chunk.embedding);
    if (score >= options.min_score) {
      matches.push_back(KnowledgeVectorMatch{record.chunk.id, score});
    }
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

void SqliteKnowledgeVectorIndex::delete_chunks(const std::vector<std::string>& chunk_ids) {
  if (chunk_ids.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  load();
  for (const auto& chunk_id : chunk_ids) {
    const auto found = chunks_.find(chunk_id);
    if (found != chunks_.end() && sqlite_vector_namespace_matches(config_.namespace_id, found->second.namespace_id)) {
      chunks_.erase(found);
    }
  }
  save();
}

void SqliteKnowledgeVectorIndex::clear(const KnowledgeVectorIndexClearOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  load();
  if (options.knowledge_base_id.empty() && options.tenant_id.empty() && config_.namespace_id.empty()) {
    chunks_.clear();
    save();
    return;
  }
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    const auto& record = it->second;
    const auto& chunk = record.chunk;
    if (!sqlite_vector_namespace_matches(config_.namespace_id, record.namespace_id)) {
      ++it;
      continue;
    }
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
  save();
}

KnowledgeVectorIndexStats SqliteKnowledgeVectorIndex::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  load();
  KnowledgeVectorIndexStats stats;
  stats.namespace_id = config_.namespace_id;
  for (const auto& [_, record] : chunks_) {
    if (!sqlite_vector_namespace_matches(config_.namespace_id, record.namespace_id)) {
      continue;
    }
    ++stats.chunk_count;
    ++stats.asset_type_counts[knowledge_asset_type_label(record.chunk.asset_type)];
  }
  return stats;
}

QdrantKnowledgeVectorIndex::QdrantKnowledgeVectorIndex(QdrantKnowledgeVectorIndexConfig config)
    : config_(std::move(config)) {
  if (config_.base_url.empty()) {
    throw ConfigurationError("Qdrant base_url is required.");
  }
  if (config_.collection.empty()) {
    throw ConfigurationError("Qdrant collection is required.");
  }
  if (config_.oversample_factor == 0) {
    config_.oversample_factor = 4;
  }
  if (!config_.transport) {
    throw ConfigurationError("Qdrant vector index requires an HTTP transport.");
  }
}

std::string QdrantKnowledgeVectorIndex::path(std::string suffix) const {
  return "/collections/" + config_.collection + std::move(suffix);
}

std::map<std::string, std::string> QdrantKnowledgeVectorIndex::headers() const {
  auto result = config_.headers;
  if (!config_.api_key.empty()) {
    result["api-key"] = config_.api_key;
  }
  return result;
}

Value QdrantKnowledgeVectorIndex::vector_payload(const EmbeddingVector& embedding) const {
  if (config_.vector_name.empty()) {
    return embedding_to_value(embedding);
  }
  return Value::object({{config_.vector_name, embedding_to_value(embedding)}});
}

void QdrantKnowledgeVectorIndex::ensure_collection(int dimensions) const {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  if (!config_.create_collection || collection_ready_) {
    return;
  }
  const int resolved_dimensions = dimensions > 0 ? dimensions : config_.dimensions;
  if (resolved_dimensions <= 0) {
    throw ConfigurationError("Qdrant vector dimensions are required to create a collection.");
  }
  try {
    (void)request_json(RequestJsonOptions{
        .base_url = config_.base_url,
        .path = path(""),
        .method = "GET",
        .headers = headers(),
    }, config_.transport);
    collection_ready_ = true;
    return;
  } catch (const AdapterError&) {
  }

  Value vectors = config_.vector_name.empty()
                      ? Value::object({{"size", resolved_dimensions}, {"distance", config_.distance}})
                      : Value::object({{config_.vector_name,
                                        Value::object({{"size", resolved_dimensions}, {"distance", config_.distance}})}});
  (void)request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path(""),
      .method = "PUT",
      .headers = headers(),
      .body = Value::object({{"vectors", vectors}}),
  }, config_.transport);
  collection_ready_ = true;
}

void QdrantKnowledgeVectorIndex::upsert(const std::vector<KnowledgeChunkRecord>& chunks) {
  if (chunks.empty()) {
    return;
  }
  ensure_collection(static_cast<int>(chunks.front().embedding.size()));
  Value::Array points;
  for (const auto& chunk : chunks) {
    points.push_back(Value::object({
        {"id", chunk.id},
        {"vector", vector_payload(chunk.embedding)},
        {"payload", Value::object({
                        {"id", chunk.id},
                        {"chunkId", chunk.id},
                        {"documentId", chunk.document_id},
                        {"knowledgeBaseId", chunk.knowledge_base_id},
                        {"tenantId", chunk.tenant_id},
                        {"sourceType", chunk.source_type},
                        {"assetType", knowledge_asset_type_label(chunk.asset_type)},
                        {"embeddingSpaceId", chunk.embedding_space_id},
                        {"uri", chunk.uri},
                        {"title", chunk.title},
                        {"metadata", chunk.metadata},
                    })},
    }));
  }
  (void)request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path("/points?wait=" + std::string(config_.wait ? "true" : "false")),
      .method = "PUT",
      .headers = headers(),
      .body = Value::object({{"points", Value(points)}}),
  }, config_.transport);
}

std::vector<KnowledgeVectorMatch> QdrantKnowledgeVectorIndex::search(
    const KnowledgeVectorSearchOptions& options) const {
  if (options.top_k == 0) {
    return {};
  }
  ensure_collection(static_cast<int>(options.embedding.size()));
  const auto oversampled = std::max(options.top_k, options.top_k * config_.oversample_factor);
  auto response = request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path("/points/search"),
      .method = "POST",
      .headers = headers(),
      .body = Value::object({
          {"vector", vector_payload(options.embedding)},
          {"limit", oversampled},
          {"score_threshold", options.min_score},
          {"with_payload", true},
          {"filter", qdrant_search_filter(options)},
      }),
  }, config_.transport);

  std::vector<KnowledgeVectorMatch> matches;
  for (const auto& point : response.at("result").as_array()) {
    if (!point.contains("score")) {
      continue;
    }
    if (!qdrant_payload_matches(point.at("payload"), options)) {
      continue;
    }
    const auto chunk_id = value_to_id_string(point.at("id"));
    if (chunk_id.empty()) {
      continue;
    }
    matches.push_back(KnowledgeVectorMatch{chunk_id, point.at("score").as_number()});
    if (matches.size() >= options.top_k) {
      break;
    }
  }
  return matches;
}

void QdrantKnowledgeVectorIndex::delete_chunks(const std::vector<std::string>& chunk_ids) {
  if (chunk_ids.empty()) {
    return;
  }
  Value::Array points;
  for (const auto& chunk_id : chunk_ids) {
    points.push_back(chunk_id);
  }
  (void)request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path("/points/delete?wait=" + std::string(config_.wait ? "true" : "false")),
      .method = "POST",
      .headers = headers(),
      .body = Value::object({{"points", Value(points)}}),
  }, config_.transport);
}

void QdrantKnowledgeVectorIndex::clear(const KnowledgeVectorIndexClearOptions& options) {
  KnowledgeVectorSearchOptions filter_options;
  filter_options.knowledge_base_id = options.knowledge_base_id;
  filter_options.tenant_id = options.tenant_id;
  Value body = (options.knowledge_base_id.empty() && options.tenant_id.empty())
                   ? Value::object({{"filter", Value::object({})}})
                   : Value::object({{"filter", qdrant_search_filter(filter_options)}});
  (void)request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path("/points/delete?wait=" + std::string(config_.wait ? "true" : "false")),
      .method = "POST",
      .headers = headers(),
      .body = body,
  }, config_.transport);
}

KnowledgeVectorIndexStats QdrantKnowledgeVectorIndex::stats() const {
  auto response = request_json(RequestJsonOptions{
      .base_url = config_.base_url,
      .path = path(""),
      .method = "GET",
      .headers = headers(),
  }, config_.transport);
  KnowledgeVectorIndexStats stats;
  const auto& result = response.at("result");
  stats.chunk_count = static_cast<std::size_t>(std::max<long long>(
      0, result.at("points_count").as_integer(result.at("vectors_count").as_integer(0))));
  return stats;
}

PgVectorKnowledgeVectorIndex::PgVectorKnowledgeVectorIndex(PgVectorKnowledgeVectorIndexConfig config)
    : config_(std::move(config)) {
  if (!config_.client) {
    throw ConfigurationError("PgVectorKnowledgeVectorIndex requires a query client.");
  }
  if (config_.schema_name.empty()) {
    config_.schema_name = "public";
  }
  if (config_.table_name.empty()) {
    config_.table_name = "node_agent_knowledge_chunks";
  }
}

std::string PgVectorKnowledgeVectorIndex::qualified_table() const {
  return pgvector_identifier(config_.schema_name) + "." + pgvector_identifier(config_.table_name);
}

PgVectorQueryResult PgVectorKnowledgeVectorIndex::query(std::string sql, std::vector<Value> params) const {
  return config_.client(PgVectorQuery{std::move(sql), std::move(params)});
}

void PgVectorKnowledgeVectorIndex::ensure_ready(int dimensions) const {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (ready_ || !config_.create_table) {
    return;
  }
  const int resolved_dimensions = dimensions > 0 ? dimensions : config_.dimensions;
  if (resolved_dimensions <= 0) {
    throw ConfigurationError("pgvector dimensions are required when create_table is enabled.");
  }
  (void)query("CREATE EXTENSION IF NOT EXISTS vector");
  (void)query("CREATE SCHEMA IF NOT EXISTS " + pgvector_identifier(config_.schema_name));
  (void)query(
      "CREATE TABLE IF NOT EXISTS " + qualified_table() + " ("
      "chunk_id TEXT PRIMARY KEY,"
      "document_id TEXT NOT NULL,"
      "knowledge_base_id TEXT NOT NULL,"
      "tenant_id TEXT NOT NULL,"
      "source_type TEXT NOT NULL,"
      "asset_type TEXT NOT NULL,"
      "uri TEXT NOT NULL,"
      "title TEXT NOT NULL,"
      "embedding_space_id TEXT,"
      "metadata JSONB NOT NULL DEFAULT '{}'::jsonb,"
      "embedding VECTOR(" + std::to_string(resolved_dimensions) + ") NOT NULL"
      ")");
  (void)query("CREATE INDEX IF NOT EXISTS " + pgvector_identifier(config_.table_name + "_kb_tenant_idx") +
              " ON " + qualified_table() + " (knowledge_base_id, tenant_id)");
  ready_ = true;
}

void PgVectorKnowledgeVectorIndex::upsert(const std::vector<KnowledgeChunkRecord>& chunks) {
  if (chunks.empty()) {
    return;
  }
  ensure_ready(static_cast<int>(chunks.front().embedding.size()));
  std::vector<Value> params;
  std::vector<std::string> tuples;
  params.reserve(chunks.size() * 11);
  tuples.reserve(chunks.size());
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    const auto& chunk = chunks[index];
    const std::size_t offset = index * 11;
    params.push_back(chunk.id);
    params.push_back(chunk.document_id);
    params.push_back(chunk.knowledge_base_id);
    params.push_back(chunk.tenant_id);
    params.push_back(chunk.source_type);
    params.push_back(knowledge_asset_type_label(chunk.asset_type));
    params.push_back(chunk.uri);
    params.push_back(chunk.title);
    params.push_back(chunk.embedding_space_id.empty() ? Value() : Value(chunk.embedding_space_id));
    params.push_back(chunk.metadata.stringify());
    params.push_back(pgvector_literal(chunk.embedding));
    tuples.push_back("($" + std::to_string(offset + 1) +
                     ", $" + std::to_string(offset + 2) +
                     ", $" + std::to_string(offset + 3) +
                     ", $" + std::to_string(offset + 4) +
                     ", $" + std::to_string(offset + 5) +
                     ", $" + std::to_string(offset + 6) +
                     ", $" + std::to_string(offset + 7) +
                     ", $" + std::to_string(offset + 8) +
                     ", $" + std::to_string(offset + 9) +
                     ", $" + std::to_string(offset + 10) + "::jsonb" +
                     ", $" + std::to_string(offset + 11) + "::vector)");
  }
  std::ostringstream sql;
  sql << "INSERT INTO " << qualified_table()
      << " (chunk_id, document_id, knowledge_base_id, tenant_id, source_type, asset_type, uri, title, "
         "embedding_space_id, metadata, embedding) VALUES ";
  for (std::size_t index = 0; index < tuples.size(); ++index) {
    if (index > 0) {
      sql << ", ";
    }
    sql << tuples[index];
  }
  sql << " ON CONFLICT (chunk_id) DO UPDATE SET "
      << "document_id = EXCLUDED.document_id, "
      << "knowledge_base_id = EXCLUDED.knowledge_base_id, "
      << "tenant_id = EXCLUDED.tenant_id, "
      << "source_type = EXCLUDED.source_type, "
      << "asset_type = EXCLUDED.asset_type, "
      << "uri = EXCLUDED.uri, "
      << "title = EXCLUDED.title, "
      << "embedding_space_id = EXCLUDED.embedding_space_id, "
      << "metadata = EXCLUDED.metadata, "
      << "embedding = EXCLUDED.embedding";
  (void)query(sql.str(), std::move(params));
}

std::vector<KnowledgeVectorMatch> PgVectorKnowledgeVectorIndex::search(
    const KnowledgeVectorSearchOptions& options) const {
  if (options.top_k == 0) {
    return {};
  }
  ensure_ready(static_cast<int>(options.embedding.size()));
  std::vector<std::string> conditions = {"knowledge_base_id = $2", "tenant_id = $3"};
  std::vector<Value> params = {pgvector_literal(options.embedding), options.knowledge_base_id, options.tenant_id};
  auto next_param = [&]() {
    return "$" + std::to_string(params.size() + 1);
  };
  if (!options.source_types.empty()) {
    conditions.push_back("source_type = ANY(" + next_param() + "::text[])");
    params.push_back(string_vector_to_value(options.source_types));
  }
  if (!options.asset_types.empty()) {
    conditions.push_back("asset_type = ANY(" + next_param() + "::text[])");
    params.push_back(asset_type_vector_to_value(options.asset_types));
  }
  if (!options.uri_prefix.empty()) {
    conditions.push_back("uri LIKE " + next_param());
    params.push_back(options.uri_prefix + "%");
  }
  if (!options.document_ids.empty()) {
    conditions.push_back("document_id = ANY(" + next_param() + "::text[])");
    params.push_back(string_vector_to_value(options.document_ids));
  }
  if (!options.chunk_ids.empty()) {
    conditions.push_back("chunk_id = ANY(" + next_param() + "::text[])");
    params.push_back(string_vector_to_value(options.chunk_ids));
  }
  if (!options.space_id.empty()) {
    conditions.push_back("embedding_space_id = " + next_param());
    params.push_back(options.space_id);
  }
  if (options.metadata.is_object() && !options.metadata.as_object().empty()) {
    conditions.push_back("metadata @> " + next_param() + "::jsonb");
    params.push_back(options.metadata.stringify());
  }

  std::ostringstream sql;
  sql << "SELECT chunk_id, GREATEST(0, 1 - (embedding <=> $1::vector)) AS score FROM "
      << qualified_table() << " WHERE ";
  for (std::size_t index = 0; index < conditions.size(); ++index) {
    if (index > 0) {
      sql << " AND ";
    }
    sql << conditions[index];
  }
  sql << " ORDER BY embedding <=> $1::vector ASC LIMIT " << std::max<std::size_t>(options.top_k, 1);

  const auto result = query(sql.str(), std::move(params));
  std::vector<KnowledgeVectorMatch> matches;
  for (const auto& row : result.rows) {
    const auto chunk_found = row.find("chunk_id");
    if (chunk_found == row.end()) {
      continue;
    }
    const auto score_found = row.find("score");
    const double score = score_found == row.end() ? 0 : score_found->second.as_number();
    if (score >= options.min_score) {
      matches.push_back(KnowledgeVectorMatch{chunk_found->second.as_string(), score});
    }
    if (matches.size() >= options.top_k) {
      break;
    }
  }
  return matches;
}

void PgVectorKnowledgeVectorIndex::delete_chunks(const std::vector<std::string>& chunk_ids) {
  if (chunk_ids.empty()) {
    return;
  }
  (void)query("DELETE FROM " + qualified_table() + " WHERE chunk_id = ANY($1::text[])",
              {string_vector_to_value(chunk_ids)});
}

void PgVectorKnowledgeVectorIndex::clear(const KnowledgeVectorIndexClearOptions& options) {
  std::vector<std::string> conditions;
  std::vector<Value> params;
  if (!options.knowledge_base_id.empty()) {
    params.push_back(options.knowledge_base_id);
    conditions.push_back("knowledge_base_id = $" + std::to_string(params.size()));
  }
  if (!options.tenant_id.empty()) {
    params.push_back(options.tenant_id);
    conditions.push_back("tenant_id = $" + std::to_string(params.size()));
  }
  std::string sql = "DELETE FROM " + qualified_table();
  if (!conditions.empty()) {
    sql += " WHERE ";
    for (std::size_t index = 0; index < conditions.size(); ++index) {
      if (index > 0) {
        sql += " AND ";
      }
      sql += conditions[index];
    }
  }
  (void)query(std::move(sql), std::move(params));
}

KnowledgeVectorIndexStats PgVectorKnowledgeVectorIndex::stats() const {
  const auto result = query("SELECT COUNT(*)::text AS count FROM " + qualified_table());
  KnowledgeVectorIndexStats stats;
  if (!result.rows.empty()) {
    const auto found = result.rows.front().find("count");
    if (found != result.rows.front().end()) {
      long long count = 0;
      if (found->second.is_string()) {
        count = std::stoll(found->second.as_string("0"));
      } else {
        count = found->second.as_integer(static_cast<long long>(found->second.as_number()));
      }
      stats.chunk_count = static_cast<std::size_t>(std::max<long long>(0, count));
    }
  }
  return stats;
}

KnowledgeVectorIndexRegistry::KnowledgeVectorIndexRegistry(std::map<std::string, Factory> factories) {
  for (auto& [name, factory] : factories) {
    register_index(std::move(name), std::move(factory));
  }
}

KnowledgeVectorIndexRegistry::KnowledgeVectorIndexRegistry(std::vector<KnowledgeVectorIndexProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeVectorIndexRegistry::KnowledgeVectorIndexRegistry(const KnowledgeVectorIndexRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeVectorIndexRegistry& KnowledgeVectorIndexRegistry::operator=(const KnowledgeVectorIndexRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeVectorIndexRegistry::KnowledgeVectorIndexRegistry(KnowledgeVectorIndexRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeVectorIndexRegistry& KnowledgeVectorIndexRegistry::operator=(KnowledgeVectorIndexRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeVectorIndexProvider& KnowledgeVectorIndexRegistry::register_provider(KnowledgeVectorIndexProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge vector index provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge vector index provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

void KnowledgeVectorIndexRegistry::register_index(std::string name, Factory factory) {
  if (name.empty()) {
    throw ConfigurationError("Knowledge vector index name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Knowledge vector index factory is required: " + name);
  }
  const auto provider_name = name;
  register_provider(KnowledgeVectorIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = provider_name,
          .tier = "core-safe",
          .title = provider_name,
          .tags = {"vector-index"},
      },
      .create = [factory = std::move(factory)](const Value&, const KnowledgeVectorIndexRegistry&) {
        return factory();
      },
  });
}

const KnowledgeVectorIndexProvider* KnowledgeVectorIndexRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeVectorIndex> KnowledgeVectorIndexRegistry::create(const std::string& name,
                                                                           const Value& options) const {
  KnowledgeVectorIndexProvider::Factory factory;
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

std::vector<KnowledgeVectorIndexProvider> KnowledgeVectorIndexRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeVectorIndexProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<std::string> KnowledgeVectorIndexRegistry::list_names() const {
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

KnowledgeVectorIndexRegistry create_default_knowledge_vector_index_registry() {
  KnowledgeVectorIndexRegistry registry;
  registry.register_provider(KnowledgeVectorIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "memory",
          .tier = "core-safe",
          .title = "In-Memory Vector Index",
          .tags = {"vector-index", "memory"},
      },
      .create = [](const Value&, const KnowledgeVectorIndexRegistry&) {
        return std::make_shared<InMemoryKnowledgeVectorIndex>();
      },
  });
  registry.register_provider(KnowledgeVectorIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "sqlite",
          .tier = "portable",
          .title = "SQLite Vector Index",
          .description = "File-backed local vector index with SQLite-compatible exact similarity semantics.",
          .tags = {"vector-index", "sqlite", "local"},
      },
      .create = [](const Value& options, const KnowledgeVectorIndexRegistry&) {
        const auto file_path = options.at("filePath").as_string();
        if (file_path.empty()) {
          throw ConfigurationError("SQLite knowledge vector index provider requires filePath.");
        }
        return std::make_shared<SqliteKnowledgeVectorIndex>(SqliteKnowledgeVectorIndexConfig{
            .file_path = file_path,
            .dimensions = static_cast<int>(std::max<long long>(0, options.at("dimensions").as_integer(0))),
            .table_name = options.at("tableName").as_string("knowledge_vectors"),
            .create_table = options.at("createTable").as_bool(true),
            .namespace_id = options.at("namespace").as_string(options.at("namespaceId").as_string()),
            .batch_size = static_cast<std::size_t>(std::max<long long>(1, options.at("batchSize").as_integer(200))),
        });
      },
  });
  registry.register_provider(KnowledgeVectorIndexProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "qdrant",
          .tier = "external",
          .title = "Qdrant Vector Index",
          .description = "Qdrant-backed vector index using an injected HTTP transport.",
          .tags = {"vector-index", "qdrant", "remote"},
      },
      .create = [](const Value& options, const KnowledgeVectorIndexRegistry&) {
        return std::make_shared<QdrantKnowledgeVectorIndex>(QdrantKnowledgeVectorIndexConfig{
            .base_url = options.at("baseUrl").as_string(),
            .collection = options.at("collection").as_string(),
            .api_key = options.at("apiKey").as_string(),
            .headers = string_map_from_value(options.at("headers")),
            .vector_name = options.at("vectorName").as_string(),
            .create_collection = options.at("createCollection").as_bool(false),
            .distance = options.at("distance").as_string("Cosine"),
            .dimensions = static_cast<int>(std::max<long long>(0, options.at("dimensions").as_integer(0))),
            .wait = options.at("wait").as_bool(true),
            .oversample_factor =
                static_cast<std::size_t>(std::max<long long>(1, options.at("oversampleFactor").as_integer(4))),
        });
      },
  });
  return registry;
}
