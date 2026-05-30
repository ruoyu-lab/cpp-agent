#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <iomanip>
#include <limits>
#include <iterator>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace agent {

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

const std::set<std::string>& default_repository_extensions() {
  static const std::set<std::string> extensions = {
      ".md",   ".mdx", ".txt", ".json", ".yaml", ".yml", ".toml", ".ts",  ".tsx", ".js",   ".jsx",
      ".mjs",  ".cjs", ".py",  ".go",   ".rs",   ".java", ".sh",  ".sql", ".vue", ".svelte"};
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

std::optional<std::size_t> optional_size_option_from_value(const Value& options, const std::string& key) {
  if (!options.is_object() || !options.contains(key) || !options.at(key).is_number()) {
    return std::nullopt;
  }
  const auto raw = options.at(key).as_integer(0);
  return static_cast<std::size_t>(std::max<long long>(0, raw));
}

Value copy_object_or_empty(const Value& value) {
  return value.is_object() ? value : Value::object({});
}

Value string_array_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return Value(array);
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

std::vector<std::string> extract_sitemap_locations(const std::string& xml) {
  std::vector<std::string> locations;
  static const std::regex loc_pattern(R"(<\s*loc\s*>\s*([^<]+?)\s*<\s*/\s*loc\s*>)",
                                      std::regex::icase);
  for (auto it = std::sregex_iterator(xml.begin(), xml.end(), loc_pattern);
       it != std::sregex_iterator(); ++it) {
    std::string value = trim_copy((*it)[1].str());
    std::string decoded;
    for (std::size_t index = 0; index < value.size();) {
      if (value.compare(index, 5, "&amp;") == 0) {
        decoded.push_back('&');
        index += 5;
      } else {
        decoded.push_back(value[index++]);
      }
    }
    if (!decoded.empty()) {
      locations.push_back(std::move(decoded));
    }
  }
  return locations;
}

std::string best_page_content(const WebFetchedPage& page) {
  if (!page.markdown.empty()) {
    return page.markdown;
  }
  if (!page.text.empty()) {
    return page.text;
  }
  return html_to_text(page.html);
}

std::string best_page_url(const WebFetchedPage& page) {
  return page.final_url.empty() ? page.url : page.final_url;
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

std::string normalize_repository_path(std::filesystem::path path) {
  auto value = path.generic_string();
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  return value;
}

std::regex repository_glob_pattern_to_regex(const std::string& pattern) {
  const auto normalized = normalize_repository_path(pattern);
  std::string escaped;
  for (std::size_t index = 0; index < normalized.size(); ++index) {
    const char ch = normalized[index];
    if (ch == '*') {
      if (index + 1 < normalized.size() && normalized[index + 1] == '*') {
        escaped += ".*";
        ++index;
      } else {
        escaped += "[^/]*";
      }
      continue;
    }
    if (ch == '?') {
      escaped += ".";
      continue;
    }
    if (std::string(".+^${}()|[]\\").find(ch) != std::string::npos) {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return std::regex("^" + escaped + "$");
}

bool repository_path_matches_patterns(const std::string& path, const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return true;
  }
  const auto normalized = normalize_repository_path(path);
  return std::any_of(patterns.begin(), patterns.end(), [&](const auto& pattern) {
    return std::regex_match(normalized, repository_glob_pattern_to_regex(pattern));
  });
}

struct ParsedGitHubRepositoryUrl {
  std::string owner;
  std::string repo;
  std::string ref;
  std::string root_path;
};

std::vector<std::string> split_path_segments(const std::string& value) {
  std::vector<std::string> segments;
  std::stringstream stream(value);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (!segment.empty()) {
      segments.push_back(segment);
    }
  }
  return segments;
}

ParsedGitHubRepositoryUrl parse_github_repository_url(const std::string& source_url) {
  const std::string https_prefix = "https://github.com/";
  const std::string http_prefix = "http://github.com/";
  std::string path;
  if (source_url.rfind(std::string(https_prefix), 0) == 0) {
    path = source_url.substr(https_prefix.size());
  } else if (source_url.rfind(std::string(http_prefix), 0) == 0) {
    path = source_url.substr(http_prefix.size());
  } else {
    throw ConfigurationError("Unsupported GitHub repository URL: " + source_url);
  }
  if (const auto marker = path.find_first_of("?#"); marker != std::string::npos) {
    path = path.substr(0, marker);
  }
  const auto segments = split_path_segments(path);
  if (segments.size() < 2) {
    throw ConfigurationError("Unsupported GitHub repository URL: " + source_url);
  }
  ParsedGitHubRepositoryUrl parsed;
  parsed.owner = segments[0];
  parsed.repo = segments[1];
  if (parsed.repo.ends_with(".git")) {
    parsed.repo = parsed.repo.substr(0, parsed.repo.size() - 4);
  }
  if (segments.size() > 3 && segments[2] == "tree") {
    parsed.ref = segments[3];
    std::filesystem::path root;
    for (std::size_t index = 4; index < segments.size(); ++index) {
      root /= segments[index];
    }
    parsed.root_path = normalize_repository_path(root);
  }
  return parsed;
}

std::string github_fetch_text(const NativeWebPageFetcher& fetcher,
                              const std::string& url,
                              const Value& source,
                              const std::string& failure_label) {
  WebFetchRequest request{.url = url, .extract = "text"};
  const auto token = source.at("token").as_string();
  if (!token.empty()) {
    request.headers["authorization"] = "Bearer " + token;
  }
  const auto page = fetcher.fetch(request);
  if (page.status >= 400) {
    throw ConfigurationError(failure_label + " failed with status " + std::to_string(page.status) + ".");
  }
  return best_page_content(page);
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

bool CompactionBudget::empty() const noexcept {
  return !max_messages.has_value() && !max_tokens.has_value();
}

std::string to_string(SessionCompactionEvent::Kind kind) {
  switch (kind) {
    case SessionCompactionEvent::Kind::Started:
      return "Started";
    case SessionCompactionEvent::Kind::Phase1Completed:
      return "Phase1Completed";
    case SessionCompactionEvent::Kind::RefinementStarted:
      return "RefinementStarted";
    case SessionCompactionEvent::Kind::RefinementCompleted:
      return "RefinementCompleted";
    case SessionCompactionEvent::Kind::Failed:
      return "Failed";
  }
  return "Unknown";
}

SessionMemory::SessionMemory(SessionMemoryOptions options)
    : session_id_(std::move(options.storage.session_id)),
      compaction_budget_(std::move(options.compaction.compaction_budget)),
      summary_label_(std::move(options.storage.summary_label)),
      summary_(options.storage.summary),  // copy, baseline takes a copy below
      messages_(std::move(options.storage.messages)),
      // Two-state baseline: when restoring from a legacy snapshot the host
      // supplies only `summary` — treat it as the polished baseline and leave
      // the pending tail empty.
      polished_baseline_(std::move(options.storage.summary)),
      on_change_(std::move(options.on_change)),
      on_compaction_(std::move(options.on_compaction)),
      summarizer_(std::move(options.compaction.summarizer)),
      auto_compact_at_(options.compaction.auto_compact_at),
      token_budget_(options.compaction.token_budget),
      token_counter_(std::move(options.compaction.token_counter)),
      compact_on_append_(options.compaction.compact_on_append),
      summary_max_chars_(options.compaction.summary_max_chars),
      summarizer_mode_(options.compaction.summarizer_mode),
      background_executor_(std::move(options.compaction.background_executor)),
      truncate_oversized_message_(options.compaction.truncate_oversized_message),
      oversized_tail_chars_(options.compaction.oversized_tail_chars) {}

SessionMemory::~SessionMemory() {
  // Wait for any in-flight background refinement so its callback does not
  // outlive this object. await_refinements is safe to call on a quiescent
  // SessionMemory and is a no-op when nothing is pending.
  try {
    await_refinements();
  } catch (...) {
    // Destructors must not throw.
  }
}

Value session_memory_snapshot_to_value(const SessionMemorySnapshot& snapshot) {
  Value::Array messages;
  for (const auto& message : snapshot.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array overflow;
  for (const auto& message : snapshot.pending_refinement_overflow) {
    overflow.push_back(agent_message_to_value(message));
  }
  return Value::object({{"sessionId", snapshot.session_id},
                        {"summary", snapshot.summary},
                        {"messages", Value(messages)},
                        {"polishedBaseline", snapshot.polished_baseline},
                        {"pendingConcatTail", snapshot.pending_concat_tail},
                        {"pendingRefinementOverflow", Value(overflow)}});
}

Value retrieved_memory_to_value(const RetrievedMemory& memory) {
  return Value::object({{"id", memory.id},
                        {"content", memory.content},
                        {"score", memory.score},
                        {"metadata", memory.metadata},
                        {"namespace", memory.namespace_id}});
}

Value vector_memory_record_to_value(const VectorMemoryRecord& record) {
  return Value::object({{"id", record.id},
                        {"content", record.content},
                        {"embedding", embedding_to_value(record.embedding)},
                        {"metadata", record.metadata},
                        {"namespace", record.namespace_id},
                        {"createdAt", record.created_at},
                        {"updatedAt", record.updated_at}});
}

Value long_term_memory_context_result_to_value(const LongTermMemoryContextResult& result) {
  Value::Array hits;
  for (const auto& hit : result.hits) {
    hits.push_back(retrieved_memory_to_value(hit));
  }
  return Value::object({{"hits", Value(hits)},
                        {"message", result.message ? agent_message_to_value(*result.message) : Value()}});
}

namespace {

// Char-based token estimator shared by plan_compaction's fallback and
// SessionMemory::estimated_token_count_unlocked: one source of truth so
// CompactionBudget::max_tokens and should_auto_compact()'s threshold use
// the same unit when no SessionMemoryTokenCounter is configured.
std::size_t estimated_messages_tokens(std::span<const AgentMessage> messages) {
  std::size_t total_chars = 0;
  for (const auto& message : messages) {
    total_chars += extract_text_content(message.content).size();
  }
  return (total_chars + 3) / 4;  // rough char-based estimate
}

// Build the snapshot the trigger and the planner BOTH measure against:
// the summary projected as a virtual system message, followed by the
// configured suffix of messages. Single source of truth for "what does
// 'budget' mean" — fixes the counter-split and summary-asymmetry bugs.
std::vector<AgentMessage> build_budget_snapshot(
    std::span<const AgentMessage> messages,
    std::string_view summary,
    std::string_view summary_label) {
  std::vector<AgentMessage> snapshot;
  snapshot.reserve(messages.size() + 1);
  if (!summary.empty()) {
    snapshot.push_back(create_message(MessageRole::System,
                                      std::string(summary_label) + ":\n" + std::string(summary),
                                      Value::object({{"source", "session-memory"}})));
  }
  snapshot.insert(snapshot.end(), messages.begin(), messages.end());
  return snapshot;
}

// Compute remaining-token cost for a drop count, using the same ruler the
// trigger uses: the configured token_counter if set, otherwise the shared
// char/4 estimator. Both branches include the summary in the budget so
// that truncation and trigger agree on the unit.
std::size_t measure_remaining_tokens(std::span<const AgentMessage> messages,
                                     std::size_t drop,
                                     std::string_view summary,
                                     std::string_view summary_label,
                                     const SessionMemoryTokenCounter& token_counter) {
  const std::span<const AgentMessage> remaining = messages.subspan(drop);
  if (token_counter) {
    auto snapshot = build_budget_snapshot(remaining, summary, summary_label);
    return token_counter(snapshot);
  }
  const std::size_t summary_tokens = summary.empty() ? 0 : (summary.size() + 3) / 4;
  return summary_tokens + estimated_messages_tokens(remaining);
}

std::string normalize_session_file_extension(std::string extension) {
  if (extension.empty()) {
    return ".json";
  }
  if (extension.front() == '.') {
    return extension;
  }
  return "." + extension;
}

}  // namespace

CompactionPlan plan_compaction(const PlanCompactionInput& input) {
  CompactionPlan plan;
  if (input.budget.empty()) {
    return plan;  // caller decides whether empty budget is an error
  }
  // Compute how many oldest messages to drop so that ALL set caps hold
  // after the drop. Linear loop; messages_ rarely exceeds a few hundred.
  // Pure: never touches summarizer or any lock.
  std::size_t drop = 0;
  while (drop < input.messages.size()) {
    const std::size_t remaining_count = input.messages.size() - drop;
    if (input.budget.max_messages && remaining_count > *input.budget.max_messages) {
      ++drop;
      continue;
    }
    if (input.budget.max_tokens) {
      const std::size_t remaining_tokens =
          measure_remaining_tokens(input.messages, drop, input.summary,
                                   input.summary_label, input.token_counter);
      if (remaining_tokens > *input.budget.max_tokens) {
        // Single-message overflow handling: when only the tail remains and
        // it alone exceeds max_tokens, we cannot drop it without leaving
        // the conversation empty. If the policy opts in, emit a truncated
        // copy of the tail; otherwise stop and let the caller surface a
        // soft over-budget warning.
        if (remaining_count <= 1) {
          if (input.truncate_oversized_message && remaining_count == 1) {
            const AgentMessage& tail =
                *(input.messages.begin() + static_cast<std::ptrdiff_t>(drop));
            AgentMessage truncated = tail;
            // Replace the message's text payload with the trailing slice.
            const std::string original_text = extract_text_content(truncated.content);
            std::string new_text;
            if (input.oversized_tail_chars > 0 &&
                original_text.size() > input.oversized_tail_chars) {
              new_text = original_text.substr(original_text.size() -
                                              input.oversized_tail_chars);
            } else {
              new_text = original_text;
            }
            // Replace all content parts with a single text part carrying the
            // truncated payload; non-text parts (images, etc.) are dropped
            // because they're not text-measured and the cap is a text cap.
            truncated.content.clear();
            truncated.content.push_back(MessageContentPart{
                .type = ContentPartType::Text,
                .text = std::move(new_text),
            });
            plan.truncated_tail_message = std::move(truncated);
          }
          break;
        }
        ++drop;
        continue;
      }
    }
    break;  // all set caps satisfied
  }
  if (drop == 0 && !plan.truncated_tail_message) {
    return plan;
  }
  plan.drop_count = drop;
  if (drop > 0) {
    plan.overflow.assign(input.messages.begin(),
                         input.messages.begin() + static_cast<std::ptrdiff_t>(drop));
  }
  return plan;
}

const std::string& SessionMemory::session_id() const noexcept {
  return session_id_;
}

SessionMemorySnapshot SessionMemory::snapshot_unlocked() const {
  // We project polished/pending state into the snapshot so persistence
  // round-trips can resume an in-flight refinement. Reading the refinement
  // bookkeeping fields here without taking refinement_mutex_ is acceptable
  // because mutex_ is already held by the caller and Phase 2 commits to
  // pending_concat_tail_/polished_baseline_ are themselves serialized through
  // mutex_ in the write-back step (see run_refinement_body).
  return SessionMemorySnapshot{session_id_, summary_, messages_,
                               polished_baseline_, pending_concat_tail_,
                               pending_refinement_overflow_};
}

void SessionMemory::notify_change(const SessionMemorySnapshot& snapshot) const {
  if (on_change_) {
    on_change_(snapshot);
  }
}

void SessionMemory::notify_compaction(const SessionCompactionEvent& event) const {
  if (on_compaction_) {
    on_compaction_(event);
  }
}

namespace {

// Build the cheap concat tail line for a single overflow message, matching
// the deterministic summarize_messages format so post-refinement state and
// pre-refinement projections render identically.
std::string format_concat_line(const AgentMessage& message) {
  std::string line = "[" + message_role_label(message.role);
  if (!message.name.empty()) {
    line += ":" + message.name;
  }
  line += "] " + extract_text_content(message.content) + "\n";
  return line;
}

// Build the new polished baseline outside the mutex. Either runs the user-
// provided summarizer (which may block on an LLM call for seconds and may
// re-enter SessionMemory via reads) or falls back to the deterministic
// summarize_messages helper. Pure with respect to SessionMemory state.
std::string compute_new_polished_baseline(const SessionMemorySummarizer& summarizer,
                                          const std::string& session_id,
                                          std::string previous_baseline,
                                          const std::vector<AgentMessage>& overflow) {
  if (summarizer) {
    return summarizer(
        SessionMemorySummaryInput{session_id, std::move(previous_baseline), overflow});
  }
  std::string updated = std::move(previous_baseline);
  if (!updated.empty()) {
    updated += "\n";
  }
  updated += summarize_messages(overflow);
  return updated;
}

// Apply summary_max_chars cap to a denormalized join, preferring to keep
// the trailing slice (the most recent context).
std::string apply_summary_cap(std::string value, std::size_t cap) {
  if (cap > 0 && value.size() > cap) {
    return value.substr(value.size() - cap);
  }
  return value;
}

}  // namespace

void SessionMemory::recompute_summary_unlocked() {
  std::string joined = polished_baseline_;
  if (!pending_concat_tail_.empty()) {
    if (!joined.empty() && joined.back() != '\n') {
      joined += "\n";
    }
    joined += pending_concat_tail_;
  }
  summary_ = apply_summary_cap(std::move(joined), summary_max_chars_);
}

// ---------------------------------------------------------------------------
// Phase 1 + Phase 2 dispatch + Phase 2 body.
// ---------------------------------------------------------------------------
//
// Phase 1 (run_phase1_locked):
//   - Caller holds compaction_pipeline_mutex_.
//   - Acquire mutex_, compute plan_compaction, erase oldest messages, apply
//     truncated_tail_message when present, append concat lines to
//     pending_concat_tail_, append AgentMessages to pending_refinement_overflow_,
//     recompute denormalized summary_.
//   - Fires on_change once for the Phase 1 mutation.
//
// Phase 2 dispatch (dispatch_refinement):
//   - Synchronous mode: call run_refinement_body() on the current thread.
//   - Background mode: under refinement_mutex_, if no in-flight refinement
//     exists, set refinement_in_flight_ = true and submit run_refinement_body
//     to the executor (or std::thread fallback). Otherwise just set
//     refinement_pending_ = true and return — the in-flight body will pick
//     up the accumulated tail/overflow on its trailing dispatch.
//
// Phase 2 body (run_refinement_body):
//   - Snapshot polished_baseline_ + pending_refinement_overflow_ + session_id
//     under mutex_; the snapshot becomes the summarizer input.
//   - Fire RefinementStarted.
//   - Run summarizer outside any lock. On exception → Failed event, clear
//     refinement_in_flight_ + refinement_pending_, notify_all, return without
//     mutating polished_baseline_ / pending_*.
//   - On success: under mutex_, commit polished_baseline_ = result, clear the
//     consumed prefix of pending_concat_tail_ + pending_refinement_overflow_,
//     recompute summary_, capture snapshot for on_change + RefinementCompleted.
//   - Under refinement_mutex_: clear in_flight; if pending was set during the
//     refinement, set in_flight = true and dispatch trailing refinement.
//     Otherwise notify_all (await_refinements consumers).

SessionMemory& SessionMemory::add(AgentMessage message) {
  bool should_compact = false;
  SessionMemorySnapshot changed;

  // Append phase under mutex_ only. compaction_pipeline_mutex_ NOT held —
  // the only contract on append is that it does not race with itself.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
    should_compact = compact_on_append_ && !compaction_budget_.empty();
    changed = snapshot_unlocked();
  }

  notify_change(changed);

  if (!should_compact) {
    return *this;
  }
  // Fall through to the same pipeline used by explicit compact(). This
  // unifies Phase 1 + Phase 2 dispatch semantics across append-driven and
  // explicit paths.
  compact();
  return *this;
}

SessionMemory& SessionMemory::add(const Value& message) {
  return add(agent_message_from_value(message));
}

SessionMemory& SessionMemory::add_many(const std::vector<AgentMessage>& messages) {
  bool should_compact = false;
  SessionMemorySnapshot changed;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.insert(messages_.end(), messages.begin(), messages.end());
    should_compact = compact_on_append_ && !compaction_budget_.empty();
    changed = snapshot_unlocked();
  }

  notify_change(changed);

  if (!should_compact) {
    return *this;
  }
  compact();
  return *this;
}

SessionMemory& SessionMemory::add_many(const std::vector<Value>& messages) {
  std::vector<AgentMessage> normalized;
  normalized.reserve(messages.size());
  for (const auto& message : messages) {
    normalized.push_back(agent_message_from_value(message));
  }
  return add_many(normalized);
}

void SessionMemory::compact() {
  // Hold compaction_pipeline_mutex_ across Phase 1 so two callers cannot
  // erase the message vector concurrently. In Synchronous mode it is also
  // held across Phase 2 (preserving the historical "one summarizer at a
  // time" property exercised by the R1 race repro). In Background mode the
  // dispatcher exits immediately, releasing the pipeline lock; the in-flight
  // Phase 2 is then serialized purely by refinement_in_flight_.
  std::unique_lock<std::mutex> pipeline_lock(compaction_pipeline_mutex_);

  CompactionPlan plan;
  std::size_t pre_token_count = 0;
  std::string snapshot_session_id;
  bool single_message_truncated = false;
  std::string warning;
  const bool phase1_did_work = run_phase1_locked(plan, pre_token_count,
                                                  snapshot_session_id,
                                                  single_message_truncated,
                                                  warning);

  // Even when Phase 1 is a no-op, we may still need to dispatch a
  // refinement: the previous attempt may have failed and left pending
  // overflow behind. This is the explicit "host retries after Failed" path
  // documented in the brief.
  bool has_pending_overflow = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_overflow = !pending_refinement_overflow_.empty();
    if (snapshot_session_id.empty()) snapshot_session_id = session_id_;
  }
  if (!phase1_did_work && !has_pending_overflow) {
    return;  // truly nothing to do
  }

  if (phase1_did_work) {
    // Phase1Completed event — messages_ is bounded; summary_ contains the
    // updated denormalization including any new concat-tail lines.
    SessionCompactionEvent phase1;
    phase1.kind = SessionCompactionEvent::Kind::Phase1Completed;
    phase1.session_id = snapshot_session_id;
    phase1.dropped_count = plan.drop_count;
    phase1.pre_token_count = pre_token_count;
    phase1.single_message_truncated = single_message_truncated;
    phase1.warning = warning;
    notify_compaction(phase1);
  }

  // In Synchronous mode, fall through to Phase 2 on the calling thread
  // while still holding the pipeline lock — Synchronous mode === historical
  // "one compact in flight" semantics.
  if (summarizer_mode_ == SummarizerMode::Synchronous) {
    {
      std::lock_guard<std::mutex> rlock(refinement_mutex_);
      refinement_in_flight_ = true;
    }
    run_refinement_body();
    return;
  }

  // Background mode: release pipeline lock and hand Phase 2 to the executor.
  pipeline_lock.unlock();
  dispatch_refinement();
}

bool SessionMemory::run_phase1_locked(CompactionPlan& plan,
                                      std::size_t& pre_token_count,
                                      std::string& snapshot_session_id,
                                      bool& single_message_truncated_out,
                                      std::string& warning_out) {
  bool budget_empty = false;
  std::string snapshot_session_id_for_fail;
  SessionCompactionListener listener_copy_for_fail;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (compaction_budget_.empty()) {
      budget_empty = true;
      snapshot_session_id_for_fail = session_id_;
      listener_copy_for_fail = on_compaction_;
    } else {
      pre_token_count = estimated_token_count_unlocked();
    }
  }
  if (budget_empty) {
    SessionCompactionEvent failed_event;
    failed_event.kind = SessionCompactionEvent::Kind::Failed;
    failed_event.session_id = snapshot_session_id_for_fail;
    failed_event.error =
        "SessionMemory compaction triggered but no CompactionBudget set "
        "(neither max_messages nor max_tokens). Set at least one cap on "
        "SessionMemoryOptions::compaction.compaction_budget.";
    if (listener_copy_for_fail) listener_copy_for_fail(failed_event);
    throw ConfigurationError(
        failed_event.error,
        "{\"code\":\"no_compaction_budget\",\"field\":\"compaction.compaction_budget\"}");
  }

  // Plan first (under mutex_) to discover whether there is real work. Then
  // emit Started outside the lock (so listeners that re-enter the session
  // do not self-deadlock), then re-acquire mutex_ to apply the mutation.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    plan = plan_compaction(PlanCompactionInput{
        .messages = std::span<const AgentMessage>(messages_),
        .summary = summary_,
        .summary_label = summary_label_,
        .budget = compaction_budget_,
        .token_counter = token_counter_,
        .truncate_oversized_message = truncate_oversized_message_,
        .oversized_tail_chars = oversized_tail_chars_,
    });
    snapshot_session_id = session_id_;
  }
  // Detect single-message soft-over-budget BEFORE early-returning. Even
  // when the planner is a no-op (single message, no truncate opt-in), we
  // still want to surface the warning so observability hosts can react.
  bool soft_over_budget = false;
  if (plan.drop_count == 0 && !plan.truncated_tail_message) {
    if (!truncate_oversized_message_ && compaction_budget_.max_tokens) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (messages_.size() == 1) {
        const std::size_t lone_tokens = measure_remaining_tokens(
            std::span<const AgentMessage>(messages_), /*drop=*/0, summary_,
            summary_label_, token_counter_);
        if (lone_tokens > *compaction_budget_.max_tokens) {
          soft_over_budget = true;
        }
      }
    }
    if (!soft_over_budget) {
      return false;  // truly no work
    }
    // Soft over-budget: emit a Phase1Completed-like event so the host sees
    // the warning. We do not fire Started/RefinementStarted because no
    // actual mutation or summarizer call happens — just a configuration
    // hint. Return false to short-circuit Phase 2 dispatch in compact().
    SessionCompactionEvent warn;
    warn.kind = SessionCompactionEvent::Kind::Phase1Completed;
    warn.session_id = snapshot_session_id;
    warn.warning =
        "single message exceeds max_tokens; enable "
        "SessionCompactionPolicy.truncate_oversized_message to opt in to "
        "tail truncation";
    notify_compaction(warn);
    warning_out = warn.warning;
    return false;
  }

  // Started fires once per compact() invocation that has real Phase 1 work.
  {
    SessionCompactionEvent started;
    started.kind = SessionCompactionEvent::Kind::Started;
    started.session_id = snapshot_session_id;
    started.pre_token_count = pre_token_count;
    started.dropped_count = plan.drop_count;
    notify_compaction(started);
  }

  // Phase 1 mutation critical section.
  SessionMemorySnapshot changed;
  bool had_work = false;
  bool truncated = false;
  bool over_budget_no_truncate = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (plan.drop_count > 0 && plan.drop_count <= messages_.size()) {
      // Append concat-tail lines BEFORE erasing so we don't have to hold a
      // reference into the moved-from prefix.
      for (const auto& msg : plan.overflow) {
        pending_concat_tail_ += format_concat_line(msg);
      }
      // Accumulate raw overflow for the eventual summarizer call.
      pending_refinement_overflow_.insert(pending_refinement_overflow_.end(),
                                          plan.overflow.begin(),
                                          plan.overflow.end());
      messages_.erase(messages_.begin(),
                      messages_.begin() + static_cast<std::ptrdiff_t>(plan.drop_count));
      had_work = true;
    }
    if (plan.truncated_tail_message) {
      // Replace the still-too-large tail with its truncated copy. Surface
      // the post-truncation text on the concat tail so the summary reflects
      // the new state of the conversation.
      if (!messages_.empty()) {
        messages_.back() = *plan.truncated_tail_message;
      }
      pending_concat_tail_ += format_concat_line(*plan.truncated_tail_message);
      truncated = true;
      had_work = true;
    }
    // Detect single-message soft-over-budget condition (no truncate opt-in):
    // post-plan, remaining count == 1 and that one message still exceeds the
    // token cap.
    if (!truncated && compaction_budget_.max_tokens && messages_.size() == 1) {
      const std::size_t lone_tokens = measure_remaining_tokens(
          std::span<const AgentMessage>(messages_), /*drop=*/0, summary_,
          summary_label_, token_counter_);
      if (lone_tokens > *compaction_budget_.max_tokens) {
        over_budget_no_truncate = true;
      }
    }

    if (had_work) {
      recompute_summary_unlocked();
      changed = snapshot_unlocked();
    }
  }

  single_message_truncated_out = truncated;
  if (over_budget_no_truncate) {
    warning_out =
        "single message exceeds max_tokens; enable "
        "SessionCompactionPolicy.truncate_oversized_message to opt in to "
        "tail truncation";
  } else {
    warning_out.clear();
  }

  if (had_work) {
    notify_change(changed);
  }
  return had_work;
}

