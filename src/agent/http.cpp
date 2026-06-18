#include "agent/core_api.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace agent {

namespace {

bool is_absolute_http_url(const std::string& value) {
  std::string lower;
  lower.reserve(value.size());
  for (const unsigned char ch : value) {
    lower.push_back(static_cast<char>(std::tolower(ch)));
  }
  return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool is_success_status(int status) {
  return status >= 200 && status < 300;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string strip_trailing_slashes(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string strip_leading_slashes(std::string value) {
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  return value;
}

void ensure_json_content_type(std::map<std::string, std::string>& headers) {
  for (const auto& [key, _] : headers) {
    if (lower_copy(key) == "content-type") {
      return;
    }
  }
  headers["content-type"] = "application/json";
}

void throw_http_status_error(int status, const Value& data) {
  throw AdapterError("Request failed with status " + std::to_string(status) + ".", safe_json_stringify(data));
}

}  // namespace

std::string build_url(const std::string& base_url, const std::string& path) {
  if (is_absolute_http_url(path)) {
    return path;
  }

  const auto normalized_base = strip_trailing_slashes(base_url);
  const auto normalized_path = strip_leading_slashes(path);
  if (normalized_path.empty()) {
    return normalized_base;
  }
  if (normalized_base.empty()) {
    return normalized_path;
  }
  return normalized_base + "/" + normalized_path;
}

HttpRequest build_http_request(const RequestJsonOptions& options) {
  HttpRequest request;
  request.url = build_url(options.base_url, options.path);
  request.method = options.method.empty() ? "POST" : options.method;
  request.headers = options.headers;
  request.cancellation = options.cancellation;
  ensure_json_content_type(request.headers);
  if (!options.body.is_null()) {
    request.body = options.body.stringify(0);
  }
  return request;
}

Value parse_http_response_body(const std::string& text) {
  if (text.empty()) {
    return Value();
  }
  try {
    return parse_json(text);
  } catch (const std::exception&) {
    return Value(text);
  }
}

Value request_json(const RequestJsonOptions& options, const HttpTransport& transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  const auto response = transport(build_http_request(options));
  const auto data = parse_http_response_body(response.body);
  if (!is_success_status(response.status)) {
    throw_http_status_error(response.status, data);
  }
  return data;
}

HttpResponse request_stream(const RequestStreamOptions& options, const HttpTransport& transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  auto response = transport(build_http_request(options));
  if (!is_success_status(response.status)) {
    throw_http_status_error(response.status, parse_http_response_body(response.body));
  }
  return response;
}

std::vector<std::string> read_text_stream(const std::vector<std::string>& chunks) {
  return chunks;
}

std::vector<std::string> read_lines(const std::vector<std::string>& chunks) {
  std::vector<std::string> lines;
  std::string buffer;

  for (const auto& chunk : chunks) {
    buffer += chunk;
    while (true) {
      const auto newline_index = buffer.find('\n');
      if (newline_index == std::string::npos) {
        break;
      }
      auto line = buffer.substr(0, newline_index);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(std::move(line));
      buffer.erase(0, newline_index + 1);
    }
  }

  if (!buffer.empty()) {
    if (buffer.back() == '\r') {
      buffer.pop_back();
    }
    if (!buffer.empty()) {
      lines.push_back(std::move(buffer));
    }
  }

  return lines;
}

std::vector<std::string> read_lines(const std::string& text) {
  return read_lines(std::vector<std::string>{text});
}

std::vector<std::string> read_sse_events(const std::vector<std::string>& chunks) {
  std::vector<std::string> events;
  std::vector<std::string> data_lines;

  for (const auto& line : read_lines(chunks)) {
    if (line.empty()) {
      if (!data_lines.empty()) {
        std::ostringstream event;
        for (std::size_t index = 0; index < data_lines.size(); ++index) {
          if (index > 0) {
            event << '\n';
          }
          event << data_lines[index];
        }
        events.push_back(event.str());
        data_lines.clear();
      }
      continue;
    }

    if (!line.empty() && line.front() == ':') {
      continue;
    }

    const std::string prefix = "data:";
    if (line.rfind(prefix, 0) == 0) {
      auto data = line.substr(prefix.size());
      while (!data.empty() && data.front() == ' ') {
        data.erase(data.begin());
      }
      data_lines.push_back(std::move(data));
    }
  }

  if (!data_lines.empty()) {
    std::ostringstream event;
    for (std::size_t index = 0; index < data_lines.size(); ++index) {
      if (index > 0) {
        event << '\n';
      }
      event << data_lines[index];
    }
    events.push_back(event.str());
  }

  return events;
}

std::vector<std::string> read_sse_events(const std::string& text) {
  return read_sse_events(std::vector<std::string>{text});
}

Value parse_sse_json_event(const std::string& event, const std::string& provider) {
  try {
    return parse_json(event);
  } catch (const std::exception&) {
    throw AdapterError("Malformed " + provider + " SSE JSON event.", event);
  }
}

}  // namespace agent
