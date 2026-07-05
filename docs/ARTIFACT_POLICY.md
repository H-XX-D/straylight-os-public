# Artifact Policy

This policy defines when public StrayLight OS releases may attach generated
artifacts and what validation is required.

## Default Rule

Public source-snapshot releases must not attach generated artifacts.

Do not attach these to source snapshots:

- ISO images.
- Debian package outputs.
- Kernel modules.
- VM disks or raw images.
- Benchmarks, traces, caches, or live-build working directories.
- Logs containing private host, user, interface, network, or hardware details.

## Allowed Artifact Types

Artifact-bearing releases may attach only artifacts that are documented in the
release notes and pass the relevant validation gate.

| Artifact type | Required gate |
|---------------|---------------|
| Source archive | Public snapshot verifier passes |
| Debian package set | Complete public source tree and package build validation |
| ISO candidate | Reproducible ISO candidate gate |
| Verified ISO | VM boot, installer, firstboot, and post-install health gates |

## Required Checksums

Every attached generated artifact must have a SHA256 checksum in the release
notes.

Use:

```bash
sha256sum <artifact>
```

For StrayLight ISO candidates, use the repository wrapper:

```bash
scripts/generate_iso_checksum.sh output/straylight-os-1.0.0-amd64.iso
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

If multiple artifacts are attached, include one table row per artifact in the
release notes.

## Required Validation Notes

Artifact-bearing releases must document:

- Build host class.
- Commands used.
- Package build result.
- ISO build result, when applicable.
- VM boot result, when applicable.
- Installer result, when applicable.
- Firstboot result, when applicable.
- Post-install health result, when applicable.
- Known limitations and gated behavior.

## Privacy Requirements

Before attaching artifacts or logs, confirm they do not contain:

- Private hostnames.
- Local IP addresses.
- MAC addresses.
- Serial numbers.
- Machine IDs.
- Personal filesystem paths.
- Credentials, tokens, keys, or session material.
- Private lab topology or interface names.

Use sanitized summaries instead of raw private logs.

## Source Repository Hygiene

Generated artifacts remain excluded from normal source commits. `.gitignore`
and `.gitattributes` are part of the artifact boundary and should be updated
before adding any new generated artifact type.

## Release Promotion

Do not promote a release from source snapshot to ISO candidate or verified ISO
by editing release notes alone. The matching gate in `docs/ROADMAP.md` and
`docs/VALIDATION_MATRIX.md` must be satisfied.
