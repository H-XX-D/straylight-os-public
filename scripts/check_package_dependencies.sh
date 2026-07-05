#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/check_package_dependencies.sh [--host-only|--source-only] [ROOT]

Checks whether a clean StrayLight source tree has the host packages and public
source payload paths required before package builds are attempted.

Use --source-only to verify the repository payload without requiring Debian
build packages to be installed on the current host.
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
  "libstraylight-common1|lib/common/src/|shared runtime library source"
  "libstraylight-common1|lib/common/include/straylight/|public runtime headers"
  "libstraylight-common1|cmake/|CMake package metadata"
  "libstraylight-common1|packaging/straylight-common/debian/|Debian package rules"
  "libstraylight-common-dev|lib/common/include/straylight/|development headers"
  "libstraylight-common-dev|cmake/Straylight*.cmake|development CMake metadata"
  "libstraylight-common-dev|packaging/straylight-common/debian/|development package rules"
  "straylight-core|bin/core/|core service source"
  "straylight-core|services/|daemon source"
  "straylight-core|tools/|control CLI source"
  "straylight-core|etc/systemd/|systemd unit files"
  "straylight-core|services/dbus/|D-Bus policy and service files"
  "straylight-core|services/udev/|udev rules"
  "straylight-core|packaging/straylight-core/debian/|Debian package rules"
  "straylight-kernel|kernel/|kernel module source"
  "straylight-kernel|kernel/dkms/|DKMS metadata"
  "straylight-kernel|kernel/xdp/|XDP/eBPF source layout"
  "straylight-kernel|packaging/straylight-kernel/debian/|Debian package rules"
  "straylight-ml|lib/ml/|ML subsystem source"
  "straylight-ml|services/predict/|prediction subsystem source"
  "straylight-ml|bin/quantum/|quantum research subsystem source"
  "straylight-ml|bin/photonics/|photonics research subsystem source"
  "straylight-ml|bin/snn/|SNN subsystem source"
  "straylight-ml|packaging/straylight-ml/debian/|Debian package rules"
  "straylight-network|lib/net/|network subsystem source"
  "straylight-network|kernel/xdp/|eBPF/XDP source"
  "straylight-network|bin/xdp/|XDP control source"
  "straylight-network|services/bridge/|bridge subsystem source"
  "straylight-network|services/swarm/|swarm subsystem source"
  "straylight-network|services/mesh/|mesh transport subsystem source"
  "straylight-network|etc/systemd/system/straylight-xdp@.service|XDP systemd unit"
  "straylight-network|packaging/straylight-network/debian/|Debian package rules"
  "straylight-exotic|bin/enclave/|enclave subsystem source"
  "straylight-exotic|bin/pmem/|persistent-memory subsystem source"
  "straylight-exotic|bin/fuse/|FUSE subsystem source"
  "straylight-exotic|tools/sandbox/|sandbox subsystem source"
  "straylight-exotic|bin/rhem/|RHEM subsystem source"
  "straylight-exotic|packaging/straylight-exotic/debian/|Debian package rules"
  "straylight-desktop|apps/|desktop app source"
  "straylight-desktop|assets/|desktop assets"
  "straylight-desktop|apps/widgets/|widget source"
  "straylight-desktop|apps/oobe/|OOBE source"
  "straylight-desktop|apps/wizard/|wizard source"
  "straylight-desktop|services/firstboot/|firstboot source"
  "straylight-desktop|apps/app-cli/|app CLI source"
  "straylight-desktop|packaging/straylight-desktop/debian/|Debian package rules"
  "straylight-os|config/|install profile metadata"
  "straylight-os|iso/live-build/auto/|live-build auto scripts"
  "straylight-os|iso/live-build/config/package-lists/|live-build package lists"
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
