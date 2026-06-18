#include "agent/web_native.hpp"

#include <utility>

namespace agent {

NativeWebSearchTransport create_native_web_search_transport(NativeHttpClientConfig config) {
  return create_native_web_search_transport(create_native_http_transport(std::move(config)));
}

NativeWebFetchTransport create_native_web_fetch_transport(NativeHttpClientConfig config) {
  return [config](const NativeWebFetchTransportRequest& request) -> WebFetchedPage {
    auto request_config = config;
    if (request.request.timeout_ms > 0) {
      request_config.timeout_ms = request.request.timeout_ms;
    }
    return create_native_web_fetch_transport(create_native_http_transport(request_config))(request);
  };
}

}  // namespace agent
