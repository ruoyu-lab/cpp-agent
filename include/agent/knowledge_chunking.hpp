#pragma once

#include "agent/knowledge_loaders.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

struct ChunkDraft {
  std::string content;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  Value metadata = Value::object({});
};

class KnowledgeChunker {
 public:
  virtual ~KnowledgeChunker() = default;
  [[nodiscard]] virtual std::vector<ChunkDraft> chunk(const LoadedKnowledgeDocument& document) const = 0;
};

class RecursiveTextChunker : public KnowledgeChunker {
 public:
  RecursiveTextChunker(std::size_t chunk_size = 1200, std::size_t chunk_overlap = 180,
                       std::size_t code_chunk_lines = 80, std::size_t code_chunk_overlap_lines = 12);
  [[nodiscard]] std::vector<ChunkDraft> chunk(const LoadedKnowledgeDocument& document) const override;

 private:
  std::size_t chunk_size_;
  std::size_t chunk_overlap_;
  std::size_t code_chunk_lines_;
  std::size_t code_chunk_overlap_lines_;
};

class KnowledgeChunkerRegistry;

struct KnowledgeChunkerProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeChunker>(
      const Value& options,
      const KnowledgeChunkerRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeChunkerRegistry {
 public:
  explicit KnowledgeChunkerRegistry(std::vector<KnowledgeChunkerProvider> providers = {});
  KnowledgeChunkerRegistry(const KnowledgeChunkerRegistry& other);
  KnowledgeChunkerRegistry& operator=(const KnowledgeChunkerRegistry& other);
  KnowledgeChunkerRegistry(KnowledgeChunkerRegistry&& other) noexcept;
  KnowledgeChunkerRegistry& operator=(KnowledgeChunkerRegistry&& other) noexcept;
  KnowledgeChunkerProvider& register_provider(KnowledgeChunkerProvider provider);
  [[nodiscard]] const KnowledgeChunkerProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeChunker> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeChunkerProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkerProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeChunkerRegistry create_default_knowledge_chunker_registry();

KnowledgeSearchOptions default_knowledge_search_options();
KnowledgeSearchOptions merge_knowledge_search_options(const KnowledgeSearchOptions& base,
                                                      const KnowledgeSearchOptions& override);
KnowledgeSearchOptions effective_knowledge_search_options(const KnowledgeSearchOptions& search_defaults,
                                                          const KnowledgeSearchOptions& options);

}  // namespace agent