void SessionMemory::dispatch_refinement() {
  std::function<void()> body;
  {
    std::lock_guard<std::mutex> rlock(refinement_mutex_);
    if (refinement_in_flight_) {
      // Mark trailing dispatch — the in-flight body will requeue itself
      // when it finishes.
      refinement_pending_ = true;
      return;
    }
    refinement_in_flight_ = true;
  }

  body = [this]() { this->run_refinement_body(); };

  if (background_executor_) {
    try {
      background_executor_(body);
    } catch (...) {
      // Executor refused the work. Reset bookkeeping and rethrow into a
      // detached failure event so we don't leave refinement_in_flight_ stuck.
      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        refinement_in_flight_ = false;
        refinement_pending_ = false;
      }
      refinement_cv_.notify_all();
      SessionCompactionEvent failed;
      failed.kind = SessionCompactionEvent::Kind::Failed;
      {
        std::lock_guard<std::mutex> dlock(mutex_);
        failed.session_id = session_id_;
      }
      failed.error = "background_executor rejected refinement work";
      notify_compaction(failed);
      throw;
    }
  } else {
    // Default fallback: detached std::thread.
    std::thread(body).detach();
  }
}

void SessionMemory::run_refinement_body() {
  for (;;) {
    // Snapshot the inputs under mutex_.
    SessionMemorySummarizer summarizer_copy;
    std::string previous_baseline;
    std::vector<AgentMessage> overflow_snapshot;
    std::string snapshot_session_id;
    std::size_t consumed_tail_size = 0;
    std::size_t consumed_overflow_size = 0;
    std::size_t pre_token_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      summarizer_copy = summarizer_;
      previous_baseline = polished_baseline_;
      overflow_snapshot = pending_refinement_overflow_;
      snapshot_session_id = session_id_;
      consumed_tail_size = pending_concat_tail_.size();
      consumed_overflow_size = pending_refinement_overflow_.size();
      pre_token_count = estimated_token_count_unlocked();
    }

    // If nothing to refine, exit the loop cleanly without firing events.
    if (overflow_snapshot.empty()) {
      // Clear in-flight / pending and notify waiters.
      bool requeue = false;
      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        if (refinement_pending_) {
          // Pending without overflow shouldn't really happen, but treat it
          // as already-satisfied: clear and exit.
          refinement_pending_ = false;
        }
        refinement_in_flight_ = false;
        (void)requeue;
      }
      refinement_cv_.notify_all();
      return;
    }

    // RefinementStarted event.
    {
      SessionCompactionEvent started;
      started.kind = SessionCompactionEvent::Kind::RefinementStarted;
      started.session_id = snapshot_session_id;
      started.dropped_count = overflow_snapshot.size();
      started.pre_token_count = pre_token_count;
      notify_compaction(started);
    }

    const auto summarizer_start = std::chrono::steady_clock::now();
    std::string new_baseline;
    bool failed = false;
    std::string failure_msg;
    try {
      new_baseline = compute_new_polished_baseline(
          summarizer_copy, snapshot_session_id, std::move(previous_baseline),
          overflow_snapshot);
    } catch (const std::exception& ex) {
      failed = true;
      failure_msg = ex.what();
    } catch (...) {
      failed = true;
      failure_msg = "unknown summarizer exception";
    }
    const auto summarizer_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - summarizer_start);

    if (failed) {
      // Preserve pending_* state so the host can retry by triggering another
      // compact(). Clear both refinement_in_flight_ and refinement_pending_
      // — refinement_pending_ is a 1-bit dispatch hint, NOT a retry queue.
      SessionCompactionEvent failed_event;
      failed_event.kind = SessionCompactionEvent::Kind::Failed;
      failed_event.session_id = snapshot_session_id;
      failed_event.dropped_count = overflow_snapshot.size();
      failed_event.pre_token_count = pre_token_count;
      failed_event.summarizer_duration = summarizer_duration;
      failed_event.error = failure_msg;
      notify_compaction(failed_event);

      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        refinement_in_flight_ = false;
        refinement_pending_ = false;
      }
      refinement_cv_.notify_all();
      return;
    }

    // Commit phase. We only consume up to `consumed_*` because callers may
    // have appended more to pending_concat_tail_ / pending_refinement_overflow_
    // while the summarizer was running.
    SessionMemorySnapshot changed;
    std::size_t post_token_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      polished_baseline_ = std::move(new_baseline);
      if (consumed_overflow_size <= pending_refinement_overflow_.size()) {
        pending_refinement_overflow_.erase(
            pending_refinement_overflow_.begin(),
            pending_refinement_overflow_.begin() +
                static_cast<std::ptrdiff_t>(consumed_overflow_size));
      } else {
        pending_refinement_overflow_.clear();
      }
      if (consumed_tail_size <= pending_concat_tail_.size()) {
        pending_concat_tail_.erase(0, consumed_tail_size);
      } else {
        pending_concat_tail_.clear();
      }
      recompute_summary_unlocked();
      changed = snapshot_unlocked();
      post_token_count = estimated_token_count_unlocked();
    }
    notify_change(changed);

    SessionCompactionEvent completed;
    completed.kind = SessionCompactionEvent::Kind::RefinementCompleted;
    completed.session_id = snapshot_session_id;
    completed.dropped_count = overflow_snapshot.size();
    completed.pre_token_count = pre_token_count;
    completed.post_token_count = post_token_count;
    completed.summarizer_duration = summarizer_duration;
    notify_compaction(completed);

    // Decide whether to do a trailing refinement. Read overflow size under
    // mutex_ (the field that owns it) THEN take the dispatch decision under
    // refinement_mutex_. The two locks are taken in order data → refinement
    // here, which is the only allowed ordering — but we release mutex_
    // immediately, so this does not violate the "refinement_mutex_ never
    // held with data mutex" invariant for the rest of the codebase.
    bool overflow_empty = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      overflow_empty = pending_refinement_overflow_.empty();
    }
    bool exit_loop = false;
    {
      std::lock_guard<std::mutex> rlock(refinement_mutex_);
      if (refinement_pending_ && !overflow_empty) {
        refinement_pending_ = false;
        // refinement_in_flight_ stays true — we continue the loop.
      } else {
        refinement_pending_ = false;
        refinement_in_flight_ = false;
        exit_loop = true;
      }
    }
    if (exit_loop) {
      refinement_cv_.notify_all();
      return;
    }
    // else continue the loop — trailing refinement swallows accumulated tail.
  }
}

