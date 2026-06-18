#pragma once

#include "agent/http_native.hpp"
#include "agent/web.hpp"

namespace agent {

NativeWebSearchTransport create_native_web_search_transport(NativeHttpClientConfig config = {});
NativeWebFetchTransport create_native_web_fetch_transport(NativeHttpClientConfig config = {});

}  // namespace agent
