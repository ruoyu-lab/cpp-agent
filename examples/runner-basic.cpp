// Example: AgentRunner — the most basic synchronous run.
//
// Demonstrates:
//   - Constructing AgentRunner with the built-in echo model.
//   - Calling run() with a string input and getting back a final message.
//   - Calling stream() and walking the produced runner event array.
//
// This file builds with zero external dependencies and is registered as
// CTest target `example_runner_basic`.

#include "agent/agent.hpp"

#include <iostream>
#include <memory>

int main() {
  agent::AgentRunnerConfig config;
  config.adapter = std::make_shared<agent::EchoChatModelAdapter>();
  config.max_iterations = 1;

  agent::AgentRunner runner(std::move(config));

  // --- Synchronous run -----------------------------------------------------
  auto result = runner.run("hello, agent_native!", "session-1");
  std::cout << "[run] session=" << result.session_id
            << " iterations=" << result.iteration_count
            << " text=" << result.text << "\n";

  // --- Streaming run -------------------------------------------------------
  auto stream = runner.stream("stream this input back to me", "session-1");
  std::cout << "[stream] events=" << stream.events.size() << "\n";
  for (const auto& event : stream.events) {
    std::cout << "  - " << agent::to_string(event.type);
    if (event.type == agent::AgentRunnerStreamEventType::Status) {
      std::cout << " stage=" << event.status.stage << " state=" << event.status.state;
    }
    std::cout << "\n";
  }
  std::cout << "[stream] final text=" << stream.result.text << "\n";

  return 0;
}
