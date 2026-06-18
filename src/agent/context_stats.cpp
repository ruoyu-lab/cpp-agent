#include "agent/context_stats.hpp"

namespace agent {

namespace {

std::size_t count_chars_in_messages(std::span<const AgentMessage> messages) {
  std::size_t total = 0;
  for (const auto& message : messages) {
    total += extract_text_content(message.content).size();
  }
  return total;
}

std::string render_tool_descriptor_text(const ChatToolDescriptor& tool) {
  return "- " + tool.name + ": " + (tool.description.empty() ? "No description" : tool.description) + "\n"
         + "  inputSchema: " + json_schema_to_value(tool.input_schema).stringify(0) + "\n";
}

std::size_t count_bucket_chars(const ContextStatsBucketInput& bucket) {
  std::size_t total = bucket.text.size();
  total += count_chars_in_messages(bucket.messages);
  for (const auto& tool : bucket.tools) {
    total += render_tool_descriptor_text(tool).size();
  }
  return total;
}

std::size_t count_bucket_tokens(const ContextStatsBucketInput& bucket, const ContextTokenCounter& counter) {
  std::size_t total = 0;
  if (!bucket.text.empty()) {
    total += counter.count_text ? counter.count_text(bucket.text) : (bucket.text.size() + 3) / 4;
  }
  if (!bucket.messages.empty()) {
    if (counter.count_messages) {
      total += counter.count_messages(bucket.messages);
    } else if (counter.count_message) {
      for (const auto& message : bucket.messages) {
        total += counter.count_message(message);
      }
    } else {
      total += (count_chars_in_messages(bucket.messages) + 3) / 4;
    }
  }
  if (!bucket.tools.empty()) {
    if (counter.count_tool) {
      for (const auto& tool : bucket.tools) {
        total += counter.count_tool(tool);
      }
    } else if (counter.count_text) {
      for (const auto& tool : bucket.tools) {
        const auto rendered = render_tool_descriptor_text(tool);
        total += counter.count_text(rendered);
      }
    } else {
      for (const auto& tool : bucket.tools) {
        const auto rendered = render_tool_descriptor_text(tool);
        total += (rendered.size() + 3) / 4;
      }
    }
  }
  return total;
}

}  // namespace

std::string to_string(ContextStatsBucketKind kind) {
  switch (kind) {
    case ContextStatsBucketKind::SystemPrompt:
      return "system_prompt";
    case ContextStatsBucketKind::Rules:
      return "rules";
    case ContextStatsBucketKind::ToolDefinitions:
      return "tool_definitions";
    case ContextStatsBucketKind::Skills:
      return "skills";
    case ContextStatsBucketKind::Mcp:
      return "mcp";
    case ContextStatsBucketKind::Subagents:
      return "subagents";
    case ContextStatsBucketKind::Memory:
      return "memory";
    case ContextStatsBucketKind::Knowledge:
      return "knowledge";
    case ContextStatsBucketKind::Planning:
      return "planning";
    case ContextStatsBucketKind::Conversation:
      return "conversation";
    case ContextStatsBucketKind::Context:
      return "context";
    case ContextStatsBucketKind::Other:
      return "other";
  }
  return "other";
}

ContextStatsBucketKind context_stats_bucket_kind_from_string(
    const std::string& value,
    ContextStatsBucketKind fallback) {
  if (value == "system_prompt") {
    return ContextStatsBucketKind::SystemPrompt;
  }
  if (value == "rules") {
    return ContextStatsBucketKind::Rules;
  }
  if (value == "tool_definitions") {
    return ContextStatsBucketKind::ToolDefinitions;
  }
  if (value == "skills") {
    return ContextStatsBucketKind::Skills;
  }
  if (value == "mcp") {
    return ContextStatsBucketKind::Mcp;
  }
  if (value == "subagents") {
    return ContextStatsBucketKind::Subagents;
  }
  if (value == "memory") {
    return ContextStatsBucketKind::Memory;
  }
  if (value == "knowledge") {
    return ContextStatsBucketKind::Knowledge;
  }
  if (value == "planning") {
    return ContextStatsBucketKind::Planning;
  }
  if (value == "conversation") {
    return ContextStatsBucketKind::Conversation;
  }
  if (value == "context") {
    return ContextStatsBucketKind::Context;
  }
  if (value == "other") {
    return ContextStatsBucketKind::Other;
  }
  return fallback;
}

Value context_stats_bucket_to_value(const ContextStatsBucket& bucket) {
  return Value::object({
      {"id", bucket.id},
      {"label", bucket.label},
      {"kind", to_string(bucket.kind)},
      {"tokens", static_cast<long long>(bucket.tokens)},
      {"chars", static_cast<long long>(bucket.chars)},
      {"messages", static_cast<long long>(bucket.messages)},
      {"metadata", bucket.metadata},
  });
}

Value context_stats_snapshot_to_value(const ContextStatsSnapshot& snapshot) {
  Value::Array buckets;
  buckets.reserve(snapshot.buckets.size());
  for (const auto& bucket : snapshot.buckets) {
    buckets.push_back(context_stats_bucket_to_value(bucket));
  }
  return Value::object({
      {"sessionId", snapshot.session_id},
      {"totalTokens", static_cast<long long>(snapshot.total_tokens)},
      {"contextWindowTokens", snapshot.context_window_tokens ? Value(static_cast<long long>(*snapshot.context_window_tokens))
                                                             : Value()},
      {"estimator", snapshot.estimator},
      {"accurate", snapshot.accurate},
      {"buckets", Value(std::move(buckets))},
  });
}

bool context_token_counter_configured(const ContextTokenCounter& counter) noexcept {
  return static_cast<bool>(counter.count_text) || static_cast<bool>(counter.count_message)
         || static_cast<bool>(counter.count_messages) || static_cast<bool>(counter.count_tool);
}

ContextTokenCounter default_context_token_counter() {
  ContextTokenCounter counter;
  counter.estimator = "char_div_4";
  counter.accurate = false;
  counter.count_text = [](std::string_view text) {
    return (text.size() + 3) / 4;
  };
  counter.count_message = [count_text = counter.count_text](const AgentMessage& message) {
    return count_text(extract_text_content(message.content));
  };
  counter.count_messages = [](std::span<const AgentMessage> messages) {
    return (count_chars_in_messages(messages) + 3) / 4;
  };
  counter.count_tool = [count_text = counter.count_text](const ChatToolDescriptor& tool) {
    return count_text(render_tool_descriptor_text(tool));
  };
  return counter;
}

ContextTokenCounter context_token_counter_from_session_counter(SessionMemoryTokenCounter token_counter,
                                                               std::string estimator,
                                                               bool accurate) {
  if (!token_counter) {
    return default_context_token_counter();
  }
  ContextTokenCounter counter;
  counter.estimator = estimator.empty() ? "session_memory_counter" : std::move(estimator);
  counter.accurate = accurate;
  counter.count_text = [token_counter](std::string_view text) {
    std::vector<AgentMessage> messages;
    messages.push_back(create_message(MessageRole::System, std::string(text)));
    return token_counter(messages);
  };
  counter.count_message = [token_counter](const AgentMessage& message) {
    std::vector<AgentMessage> messages;
    messages.push_back(message);
    return token_counter(messages);
  };
  counter.count_messages = [token_counter](std::span<const AgentMessage> messages) {
    std::vector<AgentMessage> owned(messages.begin(), messages.end());
    return token_counter(owned);
  };
  counter.count_tool = [count_text = counter.count_text](const ChatToolDescriptor& tool) {
    return count_text(render_tool_descriptor_text(tool));
  };
  return counter;
}

ContextStatsSnapshot estimate_context_stats(const ContextStatsAssembly& assembly) {
  const ContextTokenCounter counter =
      context_token_counter_configured(assembly.counter) ? assembly.counter : default_context_token_counter();

  ContextStatsSnapshot snapshot;
  snapshot.session_id = assembly.session_id;
  snapshot.context_window_tokens = assembly.context_window_tokens;
  snapshot.estimator = counter.estimator;
  snapshot.accurate = counter.accurate;
  snapshot.buckets.reserve(assembly.buckets.size());

  for (const auto& input_bucket : assembly.buckets) {
    ContextStatsBucket bucket;
    bucket.id = input_bucket.id;
    bucket.label = input_bucket.label;
    bucket.kind = input_bucket.kind;
    bucket.chars = count_bucket_chars(input_bucket);
    bucket.tokens = count_bucket_tokens(input_bucket, counter);
    bucket.messages = input_bucket.messages.size();
    bucket.metadata = input_bucket.metadata;
    snapshot.total_tokens += bucket.tokens;
    snapshot.buckets.push_back(std::move(bucket));
  }

  return snapshot;
}

}  // namespace agent
