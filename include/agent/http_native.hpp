#pragma once

#include "agent/http.hpp"

namespace agent {

struct NativeHttpClientConfig {
  int timeout_ms = 30000;
  std::size_t max_response_bytes = 10 * 1024 * 1024;
};

HttpTransport create_native_http_transport(NativeHttpClientConfig config = {});
HttpStreamingTransport create_native_http_streaming_transport(NativeHttpClientConfig config = {});

}  // namespace agent
