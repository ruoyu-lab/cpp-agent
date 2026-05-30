#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace agent {

namespace {

int dimensions_from_settings(const Value& settings, int fallback) {
  if (settings.is_object() && settings.at("dimensions").is_number()) {
    return std::max(1, static_cast<int>(settings.at("dimensions").as_integer()));
  }
  return std::max(1, fallback);
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

EmbeddingSettings embedding_settings_from_value(const Value& value) {
  EmbeddingSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.at("dimensions").is_number()) {
    settings.dimensions = std::max(1, static_cast<int>(value.at("dimensions").as_integer()));
  }
  settings.task_type = value.at("taskType").as_string(value.at("task_type").as_string());
  settings.space_id = value.at("spaceId").as_string(value.at("space_id").as_string());
  settings.extra = value;
  return settings;
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

std::string image_source_hint(const MediaSource& source) {
  if (source.kind == MediaSourceKind::Inline) {
    if (!source.filename.empty()) {
      return source.filename;
    }
    return source.mime_type;
  }
  if (source.kind == MediaSourceKind::Artifact) {
    return source.key;
  }
  if (source.kind == MediaSourceKind::Path) {
    return source.path;
  }
  return source.url;
}

std::string image_to_hint(const ImageEmbeddingInput& image) {
  std::vector<std::string> parts{image.title, image.alt_text, image.text_hint, image_source_hint(image.source)};
  std::string output;
  for (const auto& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!output.empty()) {
      output += " ";
    }
    output += part;
  }
  return output;
}

std::size_t trailing_tag_prefix_length(const std::string& value, const std::string& tag) {
  const auto max_length = std::min(value.size(), tag.empty() ? std::size_t{0} : tag.size() - 1);
  for (std::size_t length = max_length; length > 0; --length) {
    if (value.compare(value.size() - length, length, tag, 0, length) == 0) {
      return length;
    }
  }
  return 0;
}

EmbeddingSettings merge_embedding_settings(std::string provider, std::string model, int dimensions,
                                           std::string space_id, EmbeddingSettings settings,
                                           std::vector<std::string> modalities) {
  if (settings.model.empty()) {
    settings.model = std::move(model);
  }
  if (!settings.dimensions) {
    settings.dimensions = dimensions;
  }
  if (settings.space_id.empty()) {
    settings.space_id = space_id.empty() ? provider + ":" + settings.model : std::move(space_id);
  }
  settings.extra["modalities"] = Value::array({});
  for (const auto& modality : modalities) {
    settings.extra["modalities"].as_array().push_back(modality);
  }
  return settings;
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

void require_chat_api_key(const std::string& api_key,
                          const std::string& provider,
                          const std::string& env_key) {
  if (api_key.empty()) {
    throw ConfigurationError(provider + " adapter requires apiKey or " + env_key + ".");
  }
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

ModelUsage extract_model_usage(const Value& response_or_raw, std::string provider) {
  const Value* raw = &response_or_raw;
  if (response_or_raw.is_object() && response_or_raw.contains("raw")) {
    raw = &response_or_raw.at("raw");
  }

  const Value* usage = raw;
  if (raw->is_object() && raw->at("usage").is_object()) {
    usage = &raw->at("usage");
  } else if (raw->is_object() && raw->at("usage_metadata").is_object()) {
    usage = &raw->at("usage_metadata");
  } else if (raw->is_object() && raw->at("usageMetadata").is_object()) {
    usage = &raw->at("usageMetadata");
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

  int input_tokens = read_first({"inputTokens", "input_tokens", "promptTokens",
                                 "prompt_tokens", "promptTokenCount", "prompt_token_count"})
                         .value_or(0);
  int output_tokens = read_first({"outputTokens", "output_tokens", "completionTokens",
                                  "completion_tokens", "candidatesTokenCount",
                                  "candidates_token_count"})
                          .value_or(0);

  // Cached input tokens — provider-reported hit count.
  // Anthropic: usage.cache_read_input_tokens (also cache_creation_input_tokens for writes).
  // OpenAI:    usage.prompt_tokens_details.cached_tokens.
  // Gemini:    usage_metadata.cached_content_token_count (auto-cache only).
  int cached_input_tokens = 0;
  int cache_creation_tokens = 0;
  if (usage->is_object()) {
    cached_input_tokens = read_first({"cache_read_input_tokens",
                                      "cacheReadInputTokens",
                                      "cached_input_tokens",
                                      "cachedInputTokens",
                                      "cached_content_token_count",
                                      "cachedContentTokenCount"})
                              .value_or(0);
    cache_creation_tokens = read_first({"cache_creation_input_tokens",
                                        "cacheCreationInputTokens"})
                                .value_or(0);
    if (cached_input_tokens == 0) {
      const auto& details = usage->at("prompt_tokens_details");
      if (details.is_object() && details.at("cached_tokens").is_number()) {
        cached_input_tokens = static_cast<int>(details.at("cached_tokens").as_integer());
      } else {
        const auto& details_camel = usage->at("promptTokensDetails");
        if (details_camel.is_object() && details_camel.at("cachedTokens").is_number()) {
          cached_input_tokens = static_cast<int>(details_camel.at("cachedTokens").as_integer());
        }
      }
    }
  }

  // For Anthropic, the reported `input_tokens` is the UNCACHED portion only.
  // Roll cached + cache-creation back in so input_tokens represents total prompt size
  // (so downstream hitRate = cached / total math works).
  if ((provider == "anthropic" || provider == "deepseek") &&
      (cached_input_tokens > 0 || cache_creation_tokens > 0)) {
    input_tokens += cached_input_tokens + cache_creation_tokens;
  }

  // Reasoning tokens. Order: provider-specific shape first; estimation fallback
  // happens only in the ModelResponse overload (which has access to response.reasoning).
  int reasoning_tokens = 0;
  ReasoningSource reasoning_source = ReasoningSource::Unknown;

  const auto read_openai_compat_reasoning = [&]() -> std::optional<int> {
    if (!usage->is_object()) {
      return std::nullopt;
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

  if (provider == "gemini") {
    const auto thoughts = read_first({"thoughts_token_count", "thoughtsTokenCount"}).value_or(0);
    if (thoughts > 0) {
      reasoning_tokens = thoughts;
      reasoning_source = ReasoningSource::Provider;
      // Gemini's candidates_token_count excludes thoughts; combine them so
      // output_tokens reflects everything the model produced after the prompt.
      output_tokens += thoughts;
    }
  } else if (provider == "qwen") {
    if (const auto compat = read_openai_compat_reasoning()) {
      reasoning_tokens = *compat;
      reasoning_source = ReasoningSource::Provider;
    } else if (raw->is_object()) {
      // DashScope shape: output.reasoning_content_tokens (snake/camel).
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
  } else if (provider == "anthropic") {
    // Anthropic does not report reasoning token counts; estimation path on the
    // ModelResponse overload may fill it in.
  } else {
    // openai, deepseek, ollama, llama-cpp-native, unrecognized: try OpenAI-compat shape first.
    if (const auto compat = read_openai_compat_reasoning()) {
      reasoning_tokens = *compat;
      reasoning_source = ReasoningSource::Provider;
    }
  }

  const int total_tokens = read_first({"totalTokens", "total_tokens", "totalTokenCount",
                                       "total_token_count"})
                               .value_or(input_tokens + output_tokens);
  ModelUsage result;
  result.input_tokens = input_tokens;
  result.output_tokens = output_tokens;
  result.total_tokens = total_tokens;
  result.cached_input_tokens = cached_input_tokens;
  result.reasoning_tokens = reasoning_tokens;
  result.reasoning_source = reasoning_source;
  result.provider = std::move(provider);
  return result;
}

ModelUsage extract_model_usage(const ModelResponse& response) {
  auto usage = extract_model_usage(response.raw, response.provider);

  // Estimation fallback: if no provider-reported reasoning count, but the
  // ModelResponse carries reasoning text, estimate as chars / 4. This also
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
    if (merged.provider.empty()) {
      merged.provider = usage.provider;
    }
    merged.reasoning_source = any ? merge_reasoning_source(merged.reasoning_source, usage.reasoning_source)
                                  : usage.reasoning_source;
    any = true;
  }
  return merged;
}

ChatModelAdapter::ChatModelAdapter(std::string provider, std::string model, double temperature,
                                   int max_output_tokens, std::set<std::string> capabilities,
                                   std::optional<ReasoningSettings> reasoning)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      temperature_(temperature),
      max_output_tokens_(max_output_tokens),
      capabilities_(std::move(capabilities)),
      default_reasoning_(std::move(reasoning)) {
  if (provider_.empty()) {
    throw ConfigurationError("Adapter provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Adapter model is required.");
  }
}

const std::string& ChatModelAdapter::provider() const noexcept {
  return provider_;
}

const std::string& ChatModelAdapter::model() const noexcept {
  return model_;
}

const std::set<std::string>& ChatModelAdapter::capabilities() const noexcept {
  return capabilities_;
}

void ChatModelAdapter::set_capabilities(std::set<std::string> capabilities) {
  capabilities_ = std::move(capabilities);
}

void assert_reasoning_settings(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be a JSON object.");
  }
  for (const auto& [key, _] : value.as_object()) {
    if (key != "enabled" && key != "budget" && key != "includeThoughts" && key != "tagName") {
      throw ConfigurationError(path + "." + key + " is not a recognized reasoning field.");
    }
  }
  if (value.contains("enabled") && !value.at("enabled").is_bool()) {
    throw ConfigurationError(path + ".enabled must be a boolean.");
  }
  if (value.contains("budget")) {
    const auto& budget = value.at("budget");
    if (budget.is_string()) {
      const auto effort = budget.as_string();
      if (effort != "low" && effort != "medium" && effort != "high") {
        throw ConfigurationError(path + ".budget must be one of \"low\", \"medium\", \"high\" "
                                 "or a positive finite number.");
      }
    } else if (budget.is_number()) {
      const auto tokens = budget.as_number();
      if (!std::isfinite(tokens) || tokens <= 0) {
        throw ConfigurationError(path + ".budget must be a positive finite number when given as a token count.");
      }
    } else {
      throw ConfigurationError(path + ".budget must be a string bucket or a positive finite number.");
    }
  }
  if (value.contains("includeThoughts") && !value.at("includeThoughts").is_bool()) {
    throw ConfigurationError(path + ".includeThoughts must be a boolean.");
  }
  if (value.contains("tagName") &&
      (!value.at("tagName").is_string() || value.at("tagName").as_string().empty())) {
    throw ConfigurationError(path + ".tagName must be a non-empty string.");
  }
}

Value reasoning_settings_to_json_value(const ReasoningSettings& settings) {
  Value::Object object;
  if (settings.enabled) {
    object["enabled"] = *settings.enabled;
  }
  if (std::holds_alternative<std::string>(settings.budget)) {
    object["budget"] = std::get<std::string>(settings.budget);
  } else if (std::holds_alternative<double>(settings.budget)) {
    object["budget"] = std::get<double>(settings.budget);
  }
  if (settings.include_thoughts) {
    object["includeThoughts"] = *settings.include_thoughts;
  }
  if (!settings.tag_name.empty()) {
    object["tagName"] = settings.tag_name;
  }
  return Value(std::move(object));
}

ReasoningSettings reasoning_settings_from_json_value(const Value& value) {
  ReasoningSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  if (value.contains("enabled")) {
    settings.enabled = value.at("enabled").as_bool();
  }
  const auto& budget = value.at("budget");
  if (budget.is_string()) {
    settings.budget = budget.as_string();
  } else if (budget.is_number()) {
    settings.budget = budget.as_number();
  }
  if (value.contains("includeThoughts")) {
    settings.include_thoughts = value.at("includeThoughts").as_bool();
  } else if (value.contains("include_thoughts")) {
    settings.include_thoughts = value.at("include_thoughts").as_bool();
  }
  settings.tag_name = value.at("tagName").as_string(value.at("tag_name").as_string());
  return settings;
}

std::optional<ReasoningSettings> merge_reasoning_settings(
    const std::optional<ReasoningSettings>& base,
    const std::optional<ReasoningSettings>& override_settings) {
  if (!base && !override_settings) {
    return std::nullopt;
  }
  ReasoningSettings merged = base ? *base : ReasoningSettings{};
  if (override_settings) {
    if (override_settings->enabled) {
      merged.enabled = override_settings->enabled;
    }
    if (!std::holds_alternative<std::monostate>(override_settings->budget)) {
      merged.budget = override_settings->budget;
    }
    if (override_settings->include_thoughts) {
      merged.include_thoughts = override_settings->include_thoughts;
    }
    if (!override_settings->tag_name.empty()) {
      merged.tag_name = override_settings->tag_name;
    }
  }
  return merged;
}

std::string normalize_reasoning_budget_to_effort(
    const std::variant<std::monostate, std::string, double>& budget) {
  if (std::holds_alternative<std::string>(budget)) {
    return std::get<std::string>(budget);
  }
  if (!std::holds_alternative<double>(budget)) {
    return "medium";
  }
  const auto tokens = std::get<double>(budget);
  if (tokens < 2560) {
    return "low";
  }
  if (tokens < 10240) {
    return "medium";
  }
  return "high";
}

int normalize_reasoning_budget_to_tokens(
    const std::variant<std::monostate, std::string, double>& budget,
    ReasoningBudgetTokenTable table) {
  if (std::holds_alternative<double>(budget)) {
    return static_cast<int>(std::get<double>(budget));
  }
  if (std::holds_alternative<std::string>(budget)) {
    const auto effort = std::get<std::string>(budget);
    if (effort == "low") {
      return table.low;
    }
    if (effort == "high") {
      return table.high;
    }
  }
  return table.medium;
}

std::optional<ModelReasoning> build_model_reasoning(std::string text, std::string format) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::nullopt;
  }
  return ModelReasoning{std::move(text), std::move(format)};
}

std::optional<ModelReasoning> merge_model_reasoning(std::optional<ModelReasoning> primary,
                                                    std::optional<ModelReasoning> fallback) {
  if (primary && !primary->text.empty()) {
    return primary;
  }
  return fallback && !fallback->text.empty() ? fallback : std::nullopt;
}

TaggedReasoningExtraction extract_tagged_reasoning(const std::string& input,
                                                   const std::string& tag_name) {
  const std::string open_tag = "<" + (tag_name.empty() ? std::string("think") : tag_name) + ">";
  const std::string close_tag = "</" + (tag_name.empty() ? std::string("think") : tag_name) + ">";
  std::string text;
  std::string reasoning;
  std::size_t cursor = 0;
  while (cursor < input.size()) {
    const auto start = input.find(open_tag, cursor);
    if (start == std::string::npos) {
      text += input.substr(cursor);
      break;
    }
    text += input.substr(cursor, start - cursor);
    const auto reasoning_start = start + open_tag.size();
    const auto end = input.find(close_tag, reasoning_start);
    if (!reasoning.empty()) {
      reasoning += "\n\n";
    }
    if (end == std::string::npos) {
      reasoning += input.substr(reasoning_start);
      cursor = input.size();
      break;
    }
    reasoning += input.substr(reasoning_start, end - reasoning_start);
    cursor = end + close_tag.size();
  }
  return TaggedReasoningExtraction{std::move(text), build_model_reasoning(std::move(reasoning), "tagged")};
}

TaggedReasoningStreamParser::TaggedReasoningStreamParser(std::string tag_name) {
  if (tag_name.empty()) {
    tag_name = "think";
  }
  open_tag_ = "<" + tag_name + ">";
  close_tag_ = "</" + tag_name + ">";
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::push_delta(
    TaggedReasoningDeltaType type,
    std::string delta) {
  if (delta.empty()) {
    return {};
  }
  if (type == TaggedReasoningDeltaType::Text) {
    text_ += delta;
  } else {
    reasoning_text_ += delta;
  }
  return {TaggedReasoningDelta{type, std::move(delta), text_, reasoning_text_}};
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::feed(const std::string& chunk) {
  buffer_ += chunk;
  std::vector<TaggedReasoningDelta> deltas;

  while (!buffer_.empty()) {
    if (mode_ == Mode::Text) {
      const auto open_index = buffer_.find(open_tag_);
      if (open_index != std::string::npos) {
        auto safe = push_delta(TaggedReasoningDeltaType::Text, buffer_.substr(0, open_index));
        deltas.insert(deltas.end(), safe.begin(), safe.end());
        buffer_ = buffer_.substr(open_index + open_tag_.size());
        mode_ = Mode::Reasoning;
        continue;
      }

      const auto partial_length = trailing_tag_prefix_length(buffer_, open_tag_);
      auto safe = push_delta(TaggedReasoningDeltaType::Text,
                             buffer_.substr(0, buffer_.size() - partial_length));
      deltas.insert(deltas.end(), safe.begin(), safe.end());
      buffer_ = buffer_.substr(buffer_.size() - partial_length);
      break;
    }

    const auto close_index = buffer_.find(close_tag_);
    if (close_index != std::string::npos) {
      auto safe = push_delta(TaggedReasoningDeltaType::Reasoning, buffer_.substr(0, close_index));
      deltas.insert(deltas.end(), safe.begin(), safe.end());
      buffer_ = buffer_.substr(close_index + close_tag_.size());
      mode_ = Mode::Text;
      continue;
    }

    const auto partial_length = trailing_tag_prefix_length(buffer_, close_tag_);
    auto safe = push_delta(TaggedReasoningDeltaType::Reasoning,
                           buffer_.substr(0, buffer_.size() - partial_length));
    deltas.insert(deltas.end(), safe.begin(), safe.end());
    buffer_ = buffer_.substr(buffer_.size() - partial_length);
    break;
  }

  return deltas;
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::finish() {
  if (buffer_.empty()) {
    return {};
  }
  const auto type = mode_ == Mode::Text ? TaggedReasoningDeltaType::Text : TaggedReasoningDeltaType::Reasoning;
  auto buffered = std::move(buffer_);
  buffer_.clear();
  return push_delta(type, std::move(buffered));
}

std::optional<ModelReasoning> TaggedReasoningStreamParser::to_reasoning() const {
  return build_model_reasoning(reasoning_text_, "tagged");
}

const std::string& TaggedReasoningStreamParser::text() const noexcept {
  return text_;
}

const std::string& TaggedReasoningStreamParser::reasoning_text() const noexcept {
  return reasoning_text_;
}

std::string to_string(CacheStrategy strategy) {
  switch (strategy) {
    case CacheStrategy::None:
      return "none";
    case CacheStrategy::Explicit:
      return "explicit";
  }
  return "none";
}

std::string to_string(CacheScope scope) {
  switch (scope) {
    case CacheScope::SystemOnly:
      return "system-only";
    case CacheScope::SystemAndTools:
      return "system-and-tools";
    case CacheScope::SystemToolsAndSkills:
      return "system-tools-and-skills";
  }
  return "system-and-tools";
}

CacheStrategy cache_strategy_from_string(const std::string& value) {
  if (value == "explicit") {
    return CacheStrategy::Explicit;
  }
  return CacheStrategy::None;
}

CacheScope cache_scope_from_string(const std::string& value) {
  if (value == "system-only" || value == "systemOnly") {
    return CacheScope::SystemOnly;
  }
  if (value == "system-tools-and-skills" || value == "systemToolsAndSkills") {
    return CacheScope::SystemToolsAndSkills;
  }
  return CacheScope::SystemAndTools;
}

Value model_settings_to_json_value(const ModelSettings& settings) {
  Value value = Value::object({{"extra", settings.extra}});
  if (!settings.model.empty()) {
    value["model"] = settings.model;
  }
  if (settings.temperature) {
    value["temperature"] = *settings.temperature;
  }
  if (settings.max_output_tokens) {
    value["maxOutputTokens"] = *settings.max_output_tokens;
  }
  if (settings.reasoning) {
    value["reasoning"] = reasoning_settings_to_json_value(*settings.reasoning);
  }
  value["cacheStrategy"] = to_string(settings.cache_strategy);
  value["cacheScope"] = to_string(settings.cache_scope);
  if (!settings.cache_key.empty()) {
    value["cacheKey"] = settings.cache_key;
  }
  return value;
}

ModelSettings model_settings_from_json_value(const Value& value) {
  ModelSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.contains("temperature")) {
    settings.temperature = value.at("temperature").as_number();
  }
  if (value.contains("maxOutputTokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("maxOutputTokens").as_integer());
  } else if (value.contains("max_output_tokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("max_output_tokens").as_integer());
  }
  if (value.at("reasoning").is_object()) {
    settings.reasoning = reasoning_settings_from_json_value(value.at("reasoning"));
  }
  if (value.contains("cacheStrategy") && value.at("cacheStrategy").is_string()) {
    settings.cache_strategy = cache_strategy_from_string(value.at("cacheStrategy").as_string());
  } else if (value.contains("cache_strategy") && value.at("cache_strategy").is_string()) {
    settings.cache_strategy = cache_strategy_from_string(value.at("cache_strategy").as_string());
  }
  if (value.contains("cacheScope") && value.at("cacheScope").is_string()) {
    settings.cache_scope = cache_scope_from_string(value.at("cacheScope").as_string());
  } else if (value.contains("cache_scope") && value.at("cache_scope").is_string()) {
    settings.cache_scope = cache_scope_from_string(value.at("cache_scope").as_string());
  }
  if (value.contains("cacheKey") && value.at("cacheKey").is_string()) {
    settings.cache_key = value.at("cacheKey").as_string();
  } else if (value.contains("cache_key") && value.at("cache_key").is_string()) {
    settings.cache_key = value.at("cache_key").as_string();
  }
  settings.extra = value.at("extra").is_object() ? value.at("extra") : Value::object({});
  return settings;
}

bool ChatModelAdapter::supports(const std::string& capability) const {
  return capabilities_.find(capability) != capabilities_.end();
}

ModelSettings ChatModelAdapter::resolve_settings(const ModelSettings& settings) const {
  ModelSettings resolved = settings;
  if (resolved.model.empty()) {
    resolved.model = model_;
  }
  if (!resolved.temperature) {
    resolved.temperature = temperature_;
  }
  if (!resolved.max_output_tokens) {
    resolved.max_output_tokens = max_output_tokens_;
  }
  resolved.reasoning = merge_reasoning_settings(default_reasoning_, resolved.reasoning);
  return resolved;
}

ModelResponse ChatModelAdapter::build_response(ModelResponse payload) const {
  payload.provider = payload.provider.empty() ? provider_ : payload.provider;
  payload.model = payload.model.empty() ? model_ : payload.model;
  if (payload.content.empty() && !payload.text.empty()) {
    payload.content = {text_part(payload.text)};
  }
  if (payload.text.empty()) {
    payload.text = extract_text_content(payload.content);
  }
  if (payload.finish_reason.empty()) {
    payload.finish_reason = "stop";
  }
  return payload;
}

std::string to_string(ModelStreamEventType type) {
  switch (type) {
    case ModelStreamEventType::ResponseStart:
      return "response-start";
    case ModelStreamEventType::TextDelta:
      return "text-delta";
    case ModelStreamEventType::ReasoningDelta:
      return "reasoning-delta";
    case ModelStreamEventType::ContentPart:
      return "content-part";
    case ModelStreamEventType::Response:
      return "response";
    case ModelStreamEventType::ToolCallDelta:
      return "tool-call-delta";
  }
  return "response-start";
}

std::vector<ModelStreamEvent> ChatModelAdapter::stream(const GenerateParams& params) {
  const auto resolved = resolve_settings(params.settings);
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = provider_,
      .model = resolved.model,
  });

  auto response = generate(params);
  if (response.reasoning && !response.reasoning->text.empty()) {
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = response.provider,
        .model = response.model,
        .delta = response.reasoning->text,
        .reasoning = response.reasoning->text,
    });
  }
  if (!response.text.empty()) {
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = response.provider,
        .model = response.model,
        .delta = response.text,
        .text = response.text,
    });
  }
  for (const auto& part : response.content) {
    if (part.type == ContentPartType::Text) {
      continue;
    }
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ContentPart,
        .provider = response.provider,
        .model = response.model,
        .part = part,
    });
  }
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

void ChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  for (const auto& event : stream(params)) {
    if (on_event) {
      on_event(event);
    }
  }
}

EchoChatModelAdapter::EchoChatModelAdapter() : ChatModelAdapter("echo", "echo", 0, 1024, {"input.text"}) {}

ModelResponse EchoChatModelAdapter::generate(const GenerateParams& params) {
  std::string text;
  for (auto it = params.messages.rbegin(); it != params.messages.rend(); ++it) {
    if (it->role == MessageRole::User || it->role == MessageRole::Tool) {
      text = extract_text_content(it->content);
      break;
    }
  }
  return build_response(ModelResponse{.text = text});
}

namespace {

std::string describe_model_adapter(const ChatModelAdapter& adapter) {
  return adapter.provider() + ":" + (adapter.model().empty() ? "unknown" : adapter.model());
}

std::string fallback_model_error_message(const std::vector<std::shared_ptr<ChatModelAdapter>>& adapters,
                                         const std::string& last_error) {
  std::string chain;
  for (const auto& adapter : adapters) {
    if (!adapter) {
      continue;
    }
    if (!chain.empty()) {
      chain += " -> ";
    }
    chain += describe_model_adapter(*adapter);
  }
  std::string message = "All configured model candidates failed: " + chain + ".";
  if (!last_error.empty()) {
    message += " Last error: " + last_error;
  }
  return message;
}

}  // namespace

FallbackChatModelAdapter::FallbackChatModelAdapter(std::vector<std::shared_ptr<ChatModelAdapter>> adapters)
    : ChatModelAdapter(
          adapters.empty() || !adapters.front() ? "fallback" : adapters.front()->provider(),
          adapters.empty() || !adapters.front() ? "fallback" : adapters.front()->model(),
          adapters.empty() || !adapters.front() || !adapters.front()->resolve_settings().temperature
              ? 0.2
              : *adapters.front()->resolve_settings().temperature,
          adapters.empty() || !adapters.front() || !adapters.front()->resolve_settings().max_output_tokens
              ? 1024
              : *adapters.front()->resolve_settings().max_output_tokens,
          adapters.empty() || !adapters.front() ? std::set<std::string>{"input.text"}
                                                : adapters.front()->capabilities()),
      adapters_(std::move(adapters)) {
  if (adapters_.empty()) {
    throw ConfigurationError("FallbackChatModelAdapter requires at least one model candidate.");
  }
}

GenerateParams FallbackChatModelAdapter::params_for_adapter(const ChatModelAdapter& adapter,
                                                            const GenerateParams& params) const {
  GenerateParams candidate = params;
  if (!model().empty() && candidate.settings.model == model() && adapter.model() != model()) {
    candidate.settings.model.clear();
  }
  return candidate;
}

ModelResponse FallbackChatModelAdapter::generate(const GenerateParams& params) {
  std::string last_error;
  for (const auto& adapter : adapters_) {
    if (!adapter) {
      continue;
    }
    try {
      return adapter->generate(params_for_adapter(*adapter, params));
    } catch (const std::exception& error) {
      last_error = error.what();
    }
  }
  throw AdapterError(fallback_model_error_message(adapters_, last_error));
}

std::vector<ModelStreamEvent> FallbackChatModelAdapter::stream(const GenerateParams& params) {
  std::string last_error;
  for (const auto& adapter : adapters_) {
    if (!adapter) {
      continue;
    }
    try {
      return adapter->stream(params_for_adapter(*adapter, params));
    } catch (const std::exception& error) {
      last_error = error.what();
    }
  }
  throw AdapterError(fallback_model_error_message(adapters_, last_error));
}

void FallbackChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  std::string last_error;
  for (const auto& adapter : adapters_) {
    if (!adapter) {
      continue;
    }
    bool emitted = false;
    try {
      adapter->stream(params_for_adapter(*adapter, params), [&](const ModelStreamEvent& event) {
        emitted = true;
        if (on_event) {
          on_event(event);
        }
      });
      return;
    } catch (const std::exception& error) {
      if (emitted) {
        throw;
      }
      last_error = error.what();
    }
  }
  throw AdapterError(fallback_model_error_message(adapters_, last_error));
}

const std::vector<std::shared_ptr<ChatModelAdapter>>& FallbackChatModelAdapter::adapters() const noexcept {
  return adapters_;
}

namespace {

void throw_if_model_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Model);
  }
}

NativeProviderRequest with_cancellation(NativeProviderRequest request,
                                        CancellationToken* cancellation) {
  request.cancellation = cancellation;
  return request;
}

}  // namespace

Value serialize_chat_tool_descriptor(const ChatToolDescriptor& tool) {
  return Value::object({{"type", "function"},
                        {"function", Value::object({{"name", tool.name},
                                                    {"description", tool.description},
                                                    {"parameters", json_schema_to_value(tool.input_schema)},
                                                    {"strict", true}})}});
}

Value serialize_openai_chat_messages(const std::vector<AgentMessage>& messages) {
  Value::Array output;
  for (const auto& message : messages) {
    const std::string content = extract_text_content(message.content);
    if (message.role == MessageRole::System) {
      // Chat Completions spec allows only system/user/assistant/tool/function.
      // `developer` is exclusive to the Responses API (handled by
      // serialize_responses_input). OpenAI itself tolerates `developer` here
      // for back-compat, but DashScope/Qwen, DeepSeek, vLLM, SiliconFlow and
      // other OpenAI-compatible endpoints strictly reject it with HTTP 400.
      output.push_back(Value::object({{"role", "system"}, {"content", content}}));
      continue;
    }
    if (message.role == MessageRole::Tool) {
      output.push_back(Value::object({{"role", "tool"}, {"tool_call_id", message.tool_call_id},
                                     {"content", content}}));
      continue;
    }
    if (message.role == MessageRole::Assistant && !message.tool_calls.empty()) {
      Value::Array calls;
      for (const auto& tool_call : message.tool_calls) {
        calls.push_back(Value::object({{"id", tool_call.id},
                                      {"type", "function"},
                                      {"function", Value::object({{"name", tool_call.name},
                                                                  {"arguments", tool_call.arguments.stringify()}})}}));
      }
      output.push_back(Value::object({{"role", "assistant"}, {"content", content}, {"tool_calls", Value(calls)}}));
      continue;
    }
    output.push_back(Value::object({{"role", message_role_label(message.role)}, {"content", content}}));
  }
  return Value(output);
}

NativeProviderTransport create_native_provider_http_transport(HttpTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request) -> Value {
    throw_if_model_cancelled(request.cancellation);
    auto response = request_json(
        RequestJsonOptions{
            .base_url = request.base_url,
            .path = request.endpoint,
            .method = "POST",
            .headers = request.headers,
            .body = request.body,
            .cancellation = request.cancellation,
        },
        transport);
    throw_if_model_cancelled(request.cancellation);
    return response;
  };
}

NativeProviderStreamTransport create_native_provider_http_stream_transport(HttpTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request) -> std::vector<std::string> {
    throw_if_model_cancelled(request.cancellation);
    auto response = request_stream(
        RequestStreamOptions{
            .base_url = request.base_url,
            .path = request.endpoint,
            .method = "POST",
            .headers = request.headers,
            .body = request.body,
            .cancellation = request.cancellation,
        },
        transport);
    throw_if_model_cancelled(request.cancellation);
    return response.body.empty() ? std::vector<std::string>{}
                                 : std::vector<std::string>{std::move(response.body)};
  };
}

NativeProviderStreamingTransport create_native_provider_http_streaming_transport(HttpStreamingTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP streaming transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request,
                                            NativeProviderStreamingChunkHandler on_chunk) {
    throw_if_model_cancelled(request.cancellation);
    std::optional<std::string> error;
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    transport(build_http_request(RequestStreamOptions{
                  .base_url = request.base_url,
                  .path = request.endpoint,
                  .method = "POST",
                  .headers = request.headers,
                  .body = request.body,
                  .cancellation = request.cancellation,
              }),
              [&](std::string_view chunk) {
                throw_if_model_cancelled(request.cancellation);
                body.append(chunk.data(), chunk.size());
                if (on_chunk) {
                  on_chunk(chunk);
                }
              },
              [&](int done_status,
                  const std::map<std::string, std::string>& done_headers,
                  std::optional<std::string> done_error) {
                status = done_status;
                headers = done_headers;
                error = std::move(done_error);
              });
    (void)headers;
    throw_if_model_cancelled(request.cancellation);
    if (error) {
      throw AdapterError(*error);
    }
    if (status != 0 && (status < 200 || status >= 300)) {
      throw AdapterError("Request failed with status " + std::to_string(status) + ".",
                         safe_json_stringify(parse_http_response_body(body)));
    }
  };
}

