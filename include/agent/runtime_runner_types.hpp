#pragma once

#include "agent/context.hpp"
#include "agent/execution.hpp"
#include "agent/knowledge_runtime.hpp"
#include "agent/memory_retrieval.hpp"
#include "agent/runtime_loop.hpp"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct RunnerRetrievalOptions {
  std::optional<bool> enabled;
  std::optional<std::size_t> top_k;
  std::optional<double> min_score;
  std::string namespace_id;
};

struct RunnerWritebackOptions {
  std::optional<bool> enabled;
  std::string namespace_id;
  Value metadata = Value::object({});
};

struct RunnerKnowledgeRetrievalOptions {
  bool enabled = true;
  std::size_t top_k = 0;
  std::size_t vector_top_k = 0;
  std::size_t lexical_top_k = 0;
  double min_score = std::numeric_limits<double>::quiet_NaN();
  double hybrid_alpha = std::numeric_limits<double>::quiet_NaN();
  std::size_t rerank_top_k = 0;
  std::string retrieval_mode;
  double oversample_factor = 0.0;
  std::string fusion;
  std::map<KnowledgeAssetType, double> modality_weights;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<KnowledgeAssetType> asset_types;
  std::string space_id;
  std::vector<std::string> source_types;
  std::vector<std::string> chunk_ids;
  Value metadata = Value::object({});
  std::string tenant_id;
  std::vector<std::string> knowledge_base_ids;
};

struct AgentRunnerRunResult : public AgentLoopRunResult {
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  Value knowledge_debug = Value::object({});
  std::optional<ExecutionPlan> plan;
};

}  // namespace agent
