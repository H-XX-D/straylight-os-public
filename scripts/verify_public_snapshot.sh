#!/usr/bin/env bash
set -euo pipefail

root="${1:-.}"
cd "$root"

scripts/straylight_release_audit.sh .
python3 scripts/check_markdown_links.py .
scripts/check_shell_syntax.sh .

if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if [ -n "$(git status --short)" ]; then
    echo "[FAIL] working tree has uncommitted changes" >&2
    git status --short >&2
    exit 1
  fi
  echo "[OK] working tree clean"
else
  echo "[SKIP] git working tree check"
fi

echo "public snapshot verification passed"
