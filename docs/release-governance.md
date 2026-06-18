# Release Governance

This project is a native, zero-dependency framework intended to be embedded by
many host languages. Release quality is therefore measured by stable contracts,
not by a single out-of-the-box application shape.

## Versioning

- `project(... VERSION x.y.z)` in `CMakeLists.txt` is the release version for
  native builds.
- `agent_version()` returns `agent_native <project-version>` and must not use
  build-date macros such as `__DATE__` or `__TIME__`.
- Patch releases may fix behavior, docs, tests, and internal implementation
  shape without changing public signatures.
- Minor releases may add new APIs, providers, route modules, contracts, or
  optional integration points.
- Major releases are reserved for breaking changes to public C++ headers, the
  C ABI, observable contracts, or documented runtime semantics.

## ABI Governance

`include/agent_capi.h` is the stable embeddable foreign-function boundary for
Python, Go, Rust, Java, .NET, Node addons, and other hosts.
`include/agent_capi_full.h` extends it with app/config and async-agent-run
module functions for hosts that intentionally link the full framework layer.

- C ABI functions must not throw C++ exceptions across the boundary.
- New functionality is added as new functions or structs. Existing function
  signatures stay stable inside an ABI version.
- Ownership rules must stay explicit in the header comments. Callers must know
  which values they release and which values remain borrowed.
- `AGENT_CAPI_ABI_VERSION` is bumped when a binding author must change generated
  code or manual wrapper code to keep working.
- `tests/capi_smoke.c` must compile as C and check `agent_capi_abi_version()`.
- `tests/capi_ctypes_smoke.py` must keep working as the first official
  host-language binding fixture.
- `contracts/public-surface/capi-runtime-symbols.txt` is the checked-in
  baseline for the default embeddable C ABI symbol set.
- `contracts/public-surface/capi-symbols.txt` is the checked-in baseline for
  the full C ABI symbol set.

## Contract Governance

The files under `contracts/observable` define behavior that external runtimes,
tests, and language bindings may rely on.

- Contract changes must be intentional and covered by `agent_contract_tests`.
- `capi.json` covers the shipped C ABI metadata and runner JSON surfaces used
  by host-language bindings.
- Additive fields are allowed when older consumers can ignore them safely.
- Renaming fields, changing event type strings, or changing result shapes is a
  breaking contract change unless a compatibility path is documented.
- Runtime, model, server, ReAct, and tool-result changes should update both the
  JSON contract fixture and the docs that describe that surface.

## Architecture Governance

The main implementation files are thin composition units. Domain logic lives in
private implementation fragments grouped by responsibility:

- `src/agent/memory/*.inc` for session memory, knowledge sources, indexes,
  rerankers, and knowledge-base management.
- `src/agent/model/*.inc` for model core, provider protocols, provider
  adapters, registries, and embeddings.
- `src/agent/runtime/*.inc` for runner helpers, the internal agent loop, and
  `AgentRunner`.
- `src/agent/server/*.inc` for HTTP helpers, governance, access control,
  stores, metrics, route groups, and session access.

The fragments remain private to avoid expanding the public ABI with internal
helper declarations. `agent_platform_governance_policy` enforces size limits so
these files do not grow back into concentrated modules.

## Release Checklist

Before cutting a release:

1. Configure and build with CMake from a clean build directory.
2. Run `ctest --test-dir build --output-on-failure`.
3. Confirm `agent_platform_governance_policy` and `agent_artifact_policy` pass.
4. Confirm the Python ctypes binding fixture still passes.
5. Confirm both C ABI symbol manifest checks still pass.
6. Review public header changes in `include/agent`, `include/agent_capi.h`,
   and `include/agent_capi_full.h`.
7. If `AGENT_CAPI_ABI_VERSION` changed, update binding notes and release notes.
8. Review `contracts/observable` changes and document whether each change is
   additive or breaking.
9. Confirm optional integrations remain optional and the native core still
   builds without external provider dependencies.
