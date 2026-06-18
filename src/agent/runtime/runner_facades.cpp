#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace agent {

RunnerTools::RunnerTools(AgentRunner& runner) noexcept : runner_(&runner) {}

ToolDefinition& RunnerTools::register_tool(ToolDefinition tool) {
  return runner_->kernel_->tool_registry.register_tool(std::move(tool));
}

RunnerContexts::RunnerContexts(AgentRunner& runner) noexcept : runner_(&runner) {}

ContextSource& RunnerContexts::register_source(ContextSource source) {
  return runner_->kernel_->context_manager.register_source(std::move(source));
}

RunnerEvents::RunnerEvents(AgentRunner& runner) noexcept : runner_(&runner) {}

std::size_t RunnerEvents::register_sink(EventBus::Sink sink) {
  return runner_->event_bus()->register_sink(std::move(sink));
}

void RunnerEvents::unregister_sink(std::size_t sink_id) {
  runner_->event_bus()->unregister_sink(sink_id);
}

EventBus* RunnerEvents::bus() const noexcept {
  return runner_->event_bus();
}

RunnerSessions::RunnerSessions(AgentRunner& runner) noexcept : runner_(&runner) {}

std::shared_ptr<SessionMemory> RunnerSessions::get(const std::string& session_id) {
  return runner_->get_session(session_id);
}

SessionMemorySnapshot RunnerSessions::compact(const std::string& session_id) {
  auto session = runner_->kernel_->memory_store->get(session_id);
  return CompactionPlanner::compact_session(*session, runner_->event_bus());
}

SessionStore* RunnerSessions::store() const noexcept {
  return runner_->kernel_->memory_store.get();
}

ScratchStore* RunnerSessions::scratch_store() const noexcept {
  return runner_->kernel_->scratch_store.get();
}

RunnerModels::RunnerModels(const AgentRunner& runner) noexcept : runner_(&runner) {}

std::shared_ptr<ChatModelAdapter> RunnerModels::primary() const noexcept {
  return runner_->kernel_->config->adapter;
}

std::shared_ptr<ChatModelAdapter> RunnerModels::thinking() const noexcept {
  return runner_->kernel_->config->thinking_adapter;
}

std::shared_ptr<ChatModelAdapter> RunnerModels::critique() const noexcept {
  return runner_->kernel_->config->critique_adapter;
}

RunnerContextStats::RunnerContextStats(const AgentRunner& runner) noexcept : runner_(&runner) {}

RunnerExecution::RunnerExecution(AgentRunner& runner) noexcept : runner_(&runner) {}

RunnerStreaming::RunnerStreaming(AgentRunner& runner) noexcept : runner_(&runner) {}

RunnerTools AgentRunner::tools() noexcept {
  return RunnerTools(*this);
}

RunnerContexts AgentRunner::contexts() noexcept {
  return RunnerContexts(*this);
}

RunnerEvents AgentRunner::events() noexcept {
  return RunnerEvents(*this);
}

RunnerSessions AgentRunner::sessions() noexcept {
  return RunnerSessions(*this);
}

RunnerModels AgentRunner::models() const noexcept {
  return RunnerModels(*this);
}

RunnerContextStats AgentRunner::context_stats() const noexcept {
  return RunnerContextStats(*this);
}

RunnerExecution AgentRunner::execution() noexcept {
  return RunnerExecution(*this);
}

RunnerStreaming AgentRunner::streaming() noexcept {
  return RunnerStreaming(*this);
}

void AgentRunner::set_approval_handler(PermissionApprovalHandler approval_handler) {
  kernel_->config->approval_handler = std::move(approval_handler);
}

std::shared_ptr<SessionMemory> AgentRunner::get_session(const std::string& session_id) {
  return kernel_->memory_store->get(session_id);
}

EventBus* AgentRunner::event_bus() const noexcept {
  return kernel_->config->event_bus ? kernel_->config->event_bus : const_cast<EventBus*>(&kernel_->owned_event_bus);
}

std::optional<ContextStatsSnapshot> AgentRunner::last_context_stats() const {
  std::lock_guard<std::mutex> lock(kernel_->last_context_stats_mutex);
  return kernel_->last_context_stats;
}

void AgentRunner::clear_last_context_stats() {
  std::lock_guard<std::mutex> lock(kernel_->last_context_stats_mutex);
  kernel_->last_context_stats.reset();
}

void AgentRunner::store_last_context_stats(ContextStatsSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(kernel_->last_context_stats_mutex);
  kernel_->last_context_stats = std::move(snapshot);
}

}  // namespace agent
