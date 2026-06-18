#pragma once

#include "agent/knowledge_runtime.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct LoadedKnowledgeDocument {
  std::string uri;
  std::string title;
  std::string content;
  std::string source_type = "manual";
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::optional<MediaSource> media;
  std::string text_hint;
  std::string alt_text;
  std::string ocr_text;
  std::string caption;
  Value metadata = Value::object({});
};

class KnowledgeSourceLoader {
 public:
  virtual ~KnowledgeSourceLoader() = default;
  [[nodiscard]] virtual bool supports(const Value& source) const = 0;
  [[nodiscard]] virtual std::vector<LoadedKnowledgeDocument> load(const Value& source) const = 0;
};

struct KnowledgeProviderMetadata {
  std::string name;
  std::string tier = "core-safe";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
};

class KnowledgeLoaderRegistry;

struct KnowledgeLoaderProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeSourceLoader>(
      const Value& options,
      const KnowledgeLoaderRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeLoaderRegistry {
 public:
  explicit KnowledgeLoaderRegistry(std::vector<KnowledgeLoaderProvider> providers = {});
  KnowledgeLoaderRegistry(const KnowledgeLoaderRegistry& other);
  KnowledgeLoaderRegistry& operator=(const KnowledgeLoaderRegistry& other);
  KnowledgeLoaderRegistry(KnowledgeLoaderRegistry&& other) noexcept;
  KnowledgeLoaderRegistry& operator=(KnowledgeLoaderRegistry&& other) noexcept;
  KnowledgeLoaderProvider& register_provider(KnowledgeLoaderProvider provider);
  [[nodiscard]] const KnowledgeLoaderProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeSourceLoader> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeLoaderProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeLoaderProvider> providers_;
  std::vector<std::string> provider_order_;
};

class TextKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;
};

class FileKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;
};

class DirectoryKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit DirectoryKnowledgeSourceLoader(std::shared_ptr<FileKnowledgeSourceLoader> file_loader =
                                              std::make_shared<FileKnowledgeSourceLoader>());
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::shared_ptr<FileKnowledgeSourceLoader> file_loader_;
};

class MarkdownKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit MarkdownKnowledgeSourceLoader(bool recursive = true);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  bool recursive_;
};

class CompositeKnowledgeLoader : public KnowledgeSourceLoader {
 public:
  explicit CompositeKnowledgeLoader(std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders = {},
                                    bool use_default_loaders_when_empty = true);
  void add_loader(std::shared_ptr<KnowledgeSourceLoader> loader);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders_;
};

std::vector<LoadedKnowledgeDocument> load_knowledge_sources(
    const std::vector<Value>& sources,
    const KnowledgeSourceLoader& loader);

std::shared_ptr<CompositeKnowledgeLoader> create_default_knowledge_loader();

KnowledgeLoaderRegistry create_default_knowledge_loader_registry();

}  // namespace agent
