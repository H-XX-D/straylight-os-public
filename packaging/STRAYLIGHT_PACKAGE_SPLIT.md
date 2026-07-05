# StrayLight Package Split

This is the packaging target for turning StrayLight from a lab image into a clean
Debian-family installable operating environment. The release source audit is the
first gate for every package and ISO build.

## Build Gate

All release builders should run:

```bash
scripts/straylight_release_audit.sh .
```

The audit blocks private host paths, lab-only host names, private network
addresses, hardware-only identifiers, and other values that should not ship in a
general installer. Builders may expose an explicit skip flag for local debug
runs, but release artifacts should not use it.

## Target Binary Packages

| Package | Purpose | Default ISO | Notes |
| --- | --- | --- | --- |
| `straylight-base` | Shared filesystem layout, default config, policies, udev/systemd base units, common runtime data. | yes | Owns install-safe defaults. |
| `straylight-firstboot` | Hardware discovery, driver activation, first-run repair, install-time checks. | yes | Should be runnable from Calamares and post-install. |
| `straylight-core` | Core daemons and control-plane services. | yes | Should avoid pulling desktop UI dependencies. |
| `straylight-observe` | Health, telemetry, status, diagnostics, and report tools. | yes | Keeps observability installable on headless systems. |
| `straylight-ops` | Backup, migration, remote control, scheduling, and admin tools. | optional | Enabled by full and admin profiles. |
| `straylight-hardware` | Kernel modules, DKMS packages, accelerator helpers, and hardware policy. | profile | Should be profile-gated where hardware support is narrow. |
| `straylight-ml` | ML, prediction, vector, and model-management runtime pieces. | optional | Depends on core and packaged model-location policy. |
| `straylight-ui` | Desktop app GUIs, first-run GUI tools, themes, icons, and integration with the distro desktop/window manager. | desktop | Keeps server/headless installs small and does not ship a StrayLight compositor. |
| `straylight-exotic` | Experimental or uncommon subsystems. | optional | Must not be a default dependency for base installs. |
| `straylight-all` | Meta package for the complete workstation image. | full | Depends on the selected production packages. |

## Current Source Package Mapping

| Current source package | Target package area |
| --- | --- |
| `straylight-common` | `straylight-base` shared libraries and development files. |
| `straylight-core` | `straylight-core`, plus some future `straylight-observe` units. |
| `straylight-desktop` | `straylight-ui` and firstboot-facing desktop tools for the distro desktop. |
| `straylight-kernel` | `straylight-hardware`. |
| `straylight-ml` | `straylight-ml`. |
| `straylight-network` | `straylight-core`, `straylight-ops`, and `straylight-observe` networking tools. |
| `straylight-exotic` | `straylight-exotic`. |
| `straylight-os` | Temporary full meta package; replace with `straylight-all`. |

## Release Profiles

| Profile | Packages |
| --- | --- |
| `base` | `straylight-base`, `straylight-firstboot`, `straylight-core`, `straylight-observe` |
| `desktop` | `base` plus `straylight-ui` |
| `admin` | `base` plus `straylight-ops` |
| `ml` | `desktop` plus `straylight-ml` |
| `hardware` | `base` plus `straylight-hardware` |
| `full` | `straylight-all` |

## Migration Order

1. Create empty Debian source directories for the target packages with strict
   install manifests and no maintainer scripts beyond policy-safe stubs.
2. Move firstboot and hardware-detection files into `straylight-firstboot`.
3. Split observability commands and services from broad core packages.
4. Leave the custom compositor out of release packaging; move GUI artifacts into
   `straylight-ui` only when they run on the distro desktop/window manager.
5. Replace `straylight-os` with `straylight-all` after profile package
   dependencies are accurate.