void SessionMemory::await_refinements() const {
  std::unique_lock<std::mutex> lock(refinement_mutex_);
  refinement_cv_.wait(
      lock, [this] { return !refinement_in_flight_ && !refinement_pending_; });
}

std::size_t SessionMemory::estimated_token_count_unlocked() const {
  // Delegate to the shared measurement helper so the trigger
  // (should_auto_compact) and the planner (plan_compaction) use exactly
  // the same ruler — fixes the counter-split (Bug 1) and summary-
  // asymmetry (Bug 3) regressions.
  return measure_remaining_tokens(messages_, /*drop=*/0, summary_, summary_label_,
                                  token_counter_);
}

std::size_t SessionMemory::estimated_token_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return estimated_token_count_unlocked();
}

std::size_t SessionMemory::token_budget() const noexcept {
  return token_budget_;
}

bool SessionMemory::should_auto_compact() const noexcept {
  if (token_budget_ == 0) {
    return false;
  }
  if (auto_compact_at_ <= 0.0) {
    return false;
  }
  std::size_t tokens = 0;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens = estimated_token_count_unlocked();
  } catch (...) {
    return false;
  }
  const double threshold = auto_compact_at_ * static_cast<double>(token_budget_);
  return static_cast<double>(tokens) >= threshold;
}

std::vector<AgentMessage> SessionMemory::get_messages(bool include_summary) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentMessage> messages;
  if (include_summary && !summary_.empty()) {
    messages.push_back(create_message(MessageRole::System, summary_label_ + ":\n" + summary_,
                                      Value::object({{"source", "session-memory"}})));
  }
  messages.insert(messages.end(), messages_.begin(), messages_.end());
  return messages;
}

std::vector<AgentMessage> SessionMemory::get_messages(const SessionMemoryGetMessagesOptions& options) const {
  return get_messages(options.include_summary);
}

SessionMemorySnapshot SessionMemory::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_unlocked();
}

SessionMemory& SessionMemory::restore(const SessionMemorySnapshot& snapshot) {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Two-state restore: prefer the explicitly persisted polished + pending
    // fields when present. Fall back to the legacy single `summary` field
    // (treat it as the polished baseline with no pending work) so older
    // session files keep loading.
    messages_ = snapshot.messages;
    if (!snapshot.polished_baseline.empty() ||
        !snapshot.pending_concat_tail.empty() ||
        !snapshot.pending_refinement_overflow.empty()) {
      polished_baseline_ = snapshot.polished_baseline;
      pending_concat_tail_ = snapshot.pending_concat_tail;
      pending_refinement_overflow_ = snapshot.pending_refinement_overflow;
    } else {
      polished_baseline_ = snapshot.summary;
      pending_concat_tail_.clear();
      pending_refinement_overflow_.clear();
    }
    recompute_summary_unlocked();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
  return *this;
}

SessionMemory& SessionMemory::restore(const SessionMemoryRestoreInput& snapshot) {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_ = snapshot.messages.value_or(std::vector<AgentMessage>{});
    const bool has_two_state =
        snapshot.polished_baseline.has_value() ||
        snapshot.pending_concat_tail.has_value() ||
        snapshot.pending_refinement_overflow.has_value();
    if (has_two_state) {
      polished_baseline_ = snapshot.polished_baseline.value_or("");
      pending_concat_tail_ = snapshot.pending_concat_tail.value_or("");
      pending_refinement_overflow_ =
          snapshot.pending_refinement_overflow.value_or(std::vector<AgentMessage>{});
    } else {
      polished_baseline_ = snapshot.summary.value_or("");
      pending_concat_tail_.clear();
      pending_refinement_overflow_.clear();
    }
    recompute_summary_unlocked();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
  return *this;
}

void SessionMemory::clear() {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    summary_.clear();
    messages_.clear();
    polished_baseline_.clear();
    pending_concat_tail_.clear();
    pending_refinement_overflow_.clear();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
}

InMemorySessionStore::InMemorySessionStore(SessionMemoryOptions session_options)
    : session_options_(std::move(session_options)) {}

InMemorySessionStore::InMemorySessionStore(InMemorySessionStoreConfig config)
    : InMemorySessionStore(std::move(config.session_options)) {}

std::shared_ptr<SessionMemory> InMemorySessionStore::get(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = session_id.empty() ? "default" : session_id;
  auto found = sessions_.find(id);
  if (found != sessions_.end()) {
    return found->second;
  }
  auto options = session_options_;
  options.storage.session_id = id;
  auto session = std::make_shared<SessionMemory>(std::move(options));
  sessions_[id] = session;
  return session;
}

void InMemorySessionStore::clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    sessions_.clear();
    return;
  }
  sessions_.erase(session_id);
}

std::vector<std::string> InMemorySessionStore::list_session_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(sessions_.size());
  for (const auto& [id, _] : sessions_) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, std::string file_extension,
                                   CompactionBudget compaction_budget)
    : base_dir_(std::move(base_dir)),
      file_extension_(normalize_session_file_extension(std::move(file_extension))),
      session_options_() {
  session_options_.compaction.compaction_budget = std::move(compaction_budget);
}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, SessionMemoryOptions session_options)
    : FileSessionStore(std::move(base_dir), ".json", std::move(session_options)) {}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, std::string file_extension,
                                   SessionMemoryOptions session_options)
    : base_dir_(std::move(base_dir)),
      file_extension_(normalize_session_file_extension(std::move(file_extension))),
      session_options_(std::move(session_options)) {}

FileSessionStore::FileSessionStore(FileSessionStoreConfig config)
    : FileSessionStore(std::move(config.base_dir),
                       std::move(config.file_extension),
                       std::move(config.session_options)) {}

FileSessionStore::~FileSessionStore() {
  flush();
}

void write_session_snapshot_to_file(const std::filesystem::path& path,
                                    const SessionMemorySnapshot& snapshot);

std::shared_ptr<SessionMemory> FileSessionStore::get(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = session_id.empty() ? "default" : session_id;
  if (sessions_.contains(id)) {
    return sessions_.at(id);
  }
  const auto snapshot = read_snapshot(id);
  const auto path = file_path(id);
  auto options = session_options_;
  options.storage.session_id = id;
  if (snapshot) {
    // Construct the SessionMemory empty-of-storage, then restore so the
    // two-state pending fields are populated. (Storage::summary maps only
    // to the legacy single-string baseline; the polished + pending fields
    // are not part of SessionStorage.)
    options.storage.summary.clear();
    options.storage.messages.clear();
  }
  options.on_change = [path](const SessionMemorySnapshot& changed) {
    write_session_snapshot_to_file(path, changed);
  };
  auto session = std::make_shared<SessionMemory>(std::move(options));
  if (snapshot) {
    session->restore(*snapshot);
  }
  sessions_[id] = session;
  return session;
}