NativeProviderRequest build_openai_chat_request(const GenerateParams& params, std::string model,
                                                std::string endpoint, std::string base_url) {
  Value::Array tools;
  for (const auto& tool : params.tools) {
    tools.push_back(serialize_chat_tool_descriptor(tool));
  }
  Value body = Value::object({{"model", model},
                              {"messages", serialize_openai_chat_messages(params.messages)}});
  if (!tools.empty()) {
    body["tools"] = Value(tools);
    body["tool_choice"] = "auto";
    body["parallel_tool_calls"] = true;
  }
  if (params.settings.temperature) {
    body["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    body["max_completion_tokens"] = *params.settings.max_output_tokens;
  }
  return with_cancellation(NativeProviderRequest{
      .provider = "openai-compatible",
      .endpoint = std::move(endpoint),
      .body = body,
      .headers = {{"content-type", "application/json"}},
      .base_url = std::move(base_url),
  }, params.cancellation);
}

NativeProviderRequest build_openai_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string endpoint, std::string base_url) {
  auto request = build_openai_chat_request(params, std::move(model), std::move(endpoint), std::move(base_url));
  request.body["stream"] = true;
  // Opt-in to receive a terminal chunk carrying the `usage` block so streaming
  // calls can report the same token counts as non-streaming calls.
  request.body["stream_options"] = Value::object({{"include_usage", true}});
  return request;
}

namespace {

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

}  // namespace

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

void apply_openai_reasoning_effort(NativeProviderRequest& request,
                                   const std::optional<ReasoningSettings>& reasoning) {
  if (!reasoning || reasoning->enabled != true) return;
  if (!request.body.is_object()) return;
  request.body["reasoning_effort"] = normalize_reasoning_budget_to_effort(reasoning->budget);
}

namespace {

std::string trim_provider_base_url(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string collect_system_prompt(const std::vector<AgentMessage>& messages) {
  std::string output;
  for (const auto& message : messages) {
    if (message.role != MessageRole::System) {
      continue;
    }
    const auto text = extract_text_content(message.content);
    if (text.empty()) {
      continue;
    }
    if (!output.empty()) {
      output += "\n\n";
    }
    output += text;
  }
  return output;
}

Value tool_schema_placeholder(const ChatToolDescriptor& tool) {
  return Value::object({{"name", tool.name},
                        {"description", tool.description},
                        {"input_schema", json_schema_to_value(tool.input_schema)}});
}

bool has_non_text_content(const std::vector<AgentMessage>& messages) {
  for (const auto& message : messages) {
    for (const auto& part : message.content) {
      if (part.type != ContentPartType::Text) {
        return true;
      }
    }
  }
  return false;
}

bool is_http_url(const std::string& url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool is_file_or_data_url(const std::string& url) {
  return url.rfind("file://", 0) == 0 || url.rfind("data:", 0) == 0;
}

std::string media_data_url(const MessageContentPart& part) {
  DefaultMediaResolver resolver;
  const auto resolved = resolver.resolve(part.source);
  const auto mime_type = resolved.mime_type.empty() ? std::string("application/octet-stream")
                                                    : resolved.mime_type;
  return "data:" + mime_type + ";base64," + base64_encode(resolved.bytes);
}

Value serialize_responses_content_part(const MessageContentPart& part, bool qwen_url_rules) {
  if (part.type == ContentPartType::Image) {
    if (part.source.kind == MediaSourceKind::Url && is_http_url(part.source.url)) {
      Value value = Value::object({{"type", "input_image"},
                                   {"image_url", part.source.url}});
      if (!part.detail.empty()) {
        value["detail"] = part.detail;
      }
      return value;
    }
    if (part.source.kind == MediaSourceKind::Url && qwen_url_rules && !is_file_or_data_url(part.source.url)) {
      throw ConfigurationError("Qwen image inputs must use http, https, file, data, or inline sources.");
    }
    Value value = Value::object({{"type", "input_image"},
                                 {"image_url", media_data_url(part)}});
    if (!part.detail.empty()) {
      value["detail"] = part.detail;
    }
    return value;
  }

  if (part.type == ContentPartType::File) {
    const auto data_url = media_data_url(part);
    DefaultMediaResolver resolver;
    const auto resolved = resolver.resolve(part.source);
    std::string filename = part.source.filename;
    if (filename.empty()) {
      filename = part.title;
    }
    if (filename.empty()) {
      filename = resolved.filename;
    }
    if (filename.empty()) {
      filename = "input-file";
    }
    return Value::object({{"type", "input_file"},
                          {"file_data", data_url},
                          {"filename", filename}});
  }

  return Value();
}

Value serialize_responses_input(const std::vector<AgentMessage>& messages, bool qwen_url_rules) {
  Value::Array input;
  for (const auto& message : messages) {
    if (message.role == MessageRole::System) {
      continue;
    }
    if (message.role == MessageRole::Tool) {
      input.push_back(Value::object({{"type", "function_call_output"},
                                    {"call_id", message.tool_call_id},
                                    {"output", extract_text_content(message.content)}}));
      continue;
    }

    if (!message.content.empty()) {
      Value::Array content;
      const auto text = extract_text_content(message.content);
      if (!text.empty()) {
        content.push_back(Value::object({{"type", "input_text"}, {"text", text}}));
      }
      for (const auto& part : message.content) {
        if (part.type == ContentPartType::Image || part.type == ContentPartType::File) {
          content.push_back(serialize_responses_content_part(part, qwen_url_rules));
        }
      }
      input.push_back(Value::object({{"type", "message"},
                                    {"role", message_role_label(message.role)},
                                    {"content", Value(content)}}));
    }

    if (message.role == MessageRole::Assistant) {
      for (const auto& tool_call : message.tool_calls) {
        input.push_back(Value::object({{"type", "function_call"},
                                      {"call_id", tool_call.id},
                                      {"name", tool_call.name},
                                      {"arguments", tool_call.arguments.stringify()}}));
      }
    }
  }
  return Value(std::move(input));
}

Value serialize_responses_tool_descriptor(const ChatToolDescriptor& tool) {
  return Value::object({{"type", "function"},
                        {"name", tool.name},
                        {"description", tool.description},
                        {"parameters", json_schema_to_value(tool.input_schema)},
                        {"strict", true}});
}

Value responses_reasoning_payload(const std::optional<ReasoningSettings>& reasoning) {
  if (!reasoning || reasoning->enabled != true) {
    return Value();
  }
  return Value::object({{"effort", normalize_reasoning_budget_to_effort(reasoning->budget)},
                        {"summary", reasoning->include_thoughts == true ? "detailed" : "auto"}});
}

std::string responses_endpoint_from_chat_endpoint(const std::string& endpoint) {
  const std::string suffix = "/chat/completions";
  if (endpoint.size() >= suffix.size() &&
      endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return endpoint.substr(0, endpoint.size() - suffix.size()) + "/responses";
  }
  return "/v1/responses";
}

std::set<std::string> openai_compatible_capabilities(const std::string& provider) {
  if (provider == "openai") {
    return {"input.text", "input.image", "input.file", "output.structuredContent",
            "transport.inline", "transport.reference", "reasoning", "reasoning.budget",
            "reasoning.includeThoughts"};
  }
  return {"input.text", "output.structuredContent", "reasoning"};
}

GenerateParams params_with_resolved_settings(const GenerateParams& params,
                                             const ModelSettings& settings) {
  GenerateParams resolved = params;
  resolved.settings = settings;
  return resolved;
}

Value serialize_anthropic_messages(const std::vector<AgentMessage>& messages) {
  Value::Array output;
  for (const auto& message : messages) {
    if (message.role == MessageRole::System) {
      continue;
    }
    Value::Array content;
    if (message.role == MessageRole::Tool) {
      content.push_back(Value::object({{"type", "tool_result"},
                                      {"tool_use_id", message.tool_call_id},
                                      {"content", extract_text_content(message.content)}}));
      output.push_back(Value::object({{"role", "user"}, {"content", Value(content)}}));
      continue;
    }
    const auto text = extract_text_content(message.content);
    if (!text.empty()) {
      content.push_back(Value::object({{"type", "text"}, {"text", text}}));
    }
    for (const auto& part : message.content) {
      if (part.type == ContentPartType::Image) {
        Value source = Value::object({});
        if (part.source.kind == MediaSourceKind::Url) {
          source = Value::object({{"type", "url"}, {"url", part.source.url}});
        } else if (part.source.kind == MediaSourceKind::Inline) {
          source = Value::object({{"type", "base64"},
                                  {"media_type", part.source.mime_type},
                                  {"data", part.source.data}});
        } else if (!part.source.key.empty()) {
          source = Value::object({{"type", "file"}, {"file_id", part.source.key}});
        }
        content.push_back(Value::object({{"type", "image"}, {"source", source}}));
      } else if (part.type == ContentPartType::File) {
        Value source = Value::object({});
        if (part.source.kind == MediaSourceKind::Url) {
          source = Value::object({{"type", "url"}, {"url", part.source.url}});
        } else if (part.source.kind == MediaSourceKind::Inline) {
          source = Value::object({{"type", "base64"},
                                  {"media_type", part.source.mime_type},
                                  {"data", part.source.data}});
        } else if (!part.source.key.empty()) {
          source = Value::object({{"type", "file"}, {"file_id", part.source.key}});
        }
        content.push_back(Value::object({{"type", "document"},
                                        {"source", source},
                                        {"title", part.title},
                                        {"context", part.text_hint}}));
      }
    }
    if (message.role == MessageRole::Assistant) {
      for (const auto& tool_call : message.tool_calls) {
        content.push_back(Value::object({{"type", "tool_use"},
                                        {"id", tool_call.id},
                                        {"name", tool_call.name},
                                        {"input", tool_call.arguments}}));
      }
    }
    if (content.empty()) {
      content.push_back(Value::object({{"type", "text"}, {"text", ""}}));
    }
    output.push_back(Value::object({{"role", message.role == MessageRole::Assistant ? "assistant" : "user"},
                                   {"content", Value(content)}}));
  }
  return Value(output);
}

Value serialize_ollama_messages(const std::vector<AgentMessage>& messages) {
  Value::Array output;
  for (const auto& message : messages) {
    Value item = Value::object({{"role", message_role_label(message.role)},
                               {"content", extract_text_content(message.content)}});
    if (message.role == MessageRole::Tool) {
      item["role"] = "tool";
      item["tool_name"] = message.name;
    }
    Value::Array images;
    for (const auto& part : message.content) {
      if (part.type == ContentPartType::Image && part.source.kind == MediaSourceKind::Inline) {
        images.push_back(part.source.data);
      }
    }
    if (!images.empty()) {
      item["images"] = Value(images);
    }
    if (message.role == MessageRole::Assistant && !message.tool_calls.empty()) {
      Value::Array calls;
      for (std::size_t index = 0; index < message.tool_calls.size(); ++index) {
        const auto& tool_call = message.tool_calls[index];
        calls.push_back(Value::object({{"type", "function"},
                                      {"function", Value::object({{"index", index},
                                                                  {"name", tool_call.name},
                                                                  {"arguments", tool_call.arguments}})}}));
      }
      item["tool_calls"] = Value(calls);
    }
    output.push_back(std::move(item));
  }
  return Value(output);
}

Value serialize_gemini_contents(const std::vector<AgentMessage>& messages, Value& system_instruction) {
  Value::Array contents;
  Value::Array system_parts;
  for (const auto& message : messages) {
    if (message.role == MessageRole::System) {
      const auto text = extract_text_content(message.content);
      if (!text.empty()) {
        system_parts.push_back(Value::object({{"text", text}}));
      }
      continue;
    }
    Value::Array parts;
    const auto text = extract_text_content(message.content);
    if (!text.empty()) {
      parts.push_back(Value::object({{"text", text}}));
    }
    for (const auto& part : message.content) {
      if ((part.type == ContentPartType::Image || part.type == ContentPartType::File) &&
          part.source.kind == MediaSourceKind::Inline) {
        parts.push_back(Value::object({{"inlineData", Value::object({{"mimeType", part.source.mime_type},
                                                                     {"data", part.source.data}})}}));
      } else if ((part.type == ContentPartType::Image || part.type == ContentPartType::File) &&
                 part.source.kind == MediaSourceKind::Url) {
        parts.push_back(Value::object({{"fileData", Value::object({{"mimeType", part.source.mime_type},
                                                                   {"fileUri", part.source.url}})}}));
      }
    }
    if (message.role == MessageRole::Assistant) {
      for (const auto& tool_call : message.tool_calls) {
        parts.push_back(Value::object({{"functionCall", Value::object({{"id", tool_call.id},
                                                                       {"name", tool_call.name},
                                                                       {"args", tool_call.arguments}})}}));
      }
    }
    if (message.role == MessageRole::Tool) {
      Value response = Value::object({{"result", extract_text_content(message.content)}});
      try {
        response = parse_json(extract_text_content(message.content));
      } catch (...) {
      }
      parts.push_back(Value::object({{"functionResponse", Value::object({{"id", message.tool_call_id},
                                                                         {"name", message.name},
                                                                         {"response", response}})}}));
    }
    if (parts.empty()) {
      parts.push_back(Value::object({{"text", ""}}));
    }
    contents.push_back(Value::object({{"role", message.role == MessageRole::Assistant ? "model" : "user"},
                                     {"parts", Value(parts)}}));
  }
  if (!system_parts.empty()) {
    system_instruction = Value::object({{"parts", Value(system_parts)}});
  }
  return Value(contents);
}

Value serialize_gemini_tools(const std::vector<ChatToolDescriptor>& tools) {
  if (tools.empty()) {
    return Value();
  }
  Value::Array declarations;
  for (const auto& tool : tools) {
    declarations.push_back(Value::object({{"name", tool.name},
                                         {"description", tool.description},
                                         {"parameters", json_schema_to_value(tool.input_schema)}}));
  }
  return Value::array({Value::object({{"functionDeclarations", Value(declarations)}})});
}

ModelReasoning reasoning_from_text(std::string text) {
  auto reasoning = build_model_reasoning(std::move(text), "provider-visible");
  return reasoning ? *reasoning : ModelReasoning{};
}

ModelResponse apply_tagged_reasoning(ModelResponse response, const ModelSettings& settings) {
  if (!settings.reasoning || settings.reasoning->enabled != true || response.text.empty()) {
    return response;
  }
  const auto extracted = extract_tagged_reasoning(
      response.text,
      settings.reasoning->tag_name.empty() ? std::string("think") : settings.reasoning->tag_name);
  if (!extracted.reasoning) {
    return response;
  }
  response.text = extracted.text;
  response.reasoning = merge_model_reasoning(response.reasoning, extracted.reasoning);
  if (response.content.empty() || (response.content.size() == 1 && response.content.front().type == ContentPartType::Text)) {
    response.content = response.text.empty() ? std::vector<MessageContentPart>{}
                                             : std::vector<MessageContentPart>{text_part(response.text)};
  }
  return response;
}

std::vector<ModelStreamEvent> apply_tagged_reasoning_stream(
    std::vector<ModelStreamEvent> events,
    const ModelSettings& settings) {
  if (!settings.reasoning || settings.reasoning->enabled != true) {
    return events;
  }

  TaggedReasoningStreamParser parser(settings.reasoning->tag_name.empty()
                                         ? std::string("think")
                                         : settings.reasoning->tag_name);
  std::vector<ModelStreamEvent> output;
  output.reserve(events.size());
  std::string current_provider;
  std::string current_model;
  std::optional<ModelReasoning> native_reasoning;
  bool saw_text_delta = false;

  const auto append_parser_deltas = [&](const std::vector<TaggedReasoningDelta>& deltas,
                                        const std::string& provider,
                                        const std::string& model) {
    for (const auto& item : deltas) {
      if (item.type == TaggedReasoningDeltaType::Reasoning) {
        output.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = provider,
            .model = model,
            .delta = item.delta,
            .reasoning = item.reasoning_text,
        });
        continue;
      }
      output.push_back(ModelStreamEvent{
          .type = ModelStreamEventType::TextDelta,
          .provider = provider,
          .model = model,
          .delta = item.delta,
          .text = item.text,
      });
    }
  };

  for (auto& event : events) {
    if (!event.provider.empty()) {
      current_provider = event.provider;
    }
    if (!event.model.empty()) {
      current_model = event.model;
    }

    if (event.type == ModelStreamEventType::TextDelta) {
      saw_text_delta = true;
      append_parser_deltas(parser.feed(event.delta), event.provider, event.model);
      continue;
    }

    if (event.type == ModelStreamEventType::ReasoningDelta) {
      if (!event.reasoning.empty()) {
        native_reasoning = build_model_reasoning(event.reasoning, "provider-visible");
      }
      output.push_back(std::move(event));
      continue;
    }

    if (event.type == ModelStreamEventType::Response) {
      const auto provider = event.response.provider.empty() ? current_provider : event.response.provider;
      const auto model = event.response.model.empty() ? current_model : event.response.model;
      append_parser_deltas(parser.finish(), provider, model);
      if (saw_text_delta) {
        event.response.text = parser.text();
        event.response.content = event.response.text.empty()
                                     ? std::vector<MessageContentPart>{}
                                     : std::vector<MessageContentPart>{text_part(event.response.text)};
      }
      if (!event.response.reasoning && native_reasoning) {
        event.response.reasoning = native_reasoning;
      }
      event.response.reasoning = merge_model_reasoning(event.response.reasoning, parser.to_reasoning());
      output.push_back(std::move(event));
      continue;
    }

    output.push_back(std::move(event));
  }

  return output;
}

Value parse_tool_arguments_text(const std::string& arguments_text) {
  if (arguments_text.empty()) {
    return Value::object({});
  }
  try {
    auto parsed = parse_json(arguments_text);
    if (parsed.is_object()) {
      return parsed;
    }
    return Value::object({{"value", parsed}});
  } catch (...) {
    return Value::object({{"raw", arguments_text}});
  }
}

struct StreamToolCallState {
  std::string id;
  std::string name;
  Value input = Value::object({});
  std::string arguments_text;
};

using ToolCallDeltaSink = std::function<void(const std::string& id,
                                              const std::string& name,
                                              const std::string& args_delta,
                                              const std::string& args_accumulated)>;

void append_openai_stream_tool_calls(std::map<long long, StreamToolCallState>& tool_call_state,
                                     const Value& delta,
                                     const ToolCallDeltaSink& on_delta = {}) {
  for (const auto& streamed_tool_call : delta.at("tool_calls").as_array()) {
    const auto index = streamed_tool_call.at("index").as_integer(
        static_cast<long long>(tool_call_state.size()));
    auto& current = tool_call_state[index];
    if (current.id.empty()) {
      current.id = "tool_" + std::to_string(index + 1);
    }
    const auto id = streamed_tool_call.at("id").as_string();
    if (!id.empty()) {
      current.id = id;
    }
    const auto& function = streamed_tool_call.at("function");
    const auto name = function.at("name").as_string();
    if (!name.empty()) {
      current.name = name;
    }
    const auto piece = function.at("arguments").as_string();
    current.arguments_text += piece;
    if (on_delta && !piece.empty()) {
      on_delta(current.id, current.name, piece, current.arguments_text);
    }
  }
}

}  // namespace

