#include "agent/browser.hpp"
#include "agent/knowledge_io.hpp"
#include "agent/web.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <sstream>

namespace agent {
namespace {

const std::set<std::string>& default_repository_extensions() {
  static const std::set<std::string> extensions = {
      ".md",   ".mdx", ".txt", ".json", ".yaml", ".yml", ".toml", ".ts",  ".tsx", ".js",   ".jsx",
      ".mjs",  ".cjs", ".py",  ".go",   ".rs",   ".java", ".sh",  ".sql", ".vue", ".svelte"};
  return extensions;
}

const std::set<std::string>& default_knowledge_exclude_directories() {
  static const std::set<std::string> directories = {
      ".git", "node_modules", "dist", "build", "coverage", ".next", ".nuxt", "target", "out"};
  return directories;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string extension_for_path(const std::filesystem::path& path) {
  return lower_copy(path.extension().string());
}

std::string normalize_extension(std::string extension) {
  extension = lower_copy(std::move(extension));
  if (!extension.empty() && extension.front() != '.') {
    extension.insert(extension.begin(), '.');
  }
  return extension;
}

std::set<std::string> string_set_from_value(const Value& value, const std::set<std::string>& fallback = {}) {
  if (!value.is_array()) {
    return fallback;
  }
  std::set<std::string> values;
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.insert(normalize_extension(text));
    }
  }
  return values;
}

std::set<std::string> string_set_preserve_from_value(const Value& value, const std::set<std::string>& fallback = {}) {
  if (!value.is_array()) {
    return fallback;
  }
  std::set<std::string> values;
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.insert(text);
    }
  }
  return values;
}

std::vector<std::string> string_vector_from_value(const Value& value) {
  std::vector<std::string> values;
  if (!value.is_array()) {
    return values;
  }
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      values.push_back(text);
    }
  }
  return values;
}

std::optional<std::size_t> optional_size_option_from_value(const Value& options, const std::string& key) {
  if (!options.is_object() || !options.contains(key) || !options.at(key).is_number()) {
    return std::nullopt;
  }
  const auto raw = options.at(key).as_integer(0);
  return static_cast<std::size_t>(std::max<long long>(0, raw));
}

Value copy_object_or_empty(const Value& value) {
  return value.is_object() ? value : Value::object({});
}

Value string_array_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return Value(array);
}

LoadedKnowledgeDocument build_loaded_document(std::string source_type,
                                              std::string uri,
                                              std::string title,
                                              std::string content,
                                              Value metadata = Value::object({})) {
  LoadedKnowledgeDocument document;
  document.source_type = std::move(source_type);
  document.uri = std::move(uri);
  document.title = title.empty() ? document.uri : std::move(title);
  document.content = std::move(content);
  document.metadata = std::move(metadata);
  return document;
}

std::vector<std::string> extract_sitemap_locations(const std::string& xml) {
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

std::string best_page_content(const WebFetchedPage& page) {
  if (!page.markdown.empty()) {
    return page.markdown;
  }
  if (!page.text.empty()) {
    return page.text;
  }
  return html_to_text(page.html);
}

std::string best_page_url(const WebFetchedPage& page) {
  return page.final_url.empty() ? page.url : page.final_url;
}

std::string normalize_repository_path(std::filesystem::path path) {
  auto value = path.generic_string();
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  return value;
}

std::regex repository_glob_pattern_to_regex(const std::string& pattern) {
  const auto normalized = normalize_repository_path(pattern);
  std::string escaped;
  for (std::size_t index = 0; index < normalized.size(); ++index) {
    const char ch = normalized[index];
    if (ch == '*') {
      if (index + 1 < normalized.size() && normalized[index + 1] == '*') {
        escaped += ".*";
        ++index;
      } else {
        escaped += "[^/]*";
      }
      continue;
    }
    if (ch == '?') {
      escaped += ".";
      continue;
    }
    if (std::string(".+^${}()|[]\\").find(ch) != std::string::npos) {
      escaped.push_back('\\');
    }
    escaped.push_back(ch);
  }
  return std::regex("^" + escaped + "$");
}

bool repository_path_matches_patterns(const std::string& path, const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return true;
  }
  const auto normalized = normalize_repository_path(path);
  return std::any_of(patterns.begin(), patterns.end(), [&](const auto& pattern) {
    return std::regex_match(normalized, repository_glob_pattern_to_regex(pattern));
  });
}

