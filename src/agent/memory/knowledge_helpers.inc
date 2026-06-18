
namespace {

const std::set<std::string>& default_knowledge_text_extensions() {
  static const std::set<std::string> extensions = {
      ".txt", ".md",  ".mdx", ".json", ".yaml", ".yml", ".toml", ".xml",  ".html",
      ".htm", ".csv", ".ts",  ".tsx",  ".js",   ".jsx", ".mjs",  ".cjs",  ".py",
      ".java", ".go", ".rs",  ".cpp",  ".c",    ".h",   ".hpp",  ".cs",   ".php",
      ".rb",  ".swift", ".kt", ".scala", ".sh", ".sql", ".vue",  ".svelte", ".pdf",
      ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"};
  return extensions;
}

const std::set<std::string>& default_knowledge_exclude_directories() {
  static const std::set<std::string> directories = {
      ".git", "node_modules", "dist", "build", "coverage", ".next", ".nuxt", "target", "out"};
  return directories;
}

const std::set<std::string>& knowledge_image_extensions() {
  static const std::set<std::string> extensions = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"};
  return extensions;
}

const std::set<std::string>& markdown_knowledge_extensions() {
  static const std::set<std::string> extensions = {".md", ".mdx"};
  return extensions;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string extension_for_path(const std::filesystem::path& path) {
  return lower_copy(path.extension().string());
}

std::string normalize_extension(std::string extension) {
  extension = lower_copy(std::move(extension));
  if (!extension.empty() && extension.front() != '.') {
    extension.insert(extension.begin(), '.');
  }
  return extension;
}

std::set<std::string> string_set_from_value(const Value& value, const std::set<std::string>& fallback = {}) {
  if (!value.is_array()) {
    return fallback;
  }
  std::set<std::string> values;
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.insert(normalize_extension(text));
    }
  }
  return values;
}

std::set<std::string> string_set_preserve_from_value(const Value& value, const std::set<std::string>& fallback = {}) {
  if (!value.is_array()) {
    return fallback;
  }
  std::set<std::string> values;
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.insert(text);
    }
  }
  return values;
}

std::vector<std::string> string_vector_from_value(const Value& value) {
  std::vector<std::string> values;
  if (!value.is_array()) {
    return values;
  }
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.push_back(text);
    }
  }
  return values;
}

std::size_t size_option_from_value(const Value& options, const std::string& key, std::size_t fallback) {
  if (!options.is_object() || !options.contains(key)) {
    return fallback;
  }
  const auto raw = options.at(key).as_integer(static_cast<long long>(fallback));
  if (raw < 0) {
    return fallback;
  }
  return static_cast<std::size_t>(raw);
}

Value copy_object_or_empty(const Value& value) {
  return value.is_object() ? value : Value::object({});
}

std::string metadata_text(const Value& metadata, const std::string& key) {
  if (!metadata.is_object() || !metadata.contains(key)) {
    return {};
  }
  return metadata.at(key).as_string();
}

std::string find_markdown_title(const std::string& content, const std::string& fallback) {
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    auto trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed.front() != '#') {
      continue;
    }
    std::size_t index = 0;
    while (index < trimmed.size() && trimmed[index] == '#') {
      ++index;
    }
    if (index < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[index]))) {
      return trim_copy(trimmed.substr(index));
    }
  }
  return fallback;
}

LoadedKnowledgeDocument build_loaded_document(std::string source_type,
                                              std::string uri,
                                              std::string title,
                                              std::string content,
                                              Value metadata = Value::object({}),
                                              KnowledgeAssetType asset_type = KnowledgeAssetType::Text,
                                              std::optional<MediaSource> media = std::nullopt,
                                              std::string text_hint = {},
                                              std::string alt_text = {},
                                              std::string ocr_text = {},
                                              std::string caption = {}) {
  LoadedKnowledgeDocument document;
  document.source_type = std::move(source_type);
  document.uri = std::move(uri);
  document.title = title.empty() ? document.uri : std::move(title);
  document.content = std::move(content);
  document.asset_type = asset_type;
  document.media = std::move(media);
  document.text_hint = std::move(text_hint);
  document.alt_text = std::move(alt_text);
  document.ocr_text = std::move(ocr_text);
  document.caption = std::move(caption);
  document.metadata = std::move(metadata);
  return document;
}