void FileSessionStore::clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    sessions_.clear();
    std::filesystem::remove_all(base_dir_);
    return;
  }
  sessions_.erase(session_id);
  std::filesystem::remove(file_path(session_id));
}

void FileSessionStore::flush(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    for (const auto& [_, session] : sessions_) {
      write_snapshot(session->snapshot());
    }
    return;
  }
  const auto found = sessions_.find(session_id);
  if (found != sessions_.end()) {
    write_snapshot(found->second->snapshot());
  }
}

std::vector<std::string> FileSessionStore::list_session_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<std::string> ids;
  for (const auto& [id, _] : sessions_) {
    ids.insert(id);
  }
  if (std::filesystem::exists(base_dir_)) {
    for (const auto& entry : std::filesystem::directory_iterator(base_dir_)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto filename = entry.path().filename().string();
      if (filename.size() < file_extension_.size()) {
        continue;
      }
      if (filename.compare(filename.size() - file_extension_.size(), file_extension_.size(), file_extension_) == 0) {
        const auto decoded = decode_uri_component(filename.substr(0, filename.size() - file_extension_.size()));
        if (decoded) {
          ids.insert(*decoded);
        }
      }
    }
  }
  return std::vector<std::string>(ids.begin(), ids.end());
}

std::filesystem::path FileSessionStore::file_path(const std::string& session_id) const {
  return base_dir_ / (encode_uri_component(session_id) + file_extension_);
}

std::optional<SessionMemorySnapshot> FileSessionStore::read_snapshot(const std::string& session_id) const {
  const auto path = file_path(session_id);
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  const auto raw = read_json_file(path);
  SessionMemorySnapshot snapshot;
  snapshot.session_id = raw.at("sessionId").as_string(session_id);
  snapshot.summary = raw.at("summary").as_string();
  for (const auto& item : raw.at("messages").as_array()) {
    snapshot.messages.push_back(agent_message_from_value(item));
  }
  // Two-state fields are OPTIONAL — legacy snapshots written before the
  // background-summarizer upgrade do not have them. as_string() / as_array()
  // on a missing key returns the default-constructed value so reads stay
  // forward/backward compatible.
  snapshot.polished_baseline = raw.at("polishedBaseline").as_string();
  snapshot.pending_concat_tail = raw.at("pendingConcatTail").as_string();
  for (const auto& item : raw.at("pendingRefinementOverflow").as_array()) {
    snapshot.pending_refinement_overflow.push_back(agent_message_from_value(item));
  }
  return snapshot;
}

void write_session_snapshot_to_file(const std::filesystem::path& path,
                                    const SessionMemorySnapshot& snapshot) {
  Value::Array messages;
  for (const auto& message : snapshot.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array overflow;
  for (const auto& message : snapshot.pending_refinement_overflow) {
    overflow.push_back(agent_message_to_value(message));
  }
  write_json_file(path,
                  Value::object({{"sessionId", snapshot.session_id},
                                 {"summary", snapshot.summary},
                                 {"messages", Value(messages)},
                                 {"polishedBaseline", snapshot.polished_baseline},
                                 {"pendingConcatTail", snapshot.pending_concat_tail},
                                 {"pendingRefinementOverflow", Value(overflow)}}));
}

void FileSessionStore::write_snapshot(const SessionMemorySnapshot& snapshot) const {
  write_session_snapshot_to_file(file_path(snapshot.session_id), snapshot);
}

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

bool TextKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "text";
}

std::vector<LoadedKnowledgeDocument> TextKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto text = source.at("text").as_string();
  return {build_loaded_document("text",
                                source.at("uri").as_string("text://" + generate_uuid()),
                                source.at("title").as_string("Inline text"),
                                text,
                                copy_object_or_empty(source.at("metadata")))};
}

bool FileKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "file";
}

std::vector<LoadedKnowledgeDocument> FileKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("File knowledge source requires path.");
  }

  const auto path = std::filesystem::absolute(std::filesystem::path(raw_path));
  const auto extension = extension_for_path(path);
  auto metadata = copy_object_or_empty(source.at("metadata"));
  const auto title = source.at("title").as_string(path.filename().string());

  if (knowledge_image_extensions().contains(extension)) {
    const std::string alt_text = metadata_text(metadata, "altText");
    const std::string ocr_text = metadata_text(metadata, "ocrText");
    const std::string caption = metadata_text(metadata, "caption");
    std::string text_hint = metadata_text(metadata, "textHint");
    if (text_hint.empty()) text_hint = ocr_text;
    if (text_hint.empty()) text_hint = alt_text;
    if (text_hint.empty()) text_hint = caption;
    MediaSource media;
    media.kind = MediaSourceKind::Path;
    media.path = path.string();
    media.mime_type = DefaultMediaResolver::extension_to_mime(path);
    media.filename = path.filename().string();
    metadata["mediaKind"] = "path";
    metadata["path"] = path.string();
    return {build_loaded_document("file", path.string(), title, text_hint, std::move(metadata),
                                  KnowledgeAssetType::Image, media, text_hint, alt_text, ocr_text, caption)};
  }

  const auto bytes = read_binary_file(path);
  if (extension == ".pdf") {
    metadata["pdfExtractor"] = "native";
    return {build_loaded_document("file", path.string(), title, extract_pdf_text_from_bytes(bytes), std::move(metadata))};
  }

  std::string content = bytes_to_text(bytes);
  if (extension == ".html" || extension == ".htm") {
    const auto parsed_title = extract_html_title(content);
    return {build_loaded_document("file", path.string(),
                                  source.at("title").as_string(parsed_title.empty() ? title : parsed_title),
                                  html_to_text(content),
                                  std::move(metadata))};
  }

  return {build_loaded_document("file", path.string(), title, std::move(content), std::move(metadata))};
}

DirectoryKnowledgeSourceLoader::DirectoryKnowledgeSourceLoader(std::shared_ptr<FileKnowledgeSourceLoader> file_loader)
    : file_loader_(std::move(file_loader)) {
  if (!file_loader_) {
    file_loader_ = std::make_shared<FileKnowledgeSourceLoader>();
  }
}

bool DirectoryKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "directory";
}

std::vector<LoadedKnowledgeDocument> DirectoryKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Directory knowledge source requires path.");
  }

  const auto directory = std::filesystem::absolute(std::filesystem::path(raw_path));
  if (!std::filesystem::is_directory(directory)) {
    throw ConfigurationError("Path is not a directory: " + directory.string());
  }

  const bool recursive = !source.contains("recursive") || source.at("recursive").as_bool(true);
  const auto include_extensions = string_set_from_value(source.at("includeExtensions"),
                                                        default_knowledge_text_extensions());
  const auto exclude_extensions = string_set_from_value(source.at("excludeExtensions"));
  const auto exclude_directories = string_set_preserve_from_value(source.at("excludeDirectories"),
                                                                  default_knowledge_exclude_directories());

  std::vector<std::filesystem::path> files;
  const auto should_include = [&](const std::filesystem::path& path) {
    const auto extension = extension_for_path(path);
    if (!include_extensions.empty() && !include_extensions.contains(extension)) {
      return false;
    }
    if (exclude_extensions.contains(extension)) {
      return false;
    }
    return true;
  };

  if (recursive) {
    std::filesystem::recursive_directory_iterator it(directory);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
      if (it->is_directory() && exclude_directories.contains(it->path().filename().string())) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file() && should_include(it->path())) {
        files.push_back(std::filesystem::absolute(it->path()));
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && should_include(entry.path())) {
        files.push_back(std::filesystem::absolute(entry.path()));
      }
    }
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& file : files) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["sourceKind"] = "directory";
    auto loaded = file_loader_->load(Value::object({{"type", "file"},
                                                   {"path", file.string()},
                                                   {"metadata", metadata}}));
    for (auto& document : loaded) {
      document.source_type = "directory";
      documents.push_back(std::move(document));
    }
  }
  return documents;
}

MarkdownKnowledgeSourceLoader::MarkdownKnowledgeSourceLoader(bool recursive)
    : recursive_(recursive) {}

bool MarkdownKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "markdown";
}

std::vector<LoadedKnowledgeDocument> MarkdownKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Markdown knowledge source requires path.");
  }

  const auto path = std::filesystem::absolute(std::filesystem::path(raw_path));
  const bool recursive = source.contains("recursive") ? source.at("recursive").as_bool(recursive_) : recursive_;
  std::vector<std::filesystem::path> files;
  const auto include_markdown = [](const std::filesystem::path& candidate) {
    return markdown_knowledge_extensions().contains(extension_for_path(candidate));
  };

  if (std::filesystem::is_regular_file(path)) {
    if (include_markdown(path)) {
      files.push_back(path);
    }
  } else if (std::filesystem::is_directory(path)) {
    if (recursive) {
      std::filesystem::recursive_directory_iterator it(path);
      const std::filesystem::recursive_directory_iterator end;
      for (; it != end; ++it) {
        if (it->is_regular_file() && include_markdown(it->path())) {
          files.push_back(std::filesystem::absolute(it->path()));
        }
      }
    } else {
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file() && include_markdown(entry.path())) {
          files.push_back(std::filesystem::absolute(entry.path()));
        }
      }
    }
  } else {
    throw ConfigurationError("Markdown knowledge source path does not exist: " + path.string());
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  documents.reserve(files.size());
  for (const auto& file : files) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["loader"] = "markdown";
    const auto content = bytes_to_text(read_binary_file(file));
    documents.push_back(build_loaded_document("markdown",
                                              file.string(),
                                              find_markdown_title(content, file.filename().string()),
                                              content,
                                              std::move(metadata)));
  }
  return documents;
}

RepositoryKnowledgeSourceLoader::RepositoryKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher,
                                                                 std::set<std::string> extensions,
                                                                 std::set<std::string> exclude_directories)
    : fetcher_(fetcher),
      extensions_(std::move(extensions)),
      exclude_directories_(std::move(exclude_directories)) {}

void RepositoryKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool RepositoryKnowledgeSourceLoader::supports(const Value& source) const {
  const auto type = source.at("type").as_string();
  return type == "repository" || type == "github";
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load(const Value& source) const {
  const auto type = source.at("type").as_string();
  if (type == "repository") {
    return load_local_repository(source);
  }
  if (type == "github") {
    return load_github_repository(source);
  }
  return {};
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load_local_repository(const Value& source) const {
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Repository knowledge source requires path.");
  }
  const auto repository_root = std::filesystem::absolute(std::filesystem::path(raw_path)).lexically_normal();
  if (!std::filesystem::is_directory(repository_root)) {
    throw ConfigurationError("Path is not a repository directory: " + repository_root.string());
  }

  const auto include_extensions = string_set_from_value(source.at("extensions"),
                                                        extensions_.empty() ? default_repository_extensions()
                                                                            : extensions_);
  std::set<std::string> exclude_directories = default_knowledge_exclude_directories();
  exclude_directories.insert(exclude_directories_.begin(), exclude_directories_.end());
  const auto source_exclude_directories = string_set_preserve_from_value(source.at("excludeDirectories"));
  exclude_directories.insert(source_exclude_directories.begin(), source_exclude_directories.end());
  const bool recursive = !source.contains("recursive") || source.at("recursive").as_bool(true);
  const auto include_patterns = string_vector_from_value(source.at("include"));
  const auto exclude_patterns = string_vector_from_value(source.at("exclude"));

  std::vector<std::filesystem::path> files;
  const auto should_include_file = [&](const std::filesystem::path& file) {
    const auto relative_path = normalize_repository_path(std::filesystem::relative(file, repository_root));
    if (!include_extensions.contains(extension_for_path(file))) {
      return false;
    }
    if (!repository_path_matches_patterns(relative_path, include_patterns)) {
      return false;
    }
    return exclude_patterns.empty() || !repository_path_matches_patterns(relative_path, exclude_patterns);
  };

  if (recursive) {
    std::filesystem::recursive_directory_iterator it(repository_root);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
      if (it->is_directory() && exclude_directories.contains(it->path().filename().string())) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file() && should_include_file(it->path())) {
        files.push_back(std::filesystem::absolute(it->path()).lexically_normal());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(repository_root)) {
      if (entry.is_regular_file() && should_include_file(entry.path())) {
        files.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
      }
    }
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& file : files) {
    const auto relative_path = normalize_repository_path(std::filesystem::relative(file, repository_root));
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["repositoryRoot"] = repository_root.string();
    metadata["repositoryPath"] = relative_path;
    metadata["loader"] = "repository";
    documents.push_back(build_loaded_document("repository",
                                              file.string(),
                                              file.filename().string(),
                                              bytes_to_text(read_binary_file(file)),
                                              std::move(metadata)));
  }
  return documents;
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load_github_repository(const Value& source) const {
  const auto source_url = source.at("url").as_string();
  if (source_url.empty()) {
    throw ConfigurationError("GitHub knowledge source requires url.");
  }
  if (!fetcher_) {
    throw ConfigurationError("GitHub knowledge loader requires a NativeWebPageFetcher.");
  }

  const auto parsed = parse_github_repository_url(source_url);
  const auto repo_json = parse_json(github_fetch_text(*fetcher_,
                                                      "https://api.github.com/repos/" + parsed.owner + "/" + parsed.repo,
                                                      source,
                                                      "GitHub API request"));
  const auto configured_ref = source.at("ref").as_string();
  const auto ref = !configured_ref.empty()
                       ? configured_ref
                       : (!parsed.ref.empty() ? parsed.ref : repo_json.at("default_branch").as_string("main"));
  const auto encoded_ref = encode_uri_component(ref);
  const auto tree_json = parse_json(github_fetch_text(
      *fetcher_,
      "https://api.github.com/repos/" + parsed.owner + "/" + parsed.repo + "/git/trees/" + encoded_ref +
          "?recursive=1",
      source,
      "GitHub API request"));

  const auto include_extensions = string_set_from_value(source.at("extensions"),
                                                        extensions_.empty() ? default_repository_extensions()
                                                                            : extensions_);
  const auto include_patterns = string_vector_from_value(source.at("include"));
  const auto exclude_patterns = string_vector_from_value(source.at("exclude"));
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& entry : tree_json.at("tree").as_array()) {
    if (entry.at("type").as_string() != "blob") {
      continue;
    }
    const auto relative_path = normalize_repository_path(entry.at("path").as_string());
    if (relative_path.empty()) {
      continue;
    }
    if (!parsed.root_path.empty() && relative_path.rfind(parsed.root_path, 0) != 0) {
      continue;
    }
    if (!include_extensions.contains(extension_for_path(relative_path))) {
      continue;
    }
    if (!repository_path_matches_patterns(relative_path, include_patterns)) {
      continue;
    }
    if (!exclude_patterns.empty() && repository_path_matches_patterns(relative_path, exclude_patterns)) {
      continue;
    }

    const auto raw_url = "https://raw.githubusercontent.com/" + parsed.owner + "/" + parsed.repo + "/" +
                         encoded_ref + "/" + relative_path;
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["repositoryOwner"] = parsed.owner;
    metadata["repositoryName"] = parsed.repo;
    metadata["repositoryRef"] = ref;
    metadata["repositoryPath"] = relative_path;
    metadata["loader"] = "repository";
    documents.push_back(build_loaded_document("github",
                                              "github://" + parsed.owner + "/" + parsed.repo + "/" + relative_path,
                                              std::filesystem::path(relative_path).filename().string(),
                                              github_fetch_text(*fetcher_, raw_url, source, "GitHub raw file request"),
                                              std::move(metadata)));
  }
  return documents;
}

