# StrayLight Current Status

This public source snapshot documents an alpha distribution-prep state.

Verified in the source handoff and public tree:

- 8 package groups built successfully.
- A local APT package repository was produced with `.deb` packages and
  `Packages.gz`.
- An ISO artifact was built from the public source tree at
  `output/straylight-os-1.0.0-amd64.iso`.
- The ISO checksum sidecar was generated and verified for SHA256
  `0c74fc9df806609cbf74e0333ce15c6438053302165a6f39b31b4efe8f905373`.
- VM boot validation passed in a generic UEFI amd64 QEMU/KVM VM and reached the
  GNOME live session.
- Installer validation passed in a generic UEFI amd64 QEMU/KVM VM using a blank
  64 GiB-class virtual disk.
- Installed-disk inspection found EFI, root, and swap partitions, generated
  `/boot/grub/grub.cfg`, `EFI/StrayLight_OS/grubx64.efi`, and fallback
  `EFI/BOOT/BOOTX64.EFI`.
- Firstboot validation passed with the ISO detached and reached the graphical
  login target for the created test user.
- The release audit passed after package and ISO builds.
- The public package source payload check passes with
  `scripts/check_package_dependencies.sh --source-only .`.

Still required before public distribution:

- Post-install health check run.

The public documentation now separates validation procedures from completed
validation results. VM boot, installer, and firstboot have passed for this ISO
candidate; post-install health remains the final verified-ISO gate. The
sanitized release-note draft is in
`docs/STRAYLIGHT_OS_1_0_0_ISO_CANDIDATE.md`.