struct ParsedGitHubRepositoryUrl {
  std::string owner;
  std::string repo;
  std::string ref;
  std::string root_path;
};

std::vector<std::string> split_path_segments(const std::string& value) {
  std::vector<std::string> segments;
  std::stringstream stream(value);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (!segment.empty()) {
      segments.push_back(segment);
    }
  }
  return segments;
}

ParsedGitHubRepositoryUrl parse_github_repository_url(const std::string& source_url) {
  const std::string https_prefix = "https://github.com/";
  const std::string http_prefix = "http://github.com/";
  std::string path;
  if (source_url.rfind(std::string(https_prefix), 0) == 0) {
    path = source_url.substr(https_prefix.size());
  } else if (source_url.rfind(std::string(http_prefix), 0) == 0) {
    path = source_url.substr(http_prefix.size());
  } else {
    throw ConfigurationError("Unsupported GitHub repository URL: " + source_url);
  }
  if (const auto marker = path.find_first_of("?#"); marker != std::string::npos) {
    path = path.substr(0, marker);
  }
  const auto segments = split_path_segments(path);
  if (segments.size() < 2) {
    throw ConfigurationError("Unsupported GitHub repository URL: " + source_url);
  }
  ParsedGitHubRepositoryUrl parsed;
  parsed.owner = segments[0];
  parsed.repo = segments[1];
  if (parsed.repo.ends_with(".git")) {
    parsed.repo = parsed.repo.substr(0, parsed.repo.size() - 4);
  }
  if (segments.size() > 3 && segments[2] == "tree") {
    parsed.ref = segments[3];
    std::filesystem::path root;
    for (std::size_t index = 4; index < segments.size(); ++index) {
      root /= segments[index];
    }
    parsed.root_path = normalize_repository_path(root);
  }
  return parsed;
}

std::string github_fetch_text(const NativeWebPageFetcher& fetcher,
                              const std::string& url,
                              const Value& source,
                              const std::string& failure_label) {
  WebFetchRequest request{.url = url, .extract = "text"};
  const auto token = source.at("token").as_string();
  if (!token.empty()) {
    request.headers["authorization"] = "Bearer " + token;
  }
  const auto page = fetcher.fetch(request);
  if (page.status >= 400) {
    throw ConfigurationError(failure_label + " failed with status " + std::to_string(page.status) + ".");
  }
  return best_page_content(page);
}

}  // namespace

RepositoryKnowledgeSourceLoader::RepositoryKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher,
                                                                 std::set<std::string> extensions,
                                                                 std::set<std::string> exclude_directories)
    : fetcher_(fetcher),
      extensions_(std::move(extensions)),
      exclude_directories_(std::move(exclude_directories)) {}

void RepositoryKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool RepositoryKnowledgeSourceLoader::supports(const Value& source) const {
  const auto type = source.at("type").as_string();
  return type == "repository" || type == "github";
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load(const Value& source) const {
  const auto type = source.at("type").as_string();
  if (type == "repository") {
    return load_local_repository(source);
  }
  if (type == "github") {
    return load_github_repository(source);
  }
  return {};
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load_local_repository(const Value& source) const {
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Repository knowledge source requires path.");
  }
  const auto repository_root = std::filesystem::absolute(std::filesystem::path(raw_path)).lexically_normal();
  if (!std::filesystem::is_directory(repository_root)) {
    throw ConfigurationError("Path is not a repository directory: " + repository_root.string());
  }

  const auto include_extensions = string_set_from_value(source.at("extensions"),
                                                        extensions_.empty() ? default_repository_extensions()
                                                                            : extensions_);
  std::set<std::string> exclude_directories = default_knowledge_exclude_directories();
  exclude_directories.insert(exclude_directories_.begin(), exclude_directories_.end());
  const auto source_exclude_directories = string_set_preserve_from_value(source.at("excludeDirectories"));
  exclude_directories.insert(source_exclude_directories.begin(), source_exclude_directories.end());
  const bool recursive = !source.contains("recursive") || source.at("recursive").as_bool(true);
  const auto include_patterns = string_vector_from_value(source.at("include"));
  const auto exclude_patterns = string_vector_from_value(source.at("exclude"));

  std::vector<std::filesystem::path> files;
  const auto should_include_file = [&](const std::filesystem::path& file) {
    const auto relative_path = normalize_repository_path(std::filesystem::relative(file, repository_root));
    if (!include_extensions.contains(extension_for_path(file))) {
      return false;
    }
    if (!repository_path_matches_patterns(relative_path, include_patterns)) {
      return false;
    }
    return exclude_patterns.empty() || !repository_path_matches_patterns(relative_path, exclude_patterns);
  };

  if (recursive) {
    std::filesystem::recursive_directory_iterator it(repository_root);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
      if (it->is_directory() && exclude_directories.contains(it->path().filename().string())) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file() && should_include_file(it->path())) {
        files.push_back(std::filesystem::absolute(it->path()).lexically_normal());
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(repository_root)) {
      if (entry.is_regular_file() && should_include_file(entry.path())) {
        files.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
      }
    }
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& file : files) {
    const auto relative_path = normalize_repository_path(std::filesystem::relative(file, repository_root));
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["repositoryRoot"] = repository_root.string();
    metadata["repositoryPath"] = relative_path;
    metadata["loader"] = "repository";
    documents.push_back(build_loaded_document("repository",
                                              file.string(),
                                              file.filename().string(),
                                              bytes_to_text(read_binary_file(file)),
                                              std::move(metadata)));
  }
  return documents;
}

std::vector<LoadedKnowledgeDocument> RepositoryKnowledgeSourceLoader::load_github_repository(const Value& source) const {
  const auto source_url = source.at("url").as_string();
  if (source_url.empty()) {
    throw ConfigurationError("GitHub knowledge source requires url.");
  }
  if (!fetcher_) {
    throw ConfigurationError("GitHub knowledge loader requires a NativeWebPageFetcher.");
  }

  const auto parsed = parse_github_repository_url(source_url);
  const auto repo_json = parse_json(github_fetch_text(*fetcher_,
                                                      "https://api.github.com/repos/" + parsed.owner + "/" + parsed.repo,
                                                      source,
                                                      "GitHub API request"));
  const auto configured_ref = source.at("ref").as_string();
  const auto ref = !configured_ref.empty()
                       ? configured_ref
                       : (!parsed.ref.empty() ? parsed.ref : repo_json.at("default_branch").as_string("main"));
  const auto encoded_ref = encode_uri_component(ref);
  const auto tree_json = parse_json(github_fetch_text(
      *fetcher_,
      "https://api.github.com/repos/" + parsed.owner + "/" + parsed.repo + "/git/trees/" + encoded_ref +
          "?recursive=1",
      source,
      "GitHub API request"));

  const auto include_extensions = string_set_from_value(source.at("extensions"),
                                                        extensions_.empty() ? default_repository_extensions()
                                                                            : extensions_);
  const auto include_patterns = string_vector_from_value(source.at("include"));
  const auto exclude_patterns = string_vector_from_value(source.at("exclude"));
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& entry : tree_json.at("tree").as_array()) {
    if (entry.at("type").as_string() != "blob") {
      continue;
    }
    const auto relative_path = normalize_repository_path(entry.at("path").as_string());
    if (relative_path.empty()) {
      continue;
    }
    if (!parsed.root_path.empty() && relative_path.rfind(parsed.root_path, 0) != 0) {
      continue;
    }
    if (!include_extensions.contains(extension_for_path(relative_path))) {
      continue;
    }
    if (!repository_path_matches_patterns(relative_path, include_patterns)) {
      continue;
    }
    if (!exclude_patterns.empty() && repository_path_matches_patterns(relative_path, exclude_patterns)) {
      continue;
    }

    const auto raw_url = "https://raw.githubusercontent.com/" + parsed.owner + "/" + parsed.repo + "/" +
                         encoded_ref + "/" + relative_path;
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["repositoryOwner"] = parsed.owner;
    metadata["repositoryName"] = parsed.repo;
    metadata["repositoryRef"] = ref;
    metadata["repositoryPath"] = relative_path;
    metadata["loader"] = "repository";
    documents.push_back(build_loaded_document("github",
                                              "github://" + parsed.owner + "/" + parsed.repo + "/" + relative_path,
                                              std::filesystem::path(relative_path).filename().string(),
                                              github_fetch_text(*fetcher_, raw_url, source, "GitHub raw file request"),
                                              std::move(metadata)));
  }
  return documents;
}

