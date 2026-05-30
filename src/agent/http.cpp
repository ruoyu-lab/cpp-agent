#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

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

std::string upper_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
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

bool header_exists(const std::map<std::string, std::string>& headers, const std::string& name) {
  const auto expected = lower_copy(name);
  for (const auto& [key, _] : headers) {
    if (lower_copy(key) == expected) {
      return true;
    }
  }
  return false;
}

std::string trim_copy(const std::string& value) {
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

struct ParsedHttpUrl {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target;
};

ParsedHttpUrl parse_http_url(const std::string& url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    throw AdapterError("Native HTTP transport requires an absolute HTTP URL.", url);
  }
  ParsedHttpUrl parsed;
  parsed.scheme = lower_copy(url.substr(0, scheme_end));
  if (parsed.scheme != "http") {
    if (parsed.scheme == "https") {
      throw AdapterError("Native HTTP transport does not provide TLS. Configure an HTTPS-capable transport.", url);
    }
    throw AdapterError("Unsupported URL scheme for native HTTP transport: " + parsed.scheme, url);
  }

  const auto authority_start = scheme_end + 3;
  const auto path_start = url.find('/', authority_start);
  const auto authority = url.substr(authority_start, path_start == std::string::npos
                                                       ? std::string::npos
                                                       : path_start - authority_start);
  parsed.target = path_start == std::string::npos ? "/" : url.substr(path_start);
  if (parsed.target.empty()) {
    parsed.target = "/";
  }
  if (authority.empty()) {
    throw AdapterError("Native HTTP transport URL requires a host.", url);
  }

  if (authority.front() == '[') {
    const auto end = authority.find(']');
    if (end == std::string::npos) {
      throw AdapterError("Invalid IPv6 host in URL.", url);
    }
    parsed.host = authority.substr(1, end - 1);
    if (end + 1 < authority.size() && authority[end + 1] == ':') {
      parsed.port = authority.substr(end + 2);
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
      parsed.host = authority.substr(0, colon);
      parsed.port = authority.substr(colon + 1);
    } else {
      parsed.host = authority;
    }
  }
  if (parsed.host.empty()) {
    throw AdapterError("Native HTTP transport URL requires a host.", url);
  }
  if (parsed.port.empty()) {
    parsed.port = "80";
  }
  return parsed;
}

std::pair<std::size_t, std::size_t> find_header_end(const std::string& buffer) {
  const auto crlf = buffer.find("\r\n\r\n");
  if (crlf != std::string::npos) {
    return {crlf, 4};
  }
  const auto lf = buffer.find("\n\n");
  if (lf != std::string::npos) {
    return {lf, 2};
  }
  return {std::string::npos, 0};
}

std::size_t content_length_from_headers(const std::map<std::string, std::string>& headers) {
  for (const auto& [key, value] : headers) {
    if (lower_copy(key) != "content-length") {
      continue;
    }
    try {
      return static_cast<std::size_t>(std::stoull(trim_copy(value)));
    } catch (const std::exception&) {
      throw AdapterError("Invalid Content-Length response header.", value);
    }
  }
  return 0;
}

bool has_chunked_transfer_encoding(const std::map<std::string, std::string>& headers) {
  for (const auto& [key, value] : headers) {
    if (lower_copy(key) == "transfer-encoding" && lower_copy(value).find("chunked") != std::string::npos) {
      return true;
    }
  }
  return false;
}

HttpResponse parse_http_response_head(const std::string& head) {
  std::istringstream stream(head);
  std::string status_line;
  if (!std::getline(stream, status_line)) {
    throw AdapterError("Malformed HTTP response.");
  }
  if (!status_line.empty() && status_line.back() == '\r') {
    status_line.pop_back();
  }
  std::istringstream status_stream(status_line);
  std::string version;
  HttpResponse response;
  status_stream >> version >> response.status;
  if (version.rfind("HTTP/", 0) != 0 || response.status <= 0) {
    throw AdapterError("Malformed HTTP response status line.", status_line);
  }

  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const auto key = lower_copy(trim_copy(line.substr(0, colon)));
    if (!key.empty()) {
      response.headers[key] = trim_copy(line.substr(colon + 1));
    }
  }
  return response;
}

