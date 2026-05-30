#include "agent/knowledge.hpp"

#include "agent/execution.hpp"

#include <iomanip>
#include <sstream>

namespace agent {

namespace {

void throw_if_knowledge_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Retrieval);
  }
}

}  // namespace

KnowledgeIngestionResult DefaultKnowledgeIngestionStrategy::ingest(
    const KnowledgeIngestionStrategyContext& context) const {
  throw_if_knowledge_cancelled(context.options.cancellation);
  auto documents = load_knowledge_sources(context.sources, context.loader);
  throw_if_knowledge_cancelled(context.options.cancellation);
  return context.knowledge_base.ingest_loaded_documents(documents, context.options);
}

std::optional<AgentMessage> DefaultKnowledgeContextRenderer::render(
    const KnowledgeContextRenderContext& context) const {
  if (context.hits.empty()) {
    return std::nullopt;
  }

  std::ostringstream content;
  content << context.knowledge_base.context_title();
  for (std::size_t index = 0; index < context.hits.size(); ++index) {
    content << "\n" << index + 1 << ". [" << context.hits[index].citation.title << "] "
            << context.hits[index].citation.snippet << " (score=" << std::fixed << std::setprecision(3)
            << context.hits[index].score << ")";
  }

  std::vector<MessageContentPart> parts{text_part(content.str())};
  for (const auto& hit : context.hits) {
    if (hit.citation.asset_type == KnowledgeAssetType::Image && hit.citation.media) {
      parts.push_back(image_part(*hit.citation.media,
                                 hit.citation.title,
                                 "auto",
                                 Value::object({{"source", "knowledge-base"}, {"uri", hit.citation.uri}})));
    }
  }

  return create_message(MessageRole::System,
                        parts,
                        Value::object({{"source", "knowledge-base"},
                                      {"knowledgeBaseId", context.knowledge_base.id()},
                                      {"hits", context.hits.size()}}));
}

}  // namespace agent

