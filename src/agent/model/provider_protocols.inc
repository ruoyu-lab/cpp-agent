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
                        {"input_schema", json_schema_to_value(tool.input_schema)},
                        {"readOnly", tool.read_only},
                        {"mutatesFiles", tool.mutates_files},
                        {"interactive", tool.interactive},
                        {"longRunning", tool.long_running},
                        {"batchable", tool.batchable},
                        {"concurrencyKey", tool.concurrency_key},
                        {"sideEffectLevel", tool.side_effect_level}});
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

bool is_provider_media_part(const MessageContentPart& part) {
  return part.type == ContentPartType::Image || part.type == ContentPartType::File ||
         part.type == ContentPartType::Audio || part.type == ContentPartType::Video;
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

std::string media_base64_payload(const MessageContentPart& part) {
  if (part.source.kind == MediaSourceKind::Inline) {
    if (part.source.data.empty()) {
      throw ConfigurationError("Inline media input is empty.");
    }
    return part.source.data;
  }

  DefaultMediaResolver resolver;
  const auto resolved = resolver.resolve(part.source);
  if (resolved.bytes.empty()) {
    throw ConfigurationError("Media input is empty.");
  }
  return base64_encode(resolved.bytes);
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

  if (part.type == ContentPartType::File ||
      part.type == ContentPartType::Audio ||
      part.type == ContentPartType::Video) {
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
      filename = "input-" + content_part_type_label(part.type);
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
        if (is_provider_media_part(part)) {
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

std::string responses_endpoint_from_chat_endpoint(const std::string& endpoint) {
  const std::string suffix = "/chat/completions";
  if (endpoint.size() >= suffix.size() &&
      endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return endpoint.substr(0, endpoint.size() - suffix.size()) + "/responses";
  }
  return "/v1/responses";
}

GenerateParams params_with_resolved_settings(const GenerateParams& params,
                                             const ModelSettings& settings) {
  GenerateParams resolved = params;
  resolved.settings = settings;
  return resolved;
}

void apply_protocol_reasoning(NativeProviderRequest& request,
                              const std::string& provider,
                              const std::string& model,
                              ProviderRequestProtocol protocol,
                              const std::optional<ReasoningSettings>& reasoning) {
  apply_provider_reasoning(request, ProviderReasoningContext{
      .provider = provider,
      .model = model,
      .reasoning = reasoning,
      .profile = make_provider_request_profile(provider, model, protocol),
  });
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
      } else if (part.type == ContentPartType::Audio || part.type == ContentPartType::Video) {
        throw ConfigurationError("Anthropic adapter does not support " +
                                 content_part_type_label(part.type) + " inputs.");
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
      if (part.type == ContentPartType::Image) {
        images.push_back(media_base64_payload(part));
      } else if (part.type == ContentPartType::Audio || part.type == ContentPartType::Video) {
        throw ConfigurationError("Ollama adapter does not support " +
                                 content_part_type_label(part.type) + " inputs.");
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
      if (is_provider_media_part(part) && part.source.kind == MediaSourceKind::Inline) {
        parts.push_back(Value::object({{"inlineData", Value::object({{"mimeType", part.source.mime_type},
                                                                     {"data", part.source.data}})}}));
      } else if (is_provider_media_part(part) && part.source.kind == MediaSourceKind::Url) {
        parts.push_back(Value::object({{"fileData", Value::object({{"mimeType", part.source.mime_type},
                                                                   {"fileUri", part.source.url}})}}));
      } else if (is_provider_media_part(part)) {
        const auto resolved = DefaultMediaResolver().resolve(part.source);
        const auto mime_type = resolved.mime_type.empty() ? std::string("application/octet-stream")
                                                          : resolved.mime_type;
        parts.push_back(Value::object({{"inlineData", Value::object({{"mimeType", mime_type},
                                                                     {"data", base64_encode(resolved.bytes)}})}}));
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

AgentOutput apply_tagged_reasoning(AgentOutput response, const ModelSettings& settings) {
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
  NativeProviderRequest request{
      .provider = "openai",
      .endpoint = std::move(endpoint),
      .body = body,
      .headers = {{"content-type", "application/json"}},
      .base_url = std::move(base_url),
  };
  apply_protocol_reasoning(request,
                           "openai",
                           model,
                           ProviderRequestProtocol::OpenAIResponses,
                           params.settings.reasoning);
  return with_native_provider_cancellation(std::move(request), params.cancellation);
}

NativeProviderRequest build_openai_responses_stream_request(const GenerateParams& params, std::string model,
                                                            std::string endpoint, std::string base_url) {
  auto request = build_openai_responses_request(params, std::move(model), std::move(endpoint), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

AgentOutput parse_openai_responses_response(const Value& raw, std::string provider, std::string model) {
  AgentOutput response;
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
  Utf8StreamBuffer text_utf8("openai responses text delta");
  Utf8StreamBuffer reasoning_utf8("openai responses reasoning delta");
  std::map<std::string, StreamToolCallState> tool_call_state;
  std::vector<std::string> tool_call_order;
  Value::Array raw_events;
  std::optional<Value> completed_response;

  const auto emit_text_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    text += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = provider,
        .model = response_model,
        .delta = delta,
        .text = text,
    });
  };
  const auto emit_reasoning_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    reasoning += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = provider,
        .model = response_model,
        .delta = delta,
        .reasoning = reasoning,
    });
  };

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
    Value chunk = parse_sse_json_event(event, provider);
    raw_events.push_back(chunk);
    const auto type = chunk.at("type").as_string();
    if (type == "response.output_text.delta") {
      emit_text_delta(text_utf8.push(chunk.at("delta").as_string()));
      continue;
    }
    if (type == "response.reasoning_summary_text.delta" ||
        type == "response.reasoning_text.delta") {
      reasoning_format = type == "response.reasoning_text.delta" ? "provider-visible" : "summary";
      emit_reasoning_delta(reasoning_utf8.push(chunk.at("delta").as_string()));
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
  emit_text_delta(text_utf8.finish());
  emit_reasoning_delta(reasoning_utf8.finish());

  AgentOutput response;
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
  apply_protocol_reasoning(request,
                           "qwen",
                           request.body.at("model").as_string(model),
                           ProviderRequestProtocol::QwenChatCompletions,
                           params.settings.reasoning);
  return request;
}

NativeProviderRequest build_qwen_chat_stream_request(const GenerateParams& params, std::string model,
                                                     std::string api_key, std::string base_url) {
  auto request = build_qwen_chat_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  request.body["stream_options"] = Value::object({{"include_usage", true}});
  return request;
}

AgentOutput parse_qwen_chat_response(const Value& raw, std::string model) {
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
  std::map<std::string, std::string> headers{{"content-type", "application/json"}};
  if (!api_key.empty()) {
    headers["authorization"] = "Bearer " + api_key;
  }
  NativeProviderRequest request{"qwen", "/responses", body, headers, trim_provider_base_url(std::move(base_url))};
  apply_protocol_reasoning(request,
                           "qwen",
                           request.body.at("model").as_string(model),
                           ProviderRequestProtocol::QwenResponses,
                           params.settings.reasoning);
  return with_native_provider_cancellation(std::move(request), params.cancellation);
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

AgentOutput parse_qwen_responses_response(const Value& raw, std::string model) {
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

AgentOutput parse_mimo_chat_response(const Value& raw, std::string model) {
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
  std::map<std::string, std::string> headers{{"content-type", "application/json"},
                                             {"anthropic-version", "2023-06-01"}};
  if (!api_key.empty()) {
    headers["x-api-key"] = api_key;
  }
  NativeProviderRequest request{
      "anthropic", "/messages", body, headers, trim_provider_base_url(std::move(base_url))};
  apply_protocol_reasoning(request,
                           "anthropic",
                           request.body.at("model").as_string(model),
                           ProviderRequestProtocol::AnthropicMessages,
                           params.settings.reasoning);
  return with_native_provider_cancellation(std::move(request), params.cancellation);
}

NativeProviderRequest build_anthropic_messages_stream_request(const GenerateParams& params, std::string model,
                                                              std::string api_key, std::string base_url) {
  auto request = build_anthropic_messages_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

AgentOutput parse_anthropic_messages_response(const Value& raw, std::string provider, std::string model) {
  AgentOutput response;
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
  Utf8StreamBuffer text_utf8("anthropic text delta");
  Utf8StreamBuffer reasoning_utf8("anthropic reasoning delta");
  std::map<long long, StreamToolCallState> tool_call_state;
  Value::Array raw_events;
  Value::Object usage_acc;

  const auto merge_usage_fields = [&](const Value& src) {
    if (!src.is_object()) {
      return;
    }
    for (const auto& [key, value] : src.as_object()) {
      if (value.is_number() || value.is_object()) {
        usage_acc[key] = value;
      }
    }
  };
  const auto emit_text_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    text += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = provider,
        .model = response_model,
        .delta = delta,
        .text = text,
    });
  };
  const auto emit_reasoning_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    reasoning += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = provider,
        .model = response_model,
        .delta = delta,
        .reasoning = reasoning,
    });
  };

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk = parse_sse_json_event(event, provider);
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
      emit_reasoning_delta(reasoning_utf8.push(delta.at("thinking").as_string()));
      continue;
    }
    if (delta_type == "text_delta") {
      emit_text_delta(text_utf8.push(delta.at("text").as_string()));
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
  emit_text_delta(text_utf8.finish());
  emit_reasoning_delta(reasoning_utf8.finish());

  AgentOutput response;
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
  GenerateParams base_params = params;
  base_params.settings.reasoning = std::nullopt;
  auto request = build_anthropic_messages_request(base_params, std::move(model), {}, std::move(base_url));
  request.provider = "deepseek";
  request.headers.erase("anthropic-version");
  if (!api_key.empty()) {
    request.headers["x-api-key"] = api_key;
  }
  apply_protocol_reasoning(request,
                           "deepseek",
                           request.body.at("model").as_string(),
                           ProviderRequestProtocol::DeepSeekMessages,
                           params.settings.reasoning);
  return request;
}

NativeProviderRequest build_deepseek_messages_stream_request(const GenerateParams& params, std::string model,
                                                             std::string api_key, std::string base_url) {
  auto request = build_deepseek_messages_request(params, std::move(model), std::move(api_key), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

AgentOutput parse_deepseek_messages_response(const Value& raw, std::string model) {
  return parse_anthropic_messages_response(raw, "deepseek", std::move(model));
}

Value ollama_extra_settings(const ModelSettings& settings) {
  if (!settings.extra.is_object()) {
    return Value::object({});
  }
  const auto& extra = settings.extra.at("ollama");
  if (extra.is_null()) {
    return Value::object({});
  }
  if (!extra.is_object()) {
    throw ConfigurationError("ModelSettings.extra.ollama must be an object.");
  }
  return extra;
}

void apply_ollama_extra_option(Value& options,
                               const Value& extra,
                               const std::string& camel_key,
                               const std::string& snake_key,
                               const std::string& option_key) {
  const auto& camel = extra.at(camel_key);
  const auto& snake = extra.at(snake_key);
  const Value* selected = nullptr;
  std::string selected_key;
  if (!camel.is_null()) {
    selected = &camel;
    selected_key = camel_key;
  }
  if (!snake.is_null()) {
    selected = &snake;
    selected_key = snake_key;
  }
  if (!selected) {
    return;
  }
  if (!selected->is_number()) {
    throw ConfigurationError("ModelSettings.extra.ollama." + selected_key + " must be a number.");
  }
  options[option_key] = selected->as_integer();
}

void apply_ollama_provider_extra(Value& body, const ModelSettings& settings) {
  const auto extra = ollama_extra_settings(settings);
  if (!extra.is_object() || extra.as_object().empty()) {
    return;
  }

  auto& options = body["options"];
  if (!options.is_object()) {
    options = Value::object({});
  }
  const auto& raw_options = extra.at("options");
  if (!raw_options.is_null()) {
    if (!raw_options.is_object()) {
      throw ConfigurationError("ModelSettings.extra.ollama.options must be an object.");
    }
    for (const auto& [key, value] : raw_options.as_object()) {
      options[key] = value;
    }
  }

  apply_ollama_extra_option(options, extra, "numCtx", "num_ctx", "num_ctx");

  const auto& keep_alive_camel = extra.at("keepAlive");
  const auto& keep_alive_snake = extra.at("keep_alive");
  const Value* keep_alive = nullptr;
  std::string keep_alive_key;
  if (!keep_alive_camel.is_null()) {
    keep_alive = &keep_alive_camel;
    keep_alive_key = "keepAlive";
  }
  if (!keep_alive_snake.is_null()) {
    keep_alive = &keep_alive_snake;
    keep_alive_key = "keep_alive";
  }
  if (keep_alive) {
    if (!keep_alive->is_string() && !keep_alive->is_number()) {
      throw ConfigurationError("ModelSettings.extra.ollama." + keep_alive_key +
                               " must be a string or number.");
    }
    body["keep_alive"] = *keep_alive;
  }
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
                              {"options", options}});
  apply_ollama_provider_extra(body, params.settings);
  if (!tools.empty()) {
    body["tools"] = Value(tools);
  }
  base_url = trim_provider_base_url(std::move(base_url));
  if (base_url.size() < 4 || base_url.substr(base_url.size() - 4) != "/api") {
    base_url += "/api";
  }
  NativeProviderRequest request{
      "ollama", "/chat", body, {{"content-type", "application/json"}}, std::move(base_url)};
  apply_protocol_reasoning(request,
                           "ollama",
                           request.body.at("model").as_string(model),
                           ProviderRequestProtocol::OllamaChat,
                           params.settings.reasoning);
  return with_native_provider_cancellation(std::move(request), params.cancellation);
}

NativeProviderRequest build_ollama_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string base_url) {
  auto request = build_ollama_chat_request(params, std::move(model), std::move(base_url));
  request.body["stream"] = true;
  return request;
}

