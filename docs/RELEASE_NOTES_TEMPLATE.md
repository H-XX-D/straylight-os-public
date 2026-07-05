# Release Notes Template

Use this template for public StrayLight OS releases. Keep source-only snapshots
and artifact-bearing releases clearly separated.

```markdown
# StrayLight OS <version>

## Release Type

- [ ] Source snapshot
- [ ] ISO candidate
- [ ] Verified ISO

## Summary

Briefly describe what changed and who should use this release.

## Current Status

| Area | Status |
|------|--------|
| Source tree | <complete/incomplete/source-only> |
| Package builds | <not run/passed/failed/gated> |
| ISO build | <not run/passed/failed/gated> |
| VM boot | <not run/passed/failed/gated> |
| Installer | <not run/passed/failed/gated> |
| Firstboot | <not run/passed/failed/gated> |
| Post-install health | <not run/passed/failed/gated> |

## Build Host Class

Describe the build host without private details:

- Distribution:
- Architecture:
- CPU/RAM class:
- Required packages:
- Privilege requirements:

## Commands

```bash
scripts/verify_public_snapshot.sh .
<package build command>
<iso build command>
<checksum command>
```

## Artifacts

| Artifact | SHA256 | Notes |
|----------|--------|-------|
| `<artifact>` | `<sha256>` | `<purpose>` |

For ISO candidates, include the ISO and checksum file:

| Artifact | SHA256 | Notes |
|----------|--------|-------|
| `output/straylight-os-1.0.0-amd64.iso` | `<sha256>` | ISO candidate, not a verified ISO |
| `output/straylight-os-1.0.0-amd64.iso.sha256` | `<sha256-of-checksum-file-if-attached>` | Checksum sidecar |

## Validation Results

### Package Build

- Command:
- Result:
- Notes:

### ISO Build

- Command:
- Result:
- Notes:

### VM Boot

- Runtime:
- Result:
- Notes:

### Installer

- Path:
- Result:
- Notes:

### Firstboot

- Result:
- Notes:

### Post-Install Health

- Commands:
- Result:
- Notes:

## Verified Behavior

List only behavior validated for this release.

## Gated Or Experimental Behavior

List behavior that remains alpha, gated, or research-only.

## Known Limitations

List known limitations without private host details.

## Security And Privacy Review

- [ ] No private hostnames, local IPs, MAC addresses, serials, machine IDs, personal paths, credentials, or logs.
- [ ] Generated artifacts match `docs/ARTIFACT_POLICY.md`.
- [ ] Public claims match `docs/VALIDATION_MATRIX.md`.
- [ ] Security-sensitive changes reviewed against `SECURITY.md`.

## Upgrade Or Compatibility Notes

Describe compatibility expectations, if any.
```

## Source Snapshot Shortcut

For source-only alpha snapshots, omit artifact, boot, installer, firstboot, and
post-install health sections unless they are explicitly relevant. State clearly
that no binary artifacts are attached.

## ISO Candidate Shortcut

For ISO candidates, keep boot, installer, firstboot, and post-install health
sections even when they are not run yet. Mark them as `not run` or `gated` so
the release notes do not imply verified install support.