NativeProviderRequest build_openai_responses_request(const GenerateParams& params, std::string model,
                                                      std::string endpoint, std::string base_url) {
  Value::Array tools;
  for (const auto& tool : params.tools) {
    tools.push_back(serialize_responses_tool_descriptor(tool));
  }

  Value body = Value::object({{"model", model},
                              {"input", serialize_responses_input(params.messages, false)}});
  const auto instructions = collect_system_prompt(params.messages);
  if (!instructions.empty()) {
    body["instructions"] = instructions;
  }
  if (!tools.empty()) {
    body["tools"] = Value(tools);
  }
  if (params.settings.temperature) {
    body["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    body["max_output_tokens"] = *params.settings.max_output_tokens;
  }
  const auto reasoning = responses_reasoning_payload(params.settings.reasoning);
  if (!reasoning.is_null()) {
    body["reasoning"] = reasoning;
  }
  return with_cancellation(NativeProviderRequest{
      .provider = "openai",
      .endpoint = std::move(endpoint),
      .body = body,
      .headers = {{"content-type", "application/json"}},
      .base_url = std::move(base_url),
  }, params.cancellation);
}

NativeProviderRequest build_openai_responses_stream_request(const GenerateParams& params, std::string model,
                                                            std::string endpoint, std::string base_url) {
  auto request = build_openai_responses_request(params, std::move(model), std::move(endpoint), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_openai_chat_response(const Value& raw, std::string provider, std::string model) {
  ModelResponse response;
  response.provider = std::move(provider);
  response.model = std::move(model);
  response.raw = raw;
  response.id = raw.at("id").as_string();
  const auto& choices = raw.at("choices").as_array();
  if (!choices.empty()) {
    const auto& choice = choices.front();
    const auto& message = choice.at("message");
    response.text = message.at("content").as_string();
    response.content = response.text.empty() ? std::vector<MessageContentPart>{}
                                             : std::vector<MessageContentPart>{text_part(response.text)};
    const auto& tool_calls = message.at("tool_calls").as_array();
    for (std::size_t index = 0; index < tool_calls.size(); ++index) {
      const auto& tool_call = tool_calls[index];
      const auto arguments_text = tool_call.at("function").at("arguments").as_string();
      response.tool_calls.push_back(ToolCall{tool_call.at("id").as_string("tool_" + std::to_string(index + 1)),
                                             tool_call.at("function").at("name").as_string(),
                                             parse_tool_arguments_text(arguments_text)});
    }
    response.finish_reason = response.tool_calls.empty() ? choice.at("finish_reason").as_string("stop")
                                                         : "tool_calls";
  }
  if (response.finish_reason.empty()) {
    response.finish_reason = "stop";
  }
  return response;
}

std::vector<ModelStreamEvent> parse_openai_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model) {
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = provider,
      .model = model,
  });

  std::string response_id;
  std::string response_model = model;
  std::string text;
  std::string reasoning;
  std::string finish_reason = "stop";
  std::map<long long, StreamToolCallState> tool_call_state;
  Value::Array raw_chunks;
  Value captured_usage;

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk;
    try {
      chunk = parse_json(event);
    } catch (...) {
      continue;
    }
    raw_chunks.push_back(chunk);
    const auto id = chunk.at("id").as_string();
    if (!id.empty()) {
      response_id = id;
    }
    const auto chunk_model = chunk.at("model").as_string();
    if (!chunk_model.empty()) {
      response_model = chunk_model;
    }
    // OpenAI/Qwen/Ollama-compat stream_options=include_usage delivers usage in
    // a terminal chunk with an empty `choices` array.
    if (chunk.at("usage").is_object()) {
      captured_usage = chunk.at("usage");
    }
    const auto& choices = chunk.at("choices").as_array();
    if (choices.empty()) {
      continue;
    }
    const auto& choice = choices.front();
    const auto finish = choice.at("finish_reason").as_string();
    if (!finish.empty()) {
      finish_reason = finish;
    }
    const auto& delta = choice.at("delta");
    const auto reasoning_delta = delta.at("reasoning_content").as_string();
    if (!reasoning_delta.empty()) {
      reasoning += reasoning_delta;
      events.push_back(ModelStreamEvent{
          .type = ModelStreamEventType::ReasoningDelta,
          .provider = provider,
          .model = response_model,
          .delta = reasoning_delta,
          .reasoning = reasoning,
      });
    }
    const auto content_delta = delta.at("content").as_string();
    if (!content_delta.empty()) {
      text += content_delta;
      events.push_back(ModelStreamEvent{
          .type = ModelStreamEventType::TextDelta,
          .provider = provider,
          .model = response_model,
          .delta = content_delta,
          .text = text,
      });
    }
    append_openai_stream_tool_calls(tool_call_state, delta,
        [&](const std::string& id, const std::string& name,
            const std::string& args_delta, const std::string& args_accumulated) {
          events.push_back(ModelStreamEvent{
              .type = ModelStreamEventType::ToolCallDelta,
              .provider = provider,
              .model = response_model,
              .tool_call_id = id,
              .tool_call_name = name,
              .tool_call_args_delta = args_delta,
              .tool_call_args_accumulated = args_accumulated,
          });
        });
  }

  ModelResponse response;
  response.provider = provider;
  response.model = response_model;
  response.id = response_id;
  response.text = text;
  Value::Object raw_object{
      {"id", Value(response_id)},
      {"model", Value(response_model)},
      {"chunks", Value(std::move(raw_chunks))},
  };
  if (captured_usage.is_object()) {
    raw_object.emplace("usage", captured_usage);
  }
  response.raw = Value(std::move(raw_object));
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  for (const auto& [index, tool_call] : tool_call_state) {
    (void)index;
    response.tool_calls.push_back(ToolCall{
        tool_call.id,
        tool_call.name,
        parse_tool_arguments_text(tool_call.arguments_text),
    });
  }
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(finish_reason) : "tool_calls";

  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

ModelResponse parse_openai_responses_response(const Value& raw, std::string provider, std::string model) {
  ModelResponse response;
  response.provider = std::move(provider);
  response.model = raw.at("model").as_string(std::move(model));
  response.raw = raw;
  response.id = raw.at("id").as_string();

  std::string text;
  std::string summary_reasoning;
  std::string visible_reasoning;
  std::size_t tool_index = 0;
  for (const auto& item : raw.at("output").as_array()) {
    const auto type = item.at("type").as_string();
    if (type == "message") {
      for (const auto& part : item.at("content").as_array()) {
        if (part.at("type").as_string() == "refusal") {
          text += part.at("refusal").as_string();
        } else {
          text += part.at("text").as_string();
        }
      }
      continue;
    }
    if (type == "reasoning") {
      for (const auto& part : item.at("summary").as_array()) {
        if (!summary_reasoning.empty()) {
          summary_reasoning += "\n\n";
        }
        summary_reasoning += part.at("text").as_string();
      }
      for (const auto& part : item.at("content").as_array()) {
        if (!visible_reasoning.empty()) {
          visible_reasoning += "\n\n";
        }
        visible_reasoning += part.at("text").as_string();
      }
      continue;
    }
    if (type == "function_call") {
      const auto fallback_id = "tool_" + std::to_string(++tool_index);
      const auto id = item.at("call_id").as_string(item.at("id").as_string(fallback_id));
      response.tool_calls.push_back(ToolCall{id,
                                             item.at("name").as_string(),
                                             parse_tool_arguments_text(item.at("arguments").as_string())});
    }
  }

  response.text = text;
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  response.reasoning = !summary_reasoning.empty()
                           ? build_model_reasoning(summary_reasoning, "summary")
                           : build_model_reasoning(visible_reasoning, "provider-visible");
  response.finish_reason = response.tool_calls.empty()
                               ? normalize_finish_reason(raw.at("status").as_string() == "incomplete"
                                                             ? "incomplete"
                                                             : "stop")
                               : "tool_calls";
  return response;
}

std::vector<ModelStreamEvent> parse_openai_responses_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model) {
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = provider,
      .model = model,
  });

  std::string response_id;
  std::string response_model = model;
  std::string text;
  std::string reasoning;
  std::string reasoning_format = "summary";
  std::map<std::string, StreamToolCallState> tool_call_state;
  std::vector<std::string> tool_call_order;
  Value::Array raw_events;
  std::optional<Value> completed_response;

  const auto ensure_tool_call = [&](std::string key) -> StreamToolCallState& {
    if (key.empty()) {
      key = "tool_" + std::to_string(tool_call_order.size() + 1);
    }
    if (tool_call_state.find(key) == tool_call_state.end()) {
      tool_call_order.push_back(key);
      auto& current = tool_call_state[key];
      current.id = key;
      return current;
    }
    return tool_call_state[key];
  };

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk;
    try {
      chunk = parse_json(event);
    } catch (...) {
      continue;
    }
    raw_events.push_back(chunk);
    const auto type = chunk.at("type").as_string();
    if (type == "response.output_text.delta") {
      const auto delta = chunk.at("delta").as_string();
      if (!delta.empty()) {
        text += delta;
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = provider,
            .model = response_model,
            .delta = delta,
            .text = text,
        });
      }
      continue;
    }
    if (type == "response.reasoning_summary_text.delta" ||
        type == "response.reasoning_text.delta") {
      const auto delta = chunk.at("delta").as_string();
      if (!delta.empty()) {
        reasoning += delta;
        reasoning_format = type == "response.reasoning_text.delta" ? "provider-visible" : "summary";
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = provider,
            .model = response_model,
            .delta = delta,
            .reasoning = reasoning,
        });
      }
      continue;
    }
    if (type == "response.output_item.added") {
      const auto& item = chunk.at("item");
      if (item.at("type").as_string() == "function_call") {
        const auto key = item.at("id").as_string(item.at("call_id").as_string());
        auto& current = ensure_tool_call(key);
        const auto call_id = item.at("call_id").as_string();
        if (!call_id.empty()) {
          current.id = call_id;
        }
        const auto name = item.at("name").as_string();
        if (!name.empty()) {
          current.name = name;
        }
        const auto piece = item.at("arguments").as_string();
        current.arguments_text += piece;
        if (!piece.empty()) {
          events.push_back(ModelStreamEvent{
              .type = ModelStreamEventType::ToolCallDelta,
              .provider = provider,
              .model = response_model,
              .tool_call_id = current.id,
              .tool_call_name = current.name,
              .tool_call_args_delta = piece,
              .tool_call_args_accumulated = current.arguments_text,
          });
        }
      }
      continue;
    }
    if (type == "response.function_call_arguments.delta") {
      auto& current = ensure_tool_call(chunk.at("item_id").as_string());
      const auto piece = chunk.at("delta").as_string();
      current.arguments_text += piece;
      if (!piece.empty()) {
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ToolCallDelta,
            .provider = provider,
            .model = response_model,
            .tool_call_id = current.id,
            .tool_call_name = current.name,
            .tool_call_args_delta = piece,
            .tool_call_args_accumulated = current.arguments_text,
        });
      }
      continue;
    }
    if (type == "response.function_call_arguments.done") {
      auto& current = ensure_tool_call(chunk.at("item_id").as_string());
      const auto name = chunk.at("name").as_string();
      if (!name.empty()) {
        current.name = name;
      }
      if (chunk.at("arguments").is_string()) {
        current.arguments_text = chunk.at("arguments").as_string();
      }
      continue;
    }
    if (type == "response.completed") {
      completed_response = chunk.at("response");
      const auto id = completed_response->at("id").as_string();
      if (!id.empty()) {
        response_id = id;
      }
      const auto completed_model = completed_response->at("model").as_string();
      if (!completed_model.empty()) {
        response_model = completed_model;
      }
    }
  }

  ModelResponse response;
  if (completed_response) {
    response = parse_openai_responses_response(*completed_response, provider, response_model);
  } else {
    response.provider = provider;
    response.model = response_model;
    response.id = response_id;
    response.raw = Value::object({
        {"id", response_id},
        {"model", response_model},
        {"events", Value(std::move(raw_events))},
    });
    response.finish_reason = tool_call_order.empty() ? "stop" : "tool_calls";
  }

  if (response.id.empty()) {
    response.id = response_id;
  }
  if (response.model.empty()) {
    response.model = response_model;
  }
  if (response.text.empty() && !text.empty()) {
    response.text = text;
    response.content = {text_part(text)};
  }
  response.reasoning = merge_model_reasoning(
      build_model_reasoning(reasoning, reasoning_format),
      response.reasoning);
  if (response.tool_calls.empty()) {
    for (const auto& key : tool_call_order) {
      const auto found = tool_call_state.find(key);
      if (found == tool_call_state.end()) {
        continue;
      }
      response.tool_calls.push_back(ToolCall{
          found->second.id.empty() ? key : found->second.id,
          found->second.name,
          parse_tool_arguments_text(found->second.arguments_text),
      });
    }
    if (!response.tool_calls.empty()) {
      response.finish_reason = "tool_calls";
    }
  }
  if (response.finish_reason.empty()) {
    response.finish_reason = response.tool_calls.empty() ? "stop" : "tool_calls";
  }

  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

