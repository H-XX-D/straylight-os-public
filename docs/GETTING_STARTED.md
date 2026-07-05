# Getting Started

This public repository is a starter snapshot for building a StrayLight-style
Debian/Ubuntu-compatible operating environment. It is intentionally sanitized:
it contains documentation, package layout guidance, example manifests, and
release hygiene scripts, not a full private workstation image.

## Prerequisites

Use a Debian Trixie-compatible amd64 build host.

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config rsync \
  dpkg-dev debhelper dh-cmake devscripts \
  dkms linux-headers-amd64

sudo apt-get install -y \
  live-build debootstrap squashfs-tools xorriso \
  grub-pc-bin grub-efi-amd64-bin
```

## Expected Repository Shape

A complete StrayLight source tree should provide these top-level areas:

- `apps/` for graphical tools and widgets.
- `bin/` and `tools/` for CLI entrypoints.
- `services/` for daemons and systemd-facing services.
- `kernel/` for DKMS modules and eBPF/XDP sources.
- `lib/` for shared libraries.
- `packaging/` for Debian package groups.
- `iso/` for live-build and installer configuration.
- `docs/` for public documentation.
- `scripts/` for release audit, package build, and ISO build commands.

## Package Build Flow

```bash
scripts/straylight_release_audit.sh .
scripts/check_package_dependencies.sh .
scripts/build-packages.sh --check-deps --no-sign
scripts/build-packages.sh --clean --no-sign
```

This public starter does not include every implementation file needed to produce
an ISO by itself. Use it as the public documentation and hygiene frame for a
complete source release. See [Build The ISO](BUILD_ISO.md) for the full source
layout, package payload, root/live-build requirements, and validation gates.
See [Clean-Clone Package Dependency Check](CLEAN_CLONE_PACKAGE_CHECK.md) for the
preflight command and expected starter failure modes.

## ISO Candidate Flow

Run this only after package builds have produced `output/debs/Packages.gz`:

```bash
scripts/check_iso_candidate_requirements.sh .
sudo scripts/build-iso.sh --clean --config-only
sudo scripts/build-iso.sh --clean
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

## Starter Examples

Use the sanitized examples as a starting point for a complete private or public
source tree:

- [`examples/hardware-fabric.yaml`](../examples/hardware-fabric.yaml)
- [`examples/xdp.conf`](../examples/xdp.conf)
- [`examples/package-profile.json`](../examples/package-profile.json)
- [`examples/iso-build.env`](../examples/iso-build.env)

Keep deployment-specific values in private repos or local configuration files.
