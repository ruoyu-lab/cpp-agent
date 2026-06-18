#pragma once

#include "agent/model.hpp"

#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agent {

enum class KnowledgeAssetType {
  Text,
  Image,
};

struct KnowledgeDocumentRecord {
  std::string id;
  std::string knowledge_base_id;
  std::string tenant_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::string content;
  std::optional<MediaSource> media;
  std::string text_hint;
  Value metadata = Value::object({});
  std::string created_at;
  std::string updated_at;
};

struct KnowledgeChunkRecord {
  std::string id;
  std::string document_id;
  std::string knowledge_base_id;
  std::string tenant_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::string content;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  std::optional<MediaSource> media;
  std::string embedding_space_id;
  Value metadata = Value::object({});
  EmbeddingVector embedding;
  std::string created_at;
  std::string updated_at;
};

struct KnowledgeCitation {
  std::string knowledge_base_id;
  std::string knowledge_base_title;
  std::string tenant_id;
  std::string document_id;
  std::string chunk_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  double score = 0;
  double vector_score = 0;
  double lexical_score = 0;
  std::optional<MediaSource> media;
  std::string embedding_space_id;
  Value metadata = Value::object({});
  std::string snippet;
};

struct KnowledgeSearchHit {
  KnowledgeDocumentRecord document;
  KnowledgeChunkRecord chunk;
  double score = 0;
  double vector_score = 0;
  double lexical_score = 0;
  std::optional<double> rerank_score;
  KnowledgeCitation citation;
};

struct KnowledgeSearchResult {
  std::vector<KnowledgeSearchHit> hits;
  Value debug = Value::object({});
};

struct KnowledgeContextResult {
  std::vector<KnowledgeSearchHit> hits;
  std::optional<AgentMessage> message;
  Value debug = Value::object({});
};

struct KnowledgeSearchOptions {
  std::size_t top_k = 0;
  std::size_t vector_top_k = 0;
  std::size_t lexical_top_k = 0;
  double min_score = std::numeric_limits<double>::quiet_NaN();
  double hybrid_alpha = std::numeric_limits<double>::quiet_NaN();
  std::size_t rerank_top_k = 0;
  std::string retrieval_mode;
  double oversample_factor = 0.0;
  std::string fusion;
  std::map<KnowledgeAssetType, double> modality_weights;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<KnowledgeAssetType> asset_types;
  std::string space_id;
  std::vector<std::string> source_types;
  std::vector<std::string> chunk_ids;
  Value metadata = Value::object({});
  std::string tenant_id;
  std::vector<std::string> knowledge_base_ids;
  CancellationToken* cancellation = nullptr;
};

Value knowledge_citation_to_value(const KnowledgeCitation& citation);
Value knowledge_search_hit_to_value(const KnowledgeSearchHit& hit);

class KnowledgeContextProvider {
 public:
  virtual ~KnowledgeContextProvider() = default;
  [[nodiscard]] virtual std::string knowledge_context_provider_name() const {
    return "knowledge";
  }
  virtual KnowledgeContextResult build_context_message(
      const std::string& query,
      KnowledgeSearchOptions options = {}) = 0;
  virtual KnowledgeContextResult build_context_message(
      const ImageEmbeddingInput& query,
      KnowledgeSearchOptions options = {}) = 0;
  virtual std::vector<KnowledgeSearchHit> search(
      const std::string& query,
      KnowledgeSearchOptions options = {});
};

}  // namespace agent
