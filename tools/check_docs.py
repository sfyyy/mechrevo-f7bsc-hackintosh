#!/usr/bin/env python3
"""Validate repository documentation invariants without requiring hardware."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
PERSONAL_PATH_RE = re.compile(r"/Users/(?!Shared(?:/|$))[^/\s`]+")


def markdown_files() -> list[Path]:
    return sorted(p for p in ROOT.rglob("*.md") if ".git" not in p.parts)


def check_links(files: list[Path]) -> list[str]:
    errors: list[str] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for match in LINK_RE.finditer(text):
            target = match.group(1).strip().split()[0]
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            local = (path.parent / target.split("#", 1)[0]).resolve()
            if not local.exists():
                errors.append(f"{path.relative_to(ROOT)}: missing link target {target!r}")
    return errors


def check_private_paths(files: list[Path]) -> list[str]:
    errors: list[str] = []
    for path in files:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = PERSONAL_PATH_RE.search(line)
            if match:
                errors.append(
                    f"{path.relative_to(ROOT)}:{number}: personal home path {match.group(0)!r}"
                )
    return errors


def check_language_pair() -> list[str]:
    errors: list[str] = []
    pair = [ROOT / "README.md", ROOT / "README.zh-CN.md"]
    for path in pair:
        if not path.is_file():
            errors.append(f"missing bilingual README: {path.relative_to(ROOT)}")
            continue
        text = path.read_text(encoding="utf-8")
        other = pair[1] if path == pair[0] else pair[0]
        if other.name not in text:
            errors.append(f"{path.relative_to(ROOT)}: language switch does not link to {other.name}")
    return errors


def main() -> int:
    files = markdown_files()
    errors = check_links(files) + check_private_paths(files) + check_language_pair()
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Documentation checks passed ({len(files)} Markdown files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