WebKnowledgeSourceLoader::WebKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher) : fetcher_(fetcher) {}

void WebKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool WebKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "web";
}

std::vector<LoadedKnowledgeDocument> WebKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!fetcher_) {
    throw ConfigurationError("Web knowledge loader requires a NativeWebPageFetcher.");
  }
  const auto url = source.at("url").as_string();
  if (url.empty()) {
    throw ConfigurationError("Web knowledge source requires url.");
  }
  const auto page = fetcher_->fetch(WebFetchRequest{.url = url, .extract = "html"});
  if (page.status >= 400) {
    throw ConfigurationError("Failed to fetch web source: " + url);
  }
  const auto uri = best_page_url(page);
  return {build_loaded_document("web",
                                url,
                                source.at("title").as_string(page.title.empty() ? uri : page.title),
                                best_page_content(page),
                                copy_object_or_empty(source.at("metadata")))};
}

WebsiteKnowledgeSourceLoader::WebsiteKnowledgeSourceLoader(const NativeWebCrawler* crawler,
                                                           BrowserRenderer* browser)
    : crawler_(crawler), browser_(browser) {}

void WebsiteKnowledgeSourceLoader::set_crawler(const NativeWebCrawler* crawler) {
  crawler_ = crawler;
}

void WebsiteKnowledgeSourceLoader::set_browser(BrowserRenderer* browser) {
  browser_ = browser;
}

bool WebsiteKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "website";
}

std::vector<LoadedKnowledgeDocument> WebsiteKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!crawler_) {
    throw ConfigurationError("Website knowledge loader requires a NativeWebCrawler.");
  }
  const auto url = source.at("url").as_string();
  if (url.empty()) {
    throw ConfigurationError("Website knowledge source requires url.");
  }

  WebCrawlRequest request;
  request.url = url;
  if (source.at("maxDepth").is_number()) {
    request.max_depth = static_cast<std::size_t>(std::max<long long>(0, source.at("maxDepth").as_integer()));
  }
  if (source.at("maxPages").is_number()) {
    request.max_pages = static_cast<std::size_t>(std::max<long long>(0, source.at("maxPages").as_integer()));
  }
  const auto crawled = crawler_->crawl(request);

  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& crawled_page : crawled.pages) {
    std::optional<BrowserRenderResult> rendered;
    if (source.at("render").as_bool(false) && browser_) {
      BrowserRenderRequest render_request;
      render_request.url = crawled_page.page.url.empty() ? best_page_url(crawled_page.page) : crawled_page.page.url;
      rendered = browser_->render(render_request);
    }
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["links"] = string_array_value(crawled_page.links);
    metadata["crawledFrom"] = url;
    const auto uri = best_page_url(crawled_page.page);
    documents.push_back(build_loaded_document(
        "website",
        uri,
        rendered && !rendered->title.empty()
            ? rendered->title
            : (crawled_page.page.title.empty() ? source.at("title").as_string(uri) : crawled_page.page.title),
        rendered && !rendered->text.empty() ? rendered->text : best_page_content(crawled_page.page),
        std::move(metadata)));
  }
  return documents;
}

SitemapKnowledgeSourceLoader::SitemapKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher,
                                                           std::optional<std::size_t> default_limit)
    : fetcher_(fetcher), default_limit_(default_limit) {}

void SitemapKnowledgeSourceLoader::set_fetcher(const NativeWebPageFetcher* fetcher) {
  fetcher_ = fetcher;
}

bool SitemapKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "sitemap";
}

