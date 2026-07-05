#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/check_iso_candidate_requirements.sh [--host-only|--source-only] [ROOT]

Checks whether a clean StrayLight source tree has the host packages, public
iso/live-build paths, and generated package repository required before
producing an ISO candidate.

Use --source-only to verify repository-controlled ISO source paths without
requiring generated package outputs such as output/debs/Packages.gz.
USAGE
}

mode="all"
root="."

while [ "$#" -gt 0 ]; do
  case "$1" in
    --host-only)
      mode="host"
      shift
      ;;
    --source-only)
      mode="source"
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
      root="$1"
      shift
      ;;
  esac
done

if [ "$#" -gt 0 ]; then
  root="$1"
fi

if [ ! -d "$root" ]; then
  echo "[FAIL] root does not exist: $root" >&2
  exit 2
fi

root="$(cd "$root" && pwd)"
fail=0

host_packages=(
  live-build
  debootstrap
  squashfs-tools
  xorriso
  grub-pc-bin
  grub-efi-amd64-bin
)

source_paths=(
  "iso/live-build/auto/config|live-build auto config entrypoint"
  "iso/live-build/config/package-lists/live.list.chroot|base live package list"
  "iso/live-build/config/package-lists/straylight.list.chroot|StrayLight package list"
  "iso/live-build/config/includes.chroot/|chroot include tree"
  "iso/live-build/config/hooks/normal/|live-build hook directory"
  "iso/calamares/settings.conf|Calamares settings"
  "iso/calamares/modules/|Calamares module configuration"
  "scripts/build-iso.sh|ISO build wrapper"
)

generated_paths=(
  "output/debs/Packages.gz|local package repository index"
)

if [ "$mode" != "source" ]; then
  echo "ISO host package prerequisite check"
  if ! command -v dpkg-query >/dev/null 2>&1; then
    echo "[MISSING_HOST_TOOL] dpkg-query: install or run on a Debian-compatible host" >&2
    fail=1
  else
    for pkg in "${host_packages[@]}"; do
      if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -qx 'install ok installed'; then
        echo "[OK_HOST_PACKAGE] $pkg"
      else
        echo "[MISSING_HOST_PACKAGE] $pkg" >&2
        fail=1
      fi
    done
  fi
fi

if [ "$mode" != "host" ]; then
  echo "ISO source payload check"
  for entry in "${source_paths[@]}"; do
    IFS='|' read -r path description <<EOF
$entry
EOF
    if [ -e "$root/$path" ]; then
      echo "[OK_ISO_PATH] $path"
    else
      echo "[MISSING_ISO_PATH] $path - $description" >&2
      fail=1
    fi
  done
fi

if [ "$mode" = "all" ]; then
  echo "ISO generated prerequisite check"
  for entry in "${generated_paths[@]}"; do
    IFS='|' read -r path description <<EOF
$entry
EOF
    if [ -e "$root/$path" ]; then
      echo "[OK_GENERATED_PATH] $path"
    else
      echo "[MISSING_GENERATED_PATH] $path - $description" >&2
      fail=1
    fi
  done
fi

if [ "$fail" -ne 0 ]; then
  echo "ISO candidate requirements check failed" >&2
  exit 1
fi

echo "ISO candidate requirements check passed"
