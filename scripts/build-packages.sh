#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/build-packages.sh [OPTIONS] [PACKAGE...]

Public package-build wrapper for StrayLight OS.

Options:
  --check-deps      Run public host/source dependency checks and exit.
  --clean           Remove generated output before building.
  --repo-only       Generate output/debs/Packages and Packages.gz from existing .deb files.
  --no-sign         Pass unsigned-build flags to dpkg-buildpackage.
  --list            List known package groups and exit.
  -h, --help        Show this help.

Examples:
  scripts/build-packages.sh --check-deps --no-sign
  scripts/build-packages.sh --clean --no-sign
  scripts/build-packages.sh --no-sign straylight-desktop

The public source tree should pass source preflight. Package builds still
require a prepared Debian-compatible host with the documented build packages.
USAGE
}

check_deps=0
clean=0
repo_only=0
no_sign=0
list_only=0
packages=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --check-deps)
      check_deps=1
      shift
      ;;
    --clean)
      clean=1
      shift
      ;;
    --repo-only)
      repo_only=1
      shift
      ;;
    --no-sign)
      no_sign=1
      shift
      ;;
    --list)
      list_only=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "[FAIL] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      packages+=("$1")
      shift
      ;;
  esac
done

while [ "$#" -gt 0 ]; do
  packages+=("$1")
  shift
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
cd "$root"

package_order=(
  straylight-common
  straylight-kernel
  straylight-core
  straylight-ml
  straylight-network
  straylight-exotic
  straylight-desktop
  straylight-os
)

package_paths=(
  "straylight-common|packaging/straylight-common"
  "straylight-kernel|packaging/straylight-kernel"
  "straylight-core|packaging/straylight-core"
  "straylight-ml|packaging/straylight-ml"
  "straylight-network|packaging/straylight-network"
  "straylight-exotic|packaging/straylight-exotic"
  "straylight-desktop|packaging/straylight-desktop"
  "straylight-os|packaging/straylight-os"
)

package_path() {
  local name="$1"
  local entry package path
  for entry in "${package_paths[@]}"; do
    IFS='|' read -r package path <<EOF
$entry
EOF
    if [ "$package" = "$name" ]; then
      printf '%s\n' "$path"
      return 0
    fi
  done
  return 1
}

known_package() {
  local name="$1"
  local package
  for package in "${package_order[@]}"; do
    [ "$package" = "$name" ] && return 0
  done
  return 1
}

if [ "$list_only" -eq 1 ]; then
  printf '%s\n' "${package_order[@]}"
  exit 0
fi

if [ "$repo_only" -eq 1 ]; then
  scripts/generate_package_repo.sh
  exit $?
fi

if [ "${#packages[@]}" -eq 0 ]; then
  packages=("${package_order[@]}")
fi

for package in "${packages[@]}"; do
  if ! known_package "$package"; then
    echo "[FAIL] unknown package group: $package" >&2
    echo "Run scripts/build-packages.sh --list for supported package groups." >&2
    exit 2
  fi
done

if [ "$check_deps" -eq 1 ]; then
  scripts/check_package_dependencies.sh .
  exit $?
fi

if [ "$clean" -eq 1 ]; then
  rm -rf output/debs output/package-build
  rm -f packaging/*.buildinfo packaging/*.changes packaging/*.deb
fi

scripts/straylight_release_audit.sh .

if ! scripts/check_package_dependencies.sh .; then
  echo "[BUILD_BLOCKED] package dependency preflight failed" >&2
  echo "Resolve the public host/source gaps above before running package builds." >&2
  exit 1
fi

if ! command -v dpkg-buildpackage >/dev/null 2>&1; then
  echo "[BUILD_BLOCKED] dpkg-buildpackage is not available" >&2
  echo "Install devscripts and dpkg-dev on a Debian-compatible build host." >&2
  exit 1
fi

mkdir -p output/debs
mkdir -p output/package-build

build_args=()
if [ "$no_sign" -eq 1 ]; then
  build_args+=(-us -uc)
fi

stage_package_tree() {
  local package="$1"
  local package_debian="$2/debian"
  local build_dir="output/package-build/$package"

  rm -rf "$build_dir"
  mkdir -p "$build_dir"
  rsync -a --delete \
    --exclude .git \
    --exclude output \
    --exclude 'packaging/*.buildinfo' \
    --exclude 'packaging/*.changes' \
    --exclude 'packaging/*.deb' \
    "$root"/ "$build_dir"/
  rm -rf "$build_dir/debian"
  cp -a "$package_debian" "$build_dir/debian"
}

for package in "${packages[@]}"; do
  path="$(package_path "$package")"
  if [ ! -d "$path/debian" ]; then
    echo "[BUILD_BLOCKED] $package is missing $path/debian" >&2
    echo "Publish the package payload and Debian metadata before building this group." >&2
    exit 1
  fi

  echo "[BUILD_PACKAGE] $package"
  stage_package_tree "$package" "$path"
  (
    cd "output/package-build/$package"
    dpkg-buildpackage -b "${build_args[@]}"
  )
  find output/package-build -maxdepth 1 -type f -name '*.deb' -exec mv -f {} output/debs/ \;
done

scripts/generate_package_repo.sh
find output/debs -maxdepth 1 -type f -name '*.deb' -print | sort
echo "package build wrapper completed"
