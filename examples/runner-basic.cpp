// Example: AgentRunner — the most basic synchronous run.
//
// Demonstrates:
//   - Constructing AgentRunner with the built-in echo model.
//   - Calling run() with a string input and getting back a final message.
//   - Calling stream() and handling runner events as they are produced.
//
// This file builds with zero external dependencies and is registered as
// CTest target `example_runner_basic`.

#include "agent/agent.hpp"

#include <iostream>
#include <memory>

int main() {
  auto runner = agent::AgentRuntimeBuilder()
                    .model(std::make_shared<agent::EchoChatModelAdapter>())
                    .max_iterations(1)
                    .build();

  // --- Synchronous run -----------------------------------------------------
  auto result = runner.execution().run("hello, agent_native!", "session-1");
  std::cout << "[run] session=" << result.session_id
            << " iterations=" << result.iteration_count
            << " text=" << result.text << "\n";

  // --- Streaming run -------------------------------------------------------
  std::size_t stream_event_count = 0;
  auto stream = runner.streaming().stream("stream this input back to me",
                              [&](const agent::AgentRunnerStreamEvent& event) {
                                ++stream_event_count;
                                std::cout << "  - " << agent::to_string(event.type);
                                if (event.type == agent::AgentRunnerStreamEventType::Status) {
                                  std::cout << " stage=" << event.status.stage
                                            << " state=" << event.status.state;
                                }
                                std::cout << "\n";
                              },
                              "session-1");
  std::cout << "[stream] events=" << stream_event_count << "\n";
  std::cout << "[stream] final text=" << stream.result.text << "\n";

  return 0;
}