AgentOutput parse_ollama_chat_response(const Value& raw, std::string model) {
  AgentOutput response;
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
  Utf8StreamBuffer text_utf8("ollama text delta");
  Utf8StreamBuffer reasoning_utf8("ollama reasoning delta");
  std::vector<ToolCall> tool_calls;
  Value::Array raw_chunks;

  const auto emit_text_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    text += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = "ollama",
        .model = response_model,
        .delta = delta,
        .text = text,
    });
  };
  const auto emit_reasoning_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    reasoning += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = "ollama",
        .model = response_model,
        .delta = delta,
        .reasoning = reasoning,
    });
  };

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
    emit_reasoning_delta(reasoning_utf8.push(message.at("thinking").as_string()));
    emit_text_delta(text_utf8.push(message.at("content").as_string()));
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
  emit_text_delta(text_utf8.finish());
  emit_reasoning_delta(reasoning_utf8.finish());

  AgentOutput response;
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
  std::map<std::string, std::string> headers{{"content-type", "application/json"}};
  if (!api_key.empty()) {
    headers["x-goog-api-key"] = api_key;
  }
  NativeProviderRequest request{
      "gemini", "/models/" + model + ":generateContent", body, headers,
      trim_provider_base_url(std::move(base_url))};
  apply_protocol_reasoning(request,
                           "gemini",
                           model,
                           ProviderRequestProtocol::GeminiGenerateContent,
                           params.settings.reasoning);
  return with_native_provider_cancellation(std::move(request), params.cancellation);
}

