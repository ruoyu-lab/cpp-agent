# Multi-Agent Patterns

This framework supports **six distinct multi-agent patterns** out of the box.
The pieces are spread across `runtime`, `skills`, `workflow`, `orchestration`,
`autonomous`, and `tool_client` modules — which is why they're easy to miss.
This page is the map.

> The NodeJS implementation mirrors every pattern below with the same
> semantics; see [`NodeJS/docs/multi-agent.md`](../../NodeJS/docs/multi-agent.md)
> for the TypeScript-side API names (camelCase) and code samples.

---

## When you DON'T need multi-agent

Before reaching for any of these patterns: **a single `AgentRunner` with a
well-chosen tool set, hooks, and a planner is enough for most tasks**. Adding
agents adds coordination cost, increases token spend, and multiplies failure
modes. Reach for multi-agent only when you have a clear, structural reason:

- The work decomposes into **specialized roles** with different tool/permission profiles
- Different stages need **different model tiers** (e.g., cheap planner + expensive executor)
- You need **failure isolation** between subtasks (one subagent crashing shouldn't poison the rest)
- You need **parallel exploration** with independent contexts (different agents try different approaches)
- You need **cross-process** or cross-machine distribution of agent work

If none of those apply, stay with one runner.

---

## Decision tree

```
What are you trying to do?
│
├─ "Specialize one slice of a task" (e.g. only-read planner, only-write coder)
│   └─▶ Pattern 1: Skill Fork Subagent
│
├─ "Embed agent calls inside a structured flow with conditions and branches"
│   └─▶ Pattern 2: Workflow Agent Node
│
├─ "Implement a known multi-agent shape (supervisor/worker/critic/loop)"
│   └─▶ Pattern 3: Coordinator (pick one of seven)
│
├─ "Agent A on host X needs to call Agent B on host Y"
│   └─▶ Pattern 4: Tool-Client Remote Agent
│
├─ "Loosely coupled agents passing messages, no central orchestrator"
│   └─▶ Pattern 5: Mailbox Messaging
│
└─ "Long-running multi-step job that decides its own next steps over hours/days"
    └─▶ Pattern 6: Autonomous Multi-Step Runner
```

You can also combine patterns — e.g. a Coordinator that uses Workflow nodes
that contain Skill-Fork subagents whose results post to a Mailbox.

---

## Pattern 1: Skill Fork Subagent

**When**: One slice of the task should run with a different tool set / model /
session, then hand a result back to the parent agent. This is the pattern to
use for things like "spawn a read-only planner subagent before executing".

**Primitives**:
- `AgentRunnerConfig.context_runtime.skill_subagents` — `map<name, AgentRunner*>` of named child runners
- `AgentRunnerConfig.context_runtime.skill_fork_handler` — callback that decides when/how to fork
- `SkillForkRequest` / `SkillForkResult` — the bridge contract

**Template — Planner-as-Subagent (the example we keep coming back to)**:

```cpp
// 1. Build the planner subagent — read-only tools, cheap model, no session leak.
AgentRunnerConfig planner_cfg;
planner_cfg.model_runtime.adapter = haiku_adapter();
planner_cfg.tool_runtime.definitions = std::vector<ToolDefinition>{
    create_builtin_tools({"core"})[0],   // tool.search / tool.describe
    fs_read_only_tool,
    knowledge_search_tool,
};
planner_cfg.context_runtime.max_iterations = 4;
planner_cfg.context_runtime.system_prompt =
    "You are a read-only planner. Explore the task with available tools, "
    "then output a JSON plan with steps[].";
auto planner_runner = std::make_shared<AgentRunner>(planner_cfg);

// 2. Wire it onto the main agent under a named skill.
AgentRunnerConfig main_cfg;
main_cfg.model_runtime.adapter = opus_adapter();
main_cfg.tool_runtime.definitions = create_builtin_tools({"core", "local", "developer"});
main_cfg.context_runtime.skill_subagents = {{"plan", planner_runner.get()}};
main_cfg.context_runtime.skill_fork_handler = [](const SkillForkRequest& req)
    -> std::optional<SkillForkResult> {
  if (req.skill.name != "plan") return std::nullopt;
  // The framework will invoke the named subagent with req.input_text;
  // returning nullopt means "use the registered subagent default behaviour".
  return std::nullopt;
};
AgentRunner main_runner(main_cfg);
```

**Isolation guarantees**:
- Subagent has its own `SessionMemory` — parent transcript doesn't bleed in,
  child transcript doesn't bleed out
- Subagent has its own tool registry view — write tools simply aren't there
- Subagent can use a different model — typically cheaper

**Concerns**:
- You write the "what tools count as read-only" logic — framework doesn't
  guess (intentional: business knows its risk profile better than we do)
- Returning structured plans reliably needs a JSON schema or a forced last
  tool call like `submit_plan(...)`

---

## Pattern 2: Workflow Agent Node

**When**: You want a structured DAG of steps with conditions, branches,
human-wait nodes, webhook-wait nodes — and some of those steps should be
agents (not just tool calls or transforms).

**Primitives**:
- `WorkflowAgentRegistry` — register named agents the workflow can call
- `WorkflowAgentDefinition.runner` — the `AgentRunner` to invoke
- `WorkflowEngine` — runs the DAG, dispatches to registered agents

**Template — "validate → planner agent → coder agent → end"**:

```cpp
WorkflowAgentRegistry agents;
agents.register_runner("planner",  planner_runner);
agents.register_runner("coder",    coder_runner);

WorkflowNodeRegistry nodes = create_default_workflow_node_registry();
ToolRegistry tools = ToolRegistry(create_builtin_tools({"core"}));
ToolExecutor executor(tools);

WorkflowEngine engine(WorkflowEngineConfig{
    .node_registry  = &nodes,
    .agent_registry = &agents,
    .tools          = &tools,
    .tool_executor  = &executor,
});

auto wf = create_workflow("refactor")
    .start()
    .agent("plan",  "planner", { {"input", workflow_input("user_request")} })
    .agent("code",  "coder",   { {"plan",  workflow_ref("nodes.plan.output")} })
    .end("done", workflow_ref("nodes.code.output"))
    .sequence({"start", "plan", "code", "done"})
    .build();

auto run = engine.run(wf, Value::object({{"user_request", "Refactor auth"}}));
```

**When to prefer over Pattern 1**: When you need persistent runs (resume from
checkpoint), human-in-the-loop pauses, webhook waits, or fan-out / fan-in
across multiple agents.

**Concerns**:
- Workflow run state is persistable (`WorkflowStore`) — pick `InMemory` for
  short runs, `File` for durability, inject Postgres for production
- Agent nodes get inputs via `workflow_value` / `workflow_ref` plumbing —
  read the workflow doc for the data-flow rules

---

## Pattern 3: Coordinators (seven variants)

**When**: You want to assemble a known multi-agent topology without writing
the bookkeeping yourself. The `orchestration` module gives you seven
ready-made coordinators plus the shared primitives they all use.

### Shared primitives

| Type | Purpose |
|---|---|
| `CoordinatorContext` | Holds `primary_agent`, `worker_agents` map, `workflow_engine`, `artifact_store`, `shared_state`, `mailbox`, and `state` (a `SharedTaskState`). Every coordinator's `run(input, ctx)` takes this. |
| `TaskRouterStrategy` | Decides which worker handles a task. Built-in: `RuleBasedTaskRouter`, `WeightedTaskRouter`, `MailboxTaskRouter`. |
| `EvaluationStrategy` | Scores worker output. Built-in: `ThresholdEvaluationStrategy`, `KeywordEvaluationStrategy`. |
| `ReplanStrategy` | Decides whether to replan and what the new plan should be. Built-in: `RuleBasedReplanStrategy`, `ArtifactAwareReplanStrategy`. |
| `InMemoryArtifactStore` / `InMemorySharedStateStore` / `InMemoryMailbox` | Stateful sidecars for cross-agent data. |

### The seven coordinators

| Coordinator | Topology | When to pick |
|---|---|---|
| **3a. `PlanActObserveCoordinator`** | One primary agent loops `plan → act → observe → replan` for N rounds | Single agent doing iterative refinement, no specialists |
| **3b. `SupervisorWorkerCoordinator`** | Router sends tasks to one of N specialised workers | Heterogeneous specialists, no quality gate |
| **3c. `SupervisorEvaluatorCoordinator`** | Same as 3b but every output goes through an evaluator; retries on fail | When output quality is uncertain and you can afford retries |
| **3d. `EvaluatorLoopCoordinator`** | One agent + one evaluator loop until threshold | Critique-driven self-improvement (GAN-for-prose style) |
| **3e. `WorkflowBackedCoordinator`** | Coordinator delegates to a `WorkflowDefinition` for the orchestration logic | When the topology is too complex for a fixed coordinator |
| **3f. `SupervisorWorkflowCoordinator`** | Supervisor agent assigns work into a workflow sub-process | Hybrid: supervisor decides, workflow executes |
| **3g. `CoordinatorWorkflowBridge`** | Bidirectional adapter between any coordinator and a workflow | Glue when you need both abstractions |

### Template — Supervisor + Evaluator with retries

```cpp
// 1. Workers
auto editor_runner   = std::make_shared<AgentRunner>(editor_cfg);
auto reviewer_runner = std::make_shared<AgentRunner>(reviewer_cfg);
auto critic_runner   = std::make_shared<AgentRunner>(critic_cfg);

// 2. Router and evaluator
RuleBasedTaskRouter router(RuleBasedTaskRouterConfig{
    .rules = {
        {.match = "edit",   .worker = "editor"},
        {.match = "review", .worker = "reviewer"},
    },
    .default_worker = "editor",
});
ThresholdEvaluationStrategy evaluation(ThresholdEvaluationStrategyConfig{
    .pass_threshold = 0.8,
    .scorer = critic_runner,    // evaluator is itself an agent
});

// 3. Coordinator
SupervisorEvaluatorCoordinator coord(SupervisorEvaluatorCoordinatorOptions{
    .router              = std::make_shared<RuleBasedTaskRouter>(router),
    .evaluation_strategy = std::make_shared<ThresholdEvaluationStrategy>(evaluation),
    .max_attempts        = 3,
});

// 4. Run
InMemoryArtifactStore artifacts;
InMemorySharedStateStore shared;
InMemoryMailbox mailbox;
CoordinatorContext ctx{
    .worker_agents = { {"editor", editor_runner.get()},
                       {"reviewer", reviewer_runner.get()} },
    .artifact_store = &artifacts,
    .shared_state   = &shared,
    .mailbox        = &mailbox,
    .state          = create_shared_task_state(Value("Rewrite the intro paragraph.")),
};
auto result = coord.run("Rewrite the intro paragraph.", ctx);
```

---

## Pattern 4: Tool-Client Remote Agent

**When**: Agent A and Agent B run in different processes (or different
machines), and you want A to invoke B as if B were just another tool.

**Primitives**:
- `tool_client` module: `ToolBroker`, `ToolRuntime`, `ToolClientTransport`
- Pair an exposed `AgentRunner` on the server side with a tool stub on the
  client side; the stub forwards inputs over the transport

**Template — agent-as-tool over a paired in-memory transport**:

```cpp
// SERVER side (process A)
auto server_runner = std::make_shared<AgentRunner>(server_cfg);
ToolBroker broker;
broker.register_tool_runtime("remote.qa",
    [server_runner](const Value& input, ToolExecutionContext&) -> Value {
      auto r = server_runner->run(input.at("question").as_string());
      return Value::object({{"answer", r.text}});
    });
auto paired_transport = create_paired_tool_client_transport();
broker.attach(paired_transport.server);

// CLIENT side (process B) — speaks to the broker through the paired transport
ToolClient client(paired_transport.client);
auto tool_def = client.materialize_tool("remote.qa");
client_runner_cfg.tools.push_back(tool_def);
```

**For real-world deployment**, swap `paired` for the injected Socket.IO
transport adapter (or your own — `ToolClientTransport` is the interface).
TLS, auth, rate-limiting are the host's responsibility (zero-deps boundary).

**Concerns**:
- Latency budget: every remote tool call is a round-trip; design with that in mind
- Backpressure: the broker has timeout/cancel semantics — wire them
- Versioning: tool schemas are pulled at materialize time; pin them if needed

---

## Pattern 5: Mailbox Messaging

**When**: Agents are loosely coupled. No central orchestrator. They publish
to channels and react to messages — closer to an actor system than a workflow.

**Primitives**:
- `InMemoryMailbox` — `publish(channel, sender, payload, recipient?, type?)`
- `MessageEnvelope` — the message shape (id, channel, sender, recipient, type, payload)
- `MailboxTaskRouter` — routes work from inbox to workers (used by 3b/3c too)
- `MailboxReadOptions` — filter by channel/recipient/limit

**Template — pub/sub between three agents on a topic**:

```cpp
InMemoryMailbox mailbox;

// Agent A publishes when it finishes
runner_a.event_bus().register_sink([&](const FrameworkEvent& e) {
  if (e.category == "run.completed") {
    mailbox.publish("results", "agent-a", e.payload, "", "result");
  }
});

// Agent B watches the inbox channel
auto inbox = mailbox.receive(MailboxReadOptions{.channel = "results", .recipient = ""});
for (const auto& env : inbox) {
  runner_b.run(env.payload.at("text").as_string());
  mailbox.ack({env.id});
}
```

**Concerns**:
- `InMemoryMailbox` is single-process; for cross-process, pair with a realtime
  adapter (Redis-compatible interface in `execution.hpp`) or build over
  `tool_client`
- Ordering: publish order is preserved per channel; no global ordering
- Replay: `MailboxTaskRouter` consumes-and-acks; if you need replay, persist
  the envelopes yourself

---

## Pattern 6: Autonomous Multi-Step Runner

**When**: A long-running job that needs to run unattended over hours or days,
deciding its own next steps, persisting checkpoints, surviving process
restarts. The "agent that wakes itself up" pattern.

**Primitives**:
- `autonomous.hpp`: `AutonomousRun`, `AutonomousStep`, `AutonomousRunManager`
- `StaticPlanner` / `AgentRunnerJsonPlannerAdapter` for step generation
- `CallbackStepExecutor` / `AgentRunnerStepExecutor` for step execution
- Stores: `InMemoryAutonomousStore`, `FileAutonomousStore`, injected Postgres

**Template — overnight research job**:

```cpp
auto planner  = std::make_shared<AgentRunnerJsonPlannerAdapter>(planner_runner);
auto executor = std::make_shared<AgentRunnerStepExecutor>(worker_runner);

InMemoryAutonomousStore store;
AutonomousRunManager mgr(AutonomousRunManagerOptions{
    .store    = &store,
    .planner  = planner,
    .executor = executor,
});

auto run = mgr.create(AutonomousRunInput{
    .goal       = "Survey 10 articles on prompt caching and produce a comparison table.",
    .max_steps  = 20,
    .auto_start = true,
});

// Later — different process even — resume from store:
mgr.resume(run.id);
```

**When to prefer over Pattern 2 (Workflow)**:
- The plan isn't known up front — it evolves as the agent learns
- Steps may take minutes to hours each
- You need waiting steps (cron-like) and external nudges (`waiting_step_complete`)

---

## Cross-cutting concerns

### Shared state between agents

| Need | Use |
|---|---|
| Drop a file/blob for another agent | `InMemoryArtifactStore` / `FileArtifactStore` |
| Key-value flag both agents read/write | `InMemorySharedStateStore` / `FileSharedStateStore` |
| Async message with optional recipient | `InMemoryMailbox` |
| Synchronous "give me back a value" | Pattern 4 (Tool-Client) |
| Long-lived working state per session | `scratch.*` and `todo.*` builtin tools |

### Persistence

Every shared store has an in-memory variant (default) and a file-backed
variant (durability). For production:
- Workflows / autonomous runs / approvals: inject Postgres via the existing
  store interfaces (see `PORTING_STATUS.md`)
- Mailbox: pair with the injected realtime client interface

### Observability across agents

- Each `AgentRunner` has its own `EventBus`; subscribe per-agent or merge upstream
- Each child agent inherits a `TraceContext` derived from its parent's run
  trace — span trees stay linked even across Tool-Client boundaries
- `model.cache_stats`, `session.auto_compact`, `eval.self_grading_detected`,
  `run.started/completed/failed`, `tool.audit`, `permission.checked` events
  all fire per agent

### Permissions

Each subagent gets its own `PermissionPolicy`. Best practice: **derive child
permissions by intersection with parent**, not by copy — so the child can
never escalate above what the parent was allowed. The framework doesn't
enforce this automatically (intentional: business knows its risk model);
write it as a helper in your wiring code.

---

## Anti-patterns to avoid

| Anti-pattern | What goes wrong | Fix |
|---|---|---|
| **Monolithic God Agent with 50 tools** | Tool descriptions blow context budget, model confuses similar tools | Split by role; use Pattern 1 or 3b |
| **Hidden side-channels** (agents writing to global vars) | Race conditions, untraceable state | Always go through `SharedStateStore` / `Mailbox` / `Artifact` |
| **Implicit coordination via shared session memory** | Cross-talk between supposedly-isolated agents | Give each agent its own `SessionMemory`; pass needed context explicitly |
| **Skipping isolation for "performance"** | One agent's bug corrupts the others' state | Isolation is cheaper than debugging cross-contamination later |
| **Re-implementing a coordinator that already exists** | Subtle bugs, missing observability hooks | Check the 7-coordinator catalog first |
| **Self-grading** (same model judges its own work) | Systematic positive bias | Use `critique_adapter` (different model) or a separate evaluator agent in 3c/3d |
| **Routing by string match without fallback** | Unknown task types silently dropped | `RuleBasedTaskRouter.default_worker` must be set |

---

## Putting it all together: a realistic example

A common deployable shape is:

```
                  ┌─────────────────────────────┐
                  │  SupervisorWorkerCoordinator│  ← Pattern 3b
                  └──────────────┬──────────────┘
                                 │
                ┌────────────────┼─────────────────┐
                ▼                ▼                 ▼
       ┌────────────────┐ ┌────────────┐ ┌────────────────┐
       │ Researcher     │ │ Coder      │ │ Reviewer       │
       │ (uses Pattern 1│ │ (uses tools│ │ (critique adptr│
       │  for read-only │ │  + git     │ │  + Pattern 3d  │
       │  planner)      │ │  snapshot) │ │  loop)         │
       └────────────────┘ └────────────┘ └────────────────┘
                                 │
                                 ▼
                      ┌──────────────────────┐
                      │  Artifact + Mailbox  │  ← Pattern 5 for results
                      └──────────────────────┘
                                 │
                                 ▼
                      ┌──────────────────────┐
                      │  AutonomousRunManager│  ← Pattern 6 for overnight
                      └──────────────────────┘
```

All six patterns can compose. None of them require forking the framework.
None of them have an external dependency.

---

## Cheat sheet

| You want… | Pattern | Module |
|---|---|---|
| Read-only planner before execute | 1 | `runtime.hpp` (`skill_subagents`) |
| Structured DAG with agent steps | 2 | `workflow.hpp` |
| Iterative `plan→act→observe` | 3a | `orchestration.hpp` |
| Router + workers | 3b | `orchestration.hpp` |
| Workers + retry-on-bad-grade | 3c | `orchestration.hpp` |
| Self-improvement loop | 3d | `orchestration.hpp` |
| Workflow-as-coordinator | 3e / 3f | `orchestration.hpp` + `workflow.hpp` |
| Cross-process agent calls | 4 | `tool_client.hpp` |
| Loose pub/sub | 5 | `orchestration.hpp` (`InMemoryMailbox`) |
| Multi-day unattended job | 6 | `autonomous.hpp` |

## See also

- [Architecture](architecture.md) — the 9-component model the patterns sit on
- [Workflow](workflow.md) — node/agent/condition primitives
- [Orchestration](orchestration.md) — coordinator-by-coordinator API reference
- [Autonomous](autonomous.md) — run/step/event model
- [Tool-Client](tool-client.md) — broker/runtime/transport model
- [Hooks](hooks.md) — for instrumenting any of these patterns
- [`NodeJS/docs/multi-agent.md`](../../NodeJS/docs/multi-agent.md) — TypeScript-side mirror
