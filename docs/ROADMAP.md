# Roadmap

This roadmap is gate-based. It describes the work needed to move from a public
alpha source snapshot toward a complete, reproducible public StrayLight OS
release.

## Current Stage: Public Alpha Source Snapshot

Status: active.

The public repository currently provides:

- Professional README and public documentation.
- MIT license.
- Sanitized examples.
- ISO build requirements and source layout expectations.
- Release hygiene scripts and GitHub Actions.
- Community, support, security, issue, and pull request templates.
- Source-only pre-release tag `v0.1.0-alpha`.
- Public package source payloads that pass
  `scripts/check_package_dependencies.sh --source-only .`.
- Public ISO source paths that pass
  `scripts/check_iso_candidate_requirements.sh --source-only .`.

Boundary: this does not yet ship verified package or ISO artifacts.

## Next Gate: Public Package Build Validation

Goal: prove that outside users can build package groups from a fresh clone on a
prepared Debian-compatible build host.

Required outcomes:

- Public implementations for package payloads named in the package split remain
  present.
- Package payload inventory maps each group to required source paths.
- Clean-clone package dependency preflight passes its source-only mode.
- Intentionally excluded implementation areas are documented with inclusion
  gates.
- Public package build wrapper is present and runnable from the repository root.
- Generated output remains ignored and absent from source.
- Package build order is reproducible.
- Release hygiene checks remain green.

Exit criteria:

- A clean clone can run package dependency checks.
- Package builds complete on a prepared Debian-compatible host, or any failure
  is documented as a package-build blocker.
- Documentation states any intentionally excluded implementation areas.
- `docs/PACKAGE_PAYLOAD_INVENTORY.md` has no unresolved package groups.
- `docs/EXCLUDED_IMPLEMENTATION_AREAS.md` has no unresolved source-boundary
  entries for included package payloads.

## Next Gate: Reproducible ISO Candidate

Goal: produce an ISO candidate from a clean public source tree.

Required outcomes:

- Public live-build configuration is complete.
- Live-build source requirements and checker are documented.
- Package repository generation is reproducible.
- ISO build script documents root requirements and host package dependencies.
- SHA256 checksum is generated.
- No private lab files enter the build context.

Exit criteria:

- ISO candidate builds from documented commands.
- Release notes include host profile, commands, checksum, and known limitations.
- The artifact is marked as an ISO candidate, not a verified release.

## Next Gate: VM Boot And Installer Validation

Goal: prove that the ISO candidate boots and installs in a clean virtual
machine.

Required outcomes:

- Boot path is documented in `docs/VM_BOOT_VALIDATION.md`.
- Installer path is documented in `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`.
- Firstboot behavior is documented in
  `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`.
- Post-install health checks are documented in
  `docs/POST_INSTALL_HEALTH_CHECKLIST.md`.

Exit criteria:

- VM boot log and installer result are summarized without private host details.
- Firstboot completes.
- Post-install StrayLight health commands run successfully or failures are
  documented as release blockers.

## Next Gate: Verified Public Release

Goal: publish a release that can reasonably be consumed by outside testers.

Required outcomes:

- Source, package, and ISO build paths are documented and reproducible.
- Artifacts have checksums.
- CI and release hygiene pass.
- Release notes distinguish verified behavior from alpha or experimental paths.
- Security, support, and contribution docs are current.

Exit criteria:

- GitHub release attaches verified artifacts.
- Release notes include build host class, commands, checksums, boot/install
  validation, firstboot validation, and post-install health validation.

## Research Tracks

The following tracks remain active research and should stay clearly labeled
until their validation gates are defined:

- Swarm scheduling and node coordination.
- GA-GPT2 transformer experiments.
- AURA/XIT packet work.
- Device Memory ABI and Ledger work.
- Hardware acceleration paths beyond the documented public source surface.
