# Build

`agent_native` builds with CMake (≥ 3.20) and any C++20 compiler. The default
build links only the C++ standard library.

## Targets

| Target | Type | Purpose |
|---|---|---|
| `agent_core` | static library | Foundation layer: values, messages, HTTP contracts, hooks, observability. |
| `agent_platform` | static library | Optional zero-dependency native platform helpers such as the plain-HTTP socket transport and POSIX process hooks. Depends on `agent_core`. |
| `agent_model` | static library | Model/provider layer. Depends on `agent_core`. |
| `agent_tools` | static library | Tool definitions, registry, executor, sandbox, scratch, tool-run services, security governance. Depends on `agent_model`. |
| `agent_mcp` | static library | MCP JSON-RPC protocol helpers, in-process server/client, callback/line transports, and tool/resource/prompt adapters. Depends on `agent_tools`. |
| `agent_mcp_native` | static library | Optional native MCP stdio subprocess and plain-HTTP transports. Depends on `agent_mcp` and `agent_platform`. |
| `agent_runtime` | static library | Embeddable runner core: context, memory, skills/knowledge contracts, streaming, and `AgentRunner`. Depends on `agent_tools`. |
| `agent_runtime_io` | static library | Explicit host I/O modules: browser adapter, web helpers, media/document preprocessing, and web-enabled knowledge loaders. Depends on `agent_runtime`. |
| `agent_runtime_io_native` | static library | Optional native plain-HTTP web/media helpers. Depends on `agent_runtime_io` and `agent_platform`. |
| `agent_runtime_modules` | static library | Explicit high-level runtime modules: async runs, tasks, autonomous execution, plan-and-execute, realtime, orchestration, workflow. Depends on `agent_runtime`. |
| `agent_app` | static library | Config, built-ins, built-in provider descriptor registry, MCP assembly, CLI helpers, eval/replay, default app assembly. Depends on `agent_runtime_modules`, `agent_runtime_io_native`, and `agent_mcp_native`. |
| `agent_server` | static library | Server app and route modules. Depends on `agent_app`. |
| `agent_native` | interface aggregate | Embeddable runtime aggregate. Does not link app/server. |
| `agent_full` | interface aggregate | Full app/server aggregate for applications that intentionally want every layer. |
| `agent_capi` / `agent_capi_shared` | C ABI library | Embeddable C ABI over `agent_runtime`; no app/server/I/O by default. |
| `agent_capi_full` / `agent_capi_full_shared` | C ABI library | Full C ABI over `agent_app`; includes config constructors and async-agent-run modules. |
| `native_agent_cli` | executable | The reference CLI binary. |
| `agent_native_smoke` | executable | Smoke test exercised by CTest and direct repeated runs. |

## Standard Build

```bash
cd C++
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces layered static libraries such as `build/libagent_core.a`,
`build/libagent_model.a`, `build/libagent_tools.a`,
`build/libagent_mcp.a`, `build/libagent_mcp_native.a`,
`build/libagent_runtime.a`, `build/libagent_platform.a`,
`build/libagent_runtime_io.a`, `build/libagent_runtime_io_native.a`,
`build/libagent_runtime_modules.a`, `build/libagent_app.a`, and
`build/libagent_server.a`, plus `build/libagent_capi.a` and
`build/libagent_capi_full.a`, plus the
`native_agent_cli` binary in `build/`.
`agent_native` and `agent_full` are CMake aggregate targets, not separate
archives.

The CMake target sets `-Wall -Wextra -Wpedantic` for GCC/Clang and `/W4
/permissive-` for MSVC. Treat new warnings as porting regressions.

## Smoke Test Loop

The porting audit standard is to run the smoke binary repeatedly:

```bash
for i in $(seq 1 10); do ./build/agent_native_smoke || break; done
```

The smoke binary exercises registries, runtime, serialization, and store paths.
Consecutive failures indicate concurrency or state-leak regressions.

## Optional llama.cpp Native Binding

The framework ships with a stub llama.cpp adapter so the default build stays
zero-dependency. To enable the built-in dynamic-loader binding:

```bash
cmake -S . -B build-llama \
  -DAGENT_NATIVE_ENABLE_LLAMA_CPP=ON \
  -DLLAMA_CPP_INCLUDE_DIR=/path/to/llama.cpp/include \
  -DGGML_INCLUDE_DIR=/path/to/llama.cpp/ggml/include
