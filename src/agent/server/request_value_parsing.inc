AgentServerCitationGovernanceConfig citation_governance_from_value(const Value& value) {
  AgentServerCitationGovernanceConfig config;
  if (value.is_bool()) {
    config.mode = value.as_bool() ? "require" : "off";
    return config;
  }
  if (!value.is_object()) {
    return config;
  }
  config.mode = value.at("mode").as_string("require");
  if (value.at("appendToText").is_bool()) {
    config.append_to_text = value.at("appendToText").as_bool();
  } else if (value.at("append_to_text").is_bool()) {
    config.append_to_text = value.at("append_to_text").as_bool();
  }
  if (value.at("maxSources").is_number()) {
    config.max_sources = static_cast<int>(value.at("maxSources").as_integer(3));
  } else if (value.at("max_sources").is_number()) {
    config.max_sources = static_cast<int>(value.at("max_sources").as_integer(3));
  }
  config.header = value.at("header").as_string("Sources");
  return config;
}

std::string optional_string_field(const Value& body, const std::string& field) {
  if (!body.contains(field) || body.at(field).is_null()) {
    return {};
  }
  if (!body.at(field).is_string()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a string.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a string."},
                                          {"field", field}}));
  }
  return body.at(field).as_string();
}

std::string optional_string_field_strict(const Value& body, const std::string& field) {
  if (!body.contains(field)) {
    return {};
  }
  if (!body.at(field).is_string()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a string.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a string."},
                                          {"field", field}}));
  }
  return body.at(field).as_string();
}

Value optional_object_field(const Value& body, const std::string& field) {
  if (!body.contains(field) || body.at(field).is_null()) {
    return Value::object({});
  }
  if (!body.at(field).is_object()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a JSON object."},
                                          {"field", field}}));
  }
  return body.at(field);
}

Value optional_object_field_strict(const Value& body, const std::string& field) {
  if (!body.contains(field)) {
    return Value::object({});
  }
  if (!body.at(field).is_object()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a JSON object."},
                                          {"field", field}}));
  }
  return body.at(field);
}

bool optional_bool_field_strict(const Value& body, const std::string& field, bool fallback = false) {
  if (!body.contains(field)) {
    return fallback;
  }
  if (!body.at(field).is_bool()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a boolean.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a boolean."},
                                          {"field", field}}));
  }
  return body.at(field).as_bool();
}

std::optional<std::string> optional_reason_field(const Value& body) {
  if (!body.contains("reason")) {
    return std::nullopt;
  }
  if (!body.at("reason").is_string()) {
    throw HttpRequestError(400, "Request body field \"reason\" must be a string.",
                           Value::object({{"error", "Request body field \"reason\" must be a string."},
                                          {"field", "reason"}}));
  }
  return body.at("reason").as_string();
}
