#pragma once

#include "agent/http.hpp"

namespace agent {

class CancellationToken;

struct NativeProviderRequest {
  std::string provider;
  std::string endpoint;
  Value body = Value::object({});
  std::map<std::string, std::string> headers;
  std::string base_url;
  Value metadata = Value::object({});
  CancellationToken* cancellation = nullptr;
};

using NativeProviderTransport = std::function<Value(const NativeProviderRequest&)>;
using NativeProviderStreamTransport = std::function<std::vector<std::string>(const NativeProviderRequest&)>;
using NativeProviderStreamingChunkHandler = std::function<void(std::string_view chunk)>;
using NativeProviderStreamingTransport =
    std::function<void(const NativeProviderRequest&,
                       NativeProviderStreamingChunkHandler on_chunk)>;

void throw_if_native_provider_cancelled(CancellationToken* cancellation);
NativeProviderRequest with_native_provider_cancellation(NativeProviderRequest request,
                                                        CancellationToken* cancellation);

NativeProviderTransport create_native_provider_http_transport(HttpTransport transport);
NativeProviderStreamTransport create_native_provider_http_stream_transport(HttpTransport transport);
NativeProviderStreamingTransport create_native_provider_http_streaming_transport(HttpStreamingTransport transport);

}  // namespace agent
