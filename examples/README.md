# Examples

Self-contained programs that exercise one focused capability of the native
agent framework. All examples build with zero external dependencies and use
the built-in echo chat model so they can run anywhere `agent_native` builds.

| File | Demonstrates |
|---|---|
| [`runner-basic.cpp`](runner-basic.cpp) | Constructing `AgentRunner`, calling `run` and `stream`. |
| [`tools-and-permissions.cpp`](tools-and-permissions.cpp) | Defining a tool, applying a capability permission policy, and observing allow/deny outcomes through the executor. |
| [`rag-basic.cpp`](rag-basic.cpp) | Ingesting documents into an in-memory `KnowledgeBase`, searching, and wiring it onto `AgentRunner` for automatic retrieval. |
| [`workflow.cpp`](workflow.cpp) | Building a small workflow with the default node registry and a custom tool, then running it through `WorkflowEngine` and inspecting the persisted state. |
| [`evals-basic.cpp`](evals-basic.cpp) | Running an `EvalSuite` with both built-in (`expect_contains`) and custom (`EvalAssertion`) assertions. |
| [`capi-bindings/`](capi-bindings/README.md) | Minimal C, Python, Rust, and C# bindings for the C ABI v3 surface. |

## Build

The examples are wired into the same CMake build as the framework. After a
top-level `cmake --build build`, each example produces a binary in `build/`:

```bash
./build/example_runner_basic
./build/example_tools_and_permissions
./build/example_rag_basic
./build/example_workflow
./build/example_evals_basic
./build/example_capi_c
```

All five are also registered with CTest as `example_*` cases so a full
`ctest --test-dir build --output-on-failure` exercises them.

## Where to go next

- Want to expose any of these flows to another language? See
  [`docs/bindings.md`](../docs/bindings.md) and the shipped
  [`agent_capi`](../include/agent_capi.h) / [`agent_capi_full`](../include/agent_capi_full.h)
  shims.
- Want to add a new external provider, browser, OCR, or store backend? See the
  injection points in [`docs/architecture.md`](../docs/architecture.md).
