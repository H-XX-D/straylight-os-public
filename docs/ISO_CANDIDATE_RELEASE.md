# ISO Candidate Release Flow

This document defines the public release-note and checksum flow for a
StrayLight ISO candidate.

An ISO candidate is not a verified installation release. It means a complete
public source tree produced an ISO and checksum from documented commands, while
VM boot, installer, firstboot, and post-install health validation may still be
pending.

## Candidate Build Flow

Run these commands from a clean public source tree:

```bash
scripts/verify_public_snapshot.sh .
scripts/build-packages.sh --clean --no-sign
scripts/generate_package_repo.sh
scripts/check_iso_candidate_requirements.sh .
sudo scripts/build-iso.sh --clean
scripts/generate_iso_checksum.sh output/straylight-os-1.0.0-amd64.iso
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

The current public source tree should pass source-payload checks, but the full
ISO candidate flow still requires package builds to produce
`output/debs/Packages.gz`.

## VM Boot Validation

After checksum verification, use `docs/VM_BOOT_VALIDATION.md` to test the ISO
candidate in a generic amd64 VM.

Release notes may mark VM boot as `passed` only when the ISO checksum verifies,
the VM starts from the ISO, the boot loader and kernel path complete, and the
documented live target is reached. Keep installer, firstboot, and post-install
health results separate from this boot-only gate.

## Installer And Firstboot Validation

After VM boot validation, use `docs/INSTALLER_FIRSTBOOT_VALIDATION.md` to test
a clean-disk install and firstboot from the installed virtual disk.

Release notes may mark installer and firstboot as `passed` only when the
installer completes against the blank test disk, the ISO is detached, and the
installed system reaches the documented firstboot target. Known alpha
limitations must be separated from release blockers.

## Post-Install Health Validation

After firstboot validation, use `docs/POST_INSTALL_HEALTH_CHECKLIST.md` to run
package, service, CLI, app, and required surface checks on the installed system.

Release notes may mark post-install health as `passed` only when required
commands pass or any non-blocking warnings are listed as known alpha
limitations. Keep optional hardware and XDP policy checks separate unless the
release profile requires them.

## Checksum Requirements

The checksum command writes:

```text
output/straylight-os-1.0.0-amd64.iso.sha256
```

Generate it with:

```bash
scripts/generate_iso_checksum.sh output/straylight-os-1.0.0-amd64.iso
```

Verify it with:

```bash
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

## Release Notes Requirements

Use `docs/RELEASE_NOTES_TEMPLATE.md` and mark exactly:

```markdown
- [ ] Source snapshot
- [x] ISO candidate
- [ ] Verified ISO
```

The release notes must distinguish build success from install validation:

- Package build: passed, failed, or gated.
- ISO build: passed, failed, or gated.
- Checksum: generated and verified, or gated.
- VM boot: not run, passed, failed, or gated.
- Installer: not run, passed, failed, or gated.
- Firstboot: not run, passed, failed, or gated.
- Post-install health: not run, passed, failed, or gated.

Do not write "verified ISO" unless the VM boot, installer, firstboot, and
post-install health gates have passed and are documented.

## Artifact Attachment Rule

Do not attach ISO artifacts to a source snapshot.

For an ISO candidate release, attach generated artifacts only when all of these
are true:

- the complete public source tree was used
- package repository generation completed
- ISO build completed
- SHA256 checksum was generated and verified
- release notes include build host class, commands, checksum, known
  limitations, and gated validation state
- the artifact is explicitly labeled "ISO candidate"

## Privacy Boundary

Release notes may include sanitized summaries and repository-relative paths.
They must not include:

- private hostnames, local IP addresses, MAC addresses, serials, or machine IDs
- personal filesystem paths
- local interface names or lab topology
- credentials, tokens, keys, `.env` files, or private certificates
- raw logs, packet captures, traces, caches, or private benchmark output
