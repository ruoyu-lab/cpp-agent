class IncrementalParsedStreamEmitter {
 public:
  using Parser = std::function<std::vector<ModelStreamEvent>(const std::vector<std::string>&)>;

  enum class FrameBoundary {
    None,
    SseEvent,
    JsonLine,
  };

  IncrementalParsedStreamEmitter(Parser parser, ModelSettings settings,
                                 FrameBoundary frame_boundary = FrameBoundary::None)
      : parser_(std::move(parser)),
        settings_(std::move(settings)),
        frame_boundary_(frame_boundary),
        reasoning_parser_(settings_.reasoning && !settings_.reasoning->tag_name.empty()
                              ? settings_.reasoning->tag_name
                              : std::string("think")) {}

  void feed(std::string_view chunk, const ChatModelAdapter::StreamEventHandler& on_event) {
    if (frame_boundary_ == FrameBoundary::JsonLine) {
      feed_json_line(chunk, on_event);
      return;
    }
    chunks_.emplace_back(chunk.data(), chunk.size());
    const auto complete_chunks = complete_framed_chunks();
    if (complete_chunks.empty()) {
      return;
    }
    const auto parsed = parser_(complete_chunks);
    emit_new_events(parsed, on_event);
  }

  void finish(const ChatModelAdapter::StreamEventHandler& on_event) {
    if (frame_boundary_ == FrameBoundary::JsonLine) {
      feed_json_line({}, on_event);
      emit_json_line_response(on_event);
      return;
    }
    const auto parsed = parser_(chunks_);
    emit_new_events(parsed, on_event);
    const auto response_it = std::find_if(parsed.begin(), parsed.end(), [](const ModelStreamEvent& event) {
      return event.type == ModelStreamEventType::Response;
    });
    if (response_it == parsed.end()) {
      return;
    }
    emit_response_event(*response_it, on_event);
  }

 private:
  void emit_sequenced_model_event(ModelStreamEvent event,
                                  const ChatModelAdapter::StreamEventHandler& on_event) {
    if (!on_event) {
      return;
    }
    event.schema_version = kAgentStreamEventSchemaVersion;
    if (event.sequence == 0) {
      event.sequence = ++event_sequence_;
    } else {
      event_sequence_ = event.sequence;
    }
    on_event(event);
  }

  void emit_response_event(ModelStreamEvent response_event,
                           const ChatModelAdapter::StreamEventHandler& on_event) {
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
    emit_sequenced_model_event(std::move(response_event), on_event);
  }

  static std::string join_chunks(const std::vector<std::string>& chunks) {
    std::size_t size = 0;
    for (const auto& chunk : chunks) {
      size += chunk.size();
    }
    std::string joined;
    joined.reserve(size);
    for (const auto& chunk : chunks) {
      joined += chunk;
    }
    return joined;
  }

  static std::size_t last_sse_event_end(const std::string& text) {
    std::size_t end = 0;
    for (std::size_t offset = 0; offset < text.size();) {
      const auto lf = text.find("\n\n", offset);
      const auto crlf = text.find("\r\n\r\n", offset);
      if (lf == std::string::npos && crlf == std::string::npos) {
        break;
      }
      if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
        end = crlf + 4;
        offset = end;
      } else {
        end = lf + 2;
        offset = end;
      }
    }
    return end;
  }

  std::vector<std::string> complete_framed_chunks() const {
    if (frame_boundary_ == FrameBoundary::None) {
      return chunks_;
    }
    const auto text = join_chunks(chunks_);
    std::size_t end = 0;
    if (frame_boundary_ == FrameBoundary::SseEvent) {
      end = last_sse_event_end(text);
    } else if (frame_boundary_ == FrameBoundary::JsonLine) {
      const auto newline = text.find_last_of('\n');
      end = newline == std::string::npos ? 0 : newline + 1;
    }
    if (end == 0) {
      return {};
    }
    return {text.substr(0, end)};
  }

  bool reasoning_enabled() const {
    return settings_.reasoning && settings_.reasoning->enabled == true;
  }

  void emit_tagged_deltas(const std::vector<TaggedReasoningDelta>& deltas,
                          const std::string& provider,
                          const std::string& model,
                          const ChatModelAdapter::StreamEventHandler& on_event) {
    for (const auto& item : deltas) {
      if (item.type == TaggedReasoningDeltaType::Reasoning) {
        emit_sequenced_model_event(ModelStreamEvent{
            .type = ModelStreamEventType::ReasoningDelta,
            .provider = provider,
            .model = model,
            .delta = item.delta,
            .reasoning = item.reasoning_text,
        }, on_event);
      } else {
        emit_sequenced_model_event(ModelStreamEvent{
            .type = ModelStreamEventType::TextDelta,
            .provider = provider,
            .model = model,
            .delta = item.delta,
            .text = item.text,
        }, on_event);
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
    emit_sequenced_model_event(event, on_event);
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

  void emit_json_line_event(ModelStreamEvent event,
                            const ChatModelAdapter::StreamEventHandler& on_event) {
    if (event.type == ModelStreamEventType::ResponseStart) {
      if (json_line_started_) {
        return;
      }
      json_line_started_ = true;
      emit_model_event(event, on_event);
      return;
    }

    if (event.type == ModelStreamEventType::TextDelta) {
      if (!event.delta.empty()) {
        json_line_text_ += event.delta;
        event.text = json_line_text_;
      } else if (!event.text.empty() && event.text.size() > json_line_text_.size()) {
        event.delta = event.text.substr(json_line_text_.size());
        json_line_text_ = event.text;
      }
    } else if (event.type == ModelStreamEventType::ReasoningDelta) {
      if (!event.delta.empty()) {
        json_line_reasoning_ += event.delta;
        event.reasoning = json_line_reasoning_;
      } else if (!event.reasoning.empty() && event.reasoning.size() > json_line_reasoning_.size()) {
        event.delta = event.reasoning.substr(json_line_reasoning_.size());
        json_line_reasoning_ = event.reasoning;
      }
    } else if (event.type == ModelStreamEventType::ToolCallDelta) {
      Value arguments = Value::object({});
      if (!event.tool_call_args_accumulated.empty()) {
        try {
          arguments = parse_json(event.tool_call_args_accumulated);
        } catch (...) {
          arguments = Value::object({});
        }
      }
      upsert_json_line_tool_call(ToolCall{
          event.tool_call_id.empty()
              ? event.tool_call_name + "_" + std::to_string(json_line_tool_calls_.size() + 1)
              : event.tool_call_id,
          event.tool_call_name,
          arguments.is_object() ? arguments : Value::object({}),
      });
    }

    emit_model_event(event, on_event);
  }

  void upsert_json_line_tool_call(ToolCall call) {
    if (call.id.empty()) {
      call.id = call.name.empty() ? "tool_" + std::to_string(json_line_tool_calls_.size() + 1)
                                  : call.name + "_" + std::to_string(json_line_tool_calls_.size() + 1);
    }
    for (auto& existing : json_line_tool_calls_) {
      if (existing.id == call.id) {
        existing = std::move(call);
        return;
      }
    }
    json_line_tool_calls_.push_back(std::move(call));
  }

  void remember_json_line_response(const AgentOutput& response) {
    if (!response.raw.is_object()) {
      return;
    }
    const auto& chunks = response.raw.at("chunks");
    if (!chunks.is_array()) {
      return;
    }
    for (const auto& chunk : chunks.as_array()) {
      json_line_raw_chunks_.push_back(chunk);
    }
  }

  void emit_json_line_events(const std::vector<ModelStreamEvent>& parsed,
                             const ChatModelAdapter::StreamEventHandler& on_event) {
    for (const auto& event : parsed) {
      if (event.type == ModelStreamEventType::Response) {
        remember_json_line_response(event.response);
        for (const auto& tool_call : event.response.tool_calls) {
          upsert_json_line_tool_call(tool_call);
        }
        pending_json_line_response_ = event;
        continue;
      }
      emit_json_line_event(event, on_event);
    }
  }

  void feed_json_line(std::string_view chunk,
                      const ChatModelAdapter::StreamEventHandler& on_event) {
    json_line_buffer_.append(chunk.data(), chunk.size());
    while (auto frame = next_complete_json_frame(json_line_buffer_)) {
      if (!frame->text.empty()) {
        emit_json_line_events(parser_(std::vector<std::string>{frame->text}), on_event);
      }
      json_line_buffer_.erase(0, frame->consumed);
    }
  }

  void emit_json_line_response(const ChatModelAdapter::StreamEventHandler& on_event) {
    if (!pending_json_line_response_) {
      return;
    }
    auto response_event = std::move(*pending_json_line_response_);
    pending_json_line_response_.reset();
    if (!json_line_text_.empty()) {
      response_event.response.text = json_line_text_;
      response_event.response.content = json_line_text_.empty()
                                            ? std::vector<MessageContentPart>{}
                                            : std::vector<MessageContentPart>{text_part(json_line_text_)};
    }
    if (!response_event.response.reasoning && native_reasoning_) {
      response_event.response.reasoning = native_reasoning_;
    }
    if (response_event.response.tool_calls.empty() && !json_line_tool_calls_.empty()) {
      response_event.response.tool_calls = json_line_tool_calls_;
    }
    if (!response_event.response.tool_calls.empty()) {
      response_event.response.finish_reason = "tool_calls";
    }
    if (!json_line_raw_chunks_.empty()) {
      response_event.response.raw = Value::object({
          {"id", response_event.response.id},
          {"model", response_event.response.model},
          {"chunks", Value(json_line_raw_chunks_)},
      });
    }
    emit_response_event(std::move(response_event), on_event);
  }

  Parser parser_;
  ModelSettings settings_;
  FrameBoundary frame_boundary_;
  TaggedReasoningStreamParser reasoning_parser_;
  std::vector<std::string> chunks_;
  std::string json_line_buffer_;
  std::optional<ModelStreamEvent> pending_json_line_response_;
  std::size_t raw_cursor_ = 0;
  std::string current_provider_;
  std::string current_model_;
  std::string json_line_text_;
  std::string json_line_reasoning_;
  std::vector<ToolCall> json_line_tool_calls_;
  Value::Array json_line_raw_chunks_;
  std::optional<ModelReasoning> native_reasoning_;
  std::uint64_t event_sequence_ = 0;
  bool saw_text_delta_ = false;
  bool json_line_started_ = false;
};
