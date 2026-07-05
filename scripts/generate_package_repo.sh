#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/generate_package_repo.sh [--check-only] [--repo-dir DIR]

Generates the local APT package index consumed by the StrayLight ISO build.

Inputs:
  output/debs/*.deb

Outputs:
  output/debs/Packages
  output/debs/Packages.gz

The public source tree does not include generated .deb inputs. Build packages
first, then generate the local package repository.
USAGE
}

repo_dir="output/debs"
check_only=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --check-only)
      check_only=1
      shift
      ;;
    --repo-dir)
      if [ "$#" -lt 2 ]; then
        echo "[FAIL] --repo-dir requires a value" >&2
        exit 2
      fi
      repo_dir="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[FAIL] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
cd "$root"

if ! command -v dpkg-scanpackages >/dev/null 2>&1; then
  echo "[MISSING_HOST_TOOL] dpkg-scanpackages: install dpkg-dev" >&2
  exit 1
fi

if ! command -v gzip >/dev/null 2>&1; then
  echo "[MISSING_HOST_TOOL] gzip" >&2
  exit 1
fi

if [ ! -d "$repo_dir" ]; then
  echo "[MISSING_PACKAGE_REPO_INPUT] $repo_dir does not exist" >&2
  exit 1
fi

mapfile -t debs < <(find "$repo_dir" -maxdepth 1 -type f -name '*.deb' -print | LC_ALL=C sort)
if [ "${#debs[@]}" -eq 0 ]; then
  echo "[MISSING_PACKAGE_REPO_INPUT] no .deb files found in $repo_dir" >&2
  exit 1
fi

echo "[PACKAGE_REPO_INPUT] $repo_dir"
for deb in "${debs[@]}"; do
  echo "[PACKAGE_REPO_DEB] $deb"
done

if [ "$check_only" -eq 1 ]; then
  echo "package repository inputs present"
  exit 0
fi

tmp_packages="$(mktemp)"
tmp_packages_gz="$(mktemp)"
cleanup() {
  rm -f "$tmp_packages" "$tmp_packages_gz"
}
trap cleanup EXIT

(
  cd "$repo_dir"
  dpkg-scanpackages . /dev/null
) > "$tmp_packages"

gzip -n -9 < "$tmp_packages" > "$tmp_packages_gz"

mv "$tmp_packages" "$repo_dir/Packages"
mv "$tmp_packages_gz" "$repo_dir/Packages.gz"
trap - EXIT

echo "[PACKAGE_REPO_OUTPUT] $repo_dir/Packages"
echo "[PACKAGE_REPO_OUTPUT] $repo_dir/Packages.gz"
echo "package repository generation completed"
