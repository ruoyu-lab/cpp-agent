std::optional<UsagePricing> AgentServerMetricsCollector::pricing_for(const std::string& provider,
                                                                     const std::string& model) const {
  const auto exact = pricing_.find(provider + ":" + model);
  if (exact != pricing_.end()) {
    return exact->second;
  }
  const auto provider_default = pricing_.find(provider + ":*");
  if (provider_default != pricing_.end()) {
    return provider_default->second;
  }
  const auto global_default = pricing_.find("*");
  if (global_default != pricing_.end()) {
    return global_default->second;
  }
  return std::nullopt;
}

void AgentServerMetricsCollector::apply(AgentServerMetricsBucket& bucket, int status_code,
                                        long long duration_ms, const std::string& kind,
                                        const Value& model_response) {
  bucket.requests += 1;
  bucket.duration_ms += duration_ms;
  bucket.status_codes[std::to_string(status_code)] += 1;
  if (kind == "succeeded") {
    bucket.succeeded += 1;
    if (model_response.is_object()) {
      const auto provider = model_response.at("provider").as_string();
      const auto model = model_response.at("model").as_string();
      const auto usage = extract_model_usage(model_response, provider);
      bucket.input_tokens += usage.input_tokens;
      bucket.output_tokens += usage.output_tokens;
      bucket.total_tokens += usage.total_tokens;
      if (auto pricing = pricing_for(provider, model)) {
        const auto estimate = estimate_usage_cost(usage, *pricing, provider, model);
        bucket.estimated_cost_by_currency[estimate.currency] += estimate.total_cost;
      }
    }
  } else if (kind == "rejected") {
    bucket.rejected += 1;
  } else {
    bucket.failed += 1;
  }
}

void AgentServerMetricsCollector::record(const std::string& route, int status_code, long long duration_ms,
                                         std::string kind, std::string api_key_id,
                                         std::string agent_id, Value model_response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) {
    return;
  }
  apply(totals_, status_code, duration_ms, kind, model_response);
  if (!route.empty()) {
    apply(bucket_for(routes_, route), status_code, duration_ms, kind, model_response);
  }
  if (!agent_id.empty()) {
    apply(bucket_for(agents_, agent_id), status_code, duration_ms, kind, model_response);
  }
  if (!api_key_id.empty()) {
    apply(bucket_for(api_keys_, api_key_id), status_code, duration_ms, kind, model_response);
  }
}

Value AgentServerMetricsCollector::snapshot(const std::string& service_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) {
    return Value::object({{"ok", true}, {"service", service_name}, {"enabled", false}});
  }
  return Value::object({{"ok", true},
                        {"service", service_name},
                        {"enabled", true},
                        {"totals", agent_server_metrics_bucket_to_value(totals_)},
                        {"routes", buckets_to_value(routes_)},
                        {"agents", buckets_to_value(agents_)},
                        {"apiKeys", buckets_to_value(api_keys_)}});
}

RateLimitWindow::Result RateLimitWindow::consume(const std::string& key, int max_requests, int window_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current_ms = now_ms();
  auto& bucket = buckets_[key];
  if (bucket.reset_at_ms <= current_ms) {
    bucket.count = 0;
    bucket.reset_at_ms = current_ms + window_ms;
  }
  bucket.count += 1;
  return RateLimitWindow::Result{
      .allowed = max_requests <= 0 || bucket.count <= max_requests,
      .remaining = std::max(0, max_requests - bucket.count),
      .reset_at_ms = bucket.reset_at_ms,
  };
}

Value read_json_body(const HttpRequest& request, std::size_t max_bytes) {
  if (request.body.size() > max_bytes) {
    throw HttpRequestError(413, "Request body exceeded configured size limit.",
                           Value::object({{"error", "Request body exceeded configured size limit."}}));
  }
  if (request.body.empty()) {
    return Value::object({});
  }
  try {
    auto parsed = parse_json(request.body);
    if (!parsed.is_object()) {
      throw HttpRequestError(400, "Request body must be a JSON object.",
                             Value::object({{"error", "Request body must be a JSON object."}}));
    }
    return parsed;
  } catch (const HttpRequestError&) {
    throw;
  } catch (const std::exception&) {
    throw HttpRequestError(400, "Request body must be valid JSON.",
                           Value::object({{"error", "Request body must be valid JSON."}}));
  }
}
