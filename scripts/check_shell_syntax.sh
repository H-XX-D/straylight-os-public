#!/usr/bin/env bash
set -euo pipefail

root="${1:-.}"
cd "$root"

fail=0

while IFS= read -r -d '' script; do
  if bash -n "$script"; then
    echo "[OK] shell syntax $script"
  else
    echo "[FAIL] shell syntax $script" >&2
    fail=1
  fi
done < <(find . \
  \( -path './.git' -o -path './node_modules' -o -path './build' -o -path './out' -o -path './output' -o -path './.tmp' \) -prune -o \
  -type f -name '*.sh' -print0 | sort -z)

if [ "$fail" -ne 0 ]; then
  echo "shell syntax check failed" >&2
  exit 1
fi

echo "shell syntax check passed"