bool chunked_body_complete(const std::string& body) {
  std::size_t offset = 0;
  while (true) {
    const auto line_end = body.find("\r\n", offset);
    if (line_end == std::string::npos) {
      return false;
    }
    const auto size_text = body.substr(offset, line_end - offset);
    const auto semicolon = size_text.find(';');
    const auto hex_text = semicolon == std::string::npos ? size_text : size_text.substr(0, semicolon);
    std::size_t chunk_size = 0;
    try {
      chunk_size = static_cast<std::size_t>(std::stoull(trim_copy(hex_text), nullptr, 16));
    } catch (const std::exception&) {
      throw AdapterError("Invalid chunked response size.", size_text);
    }
    offset = line_end + 2;
    if (body.size() < offset + chunk_size + 2) {
      return false;
    }
    offset += chunk_size;
    if (body.substr(offset, 2) != "\r\n") {
      throw AdapterError("Malformed chunked response body.");
    }
    offset += 2;
    if (chunk_size == 0) {
      const auto trailer_end = body.find("\r\n\r\n", offset);
      return trailer_end != std::string::npos || offset == body.size();
    }
  }
}

std::string decode_chunked_body(const std::string& body) {
  std::string decoded;
  std::size_t offset = 0;
  while (true) {
    const auto line_end = body.find("\r\n", offset);
    if (line_end == std::string::npos) {
      throw AdapterError("Incomplete chunked response body.");
    }
    const auto size_text = body.substr(offset, line_end - offset);
    const auto semicolon = size_text.find(';');
    const auto hex_text = semicolon == std::string::npos ? size_text : size_text.substr(0, semicolon);
    const auto chunk_size = static_cast<std::size_t>(std::stoull(trim_copy(hex_text), nullptr, 16));
    offset = line_end + 2;
    if (chunk_size == 0) {
      return decoded;
    }
    if (body.size() < offset + chunk_size + 2) {
      throw AdapterError("Incomplete chunked response body.");
    }
    decoded.append(body, offset, chunk_size);
    offset += chunk_size + 2;
  }
}

std::string serialize_http_request(const HttpRequest& request, const ParsedHttpUrl& url) {
  std::map<std::string, std::string> headers = request.headers;
  if (!header_exists(headers, "host")) {
    headers["host"] = url.port == "80" ? url.host : url.host + ":" + url.port;
  }
  if (!header_exists(headers, "connection")) {
    headers["connection"] = "close";
  }
  if (!request.body.empty() && !header_exists(headers, "content-length")) {
    headers["content-length"] = std::to_string(request.body.size());
  }

  std::ostringstream out;
  out << upper_copy(request.method.empty() ? "GET" : request.method) << ' ' << url.target << " HTTP/1.1\r\n";
  for (const auto& [key, value] : headers) {
    out << key << ": " << value << "\r\n";
  }
  out << "\r\n";
  out << request.body;
  return out.str();
}

#ifndef _WIN32
void close_fd(int fd) {
  if (fd >= 0) {
    (void)::close(fd);
  }
}

void set_socket_timeout(int fd, int timeout_ms) {
  if (timeout_ms <= 0) {
    return;
  }
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

int connect_native_http_socket(const ParsedHttpUrl& url, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const int lookup = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &results);
  if (lookup != 0) {
    throw AdapterError("DNS lookup failed for " + url.host + ": " + ::gai_strerror(lookup));
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(results, ::freeaddrinfo);

  std::string last_error;
  for (auto* item = results; item != nullptr; item = item->ai_next) {
    const int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    set_socket_timeout(fd, timeout_ms);
    if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
      return fd;
    }
    last_error = std::strerror(errno);
    close_fd(fd);
  }
  throw AdapterError("Native HTTP connection failed: " + last_error, url.host + ":" + url.port);
}

void send_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
    const auto result = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
    const auto result = ::send(fd, data.data() + sent, data.size() - sent, 0);
