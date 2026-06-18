std::string to_string(ReasoningSource source) {
  switch (source) {
    case ReasoningSource::Provider:
      return "provider";
    case ReasoningSource::Estimated:
      return "estimated";
    case ReasoningSource::Unknown:
      break;
  }
  return "unknown";
}

ReasoningSource reasoning_source_from_string(std::string_view text, ReasoningSource fallback) {
  if (text == "provider") {
    return ReasoningSource::Provider;
  }
  if (text == "estimated") {
    return ReasoningSource::Estimated;
  }
  if (text == "unknown") {
    return ReasoningSource::Unknown;
  }
  return fallback;
}

ReasoningSource merge_reasoning_source(ReasoningSource a, ReasoningSource b) {
  // Worst-case wins: Unknown > Estimated > Provider (Unknown is "worst" because it
  // means a contributing call had no data at all, so the merged total can't claim
  // provider-authoritative coverage).
  if (a == ReasoningSource::Unknown || b == ReasoningSource::Unknown) {
    return ReasoningSource::Unknown;
  }
  if (a == ReasoningSource::Estimated || b == ReasoningSource::Estimated) {
    return ReasoningSource::Estimated;
  }
  return ReasoningSource::Provider;
}

std::string to_string(TokenUsageSource source) {
  switch (source) {
    case TokenUsageSource::Provider:
      return "provider";
    case TokenUsageSource::Derived:
      return "derived";
    case TokenUsageSource::Estimated:
      return "estimated";
    case TokenUsageSource::Unknown:
      break;
  }
  return "unknown";
}

TokenUsageSource token_usage_source_from_string(std::string_view text,
                                                TokenUsageSource fallback) {
  if (text == "provider") {
    return TokenUsageSource::Provider;
  }
  if (text == "derived") {
    return TokenUsageSource::Derived;
  }
  if (text == "estimated") {
    return TokenUsageSource::Estimated;
  }
  if (text == "unknown") {
    return TokenUsageSource::Unknown;
  }
  return fallback;
}

TokenUsageSource merge_token_usage_source(TokenUsageSource a, TokenUsageSource b) {
  if (a == TokenUsageSource::Unknown || b == TokenUsageSource::Unknown) {
    return TokenUsageSource::Unknown;
  }
  if (a == TokenUsageSource::Estimated || b == TokenUsageSource::Estimated) {
    return TokenUsageSource::Estimated;
  }
  if (a == TokenUsageSource::Derived || b == TokenUsageSource::Derived) {
    return TokenUsageSource::Derived;
  }
  return TokenUsageSource::Provider;
}

std::string to_string(TokenUsageQuality quality) {
  switch (quality) {
    case TokenUsageQuality::Provider:
      return "provider";
    case TokenUsageQuality::Mixed:
      return "mixed";
    case TokenUsageQuality::Estimated:
      return "estimated";
    case TokenUsageQuality::Unknown:
      break;
  }
  return "unknown";
}

TokenUsageQuality token_usage_quality_from_sources(const std::vector<TokenUsageSource>& sources) {
  bool has_known = false;
  bool has_unknown = false;
  bool has_estimated = false;
  bool has_provider_or_derived = false;
  for (const auto source : sources) {
    if (source == TokenUsageSource::Unknown) {
      has_unknown = true;
      continue;
    }
    has_known = true;
    if (source == TokenUsageSource::Estimated) {
      has_estimated = true;
    }
    if (source == TokenUsageSource::Provider || source == TokenUsageSource::Derived) {
      has_provider_or_derived = true;
    }
  }
  if (!has_known) {
    return TokenUsageQuality::Unknown;
  }
  if (has_unknown) {
    return TokenUsageQuality::Mixed;
  }
  if (has_estimated && has_provider_or_derived) {
    return TokenUsageQuality::Mixed;
  }
  if (has_estimated) {
    return TokenUsageQuality::Estimated;
  }
  return TokenUsageQuality::Provider;
}