WebKnowledgeSourceLoader::WebKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher) : fetcher_(fetcher) {}

void WebKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool WebKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "web";
}

std::vector<LoadedKnowledgeDocument> WebKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!fetcher_) {
    throw ConfigurationError("Web knowledge loader requires a NativeWebPageFetcher.");
  }
  const auto url = source.at("url").as_string();
  if (url.empty()) {
    throw ConfigurationError("Web knowledge source requires url.");
  }
  const auto page = fetcher_->fetch(WebFetchRequest{.url = url, .extract = "html"});
  if (page.status >= 400) {
    throw ConfigurationError("Failed to fetch web source: " + url);
  }
  const auto uri = best_page_url(page);
  return {build_loaded_document("web",
                                url,
                                source.at("title").as_string(page.title.empty() ? uri : page.title),
                                best_page_content(page),
                                copy_object_or_empty(source.at("metadata")))};
}

WebsiteKnowledgeSourceLoader::WebsiteKnowledgeSourceLoader(const NativeWebCrawler* crawler,
                                                           BrowserRenderer* browser)
    : crawler_(crawler), browser_(browser) {}

void WebsiteKnowledgeSourceLoader::set_crawler(const NativeWebCrawler* crawler) {
  crawler_ = crawler;
}

void WebsiteKnowledgeSourceLoader::set_browser(BrowserRenderer* browser) {
  browser_ = browser;
}

bool WebsiteKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "website";
}

std::vector<LoadedKnowledgeDocument> WebsiteKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!crawler_) {
    throw ConfigurationError("Website knowledge loader requires a NativeWebCrawler.");
  }
  const auto url = source.at("url").as_string();
  if (url.empty()) {
    throw ConfigurationError("Website knowledge source requires url.");
  }

  WebCrawlRequest request;
  request.url = url;
  if (source.at("maxDepth").is_number()) {
    request.max_depth = static_cast<std::size_t>(std::max<long long>(0, source.at("maxDepth").as_integer()));
  }
  if (source.at("maxPages").is_number()) {
    request.max_pages = static_cast<std::size_t>(std::max<long long>(0, source.at("maxPages").as_integer()));
  }
  const auto crawled = crawler_->crawl(request);

  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& crawled_page : crawled.pages) {
    std::optional<BrowserRenderResult> rendered;
    if (source.at("render").as_bool(false) && browser_) {
      BrowserRenderRequest render_request;
      render_request.url = crawled_page.page.url.empty() ? best_page_url(crawled_page.page) : crawled_page.page.url;
      rendered = browser_->render(render_request);
    }
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["links"] = string_array_value(crawled_page.links);
    metadata["crawledFrom"] = url;
    const auto uri = best_page_url(crawled_page.page);
    documents.push_back(build_loaded_document(
        "website",
        uri,
        rendered && !rendered->title.empty()
            ? rendered->title
            : (crawled_page.page.title.empty() ? source.at("title").as_string(uri) : crawled_page.page.title),
        rendered && !rendered->text.empty() ? rendered->text : best_page_content(crawled_page.page),
        std::move(metadata)));
  }
  return documents;
}

SitemapKnowledgeSourceLoader::SitemapKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher,
                                                           std::optional<std::size_t> default_limit)
    : fetcher_(fetcher), default_limit_(default_limit) {}

void SitemapKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool SitemapKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "sitemap";
}

std::vector<LoadedKnowledgeDocument> SitemapKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!fetcher_) {
    throw ConfigurationError("Sitemap knowledge loader requires a NativeWebPageFetcher.");
  }
  const auto sitemap_url = source.at("url").as_string();
  if (sitemap_url.empty()) {
    throw ConfigurationError("Sitemap knowledge source requires url.");
  }

  const auto sitemap = fetcher_->fetch(WebFetchRequest{.url = sitemap_url, .extract = "text"});
  const auto xml = !sitemap.text.empty() ? sitemap.text : sitemap.html;
  auto locations = extract_sitemap_locations(xml);
  auto limit = default_limit_;
  if (auto source_limit = optional_size_option_from_value(source, "limit")) {
    limit = source_limit;
  }
  if (limit && locations.size() > *limit) {
    locations.resize(*limit);
  }

  WebKnowledgeSourceLoader web_loader(fetcher_);
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& location : locations) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["loader"] = "sitemap";
    metadata["sitemap"] = sitemap_url;
    auto loaded = web_loader.load(Value::object({{"type", "web"},
                                                {"url", location},
                                                {"metadata", metadata}}));
    for (auto& document : loaded) {
      document.source_type = "sitemap";
      documents.push_back(std::move(document));
    }
  }
  return documents;
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(std::vector<KnowledgeLoaderProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(const KnowledgeLoaderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeLoaderRegistry& KnowledgeLoaderRegistry::operator=(const KnowledgeLoaderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(KnowledgeLoaderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeLoaderRegistry& KnowledgeLoaderRegistry::operator=(KnowledgeLoaderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeLoaderProvider& KnowledgeLoaderRegistry::register_provider(KnowledgeLoaderProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge loader provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge loader provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

const KnowledgeLoaderProvider* KnowledgeLoaderRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeSourceLoader> KnowledgeLoaderRegistry::create(const std::string& name,
                                                                       const Value& options) const {
  KnowledgeLoaderProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      throw ConfigurationError("Unknown knowledge loader provider: " + name);
    }
    factory = found->second.create;
  }
  auto loader = factory(options, *this);
  if (!loader) {
    throw ConfigurationError("Knowledge loader provider returned null: " + name);
  }
  return loader;
}

std::vector<KnowledgeLoaderProvider> KnowledgeLoaderRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeLoaderProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<std::string> KnowledgeLoaderRegistry::list_names() const {
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

CompositeKnowledgeLoader::CompositeKnowledgeLoader(std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders,
                                                   bool use_default_loaders_when_empty)
    : loaders_(std::move(loaders)) {
  if (loaders_.empty() && use_default_loaders_when_empty) {
    loaders_.push_back(std::make_shared<TextKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<FileKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<DirectoryKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<RepositoryKnowledgeSourceLoader>());
  }
}

void CompositeKnowledgeLoader::add_loader(std::shared_ptr<KnowledgeSourceLoader> loader) {
  if (loader) {
    loaders_.push_back(std::move(loader));
  }
}

bool CompositeKnowledgeLoader::supports(const Value&) const {
  return true;
}

std::vector<LoadedKnowledgeDocument> CompositeKnowledgeLoader::load(const Value& source) const {
  for (const auto& loader : loaders_) {
    if (loader && loader->supports(source)) {
      return loader->load(source);
    }
  }
  throw ConfigurationError("No knowledge loader found for source type: " + source.at("type").as_string());
}

std::vector<LoadedKnowledgeDocument> load_knowledge_sources(
    const std::vector<Value>& sources,
    const KnowledgeSourceLoader& loader) {
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& source : sources) {
    auto loaded = loader.load(source);
    documents.insert(documents.end(), std::make_move_iterator(loaded.begin()), std::make_move_iterator(loaded.end()));
  }
  return documents;
}

std::shared_ptr<CompositeKnowledgeLoader> create_default_knowledge_loader(
    const NativeWebPageFetcher* fetcher,
    const NativeWebCrawler* crawler,
    BrowserRenderer* browser) {
  std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders = {
      std::make_shared<TextKnowledgeSourceLoader>(),
      std::make_shared<FileKnowledgeSourceLoader>(),
      std::make_shared<DirectoryKnowledgeSourceLoader>(),
      std::make_shared<MarkdownKnowledgeSourceLoader>(),
      std::make_shared<RepositoryKnowledgeSourceLoader>(fetcher),
  };
  if (fetcher) {
    loaders.push_back(std::make_shared<WebKnowledgeSourceLoader>(fetcher));
    loaders.push_back(std::make_shared<SitemapKnowledgeSourceLoader>(fetcher));
  }
  if (crawler) {
    loaders.push_back(std::make_shared<WebsiteKnowledgeSourceLoader>(crawler, browser));
  }
  return std::make_shared<CompositeKnowledgeLoader>(std::move(loaders));
}

KnowledgeLoaderRegistry create_default_knowledge_loader_registry(const NativeWebPageFetcher* fetcher,
                                                                 const NativeWebCrawler* crawler,
                                                                 BrowserRenderer* browser) {
  KnowledgeLoaderRegistry registry;
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "text",
          .tier = "core-safe",
          .title = "Inline Text Loader",
          .tags = {"loader", "text"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<TextKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "file",
          .tier = "core-safe",
          .title = "File Loader",
          .tags = {"loader", "file"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<FileKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "directory",
          .tier = "core-safe",
          .title = "Directory Loader",
          .tags = {"loader", "directory"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<DirectoryKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "markdown",
          .tier = "portable",
          .title = "Markdown Loader",
          .description = "Loads Markdown files and directories.",
          .tags = {"loader", "markdown"},
      },
      .create = [](const Value& options, const KnowledgeLoaderRegistry&) {
        return std::make_shared<MarkdownKnowledgeSourceLoader>(options.at("recursive").as_bool(true));
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "repository",
          .tier = "portable",
          .title = "Repository Loader",
          .description = "Loads local repositories and GitHub repository URLs.",
          .tags = {"loader", "repository", "github", "code"},
      },
      .create = [fetcher](const Value& options, const KnowledgeLoaderRegistry&) {
        return std::make_shared<RepositoryKnowledgeSourceLoader>(
            fetcher,
            string_set_from_value(options.at("extensions")),
            string_set_preserve_from_value(options.at("excludeDirectories")));
      },
  });
  if (fetcher) {
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "web",
            .tier = "portable",
            .title = "Web Loader",
            .tags = {"loader", "web"},
        },
        .create = [fetcher](const Value&, const KnowledgeLoaderRegistry&) {
          return std::make_shared<WebKnowledgeSourceLoader>(fetcher);
        },
    });
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "sitemap",
            .tier = "portable",
            .title = "Sitemap Loader",
            .description = "Expands sitemap XML into multiple web documents.",
            .tags = {"loader", "sitemap", "web"},
        },
        .create = [fetcher](const Value& options, const KnowledgeLoaderRegistry&) {
          return std::make_shared<SitemapKnowledgeSourceLoader>(
              fetcher,
              optional_size_option_from_value(options, "limit"));
        },
    });
  }
  if (crawler) {
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "website",
            .tier = "portable",
            .title = "Website Loader",
            .description = "Site-level ingest loader backed by crawler and optional browser rendering.",
            .tags = {"loader", "website", "crawl"},
        },
        .create = [crawler, browser](const Value&, const KnowledgeLoaderRegistry&) {
          return std::make_shared<WebsiteKnowledgeSourceLoader>(crawler, browser);
        },
    });
  }
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "composite",
          .tier = "core-safe",
          .title = "Composite Loader",
          .tags = {"loader", "composite"},
      },
      .create = [](const Value& options, const KnowledgeLoaderRegistry& active_registry) {
        std::vector<std::string> loader_names = string_vector_from_value(options.at("loaders"));
        if (loader_names.empty()) {
          for (const auto& provider : active_registry.list()) {
            if (provider.metadata.name != "composite") {
              loader_names.push_back(provider.metadata.name);
            }
          }
        }

        std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders;
        for (const auto& loader_name : loader_names) {
          if (loader_name == "composite") {
            continue;
          }
          if (active_registry.get(loader_name)) {
            loaders.push_back(active_registry.create(loader_name));
          }
        }
        return std::make_shared<CompositeKnowledgeLoader>(std::move(loaders), false);
      },
  });
  return registry;
}

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

std::vector<KnowledgeDocumentRecord> InMemoryKnowledgeStore::upsert_documents(
    const std::vector<KnowledgeDocumentRecord>& documents) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& document : documents) {
    documents_[document.id] = document;
  }
  return documents;
}

std::vector<KnowledgeChunkRecord> InMemoryKnowledgeStore::upsert_chunks(
    const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& chunk : chunks) {
    chunks_[chunk.id] = chunk;
  }
  return chunks;
}

std::vector<KnowledgeDocumentRecord> InMemoryKnowledgeStore::list_documents() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<KnowledgeDocumentRecord> documents;
  documents.reserve(documents_.size());
  for (const auto& [_, document] : documents_) {
    documents.push_back(document);
  }
  return documents;
}

