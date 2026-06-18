namespace {

std::optional<std::size_t> first_any_section_position_after(const std::string& text,
                                                            std::size_t after) {
  for (std::size_t index = after; index < text.size(); ++index) {
    if (is_section_label_at(text, index)) {
      return index;
    }
  }
  return std::nullopt;
}

std::size_t section_value_start(const std::string& text,
                                std::size_t label_start,
                                const std::string& label) {
  std::size_t start = label_start + label.size() + 1;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  return start;
}

std::size_t partial_react_label_holdback(const std::string& text) {
  static const std::vector<std::string> prefixes = {
      "Thought:",
      "Message:",
      "Assistant:",
      "Progress:",
      "Actions:",
      "Observation:",
      "Final Answer:",
      "\nThought:",
      "\nMessage:",
      "\nAssistant:",
      "\nProgress:",
      "\nActions:",
      "\nObservation:",
      "\nFinal Answer:",
      "\rThought:",
      "\rMessage:",
      "\rAssistant:",
      "\rProgress:",
      "\rActions:",
      "\rObservation:",
      "\rFinal Answer:",
  };
  std::size_t holdback = 0;
  for (const auto& prefix : prefixes) {
    const std::size_t limit = std::min(text.size(), prefix.size() - 1);
    for (std::size_t size = 1; size <= limit; ++size) {
      if (text.compare(text.size() - size, size, prefix, 0, size) == 0) {
        holdback = std::max(holdback, size);
      }
    }
  }
  return holdback;
}

struct ReActVisibleDelta {
  std::string delta;
  std::string text;
};

std::optional<std::pair<std::size_t, std::string>> first_visible_section_after(const std::string& text,
                                                                                std::size_t after) {
  static constexpr std::string_view labels[] = {"Message", "Assistant", "Progress"};
  for (std::size_t index = after; index < text.size(); ++index) {
    for (const auto& label : labels) {
      if (starts_with_label_at(text, index, std::string(label))) {
        return std::make_pair(index, std::string(label));
      }
    }
  }
  return std::nullopt;
}

class IncrementalReActStreamParser {
 public:
  explicit IncrementalReActStreamParser(ReActParserOptions options = {})
      : options_(options) {}

  std::optional<ReActVisibleDelta> feed(std::string_view delta) {
    text_.append(delta.data(), delta.size());
    return visible_delta(false);
  }

  std::optional<ReActVisibleDelta> finish() {
    return visible_delta(true);
  }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }

 private:
  std::optional<ReActVisibleDelta> visible_delta(bool final) {
    std::string visible;
    std::size_t cursor = 0;
    bool found_visible = false;
    while (auto section = first_visible_section_after(text_, cursor)) {
      found_visible = true;
      const auto content_start = section_value_start(text_, section->first, section->second);
      if (content_start > text_.size()) {
        break;
      }
      const auto next_label = first_any_section_position_after(text_, content_start);
      const auto content_end = next_label.value_or(text_.size());
      if (content_end > content_start) {
        std::string chunk = text_.substr(content_start, content_end - content_start);
        if (!final && !next_label) {
          const auto holdback = partial_react_label_holdback(chunk);
          const auto available = holdback >= chunk.size() ? 0 : chunk.size() - holdback;
          chunk = chunk.substr(0, available);
        }
        if (next_label || final) {
          chunk = trim_copy(std::move(chunk));
        }
        visible += chunk;
      }
      if (!next_label) {
        break;
      }
      cursor = *next_label;
    }
    if (!found_visible || visible.size() <= emitted_visible_bytes_) {
      return std::nullopt;
    }
    if (visible.size() <= emitted_visible_bytes_) {
      return std::nullopt;
    }
    auto delta = visible.substr(emitted_visible_bytes_);
    emitted_visible_bytes_ = visible.size();
    return ReActVisibleDelta{std::move(delta), std::move(visible)};
  }

  ReActParserOptions options_;
  std::string text_;
  std::size_t emitted_visible_bytes_ = 0;
};

void emit_loop_stream_event(const AgentLoopStreamEventHandler& on_event,
                            AgentLoopStreamEvent event) {
  if (on_event) {
    on_event(event);
  }
}

}  // namespace
