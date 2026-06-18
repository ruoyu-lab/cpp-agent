#pragma once

#include "agent/core.hpp"

namespace agent {

class CancellationToken;

struct HttpRequest {
  std::string url;
  std::string method = "POST";
  std::map<std::string, std::string> headers;
  std::string body;
  CancellationToken* cancellation = nullptr;
};

struct HttpResponse {
  int status = 200;
  std::map<std::string, std::string> headers;
  std::string body;
  std::string final_url;
  Value metadata = Value::object({});
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;
using HttpStreamingChunkHandler = std::function<void(std::string_view chunk)>;
using HttpStreamingDoneHandler =
    std::function<void(int status,
                       const std::map<std::string, std::string>& headers,
                       std::optional<std::string> error)>;
using HttpStreamingTransport = std::function<void(const HttpRequest&,
                                                  HttpStreamingChunkHandler on_chunk,
                                                  HttpStreamingDoneHandler on_done)>;

struct RequestJsonOptions {
  std::string base_url;
  std::string path;
  std::string method = "POST";
  std::map<std::string, std::string> headers;
  Value body;
  CancellationToken* cancellation = nullptr;
};

using RequestStreamOptions = RequestJsonOptions;

std::string build_url(const std::string& base_url, const std::string& path);
HttpRequest build_http_request(const RequestJsonOptions& options);
Value parse_http_response_body(const std::string& text);
Value request_json(const RequestJsonOptions& options, const HttpTransport& transport);
HttpResponse request_stream(const RequestStreamOptions& options, const HttpTransport& transport);
std::vector<std::string> read_text_stream(const std::vector<std::string>& chunks);
std::vector<std::string> read_lines(const std::vector<std::string>& chunks);
std::vector<std::string> read_lines(const std::string& text);
std::vector<std::string> read_sse_events(const std::vector<std::string>& chunks);
std::vector<std::string> read_sse_events(const std::string& text);
Value parse_sse_json_event(const std::string& event, const std::string& provider);

}  // namespace agent
