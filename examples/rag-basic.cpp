// Example: a minimal RAG flow using the in-memory KnowledgeBase.
//
// Demonstrates:
//   - Ingesting two short documents into an in-memory knowledge base.
//   - Searching with a free-text query and inspecting hit scores.
//   - Wiring the knowledge base into AgentRunnerConfig so retrieved hits
//     ride along into the run context automatically.

#include "agent/agent.hpp"
#include "agent/knowledge.hpp"

#include <iostream>
#include <memory>

int main() {
  agent::KnowledgeBase kb("docs", "default", "Example knowledge base");

  std::vector<agent::LoadedKnowledgeDocument> documents = {
      {
          .uri = "doc://octopus",
          .title = "Octopus facts",
          .content =
              "Octopuses have three hearts and blue blood. They can solve simple puzzles.",
      },
      {
          .uri = "doc://cuttlefish",
          .title = "Cuttlefish facts",
          .content = "Cuttlefish change skin color and texture for camouflage and communication.",
      },
  };
  auto ingest_result = kb.ingest_loaded_documents(documents);
  std::cout << "[ingest] documents=" << ingest_result.document_count
            << " chunks=" << ingest_result.chunk_count << "\n";

  auto hits = kb.search("how many hearts does an octopus have?",
                        agent::KnowledgeSearchOptions{.top_k = 2});
  std::cout << "[search] hits=" << hits.size() << "\n";
  for (const auto& hit : hits) {
    std::cout << "  - uri=" << hit.document.uri
              << " score=" << hit.score
              << " snippet=\"" << hit.chunk.content.substr(0, 60) << "...\"\n";
  }

  agent::AgentRunnerConfig config;
  config.model_runtime.adapter = std::make_shared<agent::EchoChatModelAdapter>();
  config.knowledge_runtime.provider = &kb;
  agent::AgentRunner runner(std::move(config));

  // The runner will automatically retrieve from the knowledge base before
  // calling the model and expose the hits in the result.
  auto result = runner.execution().run("octopus hearts?", "rag-session");
  std::cout << "[runner] knowledge_hits=" << result.knowledge_hits.size()
            << " text=" << result.text << "\n";
  return 0;
}
