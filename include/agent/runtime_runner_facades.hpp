#pragma once

#include "agent/runtime_runner_config.hpp"

namespace agent {

// Native C++ SDK surface. Cross-language bindings must use agent_capi.h or
// agent_capi_full.h instead of wrapping this STL-based API directly.
class AgentRunner;

class RunnerTools {
 public:
  explicit RunnerTools(AgentRunner& runner) noexcept;

  ToolDefinition& register_tool(ToolDefinition tool);

 private:
  AgentRunner* runner_;
};

class RunnerContexts {
 public:
  explicit RunnerContexts(AgentRunner& runner) noexcept;

  ContextSource& register_source(ContextSource source);

 private:
  AgentRunner* runner_;
};

class RunnerEvents {
 public:
  explicit RunnerEvents(AgentRunner& runner) noexcept;

  std::size_t register_sink(EventBus::Sink sink);
  void unregister_sink(std::size_t sink_id);
  [[nodiscard]] EventBus* bus() const noexcept;

 private:
  AgentRunner* runner_;
};

class RunnerSessions {
 public:
  explicit RunnerSessions(AgentRunner& runner) noexcept;

  std::shared_ptr<SessionMemory> get(const std::string& session_id = "default");
  SessionMemorySnapshot compact(const std::string& session_id = "default");
  [[nodiscard]] SessionStore* store() const noexcept;
  [[nodiscard]] ScratchStore* scratch_store() const noexcept;

 private:
  AgentRunner* runner_;
};

class RunnerModels {
 public:
  explicit RunnerModels(const AgentRunner& runner) noexcept;

  [[nodiscard]] std::shared_ptr<ChatModelAdapter> primary() const noexcept;
  [[nodiscard]] std::shared_ptr<ChatModelAdapter> thinking() const noexcept;
  [[nodiscard]] std::shared_ptr<ChatModelAdapter> critique() const noexcept;

 private:
  const AgentRunner* runner_;
};

class RunnerContextStats {
 public:
  explicit RunnerContextStats(const AgentRunner& runner) noexcept;

  ContextStatsSnapshot estimate(const std::string& input = {},
                                const std::string& session_id = "default",
                                ContextStatsOptions options = {}) const;
  ContextStatsSnapshot estimate(std::vector<MessageContentPart> input_parts,
                                const std::string& session_id = "default",
                                ContextStatsOptions options = {}) const;
  ContextStatsSnapshot estimate(AgentMessage input_message,
                                const std::string& session_id = "default",
                                ContextStatsOptions options = {}) const;
  [[nodiscard]] std::optional<ContextStatsSnapshot> last() const;

 private:
  const AgentRunner* runner_;
};

class RunnerExecution {
 public:
  explicit RunnerExecution(AgentRunner& runner) noexcept;

  AgentRunnerRunResult run(const std::string& input, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);
  AgentRunnerRunResult run(std::vector<MessageContentPart> input_parts, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);
  AgentRunnerRunResult run(AgentMessage input_message, const std::string& session_id = "default",
                           const ModelSettings& model_settings = {},
                           RunnerRetrievalOptions retrieval_options = {},
                           RunnerWritebackOptions writeback_options = {},
                           std::vector<SkillActivation> skill_activations = {},
                           Value context = Value::object({}),
                           std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                           AgentRunnerDurableOptions durable_options = {},
                           CancellationToken* cancellation = nullptr,
                           bool enable_planning = true);

 private:
  AgentRunner* runner_;
};

class RunnerStreaming {
 public:
  explicit RunnerStreaming(AgentRunner& runner) noexcept;

  AgentRunnerStreamResult stream(const std::string& input,
                                 AgentRunnerStreamEventHandler on_event,
                                 const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  AgentRunnerStreamResult stream(std::vector<MessageContentPart> input_parts,
                                 AgentRunnerStreamEventHandler on_event,
                                 const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  AgentRunnerStreamResult stream(AgentMessage input_message,
                                 AgentRunnerStreamEventHandler on_event,
                                 const std::string& session_id = "default",
                                 const ModelSettings& model_settings = {},
                                 RunnerRetrievalOptions retrieval_options = {},
                                 RunnerWritebackOptions writeback_options = {},
                                 std::vector<SkillActivation> skill_activations = {},
                                 Value context = Value::object({}),
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                 CancellationToken* cancellation = nullptr,
                                 bool enable_planning = true);
  AgentRunnerEventStream events(const std::string& input,
                                StreamQueueOptions queue_options = {},
                                const std::string& session_id = "default",
                                const ModelSettings& model_settings = {},
                                RunnerRetrievalOptions retrieval_options = {},
                                RunnerWritebackOptions writeback_options = {},
                                std::vector<SkillActivation> skill_activations = {},
                                Value context = Value::object({}),
                                std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                CancellationToken* cancellation = nullptr,
                                bool enable_planning = true);
  AgentRunnerEventStream events(std::vector<MessageContentPart> input_parts,
                                StreamQueueOptions queue_options = {},
                                const std::string& session_id = "default",
                                const ModelSettings& model_settings = {},
                                RunnerRetrievalOptions retrieval_options = {},
                                RunnerWritebackOptions writeback_options = {},
                                std::vector<SkillActivation> skill_activations = {},
                                Value context = Value::object({}),
                                std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                CancellationToken* cancellation = nullptr,
                                bool enable_planning = true);
  AgentRunnerEventStream events(AgentMessage input_message,
                                StreamQueueOptions queue_options = {},
                                const std::string& session_id = "default",
                                const ModelSettings& model_settings = {},
                                RunnerRetrievalOptions retrieval_options = {},
                                RunnerWritebackOptions writeback_options = {},
                                std::vector<SkillActivation> skill_activations = {},
                                Value context = Value::object({}),
                                std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt,
                                CancellationToken* cancellation = nullptr,
                                bool enable_planning = true);

 private:
  AgentRunner* runner_;
};

}  // namespace agent
