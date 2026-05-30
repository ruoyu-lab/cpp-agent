# Browser API

The native browser module mirrors the NodeJS browser package at the interface
level. It defines renderer requests/results and a callback-backed renderer
adapter without linking a browser engine into the core library.

## Renderer Interface

Implement `BrowserRenderer` when host code owns browser automation:

```cpp
class MyRenderer final : public agent::BrowserRenderer {
 public:
  const agent::BrowserRendererMetadata& metadata() const noexcept override {
    return metadata_;
  }

  agent::BrowserRenderResult render(const agent::BrowserRenderRequest& request) override {
    // Delegate to a host browser process, service, or test fixture.
    return agent::BrowserRenderResult{
        .url = request.url,
        .title = "Rendered",
        .text = "rendered page text",
    };
  }

 private:
  agent::BrowserRendererMetadata metadata_{
      .name = "host-browser",
      .tier = "host-sensitive",
  };
};
```

`BrowserRenderRequest` contains:

- `url`: page URL.
- `selector`: optional extraction selector.
- `wait_until`: `load`, `domcontentloaded`, or `networkidle`.
- `timeout_ms`: optional timeout in milliseconds.
- `screenshot`: whether screenshot output is requested.
- `cancellation`: optional cooperative cancellation token.

`BrowserRenderResult` contains the final URL, optional title, HTML, text, and
base64 screenshot payload.

## Callback Renderer

Use `BrowserCallbackRenderer` for tests, embedded hosts, or app-provided
browser bridges:

```cpp
agent::BrowserCallbackRenderer renderer(
    [](const agent::BrowserRenderRequest& request) {
      return agent::BrowserRenderResult{
          .url = request.url,
          .text = request.selector.empty() ? "page text" : "selected text",
      };
    },
    agent::BrowserRendererMetadata{.name = "callback"});
```

The constructor validates that a render handler exists and fills missing
metadata defaults with `callback` and `host-sensitive`.

## Builtin Tools

`create_browser_builtin_tools` exposes:

- `browser.render`: render a page.
- `browser.extract`: render and extract text, optionally scoped by selector.
- `browser.screenshot`: render with screenshot output requested.

```cpp
auto tools = agent::create_browser_builtin_tools(&renderer);
agent::ToolExecutor executor(agent::ToolRegistry(tools));
```

The tools can also resolve the renderer from
`ToolExecutionContext::service_refs.browser_renderer`, which is how configured
apps and runners inject browser services at execution time. The tool
cancellation token is copied into `BrowserRenderRequest::cancellation` so host
renderers can abort browser work.

## Web And Knowledge Integration

Browser renderers are reused by:

- `BrowserBackedWebPageFetcher`, which falls back to rendering when primary
  fetch text is too short.
- Website knowledge ingestion, where browser-rendered text can be used before
  documents are chunked and indexed.

The native framework does not bundle Playwright or any browser runtime. A
production browser backend remains an injected host concern under the
zero-dependency rule.