std::string extract_html_title(const std::string& html) {
  static const std::regex title_pattern(R"(<\s*title[^>]*>(.*?)<\s*/\s*title\s*>)",
                                        std::regex::icase);
  std::smatch match;
  if (std::regex_search(html, match, title_pattern) && match.size() > 1) {
    return trim_copy(html_to_text(match[1].str()));
  }
  return {};
}

std::string knowledge_asset_type_label(KnowledgeAssetType type) {
  switch (type) {
    case KnowledgeAssetType::Image:
      return "image";
    case KnowledgeAssetType::Text:
      return "text";
  }
  return "text";
}

KnowledgeAssetType knowledge_asset_type_from_label(const std::string& label) {
  return label == "image" ? KnowledgeAssetType::Image : KnowledgeAssetType::Text;
}

Value embedding_to_value(const EmbeddingVector& embedding) {
  Value::Array values;
  for (const auto value : embedding) {
    values.emplace_back(value);
  }
  return Value(values);
}

EmbeddingVector embedding_from_value(const Value& value) {
  EmbeddingVector embedding;
  for (const auto& item : value.as_array()) {
    embedding.push_back(item.as_number());
  }
  return embedding;
}

Value media_source_to_value(const std::optional<MediaSource>& media) {
  if (!media) {
    return Value();
  }
  return Value::object({{"kind", media_source_kind_label(media->kind)},
                        {"data", media->data},
                        {"url", media->url},
                        {"path", media->path},
                        {"key", media->key},
                        {"mimeType", media->mime_type},
                        {"filename", media->filename}});
}

std::optional<MediaSource> media_source_from_value(const Value& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }
  MediaSource source;
  source.kind = media_source_kind_from_label(value.at("kind").as_string("inline"));
  source.data = value.at("data").as_string();
  source.url = value.at("url").as_string();
  source.path = value.at("path").as_string();
  source.key = value.at("key").as_string();
  source.mime_type = value.at("mimeType").as_string();
  source.filename = value.at("filename").as_string();
  return source;
}

ImageEmbeddingInput image_embedding_input_from_document(const LoadedKnowledgeDocument& document) {
  if (!document.media) {
    throw ConfigurationError("Image knowledge document is missing media source: " + document.uri);
  }
  ImageEmbeddingInput input;
  input.source = *document.media;
  input.alt_text = !document.alt_text.empty() ? document.alt_text : metadata_text(document.metadata, "altText");
  if (input.alt_text.empty()) {
    input.alt_text = document.text_hint;
  }
  input.title = document.title;
  std::vector<std::string> hints = {document.content,
                                    document.ocr_text.empty() ? metadata_text(document.metadata, "ocrText") : document.ocr_text,
                                    document.caption.empty() ? metadata_text(document.metadata, "caption") : document.caption,
                                    document.text_hint};
  for (const auto& hint : hints) {
    if (hint.empty()) {
      continue;
    }
    if (!input.text_hint.empty()) {
      input.text_hint += "\n";
    }
    input.text_hint += hint;
  }
  input.metadata = document.metadata;
  return input;
}

std::string image_embedding_query_text(const ImageEmbeddingInput& query) {
  std::string text;
  for (const auto& part : std::vector<std::string>{query.title, query.alt_text, query.text_hint}) {
    if (part.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += " ";
    }
    text += part;
  }
  return text.empty() ? "[image]" : text;
}

Value knowledge_document_record_to_value(const KnowledgeDocumentRecord& document) {
  return Value::object({{"id", document.id},
                        {"knowledgeBaseId", document.knowledge_base_id},
                        {"tenantId", document.tenant_id},
                        {"sourceType", document.source_type},
                        {"assetType", knowledge_asset_type_label(document.asset_type)},
                        {"uri", document.uri},
                        {"title", document.title},
                        {"content", document.content},
                        {"media", media_source_to_value(document.media)},
                        {"textHint", document.text_hint},
                        {"metadata", document.metadata},
                        {"createdAt", document.created_at},
                        {"updatedAt", document.updated_at}});
}

