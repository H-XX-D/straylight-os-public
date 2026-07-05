#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/generate_iso_checksum.sh [--check] [ISO_PATH]

Generates or verifies a SHA256 checksum for a StrayLight ISO candidate.

Default ISO path:
  output/straylight-os-1.0.0-amd64.iso

Default checksum path:
  <ISO_PATH>.sha256

The current public starter is expected to report a missing ISO until the
v0.3.0-alpha reproducible ISO candidate gate is complete.
USAGE
}

check=0
iso_path="output/straylight-os-1.0.0-amd64.iso"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --check)
      check=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "[FAIL] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      iso_path="$1"
      shift
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
cd "$root"

checksum_path="${iso_path}.sha256"

if ! command -v sha256sum >/dev/null 2>&1; then
  echo "[MISSING_HOST_TOOL] sha256sum" >&2
  exit 1
fi

if [ ! -f "$iso_path" ]; then
  echo "[MISSING_ISO_ARTIFACT] $iso_path" >&2
  exit 1
fi

if [ "$check" -eq 1 ]; then
  if [ ! -f "$checksum_path" ]; then
    echo "[MISSING_CHECKSUM] $checksum_path" >&2
    exit 1
  fi
  sha256sum -c "$checksum_path"
  echo "[CHECKSUM_VERIFIED] $checksum_path"
  exit 0
fi

sha256sum "$iso_path" > "$checksum_path"
echo "[CHECKSUM_OUTPUT] $checksum_path"
