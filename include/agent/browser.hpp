#pragma once

#include "agent/core.hpp"

namespace agent {

class CancellationToken;

struct BrowserRenderRequest {
  std::string url;
  std::string selector;
  std::string wait_until = "networkidle";
  int timeout_ms = 0;
  bool screenshot = false;
  CancellationToken* cancellation = nullptr;
};

struct BrowserRenderResult {
  std::string url;
  std::string title;
  std::string html;
  std::string text;
  std::string screenshot_base64;
};

struct BrowserRendererMetadata {
  std::string name = "browser";
  std::string tier = "host-sensitive";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
};

class BrowserRenderer {
 public:
  virtual ~BrowserRenderer() = default;
  [[nodiscard]] virtual const BrowserRendererMetadata& metadata() const noexcept = 0;
  [[nodiscard]] virtual BrowserRenderResult render(const BrowserRenderRequest& request) = 0;
};

using BrowserRenderHandler = std::function<BrowserRenderResult(const BrowserRenderRequest&)>;

class BrowserCallbackRenderer : public BrowserRenderer {
 public:
  BrowserCallbackRenderer(BrowserRenderHandler handler,
                          BrowserRendererMetadata metadata = {});
  [[nodiscard]] const BrowserRendererMetadata& metadata() const noexcept override;
  [[nodiscard]] BrowserRenderResult render(const BrowserRenderRequest& request) override;

 private:
  BrowserRenderHandler handler_;
  BrowserRendererMetadata metadata_;
};

Value browser_render_request_to_value(const BrowserRenderRequest& request);
Value browser_render_result_to_value(const BrowserRenderResult& result);

}  // namespace agent