KnowledgeDocumentRecord knowledge_document_record_from_value(const Value& value) {
  KnowledgeDocumentRecord document;
  document.id = value.at("id").as_string();
  document.knowledge_base_id = value.at("knowledgeBaseId").as_string();
  document.tenant_id = value.at("tenantId").as_string();
  document.source_type = value.at("sourceType").as_string();
  document.asset_type = knowledge_asset_type_from_label(value.at("assetType").as_string("text"));
  document.uri = value.at("uri").as_string();
  document.title = value.at("title").as_string();
  document.content = value.at("content").as_string();
  document.media = media_source_from_value(value.at("media"));
  document.text_hint = value.at("textHint").as_string();
  document.metadata = copy_object_or_empty(value.at("metadata"));
  document.created_at = value.at("createdAt").as_string();
  document.updated_at = value.at("updatedAt").as_string();
  return document;
}

Value knowledge_chunk_record_to_value(const KnowledgeChunkRecord& chunk) {
  return Value::object({{"id", chunk.id},
                        {"documentId", chunk.document_id},
                        {"knowledgeBaseId", chunk.knowledge_base_id},
                        {"tenantId", chunk.tenant_id},
                        {"sourceType", chunk.source_type},
                        {"assetType", knowledge_asset_type_label(chunk.asset_type)},
                        {"uri", chunk.uri},
                        {"title", chunk.title},
                        {"content", chunk.content},
                        {"chunkIndex", chunk.chunk_index},
                        {"startOffset", chunk.start_offset},
                        {"endOffset", chunk.end_offset},
                        {"lineStart", chunk.line_start},
                        {"lineEnd", chunk.line_end},
                        {"media", media_source_to_value(chunk.media)},
                        {"embeddingSpaceId", chunk.embedding_space_id},
                        {"metadata", chunk.metadata},
                        {"embedding", embedding_to_value(chunk.embedding)},
                        {"createdAt", chunk.created_at},
                        {"updatedAt", chunk.updated_at}});
}

KnowledgeChunkRecord knowledge_chunk_record_from_value(const Value& value) {
  KnowledgeChunkRecord chunk;
  chunk.id = value.at("id").as_string();
  chunk.document_id = value.at("documentId").as_string();
  chunk.knowledge_base_id = value.at("knowledgeBaseId").as_string();
  chunk.tenant_id = value.at("tenantId").as_string();
  chunk.source_type = value.at("sourceType").as_string();
  chunk.asset_type = knowledge_asset_type_from_label(value.at("assetType").as_string("text"));
  chunk.uri = value.at("uri").as_string();
  chunk.title = value.at("title").as_string();
  chunk.content = value.at("content").as_string();
  chunk.chunk_index = static_cast<std::size_t>(value.at("chunkIndex").as_integer());
  chunk.start_offset = static_cast<std::size_t>(value.at("startOffset").as_integer());
  chunk.end_offset = static_cast<std::size_t>(value.at("endOffset").as_integer());
  chunk.line_start = static_cast<std::size_t>(value.at("lineStart").as_integer());
  chunk.line_end = static_cast<std::size_t>(value.at("lineEnd").as_integer());
  chunk.media = media_source_from_value(value.at("media"));
  chunk.embedding_space_id = value.at("embeddingSpaceId").as_string();
  chunk.metadata = copy_object_or_empty(value.at("metadata"));
  chunk.embedding = embedding_from_value(value.at("embedding"));
  chunk.created_at = value.at("createdAt").as_string();
  chunk.updated_at = value.at("updatedAt").as_string();
  return chunk;
}

void sort_hits_by_rerank_score(std::vector<KnowledgeSearchHit>& hits) {
  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    const double left_score = left.rerank_score.value_or(left.score);
    const double right_score = right.rerank_score.value_or(right.score);
    if (left_score == right_score) {
      return left.score > right.score;
    }
    return left_score > right_score;
  });
}

std::size_t token_overlap_count(const std::string& query, const std::string& surface) {
  const auto query_tokens = tokenize(query);
  const auto surface_tokens = tokenize(surface);
  std::set<std::string> query_set(query_tokens.begin(), query_tokens.end());
  std::size_t overlap = 0;
  for (const auto& token : surface_tokens) {
    if (query_set.contains(token)) {
      ++overlap;
    }
  }
  return overlap;
}

