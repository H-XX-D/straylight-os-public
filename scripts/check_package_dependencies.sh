#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/check_package_dependencies.sh [--host-only|--source-only] [ROOT]

Checks whether a clean StrayLight source tree has the host packages and public
source payload paths required before package builds are attempted.

The current public starter is expected to report missing source payload paths
until the v0.2.0-alpha complete-source-tree gate is finished.
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
  build-essential
  cmake
  ninja-build
  pkg-config
  rsync
  dpkg-dev
  debhelper
  dh-cmake
  devscripts
  dkms
  linux-headers-amd64
)

source_paths=(
  "libstraylight-common1|src/common/|shared runtime library source"
  "libstraylight-common1|include/straylight/|public runtime headers"
  "libstraylight-common1|cmake/|CMake package metadata"
  "libstraylight-common1|packaging/libstraylight-common/debian/|Debian package rules"
  "libstraylight-common-dev|include/straylight/|development headers"
  "libstraylight-common-dev|cmake/StrayLight*.cmake|development CMake metadata"
  "libstraylight-common-dev|packaging/libstraylight-common/debian/|development package rules"
  "straylight-core|src/core/|core service source"
  "straylight-core|src/daemons/|daemon source"
  "straylight-core|src/cli/|control CLI source"
  "straylight-core|systemd/|systemd unit files"
  "straylight-core|dbus/|D-Bus policy and service files"
  "straylight-core|udev/|udev rules"
  "straylight-core|packaging/straylight-core/debian/|Debian package rules"
  "straylight-kernel|kernel/|kernel module source"
  "straylight-kernel|kernel/dkms/|DKMS metadata"
  "straylight-kernel|kernel/modules/|module source layout"
  "straylight-kernel|modprobe.d/|module options"
  "straylight-kernel|modules-load.d/|module-load configuration"
  "straylight-kernel|packaging/straylight-kernel/debian/|Debian package rules"
  "straylight-ml|src/ml/|ML subsystem source"
  "straylight-ml|src/predict/|prediction subsystem source"
  "straylight-ml|src/quantum/|quantum research subsystem source"
  "straylight-ml|src/photonics/|photonics research subsystem source"
  "straylight-ml|src/snn/|SNN subsystem source"
  "straylight-ml|packaging/straylight-ml/debian/|Debian package rules"
  "straylight-network|src/network/|network subsystem source"
  "straylight-network|src/bpf/|eBPF source"
  "straylight-network|src/xdp/|XDP source"
  "straylight-network|src/bridge/|bridge subsystem source"
  "straylight-network|src/swarm/|swarm subsystem source"
  "straylight-network|src/transport/|transport subsystem source"
  "straylight-network|systemd/straylight-xdp@.service|XDP systemd unit"
  "straylight-network|packaging/straylight-network/debian/|Debian package rules"
  "straylight-exotic|src/enclave/|enclave subsystem source"
  "straylight-exotic|src/pmem/|persistent-memory subsystem source"
  "straylight-exotic|src/fuse/|FUSE subsystem source"
  "straylight-exotic|src/sandbox/|sandbox subsystem source"
  "straylight-exotic|src/rhem/|RHEM subsystem source"
  "straylight-exotic|packaging/straylight-exotic/debian/|Debian package rules"
  "straylight-desktop|apps/|desktop app source"
  "straylight-desktop|desktop/|desktop integration files"
  "straylight-desktop|widgets/|widget source"
  "straylight-desktop|oobe/|OOBE source"
  "straylight-desktop|wizard/|wizard source"
  "straylight-desktop|firstboot/|firstboot source"
  "straylight-desktop|src/app-cli/|app CLI source"
  "straylight-desktop|packaging/straylight-desktop/debian/|Debian package rules"
  "straylight-os|profiles/straylight-os/|install profile metadata"
  "straylight-os|iso/live-build/|live-build configuration"
  "straylight-os|iso/calamares/|installer configuration"
  "straylight-os|packaging/straylight-os/debian/|metapackage rules"
)

path_exists() {
  local path="$1"
  case "$path" in
    *[\*\?[]*)
      compgen -G "$root/$path" >/dev/null
      ;;
    *)
      [ -e "$root/$path" ]
      ;;
  esac
}

if [ "$mode" != "source" ]; then
  echo "Host package prerequisite check"
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
  echo "Public source payload check"
  for entry in "${source_paths[@]}"; do
    IFS='|' read -r package path description <<EOF
$entry
EOF
    if path_exists "$path"; then
      echo "[OK_SOURCE_PATH] $package $path"
    else
      echo "[MISSING_SOURCE_PATH] $package $path - $description" >&2
      fail=1
    fi
  done
fi

if [ "$fail" -ne 0 ]; then
  echo "package dependency check failed" >&2
  exit 1
fi

echo "package dependency check passed"
