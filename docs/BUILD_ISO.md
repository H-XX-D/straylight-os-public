# Build The ISO

This document describes what is actually required to build a StrayLight ISO.
The public source snapshot is a sanitized documentation and release-hygiene
frame. It is not, by itself, the full implementation tree needed to produce an
installable ISO unless the complete package and ISO payloads are present.

## Required Host

Use a Debian Bookworm or Trixie compatible amd64 build host. The current public
README and source handoff assume Trixie for the live-build target.

The ISO build must run as root because `live-build` creates and configures a
chroot.

## Required System Packages

Install package-build tooling:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config rsync \
  dpkg-dev debhelper dh-cmake devscripts \
  dkms linux-headers-amd64
```

Install ISO/live-build tooling:

```bash
sudo apt-get install -y \
  live-build debootstrap squashfs-tools xorriso \
  grub-pc-bin grub-efi-amd64-bin
```

The public live-build source requirements and checker are documented in
`docs/LIVE_BUILD_REQUIREMENTS.md`.

Optional VM smoke-test tooling:

```bash
sudo apt-get install -y qemu-system-x86 ovmf
```

## Required Source Tree

A buildable StrayLight source tree needs the complete implementation payload. At
minimum, it must contain:

- `scripts/straylight_release_audit.sh`
- `scripts/build-packages.sh`
- `scripts/build-iso.sh`
- `packaging/<package>/debian/` directories for each package group
- source directories referenced by those package rules, including `apps/`,
  `bin/`, `tools/`, `services/`, `kernel/`, `lib/`, and `etc/`
- `iso/live-build/auto/config`
- `iso/live-build/config/package-lists/live.list.chroot`
- `iso/live-build/config/package-lists/straylight.list.chroot`
- `iso/calamares/settings.conf`
- `iso/calamares/modules/*.conf`
- service D-Bus policy files under `services/dbus/`
- udev rules under `services/udev/`
- optional theme and icon assets under `etc/straylight/themes/` and
  `assets/icons/`

The release packaging profile currently excludes `straylight-compositor` and
`straylight-shell`. The desktop path uses the distro GNOME/GDM/Mutter stack.

## Package Build Order

`scripts/build-packages.sh` builds packages in dependency order:

1. `straylight-common`
2. `straylight-kernel`
3. `straylight-core`
4. `straylight-ml`
5. `straylight-network`
6. `straylight-exotic`
7. `straylight-desktop`
8. `straylight-os`

Run the preflight dependency check first:

```bash
scripts/straylight_release_audit.sh .
scripts/check_package_dependencies.sh .
scripts/build-packages.sh --check-deps --no-sign
```

An incomplete source snapshot is expected to fail
`scripts/check_package_dependencies.sh .` with `[MISSING_SOURCE_PATH]` entries
until the complete public source tree is published. See
`docs/CLEAN_CLONE_PACKAGE_CHECK.md` for host prerequisites, source payload
checks, and failure classes.

Build all package groups:

```bash
scripts/build-packages.sh --clean --no-sign
scripts/generate_package_repo.sh
```

The public wrapper is documented in `docs/PACKAGE_BUILD_WRAPPER.md`. On an
incomplete source snapshot, it fails before build execution with public-safe
missing payload messages. Once the complete source tree is present, it invokes
`dpkg-buildpackage` for package groups in dependency order.

Expected package output:

- runtime `.deb` files under `output/debs/`
- `output/debs/Packages`
- `output/debs/Packages.gz`
- a timestamped package build log under `output/debs/`

The ISO builder injects runtime `.deb` files from `output/debs/` into
`iso/live-build/config/packages.chroot/`. It skips debug symbol and development
packages when staging runtime payloads.

The package repository generation flow is documented in
`docs/PACKAGE_REPOSITORY.md`.

## Live ISO Package Payload

The live-build package lists include the StrayLight metapackage plus the base
live environment, installer, firmware, hardware support, networking, desktop,
fonts, and utility packages.

Core payload classes:

- `straylight-os`
- Linux kernel image and headers
- `live-boot`, `live-config`, and systemd live integration
- Calamares installer
- DKMS and hardware inspection tools
- NetworkManager, wireless support, and DHCP client
- GNOME Shell, GDM, GNOME session services, terminal, Files, Xwayland, Mesa,
  PipeWire, and desktop portals
- Noto, emoji, Liberation, and DejaVu fonts
- common operator utilities such as `curl`, `wget`, `less`, `nano`, `htop`,
  `zstd`, `gnupg`, and `ca-certificates`

Firmware packages such as Linux free/non-free firmware, Wi-Fi firmware, Realtek,
Atheros, and miscellaneous non-free firmware are part of the current live image
profile. Check local redistribution requirements before publishing ISO media.

## Configure The ISO

Run config-only mode first. This validates release sanitization, configures
live-build, stages local package payloads, and copies installer/service/theme
assets into the chroot skeleton.

```bash
scripts/check_iso_candidate_requirements.sh .
sudo scripts/build-iso.sh --clean --config-only
```

Useful options:

```bash
sudo scripts/build-iso.sh --clean --config-only --distribution trixie
sudo scripts/build-iso.sh --clean --config-only --apt-recommends false
sudo scripts/build-iso.sh --clean --config-only --repo <apt-repository-url>
```

## Build The ISO

After package and config-only gates pass:

```bash
sudo scripts/build-iso.sh --clean
scripts/generate_iso_checksum.sh output/straylight-os-1.0.0-amd64.iso
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

Expected ISO outputs:

- `output/straylight-os-1.0.0-amd64.iso`
- `output/straylight-os-1.0.0-amd64.iso.sha256`
- `output/iso-build-<timestamp>.log`

The ISO candidate release-note and checksum flow is documented in
`docs/ISO_CANDIDATE_RELEASE.md`.

## Smoke Test

The build script prints a QEMU command after a successful ISO build. Public VM
boot validation is documented in `docs/VM_BOOT_VALIDATION.md`; installer and
firstboot validation is documented in
`docs/INSTALLER_FIRSTBOOT_VALIDATION.md`; post-install health validation is
documented in `docs/POST_INSTALL_HEALTH_CHECKLIST.md`.

A typical BIOS-compatible smoke test uses:

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm:tcg \
  -cpu max \
  -smp 4 \
  -m 8192 \
  -boot d \
  -cdrom output/straylight-os-1.0.0-amd64.iso \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
```

Before treating an ISO as distributable, verify:

- VM boot reaches the documented live target and is summarized according to
  `docs/VM_BOOT_VALIDATION.md`.
- Calamares launches and completes an install according to
  `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`.
- Firstboot reaches the documented target with the ISO detached.
- Post-install health checks pass according to
  `docs/POST_INSTALL_HEALTH_CHECKLIST.md`.
- Kernel and XDP surfaces behave according to the target hardware policy.

## Common Failure Points

- Running `build-iso.sh` without root privileges.
- Missing `live-build`, `debootstrap`, `xorriso`, or GRUB binary packages.
- Building the ISO before `output/debs/` contains runtime StrayLight packages.
- Publishing generated `output/`, live-build `chroot/`, or package artifacts as
  source.
- Including private hostnames, local interfaces, serials, machine IDs, or live
  lab paths in docs or configs.
- Claiming production readiness before boot, install, firstboot, and health
  gates pass.
