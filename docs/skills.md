# Skills API

The native skills module mirrors Anthropic-style filesystem skills and wires
them into runner prompt resolution without requiring a plugin loader or
JavaScript runtime.

## Skill Files

Parse a `SKILL.md` file or markdown string:

```cpp
auto skill = agent::parse_anthropic_skill_markdown(
    "---\n"
    "name: audit\n"
    "description: Audit repository changes\n"
    "user-invocable: true\n"
    "allowed-tools: fs.read* web.search\n"
    "model: review-model\n"
    "effort: low\n"
    "---\n"
    "Audit $ARGUMENTS in ${CLAUDE_SKILL_DIR}.",
    "/repo/.claude/skills/audit/SKILL.md",
    "project");
```

Known frontmatter fields map to `SkillManifest`:

- `name`
- `description`
- `argumentHint` or `argument-hint`
- `userInvocable` or `user-invocable`
- `disableModelInvocation` or `disable-model-invocation`
- `allowedTools`, `allowed-tools`, or `tools`
- `model`
- `effort`
- `context`
- `agent`
- `paths`
- `tier`

Unknown frontmatter fields are preserved in `manifest.metadata`.

If `name` is omitted, the parser derives it from the skill directory. If
`description` is omitted, the first body line is used. Empty skill bodies fail
with `ConfigurationError`.

## Prompt Rendering

Render prompts with arguments, session id, skill directory, and environment
values:

```cpp
auto prompt = agent::render_skill_prompt(skill, agent::SkillRenderContext{
    .arguments_text = "src tests",
    .session_id = "session-1",
    .env = {{"REVIEW_MODE", "strict"}},
});
```

Supported substitutions:

- `$ARGUMENTS`: full argument text.
- `$1`, `$2`, and other one-based argument tokens.
- `$ARGUMENTS[0]`, `$ARGUMENTS[1]`, and other zero-based argument tokens.
- `${CLAUDE_SESSION_ID}`.
- `${CLAUDE_SKILL_DIR}`.
- `${ENV_NAME}` for alphanumeric or underscore environment keys.

Argument tokenization keeps text inside parentheses together, matching the
runtime slash-skill behavior.

## Loading Skills

Load one skill file:

```cpp
auto loaded = agent::load_skill_file("/repo/.claude/skills/audit/SKILL.md");
```

Load all immediate child directories containing `SKILL.md`:

```cpp
auto skills = agent::load_skill_directory("/repo/.claude/skills", "project");
```

Discover project skill directories by walking upward from a working directory:

```cpp
auto dirs = agent::collect_project_skill_directories("/repo/packages/core");
```

`load_anthropic_skill_registry` loads project skills from discovered
`.claude/skills` directories and, by default, user skills from
`~/.claude/skills`. Project skills encountered first win unless a later
registration explicitly replaces them.

## Registry

`SkillRegistry` provides synchronized registration and snapshots:

```cpp
agent::SkillRegistry registry({skill});
registry.register_skill(loaded);

if (const auto* found = registry.get("audit")) {
  // Use found->manifest or found->prompt.
}
```

`model_invocable()` returns skills that do not set
`disable-model-invocation`.

## Runtime Resolution

Resolve a prompt-time skill state via the structured options bag. The
options carry per-skill arguments, source attribution, priorities, and
the conflict policies the resolver should apply when active skills
disagree about which model or reasoning-effort budget to use.

```cpp
agent::SkillResolveOptions opts;
opts.input_text = "/audit src";
opts.paths = {"src/auth/login.cpp"};
opts.session_id = "session-1";
opts.activations = {
    agent::SkillActivation{"audit", "", agent::SkillActivationSource::Host, 0},
};
opts.model_conflict = agent::SkillConflictPolicy::HighestPriority;
auto state = agent::resolve_skills_state(&registry, opts);
```

### SkillActivation

Each `SkillActivation` records who triggered the skill so the resolver can
apply the appropriate gating:

- `name`: skill name (must match the registry; unknowns are dropped silently).
- `arguments_text`: the raw argument string passed into `render_skill_prompt`.
- `source`: `User`, `Host`, or `Model` (see below).
- `priority`: used by `HighestPriority` conflict policy; ties break on first
  occurrence in the activation list.

### `paths` conditional activation

`SkillManifest::paths` is a host-provided conditional trigger. The resolver does
not infer file paths from free-form user text. Callers pass known relevant paths
through `SkillResolveOptions::paths`; any model-invocable skill whose
`manifest.paths` pattern matches those paths is appended as a `Model`-source
activation and recorded in `ResolvedSkillsState::auto_selected_skills`.

