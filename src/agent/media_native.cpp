#include "agent/media_native.hpp"

#include <utility>

namespace agent {

MediaResolverFunction create_native_http_media_resolver(NativeHttpClientConfig config) {
  return create_native_http_media_resolver(create_native_http_transport(std::move(config)));
}

}  // namespace agent
