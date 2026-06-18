#include "agent/web.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <queue>
#include <regex>
#include <sstream>

namespace agent {

namespace {

Value string_array_to_value(const std::vector<std::string>& values) {
  Value::Array output;
  for (const auto& value : values) {
    output.emplace_back(value);
  }
  return Value(output);
}

std::string origin_from_url(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) {
    return {};
  }
  const auto host_start = scheme + 3;
  const auto path_start = url.find('/', host_start);
  return path_start == std::string::npos ? url : url.substr(0, path_start);
}

std::string path_from_url(const std::string& url) {
  const auto scheme = url.find("://");
  const auto host_start = scheme == std::string::npos ? 0 : scheme + 3;
  const auto path_start = url.find('/', host_start);
  if (path_start == std::string::npos) {
    return "/";
  }
  const auto path_end = url.find_first_of("?#", path_start);
  return path_end == std::string::npos ? url.substr(path_start) : url.substr(path_start, path_end - path_start);
}

std::string directory_url(const std::string& url) {
  const auto query = url.find_first_of("?#");
  std::string clean = query == std::string::npos ? url : url.substr(0, query);
  const auto slash = clean.rfind('/');
  if (slash == std::string::npos) {
    return clean + "/";
  }
  return clean.substr(0, slash + 1);
}

std::string trim_trailing_slash(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string host_from_url(const std::string& url) {
  const auto scheme = url.find("://");
  const auto host_start = scheme == std::string::npos ? 0 : scheme + 3;
  const auto host_end = url.find_first_of(":/?#", host_start);
  std::string host = host_end == std::string::npos ? url.substr(host_start)
                                                   : url.substr(host_start, host_end - host_start);
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return host;
}

bool host_matches_domain(const std::string& host, const std::string& domain) {
  std::string normalized = domain;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (host == normalized) {
    return true;
  }
  return host.size() > normalized.size() &&
         host.compare(host.size() - normalized.size(), normalized.size(), normalized) == 0 &&
         host[host.size() - normalized.size() - 1] == '.';
}

std::vector<WebSearchResult> apply_domain_filter(std::vector<WebSearchResult> results,
                                                 const std::vector<std::string>& domains) {
  if (domains.empty()) {
    return results;
  }
  std::vector<WebSearchResult> filtered;
  for (auto& result : results) {
    const auto host = host_from_url(result.url);
    const bool allowed = std::any_of(domains.begin(), domains.end(), [&](const std::string& domain) {
      return host_matches_domain(host, domain);
    });
    if (allowed) {
      filtered.push_back(std::move(result));
    }
  }
  return filtered;
}

std::vector<WebSearchResult> limit_results(std::vector<WebSearchResult> results, std::size_t top_k) {
  if (top_k == 0) {
    return {};
  }
  if (results.size() <= top_k) {
    return results;
  }
  results.resize(top_k);
  return results;
}

std::string searxng_time_range(int recency_days) {
  if (recency_days <= 0) {
    return {};
  }
  if (recency_days <= 1) {
    return "day";
  }
  if (recency_days <= 7) {
    return "week";
  }
  if (recency_days <= 31) {
    return "month";
  }
  return "year";
}

std::vector<std::string> parse_robots_disallow_rules(const std::string& text) {
  std::vector<std::string> rules;
  bool active = false;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    auto normalized = trim_copy(line);
    if (normalized.empty() || normalized.front() == '#') {
      continue;
    }
    const auto comment = normalized.find('#');
    if (comment != std::string::npos) {
      normalized = trim_copy(normalized.substr(0, comment));
    }
    const auto colon = normalized.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto key = trim_copy(normalized.substr(0, colon));
    auto value = trim_copy(normalized.substr(colon + 1));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (key == "user-agent") {
      active = value == "*";
      continue;
    }
    if (active && key == "disallow" && !value.empty()) {
      rules.push_back(std::move(value));
    }
  }
  return rules;
}

std::vector<std::string> extract_sitemap_locations_from_xml(const std::string& xml) {
  std::vector<std::string> locations;
  static const std::regex loc_pattern(R"(<\s*loc\s*>\s*([^<]+?)\s*<\s*/\s*loc\s*>)",
                                      std::regex::icase);
  for (auto it = std::sregex_iterator(xml.begin(), xml.end(), loc_pattern);
       it != std::sregex_iterator(); ++it) {
    std::string value = trim_copy((*it)[1].str());
    std::string decoded;
    for (std::size_t index = 0; index < value.size();) {
      if (value.compare(index, 5, "&amp;") == 0) {
        decoded.push_back('&');
        index += 5;
      } else {
        decoded.push_back(value[index++]);
      }
    }
    if (!decoded.empty()) {
      locations.push_back(std::move(decoded));
    }
  }
  return locations;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool has_header(const std::map<std::string, std::string>& headers, const std::string& name) {
  const auto expected = lower_copy(name);
  for (const auto& [key, _] : headers) {
    if (lower_copy(key) == expected) {
      return true;
    }
  }
  return false;
}

std::string header_value(const std::map<std::string, std::string>& headers, const std::string& name) {
  const auto expected = lower_copy(name);
  for (const auto& [key, value] : headers) {
    if (lower_copy(key) == expected) {
      return value;
    }
  }
  return {};
}

std::string percent_encode_query_component(const std::string& value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (const unsigned char ch : value) {
    const bool unreserved = (ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '-' || ch == '_' || ch == '.' || ch == '~';
    if (unreserved) {
      out << static_cast<char>(ch);
      continue;
    }
    out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
  }
  return out.str();
}

std::string query_value_to_string(const Value& value) {
  if (value.is_string()) {
    return value.as_string();
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  return value.stringify(0);
}

void append_query_pairs(std::vector<std::pair<std::string, std::string>>& pairs,
                        const std::string& key,
                        const Value& value) {
  if (key.empty() || value.is_null()) {
    return;
  }
  if (value.is_array()) {
    for (const auto& item : value.as_array()) {
      append_query_pairs(pairs, key, item);
    }
    return;
  }
  auto text = query_value_to_string(value);
  if (text.empty()) {
    return;
  }
  pairs.emplace_back(key, std::move(text));
}

std::string append_query_params(std::string url, const Value& params) {
  if (!params.is_object()) {
    return url;
  }
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& [key, value] : params.as_object()) {
    append_query_pairs(pairs, key, value);
  }
  if (pairs.empty()) {
    return url;
  }
  url += url.find('?') == std::string::npos ? '?' : '&';
  for (std::size_t index = 0; index < pairs.size(); ++index) {
    if (index > 0) {
      url += '&';
    }
    url += percent_encode_query_component(pairs[index].first);
    url += '=';
    url += percent_encode_query_component(pairs[index].second);
  }
  return url;
}

bool looks_like_html(const std::string& content_type, const std::string& body) {
  if (lower_copy(content_type).find("html") != std::string::npos) {
    return true;
  }
  auto trimmed = lower_copy(trim_copy(body));
  return trimmed.rfind("<!doctype html", 0) == 0 ||
         trimmed.rfind("<html", 0) == 0 ||
         trimmed.find("<body") != std::string::npos;
}

std::string extract_html_title_for_web(const std::string& html) {
  static const std::regex title_pattern(R"(<\s*title[^>]*>(.*?)<\s*/\s*title\s*>)",
                                        std::regex::icase);
  std::smatch match;
  if (std::regex_search(html, match, title_pattern) && match.size() > 1) {
    return trim_copy(html_to_text(match[1].str()));
  }
  return {};
}

WebFetchedPage page_from_http_response(const WebFetchRequest& request, const HttpResponse& response) {
  WebFetchedPage page;
  page.url = request.url;
  page.final_url = response.final_url.empty() ? request.url : response.final_url;
  page.status = response.status;
  page.content_type = header_value(response.headers, "content-type");
  const auto extract = lower_copy(request.extract.empty() ? "auto" : request.extract);
  const bool html = looks_like_html(page.content_type, response.body);
  const auto mode = extract == "auto" ? (html ? "html" : "text") : extract;

  if (html) {
    page.html = response.body;
    page.title = extract_html_title_for_web(response.body);
    const auto extracted_text = html_to_text(response.body);
    if (mode == "html" || mode == "text") {
      page.text = extracted_text;
    }
    page.markdown = extracted_text;
    return page;
  }

  if (mode == "text") {
    page.text = response.body;
  } else if (mode == "markdown") {
    page.markdown = response.body;
  }
  return page;
}

}  // namespace

StaticWebSearchProvider::StaticWebSearchProvider(std::string name, std::vector<WebSearchResult> results)
    : name_(std::move(name)), results_(std::move(results)) {}

StaticWebSearchProvider::StaticWebSearchProvider(std::string name, WebSearchHandler handler)
    : name_(std::move(name)), handler_(std::move(handler)) {}

const std::string& StaticWebSearchProvider::name() const noexcept {
  return name_;
}

void StaticWebSearchProvider::add_result(WebSearchResult result) {
  if (result.rank == 0) {
    result.rank = results_.size() + 1;
  }
  results_.push_back(std::move(result));
}

std::vector<WebSearchResult> StaticWebSearchProvider::search(const WebSearchQuery& query) const {
  if (handler_) {
    return handler_(query);
  }
  if (query.top_k == 0) {
    return {};
  }

  std::string needle = query.query;
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  std::vector<WebSearchResult> matches;
  for (auto result : results_) {
    std::string haystack = result.title + " " + result.url + " " + result.snippet + " " + result.source;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (!needle.empty() && haystack.find(needle) == std::string::npos) {
      bool any_token = false;
      for (const auto& token : tokenize(needle)) {
        any_token = any_token || haystack.find(token) != std::string::npos;
      }
      if (!any_token) {
        continue;
      }
    }
    if (!query.domains.empty()) {
      const auto host = host_from_url(result.url);
      const bool domain_match = std::any_of(query.domains.begin(), query.domains.end(), [&](const auto& domain) {
        return host_matches_domain(host, domain);
      });
      if (!domain_match) {
        continue;
      }
    }
    result.rank = matches.size() + 1;
    matches.push_back(std::move(result));
    if (query.top_k > 0 && matches.size() >= query.top_k) {
      break;
    }
  }
  return matches;
}

WebSearchProviderRegistry::WebSearchProviderRegistry(std::vector<StaticWebSearchProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

WebSearchProviderRegistry::WebSearchProviderRegistry(const WebSearchProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
}

WebSearchProviderRegistry& WebSearchProviderRegistry::operator=(const WebSearchProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  return *this;
}

WebSearchProviderRegistry::WebSearchProviderRegistry(WebSearchProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
}

WebSearchProviderRegistry& WebSearchProviderRegistry::operator=(WebSearchProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  return *this;
}

StaticWebSearchProvider& WebSearchProviderRegistry::register_provider(StaticWebSearchProvider provider) {
  const auto name = provider.name();
  std::lock_guard<std::mutex> lock(mutex_);
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

const StaticWebSearchProvider* WebSearchProviderRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::optional<StaticWebSearchProvider> WebSearchProviderRegistry::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? std::nullopt : std::optional<StaticWebSearchProvider>(found->second);
}

std::vector<StaticWebSearchProvider> WebSearchProviderRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StaticWebSearchProvider> providers;
  for (const auto& [_, provider] : providers_) {
    providers.push_back(provider);
  }
  return providers;
}

Value web_search_result_to_value(const WebSearchResult& result) {
  return Value::object({{"title", result.title},
                        {"url", result.url},
                        {"snippet", result.snippet},
                        {"source", result.source},
                        {"publishedAt", result.published_at.empty() ? Value() : Value(result.published_at)},
                        {"rank", result.rank}});
}

Value web_fetched_page_to_value(const WebFetchedPage& page) {
  return Value::object({{"url", page.url},
                        {"finalUrl", page.final_url},
                        {"status", page.status},
                        {"contentType", page.content_type},
                        {"title", page.title},
                        {"text", page.text},
                        {"html", page.html},
                        {"markdown", page.markdown}});
}

NativeWebSearchTransport create_native_web_search_transport(HttpTransport transport) {
  if (!transport) {
    throw ConfigurationError("Native web search transport requires an HttpTransport.");
  }
  return [transport = std::move(transport)](const NativeWebSearchRequest& request) -> Value {
    const auto base_url = request.url.empty() ? build_url(request.base_url, request.endpoint) : request.url;
    HttpRequest http_request;
    http_request.url = append_query_params(base_url, request.query_params);
    http_request.method = "GET";
    http_request.headers = request.headers;
    http_request.cancellation = request.query.cancellation;
    if (!has_header(http_request.headers, "accept")) {
      http_request.headers["accept"] = "application/json";
    }
    if (!has_header(http_request.headers, "user-agent")) {
      http_request.headers["user-agent"] = "native-agent-framework-cpp/1.0";
    }
    const auto response = transport(http_request);
    const auto data = parse_http_response_body(response.body);
    if (response.status < 200 || response.status >= 300) {
      throw AdapterError("Native web search request failed with status " +
                         std::to_string(response.status) + ".", safe_json_stringify(data));
    }
    return data;
  };
}

NativeWebFetchTransport create_native_web_fetch_transport(HttpTransport transport) {
  if (!transport) {
    throw ConfigurationError("Native web fetch transport requires an HttpTransport.");
  }
  return [transport = std::move(transport)](const NativeWebFetchTransportRequest& request) -> WebFetchedPage {
    HttpRequest http_request;
    http_request.url = request.request.url;
    http_request.method = "GET";
    http_request.headers = request.headers;
    http_request.cancellation = request.request.cancellation;
    if (!has_header(http_request.headers, "accept")) {
      http_request.headers["accept"] = "text/html,application/xhtml+xml,application/xml;q=0.9,text/plain;q=0.8,*/*;q=0.5";
    }
    if (!has_header(http_request.headers, "user-agent")) {
      http_request.headers["user-agent"] = "native-agent-framework-cpp/1.0";
    }
    return page_from_http_response(request.request, transport(http_request));
  };
}

StaticWebSearchProvider create_brave_web_search_provider(NativeWebSearchTransport transport,
                                                         std::string api_key,
                                                         std::string base_url) {
  if (!transport) {
    throw ConfigurationError("Brave search provider requires a NativeWebSearchTransport.");
  }
  base_url = trim_trailing_slash(base_url.empty() ? "https://api.search.brave.com/res/v1" : std::move(base_url));
  return StaticWebSearchProvider("brave", [transport = std::move(transport),
                                           api_key = std::move(api_key),
                                           base_url = std::move(base_url)](const WebSearchQuery& query) {
    if (api_key.empty()) {
      throw ConfigurationError("Brave search provider requires apiKey or BRAVE_SEARCH_API_KEY.");
    }
    Value query_params = Value::object({{"q", query.query}, {"count", query.top_k}});
    if (query.recency_days > 0) {
      query_params["freshness"] = std::to_string(query.recency_days) + "d";
    }
    if (!query.locale.empty()) {
      query_params["search_lang"] = query.locale;
    }
    query_params["safesearch"] = query.safe_search;
    NativeWebSearchRequest request;
    request.provider = "brave";
    request.base_url = base_url;
    request.endpoint = "/web/search";
    request.url = base_url + request.endpoint;
    request.headers = {{"x-subscription-token", api_key}};
    request.query_params = std::move(query_params);
    request.query = query;
    const auto raw = transport(request);
    std::vector<WebSearchResult> results;
    for (const auto& item : raw.at("web").at("results").as_array()) {
      const auto url = item.at("url").as_string();
      if (url.empty()) {
        continue;
      }
      results.push_back(WebSearchResult{
          .title = item.at("title").as_string(url),
          .url = url,
          .snippet = item.at("description").as_string(),
          .source = "brave",
          .published_at = item.at("age").as_string(item.at("page_age").as_string()),
          .rank = results.size() + 1,
      });
    }
    return limit_results(apply_domain_filter(std::move(results), query.domains), query.top_k);
  });
}

StaticWebSearchProvider create_searxng_web_search_provider(NativeWebSearchTransport transport,
                                                           std::string base_url) {
  if (!transport) {
    throw ConfigurationError("SearXNG search provider requires a NativeWebSearchTransport.");
  }
  if (base_url.empty()) {
    throw ConfigurationError("SearXNG search provider requires baseUrl or baseUrlEnv.");
  }
  base_url = trim_trailing_slash(std::move(base_url));
  return StaticWebSearchProvider("searxng", [transport = std::move(transport),
                                             base_url = std::move(base_url)](const WebSearchQuery& query) {
    Value query_params = Value::object({{"q", query.query}, {"format", "json"}});
    if (!query.locale.empty()) {
      query_params["language"] = query.locale;
    }
    const auto time_range = searxng_time_range(query.recency_days);
    if (!time_range.empty()) {
      query_params["time_range"] = time_range;
    }
    query_params["safesearch"] = query.safe_search == "strict" ? "2" : query.safe_search == "off" ? "0" : "1";
    NativeWebSearchRequest request;
    request.provider = "searxng";
    request.base_url = base_url;
    request.endpoint = "/search";
    request.url = base_url + request.endpoint;
    request.query_params = std::move(query_params);
    request.query = query;
    const auto raw = transport(request);
    std::vector<WebSearchResult> results;
    for (const auto& item : raw.at("results").as_array()) {
      const auto url = item.at("url").as_string();
      if (url.empty()) {
        continue;
      }
      results.push_back(WebSearchResult{
          .title = item.at("title").as_string(url),
          .url = url,
          .snippet = item.at("content").as_string(),
          .source = item.at("engine").as_string(),
          .published_at = item.at("publishedDate").as_string(),
          .rank = results.size() + 1,
      });
    }
    return limit_results(apply_domain_filter(std::move(results), query.domains), query.top_k);
  });
}

NativeWebPageFetcher::NativeWebPageFetcher(NativeWebFetchTransport transport)
    : transport_(std::move(transport)) {}

void NativeWebPageFetcher::set_transport(NativeWebFetchTransport transport) {
  std::lock_guard<std::mutex> lock(mutex_);
  transport_ = std::move(transport);
}

void NativeWebPageFetcher::register_page(WebFetchedPage page) {
  if (page.final_url.empty()) {
    page.final_url = page.url;
  }
  if (page.text.empty() && !page.html.empty()) {
    page.text = html_to_text(page.html);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  pages_[page.url] = std::move(page);
}

WebFetchedPage NativeWebPageFetcher::fetch(const WebFetchRequest& request) const {
  NativeWebFetchTransport transport;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pages_.find(request.url);
    if (found != pages_.end()) {
      return found->second;
    }
    transport = transport_;
  }
  if (request.url.rfind("file://", 0) == 0) {
    const std::filesystem::path path(request.url.substr(7));
    const auto text = bytes_to_text(read_binary_file(path));
    return WebFetchedPage{request.url, request.url, 200, "text/plain", path.filename().string(), text, "", ""};
  }
  if (transport) {
    auto page = transport(NativeWebFetchTransportRequest{.request = request, .headers = request.headers});
    if (page.url.empty()) {
      page.url = request.url;
    }
    if (page.final_url.empty()) {
      page.final_url = page.url;
    }
    const auto extract = lower_copy(request.extract.empty() ? "auto" : request.extract);
    const auto mode = extract == "auto" ? (page.html.empty() ? "text" : "html") : extract;
    if (page.text.empty() && !page.html.empty() && mode != "markdown") {
      page.text = html_to_text(page.html);
    }
    if (page.markdown.empty() && !page.html.empty()) {
      page.markdown = html_to_text(page.html);
    }
    return page;
  }
  throw ConfigurationError("No registered page for URL: " + request.url);
}

BrowserBackedWebPageFetcher::BrowserBackedWebPageFetcher(BrowserBackedWebPageFetcherConfig config)
    : fetcher_(std::move(config.fetcher)),
      browser_(std::move(config.browser)),
      minimum_text_length_(std::max(0, config.minimum_text_length)),
      browser_request_(std::move(config.browser_request)) {
  if (!fetcher_) {
    fetcher_ = std::make_shared<NativeWebPageFetcher>();
  }
  if (!browser_) {
    throw ConfigurationError("BrowserBackedWebPageFetcher requires a BrowserRenderer.");
  }
}

std::size_t BrowserBackedWebPageFetcher::extracted_length(const WebFetchedPage& page) {
  const auto text = trim_copy(page.text);
  const auto markdown = trim_copy(page.markdown);
  return std::max(text.size(), markdown.size());
}

bool BrowserBackedWebPageFetcher::should_fallback_to_browser(const WebFetchedPage& page) const {
  if (page.status >= 400) {
    return false;
  }
  return extracted_length(page) < static_cast<std::size_t>(minimum_text_length_);
}

WebFetchedPage BrowserBackedWebPageFetcher::fetch(const WebFetchRequest& request) const {
  try {
    auto page = fetcher_->fetch(request);
    if (!should_fallback_to_browser(page)) {
      return page;
    }
  } catch (const std::exception&) {
    // Match the TypeScript wrapper: primary fetch failures fall through to browser rendering.
  }

  BrowserRenderRequest render_request = browser_request_;
  render_request.url = request.url;
  if (render_request.timeout_ms <= 0) {
    render_request.timeout_ms = request.timeout_ms;
  }
  render_request.cancellation = request.cancellation;
  auto rendered = browser_->render(render_request);
  return WebFetchedPage{
      .url = request.url,
      .final_url = rendered.url.empty() ? request.url : rendered.url,
      .status = 200,
      .content_type = "text/html; charset=utf-8",
      .title = rendered.title,
      .text = rendered.text,
      .html = rendered.html,
      .markdown = rendered.text,
  };
}

Value web_crawl_page_to_value(const WebCrawlPage& page) {
  return Value::object({{"page", web_fetched_page_to_value(page.page)},
                        {"depth", page.depth},
                        {"links", string_array_to_value(page.links)}});
}

Value web_crawl_result_to_value(const WebCrawlResult& result) {
  Value::Array pages;
  for (const auto& page : result.pages) {
    pages.push_back(web_crawl_page_to_value(page));
  }
  return Value::object({{"startUrl", result.start_url},
                        {"pages", Value(pages)},
                        {"blockedUrls", string_array_to_value(result.blocked_urls)},
                        {"discoveredUrls", string_array_to_value(result.discovered_urls)}});
}

NativeWebCrawler::NativeWebCrawler(const NativeWebPageFetcher* fetcher) : fetcher_(fetcher) {}

void NativeWebCrawler::set_fetcher(const NativeWebPageFetcher* fetcher) {
  std::lock_guard<std::mutex> lock(mutex_);
  fetcher_ = fetcher;
}

WebCrawlResult NativeWebCrawler::crawl(const WebCrawlRequest& request) const {
  const NativeWebPageFetcher* fetcher = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fetcher = fetcher_;
  }
  if (!fetcher) {
    throw ConfigurationError("Web crawler requires a configured NativeWebPageFetcher.");
  }

  WebCrawlResult result;
  result.start_url = request.url;
  const std::size_t max_pages = request.max_pages;
  std::queue<std::pair<std::string, std::size_t>> pending;
  std::set<std::string> queued;
  std::set<std::string> discovered;
  pending.push({request.url, 0});
  queued.insert(request.url);

  const auto origin = origin_from_url(request.url);
  std::vector<std::string> disallow_rules;
  if (request.obey_robots && !origin.empty()) {
    try {
      auto robots = fetcher->fetch(WebFetchRequest{
          .url = origin + "/robots.txt",
          .extract = "text",
          .cancellation = request.cancellation,
      });
      disallow_rules = parse_robots_disallow_rules(!robots.text.empty() ? robots.text : robots.html);
    } catch (const std::exception&) {
      disallow_rules.clear();
    }
  }
  if (request.discover_sitemap && !origin.empty()) {
    try {
      auto sitemap = fetcher->fetch(WebFetchRequest{
          .url = origin + "/sitemap.xml",
          .extract = "text",
          .cancellation = request.cancellation,
      });
      const auto xml = !sitemap.text.empty() ? sitemap.text : sitemap.html;
      for (const auto& location : extract_sitemap_locations_from_xml(xml)) {
        if (!location.empty() && queued.insert(location).second) {
          pending.push({location, 0});
        }
      }
    } catch (const std::exception&) {
      // Sitemap discovery is opportunistic in the TypeScript crawler.
    }
  }

  while (!pending.empty() && result.pages.size() < max_pages) {
    const auto [url, depth] = pending.front();
    pending.pop();

    if (!discovered.insert(url).second) {
      continue;
    }
    result.discovered_urls.push_back(url);

    if (!domain_allowed(url, request.allowed_domains)) {
      result.blocked_urls.push_back(url);
      continue;
    }
    const auto path = path_from_url(url);
    const bool blocked_by_robots = std::any_of(disallow_rules.begin(), disallow_rules.end(),
                                               [&](const std::string& rule) {
                                                 return !rule.empty() && path.rfind(rule, 0) == 0;
                                               });
    if (blocked_by_robots) {
      result.blocked_urls.push_back(url);
      continue;
    }

    auto page = fetcher->fetch(WebFetchRequest{
        .url = url,
        .extract = "markdown",
        .cancellation = request.cancellation,
    });
    const auto raw_links = extract_links(page);
    std::vector<std::string> links;
    links.reserve(raw_links.size());
    const auto link_base = page.final_url.empty() ? page.url : page.final_url;
    for (const auto& link : raw_links) {
      const auto resolved = resolve_link(link_base, link);
      if (!resolved.empty()) {
        links.push_back(resolved);
      }
    }
    result.pages.push_back(WebCrawlPage{.page = page, .depth = depth, .links = links});
    if (depth >= request.max_depth) {
      continue;
    }
    for (const auto& link : links) {
      if (queued.find(link) != queued.end() || discovered.find(link) != discovered.end()) {
        continue;
      }
      queued.insert(link);
      pending.push({link, depth + 1});
    }
  }

  return result;
}

std::vector<std::string> NativeWebCrawler::extract_links(const WebFetchedPage& page) {
  std::vector<std::string> links;
  static const std::regex href_pattern(R"(href\s*=\s*["']([^"']+)["'])", std::regex::icase);
  const std::string source = page.html.empty() ? page.markdown : page.html;
  for (auto it = std::sregex_iterator(source.begin(), source.end(), href_pattern);
       it != std::sregex_iterator(); ++it) {
    const auto href = (*it)[1].str();
    if (href.empty() || href.front() == '#') {
      continue;
    }
    links.push_back(href);
  }
  return links;
}

bool NativeWebCrawler::domain_allowed(const std::string& url, const std::vector<std::string>& domains) {
  if (domains.empty()) {
    return true;
  }
  const auto host = host_from_url(url);
  return std::any_of(domains.begin(), domains.end(), [&](const auto& domain) {
    return host_matches_domain(host, domain);
  });
}

std::string NativeWebCrawler::resolve_link(const std::string& base_url, const std::string& href) {
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0 || href.rfind("file://", 0) == 0) {
    return href;
  }
  if (href.rfind("//", 0) == 0) {
    const auto scheme = base_url.find("://");
    return scheme == std::string::npos ? "https:" + href : base_url.substr(0, scheme) + ":" + href;
  }
  if (!href.empty() && href.front() == '/') {
    const auto origin = origin_from_url(base_url);
    return origin.empty() ? href : origin + href;
  }
  return directory_url(base_url) + href;
}
}  // namespace agent