ModelUsage extract_model_usage(const Value& response_or_raw, std::string provider) {
  const Value* raw = &response_or_raw;
  if (response_or_raw.is_object() && response_or_raw.contains("raw")) {
    raw = &response_or_raw.at("raw");
  }

  const auto has_usage_fields = [](const Value& value) {
    if (!value.is_object()) {
      return false;
    }
    for (const auto& field : {
             "inputTokens", "input_tokens", "inputTokenCount", "input_token_count",
             "promptTokens", "prompt_tokens", "promptTokenCount", "prompt_token_count",
             "promptEvalCount", "prompt_eval_count", "outputTokens", "output_tokens",
             "outputTokenCount", "output_token_count", "completionTokens",
             "completion_tokens", "completionTokenCount", "completion_token_count",
             "candidatesTokenCount", "candidates_token_count", "totalOutputTokens",
             "total_output_tokens", "evalCount", "eval_count", "totalTokens",
             "total_tokens", "totalTokenCount", "total_token_count", "reasoningTokens",
             "reasoning_tokens", "reasoningOutputTokens", "reasoning_output_tokens",
             "thoughtsTokenCount", "thoughts_token_count"}) {
      if (value.at(field).is_number()) {
        return true;
      }
    }
    return value.at("completion_tokens_details").is_object() ||
           value.at("completionTokensDetails").is_object();
  };

  const auto select_direct_usage = [](const Value& value, bool& nested) -> const Value* {
    nested = false;
    if (value.is_object() && value.at("usage").is_object()) {
      nested = true;
      return &value.at("usage");
    }
    if (value.is_object() && value.at("usage_metadata").is_object()) {
      nested = true;
      return &value.at("usage_metadata");
    }
    if (value.is_object() && value.at("usageMetadata").is_object()) {
      nested = true;
      return &value.at("usageMetadata");
    }
    if (value.is_object() && value.at("stats").is_object()) {
      nested = true;
      return &value.at("stats");
    }
    return &value;
  };

  const Value* usage = raw;
  bool nested_usage = false;
  usage = select_direct_usage(*raw, nested_usage);
  if (!nested_usage && raw->is_object() && raw->at("chunks").is_array()) {
    const auto& chunks = raw->at("chunks").as_array();
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      bool chunk_nested_usage = false;
      const Value* candidate = select_direct_usage(*it, chunk_nested_usage);
      if (chunk_nested_usage || has_usage_fields(*candidate)) {
        usage = candidate;
        break;
      }
    }
  }

  const auto read_first = [&](const std::vector<std::string>& fields) -> std::optional<int> {
    if (!usage->is_object()) {
      return std::nullopt;
    }
    for (const auto& field : fields) {
      const auto& value = usage->at(field);
      if (value.is_number()) {
        return static_cast<int>(value.as_integer());
      }
    }
    return std::nullopt;
  };

  const auto reported_input_tokens = read_first({"inputTokens", "input_tokens", "inputTokenCount",
                                                 "input_token_count", "promptTokens", "prompt_tokens",
                                                 "promptTokenCount", "prompt_token_count",
                                                 "promptEvalCount", "prompt_eval_count"});
  const auto reported_output_tokens = read_first({"outputTokens", "output_tokens", "outputTokenCount",
                                                  "output_token_count", "completionTokens", "completion_tokens",
                                                  "completionTokenCount", "completion_token_count",
                                                  "candidatesTokenCount", "candidates_token_count",
                                                  "totalOutputTokens", "total_output_tokens", "evalCount",
                                                  "eval_count"});
  int input_tokens = reported_input_tokens.value_or(0);
  int output_tokens = reported_output_tokens.value_or(0);
  const auto token_source_for = [](const std::optional<int>& value) {
    return value ? TokenUsageSource::Provider : TokenUsageSource::Unknown;
  };
  TokenUsageSource input_tokens_source = token_source_for(reported_input_tokens);
  TokenUsageSource output_tokens_source = token_source_for(reported_output_tokens);

  // Cached input tokens — provider-reported hit count.
  // Side-channel cache counters are normalized independently from provider names:
  // - cache_read_input_tokens/cache_creation_input_tokens are additive side channels.
  // - prompt_tokens_details.cached_tokens and cached_content_token_count are subsets
  //   of an already-reported prompt total and must not be added again.
  int cached_input_tokens = 0;
  int cache_creation_tokens = 0;
  TokenUsageSource cached_input_tokens_source = TokenUsageSource::Unknown;
  if (usage->is_object()) {
    const auto cache_read_input_tokens =
        read_first({"cache_read_input_tokens", "cacheReadInputTokens"});
    const auto direct_cached_input_tokens =
        cache_read_input_tokens
            ? cache_read_input_tokens
            : read_first({"cached_input_tokens",
                          "cachedInputTokens",
                          "cached_content_token_count",
                          "cachedContentTokenCount"});
    if (direct_cached_input_tokens) {
      cached_input_tokens = direct_cached_input_tokens.value_or(0);
      cached_input_tokens_source = TokenUsageSource::Provider;
    }
    cache_creation_tokens = read_first({"cache_creation_input_tokens",
                                        "cacheCreationInputTokens"})
                                .value_or(0);
    if (cached_input_tokens == 0) {
      const auto& details = usage->at("prompt_tokens_details");
      if (details.is_object() && details.at("cached_tokens").is_number()) {
        cached_input_tokens = static_cast<int>(details.at("cached_tokens").as_integer());
        cached_input_tokens_source = TokenUsageSource::Provider;
      } else {
        const auto& details_camel = usage->at("promptTokensDetails");
        if (details_camel.is_object() && details_camel.at("cachedTokens").is_number()) {
          cached_input_tokens = static_cast<int>(details_camel.at("cachedTokens").as_integer());
          cached_input_tokens_source = TokenUsageSource::Provider;
        }
      }
    }
  }

  // Some providers expose cache read/write counts as side channels while the
  // reported input token count only includes the uncached portion. When those
  // side-channel field names are present, normalize input_tokens back to total
  // prompt size so downstream hitRate = cached / total remains meaningful.
  if ((usage->is_object() &&
       (usage->at("cache_read_input_tokens").is_number() ||
        usage->at("cacheReadInputTokens").is_number() ||
        usage->at("cache_creation_input_tokens").is_number() ||
        usage->at("cacheCreationInputTokens").is_number())) &&
      (cached_input_tokens > 0 || cache_creation_tokens > 0)) {
    input_tokens += cached_input_tokens + cache_creation_tokens;
  }

  // Reasoning tokens. Order: normalized/provider-specific field shape first;
  // estimation fallback
  // happens only in the AgentOutput overload (which has access to response.reasoning).
  int reasoning_tokens = 0;
  ReasoningSource reasoning_source = ReasoningSource::Unknown;

  const auto read_structured_reasoning = [&]() -> std::optional<int> {
    if (!usage->is_object()) {
      return std::nullopt;
    }
    const auto direct = read_first({"reasoningTokens", "reasoning_tokens",
                                    "reasoningOutputTokens", "reasoning_output_tokens"});
    if (direct) {
      return direct;
    }
    const auto& details = usage->at("completion_tokens_details");
    if (details.is_object() && details.at("reasoning_tokens").is_number()) {
      return static_cast<int>(details.at("reasoning_tokens").as_integer());
    }
    const auto& details_camel = usage->at("completionTokensDetails");
    if (details_camel.is_object() && details_camel.at("reasoningTokens").is_number()) {
      return static_cast<int>(details_camel.at("reasoningTokens").as_integer());
    }
    return std::nullopt;
  };

  if (const auto structured = read_structured_reasoning()) {
    reasoning_tokens = *structured;
    reasoning_source = ReasoningSource::Provider;
  } else if (const auto thoughts = read_first({"thoughts_token_count", "thoughtsTokenCount"});
             thoughts && *thoughts > 0) {
    reasoning_tokens = *thoughts;
    reasoning_source = ReasoningSource::Provider;
    // `thoughts_token_count` is reported separately from candidate text tokens;
    // combine them so output_tokens reflects everything produced after prompt.
    output_tokens += *thoughts;
  } else if (raw->is_object()) {
    const auto& output_block = raw->at("output");
    if (output_block.is_object()) {
      if (output_block.at("reasoning_content_tokens").is_number()) {
        reasoning_tokens =
            static_cast<int>(output_block.at("reasoning_content_tokens").as_integer());
        reasoning_source = ReasoningSource::Provider;
      } else if (output_block.at("reasoningContentTokens").is_number()) {
        reasoning_tokens =
            static_cast<int>(output_block.at("reasoningContentTokens").as_integer());
        reasoning_source = ReasoningSource::Provider;
      }
    }
  }

  const auto reported_total_tokens = read_first({"totalTokens", "total_tokens", "totalTokenCount",
                                                 "total_token_count"});
  const int total_tokens = reported_total_tokens.value_or(input_tokens + output_tokens);
  TokenUsageSource total_tokens_source = TokenUsageSource::Unknown;
  if (reported_total_tokens) {
    total_tokens_source = TokenUsageSource::Provider;
  } else if (input_tokens_source != TokenUsageSource::Unknown &&
             output_tokens_source != TokenUsageSource::Unknown) {
    total_tokens_source = TokenUsageSource::Derived;
  }
  ModelUsage result;
  result.input_tokens = input_tokens;
  result.output_tokens = output_tokens;
  result.total_tokens = total_tokens;
  result.input_tokens_source = input_tokens_source;
  result.output_tokens_source = output_tokens_source;
  result.total_tokens_source = total_tokens_source;
  result.quality = token_usage_quality_from_sources(
      {input_tokens_source, output_tokens_source, total_tokens_source});
  result.cached_input_tokens = cached_input_tokens;
  result.cached_input_tokens_source = cached_input_tokens_source;
  result.reasoning_tokens = reasoning_tokens;
  result.reasoning_source = reasoning_source;
  result.provider = std::move(provider);
  return result;
}

