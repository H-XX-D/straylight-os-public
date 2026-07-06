# StrayLight OS 1.0.0 ISO Candidate

## Release Type

- [ ] Source snapshot
- [x] ISO candidate
- [ ] Verified ISO

## Summary

This candidate records the first public StrayLight OS ISO build that passed
package build, ISO build, checksum verification, UEFI VM live boot, graphical
installer, installed-disk inspection, and firstboot-to-login validation.

It remains alpha test media until post-install health validation passes and a
release explicitly promotes the artifact to a verified ISO.

## Current Status

| Area | Status |
|------|--------|
| Source tree | complete public source snapshot |
| Package builds | passed |
| ISO build | passed |
| VM boot | passed |
| Installer | passed |
| Firstboot | passed |
| Post-install health | not run |

## Build Host Class

- Distribution: Debian-compatible amd64 build host
- Architecture: amd64 / x86_64
- CPU/RAM class: multi-core host, 8 GiB RAM class or larger recommended
- Required packages: see `docs/BUILD_ISO.md` and
  `docs/LIVE_BUILD_REQUIREMENTS.md`
- Privilege requirements: package build as an unprivileged user where possible;
  live-build ISO assembly with root privileges

## Commands

```bash
scripts/verify_public_snapshot.sh .
scripts/build-packages.sh --clean --no-sign
scripts/generate_package_repo.sh
scripts/check_iso_candidate_requirements.sh .
sudo scripts/build-iso.sh --clean
scripts/generate_iso_checksum.sh output/straylight-os-1.0.0-amd64.iso
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
sha256sum -c output/straylight-os-1.0.0-amd64.iso.sha256
```

## Artifacts

| Artifact | SHA256 | Notes |
|----------|--------|-------|
| `output/straylight-os-1.0.0-amd64.iso` | `0c74fc9df806609cbf74e0333ce15c6438053302165a6f39b31b4efe8f905373` | ISO candidate, not a verified ISO |
| `output/straylight-os-1.0.0-amd64.iso.sha256` | generated sidecar | Checksum sidecar for the ISO candidate |

## Validation Results

### Package Build

- Command: `scripts/build-packages.sh --clean --no-sign`
- Result: passed
- Notes: 8 package groups built and generated a local APT repository under
  `output/debs/`.

### ISO Build

- Command: `sudo scripts/build-iso.sh --clean`
- Result: passed
- Notes: live-build produced `output/straylight-os-1.0.0-amd64.iso`.

### VM Boot

- Runtime class: generic amd64 QEMU/KVM VM, 4 vCPU class, 8 GiB RAM class
- Firmware mode: UEFI
- Boot target: GNOME live session
- Result: passed
- Failure class: none
- Public summary: The ISO booted through the UEFI path and reached the GNOME
  live desktop.

### Installer

- Runtime class: generic amd64 QEMU/KVM VM, 4 vCPU class, 8 GiB RAM class
- Disk class: blank 64 GiB-class virtual disk
- Path: graphical Calamares installer, clean-disk install
- Result: passed
- Failure class: none
- Public summary: The installer completed against the blank test disk and
  reached the completion screen.

### Firstboot

- Boot source: installed virtual disk, ISO detached
- Target: graphical login
- Result: passed
- Failure class: none
- Public summary: The installed disk booted without the ISO attached and reached
  the graphical login target.

### Post-Install Health

- Runtime class: installed amd64 VM
- Commands: package, systemd, health CLI, app CLI, and required surface checks
- Result: not run
- Failure class: `not-run`
- Blockers: post-install health evidence is not yet available
- Known alpha limitations: optional hardware accelerator and research-track
  features remain gated
- Public summary: Post-install health remains the final gate before this
  candidate can be promoted to a verified ISO.

## Verified Behavior

- Public source snapshot hygiene passes.
- Package build and local package repository generation pass.
- ISO build and checksum verification pass.
- Generic UEFI amd64 VM live boot reaches the GNOME desktop.
- Clean-disk graphical installation completes.
- Installed disk contains GRUB configuration, the StrayLight EFI loader, and the
  removable-media EFI fallback loader.
- Firstboot from the installed disk reaches graphical login.

## Gated Or Experimental Behavior

- Post-install health validation is not complete.
- Hardware-specific accelerator validation remains gated.
- XDP filter and redirect behavior require an explicit packet policy before
  production use.
- Swarm, GA-GPT2, AURA/XIT, Device Memory ABI, and ledger tracks remain
  research-only.

## Known Limitations

- This is alpha test media, not a supported production distribution.
- Real-hardware installation, upgrade behavior, and hardware accelerator paths
  are not validated by this VM candidate.
- Post-install service, CLI, app, and required surface health checks remain to
  be run on the installed VM.

## Security And Privacy Review

- [x] No private hostnames, local IPs, MAC addresses, serials, machine IDs,
  personal paths, credentials, or logs are included.
- [x] Generated artifacts are excluded from the source snapshot.
- [x] Public claims match `docs/VALIDATION_MATRIX.md`.
- [x] Security-sensitive details remain outside the public release notes.

## Upgrade Or Compatibility Notes

No upgrade path is promised for this alpha ISO candidate.
