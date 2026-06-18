#pragma once

#include "agent/memory_session.hpp"

#include <optional>
#include <string_view>

namespace agent {

enum class ContextStatsBucketKind {
  SystemPrompt,
  Rules,
  ToolDefinitions,
  Skills,
  Mcp,
  Subagents,
  Memory,
  Knowledge,
  Planning,
  Conversation,
  Context,
  Other,
};

std::string to_string(ContextStatsBucketKind kind);
ContextStatsBucketKind context_stats_bucket_kind_from_string(
    const std::string& value,
    ContextStatsBucketKind fallback = ContextStatsBucketKind::Other);

struct ContextStatsBucket {
  std::string id;
  std::string label;
  ContextStatsBucketKind kind = ContextStatsBucketKind::Other;
  std::size_t tokens = 0;
  std::size_t chars = 0;
  std::size_t messages = 0;
  Value metadata = Value::object({});
};

Value context_stats_bucket_to_value(const ContextStatsBucket& bucket);

struct ContextStatsSnapshot {
  std::string session_id;
  std::size_t total_tokens = 0;
  std::optional<std::size_t> context_window_tokens;
  std::string estimator = "char_div_4";
  bool accurate = false;
  std::vector<ContextStatsBucket> buckets;
};

Value context_stats_snapshot_to_value(const ContextStatsSnapshot& snapshot);

struct ContextTokenCounter {
  std::string estimator = "char_div_4";
  bool accurate = false;
  std::function<std::size_t(std::string_view)> count_text;
  std::function<std::size_t(const AgentMessage&)> count_message;
  std::function<std::size_t(std::span<const AgentMessage>)> count_messages;
  std::function<std::size_t(const ChatToolDescriptor&)> count_tool;
};

[[nodiscard]] bool context_token_counter_configured(const ContextTokenCounter& counter) noexcept;
ContextTokenCounter default_context_token_counter();
ContextTokenCounter context_token_counter_from_session_counter(
    SessionMemoryTokenCounter token_counter,
    std::string estimator = "session_memory_counter",
    bool accurate = false);

struct ContextStatsBucketInput {
  std::string id;
  std::string label;
  ContextStatsBucketKind kind = ContextStatsBucketKind::Other;
  std::string text;
  std::vector<AgentMessage> messages;
  std::vector<ChatToolDescriptor> tools;
  Value metadata = Value::object({});
};

struct ContextStatsAssembly {
  std::string session_id;
  std::optional<std::size_t> context_window_tokens;
  ContextTokenCounter counter;
  std::vector<ContextStatsBucketInput> buckets;
};

ContextStatsSnapshot estimate_context_stats(const ContextStatsAssembly& assembly);

}  // namespace agent
