#include "agent/providers/openai_compatible.hpp"

#include "../detail/helpers.hpp"

namespace agent {

namespace {

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

ModelReasoning reasoning_from_text(std::string text) {
  auto reasoning = build_model_reasoning(std::move(text), "provider-visible");
  return reasoning ? *reasoning : ModelReasoning{};
}

struct StreamToolCallState {
  std::string id;
  std::string name;
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
  return with_native_provider_cancellation(NativeProviderRequest{
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

AgentOutput parse_openai_chat_response(const Value& raw, std::string provider, std::string model) {
  AgentOutput response;
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
  Utf8StreamBuffer text_utf8(std::string(provider) + " chat text delta");
  Utf8StreamBuffer reasoning_utf8(std::string(provider) + " chat reasoning delta");
  std::map<long long, StreamToolCallState> tool_call_state;
  Value::Array raw_chunks;
  Value captured_usage;

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
    emit_reasoning_delta(reasoning_utf8.push(delta.at("reasoning_content").as_string()));
    emit_text_delta(text_utf8.push(delta.at("content").as_string()));
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

}  // namespace agent
