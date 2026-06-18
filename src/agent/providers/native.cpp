#include "agent/providers/native.hpp"

#include "agent/execution.hpp"

namespace agent {

void throw_if_native_provider_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Model);
  }
}

NativeProviderRequest with_native_provider_cancellation(NativeProviderRequest request,
                                                        CancellationToken* cancellation) {
  request.cancellation = cancellation;
  return request;
}

NativeProviderTransport create_native_provider_http_transport(HttpTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request) -> Value {
    throw_if_native_provider_cancelled(request.cancellation);
    auto response = request_json(
        RequestJsonOptions{
            .base_url = request.base_url,
            .path = request.endpoint,
            .method = "POST",
            .headers = request.headers,
            .body = request.body,
            .cancellation = request.cancellation,
        },
        transport);
    throw_if_native_provider_cancelled(request.cancellation);
    return response;
  };
}

NativeProviderStreamTransport create_native_provider_http_stream_transport(HttpTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request) -> std::vector<std::string> {
    throw_if_native_provider_cancelled(request.cancellation);
    auto response = request_stream(
        RequestStreamOptions{
            .base_url = request.base_url,
            .path = request.endpoint,
            .method = "POST",
            .headers = request.headers,
            .body = request.body,
            .cancellation = request.cancellation,
        },
        transport);
    throw_if_native_provider_cancelled(request.cancellation);
    return response.body.empty() ? std::vector<std::string>{}
                                 : std::vector<std::string>{std::move(response.body)};
  };
}

NativeProviderStreamingTransport create_native_provider_http_streaming_transport(HttpStreamingTransport transport) {
  if (!transport) {
    throw AdapterError("HTTP streaming transport is not configured.");
  }
  return [transport = std::move(transport)](const NativeProviderRequest& request,
                                            NativeProviderStreamingChunkHandler on_chunk) {
    throw_if_native_provider_cancelled(request.cancellation);
    std::optional<std::string> error;
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    transport(build_http_request(RequestStreamOptions{
                  .base_url = request.base_url,
                  .path = request.endpoint,
                  .method = "POST",
                  .headers = request.headers,
                  .body = request.body,
                  .cancellation = request.cancellation,
              }),
              [&](std::string_view chunk) {
                throw_if_native_provider_cancelled(request.cancellation);
                body.append(chunk.data(), chunk.size());
                if (on_chunk) {
                  on_chunk(chunk);
                }
              },
              [&](int done_status,
                  const std::map<std::string, std::string>& done_headers,
                  std::optional<std::string> done_error) {
                status = done_status;
                headers = done_headers;
                error = std::move(done_error);
              });
    (void)headers;
    throw_if_native_provider_cancelled(request.cancellation);
    if (error) {
      throw AdapterError(*error);
    }
    if (status != 0 && (status < 200 || status >= 300)) {
      throw AdapterError("Request failed with status " + std::to_string(status) + ".",
                         safe_json_stringify(parse_http_response_body(body)));
    }
  };
}

}  // namespace agent
