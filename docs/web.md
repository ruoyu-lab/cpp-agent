# Web API

The native web module mirrors the NodeJS web packages with search providers,
page fetchers, browser-backed extraction, and bounded crawling. Network and
browser integrations stay behind injected interfaces so the core library remains
zero-dependency.

## Search

Use `WebSearchProviderRegistry` to register one or more providers:

```cpp
agent::WebSearchProviderRegistry registry({
    agent::StaticWebSearchProvider("docs", [](const agent::WebSearchQuery& query) {
      return std::vector<agent::WebSearchResult>{
          {.title = "Native docs", .url = "https://docs.example/native", .rank = 1},
      };
    }),
});

const auto* provider = registry.get("docs");
auto results = provider->search(agent::WebSearchQuery{
    .query = "native",
    .top_k = 4,
    .domains = {"docs.example"},
});
```

`StaticWebSearchProvider` supports host-aware domain filtering and `top_k == 0`
returns an empty result set, matching the NodeJS explicit-zero behavior.

`create_brave_web_search_provider` and `create_searxng_web_search_provider`
shape provider requests and parse responses through `NativeWebSearchTransport`.
Use `create_native_web_search_transport` with an injected `HttpTransport` when
plain HTTP is sufficient; provide your own transport for HTTPS/TLS or production
policy.

## Fetching

`NativeWebPageFetcher` can serve registered pages, local `file://` URLs, or a
native fetch transport:

```cpp
agent::NativeWebPageFetcher fetcher(agent::create_native_web_fetch_transport());
auto page = fetcher.fetch(agent::WebFetchRequest{
    .url = "http://127.0.0.1:3000/docs",
    .extract = "auto",
});
```

The default extract modes match NodeJS:

- `auto`: HTML responses return HTML plus extracted text/markdown; non-HTML
  responses return text.
- `html`: HTML responses keep the raw HTML and extracted text.
- `text`: returns extracted text for HTML or the raw body for non-HTML.
- `markdown`: returns extracted markdown/text without setting `text`.

The built-in transport is intentionally limited to local files and plain HTTP.
Inject a transport or fetcher for HTTPS, authenticated requests, browser
sessions, proxying, or organization-specific network policy.

`WebSearchQuery`, `WebFetchRequest`, and `WebCrawlRequest` carry an optional
`CancellationToken*`. The web builtin tools populate it from
`ToolExecutionContext::cancellation`, and the native fetch/search transport
bridges pass it through to underlying `HttpRequest` values.

## Browser Fallback

`BrowserBackedWebPageFetcher` wraps a primary fetcher and a `BrowserRenderer`.
It returns the primary page when enough text or markdown was extracted. It
falls back to browser rendering when the primary fetch fails or extracted text
is shorter than `minimum_text_length`.

```cpp
auto primary = std::make_shared<agent::NativeWebPageFetcher>(
    agent::create_native_web_fetch_transport());

agent::BrowserBackedWebPageFetcher fetcher(agent::BrowserBackedWebPageFetcherConfig{
    .fetcher = primary,
    .browser = browser_renderer,
    .minimum_text_length = 160,
});
```

HTTP status codes `>= 400` do not trigger browser fallback. They are returned
as fetch results so callers can inspect the status and body.

## Crawling

`NativeWebCrawler` performs bounded crawl traversal over a configured fetcher:

```cpp
agent::NativeWebCrawler crawler(&fetcher);
auto result = crawler.crawl(agent::WebCrawlRequest{
    .url = "http://127.0.0.1:3000/",
    .max_depth = 1,
    .max_pages = 10,
    .allowed_domains = {"127.0.0.1"},
});
```

The crawler observes explicit `max_pages == 0`, reports skipped and blocked
URLs, reads robots and sitemap hints through the same fetcher, and serializes
results with `web_crawl_result_to_value`.

## Tool Integration

`create_web_builtin_tools` exposes `web.search` and `web.fetch`.
Tools can be wired directly through factory arguments or indirectly through
`ToolExecutionContext::service_refs`.

```cpp
auto tools = agent::create_web_builtin_tools(&registry, "docs", &fetcher);
agent::ToolExecutor executor(agent::ToolRegistry(tools));
```

`web.fetch` also has a native default for `file://` and plain `http://` when no
fetcher is provided. `web.search` still requires an explicit provider registry
and default provider name.
