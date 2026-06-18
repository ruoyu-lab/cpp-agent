struct NormalizedPiiPolicy {
  bool enabled = false;
  std::vector<std::string> detectors;
  std::string replacement = "[REDACTED]";
};

struct NormalizedCitationPolicy {
  std::string mode = "off";
  bool append_to_text = false;
  int max_sources = 3;
  std::string header = "Sources";
};

struct ResolvedGovernancePolicy {
  std::optional<NormalizedPiiPolicy> pii;
  NormalizedCitationPolicy citations;
  std::optional<JsonSchema> output_schema;
};

struct GovernedRun {
  std::string output;
  Value result = Value::object({});
  AgentServerGovernanceSummary summary;
};

std::optional<NormalizedPiiPolicy> normalize_pii_policy(
    const std::optional<AgentServerPiiGovernanceConfig>& policy) {
  if (!policy || !policy->enabled) {
    return std::nullopt;
  }
  NormalizedPiiPolicy normalized;
  normalized.enabled = true;
  normalized.detectors = policy->detectors.empty()
                             ? std::vector<std::string>{"email", "phone", "credit-card"}
                             : policy->detectors;
  normalized.replacement = policy->replacement.empty() ? "[REDACTED]" : policy->replacement;
  return normalized;
}

NormalizedCitationPolicy normalize_citation_policy(
    const std::optional<AgentServerCitationGovernanceConfig>& policy,
    bool has_output_schema) {
  if (!policy) {
    return {};
  }
  NormalizedCitationPolicy normalized;
  normalized.mode = policy->mode.empty() ? "require" : policy->mode;
  normalized.max_sources = policy->max_sources <= 0 ? 3 : policy->max_sources;
  normalized.header = policy->header.empty() ? "Sources" : policy->header;
  normalized.append_to_text = policy->append_to_text.value_or(normalized.mode == "require" && !has_output_schema);
  return normalized;
}

std::optional<AgentServerRequestGovernanceConfig> request_governance_from_body(const Value& body) {
  if (!body.contains("governance") || body.at("governance").is_null()) {
    return std::nullopt;
  }
  if (!body.at("governance").is_object()) {
    throw HttpRequestError(400, "Request body field \"governance\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"governance\" must be a JSON object."},
                                          {"field", "governance"}}));
  }
  AgentServerRequestGovernanceConfig config;
  const auto& governance = body.at("governance");
  if (governance.contains("citations")) {
    config.citations = citation_governance_from_value(governance.at("citations"));
  }
  if (governance.at("outputSchema").is_object()) {
    config.output_schema = json_schema_from_server_value(governance.at("outputSchema"));
  }
  return config;
}

ResolvedGovernancePolicy resolve_governance_policy(
    const std::optional<AgentServerGovernanceConfig>& options,
    const std::optional<AgentServerRequestGovernanceConfig>& request) {
  std::optional<JsonSchema> output_schema;
  if (request && request->output_schema) {
    output_schema = request->output_schema;
  } else if (options && options->output_schema) {
    output_schema = options->output_schema;
  }
  std::optional<AgentServerCitationGovernanceConfig> citations;
  if (request && request->citations) {
    citations = request->citations;
  } else if (options && options->citations) {
    citations = options->citations;
  }
  return ResolvedGovernancePolicy{
      .pii = options ? normalize_pii_policy(options->pii) : std::nullopt,
      .citations = normalize_citation_policy(citations, output_schema.has_value()),
      .output_schema = std::move(output_schema),
  };
}

Value normalized_citation_policy_to_value(const NormalizedCitationPolicy& policy) {
  return Value::object({{"mode", policy.mode},
                        {"appendToText", policy.append_to_text},
                        {"maxSources", policy.max_sources},
                        {"header", policy.header}});
}

std::string redact_text(std::string value, const NormalizedPiiPolicy& policy) {
  const auto replace_if_enabled = [&](const std::string& detector, const std::regex& pattern) {
    if (contains_string(policy.detectors, detector)) {
      value = std::regex_replace(value, pattern, policy.replacement);
    }
  };
  replace_if_enabled("email", std::regex(R"([A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,})",
                                         std::regex_constants::icase));
  replace_if_enabled("phone", std::regex(R"((\+?\d[\d().\s-]{7,}\d))"));
  replace_if_enabled("credit-card", std::regex(R"(\b(?:\d[ -]*?){13,19}\b)"));
  return value;
}

Value redact_value_strings(const Value& value, const NormalizedPiiPolicy& policy) {
  if (value.is_string()) {
    return redact_text(value.as_string(), policy);
  }
  if (value.is_array()) {
    Value::Array output;
    for (const auto& item : value.as_array()) {
      output.push_back(redact_value_strings(item, policy));
    }
    return Value(std::move(output));
  }
  if (value.is_object()) {
    Value::Object output;
    for (const auto& [key, item] : value.as_object()) {
      output[key] = redact_value_strings(item, policy);
    }
    return Value(std::move(output));
  }
  return value;
}

std::vector<Value> collect_citations(const Value& result) {
  std::vector<Value> citations;
  for (const auto& hit : result.at("knowledgeHits").as_array()) {
    if (hit.at("citation").is_object()) {
      citations.push_back(hit.at("citation"));
    }
  }
  for (const auto& hit : result.at("memoryHits").as_array()) {
    if (hit.at("metadata").at("citation").is_object()) {
      citations.push_back(hit.at("metadata").at("citation"));
    } else if (hit.at("metadata").contains("documentId") || hit.at("metadata").contains("chunkId")) {
      citations.push_back(Value::object({
          {"documentId", hit.at("metadata").at("documentId").as_string(hit.at("id").as_string())},
          {"chunkId", hit.at("metadata").at("chunkId").as_string(hit.at("id").as_string())},
          {"title", hit.at("metadata").at("title").as_string(hit.at("id").as_string())},
          {"uri", hit.at("metadata").at("uri").as_string()},
      }));
    }
  }
  return citations;
}