bool vector_contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool metadata_matches_filter(const Value& record_metadata, const Value& filter_metadata) {
  if (!filter_metadata.is_object() || filter_metadata.as_object().empty()) {
    return true;
  }
  if (!record_metadata.is_object()) {
    return false;
  }
  for (const auto& [key, expected] : filter_metadata.as_object()) {
    const auto& actual = record_metadata.at(key);
    if (expected.is_array()) {
      bool any = false;
      for (const auto& item : expected.as_array()) {
        any = any || item == actual;
      }
      if (!any) {
        return false;
      }
      continue;
    }
    if (actual != expected) {
      return false;
    }
  }
  return true;
}

std::string knowledge_text_index_surface(const KnowledgeChunkRecord& chunk) {
  std::string surface = chunk.title + "\n" + chunk.content;
  for (const auto& key : {"altText", "ocrText", "caption"}) {
    const auto value = chunk.metadata.at(key).as_string();
    if (!value.empty()) {
      surface += "\n" + value;
    }
  }
  return surface;
}

std::vector<std::string> tokenize_knowledge_text(const std::string& text) {
  return tokenize(text);
}

std::map<std::string, std::size_t> build_term_frequency(const std::vector<std::string>& tokens) {
  std::map<std::string, std::size_t> frequencies;
  for (const auto& token : tokens) {
    ++frequencies[token];
  }
  return frequencies;
}

bool chunk_matches_text_search(const KnowledgeChunkRecord& chunk, const KnowledgeTextSearchOptions& options) {
  if (chunk.knowledge_base_id != options.knowledge_base_id || chunk.tenant_id != options.tenant_id) {
    return false;
  }
  if (!options.source_types.empty() && !vector_contains(options.source_types, chunk.source_type)) {
    return false;
  }
  if (!options.asset_types.empty() &&
      std::find(options.asset_types.begin(), options.asset_types.end(), chunk.asset_type) ==
          options.asset_types.end()) {
    return false;
  }
  if (!options.uri_prefix.empty() && chunk.uri.rfind(options.uri_prefix, 0) != 0) {
    return false;
  }
  if (!options.document_ids.empty() && !vector_contains(options.document_ids, chunk.document_id)) {
    return false;
  }
  if (!options.chunk_ids.empty() && !vector_contains(options.chunk_ids, chunk.id)) {
    return false;
  }
  if (!options.space_id.empty() && chunk.embedding_space_id != options.space_id) {
    return false;
  }
  return metadata_matches_filter(chunk.metadata, options.metadata);
}

bool chunk_matches_vector_search(const KnowledgeChunkRecord& chunk, const KnowledgeVectorSearchOptions& options) {
  if (chunk.knowledge_base_id != options.knowledge_base_id || chunk.tenant_id != options.tenant_id) {
    return false;
  }
  if (!options.source_types.empty() && !vector_contains(options.source_types, chunk.source_type)) {
    return false;
  }
  if (!options.asset_types.empty() &&
      std::find(options.asset_types.begin(), options.asset_types.end(), chunk.asset_type) ==
          options.asset_types.end()) {
    return false;
  }
  if (!options.uri_prefix.empty() && chunk.uri.rfind(options.uri_prefix, 0) != 0) {
    return false;
  }
  if (!options.document_ids.empty() && !vector_contains(options.document_ids, chunk.document_id)) {
    return false;
  }
  if (!options.chunk_ids.empty() && !vector_contains(options.chunk_ids, chunk.id)) {
    return false;
  }
  if (!options.space_id.empty() && chunk.embedding_space_id != options.space_id) {
    return false;
  }
  return metadata_matches_filter(chunk.metadata, options.metadata);
}

bool sqlite_vector_namespace_matches(const std::string& configured_namespace,
                                     const std::string& record_namespace) {
  return configured_namespace.empty() || configured_namespace == record_namespace;
}

std::map<std::string, std::string> string_map_from_value(const Value& value) {
  std::map<std::string, std::string> result;
  if (!value.is_object()) {
    return result;
  }
  for (const auto& [key, item] : value.as_object()) {
    result[key] = item.as_string();
  }
  return result;
}

double vector_index_score(const EmbeddingVector& normalized_query, const EmbeddingVector& embedding) {
  return std::max(0.0, (dot_product(normalized_query, normalize_vector(embedding)) + 1.0) / 2.0);
}

struct RankedKnowledgeChunk {
  const KnowledgeChunkRecord* chunk = nullptr;
  double score = 0;
};