#endif
    if (result < 0) {
      throw AdapterError("Native HTTP send failed: " + std::string(std::strerror(errno)));
    }
    if (result == 0) {
      throw AdapterError("Native HTTP send failed: connection closed.");
    }
    sent += static_cast<std::size_t>(result);
  }
}

HttpResponse receive_http_response(int fd, const NativeHttpClientConfig& config) {
  std::string buffer;
  std::array<char, 8192> chunk{};
  while (true) {
    const auto received = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (received < 0) {
      throw AdapterError("Native HTTP receive failed: " + std::string(std::strerror(errno)));
    }
    if (received == 0) {
      break;
    }
    buffer.append(chunk.data(), static_cast<std::size_t>(received));
    if (buffer.size() > config.max_response_bytes) {
      throw AdapterError("Native HTTP response exceeded max_response_bytes.");
    }
    const auto [header_end, separator_size] = find_header_end(buffer);
    if (header_end == std::string::npos) {
      continue;
    }
    auto response = parse_http_response_head(buffer.substr(0, header_end));
    const auto body_start = header_end + separator_size;
    const auto body_size = buffer.size() - body_start;
    if (has_chunked_transfer_encoding(response.headers)) {
      const auto body = buffer.substr(body_start);
      if (chunked_body_complete(body)) {
        response.body = decode_chunked_body(body);
        return response;
      }
      continue;
    }
    const auto length = content_length_from_headers(response.headers);
    if (length > 0 && body_size >= length) {
      response.body = buffer.substr(body_start, length);
      return response;
    }
  }

  const auto [header_end, separator_size] = find_header_end(buffer);
  if (header_end == std::string::npos) {
    throw AdapterError("Native HTTP response did not include headers.");
  }
  auto response = parse_http_response_head(buffer.substr(0, header_end));
  response.body = buffer.substr(header_end + separator_size);
  if (has_chunked_transfer_encoding(response.headers)) {
    response.body = decode_chunked_body(response.body);
  }
  return response;
}

bool emit_decoded_chunked_body(std::string& body,
                               std::size_t& total_body_bytes,
                               const NativeHttpClientConfig& config,
                               const HttpStreamingChunkHandler& on_chunk) {
  while (true) {
    const auto line_end = body.find("\r\n");
    if (line_end == std::string::npos) {
      return false;
    }
    const auto size_text = body.substr(0, line_end);
    const auto semicolon = size_text.find(';');
    const auto hex_text = semicolon == std::string::npos ? size_text : size_text.substr(0, semicolon);
    std::size_t chunk_size = 0;
    try {
      chunk_size = static_cast<std::size_t>(std::stoull(trim_copy(hex_text), nullptr, 16));
    } catch (const std::exception&) {
      throw AdapterError("Invalid chunked response size.", size_text);
    }
    const std::size_t data_start = line_end + 2;
    if (body.size() < data_start + chunk_size + 2) {
      return false;
    }
    if (chunk_size == 0) {
      body.erase(0, data_start + 2);
      return true;
    }
    if (body.substr(data_start + chunk_size, 2) != "\r\n") {
      throw AdapterError("Malformed chunked response body.");
    }
    total_body_bytes += chunk_size;
    if (total_body_bytes > config.max_response_bytes) {
      throw AdapterError("Native HTTP response exceeded max_response_bytes.");
    }
    if (on_chunk) {
      on_chunk(std::string_view(body.data() + static_cast<std::ptrdiff_t>(data_start), chunk_size));
    }
    body.erase(0, data_start + chunk_size + 2);
  }
}