NativeProviderRequest build_qwen_chat_request(const GenerateParams& params, std::string model,
                                              std::string api_key, std::string base_url) {
  auto request = build_openai_chat_request(params, std::move(model), "/chat/completions");
  request.provider = "qwen";
  request.base_url = trim_provider_base_url(std::move(base_url));
  if (!api_key.empty()) {
    request.headers["authorization"] = "Bearer " + api_key;
  }
  if (params.settings.reasoning) {
    if (params.settings.reasoning->enabled == true) {
      request.body["enable_thinking"] = true;
      request.body["thinking_budget"] = normalize_reasoning_budget_to_tokens(
          params.settings.reasoning->budget,
          ReasoningBudgetTokenTable{1024, 4096, 16384});
    } else if (params.settings.reasoning->enabled == false) {
      request.body["enable_thinking"] = false;
    }
  }
  return request;
}

NativeProviderRequest build_qwen_chat_stream_request(const GenerateParams& params, std::string model,
                                                     std::string api_key, std::string base_url) {
  auto request = build_qwen_chat_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_qwen_chat_response(const Value& raw, std::string model) {
  auto response = parse_openai_chat_response(raw, "qwen", std::move(model));
  const auto& choices = raw.at("choices").as_array();
  if (!choices.empty()) {
    const auto reasoning = choices.front().at("message").at("reasoning_content").as_string();
    if (!reasoning.empty()) {
      response.reasoning = reasoning_from_text(reasoning);
    }
  }
  return response;
}

NativeProviderRequest build_qwen_responses_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key,
    std::string base_url) {
  Value::Array tools;
  for (const auto& tool : params.tools) {
    tools.push_back(serialize_responses_tool_descriptor(tool));
  }

  Value body = Value::object({{"model", model},
                              {"input", serialize_responses_input(params.messages, true)}});
  const auto instructions = collect_system_prompt(params.messages);
  if (!instructions.empty()) {
    body["instructions"] = instructions;
  }
  if (!tools.empty()) {
    body["tools"] = Value(tools);
  }
  if (params.settings.temperature) {
    body["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    body["max_output_tokens"] = *params.settings.max_output_tokens;
  }
  const auto reasoning = responses_reasoning_payload(params.settings.reasoning);
  if (!reasoning.is_null()) {
    body["reasoning"] = reasoning;
  }
  std::map<std::string, std::string> headers{{"content-type", "application/json"}};
  if (!api_key.empty()) {
    headers["authorization"] = "Bearer " + api_key;
  }
  return with_cancellation(
      NativeProviderRequest{"qwen", "/responses", body, headers, trim_provider_base_url(std::move(base_url))},
      params.cancellation);
}

NativeProviderRequest build_qwen_responses_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key,
    std::string base_url) {
  auto request = build_qwen_responses_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_qwen_responses_response(const Value& raw, std::string model) {
  return parse_openai_responses_response(raw, "qwen", std::move(model));
}

std::vector<ModelStreamEvent> parse_qwen_responses_stream_events(
    const std::vector<std::string>& chunks,
    std::string model) {
  return parse_openai_responses_stream_events(chunks, "qwen", std::move(model));
}

NativeProviderRequest build_mimo_chat_request(const GenerateParams& params, std::string model,
                                              std::string api_key, std::string base_url) {
  auto request = build_openai_chat_request(params, std::move(model), "/chat/completions");
  request.provider = "mimo";
  request.base_url = trim_provider_base_url(std::move(base_url));
  if (!api_key.empty()) {
    request.headers["authorization"] = "Bearer " + api_key;
  }
  return request;
}

NativeProviderRequest build_mimo_chat_stream_request(const GenerateParams& params, std::string model,
                                                     std::string api_key, std::string base_url) {
  auto request = build_mimo_chat_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  // Mirror the shared OpenAI stream builder: opt in to a terminal usage chunk so
  // streaming calls report the same token counts as non-streaming calls.
  request.body["stream_options"] = Value::object({{"include_usage", true}});
  return request;
}

ModelResponse parse_mimo_chat_response(const Value& raw, std::string model) {
  return parse_openai_chat_response(raw, "mimo", std::move(model));
}

NativeProviderRequest build_anthropic_messages_request(const GenerateParams& params, std::string model,
                                                       std::string api_key, std::string base_url) {
  Value::Array tools;
  for (const auto& tool : params.tools) {
    tools.push_back(tool_schema_placeholder(tool));
  }
  Value body = Value::object({{"model", model},
                              {"messages", serialize_anthropic_messages(params.messages)}});
  const auto system = collect_system_prompt(params.messages);
  const bool explicit_cache = params.settings.cache_strategy == CacheStrategy::Explicit;
  if (!system.empty()) {
    if (explicit_cache) {
      // Switch system to the structured-block form so it can carry cache_control.
      body["system"] = Value::array({Value::object({
          {"type", "text"},
          {"text", system},
          {"cache_control", Value::object({{"type", "ephemeral"}})},
      })});
    } else {
      body["system"] = system;
    }
  }
  if (!tools.empty()) {
    if (explicit_cache &&
        (params.settings.cache_scope == CacheScope::SystemAndTools ||
         params.settings.cache_scope == CacheScope::SystemToolsAndSkills)) {
      // Mark the last tool — caches everything up through tools.
      auto& last = tools.back();
      auto object = last.as_object();
      object["cache_control"] = Value::object({{"type", "ephemeral"}});
      last = Value(std::move(object));
    }
    body["tools"] = Value(tools);
  }
  if (explicit_cache && params.settings.cache_scope == CacheScope::SystemToolsAndSkills) {
    // Mark the last user/system message block as the "skills" anchor when present.
    auto messages = body.at("messages").as_array();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
      const auto role = it->at("role").as_string();
      if (role != "user" && role != "system") {
        continue;
      }
      auto message_object = it->as_object();
      const auto& content_value = message_object.count("content") ? message_object.at("content") : Value();
      if (content_value.is_array() && !content_value.as_array().empty()) {
        auto content_array = content_value.as_array();
        auto& last_part = content_array.back();
        if (last_part.is_object()) {
          auto part_object = last_part.as_object();
          part_object["cache_control"] = Value::object({{"type", "ephemeral"}});
          last_part = Value(std::move(part_object));
          message_object["content"] = Value(std::move(content_array));
          *it = Value(std::move(message_object));
        }
      } else if (content_value.is_string()) {
        // Promote to structured form so we can attach cache_control.
        message_object["content"] = Value::array({Value::object({
            {"type", "text"},
            {"text", content_value.as_string()},
            {"cache_control", Value::object({{"type", "ephemeral"}})},
        })});
        *it = Value(std::move(message_object));
      }
      break;
    }
    body["messages"] = Value(std::move(messages));
  }
  if (params.settings.temperature) {
    body["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    body["max_tokens"] = *params.settings.max_output_tokens;
  }
  if (params.settings.reasoning && params.settings.reasoning->enabled == true) {
    body["thinking"] = Value::object({
        {"type", "enabled"},
        {"budget_tokens", normalize_reasoning_budget_to_tokens(
                              params.settings.reasoning->budget,
                              ReasoningBudgetTokenTable{1024, 4096, 16384})},
    });
  }
  std::map<std::string, std::string> headers{{"content-type", "application/json"},
                                             {"anthropic-version", "2023-06-01"}};
  if (!api_key.empty()) {
    headers["x-api-key"] = api_key;
  }
  return with_cancellation(
      NativeProviderRequest{"anthropic", "/messages", body, headers, trim_provider_base_url(std::move(base_url))},
      params.cancellation);
}

NativeProviderRequest build_anthropic_messages_stream_request(const GenerateParams& params, std::string model,
                                                              std::string api_key, std::string base_url) {
  auto request = build_anthropic_messages_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_anthropic_messages_response(const Value& raw, std::string provider, std::string model) {
  ModelResponse response;
  response.provider = std::move(provider);
  response.model = raw.at("model").as_string(std::move(model));
  response.id = raw.at("id").as_string();
  response.raw = raw;
  std::string text;
  std::string reasoning;
  std::size_t tool_index = 0;
  for (const auto& block : raw.at("content").as_array()) {
    const auto type = block.at("type").as_string();
    if (type == "text") {
      text += block.at("text").as_string();
    } else if (type == "thinking") {
      if (!reasoning.empty()) {
        reasoning += "\n\n";
      }
      reasoning += block.at("thinking").as_string();
    } else if (type == "tool_use") {
      response.tool_calls.push_back(ToolCall{block.at("id").as_string("tool_" + std::to_string(++tool_index)),
                                             block.at("name").as_string(),
                                             block.at("input").is_object() ? block.at("input") : Value::object({})});
    }
  }
  response.text = text;
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(raw.at("stop_reason").as_string("stop"))
                                                       : "tool_calls";
  return response;
}

std::vector<ModelStreamEvent> parse_anthropic_messages_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model) {
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = provider,
      .model = model,
  });

  std::string response_id;
  std::string response_model = model;
  std::string text;
  std::string reasoning;
  std::string finish_reason = "stop";
  std::map<long long, StreamToolCallState> tool_call_state;
  Value::Array raw_events;
  Value::Object usage_acc;

  const auto merge_usage_fields = [&](const Value& src) {
    if (!src.is_object()) {
      return;
    }
    for (const auto& [key, value] : src.as_object()) {
      if (value.is_number()) {
        usage_acc[key] = value;
      }
    }
  };

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk;
    try {
      chunk = parse_json(event);
    } catch (...) {
      continue;
    }
    raw_events.push_back(chunk);
    const auto type = chunk.at("type").as_string();
    if (type == "message_start") {
      const auto id = chunk.at("message").at("id").as_string();
      if (!id.empty()) {
        response_id = id;
      }
      const auto chunk_model = chunk.at("message").at("model").as_string();
      if (!chunk_model.empty()) {
        response_model = chunk_model;
      }
      // message_start carries input_tokens, cache_creation_input_tokens, cache_read_input_tokens.
      merge_usage_fields(chunk.at("message").at("usage"));
      continue;
    }
    if (type == "message_delta") {
      const auto stop_reason = chunk.at("delta").at("stop_reason").as_string();
      if (!stop_reason.empty()) {
        finish_reason = stop_reason;
      }
      // message_delta carries the final output_tokens.
      merge_usage_fields(chunk.at("usage"));
      continue;
    }
    if (type == "content_block_start" && chunk.at("index").is_number()) {
      const auto& block = chunk.at("content_block");
      if (block.at("type").as_string() == "tool_use") {
        const auto index = chunk.at("index").as_integer();
        auto& current = tool_call_state[index];
        current.id = block.at("id").as_string("tool_" + std::to_string(index + 1));
        current.name = block.at("name").as_string();
        current.input = block.at("input").is_object() ? block.at("input") : Value::object({});
      }
      continue;
    }
    if (type != "content_block_delta") {
      continue;
    }

    const auto& delta = chunk.at("delta");
    const auto delta_type = delta.at("type").as_string();
    if (delta_type == "thinking_delta") {
      const auto reasoning_delta = delta.at("thinking").as_string();
      if (!reasoning_delta.empty()) {
        reasoning += reasoning_delta;
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = provider,
            .model = response_model,
            .delta = reasoning_delta,
            .reasoning = reasoning,
        });
      }
      continue;
    }
    if (delta_type == "text_delta") {
      const auto text_delta = delta.at("text").as_string();
      if (!text_delta.empty()) {
        text += text_delta;
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = provider,
            .model = response_model,
            .delta = text_delta,
            .text = text,
        });
      }
      continue;
    }
    if (delta_type == "input_json_delta" && chunk.at("index").is_number()) {
      const auto index = chunk.at("index").as_integer();
      auto found = tool_call_state.find(index);
      if (found != tool_call_state.end()) {
        const auto piece = delta.at("partial_json").as_string();
        found->second.arguments_text += piece;
        if (!piece.empty()) {
          events.push_back(ModelStreamEvent{
              .type = ModelStreamEventType::ToolCallDelta,
              .provider = provider,
              .model = response_model,
              .tool_call_id = found->second.id,
              .tool_call_name = found->second.name,
              .tool_call_args_delta = piece,
              .tool_call_args_accumulated = found->second.arguments_text,
          });
        }
      }
    }
  }

  ModelResponse response;
  response.provider = provider;
  response.model = response_model;
  response.id = response_id;
  response.text = text;
  Value::Object raw_object{
      {"id", Value(response_id)},
      {"model", Value(response_model)},
      {"events", Value(std::move(raw_events))},
  };
  if (!usage_acc.empty()) {
    raw_object.emplace("usage", Value(std::move(usage_acc)));
  }
  response.raw = Value(std::move(raw_object));
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  for (const auto& [index, tool_call] : tool_call_state) {
    (void)index;
    response.tool_calls.push_back(ToolCall{
        tool_call.id,
        tool_call.name,
        tool_call.arguments_text.empty() ? tool_call.input : parse_tool_arguments_text(tool_call.arguments_text),
    });
  }
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(finish_reason) : "tool_calls";

  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

NativeProviderRequest build_deepseek_messages_request(const GenerateParams& params, std::string model,
                                                      std::string api_key, std::string base_url) {
  auto request = build_anthropic_messages_request(params, std::move(model), {}, std::move(base_url));
  request.provider = "deepseek";
  request.headers.erase("anthropic-version");
  if (!api_key.empty()) {
    request.headers["x-api-key"] = api_key;
  }
  if (request.body.contains("thinking")) {
    request.body["thinking"] = Value::object({{"type", "enabled"}});
  }
  return request;
}

