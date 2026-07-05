# Installer And Firstboot Validation

This document defines the public installer and firstboot validation procedure
for a StrayLight OS ISO candidate.

Installer validation proves that an ISO candidate can install to a clean
virtual disk. Firstboot validation proves that the installed virtual disk can
boot without the ISO attached and reach the documented firstboot target. This
procedure does not replace post-install health checks; those are a separate
release gate.

## Scope

Use this procedure after VM boot validation has passed or after the release
notes explicitly state that installer testing is being attempted against a
gated boot result.

This gate verifies:

- The ISO and checksum sidecar are present and verified.
- The ISO boots in a generic VM with a blank virtual disk attached.
- The installer launches from the live environment.
- The installer completes a clean-disk install to the test disk.
- The installed disk boots without the ISO attached.
- Firstboot reaches the documented target.
- Results are summarized without private host details or raw logs.

This gate does not verify:

- Post-install StrayLight health commands.
- Hardware-specific accelerator behavior.
- XDP production packet policy.
- Real-hardware install behavior.
- Upgrade behavior from an older StrayLight installation.
- Production support status.

## VM Runtime Assumptions

Record the runtime generically. Do not include hostnames, local addresses,
personal filesystem paths, interface names, serials, machine IDs, or raw device
identifiers.

Minimum runtime class:

| Area | Assumption |
|------|------------|
| Architecture | x86_64 / amd64 |
| Firmware | Same firmware mode used for VM boot validation, unless testing both BIOS and UEFI |
| CPU | 4 virtual CPUs recommended |
| Memory | 8 GiB RAM recommended |
| Install disk | Blank virtual disk, 64 GiB or larger recommended |
| Install target | Clean-disk install only; no dual-boot or host-disk passthrough |
| Network | User-mode NAT or default isolated virtual networking |
| Display | Graphical display required when validating the graphical installer |
| Host sharing | No host shared folders, passthrough devices, bridged lab networks, or private mounts required |

## Required Inputs

Run these checks from the repository root before starting installer validation:

```bash
scripts/verify_public_snapshot.sh .
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

Required ISO files:

- `output/straylight-os-1.0.0-amd64.iso`
- `output/straylight-os-1.0.0-amd64.iso.sha256`

Optional QEMU packages on Debian-compatible hosts:

```bash
sudo apt-get install -y qemu-system-x86 qemu-utils ovmf
```

## Example Installer VM

Create a blank virtual disk for the install attempt:

```bash
qemu-img create -f qcow2 output/straylight-install-test.qcow2 64G
```

Boot the ISO with the blank virtual disk attached:

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm:tcg \
  -cpu max \
  -smp 4 \
  -m 8192 \
  -boot d \
  -cdrom output/straylight-os-1.0.0-amd64.iso \
  -drive file=output/straylight-install-test.qcow2,format=qcow2,if=virtio \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
```

For UEFI validation, add the host-appropriate OVMF firmware option and record
the firmware mode as `UEFI`. Do not publish the local firmware path in release
notes.

## Installer Path

Follow the same path for each ISO candidate:

1. Verify the ISO checksum.
2. Boot the ISO in a generic VM with one blank virtual disk attached.
3. Confirm the live target reaches the state documented by
   `docs/VM_BOOT_VALIDATION.md`.
4. Launch the graphical installer from the live environment.
5. Select the blank virtual disk as the install target.
6. Use clean-disk partitioning for the test disk.
7. Complete only generic locale, keyboard, timezone, user, and password fields.
8. Start the install and wait for completion.
9. Record whether package installation, bootloader installation, and installer
   finalization completed.
10. Shut down or reboot when the installer prompts.
11. Remove the ISO from the VM boot path before firstboot validation.

Do not use a host disk, passthrough disk, shared folder, private package mirror,
or bridged lab network for public validation.

## Installer Success Criteria

Mark installer validation as `passed` only when all required criteria are met:

- The ISO checksum verifies before the install attempt.
- The live environment reaches the installer launch point.
- The installer launches without crashing.
- The blank virtual disk is visible as the install target.
- The selected partitioning plan applies only to the test disk.
- Package installation completes without fatal errors.
- Bootloader installation completes for the selected firmware mode.
- The installer reaches its completion screen or documented completion state.
- No private host state is required to complete the install.

## Firstboot Path

Firstboot starts after the installer completes.

Use the installed virtual disk as the only boot media:

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm:tcg \
  -cpu max \
  -smp 4 \
  -m 8192 \
  -drive file=output/straylight-install-test.qcow2,format=qcow2,if=virtio \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
