#!/usr/bin/env python3
# Lightweight Tua formatter:
# - trims trailing whitespace
# - normalizes indentation based on `{` / `}` blocks
# - collapses multiple blank lines
#
# This is intentionally conservative: it does not reorder code or rewrite expressions.

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Tuple


DEFAULT_INDENT = "  "  # 2 spaces

# Directories to skip when recursing.
SKIP_DIR_NAMES = {
    ".git",
    "build",
    "bin",
    "node_modules",
    ".cache",
    ".idea",
    ".vscode",
}


@dataclass
class ScanState:
    in_block_comment: bool = False


def _scan_line_for_braces(line: str, state: ScanState) -> Tuple[int, int, ScanState]:
    """
    Returns (delta_indent, leading_closing_braces, new_state).
    Only counts braces outside strings and comments.
    """
    i = 0
    n = len(line)
    in_string = False
    escaped = False

    delta = 0
    leading_closes = 0
    seen_non_ws = False

    while i < n:
        c = line[i]

        if state.in_block_comment:
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                state.in_block_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escaped:
                escaped = False
                i += 1
                continue
            if c == "\\":
                escaped = True
                i += 1
                continue
            if c == '"':
                in_string = False
            i += 1
            continue

        # Not in string/comment.
        if c in (" ", "\t", "\r"):
            i += 1
            continue

        # Line comment: ignore rest of line.
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break

        # Block comment start.
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            state.in_block_comment = True
            i += 2
            continue

        # String start.
        if c == '"':
            in_string = True
            i += 1
            continue

        if c == "{":
            delta += 1
            seen_non_ws = True
            i += 1
            continue

        if c == "}":
            delta -= 1
            if not seen_non_ws:
                leading_closes += 1
            seen_non_ws = True
            i += 1
            continue

        seen_non_ws = True
        i += 1

    return delta, leading_closes, state


def format_tua_text(text: str, indent: str = DEFAULT_INDENT) -> str:
    state = ScanState()
    level = 0
    out: List[str] = []

    for raw in text.splitlines():
        # Normalize line endings and drop trailing whitespace.
        line = raw.rstrip()
        stripped = line.strip()

        if stripped == "":
            if out and out[-1] != "":
                out.append("")
            continue

        delta, leading_closes, state = _scan_line_for_braces(line, state)
        use_level = level - leading_closes
        if use_level < 0:
            use_level = 0

        out.append(f"{indent * use_level}{stripped}")

        level += delta
        if level < 0:
            level = 0

    # Avoid trailing blank line spam; ensure file ends with exactly one newline.
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"


def _iter_tua_files(paths: Iterable[Path]) -> Iterator[Path]:
    for p in paths:
        if p.is_dir():
            for root, dirs, files in os.walk(p):
                # Prune dirs.
                dirs[:] = [d for d in dirs if d not in SKIP_DIR_NAMES]
                for f in files:
                    if f.endswith(".tua"):
                        yield Path(root) / f
        else:
            if p.suffix == ".tua":
                yield p


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="strict")


def _write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="Format Tua source files.")
    ap.add_argument("paths", nargs="*", default=["."], help="Files or directories (default: .)")
    ap.add_argument("--check", action="store_true", help="Exit non-zero if any file would change")
    ap.add_argument("--write", action="store_true", help="Write changes in-place")
    ap.add_argument("--indent", default=DEFAULT_INDENT, help="Indent string (default: 2 spaces)")
    args = ap.parse_args()

    if args.check and args.write:
        ap.error("Use only one of --check or --write")

    changed = 0
    checked = 0
    for f in sorted(set(_iter_tua_files([Path(p) for p in args.paths]))):
        try:
            src = _read_text(f)
        except UnicodeDecodeError as e:
            print(f"[SKIP] {f} (utf-8 decode error: {e})")
            continue

        formatted = format_tua_text(src, indent=args.indent)
        checked += 1
        if formatted != src:
            changed += 1
            if args.write:
                _write_text(f, formatted)

    if args.check and changed:
        print(f"{changed}/{checked} files would change")
        return 1

    if args.write:
        print(f"formatted {changed}/{checked} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

