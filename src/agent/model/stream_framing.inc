namespace {

struct CompleteJsonFrame {
  std::string text;
  std::size_t consumed = 0;
};

bool is_json_whitespace(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::optional<CompleteJsonFrame> next_complete_json_frame(const std::string& buffer) {
  std::size_t start = 0;
  while (start < buffer.size() && is_json_whitespace(buffer[start])) {
    ++start;
  }
  if (start >= buffer.size()) {
    return std::nullopt;
  }

  const char first = buffer[start];
  if (first != '{' && first != '[') {
    const auto newline = buffer.find('\n', start);
    if (newline == std::string::npos) {
      return std::nullopt;
    }
    return CompleteJsonFrame{buffer.substr(start, newline - start), newline + 1};
  }

  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = start; index < buffer.size(); ++index) {
    const char ch = buffer[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      --depth;
      if (depth == 0) {
        const std::size_t end = index + 1;
        std::size_t consumed = end;
        while (consumed < buffer.size() && is_json_whitespace(buffer[consumed])) {
          ++consumed;
        }
        return CompleteJsonFrame{buffer.substr(start, end - start), consumed};
      }
      if (depth < 0) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

}  // namespace