ModelUsage extract_model_usage(const AgentOutput& response) {
  auto usage = extract_model_usage(response.raw, response.provider);

  // Estimation fallback: if no provider-reported reasoning count, but the
  // AgentOutput carries reasoning text, estimate as chars / 4. This also
  // covers DeepSeek's reasoning_content path (parsed into response.reasoning by
  // the anthropic-shape response parser).
  if (usage.reasoning_source != ReasoningSource::Provider && response.reasoning) {
    const auto chars = response.reasoning->text.size();
    if (chars > 0) {
      usage.reasoning_tokens = static_cast<int>(chars / 4);
      usage.reasoning_source = ReasoningSource::Estimated;
    }
  }
  return usage;
}

UsageCostEstimate estimate_usage_cost(const ModelUsage& usage, const UsagePricing& pricing,
                                      std::string provider, std::string model) {
  const double input_cost = pricing.input_per_1k_tokens
                                ? (static_cast<double>(usage.input_tokens) / 1000.0) *
                                      *pricing.input_per_1k_tokens
                                : 0;
  const double output_cost = pricing.output_per_1k_tokens
                                 ? (static_cast<double>(usage.output_tokens) / 1000.0) *
                                       *pricing.output_per_1k_tokens
                                 : 0;
  return UsageCostEstimate{std::move(provider), std::move(model), pricing.currency,
                           input_cost, output_cost, input_cost + output_cost};
}

