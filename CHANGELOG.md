# Changelog

All notable public starter repository changes are recorded here.

This repository follows source-snapshot versioning for public documentation,
examples, release hygiene, and build guidance. Tags do not imply a supported
binary distribution unless a release explicitly attaches and verifies an ISO or
package artifact.

## Unreleased

### Added

- Public VM boot validation procedure in `docs/VM_BOOT_VALIDATION.md`.
- ISO candidate checksum and release-note flow in
  `docs/ISO_CANDIDATE_RELEASE.md`.
- Public ISO checksum wrapper `scripts/generate_iso_checksum.sh`.

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