std::vector<KnowledgeChunkRecord> InMemoryKnowledgeStore::list_chunks() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<KnowledgeChunkRecord> chunks;
  chunks.reserve(chunks_.size());
  for (const auto& [_, chunk] : chunks_) {
    chunks.push_back(chunk);
  }
  return chunks;
}

std::size_t InMemoryKnowledgeStore::delete_documents(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += documents_.erase(id);
  }
  return deleted;
}

std::size_t InMemoryKnowledgeStore::delete_chunks(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += chunks_.erase(id);
  }
  return deleted;
}

void InMemoryKnowledgeStore::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  documents_.clear();
  chunks_.clear();
}

KnowledgeStoreStats InMemoryKnowledgeStore::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return KnowledgeStoreStats{documents_.size(), chunks_.size()};
}

FileKnowledgeStore::FileKnowledgeStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

std::vector<KnowledgeDocumentRecord> FileKnowledgeStore::upsert_documents(
    const std::vector<KnowledgeDocumentRecord>& documents) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryKnowledgeStore::upsert_documents(documents);
  persist();
  return result;
}

std::vector<KnowledgeChunkRecord> FileKnowledgeStore::upsert_chunks(
    const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryKnowledgeStore::upsert_chunks(chunks);
  persist();
  return result;
}

std::vector<KnowledgeDocumentRecord> FileKnowledgeStore::list_documents() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::list_documents();
}

std::vector<KnowledgeChunkRecord> FileKnowledgeStore::list_chunks() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::list_chunks();
}

std::size_t FileKnowledgeStore::delete_documents(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  const auto deleted = InMemoryKnowledgeStore::delete_documents(ids);
  persist();
  return deleted;
}

std::size_t FileKnowledgeStore::delete_chunks(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  const auto deleted = InMemoryKnowledgeStore::delete_chunks(ids);
  persist();
  return deleted;
}

void FileKnowledgeStore::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  InMemoryKnowledgeStore::clear();
  persist();
}

KnowledgeStoreStats FileKnowledgeStore::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::stats();
}

const std::filesystem::path& FileKnowledgeStore::file_path() const noexcept {
  return file_path_;
}

void FileKnowledgeStore::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  documents_.clear();
  chunks_.clear();
  if (!std::filesystem::exists(file_path_)) {
    return;
  }

  const auto raw = read_json_file(file_path_);
  for (const auto& item : raw.at("documents").as_array()) {
    auto document = knowledge_document_record_from_value(item);
    if (!document.id.empty()) {
      const auto document_id = document.id;
      documents_[document_id] = std::move(document);
    }
  }
  for (const auto& item : raw.at("chunks").as_array()) {
    auto chunk = knowledge_chunk_record_from_value(item);
    if (!chunk.id.empty()) {
      const auto chunk_id = chunk.id;
      chunks_[chunk_id] = std::move(chunk);
    }
  }
}

void FileKnowledgeStore::persist() const {
  Value::Array documents;
  for (const auto& [_, document] : documents_) {
    documents.push_back(knowledge_document_record_to_value(document));
  }
  Value::Array chunks;
  for (const auto& [_, chunk] : chunks_) {
    chunks.push_back(knowledge_chunk_record_to_value(chunk));
  }
  write_json_file(file_path_, Value::object({{"documents", Value(documents)}, {"chunks", Value(chunks)}}));
}

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
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
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
    config_.transport = create_native_http_transport();
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
          .description = "Qdrant-backed vector index using the injected or native plain-HTTP transport.",
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
            .transport = create_native_http_transport(),
        });
      },
  });
  return registry;
}

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

KnowledgeBase::KnowledgeBase(std::string id, std::string tenant_id, std::string title,
                             std::shared_ptr<TextEmbeddingAdapter> embedder,
                             RecursiveTextChunker chunker,
                             std::shared_ptr<KnowledgeReranker> reranker,
                             std::shared_ptr<KnowledgeStore> store,
                             std::shared_ptr<ImageEmbeddingAdapter> image_embedder,
                             std::shared_ptr<KnowledgeTextIndex> text_index,
                             std::shared_ptr<KnowledgeVectorIndex> vector_index,
                             KnowledgeSearchOptions search_defaults,
                             std::string description,
                             std::string context_title,
                             std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy,
                             std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy,
                             std::shared_ptr<KnowledgeContextRenderer> context_renderer)
    : id_(std::move(id)),
      tenant_id_(std::move(tenant_id)),
      title_(title.empty() ? id_ : std::move(title)),
      description_(std::move(description)),
      context_title_(context_title.empty() ? "Knowledge base retrieval: " + title_ : std::move(context_title)),
      embedder_(std::move(embedder)),
      image_embedder_(std::move(image_embedder)),
      chunker_(std::move(chunker)),
      reranker_(std::move(reranker)),
      store_(std::move(store)),
      text_index_(std::move(text_index)),
      vector_index_(std::move(vector_index)),
      ingestion_strategy_(std::move(ingestion_strategy)),
      retrieval_strategy_(std::move(retrieval_strategy)),
      context_renderer_(std::move(context_renderer)),
      search_defaults_(std::move(search_defaults)) {
  if (id_.empty()) {
    throw ConfigurationError("KnowledgeBase id is required.");
  }
  if (!reranker_) {
    reranker_ = std::make_shared<HeuristicKnowledgeReranker>();
  }
  if (!store_) {
    store_ = std::make_shared<InMemoryKnowledgeStore>();
  }
  if (!text_index_) {
    text_index_ = std::make_shared<InMemoryKnowledgeTextIndex>();
  }
  if (!image_embedder_) {
    image_embedder_ = std::make_shared<HashImageEmbeddingAdapter>();
  }
  if (!ingestion_strategy_) {
    ingestion_strategy_ = std::make_shared<DefaultKnowledgeIngestionStrategy>();
  }
  if (!context_renderer_) {
    context_renderer_ = std::make_shared<DefaultKnowledgeContextRenderer>();
  }
}

void KnowledgeBase::rebuild_text_index() {
  if (!text_index_) {
    return;
  }
  text_index_->clear(KnowledgeTextIndexClearOptions{id_, tenant_id_});
  text_index_->upsert(store_->list_chunks());
  text_index_ready_ = true;
}

void KnowledgeBase::ensure_text_index_ready() {
  if (!text_index_ready_) {
    rebuild_text_index();
  }
}

void KnowledgeBase::rebuild_vector_index() {
  if (!vector_index_) {
    return;
  }
  vector_index_->clear(KnowledgeVectorIndexClearOptions{id_, tenant_id_});
  vector_index_->upsert(store_->list_chunks());
  vector_index_ready_ = true;
}

void KnowledgeBase::ensure_vector_index_ready() {
  if (!vector_index_ready_) {
    rebuild_vector_index();
  }
}

KnowledgeIngestionResult KnowledgeBase::ingest_loaded_documents(
    const std::vector<LoadedKnowledgeDocument>& documents, bool replace_existing) {
  return ingest_loaded_documents(documents, KnowledgeIngestOptions{.replace_existing = replace_existing});
}

KnowledgeIngestionResult KnowledgeBase::ingest_loaded_documents(
    const std::vector<LoadedKnowledgeDocument>& documents, KnowledgeIngestOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  throw_if_knowledge_cancelled(options.cancellation);
  const auto normalized_documents = dedupe_loaded_knowledge_documents(documents, options.replace_existing);
  if (normalized_documents.empty()) {
    return KnowledgeIngestionResult{id_, tenant_id_, 0, 0, {}, {}};
  }
  const auto existing = store_->list_documents();
  const auto existing_chunks = store_->list_chunks();
  std::map<std::string, std::vector<KnowledgeDocumentRecord>> existing_by_uri;
  for (const auto& document : existing) {
    existing_by_uri[document.uri].push_back(document);
  }
  std::vector<LoadedKnowledgeDocument> documents_to_insert;
  std::set<std::string> document_ids_to_delete;
  for (const auto& document : normalized_documents) {
    throw_if_knowledge_cancelled(options.cancellation);
    const auto found = existing_by_uri.find(document.uri);
    const std::vector<KnowledgeDocumentRecord> matches =
        found == existing_by_uri.end() ? std::vector<KnowledgeDocumentRecord>{} : found->second;
    const auto content_hash = hash_knowledge_content(document.content);
    const bool unchanged = options.skip_if_unchanged &&
        std::any_of(matches.begin(), matches.end(), [&](const auto& current) {
          return hash_knowledge_content(current.content) == content_hash;
        });
    if (unchanged) {
      continue;
    }
    if (options.replace_existing) {
      for (const auto& current : matches) {
        document_ids_to_delete.insert(current.id);
      }
    }
    documents_to_insert.push_back(document);
  }
  if (documents_to_insert.empty()) {
    return KnowledgeIngestionResult{id_, tenant_id_, 0, 0, {}, {}};
  }
  if (options.replace_existing) {
    std::vector<std::string> to_delete;
    std::vector<std::string> chunks_to_delete;
    to_delete.assign(document_ids_to_delete.begin(), document_ids_to_delete.end());
    for (const auto& chunk : existing_chunks) {
      if (document_ids_to_delete.contains(chunk.document_id)) {
        chunks_to_delete.push_back(chunk.id);
      }
    }
    std::sort(chunks_to_delete.begin(), chunks_to_delete.end());
    chunks_to_delete.erase(std::unique(chunks_to_delete.begin(), chunks_to_delete.end()), chunks_to_delete.end());
    if (!to_delete.empty()) {
      std::set<std::string> seen;
      std::vector<std::string> unique_ids;
      for (const auto& id : to_delete) {
        if (seen.insert(id).second) {
          unique_ids.push_back(id);
        }
      }
      to_delete = std::move(unique_ids);
    }
    throw_if_knowledge_cancelled(options.cancellation);
    if (!chunks_to_delete.empty()) {
      store_->delete_chunks(chunks_to_delete);
    }
    if (!to_delete.empty()) {
      store_->delete_documents(to_delete);
    }
  }
  throw_if_knowledge_cancelled(options.cancellation);

  const auto now = now_iso8601();
  std::vector<KnowledgeDocumentRecord> records;
  std::vector<KnowledgeChunkRecord> chunks;
  const bool has_text_documents = std::any_of(documents_to_insert.begin(), documents_to_insert.end(), [](const auto& document) {
    return document.asset_type == KnowledgeAssetType::Text;
  });
  const bool has_image_documents = std::any_of(documents_to_insert.begin(), documents_to_insert.end(), [](const auto& document) {
    return document.asset_type == KnowledgeAssetType::Image;
  });
  const auto text_space_id = embedder_->space().id;
  const auto image_space_id = image_embedder_->space().id;
  if (has_text_documents && has_image_documents && text_space_id != image_space_id) {
    throw ConfigurationError("Cross-modal ingestion requires shared embedding space ids. Received text=\"" +
                             text_space_id + "\" and image=\"" + image_space_id + "\".");
  }

  for (const auto& loaded : documents_to_insert) {
    throw_if_knowledge_cancelled(options.cancellation);
    Value document_metadata = loaded.metadata;
    if (!loaded.alt_text.empty()) document_metadata["altText"] = loaded.alt_text;
    if (!loaded.ocr_text.empty()) document_metadata["ocrText"] = loaded.ocr_text;
    if (!loaded.caption.empty()) document_metadata["caption"] = loaded.caption;

    KnowledgeDocumentRecord record;
    record.id = generate_uuid();
    record.knowledge_base_id = id_;
    record.tenant_id = tenant_id_;
    record.source_type = loaded.source_type;
    record.asset_type = loaded.asset_type;
    record.uri = loaded.uri;
    record.title = loaded.title.empty() ? loaded.uri : loaded.title;
    record.content = loaded.content;
    record.media = loaded.media;
    record.text_hint = loaded.text_hint;
    record.metadata = document_metadata;
    record.created_at = now;
    record.updated_at = now;
    records.push_back(record);

    const auto drafts = chunker_.chunk(loaded);
    std::vector<EmbeddingVector> embeddings;
    if (loaded.asset_type == KnowledgeAssetType::Image) {
      if (!drafts.empty()) {
        auto image_vector = image_embedder_->embed_one(
            image_embedding_input_from_document(loaded),
            Value::object({{"taskType", "RETRIEVAL_DOCUMENT"}, {"spaceId", image_space_id}}),
            options.cancellation);
        embeddings.assign(drafts.size(), std::move(image_vector));
      }
    } else {
      std::vector<std::string> texts;
      texts.reserve(drafts.size());
      for (const auto& draft : drafts) {
        texts.push_back(draft.content);
      }
      const auto embedding_settings =
          Value::object({{"taskType", "RETRIEVAL_DOCUMENT"}, {"spaceId", text_space_id}});
      const std::size_t embedding_batch_size =
          options.embedding_batch_size == 0 ? texts.size() : options.embedding_batch_size;
      if (texts.empty()) {
        embeddings = {};
      } else if (embedding_batch_size >= texts.size()) {
        embeddings = embedder_->embed(texts, embedding_settings, options.cancellation);
      } else {
        for (std::size_t index = 0; index < texts.size(); index += embedding_batch_size) {
          throw_if_knowledge_cancelled(options.cancellation);
          const auto end = std::min(texts.size(), index + embedding_batch_size);
          std::vector<std::string> batch(texts.begin() + static_cast<std::ptrdiff_t>(index),
                                         texts.begin() + static_cast<std::ptrdiff_t>(end));
          auto batch_embeddings = embedder_->embed(batch, embedding_settings, options.cancellation);
          embeddings.insert(embeddings.end(),
                            std::make_move_iterator(batch_embeddings.begin()),
                            std::make_move_iterator(batch_embeddings.end()));
        }
      }
    }

    for (std::size_t index = 0; index < drafts.size(); ++index) {
      const auto& draft = drafts[index];
      Value chunk_metadata = document_metadata;
      for (const auto& [key, value] : draft.metadata.as_object()) {
        chunk_metadata[key] = value;
      }
      chunks.push_back(KnowledgeChunkRecord{generate_uuid(),
                                            record.id,
                                            id_,
                                            tenant_id_,
                                            loaded.source_type,
                                            loaded.asset_type,
                                            loaded.uri,
                                            record.title,
                                            draft.content,
                                            draft.chunk_index,
                                            draft.start_offset,
                                            draft.end_offset,
                                            draft.line_start,
                                            draft.line_end,
                                            loaded.media,
                                            loaded.asset_type == KnowledgeAssetType::Image ? image_space_id : text_space_id,
                                            chunk_metadata,
                                            index < embeddings.size() ? embeddings[index] : EmbeddingVector{},
                                            now,
                                            now});
    }
  }
  throw_if_knowledge_cancelled(options.cancellation);
  store_->upsert_documents(records);
  store_->upsert_chunks(chunks);
  rebuild_text_index();
  rebuild_vector_index();
  return KnowledgeIngestionResult{id_, tenant_id_, records.size(), chunks.size(), records, chunks};
}

