# Clean-Clone Package Dependency Check

This check is the public preflight command for clean-clone package builds. It
tells an outside user whether a fresh Debian-compatible clone has the host
packages and source payload paths needed before attempting package builds.

The current public source tree passes the source payload portion with:

```bash
scripts/check_package_dependencies.sh --source-only .
```

Full package builds are still gated by host packages, compiler/toolchain
behavior, and package build validation on a prepared Debian-compatible host.

## Host Prerequisites

Run on a Debian Bookworm or Trixie compatible amd64 host with `dpkg-query`
available.

Required package-build host packages:

- `build-essential`
- `cmake`
- `ninja-build`
- `pkg-config`
- `rsync`
- `dpkg-dev`
- `debhelper`
- `dh-cmake`
- `devscripts`
- `dkms`
- `linux-headers-amd64`

Install them with:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config rsync \
  dpkg-dev debhelper dh-cmake devscripts \
  dkms linux-headers-amd64
```

## Run The Check

From a clean clone:

```bash
scripts/check_package_dependencies.sh .
```

Host package checks only:

```bash
scripts/check_package_dependencies.sh --host-only .
```

Source payload checks only:

```bash
scripts/check_package_dependencies.sh --source-only .
```

## Output Classes

| Output | Meaning |
|--------|---------|
| `[OK_HOST_PACKAGE]` | Required Debian host package is installed. |
| `[MISSING_HOST_PACKAGE]` | Install the named package before building. |
| `[MISSING_HOST_TOOL]` | The host cannot run the Debian package check. |
| `[OK_SOURCE_PATH]` | Required source payload path exists. |
| `[MISSING_SOURCE_PATH]` | The public clone lacks a required source path. |

The command exits non-zero when any host package or source payload path is
missing.

## Expected Public Result

On the current public tree, `--source-only` should report only
`[OK_SOURCE_PATH]` entries for implementation paths listed in
`docs/PACKAGE_PAYLOAD_INVENTORY.md`.

On a host without the Debian build dependencies installed, the full check may
still report `[MISSING_HOST_PACKAGE]` entries. That is a host preparation issue,
not a missing-source issue.

The check must not read private host paths, interface identifiers, local
addresses, machine IDs, generated packages, ISO artifacts, or build logs. It
only inspects Debian package installation state and repository-relative source
paths.

## Build Flow Position

Use this preflight before package build commands:

```bash
scripts/straylight_release_audit.sh .
scripts/check_package_dependencies.sh .
scripts/build-packages.sh --check-deps --no-sign
scripts/build-packages.sh --clean --no-sign
```

See `docs/PACKAGE_BUILD_WRAPPER.md` for the public package build entrypoint and
its build-blocked failure class.
