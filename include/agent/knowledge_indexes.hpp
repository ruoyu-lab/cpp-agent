#pragma once

#include "agent/http.hpp"
#include "agent/knowledge_loaders.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

struct KnowledgeDeleteOptions {
  std::vector<std::string> document_ids;
  std::vector<std::string> uris;
  std::string uri_prefix;
  std::vector<std::string> source_types;
  Value metadata = Value::object({});
};

struct KnowledgeDeleteResult {
  std::string knowledge_base_id;
  std::string tenant_id;
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
};

struct KnowledgeTextMatch {
  std::string chunk_id;
  double score = 0;
};

struct KnowledgeVectorMatch {
  std::string chunk_id;
  double score = 0;
};

struct KnowledgeTextSearchOptions {
  std::string knowledge_base_id;
  std::string tenant_id = "default";
  std::string query;
  std::size_t top_k = 12;
  std::vector<std::string> source_types;
  std::vector<KnowledgeAssetType> asset_types;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
  std::string space_id;
  Value metadata = Value::object({});
};

struct KnowledgeTextIndexClearOptions {
  std::string knowledge_base_id;
  std::string tenant_id;
};

struct KnowledgeTextIndexStats {
  std::size_t chunk_count = 0;
};

struct KnowledgeVectorSearchOptions {
  std::string knowledge_base_id;
  std::string tenant_id = "default";
  EmbeddingVector embedding;
  std::size_t top_k = 12;
  double min_score = 0.0;
  std::vector<std::string> source_types;
  std::vector<KnowledgeAssetType> asset_types;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
  std::string space_id;
  Value metadata = Value::object({});
};

struct KnowledgeVectorIndexClearOptions {
  std::string knowledge_base_id;
  std::string tenant_id;
};

struct KnowledgeVectorIndexStats {
  std::size_t chunk_count = 0;
  std::string namespace_id;
  std::map<std::string, std::size_t> asset_type_counts;
};

class KnowledgeTextIndex {
 public:
  virtual ~KnowledgeTextIndex() = default;
  virtual void upsert(const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const = 0;
  virtual void delete_chunks(const std::vector<std::string>& chunk_ids) = 0;
  virtual void clear(const KnowledgeTextIndexClearOptions& options = {}) = 0;
  [[nodiscard]] virtual KnowledgeTextIndexStats stats() const = 0;
};

class InMemoryKnowledgeTextIndex : public KnowledgeTextIndex {
 public:
  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeTextIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeTextIndexStats stats() const override;

