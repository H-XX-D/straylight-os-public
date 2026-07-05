# Package Payload Inventory

This inventory maps every public package group to the source payload paths an
outside clean clone must contain before the package build can be considered
publicly reproducible.

The current repository is a source-only public alpha starter. It documents the
package layout and release gates, but it does not yet include the implementation
payloads listed below. Paths marked "required" are the target public source
surface for the `v0.2.0-alpha` complete-source-tree gate.

## Status Legend

| Status | Meaning |
|--------|---------|
| Present | The public starter repository contains this path now. |
| Required | The path is needed for a clean-clone build, but is not present yet. |
| Excluded | The path or artifact must not be committed to the public source tree. |
| Generated | The path is produced by a build and must remain ignored. |

## Current Public Payload

| Area | Current public paths |
|------|----------------------|
| Package map | `packaging/STRAYLIGHT_PACKAGE_SPLIT.md` |
| Build requirements | `docs/BUILD_ISO.md`, `examples/iso-build.env`, `examples/package-profile.json` |
| Release gates | `docs/ROADMAP.md`, `docs/VALIDATION_MATRIX.md`, `docs/RELEASE_PROCESS.md` |
| Sanitized examples | `examples/hardware-fabric.yaml`, `examples/xdp.conf` |
| Release hygiene | `scripts/verify_public_snapshot.sh`, `scripts/straylight_release_audit.sh`, `scripts/check_markdown_links.py`, `scripts/check_shell_syntax.sh` |

## Package Group Inventory

| Package group | Required source payload paths | Current public status |
|---------------|-------------------------------|-----------------------|
| `libstraylight-common1` | `src/common/`, `include/straylight/`, `cmake/`, `packaging/libstraylight-common/debian/` | Required |
| `libstraylight-common-dev` | `include/straylight/`, `cmake/StrayLight*.cmake`, development metadata under `packaging/libstraylight-common/debian/` | Required |
| `straylight-core` | `src/core/`, `src/daemons/`, `src/cli/`, `systemd/`, `dbus/`, `udev/`, `packaging/straylight-core/debian/` | Required |
| `straylight-kernel` | `kernel/`, `kernel/dkms/`, `kernel/modules/`, `modprobe.d/`, `modules-load.d/`, `packaging/straylight-kernel/debian/` | Required |
| `straylight-ml` | `src/ml/`, `src/predict/`, `src/quantum/`, `src/photonics/`, `src/snn/`, `packaging/straylight-ml/debian/` | Required |
| `straylight-network` | `src/network/`, `src/bpf/`, `src/xdp/`, `src/bridge/`, `src/swarm/`, `src/transport/`, `systemd/straylight-xdp@.service`, `packaging/straylight-network/debian/` | Required |
| `straylight-exotic` | `src/enclave/`, `src/pmem/`, `src/fuse/`, `src/sandbox/`, `src/rhem/`, `packaging/straylight-exotic/debian/` | Required |
| `straylight-desktop` | `apps/`, `desktop/`, `widgets/`, `oobe/`, `wizard/`, `firstboot/`, `src/app-cli/`, `packaging/straylight-desktop/debian/` | Required |
| `straylight-os` | `profiles/straylight-os/`, `iso/live-build/`, `iso/calamares/`, `packaging/straylight-os/debian/`, package-list and metapackage dependency metadata | Required |

## Intentionally Excluded Payloads

The following areas are intentionally outside the public source snapshot even
after the package source tree is complete:

- Generated Debian packages under `output/debs/`.
- Generated package indexes such as `output/debs/Packages.gz`.
- Generated ISO images, checksums, and build logs under `output/`.
- live-build working directories such as `.build/`, `chroot/`, `binary/`, and
  `cache/`.
- DKMS build output, compiled kernel modules, object files, and local build
  logs.
- Private host inventory, local interface names, private addresses, machine
  identifiers, serials, credentials, and ops state.
- Large model weights, private traces, raw packet captures, and local experiment
  caches.

The broader public/private implementation boundary is documented in
`docs/EXCLUDED_IMPLEMENTATION_AREAS.md`.

## Clean-Clone Build Gate

The `v0.2.0-alpha` source-tree gate is not complete until:

- Every package group has the required source payload paths listed above.
- Each package group has public Debian packaging metadata.
- Build commands can fail fast with documented missing dependency messages.
- `scripts/check_package_dependencies.sh .` reports host dependency and source
  payload gaps using public, repository-relative paths.
- Generated output remains ignored and absent from the source tree.
- `scripts/verify_public_snapshot.sh .` passes from a clean checkout.

When any package payload path is added, update this inventory and
`docs/VALIDATION_MATRIX.md` in the same change.