void receive_http_response_streaming(int fd,
                                     const NativeHttpClientConfig& config,
                                     const HttpStreamingChunkHandler& on_chunk,
                                     const HttpStreamingDoneHandler& on_done) {
  std::string buffer;
  std::string body_buffer;
  std::array<char, 8192> chunk{};
  HttpResponse response;
  bool saw_headers = false;
  bool chunked = false;
  std::size_t content_length = 0;
  std::size_t emitted_body_bytes = 0;

  const auto emit_plain_body = [&](std::string& data, bool final) -> bool {
    if (data.empty()) {
      return false;
    }
    std::size_t emit_size = data.size();
    if (content_length > 0) {
      const auto remaining = content_length > emitted_body_bytes
                                 ? content_length - emitted_body_bytes
                                 : 0;
      emit_size = std::min(emit_size, remaining);
    }
    if (emit_size > 0) {
      emitted_body_bytes += emit_size;
      if (emitted_body_bytes > config.max_response_bytes) {
        throw AdapterError("Native HTTP response exceeded max_response_bytes.");
      }
      if (on_chunk) {
        on_chunk(std::string_view(data.data(), emit_size));
      }
      data.erase(0, emit_size);
    }
    return (content_length > 0 && emitted_body_bytes >= content_length) ||
           (final && content_length == 0);
  };

  while (true) {
    const auto received = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (received < 0) {
      throw AdapterError("Native HTTP receive failed: " + std::string(std::strerror(errno)));
    }
    const bool eof = received == 0;
    if (!eof) {
      buffer.append(chunk.data(), static_cast<std::size_t>(received));
    }

    if (!saw_headers) {
      const auto [header_end, separator_size] = find_header_end(buffer);
      if (header_end == std::string::npos) {
        if (eof) {
          throw AdapterError("Native HTTP response did not include headers.");
        }
        continue;
      }
      response = parse_http_response_head(buffer.substr(0, header_end));
      saw_headers = true;
      chunked = has_chunked_transfer_encoding(response.headers);
      content_length = content_length_from_headers(response.headers);
      body_buffer = buffer.substr(header_end + separator_size);
      buffer.clear();
    } else if (!buffer.empty()) {
      body_buffer += buffer;
      buffer.clear();
    }

    if (chunked) {
      if (emit_decoded_chunked_body(body_buffer, emitted_body_bytes, config, on_chunk)) {
        if (on_done) {
          on_done(response.status, response.headers, std::nullopt);
        }
        return;
      }
    } else if (emit_plain_body(body_buffer, eof)) {
      if (on_done) {
        on_done(response.status, response.headers, std::nullopt);
      }
      return;
    }

    if (eof) {
      if (!saw_headers) {
        throw AdapterError("Native HTTP response did not include headers.");
      }
      if (on_done) {
        on_done(response.status, response.headers, std::nullopt);
      }
      return;
    }
  }
}
#endif

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

HttpTransport create_native_http_transport(NativeHttpClientConfig config) {
  return [config](const HttpRequest& request) -> HttpResponse {
#ifdef _WIN32
    (void)request;
    throw AdapterError("Native HTTP transport is not available on this platform.");
#else
    if (request.cancellation) {
      request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
    }
    const auto parsed = parse_http_url(request.url);
    const int fd = connect_native_http_socket(parsed, config.timeout_ms);
    try {
      if (request.cancellation) {
        request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
      }
      send_all(fd, serialize_http_request(request, parsed));
      auto response = receive_http_response(fd, config);
      if (request.cancellation) {
        request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
      }
      response.final_url = request.url;
      close_fd(fd);
      return response;
    } catch (...) {
      close_fd(fd);
      throw;
    }
#endif
  };
}

HttpStreamingTransport create_native_http_streaming_transport(NativeHttpClientConfig config) {
  return [config](const HttpRequest& request,
                  HttpStreamingChunkHandler on_chunk,
                  HttpStreamingDoneHandler on_done) {
#ifdef _WIN32
    (void)request;
    if (on_done) {
      on_done(0, {}, "Native HTTP streaming transport is not available on this platform.");
    }
#else
    try {
      if (request.cancellation) {
        request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
      }
      const auto parsed = parse_http_url(request.url);
      const int fd = connect_native_http_socket(parsed, config.timeout_ms);
      try {
        if (request.cancellation) {
          request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
        }
        send_all(fd, serialize_http_request(request, parsed));
        receive_http_response_streaming(fd, config, on_chunk, on_done);
        if (request.cancellation) {
          request.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
        }
        close_fd(fd);
      } catch (...) {
        close_fd(fd);
        throw;
      }
    } catch (const std::exception& error) {
      if (on_done) {
        on_done(0, {}, error.what());
      }
    }
#endif
  };
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

}  // namespace agent