std::size_t effective_knowledge_vector_top_k(const KnowledgeSearchOptions& options,
                                             std::size_t candidate_count) {
  if (candidate_count == 0) {
    return 0;
  }
  const auto top_k = options.top_k == 0 ? std::size_t{4} : options.top_k;
  const auto oversampled = static_cast<std::size_t>(
      std::ceil(static_cast<double>(top_k) * std::max(1.0, options.oversample_factor)));
  const auto requested = options.vector_top_k == 0 ? candidate_count : options.vector_top_k;
  return std::min(candidate_count, std::max(requested, oversampled));
}

double knowledge_modality_weight(const KnowledgeSearchOptions& options,
                                 KnowledgeAssetType asset_type) {
  const auto found = options.modality_weights.find(asset_type);
  return found == options.modality_weights.end() ? 1.0 : found->second;
}

bool knowledge_search_metadata_is_set(const Value& value) {
  return value.is_object() && !value.as_object().empty();
}

Value knowledge_search_debug_candidate_value(const std::string& knowledge_base_id,
                                             const std::string& tenant_id,
                                             const KnowledgeChunkRecord& chunk,
                                             double score,
                                             std::optional<double> vector_score = std::nullopt,
                                             std::optional<double> lexical_score = std::nullopt) {
  auto value = Value::object({{"knowledgeBaseId", knowledge_base_id},
                             {"tenantId", tenant_id},
                             {"documentId", chunk.document_id},
                             {"chunkId", chunk.id},
                             {"uri", chunk.uri},
                             {"title", chunk.title},
                             {"score", score}});
  if (vector_score.has_value()) {
    value["vectorScore"] = *vector_score;
  }
  if (lexical_score.has_value()) {
    value["lexicalScore"] = *lexical_score;
  }
  return value;
}

std::string value_to_id_string(const Value& value) {
  if (value.is_string()) {
    return value.as_string();
  }
  if (value.is_number()) {
    return std::to_string(value.as_integer());
  }
  return {};
}

Value qdrant_search_filter(const KnowledgeVectorSearchOptions& options) {
  Value::Array must;
  must.push_back(Value::object({{"key", "knowledgeBaseId"},
                                {"match", Value::object({{"value", options.knowledge_base_id}})}}));
  must.push_back(Value::object({{"key", "tenantId"},
                                {"match", Value::object({{"value", options.tenant_id}})}}));
  if (options.source_types.size() == 1) {
    must.push_back(Value::object({{"key", "sourceType"},
                                  {"match", Value::object({{"value", options.source_types.front()}})}}));
  }
  if (options.asset_types.size() == 1) {
    must.push_back(Value::object({{"key", "assetType"},
                                  {"match", Value::object({{"value", knowledge_asset_type_label(options.asset_types.front())}})}}));
  }
  if (!options.space_id.empty()) {
    must.push_back(Value::object({{"key", "embeddingSpaceId"},
                                  {"match", Value::object({{"value", options.space_id}})}}));
  }
  if (options.document_ids.size() == 1) {
    must.push_back(Value::object({{"key", "documentId"},
                                  {"match", Value::object({{"value", options.document_ids.front()}})}}));
  }
  if (!options.chunk_ids.empty()) {
    Value::Array ids;
    for (const auto& id : options.chunk_ids) {
      ids.push_back(id);
    }
    must.push_back(Value::object({{"has_id", Value(ids)}}));
  }
  if (options.metadata.is_object()) {
    for (const auto& [key, expected] : options.metadata.as_object()) {
      if (!expected.is_array()) {
        must.push_back(Value::object({{"key", "metadata." + key}, {"match", Value::object({{"value", expected}})}}));
      }
    }
  }
  return Value::object({{"must", Value(must)}});
}

