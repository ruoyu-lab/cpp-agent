#pragma once

#include "agent/memory_retrieval.hpp"
#include "agent/model.hpp"

namespace agent {

struct VectorMemoryRecord {
  std::string id;
  std::string content;
  EmbeddingVector embedding;
  Value metadata = Value::object({});
  std::string namespace_id = "default";
  std::string created_at;
  std::string updated_at;
};

struct VectorMemoryUpsertInput {
  std::string id;
  std::string content;
  EmbeddingVector embedding;
  Value metadata = Value::object({});
  std::string namespace_id;
};

struct VectorMemoryQuery {
  EmbeddingVector embedding;
  std::size_t top_k = 4;
  double min_score = 0.2;
  std::string namespace_id;
};

struct InMemoryVectorStoreConfig {
  std::string namespace_id = "default";
};

struct FileVectorStoreConfig {
  std::filesystem::path file_path;
  std::string namespace_id = "default";
};

struct RememberMemoryInput {
  std::string id;
  std::string content;
  Value metadata = Value::object({});
  std::string namespace_id;
};

struct RememberConversationPlanStep {
  std::string title;
};

struct RememberConversationPlan {
  std::vector<RememberConversationPlanStep> steps;
};

struct RememberConversationTurnInput {
  std::string session_id;
  std::string input;
  std::string output;
  std::string namespace_id;
  Value metadata = Value::object({});
  std::optional<RememberConversationPlan> plan;
  std::vector<std::string> plan_steps;
};

Value vector_memory_record_to_value(const VectorMemoryRecord& record);

class VectorStore {
 public:
  virtual ~VectorStore() = default;
  virtual std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) = 0;
  virtual std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) = 0;
  virtual std::size_t erase(const std::vector<std::string>& ids) = 0;
  virtual std::size_t delete_ids(const std::vector<std::string>& ids) { return erase(ids); }
  virtual void clear(const std::string& namespace_id = {}) = 0;
  virtual std::size_t count(const std::string& namespace_id = {}) const = 0;
};

class InMemoryVectorStore : public VectorStore {
 public:
  explicit InMemoryVectorStore(std::string namespace_id = "default");
  explicit InMemoryVectorStore(InMemoryVectorStoreConfig config);

  std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) override;
  std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) override;
  std::size_t erase(const std::vector<std::string>& ids) override;
  void clear(const std::string& namespace_id = {}) override;
  std::size_t count(const std::string& namespace_id = {}) const override;

 private:
  mutable std::mutex mutex_;
  std::string namespace_id_;
  std::unordered_map<std::string, VectorMemoryRecord> records_;
};

class FileVectorStore : public VectorStore {
 public:
  FileVectorStore(std::filesystem::path file_path, std::string namespace_id = "default");
  explicit FileVectorStore(FileVectorStoreConfig config);
  std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) override;
  std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) override;
  std::size_t erase(const std::vector<std::string>& ids) override;
  void clear(const std::string& namespace_id = {}) override;
  std::size_t count(const std::string& namespace_id = {}) const override;

 private:
  void ensure_loaded() const;
  void persist() const;

  std::filesystem::path file_path_;
  std::string namespace_id_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::unordered_map<std::string, VectorMemoryRecord> records_;
};

struct LongTermMemoryConfig {
  std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>();
  std::shared_ptr<VectorStore> store = std::make_shared<InMemoryVectorStore>();
  std::string namespace_id = "default";
  std::size_t top_k = 4;
  double min_score = 0.2;
  bool auto_remember = true;
  std::string context_title = "Long-term memory retrieval";
};

class LongTermMemory : public LongTermMemoryPort {
 public:
  LongTermMemory(std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>(),
                 std::shared_ptr<VectorStore> store = std::make_shared<InMemoryVectorStore>(),
                 std::string namespace_id = "default", std::size_t top_k = 4, double min_score = 0.2,
                 bool auto_remember = true, std::string context_title = "Long-term memory retrieval");
  explicit LongTermMemory(LongTermMemoryConfig config);

  VectorMemoryRecord remember(std::string content, Value metadata = Value::object({}),
                              std::string namespace_id = {},
                              CancellationToken* cancellation = nullptr);
  VectorMemoryRecord remember(const RememberMemoryInput& input,
                              CancellationToken* cancellation = nullptr);
  std::vector<VectorMemoryRecord> remember_many(const std::vector<RememberMemoryInput>& inputs,
                                                CancellationToken* cancellation = nullptr);
  std::vector<RetrievedMemory> search(const std::string& query,
                                      std::optional<std::size_t> top_k = std::nullopt,
                                      std::optional<double> min_score = std::nullopt,
                                      std::string namespace_id = {},
                                      CancellationToken* cancellation = nullptr);
  std::vector<RetrievedMemory> search(const std::string& query,
                                      const SearchMemoryOptions& options,
                                      CancellationToken* cancellation = nullptr);
  std::optional<AgentMessage> create_context_message(const std::vector<RetrievedMemory>& hits) const;
  LongTermMemoryContextResult build_context_message(const std::string& query,
                                                    std::optional<std::size_t> top_k = std::nullopt,
                                                    std::optional<double> min_score = std::nullopt,
                                                    std::string namespace_id = {},
                                                    CancellationToken* cancellation = nullptr);
  LongTermMemoryContextResult build_context_message(const std::string& query,
                                                    const SearchMemoryOptions& options,
                                                    CancellationToken* cancellation = nullptr) override;
  [[nodiscard]] bool auto_remember() const noexcept override;
  void remember_conversation_turn(const LongTermMemoryWritebackInput& input) override;
  VectorMemoryRecord remember_conversation_turn(const std::string& session_id, const std::string& input,
                                                const std::string& output, Value metadata = Value::object({}),
                                                std::string namespace_id = {},
                                                std::vector<std::string> plan_steps = {});
  VectorMemoryRecord remember_conversation_turn(const RememberConversationTurnInput& input);

 private:
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<VectorStore> store_;
  std::string namespace_id_;
  std::size_t top_k_;
  double min_score_;
  bool auto_remember_;
  std::string context_title_;
};

}  // namespace agent
