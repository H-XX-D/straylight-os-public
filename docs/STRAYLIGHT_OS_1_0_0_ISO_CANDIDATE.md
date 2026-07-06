# StrayLight OS 1.0.0 ISO Candidate

## Release Type

- [ ] Source snapshot
- [x] ISO candidate
- [ ] Verified ISO

## Summary

This candidate records the first public StrayLight OS ISO build that passed
package build, ISO build, checksum verification, UEFI VM live boot, graphical
installer, installed-disk inspection, firstboot-to-login validation, and
post-install health command validation in a generic installed VM.

It remains alpha test media until an artifact-bearing release explicitly
promotes the artifact and any remaining real-hardware validation scope is
documented.

## Current Status

| Area | Status |
|------|--------|
| Source tree | complete public source snapshot |
| Package builds | passed |
| ISO build | passed |
| VM boot | passed |
| Installer | passed |
| Firstboot | passed |
| Post-install health | passed with expected VM warnings |

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
| `output/straylight-os-1.0.0-amd64.iso` | `d158634893f647ec79947a76264a0f6b4ac65f74c7846fdcdb306fcc10347d06` | ISO candidate with VM validation complete, not a supported production ISO |
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
- Commands: systemd health service state and `straylight-health status --json`
- Result: passed with expected VM warnings
- Failure class: none for required command execution
- Health service: active and enabled
- Health CLI result: returned JSON with `overall_score` 77 and
  `overall_status` `warn`
- Known alpha limitations: optional hardware accelerator and research-track
  features remain gated; a generic QEMU VM reported expected warnings for SMART
  status, gateway/internet reachability, and StrayLight research services that
  are present but not all active in the VM profile
- Public summary: The installed VM exposes the packaged health service and CLI,
  and the required health command path completes. The result is a warning-state
  alpha VM profile, not a production-health claim.

## Verified Behavior

- Public source snapshot hygiene passes.
- Package build and local package repository generation pass.
- ISO build and checksum verification pass.
- Generic UEFI amd64 VM live boot reaches the GNOME desktop.
- Clean-disk graphical installation completes.
- Installed disk contains GRUB configuration, the StrayLight EFI loader, and the
  removable-media EFI fallback loader.
- Firstboot from the installed disk reaches graphical login.
- Installed system includes the StrayLight health daemon, health CLI, default
  health configuration, and `straylight-health-cli` compatibility symlink.
- Post-install health service is active and enabled in the installed VM.
- `straylight-health status --json` completes on the installed VM and returns a
  structured warning-state report.

## Gated Or Experimental Behavior

- Hardware-specific accelerator validation remains gated.
- Real-hardware installation and service-health behavior remain gated beyond
  the generic VM profile.
- XDP filter and redirect behavior require an explicit packet policy before
  production use.
- Swarm, GA-GPT2, AURA/XIT, Device Memory ABI, and ledger tracks remain
  research-only.

## Known Limitations

- This is alpha test media, not a supported production distribution.
- Real-hardware installation, upgrade behavior, and hardware accelerator paths
  are not validated by this VM candidate.
- The installed VM health report is warning-state because the generic VM lacks
  several real hardware and network conditions expected by the broader
  StrayLight profile.

## Security And Privacy Review

- [x] No private hostnames, local IPs, MAC addresses, serials, machine IDs,
  personal paths, credentials, or logs are included.
- [x] Generated artifacts are excluded from the source snapshot.
- [x] Public claims match `docs/VALIDATION_MATRIX.md`.
- [x] Security-sensitive details remain outside the public release notes.

## Upgrade Or Compatibility Notes

No upgrade path is promised for this alpha ISO candidate.
