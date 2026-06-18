
namespace {

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string upper_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

std::string url_decode(const std::string& value) {
  std::string output;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%' && index + 2 < value.size()) {
      const int high = hex_value(value[index + 1]);
      const int low = hex_value(value[index + 2]);
      if (high >= 0 && low >= 0) {
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    output.push_back(value[index] == '+' ? ' ' : value[index]);
  }
  return output;
}

std::string header_value(const HttpRequest& request, const std::string& name) {
  const auto normalized = lower_copy(name);
  for (const auto& [key, value] : request.headers) {
    if (lower_copy(key) == normalized) {
      return value;
    }
  }
  return {};
}

std::string bearer_token(const HttpRequest& request, const std::string& header_name) {
  const auto value = header_value(request, header_name.empty() ? "authorization" : header_name);
  const std::string prefix = "bearer ";
  const auto lower = lower_copy(value);
  if (lower.rfind(prefix, 0) != 0) {
    return {};
  }
  return value.substr(prefix.size());
}

bool contains_string(const std::vector<std::string>& values, const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

// Compare two secrets in time independent of where the first mismatching byte
// is. Every byte (and the length delta) is folded into a single accumulator so
// there is no early-out for an attacker to time, unlike `==` / std::find.
bool constant_time_equals(const std::string& a, const std::string& b) {
  unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
  const std::size_t n = a.size() > b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
    const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
    diff = static_cast<unsigned char>(diff | (ca ^ cb));
  }
  return diff == 0;
}

// Membership test for a secret against a configured list, using a constant-time
// per-entry compare and scanning the whole list (no short-circuit) so the match
// position isn't timing-observable.
bool contains_secret(const std::vector<std::string>& values, const std::string& candidate) {
  bool found = false;
  for (const auto& value : values) {
    if (constant_time_equals(value, candidate)) {
      found = true;
    }
  }
  return found;
}

bool path_exempt(const std::string& path, const std::vector<std::string>& exempt_paths) {
  return contains_string(exempt_paths, path);
}

std::string path_from_url(const std::string& url) {
  std::string value = url.empty() ? "/" : url;
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    const auto path_start = value.find('/', scheme + 3);
    value = path_start == std::string::npos ? "/" : value.substr(path_start);
  }
  const auto query = value.find('?');
  if (query != std::string::npos) {
    value = value.substr(0, query);
  }
  const auto hash = value.find('#');
  if (hash != std::string::npos) {
    value = value.substr(0, hash);
  }
  return value.empty() ? "/" : value;
}

std::map<std::string, std::string> query_from_url(const std::string& url) {
  std::map<std::string, std::string> query;
  const auto question = url.find('?');
  if (question == std::string::npos) {
    return query;
  }
  const auto hash = url.find('#', question + 1);
  const auto raw = url.substr(question + 1, hash == std::string::npos ? std::string::npos : hash - question - 1);
  std::stringstream stream(raw);
  std::string item;
  while (std::getline(stream, item, '&')) {
    if (item.empty()) {
      continue;
    }
    const auto equals = item.find('=');
    const auto key = url_decode(equals == std::string::npos ? item : item.substr(0, equals));
    const auto value = equals == std::string::npos ? std::string{} : url_decode(item.substr(equals + 1));
    query[key] = value;
  }
  return query;
}

std::string join_values(const std::vector<std::string>& values, const std::string& separator) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      out << separator;
    }
    out << values[index];
  }
  return out.str();
}

std::string trim_copy(const std::string& value) {
  auto begin = value.begin();
  while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
    ++begin;
  }
  auto end = value.end();
  while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }
  return std::string(begin, end);
}

bool valid_trace_header_char(char ch) {
  const auto value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_' || ch == '.' || ch == ':' || ch == '-';
}

std::string normalize_trace_header(const std::string& value) {
  const auto trimmed = trim_copy(value);
  if (trimmed.empty() || trimmed.size() > 128) {
    return {};
  }
  if (!std::all_of(trimmed.begin(), trimmed.end(), valid_trace_header_char)) {
    return {};
  }
  return trimmed;
}

AgentServerRequestTrace ensure_request_trace(const HttpRequest& request,
                                             const AgentServerTracingConfig& config) {
  const auto request_id =
      normalize_trace_header(header_value(request, config.request_id_header.empty()
                                                       ? "x-request-id"
                                                       : config.request_id_header));
  const auto trace_id =
      normalize_trace_header(header_value(request, config.trace_id_header.empty()
                                                       ? "x-trace-id"
                                                       : config.trace_id_header));
  AgentServerRequestTrace trace;
  trace.request_id = request_id.empty() ? generate_uuid() : request_id;
  trace.trace_context = create_trace_context(TraceContext{
      .trace_id = trace_id.empty() ? trace.request_id : trace_id,
      .span_name = "http.request",
  });
  return trace;
}

void apply_trace_headers(HttpResponse& response, const AgentServerRequestTrace& trace,
                         const AgentServerTracingConfig& config) {
  if (!config.response_headers) {
    return;
  }
  response.headers["x-request-id"] = trace.request_id;
  response.headers["x-trace-id"] = trace.trace_context.trace_id;
}

void add_optional_audit_field(Value& value, const std::string& key, const std::string& field) {
  if (!field.empty()) {
    value[key] = field;
  }
}

void merge_headers(const HttpResponse& source, HttpResponse& target) {
  for (const auto& [key, value] : source.headers) {
    target.headers[key] = value;
  }
}