ModelUsage merge_model_usage(const std::vector<ModelUsage>& usages) {
  ModelUsage merged;
  if (usages.empty()) {
    return merged;
  }
  bool any = false;
  for (const auto& usage : usages) {
    merged.input_tokens += usage.input_tokens;
    merged.output_tokens += usage.output_tokens;
    merged.total_tokens += usage.total_tokens;
    merged.cached_input_tokens += usage.cached_input_tokens;
    merged.reasoning_tokens += usage.reasoning_tokens;
    merged.input_tokens_source = any
                                     ? merge_token_usage_source(merged.input_tokens_source,
                                                                usage.input_tokens_source)
                                     : usage.input_tokens_source;
    merged.output_tokens_source = any
                                      ? merge_token_usage_source(merged.output_tokens_source,
                                                                 usage.output_tokens_source)
                                      : usage.output_tokens_source;
    merged.total_tokens_source = any
                                     ? merge_token_usage_source(merged.total_tokens_source,
                                                                usage.total_tokens_source)
                                     : usage.total_tokens_source;
    merged.cached_input_tokens_source =
        any ? merge_token_usage_source(merged.cached_input_tokens_source,
                                       usage.cached_input_tokens_source)
            : usage.cached_input_tokens_source;
    if (merged.provider.empty()) {
      merged.provider = usage.provider;
    }
    merged.reasoning_source = any ? merge_reasoning_source(merged.reasoning_source, usage.reasoning_source)
                                  : usage.reasoning_source;
    any = true;
  }
  merged.quality = token_usage_quality_from_sources(
      {merged.input_tokens_source, merged.output_tokens_source, merged.total_tokens_source});
  return merged;
}
