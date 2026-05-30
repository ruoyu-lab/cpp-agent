#include "agent/server.hpp"

#include <sstream>
#include <utility>

namespace agent {

namespace {

std::string trim_slashes(std::string value) {
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> parts;
  std::stringstream stream(trim_slashes(path));
  std::string item;
  while (std::getline(stream, item, '/')) {
    if (!item.empty()) {
      parts.push_back(item);
    }
  }
  return parts;
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

}  // namespace

HttpResponse send_json(int status_code, Value payload, std::map<std::string, std::string> headers) {
  headers["content-type"] = "application/json; charset=utf-8";
  return HttpResponse{.status = status_code, .headers = std::move(headers), .body = payload.stringify(2)};
}

HttpResponse send_sse(const std::vector<std::pair<std::string, Value>>& events,
                      std::map<std::string, std::string> headers) {
  headers["content-type"] = "text/event-stream; charset=utf-8";
  headers["cache-control"] = "no-cache, no-transform";
  headers["connection"] = "keep-alive";
  std::ostringstream body;
  for (const auto& [event, payload] : events) {
    body << "event: " << event << "\n";
    body << "data: " << payload.stringify(0) << "\n\n";
  }
  return HttpResponse{.status = 200, .headers = std::move(headers), .body = body.str()};
}

RouteMatch match_route_pattern(const std::string& pattern, const std::string& path) {
  const auto pattern_parts = split_path(pattern);
  const auto path_parts = split_path(path);
  if (pattern_parts.size() != path_parts.size()) {
    return {};
  }
  RouteMatch match{.matched = true};
  for (std::size_t index = 0; index < pattern_parts.size(); ++index) {
    const auto& expected = pattern_parts[index];
    const auto& actual = path_parts[index];
    if (!expected.empty() && expected.front() == ':') {
      match.params[expected.substr(1)] = url_decode(actual);
      continue;
    }
    if (expected != actual) {
      return {};
    }
  }
  return match;
}

}  // namespace agent