bool qdrant_payload_matches(const Value& payload, const KnowledgeVectorSearchOptions& options) {
  if (!payload.is_object()) {
    return false;
  }
  if (!options.source_types.empty() && !vector_contains(options.source_types, payload.at("sourceType").as_string())) {
    return false;
  }
  if (!options.uri_prefix.empty() && payload.at("uri").as_string().rfind(options.uri_prefix, 0) != 0) {
    return false;
  }
  if (!options.asset_types.empty()) {
    const auto asset_type = knowledge_asset_type_from_label(payload.at("assetType").as_string("text"));
    if (std::find(options.asset_types.begin(), options.asset_types.end(), asset_type) == options.asset_types.end()) {
      return false;
    }
  }
  if (!options.space_id.empty() && payload.at("embeddingSpaceId").as_string() != options.space_id) {
    return false;
  }
  if (!options.document_ids.empty() && !vector_contains(options.document_ids, payload.at("documentId").as_string())) {
    return false;
  }
  if (!options.chunk_ids.empty()) {
    const auto chunk_id = payload.at("id").as_string(payload.at("chunkId").as_string());
    if (!vector_contains(options.chunk_ids, chunk_id)) {
      return false;
    }
  }
  return metadata_matches_filter(payload.at("metadata"), options.metadata);
}

std::string pgvector_identifier(const std::string& value) {
  std::string escaped = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped += "\"";
  return escaped;
}

std::string pgvector_literal(const EmbeddingVector& vector) {
  std::ostringstream out;
  out << "[";
  for (std::size_t index = 0; index < vector.size(); ++index) {
    if (index > 0) {
      out << ",";
    }
    out << std::fixed << std::setprecision(12) << vector[index];
  }
  out << "]";
  return out.str();
}

Value string_vector_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(array);
}

Value asset_type_vector_to_value(const std::vector<KnowledgeAssetType>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.push_back(knowledge_asset_type_label(value));
  }
  return Value(array);
}

void report_knowledge_progress(const KnowledgeIngestionProgressCallback& callback,
                               std::string phase,
                               std::size_t source_count,
                               std::size_t loaded_document_count,
                               std::size_t processed_document_count = 0,
                               std::size_t processed_chunk_count = 0,
                               std::size_t batch_index = 0,
                               std::size_t total_batches = 0) {
  if (!callback) {
    return;
  }
  callback(KnowledgeIngestionProgress{std::move(phase),
                                      source_count,
                                      loaded_document_count,
                                      processed_document_count,
                                      processed_chunk_count,
                                      batch_index,
                                      total_batches});
}

void throw_if_knowledge_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Retrieval);
  }
}

