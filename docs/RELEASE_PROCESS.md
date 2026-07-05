# Release Process

This document describes how public StrayLight starter snapshots are cut.

The public repository is an alpha source and documentation snapshot. A tag is a
reviewed repository state, not a supported binary release, unless the GitHub
release explicitly attaches verified artifacts.

## Release Types

| Type | Meaning |
|------|---------|
| Source snapshot | Documentation, examples, packaging layout, and release hygiene are tagged. No binary artifacts are attached. |
| ISO candidate | A complete source tree has produced an ISO, but VM boot, installer, firstboot, and health gates are still in progress. |
| Verified ISO | ISO artifact, checksum, boot/install validation, firstboot validation, and post-install health checks are all documented. |

The current public repository supports source snapshots only.

## Versioning

Use alpha tags until a complete public source tree and verified installation
path are available:

```text
v0.1.0-alpha
v0.2.0-alpha
```

Do not use a stable `v1.0.0` tag until the public release includes a complete
source tree, reproducible package builds, reproducible ISO builds, and verified
installation gates.

## Pre-Release Checklist

Run these checks from the repository root:

```bash
scripts/verify_public_snapshot.sh .
```

Confirm:

- README and docs describe alpha/gated work accurately.
- `CHANGELOG.md` names the release and current boundaries.
- `docs/PUBLIC_SOURCE_MANIFEST.md` matches the files being published.
- `docs/RELEASE_NOTES_TEMPLATE.md` matches the release type.
- Package build commands use `scripts/build-packages.sh` or document why the
  package-build gate remains blocked.
- ISO candidate releases run `scripts/check_iso_candidate_requirements.sh .` or
  document why the ISO gate remains blocked.
- No personal paths, private hostnames, local IP addresses, MAC addresses,
  serials, machine IDs, credentials, or generated artifacts are present.
- GitHub Actions passes on the commit to be tagged.
- Attached artifacts, if any, follow `docs/ARTIFACT_POLICY.md` and have
  explicit checksums and validation notes.

## Source Snapshot Commands

```bash
git tag -a v0.1.0-alpha -m "StrayLight OS public starter v0.1.0-alpha"
git push origin main v0.1.0-alpha
```

Then create a GitHub release from the tag and mark it as a pre-release.

## Release Notes

Use `docs/RELEASE_NOTES_TEMPLATE.md` when preparing a public release. Source
snapshots should explicitly say that no binary artifacts are attached.

## Artifact Policy

Do not attach these to public source-snapshot releases:

- ISO images.
- Debian package outputs.
- Kernel modules.
- VM disks or raw images.
- Benchmarks, traces, caches, or live-build working directories.
- Logs containing private host, user, interface, network, or hardware details.

Artifact-bearing releases need a separate verification note that names the
build host class, commands, checksums, boot path, installer result, firstboot
result, and post-install health result.

See `docs/ARTIFACT_POLICY.md` for the full artifact and checksum policy.
