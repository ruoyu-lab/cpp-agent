#pragma once

#include "agent/knowledge_chunking.hpp"
#include "agent/knowledge_indexes.hpp"
#include "agent/knowledge_rerankers.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct KnowledgeIngestionResult {
  std::string knowledge_base_id;
  std::string tenant_id;
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
  std::vector<KnowledgeDocumentRecord> documents;
  std::vector<KnowledgeChunkRecord> chunks;
};

struct KnowledgeIngestOptions {
  bool replace_existing = true;
  bool skip_if_unchanged = true;
  std::size_t embedding_batch_size = 0;
  CancellationToken* cancellation = nullptr;
};

class KnowledgeBase;

struct KnowledgeIngestionStrategyContext {
  KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const KnowledgeSourceLoader& loader;
  KnowledgeIngestOptions options;
};

class KnowledgeIngestionStrategy {
 public:
  virtual ~KnowledgeIngestionStrategy() = default;
  [[nodiscard]] virtual KnowledgeIngestionResult ingest(
      const KnowledgeIngestionStrategyContext& context) const = 0;
};

class DefaultKnowledgeIngestionStrategy : public KnowledgeIngestionStrategy {
 public:
  [[nodiscard]] KnowledgeIngestionResult ingest(
      const KnowledgeIngestionStrategyContext& context) const override;
};

class KnowledgeRetrievalStrategy {
 public:
  virtual ~KnowledgeRetrievalStrategy() = default;
  [[nodiscard]] virtual KnowledgeSearchResult search(
      const KnowledgeBase& knowledge_base,
      const std::string& query,
      KnowledgeSearchOptions options) const = 0;
  [[nodiscard]] virtual KnowledgeSearchResult search(
      const KnowledgeBase& knowledge_base,
      const ImageEmbeddingInput& query,
      KnowledgeSearchOptions options) const = 0;
};

struct KnowledgeContextRenderContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<KnowledgeSearchHit>& hits;
};

class KnowledgeContextRenderer {
 public:
  virtual ~KnowledgeContextRenderer() = default;
  [[nodiscard]] virtual std::optional<AgentMessage> render(
      const KnowledgeContextRenderContext& context) const = 0;
};

class DefaultKnowledgeContextRenderer : public KnowledgeContextRenderer {
 public:
  [[nodiscard]] std::optional<AgentMessage> render(
      const KnowledgeContextRenderContext& context) const override;
};

std::string hash_knowledge_content(const std::string& content);
std::vector<LoadedKnowledgeDocument> dedupe_loaded_knowledge_documents(
    const std::vector<LoadedKnowledgeDocument>& documents,
    bool replace_existing = true);

struct KnowledgeStoreStats {
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
};

class KnowledgeStore {
 public:
  virtual ~KnowledgeStore() = default;
  virtual std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) = 0;
  virtual std::vector<KnowledgeChunkRecord> upsert_chunks(
      const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeDocumentRecord> list_documents() const = 0;
  [[nodiscard]] virtual std::vector<KnowledgeChunkRecord> list_chunks() const = 0;
  virtual std::size_t delete_documents(const std::vector<std::string>& ids) = 0;
  virtual std::size_t delete_chunks(const std::vector<std::string>& ids) = 0;
  virtual void clear() = 0;
  [[nodiscard]] virtual KnowledgeStoreStats stats() const = 0;
};

class InMemoryKnowledgeStore : public KnowledgeStore {
 public:
  std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) override;
  std::vector<KnowledgeChunkRecord> upsert_chunks(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeDocumentRecord> list_documents() const override;
  [[nodiscard]] std::vector<KnowledgeChunkRecord> list_chunks() const override;
  std::size_t delete_documents(const std::vector<std::string>& ids) override;
  std::size_t delete_chunks(const std::vector<std::string>& ids) override;
  void clear() override;
  [[nodiscard]] KnowledgeStoreStats stats() const override;

 protected:
  mutable std::recursive_mutex mutex_;
  mutable std::map<std::string, KnowledgeDocumentRecord> documents_;
  mutable std::map<std::string, KnowledgeChunkRecord> chunks_;
};

class FileKnowledgeStore : public InMemoryKnowledgeStore {
 public:
  explicit FileKnowledgeStore(std::filesystem::path file_path);

