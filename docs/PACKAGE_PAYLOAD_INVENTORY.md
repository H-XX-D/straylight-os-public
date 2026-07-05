# Package Payload Inventory

This inventory maps every public package group to the source payload paths an
outside clean clone must contain before the package build can be considered
publicly reproducible.

The current repository is a source-only public alpha tree. It includes the
implementation payload paths needed to begin package builds from a fresh public
clone. Generated packages, ISO images, live-build working trees, model caches,
logs, and private lab state remain excluded.

## Status Legend

| Status | Meaning |
|--------|---------|
| Present | The public repository contains this path now. |
| Required | The path is needed for a clean-clone build, but is not present yet. |
| Excluded | The path or artifact must not be committed to the public source tree. |
| Generated | The path is produced by a build and must remain ignored. |

## Current Public Payload

| Area | Current public paths |
|------|----------------------|
| Package map | `packaging/STRAYLIGHT_PACKAGE_SPLIT.md`, `packaging/straylight-package-profiles.json` |
| Source tree | `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `lib/`, `bin/`, `services/`, `kernel/`, `apps/`, `tools/`, `assets/`, `config/`, `etc/`, `protocols/`, `tests/` |
| Debian packaging | `packaging/straylight-common/`, `packaging/straylight-core/`, `packaging/straylight-kernel/`, `packaging/straylight-ml/`, `packaging/straylight-network/`, `packaging/straylight-exotic/`, `packaging/straylight-desktop/`, `packaging/straylight-os/` |
| ISO profile source | `iso/live-build/auto/`, `iso/live-build/config/`, `iso/calamares/` |
| Build requirements | `docs/BUILD_ISO.md`, `docs/PACKAGE_BUILD_WRAPPER.md`, `examples/iso-build.env`, `examples/package-profile.json` |
| Release gates | `docs/ROADMAP.md`, `docs/VALIDATION_MATRIX.md`, `docs/RELEASE_PROCESS.md` |
| Sanitized examples | `examples/hardware-fabric.yaml`, `examples/xdp.conf` |
| Release hygiene | `scripts/verify_public_snapshot.sh`, `scripts/straylight_release_audit.sh`, `scripts/check_package_dependencies.sh`, `scripts/build-packages.sh`, `scripts/check_markdown_links.py`, `scripts/check_shell_syntax.sh` |

## Package Group Inventory

| Package group | Required source payload paths | Current public status |
|---------------|-------------------------------|-----------------------|
| `libstraylight-common1` | `lib/common/src/`, `lib/common/include/straylight/`, `cmake/`, `packaging/straylight-common/debian/` | Present |
| `libstraylight-common-dev` | `lib/common/include/straylight/`, `cmake/Straylight*.cmake`, development metadata under `packaging/straylight-common/debian/` | Present |
| `straylight-core` | `bin/core/`, `services/`, `tools/`, `etc/systemd/`, `services/dbus/`, `services/udev/`, `packaging/straylight-core/debian/` | Present |
| `straylight-kernel` | `kernel/`, `kernel/dkms/`, `kernel/xdp/`, `packaging/straylight-kernel/debian/` | Present |
| `straylight-ml` | `lib/ml/`, `services/predict/`, `bin/quantum/`, `bin/photonics/`, `bin/snn/`, `packaging/straylight-ml/debian/` | Present |
| `straylight-network` | `lib/net/`, `kernel/xdp/`, `bin/xdp/`, `services/bridge/`, `services/swarm/`, `services/mesh/`, `etc/systemd/system/straylight-xdp@.service`, `packaging/straylight-network/debian/` | Present |
| `straylight-exotic` | `bin/enclave/`, `bin/pmem/`, `bin/fuse/`, `tools/sandbox/`, `bin/rhem/`, `packaging/straylight-exotic/debian/` | Present |
| `straylight-desktop` | `apps/`, `assets/`, `apps/widgets/`, `apps/oobe/`, `apps/wizard/`, `services/firstboot/`, `apps/app-cli/`, `packaging/straylight-desktop/debian/` | Present |
| `straylight-os` | `config/`, `iso/live-build/auto/`, `iso/live-build/config/`, `iso/calamares/`, `packaging/straylight-os/debian/`, package-list and metapackage dependency metadata | Present |

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

The source payload gate is complete when:

- Every package group has the required source payload paths listed above.
- Each package group has public Debian packaging metadata.
- Build commands can fail fast with documented missing dependency messages.
- `scripts/check_package_dependencies.sh --source-only .` passes from a clean
  public clone.
- `scripts/check_package_dependencies.sh .` either passes on a prepared Debian
  build host or reports only host package gaps using public package names.
- `scripts/build-packages.sh --check-deps --no-sign` is runnable from the
  repository root.
- Generated output remains ignored and absent from the source tree.
- `scripts/verify_public_snapshot.sh .` passes from a clean checkout.

When any package payload path is added, update this inventory and
`docs/VALIDATION_MATRIX.md` in the same change.
