#include "agent/knowledge_runtime.hpp"
#include "detail/helpers.hpp"

#include <utility>

namespace agent {
namespace {

std::string knowledge_asset_type_label(KnowledgeAssetType type) {
  switch (type) {
    case KnowledgeAssetType::Image:
      return "image";
    case KnowledgeAssetType::Text:
      return "text";
  }
  return "text";
}

Value embedding_to_value(const EmbeddingVector& embedding) {
  Value::Array values;
  for (const auto value : embedding) {
    values.emplace_back(value);
  }
  return Value(values);
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

}  // namespace

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

std::vector<KnowledgeSearchHit> KnowledgeContextProvider::search(
    const std::string& query,
    KnowledgeSearchOptions options) {
  return build_context_message(query, std::move(options)).hits;
}

}  // namespace agent
