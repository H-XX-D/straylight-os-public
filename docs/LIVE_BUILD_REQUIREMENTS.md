# Live-Build Requirements

This document lists the public `iso/` and live-build configuration required
before StrayLight can publish an ISO candidate.

The current repository includes the public `iso/` implementation paths and a
requirements checker. Producing an ISO candidate still requires a generated
local package repository under `output/debs/` and live-build host tooling.

## Required Host

Use a Debian Bookworm or Trixie compatible amd64 host. The current live-build
target is Trixie.

The actual ISO build must run as root because live-build creates and configures
a chroot. The requirements checker can run as a normal user.

Install live-build host packages:

```bash
sudo apt-get update
sudo apt-get install -y \
  live-build debootstrap squashfs-tools xorriso \
  grub-pc-bin grub-efi-amd64-bin
```

## Required Public Paths

An ISO-candidate source tree must include:

| Path | Purpose |
|------|---------|
| `scripts/build-iso.sh` | Public ISO build wrapper |
| `iso/live-build/auto/config` | live-build configuration entrypoint |
| `iso/live-build/config/package-lists/live.list.chroot` | Base live environment package list |
| `iso/live-build/config/package-lists/straylight.list.chroot` | StrayLight package list, including `straylight-os` |
| `iso/live-build/config/includes.chroot/` | Files staged into the live chroot |
| `iso/live-build/config/hooks/normal/` | live-build hooks used during image creation |
| `iso/calamares/settings.conf` | Installer settings |
| `iso/calamares/modules/` | Installer module configuration |
| `output/debs/Packages.gz` | Local package repository index produced by package builds |

Generated live-build directories such as `chroot/`, `binary/`, `.build/`, and
`cache/` must remain excluded from source commits.

## Run The Check

From the repository root:

```bash
scripts/check_iso_candidate_requirements.sh .
```

Host package checks only:

```bash
scripts/check_iso_candidate_requirements.sh --host-only .
```

ISO source path checks only:

```bash
scripts/check_iso_candidate_requirements.sh --source-only .
```

## Output Classes

| Output | Meaning |
|--------|---------|
| `[OK_HOST_PACKAGE]` | Required host package is installed. |
| `[MISSING_HOST_PACKAGE]` | Install the named host package before building an ISO. |
| `[MISSING_HOST_TOOL]` | Run on a Debian-compatible host with the named tool. |
| `[OK_ISO_PATH]` | Required repository-relative ISO source path exists. |
| `[MISSING_ISO_PATH]` | Required ISO source path is absent. |
| `[OK_GENERATED_PATH]` | Required generated build input exists. |
| `[MISSING_GENERATED_PATH]` | Required generated build input is absent; usually build packages first. |

The full command exits non-zero when any host package, ISO source path, or
generated prerequisite is missing. `--source-only` checks repository-controlled
ISO paths and intentionally skips generated package output.

## Private Build Context Boundary

Do not include private lab files in the ISO build context:

- private hostnames, local addresses, MAC addresses, serials, or machine IDs
- local interface names or topology files
- credentials, keys, tokens, `.env` files, or private certificates
- raw logs, packet captures, traces, model caches, or benchmark outputs
- generated live-build state, package artifacts, VM disks, or ISO images

Use sanitized examples and repository-relative paths only.

## ISO Candidate Gate

The `v0.3.0-alpha` ISO candidate gate is not complete until:

- package builds produce `output/debs/Packages.gz`
- `scripts/generate_package_repo.sh` produces the local package index from
  `output/debs/*.deb`
- the required `iso/` paths above are present
- `scripts/check_iso_candidate_requirements.sh .` passes on a clean public
  build host
- `sudo scripts/build-iso.sh --clean --config-only` succeeds
- `sudo scripts/build-iso.sh --clean` produces an ISO and checksum
- `scripts/generate_iso_checksum.sh --check` verifies the ISO checksum
- release notes mark the artifact as an ISO candidate, not a verified ISO
