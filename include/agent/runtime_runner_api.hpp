#pragma once

#include "agent/runtime_runner_facades.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct AgentRunnerResolvedConfig;
struct AgentRunnerKernel;

class AgentRunner {
 public:
  explicit AgentRunner(const AgentRunnerConfig& config);
  virtual ~AgentRunner();

  [[nodiscard]] RunnerTools tools() noexcept;
  [[nodiscard]] RunnerContexts contexts() noexcept;
  [[nodiscard]] RunnerEvents events() noexcept;
  [[nodiscard]] RunnerSessions sessions() noexcept;
  [[nodiscard]] RunnerModels models() const noexcept;
  [[nodiscard]] RunnerContextStats context_stats() const noexcept;
  [[nodiscard]] RunnerExecution execution() noexcept;
  [[nodiscard]] RunnerStreaming streaming() noexcept;

  void set_approval_handler(PermissionApprovalHandler approval_handler);
 private:
  friend class RunnerTools;
  friend class RunnerContexts;
  friend class RunnerEvents;
  friend class RunnerSessions;
  friend class RunnerModels;
  friend class RunnerContextStats;
  friend class RunnerExecution;
  friend class RunnerStreaming;

  std::shared_ptr<SessionMemory> get_session(const std::string& session_id = "default");
  [[nodiscard]] EventBus* event_bus() const noexcept;
  [[nodiscard]] std::optional<ContextStatsSnapshot> last_context_stats() const;
  AgentRunnerRunResult run_input(AgentMessage input_message, Value input_value,
                                 bool allow_skill_input_rewrite,
                                 const std::string& session_id,
                                 const ModelSettings& model_settings,
                                 RunnerRetrievalOptions retrieval_options,
                                 RunnerWritebackOptions writeback_options,
                                 std::vector<SkillActivation> skill_activations,
                                 Value context,
                                 std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                 AgentRunnerDurableOptions durable_options,
                                 CancellationToken* cancellation,
                                 bool enable_planning);
  AgentRunnerStreamResult stream_input(AgentMessage input_message, Value input_value,
                                       bool allow_skill_input_rewrite,
                                       const std::string& session_id,
                                       const ModelSettings& model_settings,
                                       RunnerRetrievalOptions retrieval_options,
                                       RunnerWritebackOptions writeback_options,
                                       std::vector<SkillActivation> skill_activations,
                                       Value context,
                                       std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                       CancellationToken* cancellation,
                                       bool enable_planning,
                                       AgentRunnerStreamEventHandler on_event);
  ContextStatsSnapshot estimate_context_stats_input(AgentMessage input_message,
                                                    Value input_value,
                                                    const std::string& session_id,
                                                    ContextStatsOptions options) const;
  void clear_last_context_stats();
  void store_last_context_stats(ContextStatsSnapshot snapshot);

  std::unique_ptr<AgentRunnerKernel> kernel_;
};

}  // namespace agent
