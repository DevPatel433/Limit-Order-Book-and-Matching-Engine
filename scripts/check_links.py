#!/usr/bin/env python3
"""Verify that every relative link in the project's markdown files points at something that exists.
usage: check_links.py [repo_root]    (exit code 1 if any link is broken)"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
link = re.compile(r"\]\(([^)\s]+)\)")
broken, checked = [], 0
for md in sorted(root.rglob("*.md")):
    if any(part.startswith("build") or part == ".git" for part in md.relative_to(root).parts):
        continue
    for target in link.findall(md.read_text(encoding="utf-8")):
        if target.startswith(("http://", "https://", "mailto:", "#")):
            continue
        path = target.split("#")[0]
        if not path:
            continue
        checked += 1
        if not (md.parent / path).exists():
            broken.append(f"{md.relative_to(root)}: {target}")
print(f"checked {checked} relative links in markdown files")
for b in broken:
    print("BROKEN:", b)
sys.exit(1 if broken else 0)