KnowledgeIngestionResult KnowledgeBase::ingest(const std::vector<Value>& sources, const KnowledgeSourceLoader& loader,
                                               KnowledgeIngestOptions options) {
  if (ingestion_strategy_) {
    return ingestion_strategy_->ingest(KnowledgeIngestionStrategyContext{*this, sources, loader, options});
  }
  throw_if_knowledge_cancelled(options.cancellation);
  auto documents = load_knowledge_sources(sources, loader);
  throw_if_knowledge_cancelled(options.cancellation);
  return ingest_loaded_documents(documents, options);
}

KnowledgeDeleteResult KnowledgeBase::delete_documents(const KnowledgeDeleteOptions& options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto all_documents = store_->list_documents();
  std::vector<KnowledgeDocumentRecord> matched_documents;
  for (const auto& document : all_documents) {
    if (!options.document_ids.empty() && !vector_contains(options.document_ids, document.id)) {
      continue;
    }
    if (!options.uris.empty() && !vector_contains(options.uris, document.uri)) {
      continue;
    }
    if (!options.uri_prefix.empty() && document.uri.rfind(options.uri_prefix, 0) != 0) {
      continue;
    }
    if (!options.source_types.empty() && !vector_contains(options.source_types, document.source_type)) {
      continue;
    }
    if (!metadata_matches_filter(document.metadata, options.metadata)) {
      continue;
    }
    matched_documents.push_back(document);
  }

  KnowledgeDeleteResult result;
  result.knowledge_base_id = id_;
  result.tenant_id = tenant_id_;
  if (matched_documents.empty()) {
    return result;
  }

  std::set<std::string> matched_document_ids;
  for (const auto& document : matched_documents) {
    matched_document_ids.insert(document.id);
    result.document_ids.push_back(document.id);
  }
  for (const auto& chunk : store_->list_chunks()) {
    if (matched_document_ids.contains(chunk.document_id)) {
      result.chunk_ids.push_back(chunk.id);
    }
  }

  if (!result.chunk_ids.empty()) {
    store_->delete_chunks(result.chunk_ids);
  }
  store_->delete_documents(result.document_ids);
  result.document_count = result.document_ids.size();
  result.chunk_count = result.chunk_ids.size();
  rebuild_text_index();
  rebuild_vector_index();
  return result;
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search(const std::string& query, KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return search_with_debug(query, std::move(options)).hits;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto query_vector = embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", embedder_->space().id}}),
      options.cancellation);
  return search_with_embedding(query, query_vector, std::move(options), nullptr);
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search(const ImageEmbeddingInput& query, KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return search_with_debug(query, std::move(options)).hits;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto query_vector = image_embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", image_embedder_->space().id}}),
      options.cancellation);
  const std::string query_text = image_embedding_query_text(query);
  return search_with_embedding(query_text, query_vector, std::move(options), nullptr);
}

KnowledgeSearchResult KnowledgeBase::search_with_debug(const std::string& query,
                                                       KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return retrieval_strategy_->search(*this, query, std::move(options));
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Value debug = Value::object({});
  const auto query_vector = embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", embedder_->space().id}}),
      options.cancellation);
  auto hits = search_with_embedding(query, query_vector, std::move(options), &debug);
  return KnowledgeSearchResult{std::move(hits), std::move(debug)};
}

KnowledgeSearchResult KnowledgeBase::search_with_debug(const ImageEmbeddingInput& query,
                                                       KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return retrieval_strategy_->search(*this, query, std::move(options));
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Value debug = Value::object({});
  const auto query_vector = image_embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", image_embedder_->space().id}}),
      options.cancellation);
  const std::string query_text = image_embedding_query_text(query);
  auto hits = search_with_embedding(query_text, query_vector, std::move(options), &debug);
  return KnowledgeSearchResult{std::move(hits), std::move(debug)};
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search_with_embedding(const std::string& query_text,
                                                                     const EmbeddingVector& query_vector,
                                                                     KnowledgeSearchOptions options,
                                                                     Value* debug) {
  options = effective_knowledge_search_options(search_defaults_, options);
  const auto documents = store_->list_documents();
  const auto chunks = store_->list_chunks();
  std::map<std::string, KnowledgeDocumentRecord> document_by_id;
  for (const auto& document : documents) {
    document_by_id[document.id] = document;
  }

  std::vector<KnowledgeChunkRecord> filtered_chunks;
  filtered_chunks.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    if (!options.uri_prefix.empty() && chunk.uri.rfind(options.uri_prefix, 0) != 0) {
      continue;
    }
    if (!options.document_ids.empty() && !vector_contains(options.document_ids, chunk.document_id)) {
      continue;
    }
    if (!options.chunk_ids.empty() && !vector_contains(options.chunk_ids, chunk.id)) {
      continue;
    }
    if (!options.source_types.empty() && !vector_contains(options.source_types, chunk.source_type)) {
      continue;
    }
    if (!options.asset_types.empty() &&
        std::find(options.asset_types.begin(), options.asset_types.end(), chunk.asset_type) ==
            options.asset_types.end()) {
      continue;
    }
    if (!options.space_id.empty() && chunk.embedding_space_id != options.space_id) {
      continue;
    }
    if (!metadata_matches_filter(chunk.metadata, options.metadata)) {
      continue;
    }
    filtered_chunks.push_back(chunk);
  }

  std::map<std::string, const KnowledgeChunkRecord*> chunk_by_id;
  for (const auto& chunk : filtered_chunks) {
    chunk_by_id[chunk.id] = &chunk;
  }

  const auto effective_vector_top_k = effective_knowledge_vector_top_k(options, filtered_chunks.size());
  std::vector<RankedKnowledgeChunk> vector_ranked;
  if (!filtered_chunks.empty() && effective_vector_top_k > 0) {
    if (vector_index_) {
      ensure_vector_index_ready();
      KnowledgeVectorSearchOptions vector_options;
      vector_options.knowledge_base_id = id_;
      vector_options.tenant_id = tenant_id_;
      vector_options.embedding = query_vector;
      vector_options.top_k = effective_vector_top_k;
      vector_options.min_score = 0.0;
      vector_options.source_types = options.source_types;
      vector_options.asset_types = options.asset_types;
      vector_options.uri_prefix = options.uri_prefix;
      vector_options.document_ids = options.document_ids;
      vector_options.chunk_ids = options.chunk_ids;
      vector_options.space_id = options.space_id;
      vector_options.metadata = options.metadata;
      for (const auto& match : vector_index_->search(vector_options)) {
        if (const auto found = chunk_by_id.find(match.chunk_id); found != chunk_by_id.end()) {
          vector_ranked.push_back(RankedKnowledgeChunk{found->second, match.score});
        }
      }
    } else {
      vector_ranked.reserve(filtered_chunks.size());
      for (const auto& chunk : filtered_chunks) {
        vector_ranked.push_back(RankedKnowledgeChunk{&chunk, dot_product(query_vector, chunk.embedding)});
      }
    }
    std::sort(vector_ranked.begin(), vector_ranked.end(), [](const auto& left, const auto& right) {
      if (left.score == right.score) {
        return left.chunk->id < right.chunk->id;
      }
      return left.score > right.score;
    });
    if (vector_ranked.size() > effective_vector_top_k) {
      vector_ranked.resize(effective_vector_top_k);
    }
  }

  std::map<std::string, double> vector_score_by_id;
  std::map<std::string, std::size_t> vector_rank_by_id;
  for (std::size_t index = 0; index < vector_ranked.size(); ++index) {
    vector_score_by_id[vector_ranked[index].chunk->id] = vector_ranked[index].score;
    vector_rank_by_id[vector_ranked[index].chunk->id] = index + 1;
  }

  const std::size_t effective_lexical_top_k = options.lexical_top_k == 0
                                                 ? filtered_chunks.size()
                                                 : std::min(filtered_chunks.size(), options.lexical_top_k);
  std::vector<RankedKnowledgeChunk> lexical_ranked;
  if (text_index_ && !query_text.empty() && !filtered_chunks.empty() && effective_lexical_top_k > 0) {
    ensure_text_index_ready();
    KnowledgeTextSearchOptions text_options;
    text_options.knowledge_base_id = id_;
    text_options.tenant_id = tenant_id_;
    text_options.query = query_text;
    text_options.top_k = effective_lexical_top_k;
    text_options.source_types = options.source_types;
    text_options.asset_types = options.asset_types;
    text_options.uri_prefix = options.uri_prefix;
    text_options.document_ids = options.document_ids;
    text_options.chunk_ids = options.chunk_ids;
    text_options.space_id = options.space_id;
    text_options.metadata = options.metadata;
    for (const auto& match : text_index_->search(text_options)) {
      if (const auto found = chunk_by_id.find(match.chunk_id); found != chunk_by_id.end()) {
        lexical_ranked.push_back(RankedKnowledgeChunk{found->second, match.score});
      }
    }
  } else if (!text_index_ && !query_text.empty() && !filtered_chunks.empty() && effective_lexical_top_k > 0) {
    const auto query_tokens = tokenize_knowledge_text(query_text);
    lexical_ranked.reserve(filtered_chunks.size());
    for (const auto& chunk : filtered_chunks) {
      const auto chunk_tokens = tokenize_knowledge_text(knowledge_text_index_surface(chunk));
      std::size_t overlap = 0;
      for (const auto& token : query_tokens) {
        if (std::find(chunk_tokens.begin(), chunk_tokens.end(), token) != chunk_tokens.end()) {
          ++overlap;
        }
      }
      const double score = query_tokens.empty() ? 0 : static_cast<double>(overlap) / query_tokens.size();
      if (score > 0) {
        lexical_ranked.push_back(RankedKnowledgeChunk{&chunk, score});
      }
    }
  }
  std::sort(lexical_ranked.begin(), lexical_ranked.end(), [](const auto& left, const auto& right) {
    if (left.score == right.score) {
      return left.chunk->id < right.chunk->id;
    }
    return left.score > right.score;
  });
  if (lexical_ranked.size() > effective_lexical_top_k) {
    lexical_ranked.resize(effective_lexical_top_k);
  }

  std::map<std::string, double> lexical_score_by_id;
  std::map<std::string, std::size_t> lexical_rank_by_id;
  for (std::size_t index = 0; index < lexical_ranked.size(); ++index) {
    lexical_score_by_id[lexical_ranked[index].chunk->id] = lexical_ranked[index].score;
    lexical_rank_by_id[lexical_ranked[index].chunk->id] = index + 1;
  }

  std::set<std::string> candidate_ids;
  for (const auto& item : vector_ranked) {
    candidate_ids.insert(item.chunk->id);
  }
  if (lower_copy(options.retrieval_mode) != "vector") {
    for (const auto& item : lexical_ranked) {
      candidate_ids.insert(item.chunk->id);
    }
  }

  std::vector<KnowledgeSearchHit> hits;
  for (const auto& chunk : filtered_chunks) {
    if (!candidate_ids.contains(chunk.id)) {
      continue;
    }
    const auto vector_found = vector_score_by_id.find(chunk.id);
    const double vector_score = vector_found == vector_score_by_id.end() ? 0 : vector_found->second;
    const auto lexical_found = lexical_score_by_id.find(chunk.id);
    const double lexical_score = lexical_found == lexical_score_by_id.end() ? 0 : lexical_found->second;
    const double modality_weight = knowledge_modality_weight(options, chunk.asset_type);
    double score = 0;
    if (lower_copy(options.fusion) == "rrf") {
      constexpr double rrf_offset = 60.0;
      const double rrf_base = (1.0 / (rrf_offset + 1.0)) * 2.0;
      const auto vector_rank_found = vector_rank_by_id.find(chunk.id);
      const auto lexical_rank_found = lexical_rank_by_id.find(chunk.id);
      const double vector_rrf = vector_rank_found == vector_rank_by_id.end()
                                    ? 0.0
                                    : 1.0 / (rrf_offset + static_cast<double>(vector_rank_found->second));
      const double lexical_rrf = lexical_rank_found == lexical_rank_by_id.end()
                                     ? 0.0
                                     : 1.0 / (rrf_offset + static_cast<double>(lexical_rank_found->second));
      score = ((vector_rrf + lexical_rrf) / rrf_base) * modality_weight;
    } else {
      score = (options.hybrid_alpha * vector_score + (1.0 - options.hybrid_alpha) * lexical_score) *
              modality_weight;
    }
    if (score < options.min_score) {
      continue;
    }
    const auto document_found = document_by_id.find(chunk.document_id);
    if (document_found == document_by_id.end()) {
      continue;
    }
    const auto& document = document_found->second;
    const std::string snippet = chunk.content.size() > 240 ? chunk.content.substr(0, 237) + "..." : chunk.content;
    KnowledgeCitation citation{id_,
                               title_,
                               tenant_id_,
                               document.id,
                               chunk.id,
                               chunk.source_type,
                               chunk.asset_type,
                               chunk.uri,
                               chunk.title,
                               chunk.chunk_index,
                               chunk.start_offset,
                               chunk.end_offset,
                               chunk.line_start,
                               chunk.line_end,
                               score,
                               vector_score,
                               lexical_score,
                               chunk.media,
                               chunk.embedding_space_id,
                               chunk.metadata,
                               snippet};
    hits.push_back(KnowledgeSearchHit{document, chunk, score, vector_score, lexical_score, std::nullopt, citation});
  }

  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    return left.score > right.score;
  });
  const auto hybrid_hits = hits;
  const std::size_t rerank_top_k = options.rerank_top_k == 0
                                       ? std::min(hits.size(), std::max<std::size_t>(options.top_k, 8))
                                       : std::min(hits.size(), options.rerank_top_k);
  std::vector<KnowledgeSearchHit> candidates(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(rerank_top_k));
  std::vector<KnowledgeSearchHit> final_hits;
  if (reranker_) {
    final_hits = reranker_->rerank(query_text, std::move(candidates), options.top_k);
  } else {
    final_hits = std::move(candidates);
    if (options.top_k > 0 && final_hits.size() > options.top_k) {
      final_hits.resize(options.top_k);
    }
  }

  if (debug) {
    Value::Array vector_candidates;
    for (const auto& item : vector_ranked) {
      vector_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, *item.chunk, item.score, item.score, std::nullopt));
    }
    Value::Array lexical_candidates;
    for (const auto& item : lexical_ranked) {
      lexical_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, *item.chunk, item.score, std::nullopt, item.score));
    }
    Value::Array hybrid_candidates;
    for (const auto& hit : hybrid_hits) {
      hybrid_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, hit.chunk, hit.score, hit.vector_score, hit.lexical_score));
    }
    Value::Array final_candidates;
    for (const auto& hit : final_hits) {
      final_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, hit.chunk, hit.rerank_score.value_or(hit.score), hit.vector_score, hit.lexical_score));
    }
    *debug = Value::object({{"query", query_text},
                            {"totalDocumentCount", documents.size()},
                            {"totalChunkCount", chunks.size()},
                            {"filteredChunkCount", filtered_chunks.size()},
                            {"vectorCandidates", Value(std::move(vector_candidates))},
                            {"lexicalCandidates", Value(std::move(lexical_candidates))},
                            {"hybridCandidates", Value(std::move(hybrid_candidates))},
                            {"finalHits", Value(std::move(final_candidates))}});
  }
  return final_hits;
}

