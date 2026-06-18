#include "agent/agent.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

#define EMBEDDED_SMOKE_CHECK(condition)                                                  \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      throw std::runtime_error(std::string("EMBEDDED_SMOKE_CHECK failed: ") + #condition); \
    }                                                                                    \
  } while (false)

class StubLongTermMemory final : public agent::LongTermMemoryPort {
 public:
  agent::LongTermMemoryContextResult build_context_message(
      const std::string& query,
      const agent::SearchMemoryOptions&,
      agent::CancellationToken*) override {
    return agent::LongTermMemoryContextResult{
        .hits = {agent::RetrievedMemory{
            .id = "memory-1",
            .content = "remembered " + query,
            .score = 1.0,
        }},
        .message = agent::create_message(agent::MessageRole::System, "memory context"),
    };
  }

  bool auto_remember() const noexcept override {
    return false;
  }

  void remember_conversation_turn(const agent::LongTermMemoryWritebackInput&) override {}
};

class StubKnowledgeProvider final : public agent::KnowledgeContextProvider {
 public:
  std::string knowledge_context_provider_name() const override {
    return "stub-knowledge";
  }

  agent::KnowledgeContextResult build_context_message(
      const std::string& query,
      agent::KnowledgeSearchOptions) override {
    agent::KnowledgeSearchHit hit;
    hit.document.id = "doc-1";
    hit.document.title = "Stub document";
    hit.chunk.id = "chunk-1";
    hit.chunk.content = "knowledge " + query;
    hit.citation.title = hit.document.title;
    hit.citation.snippet = hit.chunk.content;
    hit.score = 1.0;
    return agent::KnowledgeContextResult{
        .hits = {hit},
        .message = agent::create_message(agent::MessageRole::System, "knowledge context"),
    };
  }

  agent::KnowledgeContextResult build_context_message(
      const agent::ImageEmbeddingInput&,
      agent::KnowledgeSearchOptions) override {
    return {};
  }
};

int main() {
  auto runner = agent::AgentRuntimeBuilder()
                    .model(std::make_shared<agent::EchoChatModelAdapter>())
                    .max_iterations(1)
                    .build();

  const auto result = runner.execution().run("hello from embedded runtime", "embedded-session");
  EMBEDDED_SMOKE_CHECK(result.session_id == "embedded-session");
  EMBEDDED_SMOKE_CHECK(result.iteration_count == 1);
  EMBEDDED_SMOKE_CHECK(result.text.find("hello from embedded runtime") != std::string::npos);

  std::size_t event_count = 0;
  const auto stream = runner.streaming().stream(
      "embedded stream",
      [&](const agent::AgentRunnerStreamEvent&) {
        ++event_count;
      },
      "embedded-session");
  EMBEDDED_SMOKE_CHECK(event_count > 0);
  EMBEDDED_SMOKE_CHECK(stream.result.text.find("embedded stream") != std::string::npos);

  StubKnowledgeProvider knowledge;
  auto memory = std::make_shared<StubLongTermMemory>();
  auto port_runner = agent::AgentRuntimeBuilder()
                         .model(std::make_shared<agent::EchoChatModelAdapter>())
                         .long_term_memory(memory)
                         .knowledge(&knowledge)
                         .max_iterations(1)
                         .build();
  const auto port_result = port_runner.execution().run("embedded ports", "embedded-port-session");
  EMBEDDED_SMOKE_CHECK(port_result.memory_hits.size() == 1);
  EMBEDDED_SMOKE_CHECK(port_result.knowledge_hits.size() == 1);

  std::cout << "agent_native_embedded_smoke OK\n";
  return 0;
}
