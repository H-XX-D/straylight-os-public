# Changelog

All notable public starter repository changes are recorded here.

This repository follows source-snapshot versioning for public documentation,
examples, release hygiene, and build guidance. Tags do not imply a supported
binary distribution unless a release explicitly attaches and verifies an ISO or
package artifact.

## Unreleased

### Added

- Public ISO build wrapper `scripts/build-iso.sh`.

### Changed

- ISO candidate requirement checks now separate repository-controlled source
  paths from generated package repository output.
- Package build wrapper now points `straylight-common` at the published
  `packaging/straylight-common` directory.
- Package builds now stage repository-root build trees for Debian packaging and
  collect generated `.deb` artifacts under `output/debs` for ISO input.
- `straylight-kernel` DKMS post-install builds now target installed kernel
  header trees explicitly instead of the live-build host kernel.
- Release hygiene now treats `output/` as the generated artifact boundary during
  package and ISO build workflows.
- `straylight-core` packaging now installs the `straylight-numa-run` helper
  emitted by the core package build.
- Release audit matching now uses per-run temporary files so root and non-root
  validation paths cannot collide on stale `/tmp` permissions.
- Release audit now excludes generated live-build work directories and local
  package injection state.
- Release audit now excludes generated live-build package, file, contents, log,
  and timestamp reports emitted by completed ISO builds.
- Public snapshot Markdown and shell checks now skip generated live-build
  worktrees so post-build validation stays scoped to repository source.
- UEFI amd64 VM boot validation now reaches the GNOME live session for the
  generated ISO candidate.
- Calamares StrayLight branding now ships all image and slideshow assets
  referenced by `branding.desc`, fixing the installer launch path.
- Calamares settings now use supported `shellprocess@...` instance keys for
  StrayLight hardware validation and post-install hooks instead of unresolved
  custom module names.
- ISO live-build SGX udev includes now match the packaged source rule emitted
  during config-only staging.
- Status, roadmap, FAQ, and build docs now reflect that package builds,
  package repository generation, ISO builds, checksum verification, and UEFI VM
  boot validation have passed.
- Calamares installer validation now passes in a generic UEFI amd64 QEMU/KVM VM
  with a blank 64 GiB-class virtual disk.
- Installed-disk inspection now confirms generated GRUB configuration plus both
  the StrayLight EFI loader and removable-media fallback loader.
- Firstboot validation now passes from the installed virtual disk with the ISO
  detached and reaches the graphical login target.
- `straylight-core` now packages the health daemon, health CLI, default health
  configuration, systemd unit, and compatibility CLI symlink.
- Post-install health command validation now passes on the installed VM with
  the health service active/enabled and warning-state JSON documented.
- Status and validation docs now reflect the final ISO checksum and completed
  VM validation gates.

## v0.4.0-alpha - 2026-07-05

Source-only alpha snapshot covering public release hygiene and validation
documentation through the VM boot, installer, firstboot, and post-install
health checklist gates.

### Added

- Public post-install health checklist in
  `docs/POST_INSTALL_HEALTH_CHECKLIST.md`.
- Public installer and firstboot validation procedure in
  `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`.
- Public VM boot validation procedure in `docs/VM_BOOT_VALIDATION.md`.
- ISO candidate checksum and release-note flow in
  `docs/ISO_CANDIDATE_RELEASE.md`.
- Public ISO checksum wrapper `scripts/generate_iso_checksum.sh`.

### Current Boundaries

- This is a source snapshot, not a supported binary distribution.
- No ISO, package, kernel module, VM disk, trace, or generated binary artifact is
  attached to this tag.
- VM boot, installer, firstboot, and post-install health procedures are
  documented, but validation runs still need to pass for a verified ISO.
- Private lab paths, hostnames, addresses, machine identifiers, and credentials
  remain intentionally excluded.

## v0.1.0-alpha - 2026-07-05

Initial public alpha starter snapshot.

### Added

- Professional README for StrayLight OS as a Debian/Ubuntu-compatible operating
  environment for hardware-aware AI, systems automation, kernel-surface
  observability, high-performance networking, and recovery-focused workstation
  workflows.
- Sanitized design lineage and research-track notes covering the
  William Gibson / *Neuromancer* throwback, swarm experiments, GA-GPT2
  transformer experiments, and AURA/XIT packet work.
- MIT license.
- ISO build requirements in `docs/BUILD_ISO.md`, including host packages,
  source tree expectations, package build order, root/live-build requirements,
  and validation gates.
- Public getting-started, current-status, surface-map, network, app CLI,
  privacy, publication-checklist, and package-split documentation.
- Sanitized starter examples for hardware fabric, XDP configuration, package
  profiles, and ISO build environment variables.
- Public release hygiene scripts for sanitization, Markdown link checks, and
  shell syntax checks.
- GitHub Actions workflow for public release checks.
- Contribution, security, code of conduct, pull request, and issue templates.

### Current Boundaries

- This is a starter source snapshot, not a complete public source release.
- No ISO, package, kernel module, VM disk, trace, or generated binary artifact is
  attached to this tag.
- ISO boot, installer, firstboot, and post-install validation remain gated.
- Private lab paths, hostnames, addresses, machine identifiers, and credentials
  are intentionally excluded.
