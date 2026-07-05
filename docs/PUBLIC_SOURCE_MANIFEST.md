# Public Source Manifest

This manifest describes what belongs in the public StrayLight OS starter
repository and what must stay out of source snapshots.

## Snapshot Class

Current class: source-only public alpha starter.

This repository contains documentation, examples, package layout guidance,
release hygiene scripts, and community process files. It does not contain every
implementation file required to build a full StrayLight OS image from a fresh
clone.

## Included Areas

| Path | Purpose |
|------|---------|
| `README.md` | Main project overview, status, architecture, and documentation index |
| `CHANGELOG.md` | Public starter release history |
| `LICENSE` | MIT license |
| `CONTRIBUTING.md` | Public contribution rules and checks |
| `SECURITY.md` | Security reporting policy and scope |
| `SUPPORT.md` | Public support boundaries |
| `GOVERNANCE.md` | Alpha maintainer and decision model |
| `MAINTAINERS.md` | Current public maintainer listing |
| `CODE_OF_CONDUCT.md` | Collaboration expectations |
| `.github/` | GitHub workflow, ownership, issue, and pull request metadata |
| `docs/` | Onboarding, build, release, roadmap, privacy, status, surface, network, and CLI docs |
| `examples/` | Sanitized starter manifests and configuration examples |
| `packaging/` | Package split and package-layout guidance |
| `scripts/` | Public release hygiene and snapshot verification scripts |

## Excluded Areas

Do not include:

- Personal filesystem paths.
- Private hostnames, local IP addresses, MAC addresses, serials, or machine IDs.
- SSH keys, API tokens, credentials, `.env` files, or private ops state.
- Generated ISO images, package outputs, kernel modules, VM disks, traces,
  caches, or live-build working directories.
- Logs or reports from private lab systems unless converted into sanitized,
  generic examples.
- Unreviewed implementation files that embed private machine assumptions.

## Required Public Checks

Run the source snapshot verifier before publishing or tagging:

```bash
scripts/verify_public_snapshot.sh .
```

The verifier runs the release audit, Markdown link check, shell syntax check,
and a clean working-tree check.

## Archive Hygiene

`.gitattributes` normalizes text files to LF and marks generated binary artifact
types as binary. GitHub-specific issue and pull request templates are excluded
from source archives, while documentation, examples, scripts, and release
metadata remain part of the archive.

## Promotion Gates

A future public release can move beyond source-only starter status only when the
repository includes the complete source paths, package build commands, ISO build
commands, checksums, boot/install validation, firstboot validation, and
post-install health validation described in `docs/ROADMAP.md` and
`docs/RELEASE_PROCESS.md`.