 protected:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkRecord> chunks_;
};

class MiniSearchKnowledgeTextIndex : public InMemoryKnowledgeTextIndex {
 public:
  [[nodiscard]] std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const override;
};

class KnowledgeTextIndexRegistry;

struct KnowledgeTextIndexProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeTextIndex>(
      const Value& options,
      const KnowledgeTextIndexRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeTextIndexRegistry {
 public:
  using Factory = std::function<std::shared_ptr<KnowledgeTextIndex>()>;

  explicit KnowledgeTextIndexRegistry(std::map<std::string, Factory> factories = {});
  explicit KnowledgeTextIndexRegistry(std::vector<KnowledgeTextIndexProvider> providers);
  KnowledgeTextIndexRegistry(const KnowledgeTextIndexRegistry& other);
  KnowledgeTextIndexRegistry& operator=(const KnowledgeTextIndexRegistry& other);
  KnowledgeTextIndexRegistry(KnowledgeTextIndexRegistry&& other) noexcept;
  KnowledgeTextIndexRegistry& operator=(KnowledgeTextIndexRegistry&& other) noexcept;
  KnowledgeTextIndexProvider& register_provider(KnowledgeTextIndexProvider provider);
  void register_index(std::string name, Factory factory);
  [[nodiscard]] const KnowledgeTextIndexProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeTextIndex> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeTextIndexProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeTextIndexProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeTextIndexRegistry create_default_knowledge_text_index_registry();

class KnowledgeVectorIndex {
 public:
  virtual ~KnowledgeVectorIndex() = default;
  virtual void upsert(const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const = 0;
  virtual void delete_chunks(const std::vector<std::string>& chunk_ids) = 0;
  virtual void clear(const KnowledgeVectorIndexClearOptions& options = {}) = 0;
  [[nodiscard]] virtual KnowledgeVectorIndexStats stats() const = 0;
};

class InMemoryKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 protected:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkRecord> chunks_;
};

struct SqliteKnowledgeVectorIndexConfig {
  std::filesystem::path file_path;
  int dimensions = 0;
  std::string table_name = "knowledge_vectors";
  bool create_table = true;
  std::string namespace_id;
  std::size_t batch_size = 200;
};

class SqliteKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit SqliteKnowledgeVectorIndex(SqliteKnowledgeVectorIndexConfig config);
  ~SqliteKnowledgeVectorIndex() override = default;

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  struct StoredChunk {
    std::string namespace_id;
    KnowledgeChunkRecord chunk;
  };

  void load() const;
  void save() const;

  SqliteKnowledgeVectorIndexConfig config_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::map<std::string, StoredChunk> chunks_;
};

struct QdrantKnowledgeVectorIndexConfig {
  std::string base_url;
  std::string collection;
  std::string api_key;
  std::map<std::string, std::string> headers;
  std::string vector_name;
  bool create_collection = false;
  std::string distance = "Cosine";
  int dimensions = 0;
  bool wait = true;
  std::size_t oversample_factor = 4;
  HttpTransport transport;
};

class QdrantKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit QdrantKnowledgeVectorIndex(QdrantKnowledgeVectorIndexConfig config);

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  [[nodiscard]] std::string path(std::string suffix) const;
  [[nodiscard]] std::map<std::string, std::string> headers() const;
  void ensure_collection(int dimensions = 0) const;
  [[nodiscard]] Value vector_payload(const EmbeddingVector& embedding) const;

  QdrantKnowledgeVectorIndexConfig config_;
  mutable std::mutex collection_mutex_;
  mutable bool collection_ready_ = false;
};

struct PgVectorQuery {
  std::string sql;
  std::vector<Value> params;
};

struct PgVectorQueryResult {
  std::vector<std::map<std::string, Value>> rows;
};

using PgVectorQueryClient = std::function<PgVectorQueryResult(const PgVectorQuery&)>;

struct PgVectorKnowledgeVectorIndexConfig {
  PgVectorQueryClient client;
  std::string schema_name = "public";
  std::string table_name = "node_agent_knowledge_chunks";
  bool create_table = false;
  int dimensions = 0;
};

class PgVectorKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit PgVectorKnowledgeVectorIndex(PgVectorKnowledgeVectorIndexConfig config);

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  [[nodiscard]] std::string qualified_table() const;
  void ensure_ready(int dimensions = 0) const;
  [[nodiscard]] PgVectorQueryResult query(std::string sql, std::vector<Value> params = {}) const;

  PgVectorKnowledgeVectorIndexConfig config_;
  mutable std::mutex ready_mutex_;
  mutable bool ready_ = false;
};

class KnowledgeVectorIndexRegistry;

struct KnowledgeVectorIndexProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeVectorIndex>(
      const Value& options,
      const KnowledgeVectorIndexRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeVectorIndexRegistry {
 public:
  using Factory = std::function<std::shared_ptr<KnowledgeVectorIndex>()>;

  explicit KnowledgeVectorIndexRegistry(std::map<std::string, Factory> factories = {});
  explicit KnowledgeVectorIndexRegistry(std::vector<KnowledgeVectorIndexProvider> providers);
  KnowledgeVectorIndexRegistry(const KnowledgeVectorIndexRegistry& other);
  KnowledgeVectorIndexRegistry& operator=(const KnowledgeVectorIndexRegistry& other);
  KnowledgeVectorIndexRegistry(KnowledgeVectorIndexRegistry&& other) noexcept;
  KnowledgeVectorIndexRegistry& operator=(KnowledgeVectorIndexRegistry&& other) noexcept;
  KnowledgeVectorIndexProvider& register_provider(KnowledgeVectorIndexProvider provider);
  void register_index(std::string name, Factory factory);
  [[nodiscard]] const KnowledgeVectorIndexProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeVectorIndex> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeVectorIndexProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeVectorIndexProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeVectorIndexRegistry create_default_knowledge_vector_index_registry();

}  // namespace agent
