namespace {
void require_chat_api_key(const std::string& api_key,
                          const std::string& provider,
                          const std::string& env_key) {
  if (api_key.empty()) {
    throw ConfigurationError(provider + " adapter requires apiKey or " + env_key + ".");
  }
}

int optional_dimensions_from_settings(const Value& settings, int fallback) {
  if (settings.is_object() && settings.at("dimensions").is_number()) {
    return std::max(1, static_cast<int>(settings.at("dimensions").as_integer()));
  }
  return std::max(0, fallback);
}

std::string embedding_model_from_settings(const Value& settings, const std::string& fallback) {
  if (settings.is_object()) {
    const auto model = settings.at("model").as_string();
    if (!model.empty()) {
      return model;
    }
  }
  return fallback;
}

std::string embedding_task_type_from_settings(const Value& settings, const std::string& fallback) {
  if (settings.is_object()) {
    const auto task_type = settings.at("taskType").as_string(settings.at("task_type").as_string());
    if (!task_type.empty()) {
      return task_type;
    }
  }
  return fallback;
}

Value text_embedding_input_value(const std::vector<std::string>& texts) {
  Value::Array input;
  input.reserve(texts.size());
  for (const auto& text : texts) {
    input.push_back(text);
  }
  return Value(std::move(input));
}

EmbeddingVector embedding_vector_from_value(const Value& value) {
  EmbeddingVector vector;
  if (!value.is_array()) {
    return vector;
  }
  vector.reserve(value.as_array().size());
  for (const auto& item : value.as_array()) {
    vector.push_back(item.as_number());
  }
  return vector;
}

std::vector<EmbeddingVector> indexed_embedding_vectors_from_response(const Value& raw) {
  std::vector<std::pair<long long, EmbeddingVector>> indexed;
  for (const auto& item : raw.at("data").as_array()) {
    indexed.emplace_back(item.at("index").as_integer(static_cast<long long>(indexed.size())),
                         embedding_vector_from_value(item.at("embedding")));
  }
  std::sort(indexed.begin(), indexed.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(indexed.size());
  for (auto& entry : indexed) {
    vectors.push_back(std::move(entry.second));
  }
  return vectors;
}

std::vector<EmbeddingVector> embedding_vectors_from_array_response(const Value& value) {
  std::vector<EmbeddingVector> vectors;
  for (const auto& item : value.as_array()) {
    vectors.push_back(embedding_vector_from_value(item));
  }
  return vectors;
}

std::optional<int> optional_int_from_value(const Value& value, const std::string& key) {
  if (value.is_object() && value.at(key).is_number()) {
    return static_cast<int>(value.at(key).as_integer());
  }
  return std::nullopt;
}

std::optional<double> optional_number_from_value(const Value& value, const std::string& key) {
  if (value.is_object() && value.at(key).is_number()) {
    return value.at(key).as_number();
  }
  return std::nullopt;
}

std::string optional_string_from_value(const Value& value, const std::string& key, std::string fallback = {}) {
  if (value.is_object() && value.at(key).is_string()) {
    return value.at(key).as_string();
  }
  return fallback;
}

std::string compute_cache_fingerprint(const GenerateParams& params) {
  std::string material;
  for (const auto& message : params.messages) {
    if (message.role == MessageRole::System) {
      material += extract_text_content(message.content);
      material.push_back('\n');
    }
  }
  for (const auto& tool : params.tools) {
    material += serialize_chat_tool_descriptor(tool).stringify();
    material.push_back('\n');
  }
  return "agent:" + sha256_hex(material).substr(0, 16);
}

void apply_openai_prompt_cache_key(NativeProviderRequest& request, const GenerateParams& params) {
  if (params.settings.cache_strategy != CacheStrategy::Explicit) {
    return;
  }
  if (!request.body.is_object()) {
    return;
  }
  const std::string key = params.settings.cache_key.empty()
                              ? compute_cache_fingerprint(params)
                              : params.settings.cache_key;
  request.body["prompt_cache_key"] = key;
}

Value model_reasoning_to_value(const std::optional<ModelReasoning>& reasoning) {
  if (!reasoning) {
    return Value();
  }
  Value value = Value::object({{"text", reasoning->text}, {"format", reasoning->format}});
  return value;
}

Value llama_chat_result_to_value(const LlamaCppNativeChatResult& result) {
  Value value = Value::object({{"text", result.text}, {"finishReason", result.finish_reason}});
  if (!result.id.empty()) {
    value["id"] = result.id;
  }
  if (!result.model.empty()) {
    value["model"] = result.model;
  }
  if (result.reasoning) {
    value["reasoning"] = model_reasoning_to_value(result.reasoning);
  }
  return value;
}

std::string default_llama_model_name(const std::string& explicit_model,
                                     const std::string& model_path) {
  if (!explicit_model.empty()) {
    return explicit_model;
  }
  const auto name = path_basename(model_path);
  return name.empty() ? "llamacpp-native" : name;
}

std::set<std::string> llama_model_capabilities(const LlamaCppNativeRuntimeConfig& config) {
  std::set<std::string> capabilities{
      "input.text",
      "output.structuredContent",
      "transport.inline",
      "reasoning",
  };
  if (!config.mmproj_path.empty()) {
    capabilities.insert("input.image");
  }
  return capabilities;
}

class CancellationCallbackScope {
 public:
  CancellationCallbackScope(CancellationToken* token,
                            std::shared_ptr<LlamaCppNativeBinding> binding,
                            std::string request_id)
      : token_(token) {
    if (!token_) {
      return;
    }
    token_->throw_if_cancelled(ExecutionTarget::Model);
    if (binding && binding->cancel) {
      callback_id_ = token_->add_callback(
          [binding = std::move(binding), request_id = std::move(request_id)](const std::string&) {
            if (binding->cancel) {
              binding->cancel(request_id);
            }
          });
    }
  }

  CancellationCallbackScope(const CancellationCallbackScope&) = delete;
  CancellationCallbackScope& operator=(const CancellationCallbackScope&) = delete;

  ~CancellationCallbackScope() {
    if (token_ && callback_id_ != 0) {
      token_->remove_callback(callback_id_);
    }
  }

  void throw_if_cancelled() const {
    if (token_) {
      token_->throw_if_cancelled(ExecutionTarget::Model);
    }
  }

 private:
  CancellationToken* token_ = nullptr;
  std::size_t callback_id_ = 0;
};

std::string gbnf_terminal(const std::string& value) {
  return Value(value).stringify(0);
}

std::string join_strings(const std::vector<std::string>& values, const std::string& separator) {
  std::string output;
  for (const auto& value : values) {
    if (!output.empty()) {
      output += separator;
    }
    output += value;
  }
  return output;
}

std::string literal_json_value_gbnf(const Value& value) {
  return gbnf_terminal(value.stringify(0)) + " space";
}

std::optional<JsonSchemaType> inferred_schema_type(const JsonSchema& schema) {
  if (schema.type) {
    return schema.type;
  }
  if (!schema.properties.empty()) {
    return JsonSchemaType::Object;
  }
  if (schema.items) {
    return JsonSchemaType::Array;
  }
  return std::nullopt;
}

class LlamaGrammarBuilder {
 public:
  std::string add(std::string name, std::string expression) {
    rules_.push_back({name, expression});
    return name;
  }

  std::string value(const JsonSchema& schema) {
    const auto normalized = normalize_json_schema(schema);
    if (!normalized.enum_values.empty()) {
      std::vector<std::string> enum_items;
      enum_items.reserve(normalized.enum_values.size());
      for (const auto& item : normalized.enum_values) {
        enum_items.push_back(literal_json_value_gbnf(item));
      }
      return add(rule_name("enum"), join_strings(enum_items, " | "));
    }

    const auto type = inferred_schema_type(normalized);
    if (!type) {
      return "value";
    }
    switch (*type) {
      case JsonSchemaType::String:
        return "string";
      case JsonSchemaType::Number:
        return "number";
      case JsonSchemaType::Integer:
        return "integer";
      case JsonSchemaType::Boolean:
        return "boolean";
      case JsonSchemaType::Null:
        return "null";
      case JsonSchemaType::Array:
        return array(normalized);
      case JsonSchemaType::Object:
        return object(normalized);
    }
    return "value";
  }

  std::string build(const std::string& root_rule) const {
    std::vector<std::string> lines{
        "space ::= [ \\t\\n]{0,4}",
        "char ::= [^\"\\\\\\x7F\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]{4})",
        "string ::= \"\\\"\" char* \"\\\"\" space",
        "integer ::= \"-\"? ([0-9] | [1-9] [0-9]*) space",
        "number ::= \"-\"? ([0-9] | [1-9] [0-9]*) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)? space",
        "boolean ::= (\"true\" | \"false\") space",
        "null ::= \"null\" space",
        "array ::= \"[\" space (value (\",\" space value)*)? \"]\" space",
        "object ::= \"{\" space (string \":\" space value (\",\" space string \":\" space value)*)? \"}\" space",
        "value ::= object | array | string | number | boolean | null",
    };
    for (const auto& [name, expression] : rules_) {
      lines.push_back(name + " ::= " + expression);
    }
    lines.push_back("root ::= space " + root_rule + " space");
    return join_strings(lines, "\n");
  }

 private:
  std::string rule_name(const std::string& prefix) {
    ++counter_;
    return prefix + "-" + std::to_string(counter_);
  }

  std::string array(const JsonSchema& schema) {
    const auto item_rule = schema.items ? value(*schema.items) : std::string("value");
    return add(rule_name("array"),
               gbnf_terminal("[") + " space (" + item_rule + " (" + gbnf_terminal(",") + " space " +
                   item_rule + ")*)? " + gbnf_terminal("]") + " space");
  }

  std::string object(const JsonSchema& schema) {
    if (schema.properties.empty() ||
        static_cast<bool>(schema.additional_properties) ||
        schema.additional_properties_schema) {
      return "object";
    }

    std::set<std::string> required(schema.required.begin(), schema.required.end());
    std::vector<std::pair<std::string, JsonSchema>> properties;
    properties.reserve(schema.properties.size());
    for (const auto& [key, property_schema] : schema.properties) {
      properties.push_back({key, property_schema});
    }

    std::vector<std::pair<std::string, JsonSchema>> required_properties;
    std::vector<std::pair<std::string, JsonSchema>> optional_properties;
    for (const auto& property : properties) {
      if (required.find(property.first) != required.end()) {
        required_properties.push_back(property);
      } else {
        optional_properties.push_back(property);
      }
    }

    std::map<std::string, std::string> pair_expressions;
    for (const auto& [key, property_schema] : properties) {
      pair_expressions[key] = gbnf_terminal(Value(key).stringify(0)) + " space " +
                              gbnf_terminal(":") + " space " + value(property_schema);
    }

    std::vector<std::string> variants;
    const std::size_t combination_count = std::size_t{1} << optional_properties.size();
    for (std::size_t mask = 0; mask < combination_count; ++mask) {
      std::vector<std::string> pieces;
      for (const auto& [key, _] : required_properties) {
        pieces.push_back(pair_expressions.at(key));
      }
      for (std::size_t index = 0; index < optional_properties.size(); ++index) {
        if ((mask & (std::size_t{1} << index)) != 0) {
          pieces.push_back(pair_expressions.at(optional_properties[index].first));
        }
      }
      variants.push_back(pieces.empty() ? "space" : join_strings(pieces, " " + gbnf_terminal(",") + " space "));
    }
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    return add(rule_name("object"),
               gbnf_terminal("{") + " space (" + join_strings(variants, " | ") + ") " +
                   gbnf_terminal("}") + " space");
  }

  std::vector<std::pair<std::string, std::string>> rules_;
  int counter_ = 0;
};

std::optional<JsonSchema> resolve_llama_output_schema(const Value& settings) {
  if (!settings.is_object()) {
    return std::nullopt;
  }
  const auto& output_format = settings.at("outputFormat");
  if (output_format.is_object() &&
      output_format.at("type").as_string() == "json_schema" &&
      output_format.at("schema").is_object()) {
    return normalize_json_schema(json_schema_from_value(output_format.at("schema")));
  }
  if (settings.at("outputSchema").is_object()) {
    return normalize_json_schema(json_schema_from_value(settings.at("outputSchema")));
  }
  return std::nullopt;
}

bool valid_llama_tool_mode(const std::string& mode) {
  return mode == "auto" || mode == "required" || mode == "ignore" || mode == "error";
}

LlamaCppNativeToolMode resolve_llama_tool_mode(const LlamaCppNativeToolMode& configured,
                                               const Value& settings) {
  const auto requested = optional_string_from_value(settings, "toolMode",
                         optional_string_from_value(settings, "tool_mode"));
  if (valid_llama_tool_mode(requested)) {
    return requested;
  }
  return valid_llama_tool_mode(configured) ? configured : "auto";
}

Value llama_tools_to_value(const std::vector<ChatToolDescriptor>& tools) {
  Value::Array values;
  values.reserve(tools.size());
  for (const auto& tool : tools) {
    values.push_back(Value::object({
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", json_schema_to_value(tool.input_schema)},
    }));
  }
  return Value(std::move(values));
}

std::string build_llama_tool_instruction(const std::vector<ChatToolDescriptor>& tools,
                                         const LlamaCppNativeToolMode& tool_mode) {
  return join_strings({
      "You have access to tools. Return only compact JSON with no markdown.",
      tool_mode == "required" ? "You must call exactly one tool."
                              : "If a tool is not needed, return a final answer envelope.",
      "Tool call envelope: {\"type\":\"tool_call\",\"name\":\"tool_name\",\"arguments\":{...}}",
      "Final answer envelope: {\"type\":\"final\",\"text\":\"answer text\"}",
      "Available tools: " + llama_tools_to_value(tools).stringify(0),
  }, "\n");
}

std::string build_llama_structured_output_instruction(const JsonSchema& schema) {
  return join_strings({
      "Return only compact JSON matching this JSON Schema.",
      json_schema_to_value(schema).stringify(0),
  }, "\n");
}

std::string serialize_llama_tool_calls(const std::vector<ToolCall>& tool_calls) {
  Value::Array calls;
  calls.reserve(tool_calls.size());
  for (const auto& tool_call : tool_calls) {
    calls.push_back(Value::object({
        {"id", tool_call.id},
        {"name", tool_call.name},
        {"arguments", tool_call.arguments},
    }));
  }
  return Value::object({{"type", "tool_calls"}, {"calls", Value(std::move(calls))}}).stringify(0);
}

bool is_llama_image_mime_type(const std::string& mime_type) {
  return mime_type.rfind("image/", 0) == 0;
}

LlamaCppNativeChatMedia resolve_llama_chat_media(const MessageContentPart& part,
                                                 std::size_t index) {
  const auto resolved = DefaultMediaResolver().resolve(part.source);
  const std::string mime_type = resolved.mime_type.empty() ? part.source.mime_type : resolved.mime_type;
  if (!is_llama_image_mime_type(mime_type)) {
    throw AdapterError(
        "llama.cpp native multimodal input only accepts image files. "
        "Preprocess non-image files into text before model invocation.");
  }
  if (resolved.bytes.empty()) {
    throw AdapterError("llama.cpp native image input is empty.");
  }
  std::string id = part.source.key;
  if (id.empty()) id = part.source.path;
  if (id.empty()) id = part.source.url;
  if (id.empty()) id = resolved.filename;
  if (id.empty()) id = "image-" + std::to_string(index + 1);
  return LlamaCppNativeChatMedia{
      .bytes = resolved.bytes,
      .mime_type = mime_type,
      .id = std::move(id),
      .filename = resolved.filename,
  };
}

struct SerializedLlamaMessage {
  LlamaCppNativeChatMessage message;
  std::vector<LlamaCppNativeChatMedia> media;
};

SerializedLlamaMessage serialize_llama_message(const AgentMessage& message,
                                               bool multimodal_enabled,
                                               const std::string& media_marker) {
  std::string text;
  std::vector<LlamaCppNativeChatMedia> media;
  for (const auto& part : message.content) {
    if (part.type == ContentPartType::Text) {
      text += part.text;
      continue;
    }
    if (part.type == ContentPartType::Image || part.type == ContentPartType::File) {
      if (!multimodal_enabled) {
        throw AdapterError("llama.cpp native image inputs requires mmproj_path.");
      }
      if (message.role != MessageRole::User) {
        throw AdapterError("llama.cpp native image inputs are only supported on user messages.");
      }
      media.push_back(resolve_llama_chat_media(part, media.size()));
      text += media_marker.empty() ? std::string("<__media__>") : media_marker;
      continue;
    }
    if (part.type == ContentPartType::Audio || part.type == ContentPartType::Video) {
      throw AdapterError("llama.cpp native does not support " +
                         content_part_type_label(part.type) + " inputs.");
    }
  }
  if (message.role == MessageRole::Tool) {
    std::string prefix = "Tool result";
    if (!message.name.empty()) {
      prefix += " from " + message.name;
    }
    if (!message.tool_call_id.empty()) {
      prefix += " (" + message.tool_call_id + ")";
    }
    return SerializedLlamaMessage{
        .message = LlamaCppNativeChatMessage{.role = "user", .content = prefix + ":\n" + text},
        .media = std::move(media),
    };
  }
  if (message.role != MessageRole::System && message.role != MessageRole::User &&
      message.role != MessageRole::Assistant) {
    throw AdapterError("llama.cpp native provider only supports system, user, and assistant messages.");
  }
  std::string content = text;
  if (!message.tool_calls.empty()) {
    content += "\n\nAssistant tool calls:\n" + serialize_llama_tool_calls(message.tool_calls);
  }
  return SerializedLlamaMessage{
      .message = LlamaCppNativeChatMessage{
          .role = message_role_label(message.role),
          .content = std::move(content),
      },
      .media = std::move(media),
  };
}

Value parse_llama_json_object(const std::string& text) {
  try {
    auto value = parse_json(trim_copy(text));
    return value.is_object() ? value : Value();
  } catch (const std::exception&) {
    return Value();
  }
}

Value parse_llama_tool_arguments(const Value& value) {
  if (value.is_object()) {
    return value;
  }
  if (value.is_string()) {
    auto parsed = parse_llama_json_object(value.as_string());
    return parsed.is_object() ? parsed : Value::object({{"raw", value.as_string()}});
  }
  return Value::object({});
}

void validate_llama_structured_output(const std::string& text, const JsonSchema& schema) {
  try {
    assert_json_schema(schema, parse_json(trim_copy(text)));
  } catch (const SchemaValidationError&) {
    throw;
  } catch (const std::exception& error) {
    throw AdapterError("llama.cpp native structured output was not valid JSON.", error.what());
  }
}

struct LlamaToolOutput {
  std::string text;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
};

LlamaToolOutput parse_llama_tool_envelope(const std::string& text,
                                          const std::vector<ChatToolDescriptor>& tools,
                                          const LlamaCppNativeToolMode& tool_mode,
                                          const std::optional<JsonSchema>& output_schema) {
  if (tools.empty() || tool_mode == "ignore") {
    if (output_schema) {
      validate_llama_structured_output(text, *output_schema);
    }
    return LlamaToolOutput{.text = text};
  }

  const auto parsed = parse_llama_json_object(text);
  if (!parsed.is_object()) {
    if (tool_mode == "required") {
      throw AdapterError("llama.cpp native provider expected a tool-call JSON envelope.");
    }
    return LlamaToolOutput{.text = text};
  }

  if (parsed.at("type").as_string() == "final") {
    const auto final_text = parsed.at("text").as_string();
    if (output_schema) {
      validate_llama_structured_output(final_text, *output_schema);
    }
    return LlamaToolOutput{.text = final_text};
  }

  if (parsed.at("type").as_string() != "tool_call") {
    if (tool_mode == "required") {
      throw AdapterError("llama.cpp native provider returned a JSON envelope without a tool call.");
    }
    return LlamaToolOutput{.text = text};
  }

  const auto name = parsed.at("name").as_string();
  const auto found = std::find_if(tools.begin(), tools.end(), [&](const ChatToolDescriptor& tool) {
    return tool.name == name;
  });
  if (found == tools.end()) {
    throw AdapterError("llama.cpp native provider returned an unknown tool call: " +
                       (name.empty() ? std::string("unknown") : name) + ".");
  }
  auto arguments = parse_llama_tool_arguments(parsed.at("arguments"));
  assert_json_schema(found->input_schema, arguments);
  return LlamaToolOutput{
      .tool_calls = {ToolCall{parsed.at("id").as_string(name + "_1"), name, std::move(arguments)}},
      .finish_reason = "tool_calls",
  };
}

}  // namespace

std::string json_schema_to_gbnf(const JsonSchema& schema) {
  LlamaGrammarBuilder builder;
  return builder.build(builder.value(schema));
}

std::string llama_tool_envelope_gbnf(const std::vector<ChatToolDescriptor>& tools,
                                     bool required_only) {
  LlamaGrammarBuilder builder;
  const auto final = gbnf_terminal("{") + " space " +
      gbnf_terminal("\"type\"") + " space " + gbnf_terminal(":") + " space " +
      gbnf_terminal("\"final\"") + " space " + gbnf_terminal(",") + " space " +
      gbnf_terminal("\"text\"") + " space " + gbnf_terminal(":") + " space string " +
      gbnf_terminal("}") + " space";
  std::vector<std::string> tool_calls;
  tool_calls.reserve(tools.size());
  for (const auto& tool : tools) {
    const auto args_rule = builder.value(normalize_json_schema(tool.input_schema));
    tool_calls.push_back(gbnf_terminal("{") + " space " +
                         gbnf_terminal("\"type\"") + " space " + gbnf_terminal(":") + " space " +
                         gbnf_terminal("\"tool_call\"") + " space " + gbnf_terminal(",") + " space " +
                         gbnf_terminal("\"name\"") + " space " + gbnf_terminal(":") + " space " +
                         gbnf_terminal(Value(tool.name).stringify(0)) + " space " +
                         gbnf_terminal(",") + " space " +
                         gbnf_terminal("\"arguments\"") + " space " + gbnf_terminal(":") + " space " +
                         args_rule + " " + gbnf_terminal("}") + " space");
  }
  if (!required_only) {
    tool_calls.push_back(final);
  }
  return builder.build("(" + join_strings(tool_calls, " | ") + ")");
}
