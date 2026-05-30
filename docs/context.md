# Context And Planning API

The native context module contains two public surfaces: embedded runtime context
blocks and execution planning. Both are declared in `agent/context.hpp` and are
included by the umbrella `agent/agent.hpp` header.

## Embedded Context

`EmbeddedContextManager` collects context sources and renders their output as a
system message for a run:

```cpp
agent::EmbeddedContextManager context;
context.register_source(agent::ContextSource{
    .id = "tenant-policy",
    .title = "Tenant Policy",
    .resolve = [](const agent::Value& runtime) {
      return std::vector<agent::EmbeddedContextBlock>{
          agent::EmbeddedContextBlock{
              .content = "Use the tenant policy attached to this session.",
              .priority = 100,
          },
      };
    },
    .priority = 100,
});

auto message = context.build_message(
    agent::Value::object({{"tenantId", "acme"}}));
```

`ContextSource` fields:

- `id`: required stable source identifier.
- `title`: optional display title; defaults to `id`.
- `resolve`: required callback that receives the runtime `Value`.
- `priority`: source priority used for ordering.

`EmbeddedContextBlock` fields:

- `title`: optional block title; defaults to the source title when empty.
- `content`: block body. Empty content is skipped.
- `priority`: block priority; defaults to the source priority when zero.

`register_source` validates that `id` and `resolve` are present. Sources and
resolved blocks are sorted by descending priority. `resolve_blocks` returns the
raw blocks, while `build_message` returns a system `AgentMessage` with
`metadata.source = "embedded-context"` and the rendered block titles.

## Planner Interface

Planning is represented by `Planner`:

```cpp
class MyPlanner final : public agent::Planner {
 public:
  std::optional<agent::ExecutionPlan> plan(
      const agent::PlannerParams& params) override {
    return agent::ExecutionPlan{
        .goal = params.input,
        .steps = {
            agent::PlanStep{
                .id = "step_1",
                .title = "Answer",
                .description = "Use available context and tools.",
            },
        },
    };
  }
};
```

`PlannerParams` passes the user input, optional session memory, arbitrary
context `Value`, optional tool registry, memory hits, knowledge hits, and an
optional cancellation token. Planners should check the token when doing
long-running work.

`ExecutionPlan` contains:

- `goal`
- `steps`
- `notes`
- `updated_at`

Each `PlanStep` contains an id, title, description, optional tool name, and
dependency ids.

## Built-In Planners

`StaticPlanner` can either return a fixed plan or call a `PlannerHandler`.
Empty plans are normalized to `std::nullopt`, which lets runners skip plan
injection when planning has nothing useful to add.

`ModelPlanner` uses a configured `ChatModelAdapter` to generate plan JSON:

```cpp
auto planner = std::make_shared<agent::ModelPlanner>(
    agent::ModelPlannerConfig{
        .model = planning_model,
        .max_steps = 4,
    });
```

The model prompt includes available tools, relevant memory hits, knowledge hits,
and session summary text. The model adapter is required and receives the same
`CancellationToken*` from `PlannerParams`.

## Plan Serialization

Helper functions keep the C++ representation compatible with the NodeJS plan
shape:

- `normalize_execution_plan`
- `execution_plan_to_value`
- `parse_execution_plan_text`
- `render_execution_plan`
- `create_plan_message`

`normalize_execution_plan` accepts both `toolName` and `tool_name`, and both
`dependsOn` and `depends_on`. Missing step ids are generated as `step_1`,
`step_2`, and so on. Missing `updatedAt` is filled with the current timestamp.

`parse_execution_plan_text` accepts raw JSON or a fenced `json` code block and
returns `std::nullopt` on invalid JSON or empty steps.

## Zero-Dependency Boundary

The context module depends on framework memory, tool, message, and model
interfaces only. It does not embed a JavaScript runtime, planner service SDK,
database client, or external JSON parser.
