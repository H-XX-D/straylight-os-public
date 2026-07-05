# StrayLight Current Status

This public source snapshot documents an alpha distribution-prep state.

Verified in the source handoff and public tree:

- 8 package groups built successfully.
- A local APT package repository was produced with `.deb` packages and
  `Packages.gz`.
- An ISO artifact was built from the public source tree at
  `output/straylight-os-1.0.0-amd64.iso`.
- The ISO checksum sidecar was generated and verified.
- VM boot validation passed in a generic UEFI amd64 QEMU/KVM VM and reached the
  GNOME live session.
- The release audit passed after package and ISO builds.
- The public package source payload check passes with
  `scripts/check_package_dependencies.sh --source-only .`.

Still required before public distribution:

- Installer validation run.
- Firstboot validation run.
- Post-install health check run.

The public documentation now separates validation procedures from completed
validation results. A procedure being documented does not mean the gate has
passed for an ISO artifact.