KnowledgeStoreStats KnowledgeBase::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return store_->stats();
}

std::shared_ptr<KnowledgeStore> KnowledgeBase::store() const noexcept {
  return store_;
}

Value knowledge_citation_to_value(const KnowledgeCitation& citation) {
  return Value::object({{"knowledgeBaseId", citation.knowledge_base_id},
                        {"knowledgeBaseTitle", citation.knowledge_base_title},
                        {"tenantId", citation.tenant_id},
                        {"documentId", citation.document_id},
                        {"chunkId", citation.chunk_id},
                        {"sourceType", citation.source_type},
                        {"assetType", knowledge_asset_type_label(citation.asset_type)},
                        {"uri", citation.uri},
                        {"title", citation.title},
                        {"chunkIndex", citation.chunk_index},
                        {"startOffset", citation.start_offset},
                        {"endOffset", citation.end_offset},
                        {"lineStart", citation.line_start},
                        {"lineEnd", citation.line_end},
                        {"score", citation.score},
                        {"vectorScore", citation.vector_score},
                        {"lexicalScore", citation.lexical_score},
                        {"media", media_source_to_value(citation.media)},
                        {"embeddingSpaceId", citation.embedding_space_id},
                        {"metadata", citation.metadata},
                        {"snippet", citation.snippet}});
}

Value knowledge_search_hit_to_value(const KnowledgeSearchHit& hit) {
  auto value = Value::object({{"document", knowledge_document_record_to_value(hit.document)},
                              {"chunk", knowledge_chunk_record_to_value(hit.chunk)},
                              {"score", hit.score},
                              {"vectorScore", hit.vector_score},
                              {"lexicalScore", hit.lexical_score},
                              {"citation", knowledge_citation_to_value(hit.citation)}});
  if (hit.rerank_score) {
    value["rerankScore"] = *hit.rerank_score;
  }
  return value;
}

std::optional<AgentMessage> KnowledgeBase::create_context_message(const std::vector<KnowledgeSearchHit>& hits) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (context_renderer_) {
    return context_renderer_->render(KnowledgeContextRenderContext{*this, hits});
  }
  return DefaultKnowledgeContextRenderer().render(KnowledgeContextRenderContext{*this, hits});
}

KnowledgeContextResult KnowledgeBase::build_context_message(const std::string& query,
                                                            KnowledgeSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

KnowledgeContextResult KnowledgeBase::build_context_message(const ImageEmbeddingInput& query,
                                                            KnowledgeSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

const std::string& KnowledgeBase::id() const noexcept {
  return id_;
}

const std::string& KnowledgeBase::tenant_id() const noexcept {
  return tenant_id_;
}

const std::string& KnowledgeBase::title() const noexcept {
  return title_;
}

const std::string& KnowledgeBase::description() const noexcept {
  return description_;
}

const std::string& KnowledgeBase::context_title() const noexcept {
  return context_title_;
}

KnowledgeIngestionPipeline::KnowledgeIngestionPipeline(KnowledgeBase& knowledge_base,
                                                       const KnowledgeSourceLoader& loader)
    : knowledge_base_(knowledge_base), loader_(loader) {}

KnowledgeIngestionResult KnowledgeIngestionPipeline::ingest(
    const std::vector<Value>& sources,
    KnowledgeIngestionPipelineOptions options) const {
  throw_if_knowledge_cancelled(options.cancellation);
  report_knowledge_progress(options.on_progress, "loading", sources.size(), 0);
  FixedKnowledgeRetryStrategy default_retry_strategy(options.max_attempts, options.retry_delay_ms);
  const auto& retry_strategy = options.retry_strategy ? *options.retry_strategy : default_retry_strategy;
  auto loaded_documents = run_with_knowledge_retry([&]() {
    return load_knowledge_sources(sources, loader_);
  }, retry_strategy, KnowledgeRetryContext{knowledge_base_, "loading"}, options.cancellation);
  throw_if_knowledge_cancelled(options.cancellation);

  report_knowledge_progress(options.on_progress, "loaded", sources.size(), loaded_documents.size());
  UriContentHashDedupeStrategy default_dedupe_strategy;
  const auto& dedupe_strategy = options.dedupe_strategy ? *options.dedupe_strategy : default_dedupe_strategy;
  auto deduped = dedupe_strategy.dedupe(KnowledgeDedupeContext{knowledge_base_,
                                                               sources,
                                                               loaded_documents,
                                                               options.replace_existing});
  report_knowledge_progress(options.on_progress, "deduping", sources.size(), deduped.size());

  SkipUnchangedKnowledgeIncrementalStrategy default_incremental_strategy;
  const auto& incremental_strategy =
      options.incremental_strategy ? *options.incremental_strategy : default_incremental_strategy;
  auto incremental = incremental_strategy.prepare(KnowledgeIncrementalContext{knowledge_base_,
                                                                             sources,
                                                                             loaded_documents,
                                                                             deduped,
                                                                             options.replace_existing,
                                                                             options.skip_if_unchanged});
  KnowledgeIngestionPipelineOptions ingest_options = options;
  ingest_options.replace_existing = incremental.replace_existing;
  ingest_options.skip_if_unchanged = incremental.skip_if_unchanged;
  return ingest_loaded_document_batches(knowledge_base_, sources.size(), incremental.documents.size(),
                                        incremental.documents, ingest_options);
}

KnowledgeSyncState InMemoryKnowledgeSyncStateStore::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void InMemoryKnowledgeSyncStateStore::save(const KnowledgeSyncState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

FileKnowledgeSyncStateStore::FileKnowledgeSyncStateStore(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

KnowledgeSyncState FileKnowledgeSyncStateStore::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  KnowledgeSyncState state;
  if (!std::filesystem::exists(file_path_)) {
    return state;
  }
  const auto raw = read_json_file(file_path_);
  for (const auto& [uri, hash] : raw.at("documentHashes").as_object()) {
    state.document_hashes[uri] = hash.as_string();
  }
  state.synced_at = raw.at("syncedAt").as_string();
  return state;
}

void FileKnowledgeSyncStateStore::save(const KnowledgeSyncState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  Value hashes = Value::object({});
  for (const auto& [uri, hash] : state.document_hashes) {
    hashes[uri] = hash;
  }
  write_json_file(file_path_, Value::object({{"documentHashes", hashes}, {"syncedAt", state.synced_at}}));
}

const std::filesystem::path& FileKnowledgeSyncStateStore::file_path() const noexcept {
  return file_path_;
}

KnowledgeSyncJob::KnowledgeSyncJob(KnowledgeBase& knowledge_base,
                                   const KnowledgeSourceLoader& loader,
                                   KnowledgeSyncStateStore& state_store)
    : knowledge_base_(knowledge_base), loader_(loader), state_store_(state_store) {}

KnowledgeSyncResult KnowledgeSyncJob::sync(const std::vector<Value>& sources, KnowledgeSyncOptions options) const {
  throw_if_knowledge_cancelled(options.cancellation);
  report_knowledge_progress(options.on_progress, "loading", sources.size(), 0);
  FixedKnowledgeRetryStrategy default_retry_strategy(options.max_attempts, options.retry_delay_ms);
  const auto& retry_strategy = options.retry_strategy ? *options.retry_strategy : default_retry_strategy;
  auto loaded_documents = run_with_knowledge_retry([&]() {
    return load_knowledge_sources(sources, loader_);
  }, retry_strategy, KnowledgeRetryContext{knowledge_base_, "loading"}, options.cancellation);
  throw_if_knowledge_cancelled(options.cancellation);

  report_knowledge_progress(options.on_progress, "loaded", sources.size(), loaded_documents.size());
  UriContentHashDedupeStrategy default_dedupe_strategy;
  const auto& dedupe_strategy = options.dedupe_strategy ? *options.dedupe_strategy : default_dedupe_strategy;
  auto deduped = dedupe_strategy.dedupe(KnowledgeDedupeContext{knowledge_base_,
                                                               sources,
                                                               loaded_documents,
                                                               options.replace_existing});
  report_knowledge_progress(options.on_progress, "deduping", sources.size(), deduped.size());

  const auto previous_state = state_store_.load();
  std::map<std::string, std::string> next_hashes;
  for (const auto& document : deduped) {
    next_hashes[document.uri] = hash_knowledge_content(document.content);
  }

  std::vector<LoadedKnowledgeDocument> changed_documents;
  for (const auto& document : deduped) {
    const auto hash = next_hashes.at(document.uri);
    const auto found = previous_state.document_hashes.find(document.uri);
    if (found == previous_state.document_hashes.end() || found->second != hash) {
      changed_documents.push_back(document);
    }
  }

  std::vector<std::string> deleted_uris;
  if (options.delete_missing) {
    for (const auto& [uri, _] : previous_state.document_hashes) {
      throw_if_knowledge_cancelled(options.cancellation);
      if (!next_hashes.contains(uri)) {
        deleted_uris.push_back(uri);
      }
    }
    if (!deleted_uris.empty()) {
      throw_if_knowledge_cancelled(options.cancellation);
      KnowledgeDeleteOptions delete_options;
      delete_options.uris = deleted_uris;
      knowledge_base_.delete_documents(delete_options);
    }
  }

  options.replace_existing = true;
  options.skip_if_unchanged = true;
  auto aggregate = ingest_loaded_document_batches(knowledge_base_, sources.size(), deduped.size(),
                                                  changed_documents, options);

  KnowledgeSyncState next_state;
  next_state.document_hashes = std::move(next_hashes);
  next_state.synced_at = now_iso8601();
  throw_if_knowledge_cancelled(options.cancellation);
  state_store_.save(next_state);

  KnowledgeSyncResult result;
  result.knowledge_base_id = aggregate.knowledge_base_id;
  result.tenant_id = aggregate.tenant_id;
  result.document_count = aggregate.document_count;
  result.chunk_count = aggregate.chunk_count;
  result.documents = std::move(aggregate.documents);
  result.chunks = std::move(aggregate.chunks);
  result.loaded_document_count = deduped.size();
  result.changed_document_count = changed_documents.size();
  result.skipped_document_count = deduped.size() - changed_documents.size();
  result.deleted_document_count = deleted_uris.size();
  result.deleted_uris = std::move(deleted_uris);
  result.synced_at = next_state.synced_at;
  return result;
}

namespace {

Value knowledge_base_definition_to_value(const KnowledgeBaseDefinition& definition) {
  return Value::object({{"id", definition.id},
                        {"tenantId", definition.tenant_id},
                        {"title", definition.title},
                        {"description", definition.description.empty() ? Value() : Value(definition.description)},
                        {"storeFilePath", definition.store_file_path.empty() ? Value() : Value(definition.store_file_path)}});
}

KnowledgeBaseDefinition knowledge_base_definition_from_value(const Value& value) {
  KnowledgeBaseDefinition definition;
  definition.id = value.at("id").as_string();
  definition.tenant_id = value.at("tenantId").as_string("default");
  definition.title = value.at("title").as_string(definition.id);
  definition.description = value.at("description").as_string();
  definition.store_file_path = value.at("storeFilePath").as_string();
  return definition;
}

Value knowledge_hits_to_value(const std::vector<KnowledgeSearchHit>& hits) {
  Value::Array values;
  for (const auto& hit : hits) {
    values.push_back(knowledge_search_hit_to_value(hit));
  }
  return Value(values);
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
    store = std::make_shared<FileKnowledgeStore>(definition->second.store_file_path);
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
      std::filesystem::remove_all(std::filesystem::path(store_path).parent_path());
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
  return base_dir_ / tenant_id / id / "store.json";
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
}  // namespace agent