```cpp
agent::SkillRegistry registry({agent::SkillDefinition{
    .manifest = agent::SkillManifest{
        .name = "ts-review",
        .description = "Review TypeScript changes",
        .paths = {"src/**/*.ts"},
    },
    .prompt = "Review TypeScript changes.",
}});

agent::SkillResolveOptions opts;
opts.paths = {"src/auth/login.ts"};
auto state = agent::resolve_skills_state(&registry, opts);
// state.auto_selected_skills == {"ts-review"}
```

Matching rules:

- exact file paths and directory prefixes match without wildcards
- `*`, `?`, and `**/` glob syntax are supported
- `disable_model_invocation` skills are excluded
- explicit caller/slash activations win and are not duplicated

### SkillActivationSource

- `User`: the user typed `/name` or clicked a chip. The registry entry must
  set `user-invocable: true` — otherwise the resolver throws
  `ConfigurationError` containing `"not user-invocable"`. This is a hard
  failure (the host is not allowed to silently strip the activation, because
  doing so would hide a host-side bug).
- `Host`: host code injected the activation (default skills, the autonomous
  loop, a workflow node, etc.). `user-invocable` is not enforced.
- `Model`: the auto-selection heuristic chose the skill. Appended after the
  caller's explicit activations.

### SkillConflictPolicy

When two or more active skills request a different `model` or `effort`, the
resolver picks a winner according to the policy on `SkillResolveOptions`:

| Policy             | Behavior                                                            |
|--------------------|---------------------------------------------------------------------|
| `Error`            | Throw `ConfigurationError` (`"conflicting models"` / `"conflicting reasoning effort"`). |
| `HighestPriority`  | Pick the activation with the highest `priority`. Stable on ties.    |
| `FirstWins`        | First activation in the activations list with a non-empty value.    |
| `LastWins`         | Last activation in the activations list with a non-empty value.     |

If the caller already set `options.model_settings.model` or
`options.model_settings.reasoning`, that explicit value wins and conflict
resolution is skipped for that field entirely.

### Multi-slash parser

```cpp
auto parsed = agent::parse_slash_activations("/a /b shared text", registry);
```

`parse_slash_activations` walks leading whitespace, then consumes a run of
`/<token>` separated by whitespace. Each consumed token becomes a
`SkillActivation{source = User, arguments_text = ""}`. The scan stops on the
first token that is unknown to the registry or whose manifest does not set
`user-invocable: true` — the slash is kept as literal text so user typing is
never silently dropped.

Special case: if exactly one slash command was consumed and the remainder is
non-empty, the remainder becomes both the activation's `arguments_text` AND
the `stripped_input`. This preserves the CLI-friendly behavior of
`/audit src tests` → `audit("src tests")`.

When the parser produces a `stripped_input` different from the original
input, `ResolvedSkillsState::effective_input_text` is set; otherwise the
field is empty and the runner falls back to the original input.

### Policy table

| Scenario                                          | Behavior                                                      |
|---------------------------------------------------|---------------------------------------------------------------|
| Two skills activated as `User`                    | Both render. Both system messages emitted. Allowed-tools merged. |
| `/a /b shared text` (a, b user-invocable)         | Two User activations, both args empty, `stripped_input = "shared text"`. |
| `/a /b-noui shared text` (b-noui not user-invoc.) | Scan stops at b-noui. One activation with args/tail = `"/b-noui shared text"`. |
| `/a hello world`                                  | One activation with `arguments_text = "hello world"`, `stripped_input = "hello world"`. |
| `paths = {"src/auth/login.ts"}`, skill `paths: ["src/**/*.ts"]` | Auto-activates the skill as `Model`; records the name in `auto_selected_skills`. |
| `User` source on non-user-invocable skill         | `ConfigurationError("not user-invocable")`.                   |
| `Host` source on non-user-invocable skill         | Renders normally (programmatic injection bypass).             |
| `Error` policy, conflicting models                | `ConfigurationError("conflicting models")`.                   |
| `HighestPriority` policy, A.priority=10, B=1      | A's model wins, no throw.                                     |
| `FirstWins` / `LastWins`                          | First / last activation with non-empty value wins.            |
| Caller's `model_settings.model` already set       | Caller wins; skill conflict resolution skipped (no throw).    |

Automatic selection scores model-invocable skills by name, description, and
argument-hint token overlap. It does not auto-select when a `User`-source
activation is present, and it respects active model/effort constraints.

## Zero-Dependency Boundary

Skills use markdown parsing, small frontmatter parsing, filesystem discovery,
prompt rendering, registry snapshots, and deterministic selection heuristics.
They do not link a plugin runtime, JavaScript runtime, package manager, model
SDK, browser, or database.
