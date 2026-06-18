#pragma once

#include "agent/knowledge_loaders.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

class KnowledgeReranker {
 public:
  virtual ~KnowledgeReranker() = default;
  [[nodiscard]] virtual std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const = 0;
};

class HeuristicKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class BasicKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class OverlapKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

struct HybridKnowledgeRerankerConfig {
  double vector_weight = 0.55;
  double lexical_weight = 0.35;
  double image_asset_bias = 0.05;
  double ocr_boost = 0.05;
};

class HybridKnowledgeReranker : public KnowledgeReranker {
 public:
  explicit HybridKnowledgeReranker(HybridKnowledgeRerankerConfig config = HybridKnowledgeRerankerConfig{});
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;

 private:
  HybridKnowledgeRerankerConfig config_;
};

class RecencyKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class MmrKnowledgeReranker : public KnowledgeReranker {
 public:
  explicit MmrKnowledgeReranker(double lambda = 0.75);
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;

 private:
  double lambda_ = 0.75;
};

class KnowledgeRerankerRegistry;

struct KnowledgeRerankerProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeReranker>(
      const Value& options,
      const KnowledgeRerankerRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeRerankerRegistry {
 public:
  explicit KnowledgeRerankerRegistry(
      std::map<std::string, std::shared_ptr<KnowledgeReranker>> rerankers = {});
  explicit KnowledgeRerankerRegistry(std::vector<KnowledgeRerankerProvider> providers);
  KnowledgeRerankerRegistry(const KnowledgeRerankerRegistry& other);
  KnowledgeRerankerRegistry& operator=(const KnowledgeRerankerRegistry& other);
  KnowledgeRerankerRegistry(KnowledgeRerankerRegistry&& other) noexcept;
  KnowledgeRerankerRegistry& operator=(KnowledgeRerankerRegistry&& other) noexcept;
  KnowledgeRerankerProvider& register_provider(KnowledgeRerankerProvider provider);
  void register_reranker(std::string name, std::shared_ptr<KnowledgeReranker> reranker);
  [[nodiscard]] const KnowledgeRerankerProvider* provider(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeReranker> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::shared_ptr<KnowledgeReranker> get(const std::string& name) const;
  [[nodiscard]] std::vector<KnowledgeRerankerProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeRerankerProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeRerankerRegistry create_default_knowledge_reranker_registry();

}  // namespace agent
