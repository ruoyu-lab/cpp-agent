#!/usr/bin/env python3

import pathlib
import re
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SEED_FILES = [
    REPO_ROOT / "README.md",
    REPO_ROOT / "docs" / "README.md",
]

CANDIDATE_ROOTS = [
    REPO_ROOT / "docs",
    REPO_ROOT / "examples",
]


def parse_references(file_path: pathlib.Path) -> set[pathlib.Path]:
    text = file_path.read_text(encoding="utf-8")
    refs: set[pathlib.Path] = set()

    for match in re.finditer(r"\]\(([^)]+)\)", text):
      raw = match.group(1).strip()
      if not raw or raw.startswith(("http:", "https:", "#")):
          continue
      clean = raw.replace("<", "").replace(">", "").split(":")[0]
      refs.add((file_path.parent / clean).resolve())

    for match in re.finditer(r"\b((?:docs|examples)/[A-Za-z0-9_./+-]+\.(?:md|cpp|cc|cxx))\b", text):
      refs.add((REPO_ROOT / match.group(1)).resolve())

    return refs


def collect_reachable() -> set[pathlib.Path]:
    queue = list(SEED_FILES)
    seen = set(SEED_FILES)
    while queue:
        current = queue.pop(0)
        if current.suffix != ".md":
            continue
        for ref in parse_references(current):
            if ref.exists() and ref not in seen:
                seen.add(ref)
                queue.append(ref)
    return seen


def main() -> int:
    reachable = collect_reachable()
    candidates: list[pathlib.Path] = []
    for root in CANDIDATE_ROOTS:
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(REPO_ROOT).as_posix()
            if rel in {"README.md", "docs/README.md", "examples/README.md"}:
                continue
            candidates.append(path.resolve())

    orphans = sorted(path.relative_to(REPO_ROOT).as_posix() for path in candidates if path not in reachable)
    if orphans:
        print("Orphan docs/examples detected:", file=sys.stderr)
        for orphan in orphans:
            print(f"- {orphan}", file=sys.stderr)
        return 1
    print("doc/example reference closure OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
