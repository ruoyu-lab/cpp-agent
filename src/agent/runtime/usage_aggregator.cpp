#include "usage_aggregator.hpp"

namespace agent {

void UsageAggregator::emit_cache_stats(EventBus& bus,
                                       const AgentOutput& response,
                                       const ModelSettings& settings,
                                       const TraceContext& trace) {
  const auto usage = extract_model_usage(response);
  if (usage.input_tokens <= 0 && usage.cached_input_tokens <= 0 && usage.reasoning_tokens <= 0) {
    return;
  }
  const double hit_rate = usage.input_tokens > 0
                              ? static_cast<double>(usage.cached_input_tokens) /
                                    static_cast<double>(usage.input_tokens)
                              : 0.0;
  bus.publish("model.cache_stats", ExecutionTarget::Run,
              Value::object({
                  {"provider", response.provider},
                  {"model", response.model},
                  {"totalInputTokens", static_cast<long long>(usage.input_tokens)},
                  {"cachedInputTokens", static_cast<long long>(usage.cached_input_tokens)},
                  {"hitRate", hit_rate},
                  {"strategy", to_string(settings.cache_strategy)},
                  {"inputTokensSource", to_string(usage.input_tokens_source)},
                  {"outputTokensSource", to_string(usage.output_tokens_source)},
                  {"totalTokensSource", to_string(usage.total_tokens_source)},
                  {"quality", to_string(usage.quality)},
                  {"cachedInputTokensSource", to_string(usage.cached_input_tokens_source)},
                  {"reasoningTokens", static_cast<long long>(usage.reasoning_tokens)},
                  {"reasoningSource", to_string(usage.reasoning_source)},
              }),
              trace);
}

ModelUsage UsageAggregator::from_trace(const std::vector<AgentTraceEntry>& trace) {
  std::vector<ModelUsage> per_call;
  per_call.reserve(trace.size());
  for (const auto& entry : trace) {
    if (entry.type != "model") {
      continue;
    }
    per_call.push_back(extract_model_usage(entry.response));
  }
  return merge_model_usage(per_call);
}

}  // namespace agent