NativeProviderRequest build_gemini_generate_content_stream_request(const GenerateParams& params, std::string model,
                                                                   std::string api_key, std::string base_url) {
  auto request = build_gemini_generate_content_request(params, model, std::move(api_key), std::move(base_url));
  request.endpoint = "/models/" + std::move(model) + ":streamGenerateContent?alt=sse";
  return request;
}

AgentOutput parse_gemini_generate_content_response(const Value& raw, std::string model) {
  AgentOutput response;
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
  Utf8StreamBuffer text_utf8("gemini text delta");
  Utf8StreamBuffer reasoning_utf8("gemini reasoning delta");
  std::vector<ToolCall> tool_calls;
  Value::Array raw_chunks;
  Value captured_usage_metadata;

  const auto emit_text_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    text += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = "gemini",
        .model = response_model,
        .delta = delta,
        .text = text,
    });
  };
  const auto emit_reasoning_delta = [&](std::string delta) {
    if (delta.empty()) {
      return;
    }
    reasoning += delta;
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = "gemini",
        .model = response_model,
        .delta = delta,
        .reasoning = reasoning,
    });
  };

  for (const auto& event : read_sse_events(chunks)) {
    if (event == "[DONE]") {
      break;
    }
    Value chunk = parse_sse_json_event(event, "gemini");
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
        emit_reasoning_delta(reasoning_utf8.push(text_delta));
      } else {
        emit_text_delta(text_utf8.push(text_delta));
      }
    }
  }
  emit_text_delta(text_utf8.finish());
  emit_reasoning_delta(reasoning_utf8.finish());

  AgentOutput response;
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