NativeProviderRequest build_deepseek_messages_stream_request(const GenerateParams& params, std::string model,
                                                             std::string api_key, std::string base_url) {
  auto request = build_deepseek_messages_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_deepseek_messages_response(const Value& raw, std::string model) {
  return parse_anthropic_messages_response(raw, "deepseek", std::move(model));
}

NativeProviderRequest build_ollama_chat_request(const GenerateParams& params, std::string model,
                                                std::string base_url) {
  Value::Array tools;
  for (const auto& tool : params.tools) {
    tools.push_back(serialize_chat_tool_descriptor(tool));
  }
  Value options = Value::object({});
  if (params.settings.temperature) {
    options["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    options["num_predict"] = *params.settings.max_output_tokens;
  }
  Value body = Value::object({{"model", model},
                              {"messages", serialize_ollama_messages(params.messages)},
                              {"stream", false},
                              {"think", params.settings.reasoning && params.settings.reasoning->enabled == true},
                              {"options", options}});
  if (!tools.empty()) {
    body["tools"] = Value(tools);
  }
  base_url = trim_provider_base_url(std::move(base_url));
  if (base_url.size() < 4 || base_url.substr(base_url.size() - 4) != "/api") {
    base_url += "/api";
  }
  return with_cancellation(
      NativeProviderRequest{"ollama", "/chat", body, {{"content-type", "application/json"}}, std::move(base_url)},
      params.cancellation);
}

NativeProviderRequest build_ollama_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string base_url) {
  auto request = build_ollama_chat_request(params, std::move(model), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

ModelResponse parse_ollama_chat_response(const Value& raw, std::string model) {
  ModelResponse response;
  response.provider = "ollama";
  response.model = raw.at("model").as_string(std::move(model));
  response.id = raw.at("created_at").as_string();
  response.raw = raw;
  const auto& message = raw.at("message");
  response.text = message.at("content").as_string();
  if (!response.text.empty()) {
    response.content.push_back(text_part(response.text));
  }
  const auto thinking = message.at("thinking").as_string();
  if (!thinking.empty()) {
    response.reasoning = reasoning_from_text(thinking);
  }
  std::size_t index = 0;
  for (const auto& call : message.at("tool_calls").as_array()) {
    response.tool_calls.push_back(ToolCall{call.at("function").at("name").as_string("tool") + "_" +
                                               std::to_string(++index),
                                           call.at("function").at("name").as_string(),
                                           call.at("function").at("arguments").is_object()
                                               ? call.at("function").at("arguments")
                                               : Value::object({})});
  }
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(raw.at("done_reason").as_string("stop"))
                                                       : "tool_calls";
  return response;
}

std::vector<ModelStreamEvent> parse_ollama_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string model) {
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = "ollama",
      .model = model,
  });

  std::string response_id;
  std::string response_model = model;
  std::string text;
  std::string reasoning;
  std::string finish_reason = "stop";
  std::vector<ToolCall> tool_calls;
  Value::Array raw_chunks;

  for (const auto& line : read_lines(chunks)) {
    if (line.empty()) {
      continue;
    }
    Value chunk;
    try {
      chunk = parse_json(line);
    } catch (...) {
      continue;
    }
    raw_chunks.push_back(chunk);
    const auto id = chunk.at("created_at").as_string();
    if (!id.empty()) {
      response_id = id;
    }
    const auto chunk_model = chunk.at("model").as_string();
    if (!chunk_model.empty()) {
      response_model = chunk_model;
    }
    const auto& message = chunk.at("message");
    const auto reasoning_delta = message.at("thinking").as_string();
    if (!reasoning_delta.empty()) {
      reasoning += reasoning_delta;
      events.push_back(ModelStreamEvent{
          .type = ModelStreamEventType::ReasoningDelta,
          .provider = "ollama",
          .model = response_model,
          .delta = reasoning_delta,
          .reasoning = reasoning,
      });
    }
    const auto content_delta = message.at("content").as_string();
    if (!content_delta.empty()) {
      text += content_delta;
      events.push_back(ModelStreamEvent{
          .type = ModelStreamEventType::TextDelta,
          .provider = "ollama",
          .model = response_model,
          .delta = content_delta,
          .text = text,
      });
    }
    const auto& streamed_tool_calls = message.at("tool_calls").as_array();
    if (!streamed_tool_calls.empty()) {
      tool_calls.clear();
      std::size_t index = 0;
      for (const auto& call : streamed_tool_calls) {
        const auto& function = call.at("function");
        const auto name = function.at("name").as_string();
        const auto next_index = ++index;
        const auto synth_id = name.empty() ? "tool_" + std::to_string(next_index)
                                           : name + "_" + std::to_string(next_index);
        const auto arguments_value = function.at("arguments").is_object()
                                         ? function.at("arguments")
                                         : Value::object({});
        // Ollama emits tool_calls as a single fully-formed block (not
        // character-by-character like OpenAI). Surface it as a one-shot
        // ToolCallDelta so downstream consumers can transition into a
        // preparing state before ToolStart, matching OpenAI/Anthropic.
        const auto args_serialized = arguments_value.stringify(0);
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ToolCallDelta,
            .provider = "ollama",
            .model = response_model,
            .tool_call_id = synth_id,
            .tool_call_name = name,
            .tool_call_args_delta = args_serialized,
            .tool_call_args_accumulated = args_serialized,
        });
        tool_calls.push_back(ToolCall{synth_id, name, arguments_value});
      }
    }
    if (chunk.at("done").as_bool(false)) {
      finish_reason = chunk.at("done_reason").as_string("stop");
      break;
    }
  }

  ModelResponse response;
  response.provider = "ollama";
  response.model = response_model;
  response.id = response_id;
  response.text = text;
  response.raw = Value::object({
      {"id", response_id},
      {"model", response_model},
      {"chunks", Value(std::move(raw_chunks))},
  });
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  response.tool_calls = std::move(tool_calls);
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(finish_reason) : "tool_calls";

  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

NativeProviderRequest build_gemini_generate_content_request(const GenerateParams& params, std::string model,
                                                            std::string api_key, std::string base_url) {
  Value system_instruction;
  Value body = Value::object({{"contents", serialize_gemini_contents(params.messages, system_instruction)},
                              {"generationConfig", Value::object({})}});
  if (!system_instruction.is_null()) {
    body["systemInstruction"] = system_instruction;
  }
  const auto tools = serialize_gemini_tools(params.tools);
  if (!tools.is_null()) {
    body["tools"] = tools;
  }
  if (params.settings.temperature) {
    body["generationConfig"]["temperature"] = *params.settings.temperature;
  }
  if (params.settings.max_output_tokens) {
    body["generationConfig"]["maxOutputTokens"] = *params.settings.max_output_tokens;
  }
  if (params.settings.reasoning && params.settings.reasoning->enabled == true) {
    body["generationConfig"]["thinkingConfig"] = Value::object({
        {"includeThoughts", params.settings.reasoning->include_thoughts.value_or(true)},
        {"thinkingBudget", normalize_reasoning_budget_to_tokens(
                               params.settings.reasoning->budget,
                               ReasoningBudgetTokenTable{1024, 8192, 24576})},
    });
  }
  std::map<std::string, std::string> headers{{"content-type", "application/json"}};
  if (!api_key.empty()) {
    headers["x-goog-api-key"] = api_key;
  }
  return with_cancellation(
      NativeProviderRequest{"gemini", "/models/" + model + ":generateContent", body, headers,
                            trim_provider_base_url(std::move(base_url))},
      params.cancellation);
}

NativeProviderRequest build_gemini_generate_content_stream_request(const GenerateParams& params, std::string model,
                                                                   std::string api_key, std::string base_url) {
  auto request = build_gemini_generate_content_request(params, model, std::move(api_key), std::move(base_url));
  request.endpoint = "/models/" + std::move(model) + ":streamGenerateContent?alt=sse";
  return request;
}

ModelResponse parse_gemini_generate_content_response(const Value& raw, std::string model) {
  ModelResponse response;
  response.provider = "gemini";
  response.model = raw.at("modelVersion").as_string(std::move(model));
  response.id = raw.at("responseId").as_string();
  response.raw = raw;
  const auto& candidates = raw.at("candidates").as_array();
  if (candidates.empty()) {
    response.finish_reason = "stop";
    return response;
  }
  const auto& candidate = candidates.front();
  std::string text;
  std::string reasoning;
  for (const auto& part : candidate.at("content").at("parts").as_array()) {
    if (part.at("functionCall").is_object()) {
      response.tool_calls.push_back(ToolCall{part.at("functionCall").at("id").as_string(),
                                             part.at("functionCall").at("name").as_string(),
                                             part.at("functionCall").at("args").is_object()
                                                 ? part.at("functionCall").at("args")
                                                 : Value::object({})});
      continue;
    }
    if (part.at("thought").as_bool(false)) {
      reasoning += part.at("text").as_string();
    } else {
      text += part.at("text").as_string();
    }
  }
  response.text = text;
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(candidate.at("finishReason").as_string("stop"))
                                                       : "tool_calls";
  return response;
}

std::vector<ModelStreamEvent> parse_gemini_generate_content_stream_events(
    const std::vector<std::string>& chunks,
    std::string model) {
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = "gemini",
      .model = model,
  });

  std::string response_id;
  std::string response_model = model;
  std::string text;
  std::string reasoning;
  std::string finish_reason = "stop";
  std::vector<ToolCall> tool_calls;
  Value::Array raw_chunks;
  Value captured_usage_metadata;

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk;
    try {
      chunk = parse_json(event);
    } catch (...) {
      continue;
    }
    raw_chunks.push_back(chunk);
    const auto id = chunk.at("responseId").as_string();
    if (!id.empty()) {
      response_id = id;
    }
    const auto chunk_model = chunk.at("modelVersion").as_string();
    if (!chunk_model.empty()) {
      response_model = chunk_model;
    }
    // Gemini emits usageMetadata on the terminal chunk; later chunks may
    // overwrite earlier ones — keep the most recent populated copy.
    if (chunk.at("usageMetadata").is_object()) {
      captured_usage_metadata = chunk.at("usageMetadata");
    }
    const auto& candidates = chunk.at("candidates").as_array();
    if (candidates.empty()) {
      continue;
    }
    const auto& candidate = candidates.front();
    const auto finish = candidate.at("finishReason").as_string();
    if (!finish.empty()) {
      finish_reason = finish;
    }
    for (const auto& part : candidate.at("content").at("parts").as_array()) {
      if (part.at("functionCall").is_object()) {
        const auto& function_call = part.at("functionCall");
        const auto fallback_id = "tool_" + std::to_string(tool_calls.size() + 1);
        const auto call_id = function_call.at("id").as_string(fallback_id);
        const auto call_name = function_call.at("name").as_string();
        const auto args_value = function_call.at("args").is_object()
                                    ? function_call.at("args")
                                    : Value::object({});
        // Gemini emits each functionCall as a single fully-formed part (not
        // character-by-character like OpenAI). Surface a one-shot
        // ToolCallDelta so downstream consumers see a preparing state before
        // ToolStart, matching OpenAI/Anthropic delta semantics.
        const auto args_serialized = args_value.stringify(0);
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ToolCallDelta,
            .provider = "gemini",
            .model = response_model,
            .tool_call_id = call_id,
            .tool_call_name = call_name,
            .tool_call_args_delta = args_serialized,
            .tool_call_args_accumulated = args_serialized,
        });
        tool_calls.push_back(ToolCall{call_id, call_name, args_value});
        continue;
      }
      const auto text_delta = part.at("text").as_string();
      if (text_delta.empty()) {
        continue;
      }
      if (part.at("thought").as_bool(false)) {
        reasoning += text_delta;
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = "gemini",
            .model = response_model,
            .delta = text_delta,
            .reasoning = reasoning,
        });
      } else {
        text += text_delta;
        events.push_back(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = "gemini",
            .model = response_model,
            .delta = text_delta,
            .text = text,
        });
      }
    }
  }

  ModelResponse response;
  response.provider = "gemini";
  response.model = response_model;
  response.id = response_id;
  response.text = text;
  Value::Object raw_object{
      {"responseId", Value(response_id)},
      {"modelVersion", Value(response_model)},
      {"chunks", Value(std::move(raw_chunks))},
  };
  if (captured_usage_metadata.is_object()) {
    raw_object.emplace("usageMetadata", captured_usage_metadata);
  }
  response.raw = Value(std::move(raw_object));
  if (!text.empty()) {
    response.content.push_back(text_part(text));
  }
  if (!reasoning.empty()) {
    response.reasoning = reasoning_from_text(reasoning);
  }
  response.tool_calls = std::move(tool_calls);
  response.finish_reason = response.tool_calls.empty() ? normalize_finish_reason(finish_reason) : "tool_calls";

  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

class IncrementalParsedStreamEmitter {
 public:
  using Parser = std::function<std::vector<ModelStreamEvent>(const std::vector<std::string>&)>;

  IncrementalParsedStreamEmitter(Parser parser, ModelSettings settings)
      : parser_(std::move(parser)),
        settings_(std::move(settings)),
        reasoning_parser_(settings_.reasoning && !settings_.reasoning->tag_name.empty()
                              ? settings_.reasoning->tag_name
                              : std::string("think")) {}

  void feed(std::string_view chunk, const ChatModelAdapter::StreamEventHandler& on_event) {
    chunks_.emplace_back(chunk.data(), chunk.size());
    const auto parsed = parser_(chunks_);
    emit_new_events(parsed, on_event);
  }

  void finish(const ChatModelAdapter::StreamEventHandler& on_event) {
    const auto parsed = parser_(chunks_);
    emit_new_events(parsed, on_event);
    const auto response_it = std::find_if(parsed.begin(), parsed.end(), [](const ModelStreamEvent& event) {
      return event.type == ModelStreamEventType::Response;
    });
    if (response_it == parsed.end()) {
      return;
    }
    ModelStreamEvent response_event = *response_it;
    if (reasoning_enabled()) {
      emit_tagged_deltas(reasoning_parser_.finish(), current_provider_, current_model_, on_event);
      if (saw_text_delta_) {
        response_event.response.text = reasoning_parser_.text();
        response_event.response.content = response_event.response.text.empty()
                                              ? std::vector<MessageContentPart>{}
                                              : std::vector<MessageContentPart>{text_part(response_event.response.text)};
      }
      if (!response_event.response.reasoning && native_reasoning_) {
        response_event.response.reasoning = native_reasoning_;
      }
      response_event.response.reasoning =
          merge_model_reasoning(response_event.response.reasoning, reasoning_parser_.to_reasoning());
    }
    if (on_event) {
      on_event(response_event);
    }
  }

 private:
  bool reasoning_enabled() const {
    return settings_.reasoning && settings_.reasoning->enabled == true;
  }

  void emit_tagged_deltas(const std::vector<TaggedReasoningDelta>& deltas,
                          const std::string& provider,
                          const std::string& model,
                          const ChatModelAdapter::StreamEventHandler& on_event) {
    for (const auto& item : deltas) {
      if (!on_event) {
        continue;
      }
      if (item.type == TaggedReasoningDeltaType::Reasoning) {
        on_event(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = provider,
            .model = model,
            .delta = item.delta,
            .reasoning = item.reasoning_text,
        });
      } else {
        on_event(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = provider,
            .model = model,
            .delta = item.delta,
            .text = item.text,
        });
      }
    }
  }

  void emit_model_event(const ModelStreamEvent& event,
                        const ChatModelAdapter::StreamEventHandler& on_event) {
    if (!event.provider.empty()) {
      current_provider_ = event.provider;
    }
    if (!event.model.empty()) {
      current_model_ = event.model;
    }

    if (event.type == ModelStreamEventType::TextDelta && reasoning_enabled()) {
      saw_text_delta_ = true;
      emit_tagged_deltas(reasoning_parser_.feed(event.delta), event.provider, event.model, on_event);
      return;
    }
    if (event.type == ModelStreamEventType::ReasoningDelta && !event.reasoning.empty()) {
      native_reasoning_ = build_model_reasoning(event.reasoning, "provider-visible");
    }
    if (on_event) {
      on_event(event);
    }
  }

  void emit_new_events(const std::vector<ModelStreamEvent>& parsed,
                       const ChatModelAdapter::StreamEventHandler& on_event) {
    const auto response_it = std::find_if(parsed.begin(), parsed.end(), [](const ModelStreamEvent& event) {
      return event.type == ModelStreamEventType::Response;
    });
    const auto response_index = response_it == parsed.end()
                                    ? parsed.size()
                                    : static_cast<std::size_t>(std::distance(parsed.begin(), response_it));
    if (raw_cursor_ > response_index) {
      raw_cursor_ = 0;
    }
    for (std::size_t index = raw_cursor_; index < response_index; ++index) {
      emit_model_event(parsed[index], on_event);
    }
    raw_cursor_ = response_index;
  }

  Parser parser_;
  ModelSettings settings_;
  TaggedReasoningStreamParser reasoning_parser_;
  std::vector<std::string> chunks_;
  std::size_t raw_cursor_ = 0;
  std::string current_provider_;
  std::string current_model_;
  std::optional<ModelReasoning> native_reasoning_;
  bool saw_text_delta_ = false;
};

OpenAICompatibleChatModelAdapter::OpenAICompatibleChatModelAdapter(std::string provider, std::string model,
                                                                   NativeProviderTransport transport,
                                                                   std::string endpoint,
                                                                   std::string base_url,
                                                                   std::string api_key,
                                                                   NativeProviderStreamTransport stream_transport,
                                                                   std::string organization,
                                                                   NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter(provider, std::move(model), 0.2, 1024,
                       openai_compatible_capabilities(provider)),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      endpoint_(std::move(endpoint)),
      base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      organization_(std::move(organization)) {
  if (!transport_) {
    throw ConfigurationError("OpenAICompatibleChatModelAdapter requires a transport callback.");
  }
}

ModelResponse OpenAICompatibleChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  if (provider() == "openai") {
    require_chat_api_key(api_key_, "OpenAI", "OPENAI_API_KEY");
  }
  const bool use_responses = provider() == "openai" &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (provider() == "openai") {
    apply_openai_prompt_cache_key(request, resolved_params);
    if (!use_responses) {
      apply_openai_reasoning_effort(request, resolved_params.settings.reasoning);
    }
  }
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (provider() == "openai" && !organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(params.cancellation);
  const Value raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  if (use_responses) {
    return build_response(apply_tagged_reasoning(
        parse_openai_responses_response(raw, provider(), settings.model),
        settings));
  }
  return build_response(apply_tagged_reasoning(
      parse_openai_chat_response(raw, provider(), settings.model),
      settings));
}

std::vector<ModelStreamEvent> OpenAICompatibleChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  if (provider() == "openai") {
    require_chat_api_key(api_key_, "OpenAI", "OPENAI_API_KEY");
  }
  const bool use_responses = provider() == "openai" &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_stream_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_stream_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (provider() == "openai") {
    apply_openai_prompt_cache_key(request, resolved_params);
    if (!use_responses) {
      apply_openai_reasoning_effort(request, resolved_params.settings.reasoning);
    }
  }
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (provider() == "openai" && !organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      use_responses
          ? parse_openai_responses_stream_events(chunks, provider(), settings.model)
          : parse_openai_chat_stream_events(chunks, provider(), settings.model),
      settings);
}

void OpenAICompatibleChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  if (provider() == "openai") {
    require_chat_api_key(api_key_, "OpenAI", "OPENAI_API_KEY");
  }
  const bool use_responses = provider() == "openai" &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_stream_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_stream_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (provider() == "openai") {
    apply_openai_prompt_cache_key(request, resolved_params);
    if (!use_responses) {
      apply_openai_reasoning_effort(request, resolved_params.settings.reasoning);
    }
  }
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (provider() == "openai" && !organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  IncrementalParsedStreamEmitter emitter(
      [use_responses, provider = provider(), model = settings.model](const std::vector<std::string>& chunks) {
        return use_responses
                   ? parse_openai_responses_stream_events(chunks, provider, model)
                   : parse_openai_chat_stream_events(chunks, provider, model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

QwenChatModelAdapter::QwenChatModelAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url,
                                           NativeProviderStreamTransport stream_transport,
                                           std::string reasoning_api,
                                           NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("qwen", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent", "reasoning",
                        "reasoning.budget", "reasoning.includeThoughts"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      reasoning_api_(std::move(reasoning_api)) {
  if (!transport_) {
    throw ConfigurationError("QwenChatModelAdapter requires a transport callback.");
  }
  if (reasoning_api_.empty()) {
    reasoning_api_ = "chat-completions";
  }
  if (reasoning_api_ != "chat-completions" && reasoning_api_ != "responses") {
    throw ConfigurationError("Qwen reasoningApi must be \"chat-completions\" or \"responses\".");
  }
}

ModelResponse QwenChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(
      use_responses ? parse_qwen_responses_response(raw, settings.model)
                    : parse_qwen_chat_response(raw, settings.model),
      settings));
}

std::vector<ModelStreamEvent> QwenChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_stream_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      use_responses
          ? parse_qwen_responses_stream_events(chunks, settings.model)
          : parse_openai_chat_stream_events(chunks, "qwen", settings.model),
      settings);
}

void QwenChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_stream_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [use_responses, model = settings.model](const std::vector<std::string>& chunks) {
        return use_responses
                   ? parse_qwen_responses_stream_events(chunks, model)
                   : parse_openai_chat_stream_events(chunks, "qwen", model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

MiMoChatModelAdapter::MiMoChatModelAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url,
                                           NativeProviderStreamTransport stream_transport,
                                           NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("mimo", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent",
                        "transport.inline", "transport.reference"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("MiMoChatModelAdapter requires a transport callback.");
  }
}

ModelResponse MiMoChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(
      parse_mimo_chat_response(raw, settings.model), settings));
}

std::vector<ModelStreamEvent> MiMoChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_openai_chat_stream_events(chunks, "mimo", settings.model), settings);
}

void MiMoChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_openai_chat_stream_events(chunks, "mimo", model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

AnthropicChatModelAdapter::AnthropicChatModelAdapter(std::string model, NativeProviderTransport transport,
                                                     std::string api_key, std::string base_url,
                                                     NativeProviderStreamTransport stream_transport,
                                                     NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("anthropic", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent", "reasoning",
                        "reasoning.budget"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("AnthropicChatModelAdapter requires a transport callback.");
  }
}

ModelResponse AnthropicChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(
      parse_anthropic_messages_response(raw, "anthropic", settings.model),
      settings));
}

std::vector<ModelStreamEvent> AnthropicChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_anthropic_messages_stream_events(chunks, "anthropic", settings.model),
      settings);
}

void AnthropicChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_anthropic_messages_stream_events(chunks, "anthropic", model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

DeepSeekChatModelAdapter::DeepSeekChatModelAdapter(std::string model, NativeProviderTransport transport,
                                                   std::string api_key, std::string base_url,
                                                   NativeProviderStreamTransport stream_transport,
                                                   NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("deepseek", std::move(model), 0.2, 1024,
                       {"input.text", "output.structuredContent", "reasoning"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("DeepSeekChatModelAdapter requires a transport callback.");
  }
}

ModelResponse DeepSeekChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(parse_deepseek_messages_response(raw, settings.model), settings));
}

std::vector<ModelStreamEvent> DeepSeekChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_anthropic_messages_stream_events(chunks, "deepseek", settings.model),
      settings);
}

void DeepSeekChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_anthropic_messages_stream_events(chunks, "deepseek", model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

OllamaChatModelAdapter::OllamaChatModelAdapter(std::string model, NativeProviderTransport transport,
                                               std::string base_url,
                                               NativeProviderStreamTransport stream_transport,
                                               NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("ollama", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "output.structuredContent", "reasoning"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("OllamaChatModelAdapter requires a transport callback.");
  }
}

ModelResponse OllamaChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto request = build_ollama_chat_request(params, settings.model, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(parse_ollama_chat_response(raw, settings.model), settings));
}

std::vector<ModelStreamEvent> OllamaChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto request = build_ollama_chat_stream_request(params, settings.model, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_ollama_chat_stream_events(chunks, settings.model),
      settings);
}

void OllamaChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  const auto request = build_ollama_chat_stream_request(params, settings.model, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_ollama_chat_stream_events(chunks, model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

GeminiChatModelAdapter::GeminiChatModelAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url,
                                               NativeProviderStreamTransport stream_transport,
                                               NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("gemini", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent", "reasoning",
                        "reasoning.budget", "reasoning.includeThoughts"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("GeminiChatModelAdapter requires a transport callback.");
  }
}

ModelResponse GeminiChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_response(apply_tagged_reasoning(
      parse_gemini_generate_content_response(raw, settings.model),
      settings));
}

std::vector<ModelStreamEvent> GeminiChatModelAdapter::stream(const GenerateParams& params) {
  if (!stream_transport_) {
    return ChatModelAdapter::stream(params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_gemini_generate_content_stream_events(chunks, settings.model),
      settings);
}

void GeminiChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    return ChatModelAdapter::stream(params, std::move(on_event));
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_gemini_generate_content_stream_events(chunks, model);
      },
      settings);
  throw_if_model_cancelled(params.cancellation);
  streaming_transport_(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(params.cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(params.cancellation);
  emitter.finish(on_event);
}

LlamaCppNativeChatModelAdapter::LlamaCppNativeChatModelAdapter(
    LlamaCppNativeChatModelAdapterConfig config)
    : ChatModelAdapter("llamacpp-native",
                       default_llama_model_name(config.model, config.model_path),
                       config.temperature.value_or(0.2),
                       config.max_output_tokens.value_or(1024),
                       llama_model_capabilities(config),
                       config.reasoning),
      native_config_(config),
      sampling_(config),
      chat_template_(std::move(config.chat_template)),
      strict_template_(config.strict_template),
      tool_mode_(config.tool_mode.empty() ? "auto" : std::move(config.tool_mode)),
      grammar_(std::move(config.grammar)),
      grammar_root_(std::move(config.grammar_root)),
      session_id_(std::move(config.session_id)),
      binding_(config.binding ? std::move(config.binding) : create_llama_cpp_native_binding()) {
  if (native_config_.model_path.empty()) {
    throw ConfigurationError("LlamaCppNativeChatModelAdapter requires model_path.");
  }
  if (!binding_ || !binding_->generate_chat) {
    throw ConfigurationError("LlamaCppNativeChatModelAdapter requires a native binding.");
  }
}

const LlamaCppNativeRuntimeConfig& LlamaCppNativeChatModelAdapter::native_config() const noexcept {
  return native_config_;
}

LlamaCppNativeChatRequest LlamaCppNativeChatModelAdapter::build_request(
    const GenerateParams& params,
    const ModelSettings& settings) const {
  const auto tool_mode = resolve_llama_tool_mode(tool_mode_, settings.extra);
  const bool tools_active = !params.tools.empty() && tool_mode != "ignore";
  if (!params.tools.empty() && tool_mode == "error") {
    throw AdapterError("llama.cpp native provider does not support tool calling.");
  }

  LlamaCppNativeChatRequest request;
  request.request_id = generate_uuid();
  request.model = settings.model;
  request.tools = tools_active ? params.tools : std::vector<ChatToolDescriptor>{};
  request.tool_mode = tool_mode;
  request.chat_template = chat_template_;
  request.strict_template = strict_template_;
  request.grammar_root = optional_string_from_value(settings.extra, "grammarRoot",
                         optional_string_from_value(settings.extra, "grammar_root", grammar_root_));
  request.session_id = optional_string_from_value(settings.extra, "sessionId",
                       optional_string_from_value(settings.extra, "session_id", session_id_));
  request.reasoning_tag_name = settings.reasoning && !settings.reasoning->tag_name.empty()
                                   ? settings.reasoning->tag_name
                                   : "think";
  request.output_schema = resolve_llama_output_schema(settings.extra);
  request.temperature = settings.temperature ? settings.temperature : sampling_.temperature;
  request.max_output_tokens = settings.max_output_tokens ? settings.max_output_tokens
                                                         : sampling_.max_output_tokens;
  request.top_k = optional_int_from_value(settings.extra, "topK");
  request.top_p = optional_number_from_value(settings.extra, "topP");
  request.min_p = optional_number_from_value(settings.extra, "minP");
  request.repeat_penalty = optional_number_from_value(settings.extra, "repeatPenalty");
  request.seed = optional_int_from_value(settings.extra, "seed");
  if (!request.top_k) request.top_k = sampling_.top_k;
  if (!request.top_p) request.top_p = sampling_.top_p;
  if (!request.min_p) request.min_p = sampling_.min_p;
  if (!request.repeat_penalty) request.repeat_penalty = sampling_.repeat_penalty;
  if (!request.seed) request.seed = sampling_.seed;

  std::vector<std::string> instructions;
  if (tools_active) {
    instructions.push_back(build_llama_tool_instruction(request.tools, request.tool_mode));
  }
  if (request.output_schema) {
    instructions.push_back(build_llama_structured_output_instruction(*request.output_schema));
  }
  if (!instructions.empty()) {
    request.messages.push_back(LlamaCppNativeChatMessage{
        .role = "system",
        .content = join_strings(instructions, "\n\n"),
    });
  }

  for (const auto& message : params.messages) {
    auto serialized = serialize_llama_message(message,
                                              !native_config_.mmproj_path.empty(),
                                              native_config_.media_marker);
    request.messages.push_back(std::move(serialized.message));
    request.media.insert(request.media.end(),
                         std::make_move_iterator(serialized.media.begin()),
                         std::make_move_iterator(serialized.media.end()));
  }

  request.grammar = optional_string_from_value(settings.extra, "grammar");
  if (request.grammar.empty()) {
    if (tools_active) {
      request.grammar = llama_tool_envelope_gbnf(request.tools, request.tool_mode == "required");
    } else if (request.output_schema) {
      request.grammar = json_schema_to_gbnf(*request.output_schema);
    } else {
      request.grammar = grammar_;
    }
  }
  return request;
}

ModelResponse LlamaCppNativeChatModelAdapter::build_model_response(
    const LlamaCppNativeChatResult& raw,
    const LlamaCppNativeChatRequest& request,
    bool reasoning_enabled,
    const std::string& fallback_text) const {
  const auto raw_text = raw.text.empty() ? fallback_text : raw.text;
  const auto extracted = reasoning_enabled
                             ? extract_tagged_reasoning(raw_text, request.reasoning_tag_name.empty()
                                                                      ? std::string("think")
                                                                      : request.reasoning_tag_name)
                             : TaggedReasoningExtraction{.text = raw_text};
  const auto parsed = parse_llama_tool_envelope(extracted.text,
                                                request.tools,
                                                request.tool_mode,
                                                request.output_schema);
  ModelResponse response;
  response.id = raw.id;
  response.provider = "llamacpp-native";
  response.model = raw.model.empty() ? model() : raw.model;
  response.text = parsed.text;
  response.content = response.text.empty() ? std::vector<MessageContentPart>{}
                                           : std::vector<MessageContentPart>{text_part(response.text)};
  response.reasoning = merge_model_reasoning(raw.reasoning, extracted.reasoning);
  response.tool_calls = parsed.tool_calls;
  response.finish_reason = normalize_finish_reason(parsed.finish_reason.empty()
                                                       ? raw.finish_reason
                                                       : parsed.finish_reason);
  response.raw = raw.raw.is_null() ? llama_chat_result_to_value(raw) : raw.raw;
  return build_response(std::move(response));
}

ModelResponse LlamaCppNativeChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto request = build_request(params, settings);
  CancellationCallbackScope cancellation(params.cancellation, binding_, request.request_id);
  cancellation.throw_if_cancelled();
  const auto raw = binding_->generate_chat(native_config_, request, {});
  cancellation.throw_if_cancelled();
  return build_model_response(raw,
                              request,
                              settings.reasoning && settings.reasoning->enabled == true);
}

std::vector<ModelStreamEvent> LlamaCppNativeChatModelAdapter::stream(const GenerateParams& params) {
  std::vector<ModelStreamEvent> events;
  stream(params, [&](const ModelStreamEvent& event) {
    events.push_back(event);
  });
  return events;
}

void LlamaCppNativeChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  const auto settings = resolve_settings(params.settings);
  const auto request = build_request(params, settings);
  CancellationCallbackScope cancellation(params.cancellation, binding_, request.request_id);
  cancellation.throw_if_cancelled();
  const bool tools_active = !request.tools.empty() && request.tool_mode != "ignore";
  const bool reasoning_enabled = !tools_active && settings.reasoning && settings.reasoning->enabled == true;
  if (on_event) {
    on_event(ModelStreamEvent{
        .type = ModelStreamEventType::ResponseStart,
        .provider = "llamacpp-native",
        .model = request.model,
    });
  }
  TaggedReasoningStreamParser parser(request.reasoning_tag_name.empty()
                                         ? std::string("think")
                                         : request.reasoning_tag_name);
  std::string visible_text;
  std::string reasoning_text;
  const auto append_delta = [&](const LlamaCppNativeChatDelta& delta) {
    if (delta.type == "reasoning") {
      reasoning_text += delta.delta;
      if (on_event) {
        on_event(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = "llamacpp-native",
            .model = request.model,
            .delta = delta.delta,
            .reasoning = reasoning_text,
        });
      }
      return;
    }
    if (!reasoning_enabled) {
      visible_text += delta.delta;
      if (!tools_active && on_event) {
        on_event(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = "llamacpp-native",
            .model = request.model,
            .delta = delta.delta,
            .text = visible_text,
        });
      }
      return;
    }
    for (const auto& parsed : parser.feed(delta.delta)) {
      if (parsed.type == TaggedReasoningDeltaType::Reasoning) {
        if (on_event) {
          on_event(ModelStreamEvent{
              .type = ModelStreamEventType::ReasoningDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .reasoning = parsed.reasoning_text,
          });
        }
      } else {
        visible_text = parsed.text;
        if (on_event) {
          on_event(ModelStreamEvent{
              .type = ModelStreamEventType::TextDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .text = parsed.text,
          });
        }
      }
    }
  };
  const auto raw = binding_->generate_chat(native_config_, request, append_delta);
  cancellation.throw_if_cancelled();
  if (reasoning_enabled) {
    for (const auto& parsed : parser.finish()) {
      if (parsed.type == TaggedReasoningDeltaType::Reasoning) {
        if (on_event) {
          on_event(ModelStreamEvent{
              .type = ModelStreamEventType::ReasoningDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .reasoning = parsed.reasoning_text,
          });
        }
      } else {
        visible_text = parsed.text;
        if (on_event) {
          on_event(ModelStreamEvent{
              .type = ModelStreamEventType::TextDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .text = parsed.text,
          });
        }
      }
    }
  }
  if (on_event) {
    on_event(ModelStreamEvent{
        .type = ModelStreamEventType::Response,
        .provider = "llamacpp-native",
        .model = raw.model.empty() ? request.model : raw.model,
        .response = build_model_response(raw,
                                         request,
                                         reasoning_enabled,
                                         visible_text),
    });
  }
}

