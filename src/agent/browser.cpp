#include "agent/agent.hpp"

namespace agent {

BrowserCallbackRenderer::BrowserCallbackRenderer(BrowserRenderHandler handler,
                                                 BrowserRendererMetadata metadata)
    : handler_(std::move(handler)), metadata_(std::move(metadata)) {
  if (!handler_) {
    throw ConfigurationError("BrowserCallbackRenderer requires a render handler.");
  }
  if (metadata_.name.empty()) {
    metadata_.name = "callback";
  }
  if (metadata_.tier.empty()) {
    metadata_.tier = "host-sensitive";
  }
}

const BrowserRendererMetadata& BrowserCallbackRenderer::metadata() const noexcept {
  return metadata_;
}

BrowserRenderResult BrowserCallbackRenderer::render(const BrowserRenderRequest& request) {
  return handler_(request);
}

Value browser_render_request_to_value(const BrowserRenderRequest& request) {
  return Value::object({{"url", request.url},
                        {"selector", request.selector.empty() ? Value() : Value(request.selector)},
                        {"waitUntil", request.wait_until},
                        {"timeoutMs", request.timeout_ms},
                        {"screenshot", request.screenshot}});
}

Value browser_render_result_to_value(const BrowserRenderResult& result) {
  return Value::object({{"url", result.url},
                        {"title", result.title.empty() ? Value() : Value(result.title)},
                        {"html", result.html.empty() ? Value() : Value(result.html)},
                        {"text", result.text.empty() ? Value() : Value(result.text)},
                        {"screenshotBase64", result.screenshot_base64.empty()
                                                 ? Value()
                                                 : Value(result.screenshot_base64)}});
}

}  // namespace agent
