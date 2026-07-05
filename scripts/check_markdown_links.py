#!/usr/bin/env python3
"""Check repository-local Markdown links and anchors."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import urllib.parse


LINK_RE = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)")
HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$")


def clean_heading_text(text: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("*", "").replace("_", "").strip()
    return text


def github_anchor(text: str) -> str:
    text = clean_heading_text(text).lower()
    text = re.sub(r"[^\w\s-]", "", text)
    text = re.sub(r"\s+", "-", text.strip())
    text = re.sub(r"-+", "-", text)
    return text


def anchors_for(path: pathlib.Path) -> set[str]:
    counts: dict[str, int] = {}
    anchors: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = github_anchor(match.group(2))
        if not base:
            continue
        index = counts.get(base, 0)
        counts[base] = index + 1
        anchors.add(base if index == 0 else f"{base}-{index}")
    return anchors


def markdown_files(root: pathlib.Path) -> list[pathlib.Path]:
    ignored = {
        ".git",
        "node_modules",
        "build",
        "out",
        "output",
        ".tmp",
        ".build",
        "binary",
        "cache",
        "chroot",
        "packages.chroot",
    }
    files: list[pathlib.Path] = []
    for path in root.rglob("*.md"):
        if ignored.intersection(path.relative_to(root).parts):
            continue
        files.append(path)
    return sorted(files)


def normalize_target(raw_target: str) -> tuple[str, str]:
    target = raw_target.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1].strip()
    else:
        target = target.split()[0]

    target = urllib.parse.unquote(target)
    if "#" in target:
        path_part, fragment = target.split("#", 1)
    else:
        path_part, fragment = target, ""
    return path_part, fragment


def should_skip(path_part: str) -> bool:
    lower = path_part.lower()
    return lower.startswith(("http://", "https://", "mailto:", "tel:"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=".")
    args = parser.parse_args()

    root = pathlib.Path(args.root).resolve()
    failures: list[str] = []
    anchor_cache: dict[pathlib.Path, set[str]] = {}

    for md_file in markdown_files(root):
        text = md_file.read_text(encoding="utf-8")
        for line_number, line in enumerate(text.splitlines(), start=1):
            for match in LINK_RE.finditer(line):
                raw_target = match.group(1)
                path_part, fragment = normalize_target(raw_target)
                if should_skip(path_part) or raw_target.startswith("#"):
                    target_path = md_file
                    fragment = raw_target[1:] if raw_target.startswith("#") else fragment
                elif not path_part:
                    target_path = md_file
                else:
                    target_path = (md_file.parent / path_part).resolve()
                    try:
                        target_path.relative_to(root)
                    except ValueError:
                        failures.append(
                            f"{md_file.relative_to(root)}:{line_number}: link escapes repository: {raw_target}"
                        )
                        continue
                    if not target_path.exists():
                        failures.append(
                            f"{md_file.relative_to(root)}:{line_number}: missing target: {raw_target}"
                        )
                        continue

                if fragment:
                    if target_path.is_dir():
                        failures.append(
                            f"{md_file.relative_to(root)}:{line_number}: anchor target is a directory: {raw_target}"
                        )
                        continue
                    anchors = anchor_cache.setdefault(target_path, anchors_for(target_path))
                    if fragment not in anchors:
                        failures.append(
                            f"{md_file.relative_to(root)}:{line_number}: missing anchor #{fragment}: {raw_target}"
                        )

    if failures:
        print("markdown link check failed", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1

    print("markdown link check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
