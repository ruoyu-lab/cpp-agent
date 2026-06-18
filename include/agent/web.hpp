#pragma once

#include "agent/browser.hpp"
#include "agent/core.hpp"
#include "agent/http.hpp"

#include <mutex>
#include <optional>

namespace agent {

class CancellationToken;

struct WebSearchQuery {
  std::string query;
  std::size_t top_k = 8;
  std::string locale;
  int recency_days = 0;
  std::vector<std::string> domains;
  std::string safe_search = "moderate";
  CancellationToken* cancellation = nullptr;
};

struct WebSearchResult {
  std::string title;
  std::string url;
  std::string snippet;
  std::string source;
  std::string published_at;
  std::size_t rank = 0;
};

struct NativeWebSearchRequest {
  std::string provider;
  std::string base_url;
  std::string endpoint;
  std::string url;
  std::map<std::string, std::string> headers;
  Value query_params = Value::object({});
  WebSearchQuery query;
};

using NativeWebSearchTransport = std::function<Value(const NativeWebSearchRequest&)>;
using WebSearchHandler = std::function<std::vector<WebSearchResult>(const WebSearchQuery&)>;

class StaticWebSearchProvider {
 public:
  StaticWebSearchProvider(std::string name = "static", std::vector<WebSearchResult> results = {});
  StaticWebSearchProvider(std::string name, WebSearchHandler handler);
  [[nodiscard]] const std::string& name() const noexcept;
  void add_result(WebSearchResult result);
  [[nodiscard]] std::vector<WebSearchResult> search(const WebSearchQuery& query) const;

 private:
  std::string name_;
  std::vector<WebSearchResult> results_;
  WebSearchHandler handler_;
};

class WebSearchProviderRegistry {
 public:
  explicit WebSearchProviderRegistry(std::vector<StaticWebSearchProvider> providers = {});
  WebSearchProviderRegistry(const WebSearchProviderRegistry& other);
  WebSearchProviderRegistry& operator=(const WebSearchProviderRegistry& other);
  WebSearchProviderRegistry(WebSearchProviderRegistry&& other) noexcept;
  WebSearchProviderRegistry& operator=(WebSearchProviderRegistry&& other) noexcept;
  StaticWebSearchProvider& register_provider(StaticWebSearchProvider provider);
  [[nodiscard]] const StaticWebSearchProvider* get(const std::string& name) const;
  [[nodiscard]] std::optional<StaticWebSearchProvider> find(const std::string& name) const;
  [[nodiscard]] std::vector<StaticWebSearchProvider> list() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, StaticWebSearchProvider> providers_;
};

struct WebFetchRequest {
  std::string url;
  int timeout_ms = 15000;
  std::string extract = "auto";
  std::map<std::string, std::string> headers;
  CancellationToken* cancellation = nullptr;
};

struct WebFetchedPage {
  std::string url;
  std::string final_url;
  int status = 200;
  std::string content_type;
  std::string title;
  std::string text;
  std::string html;
  std::string markdown;
};

struct NativeWebFetchTransportRequest {
  WebFetchRequest request;
  std::map<std::string, std::string> headers;
};

using NativeWebFetchTransport = std::function<WebFetchedPage(const NativeWebFetchTransportRequest&)>;

Value web_search_result_to_value(const WebSearchResult& result);
Value web_fetched_page_to_value(const WebFetchedPage& page);
NativeWebSearchTransport create_native_web_search_transport(HttpTransport transport);
NativeWebFetchTransport create_native_web_fetch_transport(HttpTransport transport);
StaticWebSearchProvider create_brave_web_search_provider(NativeWebSearchTransport transport,
                                                         std::string api_key = {},
                                                         std::string base_url = "https://api.search.brave.com/res/v1");
StaticWebSearchProvider create_searxng_web_search_provider(NativeWebSearchTransport transport,
                                                           std::string base_url);

class NativeWebPageFetcher {
 public:
  explicit NativeWebPageFetcher(NativeWebFetchTransport transport = {});
  virtual ~NativeWebPageFetcher() = default;
  void set_transport(NativeWebFetchTransport transport);
  void register_page(WebFetchedPage page);
  [[nodiscard]] virtual WebFetchedPage fetch(const WebFetchRequest& request) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, WebFetchedPage> pages_;
  NativeWebFetchTransport transport_;
};

struct BrowserBackedWebPageFetcherConfig {
  std::shared_ptr<NativeWebPageFetcher> fetcher;
  std::shared_ptr<BrowserRenderer> browser;
  int minimum_text_length = 160;
  BrowserRenderRequest browser_request;
};

class BrowserBackedWebPageFetcher : public NativeWebPageFetcher {
 public:
  explicit BrowserBackedWebPageFetcher(BrowserBackedWebPageFetcherConfig config);
  [[nodiscard]] WebFetchedPage fetch(const WebFetchRequest& request) const override;

 private:
  [[nodiscard]] static std::size_t extracted_length(const WebFetchedPage& page);
  [[nodiscard]] bool should_fallback_to_browser(const WebFetchedPage& page) const;

  std::shared_ptr<NativeWebPageFetcher> fetcher_;
  std::shared_ptr<BrowserRenderer> browser_;
  int minimum_text_length_ = 160;
  BrowserRenderRequest browser_request_;
};

struct WebCrawlRequest {
  std::string url;
  std::size_t max_depth = 1;
  std::size_t max_pages = 10;
  std::vector<std::string> allowed_domains;
  bool obey_robots = true;
  bool discover_sitemap = true;
  CancellationToken* cancellation = nullptr;
};

struct WebCrawlPage {
  WebFetchedPage page;
  std::size_t depth = 0;
  std::vector<std::string> links;
};

struct WebCrawlResult {
  std::string start_url;
  std::vector<WebCrawlPage> pages;
  std::vector<std::string> skipped;
  std::vector<std::string> blocked_urls;
  std::vector<std::string> discovered_urls;
};

Value web_crawl_page_to_value(const WebCrawlPage& page);
Value web_crawl_result_to_value(const WebCrawlResult& result);

class NativeWebCrawler {
 public:
  explicit NativeWebCrawler(const NativeWebPageFetcher* fetcher = nullptr);
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] WebCrawlResult crawl(const WebCrawlRequest& request) const;

 private:
  [[nodiscard]] static std::vector<std::string> extract_links(const WebFetchedPage& page);
  [[nodiscard]] static bool domain_allowed(const std::string& url, const std::vector<std::string>& domains);
  [[nodiscard]] static std::string resolve_link(const std::string& base_url, const std::string& href);

  const NativeWebPageFetcher* fetcher_;
  mutable std::mutex mutex_;
};

}  // namespace agent
