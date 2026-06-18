// Example: registering a custom tool + a permission policy.
//
// Demonstrates:
//   - Defining a tool with an input schema and an inline execute lambda.
//   - Wiring it onto AgentRunnerConfig::tool_runtime.definitions.
//   - Constructing a capability-based permission policy and observing how
//     the executor wraps denials in a failed ToolExecutionResult.
//   - Driving a tool call directly through the runner's ToolExecutor.

#include "agent/agent.hpp"

#include <iostream>
#include <memory>

int main() {
  // Define a trivial "echo" tool that returns its input verbatim.
  agent::JsonSchema input_schema;
  input_schema.type = agent::JsonSchemaType::Object;
  input_schema.properties["text"].type = agent::JsonSchemaType::String;
  input_schema.required = {"text"};

  auto echo_tool = agent::define_tool(agent::ToolDefinition{
      .name = "demo.echo",
      .description = "Return the input text verbatim.",
      .input_schema = input_schema,
      .execute = [](const agent::Value& arguments,
                    agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
        return agent::Value::object({{"echoed", arguments.at("text")}});
      },
  });

  agent::AgentRunnerConfig config;
  config.model_runtime.adapter = std::make_shared<agent::EchoChatModelAdapter>();
  config.tool_runtime.definitions = std::vector<agent::ToolDefinition>{echo_tool};
  config.tool_runtime.permission_policy = agent::create_capability_policy(
      agent::CapabilityPermissionPolicyConfig{
          .allow = {"demo.echo"},
          .deny = {"shell.*"},
      });
  agent::AgentRunner runner(std::move(config));

  // The runner's session memory keeps tool results around even when the model
  // doesn't choose to call them; here we drive the tool directly to make the
  // example concrete without needing a tool-using model.
  agent::ToolRegistry registry({echo_tool});
  agent::ToolExecutor executor(registry, agent::create_capability_policy(
                                             agent::CapabilityPermissionPolicyConfig{
                                                 .allow = {"demo.echo"},
                                                 .deny = {"shell.*"},
                                             }));

  auto allowed = executor.execute_tool_call(agent::ToolCall{
      .id = "call-1",
      .name = "demo.echo",
      .arguments = agent::Value::object({{"text", "hi"}}),
  });
  std::cout << "[allowed] ok=" << (allowed.ok ? "true" : "false")
            << " output=" << allowed.output << "\n";

  auto denied = executor.execute_tool_call(agent::ToolCall{
      .id = "call-2",
      .name = "shell.exec",
      .arguments = agent::Value::object({{"cmd", "rm -rf /"}}),
  });
  std::cout << "[denied] ok=" << (denied.ok ? "true" : "false")
            << " output=" << denied.output << "\n";

  // The runner itself is unchanged — the permission policy is enforced by the
  // executor that the runner constructs internally too.
  auto result = runner.execution().run("Plan whatever you like.", "tools-session");
  std::cout << "[runner] " << result.text << "\n";
  return 0;
}