std::vector<std::vector<LoadedKnowledgeDocument>> chunk_loaded_documents(
    const std::vector<LoadedKnowledgeDocument>& documents,
    std::size_t batch_size) {
  if (documents.empty()) {
    return {};
  }
  batch_size = std::max<std::size_t>(batch_size, 1);
  std::vector<std::vector<LoadedKnowledgeDocument>> batches;
  for (std::size_t index = 0; index < documents.size(); index += batch_size) {
    const auto end = std::min(documents.size(), index + batch_size);
    batches.emplace_back(documents.begin() + static_cast<std::ptrdiff_t>(index),
                         documents.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return batches;
}

template <typename Operation>
auto run_with_knowledge_retry(Operation operation,
                              const KnowledgeRetryStrategy& retry_strategy,
                              const KnowledgeRetryContext& context,
                              CancellationToken* cancellation = nullptr) -> decltype(operation()) {
  const auto max_attempts = std::max<std::size_t>(retry_strategy.max_attempts(context), 1);
  std::exception_ptr last_error;
  for (std::size_t attempt = 1; attempt <= max_attempts; ++attempt) {
    throw_if_knowledge_cancelled(cancellation);
    try {
      return operation();
    } catch (const std::exception& error) {
      last_error = std::current_exception();
      throw_if_knowledge_cancelled(cancellation);
      if (!retry_strategy.should_retry(error, context, attempt, max_attempts)) {
        break;
      }
      const int retry_delay_ms = std::max(retry_strategy.retry_delay_ms(error, context, attempt), 0);
      if (retry_delay_ms > 0) {
        sleep_with_cancellation(std::chrono::milliseconds(retry_delay_ms),
                                ExecutionTarget::Retrieval,
                                cancellation);
      }
    } catch (...) {
      last_error = std::current_exception();
      throw_if_knowledge_cancelled(cancellation);
      const std::runtime_error unknown_error("Unknown knowledge retry error.");
      if (!retry_strategy.should_retry(unknown_error, context, attempt, max_attempts)) {
        break;
      }
      const int retry_delay_ms = std::max(retry_strategy.retry_delay_ms(unknown_error, context, attempt), 0);
      if (retry_delay_ms > 0) {
        sleep_with_cancellation(std::chrono::milliseconds(retry_delay_ms),
                                ExecutionTarget::Retrieval,
                                cancellation);
      }
    }
  }
  std::rethrow_exception(last_error);
}

std::vector<LoadedKnowledgeDocument> filter_unchanged_documents(
    const KnowledgeBase& knowledge_base,
    const std::vector<LoadedKnowledgeDocument>& documents,
    bool skip_if_unchanged) {
  if (!skip_if_unchanged) {
    return documents;
  }

  std::map<std::string, std::set<std::string>> hashes_by_uri;
  for (const auto& document : knowledge_base.store()->list_documents()) {
    hashes_by_uri[document.uri].insert(sha256_hex(document.content));
  }

  std::vector<LoadedKnowledgeDocument> filtered;
  for (const auto& document : documents) {
    const auto found = hashes_by_uri.find(document.uri);
    const auto hash = sha256_hex(document.content);
    if (found != hashes_by_uri.end() && found->second.contains(hash)) {
      continue;
    }
    filtered.push_back(document);
  }
  return filtered;
}

KnowledgeIngestionResult ingest_loaded_document_batches(
    KnowledgeBase& knowledge_base,
    std::size_t source_count,
    std::size_t loaded_document_count,
    const std::vector<LoadedKnowledgeDocument>& documents,
    const KnowledgeIngestionPipelineOptions& options) {
  const auto batches = chunk_loaded_documents(documents, options.document_batch_size);
  report_knowledge_progress(options.on_progress, "planning", source_count, loaded_document_count, 0, 0, 0,
                            batches.size());
  FixedKnowledgeRetryStrategy default_retry_strategy(options.max_attempts, options.retry_delay_ms);
  const auto& retry_strategy = options.retry_strategy ? *options.retry_strategy : default_retry_strategy;

  KnowledgeIngestionResult aggregate;
  aggregate.knowledge_base_id = knowledge_base.id();
  aggregate.tenant_id = knowledge_base.tenant_id();

  for (std::size_t index = 0; index < batches.size(); ++index) {
    throw_if_knowledge_cancelled(options.cancellation);
    auto result = run_with_knowledge_retry([&]() {
      return knowledge_base.ingest_loaded_documents(
          batches[index],
          KnowledgeIngestOptions{
              .replace_existing = options.replace_existing,
              .skip_if_unchanged = options.skip_if_unchanged,
              .embedding_batch_size = options.embedding_batch_size,
              .cancellation = options.cancellation,
          });
    }, retry_strategy, KnowledgeRetryContext{knowledge_base, "ingesting", index + 1, batches.size()},
       options.cancellation);

    aggregate.document_count += result.document_count;
    aggregate.chunk_count += result.chunk_count;
    aggregate.documents.insert(aggregate.documents.end(),
                               std::make_move_iterator(result.documents.begin()),
                               std::make_move_iterator(result.documents.end()));
    aggregate.chunks.insert(aggregate.chunks.end(),
                            std::make_move_iterator(result.chunks.begin()),
                            std::make_move_iterator(result.chunks.end()));

    report_knowledge_progress(options.on_progress, "ingesting", source_count, loaded_document_count,
                              aggregate.document_count, aggregate.chunk_count, index + 1, batches.size());
  }

  report_knowledge_progress(options.on_progress, "completed", source_count, loaded_document_count,
                            aggregate.document_count, aggregate.chunk_count, batches.size(), batches.size());
  return aggregate;
}

double sortable_timestamp_key(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  std::string digits;
  digits.reserve(14);
  for (const unsigned char ch : value) {
    if (std::isdigit(ch)) {
      digits.push_back(static_cast<char>(ch));
      if (digits.size() >= 14) {
        break;
      }
    }
  }
  if (digits.size() < 8) {
    return 0;
  }
  while (digits.size() < 14) {
    digits.push_back('0');
  }
  return std::stod(digits.substr(0, 14));
}

}  // namespace

KnowledgeSearchOptions default_knowledge_search_options() {
  KnowledgeSearchOptions options;
  options.top_k = 6;
  options.vector_top_k = 12;
  options.lexical_top_k = 12;
  options.min_score = 0.15;
  options.hybrid_alpha = 0.6;
  options.rerank_top_k = 8;
  options.retrieval_mode = "hybrid";
  options.oversample_factor = 3.0;
  options.fusion = "rrf";
  options.modality_weights = {{KnowledgeAssetType::Text, 1.0}, {KnowledgeAssetType::Image, 1.0}};
  return options;
}

KnowledgeSearchOptions merge_knowledge_search_options(const KnowledgeSearchOptions& base,
                                                      const KnowledgeSearchOptions& overrides) {
  auto merged = base;
  if (overrides.top_k != 0) {
    merged.top_k = overrides.top_k;
  }
  if (overrides.vector_top_k != 0) {
    merged.vector_top_k = overrides.vector_top_k;
  }
  if (overrides.lexical_top_k != 0) {
    merged.lexical_top_k = overrides.lexical_top_k;
  }
  if (!std::isnan(overrides.min_score)) {
    merged.min_score = overrides.min_score;
  }
  if (!std::isnan(overrides.hybrid_alpha)) {
    merged.hybrid_alpha = overrides.hybrid_alpha;
  }
  if (overrides.rerank_top_k != 0) {
    merged.rerank_top_k = overrides.rerank_top_k;
  }
  if (!overrides.retrieval_mode.empty()) {
    merged.retrieval_mode = overrides.retrieval_mode;
  }
  if (overrides.oversample_factor > 0) {
    merged.oversample_factor = overrides.oversample_factor;
  }
  if (!overrides.fusion.empty()) {
    merged.fusion = overrides.fusion;
  }
  for (const auto& [asset_type, weight] : overrides.modality_weights) {
    merged.modality_weights[asset_type] = weight;
  }
  if (!overrides.uri_prefix.empty()) {
    merged.uri_prefix = overrides.uri_prefix;
  }
  if (!overrides.document_ids.empty()) {
    merged.document_ids = overrides.document_ids;
  }
  if (!overrides.asset_types.empty()) {
    merged.asset_types = overrides.asset_types;
  }
  if (!overrides.space_id.empty()) {
    merged.space_id = overrides.space_id;
  }
  if (!overrides.source_types.empty()) {
    merged.source_types = overrides.source_types;
  }
  if (!overrides.chunk_ids.empty()) {
    merged.chunk_ids = overrides.chunk_ids;
  }
  if (knowledge_search_metadata_is_set(overrides.metadata)) {
    merged.metadata = overrides.metadata;
  }
  if (overrides.cancellation) {
    merged.cancellation = overrides.cancellation;
  }
  return merged;
}

KnowledgeSearchOptions effective_knowledge_search_options(const KnowledgeSearchOptions& search_defaults,
                                                          const KnowledgeSearchOptions& options) {
  return merge_knowledge_search_options(
      merge_knowledge_search_options(default_knowledge_search_options(), search_defaults),
      options);
}

std::vector<LoadedKnowledgeDocument> UriContentHashDedupeStrategy::dedupe(
    const KnowledgeDedupeContext& context) const {
  return dedupe_loaded_knowledge_documents(context.loaded_documents, context.replace_existing);
}

KnowledgeIncrementalResult SkipUnchangedKnowledgeIncrementalStrategy::prepare(
    const KnowledgeIncrementalContext& context) const {
  KnowledgeIncrementalResult result;
  result.replace_existing = context.replace_existing;
  result.skip_if_unchanged = context.skip_if_unchanged;
  result.documents =
      filter_unchanged_documents(context.knowledge_base, context.documents, context.skip_if_unchanged);
  return result;
}

FixedKnowledgeRetryStrategy::FixedKnowledgeRetryStrategy(std::size_t max_attempts, int delay_ms)
    : max_attempts_(std::max<std::size_t>(max_attempts, 1)),
      delay_ms_(std::max(delay_ms, 0)) {}

std::size_t FixedKnowledgeRetryStrategy::max_attempts(const KnowledgeRetryContext&) const {
  return max_attempts_;
}

bool FixedKnowledgeRetryStrategy::should_retry(const std::exception&,
                                               const KnowledgeRetryContext&,
                                               std::size_t attempt,
                                               std::size_t max_attempts) const {
  return attempt < max_attempts;
}

int FixedKnowledgeRetryStrategy::retry_delay_ms(const std::exception&,
                                                const KnowledgeRetryContext&,
                                                std::size_t) const {
  return delay_ms_;
}
