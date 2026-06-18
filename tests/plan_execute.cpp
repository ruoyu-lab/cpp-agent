#include "agent/agent.hpp"
#include "agent/plan.hpp"

#include <iostream>
#include <stdexcept>

#define PLAN_EXECUTE_CHECK(condition)                                                   \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      throw std::runtime_error(std::string("PLAN_EXECUTE_CHECK failed at ") + __FILE__ \
                               + ":" + std::to_string(__LINE__) + ": " #condition);    \
    }                                                                                   \
  } while (false)

namespace {

class PlanExecuteReactModel final : public agent::ChatModelAdapter {
 public:
  PlanExecuteReactModel() : ChatModelAdapter("fixture", "plan-execute-react", 0.0, 512, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt;
    for (const auto& message : params.messages) {
      prompt += agent::extract_text_content(message.content);
      prompt += "\n";
    }
    prompts_.push_back(prompt);

    if (prompt.find("Current step: Finish report") != std::string::npos) {
      PLAN_EXECUTE_CHECK(prompt.find("Previous completed steps") != std::string::npos);
      PLAN_EXECUTE_CHECK(prompt.find("draft-output") != std::string::npos);
      return build_output(agent::AgentOutput{
          .text = "Thought: use the draft\nFinal Answer: final-output",
      });
    }

    if (prompt.find("Current step: Draft report") != std::string::npos) {
      PLAN_EXECUTE_CHECK(prompt.find("Plan the report") != std::string::npos);
      return build_output(agent::AgentOutput{
          .text = "Thought: write the first pass\nFinal Answer: draft-output",
      });
    }

    return build_output(agent::AgentOutput{
        .text = "Thought: unexpected prompt\nFinal Answer: unexpected",
    });
  }

  [[nodiscard]] const std::vector<std::string>& prompts() const noexcept { return prompts_; }

 private:
  std::vector<std::string> prompts_;
};

agent::ExecutionPlan report_plan() {
  return agent::ExecutionPlan{
      .goal = "Plan the report",
      .steps = {
          agent::PlanStep{
              .id = "draft",
              .title = "Draft report",
              .description = "Write the first report draft.",
          },
          agent::PlanStep{
              .id = "final",
              .title = "Finish report",
              .description = "Finalize the report using the draft.",
              .depends_on = {"draft"},
          },
      },
      .updated_at = "2026-06-02T00:00:00Z",
  };
}

}  // namespace

int main() {
  try {
    int nested_planner_calls = 0;
    auto nested_planner = std::make_shared<agent::StaticPlanner>(
        agent::PlannerHandler([&](const agent::PlannerParams&) -> std::optional<agent::ExecutionPlan> {
          ++nested_planner_calls;
          return report_plan();
        }));
    auto plan_execute_planner = std::make_shared<agent::StaticPlanner>(report_plan());
    auto model = std::make_shared<PlanExecuteReactModel>();
    agent::AgentRunner step_runner(agent::AgentRunnerConfig{
        .model_runtime = {.adapter = model},
        .context_runtime = {.max_iterations = 1},
        .governance = {.planner = nested_planner},
    });
    agent::PlanAndExecuteRunner runner(agent::PlanAndExecuteRunnerConfig{
        .planner = plan_execute_planner,
        .runner = &step_runner,
    });

    const auto result = runner.run("Ship report", agent::PlanAndExecuteRunOptions{
                                                     .run_id = "plan-execute-test",
                                                     .session_id = "plan-execute-session",
                                                 });
    PLAN_EXECUTE_CHECK(result.status == agent::PlanAndExecuteRunStatus::Completed);
    PLAN_EXECUTE_CHECK(result.plan.steps.size() == 2);
    PLAN_EXECUTE_CHECK(result.snapshot.steps.size() == 2);
    PLAN_EXECUTE_CHECK(result.snapshot.steps.front().status == agent::AutonomousStepStatus::Completed);
    PLAN_EXECUTE_CHECK(result.snapshot.steps.back().status == agent::AutonomousStepStatus::Completed);
    PLAN_EXECUTE_CHECK(result.snapshot.steps.front().output.at("text").as_string() == "draft-output");
    if (result.snapshot.steps.back().output.at("text").as_string() != "final-output") {
      throw std::runtime_error("unexpected final step output: " +
                               result.snapshot.steps.back().output.stringify());
    }
    if (result.text != "final-output") {
      throw std::runtime_error("unexpected plan-execute text: " + result.text +
                               " output=" + result.snapshot.steps.back().output.stringify());
    }
    PLAN_EXECUTE_CHECK(result.snapshot.steps.back().depends_on == std::vector<std::string>{"draft"});
    PLAN_EXECUTE_CHECK(nested_planner_calls == 0);
    PLAN_EXECUTE_CHECK(model->prompts().size() == 2);

    bool saw_dependency_error = false;
    auto invalid_planner = std::make_shared<agent::StaticPlanner>(agent::ExecutionPlan{
        .goal = "Invalid",
        .steps = {
            agent::PlanStep{.id = "blocked", .title = "Blocked", .depends_on = {"missing"}},
        },
    });
    agent::PlanAndExecuteRunner invalid_runner(agent::PlanAndExecuteRunnerConfig{
        .planner = invalid_planner,
        .runner = &step_runner,
    });
    try {
      (void)invalid_runner.run("Invalid plan");
    } catch (const agent::PlanAndExecuteError& error) {
      saw_dependency_error = std::string(error.what()).find("depends on unknown step") != std::string::npos;
    }
    PLAN_EXECUTE_CHECK(saw_dependency_error);

    std::cout << "agent_plan_execute_tests OK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "agent_plan_execute_tests failed: " << error.what() << "\n";
    return 1;
  }
}
