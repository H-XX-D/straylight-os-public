# Public Source Manifest

This manifest describes what belongs in the public StrayLight OS source
repository and what must stay out of source snapshots.

## Snapshot Class

Current class: public alpha source snapshot.

This repository contains documentation, examples, package layout guidance,
release hygiene scripts, community process files, package source payloads, and
ISO source paths. It does not contain generated package or ISO artifacts.

## Included Areas

| Path | Purpose |
|------|---------|
| `README.md` | Main project overview, status, architecture, and documentation index |
| `CHANGELOG.md` | Public release history |
| `LICENSE` | MIT license |
| `CONTRIBUTING.md` | Public contribution rules and checks |
| `SECURITY.md` | Security reporting policy and scope |
| `SUPPORT.md` | Public support boundaries |
| `GOVERNANCE.md` | Alpha maintainer and decision model |
| `MAINTAINERS.md` | Current public maintainer listing |
| `CODE_OF_CONDUCT.md` | Collaboration expectations |
| `.github/` | GitHub workflow, ownership, issue, and pull request metadata |
| `docs/` | Onboarding, validation, ADRs, artifact policy, package payload inventory, package repository generation, package build wrapper, live-build requirements, ISO candidate release flow, VM boot validation, installer and firstboot validation, post-install health checklist, excluded-area boundaries, clean-clone checks, build, release, roadmap, privacy, status, surface, network, and CLI docs |
| `examples/` | Sanitized manifests and configuration examples |
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

See `docs/EXCLUDED_IMPLEMENTATION_AREAS.md` for the generic implementation
classes that remain outside the public source tree and the gates required before
they can be included.

## Required Public Checks

Run the source snapshot verifier before publishing or tagging:

```bash
scripts/verify_public_snapshot.sh .
```

The verifier runs the release audit, Markdown link check, shell syntax check,
and a clean working-tree check. The separate package dependency preflight is
documented in `docs/CLEAN_CLONE_PACKAGE_CHECK.md`; its source-only mode should
pass on the current public tree. The ISO candidate requirements checker is
documented in `docs/LIVE_BUILD_REQUIREMENTS.md`; its source-only mode should
pass, while the full check still requires generated package repository output.

## Archive Hygiene

`.gitattributes` normalizes text files to LF and marks generated binary artifact
types as binary. GitHub-specific issue and pull request templates are excluded
from source archives, while documentation, examples, scripts, and release
metadata remain part of the archive.

## Promotion Gates

A future public release can move beyond alpha source status only when package
build commands, ISO build commands, checksums, boot/install validation,
firstboot validation, and post-install health validation pass as described in
`docs/ROADMAP.md` and `docs/RELEASE_PROCESS.md`. The package source paths are
tracked in `docs/PACKAGE_PAYLOAD_INVENTORY.md`.
