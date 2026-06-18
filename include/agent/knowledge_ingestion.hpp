#pragma once

#include "agent/knowledge_base.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

struct KnowledgeIngestionProgress {
  std::string phase;
  std::size_t source_count = 0;
  std::size_t loaded_document_count = 0;
  std::size_t processed_document_count = 0;
  std::size_t processed_chunk_count = 0;
  std::size_t batch_index = 0;
  std::size_t total_batches = 0;
};

using KnowledgeIngestionProgressCallback = std::function<void(const KnowledgeIngestionProgress&)>;

struct KnowledgeDedupeContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const std::vector<LoadedKnowledgeDocument>& loaded_documents;
  bool replace_existing = true;
};

struct KnowledgeIncrementalContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const std::vector<LoadedKnowledgeDocument>& loaded_documents;
  const std::vector<LoadedKnowledgeDocument>& documents;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
};

struct KnowledgeIncrementalResult {
  std::vector<LoadedKnowledgeDocument> documents;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
};

class KnowledgeDedupeStrategy {
 public:
  virtual ~KnowledgeDedupeStrategy() = default;
  [[nodiscard]] virtual std::vector<LoadedKnowledgeDocument> dedupe(
      const KnowledgeDedupeContext& context) const = 0;
};

class UriContentHashDedupeStrategy : public KnowledgeDedupeStrategy {
 public:
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> dedupe(
      const KnowledgeDedupeContext& context) const override;
};

class KnowledgeIncrementalStrategy {
 public:
  virtual ~KnowledgeIncrementalStrategy() = default;
  [[nodiscard]] virtual KnowledgeIncrementalResult prepare(
      const KnowledgeIncrementalContext& context) const = 0;
};

class SkipUnchangedKnowledgeIncrementalStrategy : public KnowledgeIncrementalStrategy {
 public:
  [[nodiscard]] KnowledgeIncrementalResult prepare(
      const KnowledgeIncrementalContext& context) const override;
};

struct KnowledgeRetryContext {
  const KnowledgeBase& knowledge_base;
  std::string phase;
  std::size_t batch_index = 0;
  std::size_t total_batches = 0;
};

class KnowledgeRetryStrategy {
 public:
  virtual ~KnowledgeRetryStrategy() = default;
  [[nodiscard]] virtual std::size_t max_attempts(const KnowledgeRetryContext& context) const = 0;
  [[nodiscard]] virtual bool should_retry(const std::exception& error,
                                          const KnowledgeRetryContext& context,
                                          std::size_t attempt,
                                          std::size_t max_attempts) const = 0;
  [[nodiscard]] virtual int retry_delay_ms(const std::exception& error,
                                           const KnowledgeRetryContext& context,
                                           std::size_t attempt) const = 0;
};

class FixedKnowledgeRetryStrategy : public KnowledgeRetryStrategy {
 public:
  explicit FixedKnowledgeRetryStrategy(std::size_t max_attempts = 2, int delay_ms = 0);
  [[nodiscard]] std::size_t max_attempts(const KnowledgeRetryContext& context) const override;
  [[nodiscard]] bool should_retry(const std::exception& error,
                                  const KnowledgeRetryContext& context,
                                  std::size_t attempt,
                                  std::size_t max_attempts) const override;
  [[nodiscard]] int retry_delay_ms(const std::exception& error,
                                   const KnowledgeRetryContext& context,
                                   std::size_t attempt) const override;

 private:
  std::size_t max_attempts_;
  int delay_ms_;
};

struct KnowledgeIngestionPipelineOptions {
  std::size_t document_batch_size = 16;
  std::size_t embedding_batch_size = 0;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
  std::size_t max_attempts = 2;
  int retry_delay_ms = 0;
  std::shared_ptr<KnowledgeDedupeStrategy> dedupe_strategy;
  std::shared_ptr<KnowledgeIncrementalStrategy> incremental_strategy;
  std::shared_ptr<KnowledgeRetryStrategy> retry_strategy;
  KnowledgeIngestionProgressCallback on_progress;
  CancellationToken* cancellation = nullptr;
};

class KnowledgeIngestionPipeline {
 public:
  KnowledgeIngestionPipeline(KnowledgeBase& knowledge_base, const KnowledgeSourceLoader& loader);
  KnowledgeIngestionResult ingest(const std::vector<Value>& sources,
                                  KnowledgeIngestionPipelineOptions options = {}) const;

 private:
  KnowledgeBase& knowledge_base_;
  const KnowledgeSourceLoader& loader_;
};

struct KnowledgeSyncState {
  std::map<std::string, std::string> document_hashes;
  std::string synced_at;
};

class KnowledgeSyncStateStore {
 public:
  virtual ~KnowledgeSyncStateStore() = default;
  [[nodiscard]] virtual KnowledgeSyncState load() const = 0;
  virtual void save(const KnowledgeSyncState& state) = 0;
};

class InMemoryKnowledgeSyncStateStore : public KnowledgeSyncStateStore {
 public:
  [[nodiscard]] KnowledgeSyncState load() const override;
  void save(const KnowledgeSyncState& state) override;

 private:
  mutable std::mutex mutex_;
  KnowledgeSyncState state_;
};

class FileKnowledgeSyncStateStore : public KnowledgeSyncStateStore {
 public:
  explicit FileKnowledgeSyncStateStore(std::filesystem::path file_path);
  [[nodiscard]] KnowledgeSyncState load() const override;
  void save(const KnowledgeSyncState& state) override;
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

 private:
  std::filesystem::path file_path_;
  mutable std::mutex mutex_;
};

struct KnowledgeSyncOptions : KnowledgeIngestionPipelineOptions {
  bool delete_missing = false;
};

struct KnowledgeSyncResult : KnowledgeIngestionResult {
  std::size_t loaded_document_count = 0;
  std::size_t changed_document_count = 0;
  std::size_t skipped_document_count = 0;
  std::size_t deleted_document_count = 0;
  std::vector<std::string> deleted_uris;
  std::string synced_at;
};

class KnowledgeSyncJob {
 public:
  KnowledgeSyncJob(KnowledgeBase& knowledge_base,
                   const KnowledgeSourceLoader& loader,
                   KnowledgeSyncStateStore& state_store);
  KnowledgeSyncResult sync(const std::vector<Value>& sources, KnowledgeSyncOptions options = {}) const;

 private:
  KnowledgeBase& knowledge_base_;
  const KnowledgeSourceLoader& loader_;
  KnowledgeSyncStateStore& state_store_;
};

}  // namespace agent