  std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) override;
  std::vector<KnowledgeChunkRecord> upsert_chunks(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeDocumentRecord> list_documents() const override;
  [[nodiscard]] std::vector<KnowledgeChunkRecord> list_chunks() const override;
  std::size_t delete_documents(const std::vector<std::string>& ids) override;
  std::size_t delete_chunks(const std::vector<std::string>& ids) override;
  void clear() override;
  [[nodiscard]] KnowledgeStoreStats stats() const override;
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

 private:
  void ensure_loaded() const;
  void persist() const;

  std::filesystem::path file_path_;
  mutable bool loaded_ = false;
};

class KnowledgeBase : public KnowledgeContextProvider {
 public:
  KnowledgeBase(std::string id, std::string tenant_id = "default", std::string title = {},
                std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>(),
                RecursiveTextChunker chunker = {},
                std::shared_ptr<KnowledgeReranker> reranker = std::make_shared<HeuristicKnowledgeReranker>(),
                std::shared_ptr<KnowledgeStore> store = std::make_shared<InMemoryKnowledgeStore>(),
                std::shared_ptr<ImageEmbeddingAdapter> image_embedder =
                    std::make_shared<HashImageEmbeddingAdapter>(),
                std::shared_ptr<KnowledgeTextIndex> text_index = std::make_shared<InMemoryKnowledgeTextIndex>(),
                std::shared_ptr<KnowledgeVectorIndex> vector_index = {},
                KnowledgeSearchOptions search_defaults = {},
                std::string description = {},
                std::string context_title = {},
                std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy = {},
                std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy = {},
                std::shared_ptr<KnowledgeContextRenderer> context_renderer = {});

  KnowledgeIngestionResult ingest_loaded_documents(const std::vector<LoadedKnowledgeDocument>& documents,
                                                   KnowledgeIngestOptions options = {});
  KnowledgeIngestionResult ingest_loaded_documents(const std::vector<LoadedKnowledgeDocument>& documents,
                                                   bool replace_existing);
  KnowledgeIngestionResult ingest(const std::vector<Value>& sources, const KnowledgeSourceLoader& loader,
                                  KnowledgeIngestOptions options = {});
  KnowledgeDeleteResult delete_documents(const KnowledgeDeleteOptions& options);
  std::vector<KnowledgeSearchHit> search(const std::string& query, KnowledgeSearchOptions options = {}) override;
  std::vector<KnowledgeSearchHit> search(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {});
  KnowledgeSearchResult search_with_debug(const std::string& query, KnowledgeSearchOptions options = {});
  KnowledgeSearchResult search_with_debug(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {});
  std::optional<AgentMessage> create_context_message(const std::vector<KnowledgeSearchHit>& hits) const;
  KnowledgeContextResult build_context_message(const std::string& query, KnowledgeSearchOptions options = {}) override;
  KnowledgeContextResult build_context_message(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {}) override;
  [[nodiscard]] KnowledgeStoreStats stats() const;
  [[nodiscard]] std::shared_ptr<KnowledgeStore> store() const noexcept;

  [[nodiscard]] const std::string& id() const noexcept;
  [[nodiscard]] const std::string& tenant_id() const noexcept;
  [[nodiscard]] const std::string& title() const noexcept;
  [[nodiscard]] const std::string& description() const noexcept;
  [[nodiscard]] const std::string& context_title() const noexcept;
  [[nodiscard]] std::string knowledge_context_provider_name() const override {
    return "knowledge-base";
  }

 private:
  mutable std::recursive_mutex mutex_;
  std::string id_;
  std::string tenant_id_;
  std::string title_;
  std::string description_;
  std::string context_title_;
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder_;
  RecursiveTextChunker chunker_;
  std::shared_ptr<KnowledgeReranker> reranker_;
  std::shared_ptr<KnowledgeStore> store_;
  std::shared_ptr<KnowledgeTextIndex> text_index_;
  std::shared_ptr<KnowledgeVectorIndex> vector_index_;
  std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy_;
  std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy_;
  std::shared_ptr<KnowledgeContextRenderer> context_renderer_;
  KnowledgeSearchOptions search_defaults_;
  bool text_index_ready_ = false;
  bool vector_index_ready_ = false;

  std::vector<KnowledgeSearchHit> search_with_embedding(const std::string& query_text,
                                                        const EmbeddingVector& query_vector,
                                                        KnowledgeSearchOptions options,
                                                        Value* debug);
  void rebuild_text_index();
  void ensure_text_index_ready();
  void rebuild_vector_index();
  void ensure_vector_index_ready();
};

}  // namespace agent
