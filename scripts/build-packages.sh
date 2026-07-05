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

The current public starter is expected to stop at the dependency/source
preflight until the v0.2.0-alpha complete-source-tree gate is finished.
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
  "straylight-common|packaging/libstraylight-common"
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

scripts/straylight_release_audit.sh .

if ! scripts/check_package_dependencies.sh .; then
  echo "[BUILD_BLOCKED] package dependency preflight failed" >&2
  echo "Resolve the public host/source gaps above before running package builds." >&2
  echo "The source-only public starter is expected to stop here until v0.2.0-alpha is complete." >&2
  exit 1
fi

if ! command -v dpkg-buildpackage >/dev/null 2>&1; then
  echo "[BUILD_BLOCKED] dpkg-buildpackage is not available" >&2
  echo "Install devscripts and dpkg-dev on a Debian-compatible build host." >&2
  exit 1
fi

if [ "$clean" -eq 1 ]; then
  rm -rf output/debs
fi
mkdir -p output/debs

build_args=()
if [ "$no_sign" -eq 1 ]; then
  build_args+=(-us -uc)
fi

for package in "${packages[@]}"; do
  path="$(package_path "$package")"
  if [ ! -d "$path/debian" ]; then
    echo "[BUILD_BLOCKED] $package is missing $path/debian" >&2
    echo "Publish the package payload and Debian metadata before building this group." >&2
    exit 1
  fi

  echo "[BUILD_PACKAGE] $package"
  (
    cd "$path"
    dpkg-buildpackage -b "${build_args[@]}"
  )
done

scripts/generate_package_repo.sh
find output/debs -maxdepth 1 -type f -name '*.deb' -print | sort
echo "package build wrapper completed"