std::pair<std::string, int> build_citation_appendix(const Value& result,
                                                    const NormalizedCitationPolicy& policy) {
  std::set<std::string> seen;
  std::vector<std::string> lines;
  for (const auto& citation : collect_citations(result)) {
    const auto document_id = citation.at("documentId").as_string(
        citation.at("document_id").as_string(citation.at("id").as_string("unknown")));
    const auto chunk_id = citation.at("chunkId").as_string(
        citation.at("chunk_id").as_string(citation.at("chunkIndex").as_string()));
    const auto key = document_id + ":" + chunk_id;
    if (seen.find(key) != seen.end()) {
      continue;
    }
    seen.insert(key);
    const auto title = citation.at("title").as_string(document_id);
    const auto uri = citation.at("uri").as_string();
    lines.push_back("[" + std::to_string(lines.size() + 1) + "] " + title +
                    (uri.empty() ? "" : " (" + uri + ")"));
    if (static_cast<int>(lines.size()) >= policy.max_sources) {
      break;
    }
  }
  if (lines.empty()) {
    return {"", 0};
  }
  return {policy.header + ":\n" + join_values(lines, "\n"), static_cast<int>(lines.size())};
}

void set_governed_output(Value& result, const std::string& output, bool redact_raw) {
  result["text"] = output;
  result["response"]["text"] = output;
  if (redact_raw) {
    result["response"]["raw"] = Value();
  }
}

GovernedRun apply_governance_to_run(std::string output,
                                    Value result,
                                    const ResolvedGovernancePolicy& policy) {
  AgentServerGovernanceSummary summary;
  std::vector<std::string> violations;
  const auto [citation_appendix, citation_count] = build_citation_appendix(result, policy.citations);
  summary.citation_count = citation_count;
  if (policy.citations.mode == "require" && citation_count == 0) {
    violations.push_back("Citation policy requires at least one knowledge citation.");
  }

  std::optional<Value> structured_output;
  if (policy.output_schema) {
    try {
      structured_output = parse_json(output);
    } catch (const std::exception&) {
      violations.push_back("Output schema validation requires JSON output.");
    }
  }

  if (structured_output && policy.pii) {
    auto redacted = redact_value_strings(*structured_output, *policy.pii);
    summary.redacted = summary.redacted || redacted != *structured_output;
    structured_output = std::move(redacted);
  }

  if (policy.output_schema && structured_output) {
    const auto issues = validate_json_schema(*policy.output_schema, *structured_output);
    if (!issues.empty()) {
      for (const auto& issue : issues) {
        violations.push_back(issue.path + ": " + issue.message);
      }
    } else {
      summary.schema_validated = true;
    }
    output = structured_output->stringify(2);
  }

  if (policy.citations.mode == "require" && policy.citations.append_to_text &&
      !citation_appendix.empty() && !policy.output_schema) {
    output += "\n\n" + citation_appendix;
    summary.citations_appended = true;
  }

  if (policy.pii && !policy.output_schema) {
    const auto redacted = redact_text(output, *policy.pii);
    summary.redacted = summary.redacted || redacted != output;
    output = redacted;
  }

  if (!violations.empty()) {
    Value::Array violation_values;
    for (const auto& violation : violations) {
      violation_values.push_back(violation);
    }
    throw HttpRequestError(422, "Output policy violation.",
                           Value::object({{"error", "Output policy violation."},
                                          {"violations", Value(std::move(violation_values))}}));
  }

  const bool redact_raw = policy.pii.has_value() || summary.citations_appended || summary.schema_validated;
  set_governed_output(result, output, redact_raw);
  if (policy.pii) {
    result = redact_value_strings(result, *policy.pii);
    set_governed_output(result, output, true);
  }
  return GovernedRun{.output = std::move(output), .result = std::move(result), .summary = summary};
}

void add_trace_payload(Value& payload, const AgentServerRequestContext& context) {
  if (!payload.is_object()) {
    return;
  }
  if (!context.access.request_id.empty()) {
    payload["requestId"] = context.access.request_id;
  }
  if (!context.access.trace_context.trace_id.empty()) {
    payload["traceId"] = context.access.trace_context.trace_id;
  }
}

Value merge_server_execution_context(Value context,
                                     const AgentServerRequestContext& request_context,
                                     std::string span_name,
                                     Value details = Value::object({})) {
  Value output = context.is_object() ? std::move(context) : Value::object({});
  TraceContext trace = create_child_trace_context(request_context.access.trace_context, TraceContext{
      .span_name = span_name.empty() ? "server.execution" : std::move(span_name),
  });
  output["traceContext"] = trace_context_to_value(trace);
  output["traceId"] = trace.trace_id;
  if (!request_context.access.request_id.empty()) {
    output["requestId"] = request_context.access.request_id;
  }

  Value server = output.at("server").is_object() ? output.at("server") : Value::object({});
  server["path"] = request_context.path;
  if (!request_context.access.request_id.empty()) {
    server["requestId"] = request_context.access.request_id;
  }
  server["apiKeyId"] = request_context.access.api_key ? Value(request_context.access.api_key->id) : Value();
  if (details.is_object()) {
    for (const auto& [key, value] : details.as_object()) {
      server[key] = value;
    }
  }
  output["server"] = server;
  return output;
}
