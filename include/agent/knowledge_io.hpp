#pragma once

#include "agent/knowledge_core.hpp"

#include <optional>
#include <set>

namespace agent {

class BrowserRenderer;
class NativeWebCrawler;
class NativeWebPageFetcher;

class RepositoryKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit RepositoryKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr,
                                           std::set<std::string> extensions = {},
                                           std::set<std::string> exclude_directories = {});
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::vector<LoadedKnowledgeDocument> load_local_repository(const Value& source) const;
  std::vector<LoadedKnowledgeDocument> load_github_repository(const Value& source) const;

  const NativeWebPageFetcher* fetcher_;
  std::set<std::string> extensions_;
  std::set<std::string> exclude_directories_;
};

class WebKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit WebKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr);
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebPageFetcher* fetcher_;
};

class WebsiteKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit WebsiteKnowledgeSourceLoader(const NativeWebCrawler* crawler = nullptr,
                                        BrowserRenderer* browser = nullptr);
  void set_crawler(const NativeWebCrawler* crawler);
  void set_browser(BrowserRenderer* browser);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebCrawler* crawler_;
  BrowserRenderer* browser_;
};

class SitemapKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit SitemapKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr,
                                        std::optional<std::size_t> default_limit = std::nullopt);
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebPageFetcher* fetcher_;
  std::optional<std::size_t> default_limit_;
};

std::shared_ptr<CompositeKnowledgeLoader> create_web_enabled_knowledge_loader(
    const NativeWebPageFetcher* fetcher,
    const NativeWebCrawler* crawler = nullptr,
    BrowserRenderer* browser = nullptr);

KnowledgeLoaderRegistry create_web_enabled_knowledge_loader_registry(
    const NativeWebPageFetcher* fetcher,
    const NativeWebCrawler* crawler = nullptr,
    BrowserRenderer* browser = nullptr);

}  // namespace agent