cmake --build build-llama
```

When `AGENT_NATIVE_ENABLE_LLAMA_CPP=ON`:

- `src/agent/llama_cpp_native_binding.cpp` replaces the stub.
- The macro `AGENT_NATIVE_ENABLE_LLAMA_CPP=1` is propagated to consumers.
- On Linux the framework links `dl` for `dlopen`-based runtime loading.

Runtime configuration still requires the host to provide:

- `modelPath` — GGUF model file.
- `libraryPath` or `libraryDir` — location of `libllama` / `libggml`.
- `mmprojPath` plus `mtmdLibraryPath`/`mtmdLibraryDir` if image input is used
  (requires a llama.cpp build that ships `libmtmd`).

The relevant environment-variable names (`LLAMA_CPP_INCLUDE_DIR`,
`GGML_INCLUDE_DIR`, `modelPathEnv`, `libraryPathEnv`, `libraryDirEnv`,
`mmprojPathEnv`, `mtmdLibraryPathEnv`, `mtmdLibraryDirEnv`) are collected by
the config validator for env-key auditing — see [Config](config.md).

## Zero-Dependency Verification

Periodically rerun the dependency scan that the porting audits use to confirm
the build still has no transitive third-party links. The expectation is that
the native archives reference only standard library symbols (plus `dl` and
`pthread` on Linux when relevant, and `mtmd`/`llama`/`ggml` symbols loaded
dynamically only when llama.cpp is enabled).

A quick local check:

```bash
nm -gC build/libagent_runtime.a build/libagent_runtime_io.a build/libagent_runtime_io_native.a build/libagent_runtime_modules.a build/libagent_mcp.a build/libagent_mcp_native.a build/libagent_app.a build/libagent_server.a | grep -E ' U ' | sort -u
```

Anything beyond the standard library, libc, libdl, libpthread, and (when
llama.cpp is enabled) llama/ggml/mtmd entry points indicates a new transitive
dependency. Investigate before merging.

## Embedding `agent_native` Into Another Build

```cmake
add_subdirectory(third_party/agent_native/C++)
target_link_libraries(your_target PRIVATE agent_native)
target_include_directories(your_target PRIVATE third_party/agent_native/C++/include)
```

There is no install step required. The CMake target exports its include
directory publicly. Link `agent_full` instead when the embedding application
intentionally wants the config/app/server surface:

```cmake
target_link_libraries(your_server_target PRIVATE agent_full)
```

Link `agent_runtime_modules` when an embedding host wants only the high-level
runtime modules without the app/server assembly:

```cmake
target_link_libraries(your_workflow_target PRIVATE agent_runtime_modules)
```

Link `agent_runtime_io` and include `agent/knowledge_io.hpp` when an embedding
host wants browser/web/media helpers and web-enabled knowledge loaders without
the app/server assembly:

```cmake
target_link_libraries(your_io_target PRIVATE agent_runtime_io)
```

Link `agent_runtime_io_native` and include `agent/web_native.hpp` or
`agent/media_native.hpp` only when the host wants the built-in plain-HTTP
helpers:

```cmake
target_link_libraries(your_io_target PRIVATE agent_runtime_io_native)
```

Embeddable knowledge retrieval contracts are available through
`agent/knowledge_runtime.hpp` and the `KnowledgeContextProvider` port on the
normal `agent_native` path. Full knowledge-base assembly lives behind
`agent_knowledge` / `agent_full`; include `agent/knowledge_core.hpp` or
`agent/knowledge.hpp` only when that opt-in is intentional. `agent/memory.hpp`
is intentionally outside the default runtime surface; vector memory,
repository/web/sitemap loaders, and transcript/layered-memory implementations
require the explicit memory/knowledge/I/O targets.

## Building the CLI Standalone

The CLI binary is the simplest way to validate a build end-to-end:

```bash
./build/native_agent_cli --help
./build/native_agent_cli --version
```

CLI command reference is in [CLI](cli.md).

## Platform Notes

- **Linux**: GCC 11+ or Clang 14+. `dl` is linked when llama.cpp is enabled.
- **macOS**: Apple Clang 14+ or Homebrew Clang 15+. `dlopen` is provided by
  the system, no extra link flag needed.
- **Windows / MSVC**: VS 2022 (MSVC 19.34+). The framework builds cleanly on
  Windows. The server module ships as a transport-independent app
  (`AgentServerApp`); HTTP transport is provided by the host on every
  platform, so there is no platform-specific listener to maintain.
