// Example: a minimal eval suite.
//
// Demonstrates:
//   - Building an EvalSuite with two cases.
//   - Using expect_contains for a basic substring assertion and a custom
//     EvalAssertion for assertions that need to inspect the run result.
//   - Running the suite against an echo-model AgentRunner and printing the
//     JSON report.
//
// Echo model returns the input verbatim, so each case asserts that the
// output mentions a chosen phrase.

#include "agent/app_api.hpp"

#include <iostream>
#include <memory>

int main() {
  agent::AgentRunnerConfig config;
  config.model_runtime.adapter = std::make_shared<agent::EchoChatModelAdapter>();
  config.context_runtime.max_iterations = 1;
  agent::AgentRunner runner(std::move(config));

  agent::EvalSuite suite;
  suite.agent = "demo";

  agent::EvalCase greeting;
  greeting.id = "greeting";
  greeting.input = "Hello evaluator!";
  greeting.expect_contains = {"evaluator"};
  suite.cases.push_back(std::move(greeting));

  agent::EvalCase length;
  length.id = "length";
  length.input = "octopus";
  length.min_output_length = 5;
  length.expect.push_back(agent::EvalAssertion{
      .name = "uses-input",
      .evaluate = [](const agent::EvalAssertionContext& ctx) {
        agent::EvalAssertionResult result;
        result.name = "uses-input";
        const bool ok = ctx.result != nullptr &&
                        ctx.result->text.find("octopus") != std::string::npos;
        result.passed = ok;
        result.message = ok ? "output mentions the input" : "output ignored the input";
        return result;
      },
  });
  suite.cases.push_back(std::move(length));

  auto report = agent::run_eval_suite(suite, runner);
  std::cout << "[eval] cases=" << report.total_cases
            << " passed=" << report.passed_cases
            << " failed=" << report.failed_cases << "\n";
  for (const auto& case_result : report.results) {
    std::cout << "  - " << case_result.id
              << " passed=" << (case_result.passed ? "true" : "false")
              << " output=\"" << case_result.output << "\"\n";
  }
  return 0;
}
