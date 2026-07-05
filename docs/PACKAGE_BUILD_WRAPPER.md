# Package Build Wrapper

`scripts/build-packages.sh` is the public package build entrypoint. It exists so
the documented build commands are runnable from the repository root even while
the public source tree is still incomplete.

The current source-only starter is expected to fail before package builds start,
because implementation payloads and Debian package metadata are not all present
yet. That failure is intentional and public-safe.

## Commands

Check host and source prerequisites:

```bash
scripts/build-packages.sh --check-deps --no-sign
```

Build all package groups once the complete source tree is present:

```bash
scripts/build-packages.sh --clean --no-sign
```

Build one package group:

```bash
scripts/build-packages.sh --no-sign straylight-desktop
```

Generate only the local package repository index from existing `.deb` files:

```bash
scripts/build-packages.sh --repo-only
```

List package groups:

```bash
scripts/build-packages.sh --list
```

## Package Order

The wrapper uses this build order:

1. `straylight-common`
2. `straylight-kernel`
3. `straylight-core`
4. `straylight-ml`
5. `straylight-network`
6. `straylight-exotic`
7. `straylight-desktop`
8. `straylight-os`

## Failure Classes

| Output | Meaning |
|--------|---------|
| `[MISSING_HOST_PACKAGE]` | Install the named Debian package before building. |
| `[MISSING_HOST_TOOL]` | Run on a Debian-compatible host with the named tool. |
| `[MISSING_SOURCE_PATH]` | The public clone lacks a source payload path required by the package inventory. |
| `[BUILD_BLOCKED]` | The wrapper stopped before build execution because the source tree or host is incomplete. |
| `[BUILD_PACKAGE]` | The wrapper is invoking `dpkg-buildpackage` for the named package group. |
| `[PACKAGE_REPO_OUTPUT]` | The wrapper wrote `Packages` or `Packages.gz`. |

The wrapper must not read private host paths, local network state, interface
identifiers, generated artifacts, or private lab files. It uses the release
audit, `scripts/check_package_dependencies.sh`, and repository-relative package
paths only.

## Promotion Gate

The `v0.2.0-alpha` source-tree gate is not complete until this wrapper can run
from a clean public clone and either:

- build the selected package groups, or
- fail with documented public-safe dependency or source-payload messages.
