#pragma once

#include "agent/knowledge_ingestion.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct KnowledgeBaseDefinition {
  std::string id;
  std::string tenant_id = "default";
  std::string title;
  std::string description;
  std::string store_file_path;
};

struct ManagedKnowledgeBaseConfig {
  std::string id;
  std::string tenant_id = "default";
  std::string title;
  std::string description;
  std::optional<bool> persistent;
  std::shared_ptr<KnowledgeStore> store;
  std::shared_ptr<TextEmbeddingAdapter> embedder;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder;
  RecursiveTextChunker chunker = {};
  std::shared_ptr<KnowledgeReranker> reranker;
  std::shared_ptr<KnowledgeTextIndex> text_index;
  std::shared_ptr<KnowledgeVectorIndex> vector_index;
  std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy;
  std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy;
  std::shared_ptr<KnowledgeContextRenderer> context_renderer;
  KnowledgeSearchOptions search_defaults;
  KnowledgeSearchOptions retrieval_config;
  std::string context_title;
};

struct ManagerSearchOptions : KnowledgeSearchOptions {
  bool enabled = true;
};

struct KnowledgeManagerSearchResult {
  std::vector<KnowledgeSearchHit> hits;
  Value debug = Value::object({});
};

struct KnowledgeManagerContextResult : KnowledgeContextResult {};

class KnowledgeBaseManager : public KnowledgeContextProvider {
 public:
  using KnowledgeContextProvider::search;

  explicit KnowledgeBaseManager(std::filesystem::path base_dir = {},
                                std::shared_ptr<TextEmbeddingAdapter> embedder =
                                    std::make_shared<HashEmbeddingAdapter>(),
                                std::shared_ptr<ImageEmbeddingAdapter> image_embedder =
                                    std::make_shared<HashImageEmbeddingAdapter>(),
                                std::shared_ptr<KnowledgeSourceLoader> loader =
                                    std::make_shared<CompositeKnowledgeLoader>(),
                                std::shared_ptr<KnowledgeReranker> reranker =
                                    std::make_shared<HeuristicKnowledgeReranker>(),
                                std::shared_ptr<KnowledgeTextIndex> text_index =
                                    std::make_shared<InMemoryKnowledgeTextIndex>(),
                                std::shared_ptr<KnowledgeVectorIndex> vector_index = {},
                                RecursiveTextChunker chunker = {});

  [[nodiscard]] std::filesystem::path manifest_path() const;
  [[nodiscard]] std::vector<KnowledgeBaseDefinition> load_definitions();
  [[nodiscard]] std::vector<KnowledgeBaseDefinition> list_knowledge_bases(
      std::string tenant_id = {});
  std::shared_ptr<KnowledgeBase> create_knowledge_base(ManagedKnowledgeBaseConfig config);
  std::shared_ptr<KnowledgeBase> register_knowledge_base(std::shared_ptr<KnowledgeBase> base,
                                                         std::string description = {});
  [[nodiscard]] std::shared_ptr<KnowledgeBase> get_knowledge_base(const std::string& id,
                                                                  std::string tenant_id = "default");
  bool delete_knowledge_base(const std::string& id, std::string tenant_id = "default");
  KnowledgeIngestionResult ingest(const std::string& knowledge_base_id,
                                  const std::vector<Value>& sources,
                                  std::string tenant_id = "default",
                                  KnowledgeIngestionPipelineOptions options = {});
  KnowledgeDeleteResult delete_documents(const std::string& knowledge_base_id,
                                         const KnowledgeDeleteOptions& options,
                                         std::string tenant_id = "default");
  KnowledgeManagerSearchResult search_with_debug(const std::string& query,
                                                 ManagerSearchOptions options = {});
  KnowledgeManagerSearchResult search_with_debug(const ImageEmbeddingInput& query,
                                                 ManagerSearchOptions options = {});
  std::vector<KnowledgeSearchHit> search(const std::string& query,
                                         KnowledgeSearchOptions options) override;
  std::vector<KnowledgeSearchHit> search(const std::string& query,
                                         ManagerSearchOptions options = {});
  std::vector<KnowledgeSearchHit> search(const ImageEmbeddingInput& query,
                                         ManagerSearchOptions options = {});
  std::optional<AgentMessage> create_context_message(const std::vector<KnowledgeSearchHit>& hits) const;
  [[nodiscard]] std::string knowledge_context_provider_name() const override {
    return "knowledge-base-manager";
  }
  KnowledgeContextResult build_context_message(const std::string& query,
                                               KnowledgeSearchOptions options) override;
  KnowledgeContextResult build_context_message(const ImageEmbeddingInput& query,
                                               KnowledgeSearchOptions options) override;
  KnowledgeManagerContextResult build_context_message(const std::string& query,
                                                      ManagerSearchOptions options = {});
  KnowledgeManagerContextResult build_context_message(const ImageEmbeddingInput& query,
                                                      ManagerSearchOptions options = {});

 private:
  [[nodiscard]] std::string key(const std::string& tenant_id, const std::string& id) const;
  [[nodiscard]] std::filesystem::path store_file_path(const std::string& tenant_id,
                                                      const std::string& id) const;
  void persist_definitions() const;

  std::filesystem::path base_dir_;
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder_;
  std::shared_ptr<KnowledgeSourceLoader> loader_;
  std::shared_ptr<KnowledgeReranker> reranker_;
  std::shared_ptr<KnowledgeTextIndex> text_index_;
  std::shared_ptr<KnowledgeVectorIndex> vector_index_;
  RecursiveTextChunker chunker_;
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::shared_ptr<KnowledgeBase>> bases_;
  std::map<std::string, KnowledgeBaseDefinition> definitions_;
};

}  // namespace agent
