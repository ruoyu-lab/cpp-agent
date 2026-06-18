#!/usr/bin/env python3

import os
import pathlib
import subprocess
import sys


def read_expected(path: pathlib.Path) -> list[str]:
    return sorted(
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def extract_symbols(library_path: pathlib.Path) -> list[str]:
    if sys.platform == "win32":
        dumpbin = os.environ.get("DUMPBIN", "dumpbin")
        completed = subprocess.run(
            [dumpbin, "/exports", str(library_path)],
            check=True,
            capture_output=True,
            text=True,
        )
        symbols = []
        for line in completed.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0].isdigit():
                name = parts[-1].strip()
                if name.startswith("agent_"):
                    symbols.append(name)
        return sorted(set(symbols))

    nm_cmd = ["nm"]
    if sys.platform == "darwin":
        nm_cmd.extend(["-gU", str(library_path)])
    else:
        nm_cmd.extend(["-g", "--defined-only", str(library_path)])
    completed = subprocess.run(nm_cmd, check=True, capture_output=True, text=True)
    symbols = []
    for line in completed.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1].strip()
        if sys.platform == "darwin" and name.startswith("_"):
            name = name[1:]
        if name.startswith("agent_"):
            symbols.append(name)
    return sorted(set(symbols))


def main() -> int:
    library_path = pathlib.Path(os.environ["AGENT_CAPI_LIBRARY"])
    manifest_path = pathlib.Path(os.environ["AGENT_CAPI_SYMBOL_MANIFEST"])

    expected = read_expected(manifest_path)
    actual = extract_symbols(library_path)

    if actual != expected:
        print("C API symbol manifest mismatch", file=sys.stderr)
        print("Expected:", file=sys.stderr)
        for name in expected:
            print(f"  {name}", file=sys.stderr)
        print("Actual:", file=sys.stderr)
        for name in actual:
            print(f"  {name}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
