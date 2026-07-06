# StrayLight Current Status

This public source snapshot documents an alpha distribution-prep state.

Verified in the source handoff and public tree:

- 8 package groups built successfully.
- A local APT package repository was produced with `.deb` packages and
  `Packages.gz`.
- An ISO artifact was built from the public source tree at
  `output/straylight-os-1.0.0-amd64.iso`.
- The ISO checksum sidecar was generated and verified for SHA256
  `d158634893f647ec79947a76264a0f6b4ac65f74c7846fdcdb306fcc10347d06`.
- VM boot validation passed in a generic UEFI amd64 QEMU/KVM VM and reached the
  GNOME live session.
- Installer validation passed in a generic UEFI amd64 QEMU/KVM VM using a blank
  64 GiB-class virtual disk.
- Installed-disk inspection found EFI, root, and swap partitions, generated
  `/boot/grub/grub.cfg`, `EFI/StrayLight_OS/grubx64.efi`, and fallback
  `EFI/BOOT/BOOTX64.EFI`.
- Firstboot validation passed with the ISO detached and reached the graphical
  login target for the created test user.
- Post-install health command validation passed on the installed VM: the health
  service was active and enabled, and `straylight-health status --json`
  returned structured JSON.
- The installed VM health report was warning-state, with `overall_score` 77 and
  `overall_status` `warn`, due to expected generic-VM conditions such as
  unavailable SMART status, unreachable gateway/internet, and inactive
  research-profile services.
- The release audit passed after package and ISO builds.
- The public package source payload check passes with
  `scripts/check_package_dependencies.sh --source-only .`.

Still required before production-support claims:

- Real-hardware validation before any production-support claim.
- Follow-up remediation or explicit release-note treatment for warning-state VM
  health checks.

The public documentation now separates validation procedures from completed
validation results. VM boot, installer, firstboot, and post-install health
command validation have passed for this ISO candidate. The sanitized
release-note draft is in
`docs/STRAYLIGHT_OS_1_0_0_ISO_CANDIDATE.md`.
