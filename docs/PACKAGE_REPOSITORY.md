# Package Repository Generation

This document defines the public process for producing the local APT package
repository consumed by the StrayLight ISO build.

The public source tree does not include generated `.deb` artifacts. Build
packages first before generating the local package repository.

## Inputs

The package repository generator reads:

| Input | Source |
|-------|--------|
| `output/debs/*.deb` | Runtime package artifacts produced by `scripts/build-packages.sh --clean --no-sign` |

Generated packages are build artifacts. They must remain ignored by git and
must not be committed to source snapshots.

## Outputs

The generator writes:

| Output | Purpose |
|--------|---------|
| `output/debs/Packages` | Local APT package index |
| `output/debs/Packages.gz` | Compressed package index consumed by the ISO build |

`Packages.gz` is generated with `gzip -n -9` so the gzip header does not embed a
timestamp or original filename.

## Command

Generate the package repository index:

```bash
scripts/generate_package_repo.sh
```

Check only that host tools and `.deb` inputs are present:

```bash
scripts/generate_package_repo.sh --check-only
```

Use an alternate repository directory:

```bash
scripts/generate_package_repo.sh --repo-dir output/debs
```

## Failure Classes

| Output | Meaning |
|--------|---------|
| `[MISSING_HOST_TOOL]` | Install the named host tool before generating the repository. |
| `[MISSING_PACKAGE_REPO_INPUT]` | The package output directory or `.deb` inputs are missing. |
| `[PACKAGE_REPO_INPUT]` | The repository input directory being scanned. |
| `[PACKAGE_REPO_DEB]` | A `.deb` input included in generation. |
| `[PACKAGE_REPO_OUTPUT]` | An index file written by the command. |

The public source tree should pass `scripts/check_package_dependencies.sh
--source-only .`; repository generation still reports
`[MISSING_PACKAGE_REPO_INPUT]` until package artifacts exist.

## Build Flow Position

Use the generator after package builds and before ISO candidate checks:

```bash
scripts/build-packages.sh --clean --no-sign
scripts/generate_package_repo.sh
scripts/check_iso_candidate_requirements.sh .
```

## Source Hygiene

These paths must remain ignored and absent from source commits:

- `output/debs/*.deb`
- `output/debs/Packages`
- `output/debs/Packages.gz`
- package build logs and transient staging directories

Use `scripts/verify_public_snapshot.sh .` before publishing a source snapshot.
