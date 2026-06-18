// Example: a minimal workflow with the built-in node handlers.
//
// Demonstrates:
//   - Building a workflow with start -> tool -> end via the WorkflowBuilder.
//   - Registering a custom tool that the workflow's tool node calls.
//   - Running the workflow through WorkflowEngine and reading its output.
//   - Persisting and inspecting the run state through an in-memory store.

#include "agent/agent.hpp"
#include "agent/workflow.hpp"

#include <iostream>

int main() {
  // Custom tool: turn the input text into upper-case to make the run visible.
  auto upper_tool = agent::define_tool(agent::ToolDefinition{
      .name = "demo.upper",
      .description = "Upper-case the input text.",
      .execute = [](const agent::Value& arguments,
                    agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        std::string text = arguments.at("text").as_string();
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
          out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return agent::Value(out);
      },
  });

  agent::ToolRegistry tools({upper_tool});
  agent::ToolExecutor tool_executor(tools);
  agent::WorkflowNodeRegistry nodes = agent::create_default_workflow_node_registry();
  agent::InMemoryWorkflowStore store;

  agent::WorkflowEngine engine(&tools, &tool_executor, &store, &nodes);

  agent::WorkflowBuilder builder("wf_demo", "Workflow Demo");
  auto definition = builder.start()
                        .tool("shout", "demo.upper",
                              {{"text", agent::workflow_literal("hello workflow")}})
                        .end("end", agent::workflow_ref("nodes.shout.output"))
                        .sequence({"start", "shout", "end"})
                        .build();

  auto run = engine.run(definition);
  std::cout << "[run] id=" << run.workflow_run_id
            << " status=" << static_cast<int>(run.status)
            << " output=" << run.output.as_string() << "\n";

  if (auto stored = store.get_run(run.workflow_run_id); stored) {
    std::cout << "[store] visits to 'shout' node = "
              << stored->node_states.at("shout").visits << "\n";
  }
  return 0;
}