```

## Firstboot Success Criteria

Mark firstboot validation as `passed` only when all required criteria are met:

- The VM boots from the installed virtual disk with the ISO detached.
- The bootloader appears or proceeds directly into the installed system.
- The installed kernel and initramfs load without a kernel panic.
- The root filesystem mounts without emergency recovery.
- The system reaches the documented firstboot target:
  - graphical login or firstboot wizard for the desktop profile; or
  - documented console login target for a console-only candidate.
- A test user can reach the expected session or documented firstboot shell.
- Required firstboot units complete or remain in a documented pending state.
- No private host state is required after installation.

Post-install service health, StrayLight CLI checks, kernel-surface checks, and
XDP checks belong to the post-install health checklist.

## Failure Classes

Use one of these public failure classes when summarizing a failed or gated
installer or firstboot result:

| Failure class | Meaning |
|---------------|---------|
| `artifact-mismatch` | ISO or checksum sidecar is missing, stale, or does not verify |
| `installer-launch` | Live environment boots, but the installer cannot launch |
| `disk-detection` | The blank virtual disk is not visible to the installer |
| `partitioning` | The installer cannot apply the selected test-disk partition plan |
| `package-install` | Package installation fails before completion |
| `bootloader-install` | Bootloader installation fails for the selected firmware mode |
| `installer-finalize` | Installer cannot complete finalization or shutdown/reboot handoff |
| `firstboot-loader` | Installed disk cannot start the bootloader or kernel path |
| `firstboot-rootfs` | Installed root filesystem cannot mount cleanly |
| `firstboot-target` | Installed system cannot reach the documented firstboot target |
| `firstboot-login` | Test user cannot reach the expected session or shell |
| `firstboot-unit` | Required firstboot units fail or block progress |
| `privacy-report` | Available evidence contains private host details and cannot be published |
| `not-run` | Installer or firstboot validation has not been attempted for this ISO candidate |

## Blockers Versus Alpha Limitations

Blockers prevent an ISO candidate from being called a verified ISO. Treat these
as release blockers:

- Checksum mismatch or missing ISO sidecar.
- Installer crash or installer cannot launch.
- Blank virtual disk cannot be detected.
- Installer attempts to use anything other than the selected test disk.
- Package installation, partitioning, or bootloader installation fails.
- Installed disk cannot boot without the ISO attached.
- Firstboot enters emergency mode, loops, or panics.
- The expected firstboot target cannot be reached.
- Test user login or documented firstboot shell is unavailable.
- Required firstboot units fail without a documented recovery path.
- Validation evidence cannot be summarized without private host details.

Known alpha limitations may remain listed without blocking installer or
firstboot validation when they do not prevent the install or firstboot path:

- Cosmetic theme, icon, wallpaper, or branding mismatch.
- Slow boot or install time under software emulation.
- Optional accelerator, SGX, FPGA, GPU, RDMA, or XDP policy not active in the
  VM.
- Optional network mirror unavailable when local package payloads are already
  included.
- Non-critical app warning after the firstboot target is reached.
- Research-track functionality still labeled as gated or experimental.

Reclassify an alpha limitation as a blocker if it prevents installer completion,
installed-disk boot, user access, or required firstboot behavior.

## Public Result Summary

Do not publish raw installer logs, host terminal transcripts, screenshots
containing host UI details, VM disk images, packet captures, or private lab
reports.

Use compact summaries like these in release notes or issue updates:

```markdown
### Installer

- Runtime: amd64 VM, BIOS or UEFI, 4 vCPU class, 8 GiB RAM class
- Disk: blank virtual disk, 64 GiB class
- Path: graphical installer, clean-disk install
- Result: passed, failed, gated, or not run
- Failure class: `<class or none>`
- Public summary: `<one to three sentences without host details or raw log lines>`

### Firstboot

- Boot source: installed virtual disk, ISO detached
- Target: graphical login, firstboot wizard, or documented console target
- Result: passed, failed, gated, or not run
- Failure class: `<class or none>`
- Public summary: `<one to three sentences without host details or raw log lines>`

### Known Alpha Limitations

- `<non-blocking limitation or none>`
```

Do not include:

- hostnames or usernames
- local IP addresses, MAC addresses, interface names, serials, or machine IDs
- absolute personal paths
- raw installer, kernel, or systemd logs
- credentials, tokens, private certificates, or `.env` contents

## Release Notes Mapping

Update `docs/RELEASE_NOTES_TEMPLATE.md` for each artifact-bearing release:

- Set `Installer` to `passed`, `failed`, `gated`, or `not run`.
- Set `Firstboot` to `passed`, `failed`, `gated`, or `not run`.
- Record only generic runtime and disk classes.
- Record the installer path and firstboot target.
- Record failure classes when results are not `passed`.
- Put non-blocking issues under `Known Limitations`.
- Put release-blocking failures under `Gated Or Experimental Behavior` or a
  dedicated blocker list.

Do not mark a release as `Verified ISO` until VM boot, installer, firstboot, and
post-install health validation have all passed and are documented.