std::vector<LoadedKnowledgeDocument> SitemapKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  if (!fetcher_) {
    throw ConfigurationError("Sitemap knowledge loader requires a NativeWebPageFetcher.");
  }
  const auto sitemap_url = source.at("url").as_string();
  if (sitemap_url.empty()) {
    throw ConfigurationError("Sitemap knowledge source requires url.");
  }

  const auto sitemap = fetcher_->fetch(WebFetchRequest{.url = sitemap_url, .extract = "text"});
  const auto xml = !sitemap.text.empty() ? sitemap.text : sitemap.html;
  auto locations = extract_sitemap_locations(xml);
  auto limit = default_limit_;
  if (auto source_limit = optional_size_option_from_value(source, "limit")) {
    limit = source_limit;
  }
  if (limit && locations.size() > *limit) {
    locations.resize(*limit);
  }

  WebKnowledgeSourceLoader web_loader(fetcher_);
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& location : locations) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["loader"] = "sitemap";
    metadata["sitemap"] = sitemap_url;
    auto loaded = web_loader.load(Value::object({{"type", "web"},
                                                {"url", location},
                                                {"metadata", metadata}}));
    for (auto& document : loaded) {
      document.source_type = "sitemap";
      documents.push_back(std::move(document));
    }
  }
  return documents;
}

std::shared_ptr<CompositeKnowledgeLoader> create_web_enabled_knowledge_loader(
    const NativeWebPageFetcher* fetcher,
    const NativeWebCrawler* crawler,
    BrowserRenderer* browser) {
  auto loader = create_default_knowledge_loader();
  loader->add_loader(std::make_shared<RepositoryKnowledgeSourceLoader>(fetcher));
  if (fetcher) {
    loader->add_loader(std::make_shared<WebKnowledgeSourceLoader>(fetcher));
    loader->add_loader(std::make_shared<SitemapKnowledgeSourceLoader>(fetcher));
  }
  if (crawler) {
    loader->add_loader(std::make_shared<WebsiteKnowledgeSourceLoader>(crawler, browser));
  }
  return loader;
}

KnowledgeLoaderRegistry create_web_enabled_knowledge_loader_registry(const NativeWebPageFetcher* fetcher,
                                                                     const NativeWebCrawler* crawler,
                                                                     BrowserRenderer* browser) {
  auto registry = create_default_knowledge_loader_registry();
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "repository",
          .tier = "portable",
          .title = "Repository Loader",
          .description = "Loads local repositories and GitHub repository URLs.",
          .tags = {"loader", "repository", "github", "code"},
      },
      .create = [fetcher](const Value& options, const KnowledgeLoaderRegistry&) {
        return std::make_shared<RepositoryKnowledgeSourceLoader>(
            fetcher,
            string_set_from_value(options.at("extensions")),
            string_set_preserve_from_value(options.at("excludeDirectories")));
      },
  });
  if (fetcher) {
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "web",
            .tier = "portable",
            .title = "Web Loader",
            .tags = {"loader", "web"},
        },
        .create = [fetcher](const Value&, const KnowledgeLoaderRegistry&) {
          return std::make_shared<WebKnowledgeSourceLoader>(fetcher);
        },
    });
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "sitemap",
            .tier = "portable",
            .title = "Sitemap Loader",
            .description = "Expands sitemap XML into multiple web documents.",
            .tags = {"loader", "sitemap", "web"},
        },
        .create = [fetcher](const Value& options, const KnowledgeLoaderRegistry&) {
          return std::make_shared<SitemapKnowledgeSourceLoader>(
              fetcher,
              optional_size_option_from_value(options, "limit"));
        },
    });
  }
  if (crawler) {
    registry.register_provider(KnowledgeLoaderProvider{
        .metadata = KnowledgeProviderMetadata{
            .name = "website",
            .tier = "portable",
            .title = "Website Loader",
            .description = "Site-level ingest loader backed by crawler and optional browser rendering.",
            .tags = {"loader", "website", "crawl"},
        },
        .create = [crawler, browser](const Value&, const KnowledgeLoaderRegistry&) {
          return std::make_shared<WebsiteKnowledgeSourceLoader>(crawler, browser);
        },
    });
  }
  return registry;
}

}  // namespace agent
