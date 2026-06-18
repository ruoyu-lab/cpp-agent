#pragma once

#include "agent/http_native.hpp"
#include "agent/media.hpp"

namespace agent {

MediaResolverFunction create_native_http_media_resolver(NativeHttpClientConfig config = {});

}  // namespace agent