ChatProviderRegistry::ChatProviderRegistry(const ChatProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

ChatProviderRegistry& ChatProviderRegistry::operator=(const ChatProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

ChatProviderRegistry::ChatProviderRegistry(ChatProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

ChatProviderRegistry& ChatProviderRegistry::operator=(ChatProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

ChatProviderRegistry& ChatProviderRegistry::register_provider(std::string provider, ChatAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Chat provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Chat provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool ChatProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<ChatModelAdapter> ChatProviderRegistry::create(const std::string& provider,
                                                               const Value& config) const {
  ChatAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown chat provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Chat provider factory returned null for: " + provider);
  }
  return adapter;
}

TextEmbeddingAdapter::TextEmbeddingAdapter(std::string provider, std::string model, int dimensions,
                                           std::string space_id)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      dimensions_(dimensions),
      space_id_(std::move(space_id)) {
  if (provider_.empty()) {
    throw ConfigurationError("Embedding provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Embedding model is required.");
  }
}

const std::string& TextEmbeddingAdapter::provider() const noexcept {
  return provider_;
}

const std::string& TextEmbeddingAdapter::model() const noexcept {
  return model_;
}

int TextEmbeddingAdapter::dimensions() const noexcept {
  return dimensions_;
}

const std::string& TextEmbeddingAdapter::space_id() const noexcept {
  return space_id_;
}

EmbeddingSettings TextEmbeddingAdapter::resolve_settings(const EmbeddingSettings& settings) const {
  return merge_embedding_settings(provider_, model_, dimensions_, space_id_, settings, {"text"});
}

EmbeddingSpaceDescriptor TextEmbeddingAdapter::space() const {
  return EmbeddingSpaceDescriptor{
      .id = space_id_.empty() ? provider_ + ":" + model_ : space_id_,
      .modalities = {"text"},
      .dimensions = dimensions_,
  };
}

EmbeddingVector TextEmbeddingAdapter::embed_one(const std::string& text, const Value& settings,
                                                CancellationToken* cancellation) {
  auto vectors = embed({text}, settings, cancellation);
  return vectors.empty() ? EmbeddingVector{} : vectors.front();
}

HashEmbeddingAdapter::HashEmbeddingAdapter(int dimensions, std::string model, std::string space_id)
    : TextEmbeddingAdapter("hash", std::move(model), dimensions, std::move(space_id)) {}

std::vector<EmbeddingVector> HashEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                         const Value& settings,
                                                         CancellationToken* cancellation) {
  throw_if_model_cancelled(cancellation);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(texts.size());
  const int dims = dimensions_from_settings(settings, dimensions());
  for (const auto& text : texts) {
    throw_if_model_cancelled(cancellation);
    EmbeddingVector vector(static_cast<std::size_t>(dims), 0);
    for (const auto& token : tokenize(text)) {
      const auto index = static_cast<std::size_t>(hash_token(token) % static_cast<std::uint32_t>(dims));
      vector[index] += 1;
    }
    vectors.push_back(normalize_vector(std::move(vector)));
  }
  throw_if_model_cancelled(cancellation);
  return vectors;
}

ClipTextEmbeddingAdapter::ClipTextEmbeddingAdapter(ClipTextEmbeddingBatch embed_batch)
    : ClipTextEmbeddingAdapter("Xenova/clip-vit-base-patch32", 0, "clip-shared-v1", std::move(embed_batch)) {}

ClipTextEmbeddingAdapter::ClipTextEmbeddingAdapter(std::string model, int dimensions,
                                                   std::string space_id,
                                                   ClipTextEmbeddingBatch embed_batch)
    : TextEmbeddingAdapter("clip", std::move(model), dimensions, std::move(space_id)),
      embed_batch_(std::move(embed_batch)) {}

std::vector<EmbeddingVector> ClipTextEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                             const Value& settings,
                                                             CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (!embed_batch_) {
    throw ConfigurationError("ClipTextEmbeddingAdapter requires an injected batch function.");
  }
  throw_if_model_cancelled(cancellation);
  auto vectors = embed_batch_(texts, resolve_settings(embedding_settings_from_value(settings)));
  throw_if_model_cancelled(cancellation);
  return vectors;
}

OpenAIEmbeddingAdapter::OpenAIEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url,
                                               std::string organization, int dimensions, std::string space_id)
    : TextEmbeddingAdapter("openai", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      organization_(std::move(organization)) {
  if (!transport_) {
    throw ConfigurationError("OpenAIEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> OpenAIEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("OpenAI embeddings require apiKey or OPENAI_API_KEY.");
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"encoding_format", "float"}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  std::map<std::string, std::string> headers{{"content-type", "application/json"},
                                             {"authorization", "Bearer " + api_key_}};
  if (!organization_.empty()) {
    headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "openai",
      .endpoint = "/embeddings",
      .body = body,
      .headers = std::move(headers),
      .base_url = trim_provider_base_url(base_url_),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return indexed_embedding_vectors_from_response(raw);
}

QwenEmbeddingAdapter::QwenEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url, int dimensions,
                                           std::string space_id)
    : TextEmbeddingAdapter("qwen", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("QwenEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> QwenEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                         const Value& settings,
                                                         CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("Qwen embeddings require apiKey or QWEN_API_KEY.");
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"encoding_format", "float"}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "qwen",
      .endpoint = "/embeddings",
      .body = body,
      .headers = {{"content-type", "application/json"}, {"authorization", "Bearer " + api_key_}},
      .base_url = trim_provider_base_url(base_url_),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return indexed_embedding_vectors_from_response(raw);
}

OllamaEmbeddingAdapter::OllamaEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string base_url, int dimensions, std::string space_id)
    : TextEmbeddingAdapter("ollama", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("OllamaEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> OllamaEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"truncate", true}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  auto base_url = trim_provider_base_url(base_url_);
  if (base_url.size() < 4 || base_url.substr(base_url.size() - 4) != "/api") {
    base_url += "/api";
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "ollama",
      .endpoint = "/embed",
      .body = body,
      .headers = {{"content-type", "application/json"}},
      .base_url = std::move(base_url),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return embedding_vectors_from_array_response(raw.at("embeddings"));
}

GeminiEmbeddingAdapter::GeminiEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url, int dimensions,
                                               std::string task_type, std::string space_id)
    : TextEmbeddingAdapter("gemini", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      task_type_(std::move(task_type)) {
  if (!transport_) {
    throw ConfigurationError("GeminiEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> GeminiEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("Gemini embeddings require apiKey or GEMINI_API_KEY.");
  }
  const auto resolved_model = embedding_model_from_settings(settings, model());
  const auto model_resource = "models/" + resolved_model;
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  const auto task_type = embedding_task_type_from_settings(settings, task_type_);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(texts.size());
  for (const auto& text : texts) {
    throw_if_model_cancelled(cancellation);
    Value body = Value::object({
        {"model", model_resource},
        {"content", Value::object({
                        {"parts", Value::array({
                                      Value::object({{"text", text}}),
                                    })},
                    })},
    });
    if (!task_type.empty()) {
      body["taskType"] = task_type;
    }
    if (dims > 0) {
      body["outputDimensionality"] = dims;
    }
    const auto raw = transport_(NativeProviderRequest{
        .provider = "gemini",
        .endpoint = "/" + model_resource + ":embedContent",
        .body = body,
        .headers = {{"content-type", "application/json"}, {"x-goog-api-key", api_key_}},
        .base_url = trim_provider_base_url(base_url_),
        .cancellation = cancellation,
    });
    throw_if_model_cancelled(cancellation);
    vectors.push_back(embedding_vector_from_value(raw.at("embedding").at("values")));
  }
  return vectors;
}

LlamaCppNativeTextEmbeddingAdapter::LlamaCppNativeTextEmbeddingAdapter(
    LlamaCppNativeEmbeddingAdapterConfig config)
    : TextEmbeddingAdapter("llamacpp-native",
                           default_llama_model_name(config.model, config.model_path),
                           config.dimensions.value_or(0),
                           config.space_id.empty() ? "llamacpp-native:" +
                                                         default_llama_model_name(config.model, config.model_path)
                                                   : config.space_id),
      native_config_(config),
      pooling_(config.pooling.empty() ? "mean" : std::move(config.pooling)),
      normalize_(config.normalize),
      binding_(config.binding ? std::move(config.binding) : create_llama_cpp_native_binding()) {
  if (native_config_.model_path.empty()) {
    throw ConfigurationError("LlamaCppNativeTextEmbeddingAdapter requires model_path.");
  }
  if (!binding_ || !binding_->embed_texts) {
    throw ConfigurationError("LlamaCppNativeTextEmbeddingAdapter requires a native binding.");
  }
}

const LlamaCppNativeRuntimeConfig& LlamaCppNativeTextEmbeddingAdapter::native_config() const noexcept {
  return native_config_;
}

std::vector<EmbeddingVector> LlamaCppNativeTextEmbeddingAdapter::embed(
    const std::vector<std::string>& texts,
    const Value& settings,
    CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  throw_if_model_cancelled(cancellation);
  const auto resolved = resolve_settings(embedding_settings_from_value(settings));
  LlamaCppNativeEmbeddingRequest request;
  request.request_id = generate_uuid();
  request.model = resolved.model;
  request.texts = texts;
  request.pooling = optional_string_from_value(settings, "pooling", pooling_);
  request.normalize = settings.is_object() && settings.at("normalize").is_bool()
                          ? settings.at("normalize").as_bool()
                          : normalize_;
  request.dimensions = resolved.dimensions;
  CancellationCallbackScope cancellation_scope(cancellation, binding_, request.request_id);
  const auto raw = binding_->embed_texts(native_config_, request);
  cancellation_scope.throw_if_cancelled();
  const int expected_dimensions = resolved.dimensions.value_or(dimensions());
  if (expected_dimensions > 0) {
    for (std::size_t index = 0; index < raw.embeddings.size(); ++index) {
      if (raw.embeddings[index].size() != static_cast<std::size_t>(expected_dimensions)) {
        throw AdapterError("llama.cpp native embedding dimensions mismatch at index " +
                           std::to_string(index) + ": expected " +
                           std::to_string(expected_dimensions) + ", received " +
                           std::to_string(raw.embeddings[index].size()) + ".");
      }
    }
  }
  return raw.embeddings;
}

ImageEmbeddingAdapter::ImageEmbeddingAdapter(std::string provider, std::string model, int dimensions,
                                             std::string space_id)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      dimensions_(dimensions),
      space_id_(std::move(space_id)) {
  if (provider_.empty()) {
    throw ConfigurationError("Embedding provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Embedding model is required.");
  }
}

const std::string& ImageEmbeddingAdapter::provider() const noexcept {
  return provider_;
}

const std::string& ImageEmbeddingAdapter::model() const noexcept {
  return model_;
}

int ImageEmbeddingAdapter::dimensions() const noexcept {
  return dimensions_;
}

const std::string& ImageEmbeddingAdapter::space_id() const noexcept {
  return space_id_;
}

EmbeddingSettings ImageEmbeddingAdapter::resolve_settings(const EmbeddingSettings& settings) const {
  return merge_embedding_settings(provider_, model_, dimensions_, space_id_, settings, {"image"});
}

EmbeddingSpaceDescriptor ImageEmbeddingAdapter::space() const {
  return EmbeddingSpaceDescriptor{
      .id = space_id_.empty() ? provider_ + ":" + model_ : space_id_,
      .modalities = {"image"},
      .dimensions = dimensions_,
  };
}

EmbeddingVector ImageEmbeddingAdapter::embed_one(const ImageEmbeddingInput& image, const Value& settings,
                                                 CancellationToken* cancellation) {
  auto vectors = embed({image}, settings, cancellation);
  return vectors.empty() ? EmbeddingVector{} : vectors.front();
}

HashImageEmbeddingAdapter::HashImageEmbeddingAdapter(int dimensions, std::string model, std::string space_id)
    : ImageEmbeddingAdapter("hash", std::move(model), dimensions, std::move(space_id)) {}

std::vector<EmbeddingVector> HashImageEmbeddingAdapter::embed(const std::vector<ImageEmbeddingInput>& images,
                                                              const Value& settings,
                                                              CancellationToken* cancellation) {
  throw_if_model_cancelled(cancellation);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(images.size());
  const int dims = dimensions_from_settings(settings, dimensions());
  for (const auto& image : images) {
    throw_if_model_cancelled(cancellation);
    EmbeddingVector vector(static_cast<std::size_t>(dims), 0);
    for (const auto& token : tokenize(image_to_hint(image))) {
      const auto index = static_cast<std::size_t>(hash_token(token) % static_cast<std::uint32_t>(dims));
      vector[index] += 1;
    }
    vectors.push_back(normalize_vector(std::move(vector)));
  }
  throw_if_model_cancelled(cancellation);
  return vectors;
}

ClipImageEmbeddingAdapter::ClipImageEmbeddingAdapter(ClipImageEmbeddingBatch embed_batch)
    : ClipImageEmbeddingAdapter("Xenova/clip-vit-base-patch32", 0, "clip-shared-v1", std::move(embed_batch)) {}

ClipImageEmbeddingAdapter::ClipImageEmbeddingAdapter(std::string model, int dimensions,
                                                     std::string space_id,
                                                     ClipImageEmbeddingBatch embed_batch)
    : ImageEmbeddingAdapter("clip", std::move(model), dimensions, std::move(space_id)),
      embed_batch_(std::move(embed_batch)) {}

std::vector<EmbeddingVector> ClipImageEmbeddingAdapter::embed(const std::vector<ImageEmbeddingInput>& images,
                                                              const Value& settings,
                                                              CancellationToken* cancellation) {
  if (images.empty()) {
    return {};
  }
  if (!embed_batch_) {
    throw ConfigurationError("ClipImageEmbeddingAdapter requires an injected batch function.");
  }
  throw_if_model_cancelled(cancellation);
  auto vectors = embed_batch_(images, resolve_settings(embedding_settings_from_value(settings)));
  throw_if_model_cancelled(cancellation);
  return vectors;
}

EmbeddingProviderRegistry::EmbeddingProviderRegistry(const EmbeddingProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::operator=(const EmbeddingProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

EmbeddingProviderRegistry::EmbeddingProviderRegistry(EmbeddingProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::operator=(EmbeddingProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::register_provider(std::string provider,
                                                                        EmbeddingAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Embedding provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Embedding provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool EmbeddingProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<TextEmbeddingAdapter> EmbeddingProviderRegistry::create(const std::string& provider,
                                                                        const Value& config) const {
  EmbeddingAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown embedding provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Embedding provider factory returned null for: " + provider);
  }
  return adapter;
}

ImageEmbeddingProviderRegistry::ImageEmbeddingProviderRegistry(const ImageEmbeddingProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::operator=(
    const ImageEmbeddingProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

ImageEmbeddingProviderRegistry::ImageEmbeddingProviderRegistry(ImageEmbeddingProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::operator=(
    ImageEmbeddingProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::register_provider(
    std::string provider, ImageEmbeddingAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Image embedding provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Image embedding provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool ImageEmbeddingProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<ImageEmbeddingAdapter> ImageEmbeddingProviderRegistry::create(const std::string& provider,
                                                                              const Value& config) const {
  ImageEmbeddingAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown image embedding provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Image embedding provider factory returned null for: " + provider);
  }
  return adapter;
}
}  // namespace agent
