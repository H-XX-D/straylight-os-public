# VM Boot Validation

This document defines the public VM boot validation procedure for a StrayLight
OS ISO candidate.

VM boot validation proves that an ISO candidate can start in a generic virtual
machine and reach the expected live environment. It does not prove installer,
firstboot, or post-install health behavior. Those gates are tracked separately.

## Scope

Use this procedure after an ISO candidate and checksum have been generated from
the documented build flow.

This gate verifies:

- The ISO file and checksum sidecar are present.
- The checksum verifies before boot testing.
- A generic amd64 VM can read the ISO as boot media.
- The boot loader, kernel, initramfs, and live environment progress far enough
  to reach the documented target.
- The result can be summarized without private host details or raw logs.

This gate does not verify:

- Installing to a virtual disk.
- Firstboot after installation.
- Runtime health of installed services.
- Hardware-specific accelerator, XDP, or kernel-module behavior.
- Production support status.

## VM Runtime Assumptions

Record the VM runtime generically. Do not include hostnames, local addresses,
personal filesystem paths, interface names, serials, machine IDs, or raw device
identifiers.

Minimum runtime class:

| Area | Assumption |
|------|------------|
| Architecture | x86_64 / amd64 |
| Firmware | BIOS or UEFI, with the selected mode recorded |
| Accelerator | KVM preferred; software emulation acceptable for a slow smoke test |
| CPU | 4 virtual CPUs recommended |
| Memory | 8 GiB RAM recommended |
| Disk | Optional for boot-only validation; installer validation uses a separate disk gate |
| Network | User-mode NAT or default isolated virtual networking |
| Display | Default graphical display or serial console with equivalent boot visibility |
| Host sharing | No host shared folders, passthrough devices, bridged lab networks, or private mounts required |

## Required Inputs

Run these checks from the repository root before starting the VM:

```bash
scripts/verify_public_snapshot.sh .
scripts/generate_iso_checksum.sh --check output/straylight-os-1.0.0-amd64.iso
```

Required ISO files:

- `output/straylight-os-1.0.0-amd64.iso`
- `output/straylight-os-1.0.0-amd64.iso.sha256`

Optional QEMU packages on Debian-compatible hosts:

```bash
sudo apt-get install -y qemu-system-x86 ovmf
```

## Example QEMU Boot

The exact VM command may vary by host and firmware mode. Keep release notes at
the runtime-class level rather than copying host-specific command output.

Generic BIOS-compatible smoke test:

```bash
qemu-system-x86_64 \
  -machine q35,accel=kvm:tcg \
  -cpu max \
  -smp 4 \
  -m 8192 \
  -boot d \
  -cdrom output/straylight-os-1.0.0-amd64.iso \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
```

For UEFI validation, add the host-appropriate OVMF firmware option and record
the firmware mode as `UEFI`. Do not publish the local firmware path in release
notes.

## Boot Success Criteria

Mark VM boot as `passed` only when all required criteria are met:

- The ISO checksum verifies before boot.
- The VM starts without ISO read, firmware, or boot-media errors.
- The boot loader appears or the VM proceeds directly into the live boot path.
- The kernel and initramfs load without a kernel panic.
- The live root filesystem is found and mounted.
- The boot reaches the expected live target:
  - graphical live session, display manager, or desktop; or
  - documented live shell target when a graphical environment is intentionally
    unavailable for that build.
- No emergency shell, unrecovered systemd failure, or repeated boot loop blocks
  the live target.

If the VM reaches a console shell but the release target is graphical, mark the
result as `failed` or `gated` with failure class `display-target` unless the
release notes explicitly define console boot as the target for that candidate.

## Failure Classes

Use one of these public failure classes when summarizing a failed or gated boot:

| Failure class | Meaning |
|---------------|---------|
| `artifact-mismatch` | ISO or checksum sidecar is missing, stale, or does not verify |
| `firmware` | VM firmware cannot see or start the boot media |
| `boot-loader` | Boot menu or loader stage fails before kernel handoff |
| `kernel-initramfs` | Kernel or initramfs fails before live root discovery |
| `live-rootfs` | Live root filesystem cannot be found or mounted |
| `display-target` | System boots but does not reach the expected graphical target |
| `systemd-target` | Boot enters emergency mode or fails required live target units |
| `runtime` | VM runtime configuration prevents a meaningful test |
| `not-run` | Boot validation has not been attempted for this ISO candidate |

## Public Log Summary

Do not publish raw boot logs, host terminal transcripts, screenshots containing
host UI details, packet captures, VM disk images, or private lab reports.

Use a compact summary like this in release notes or issue updates:

```markdown
### VM Boot

- Runtime: amd64 VM, BIOS or UEFI, 4 vCPU class, 8 GiB RAM class, NAT networking
- ISO: `output/straylight-os-1.0.0-amd64.iso`
- Checksum: verified before boot
- Target: graphical live session
- Result: passed, failed, gated, or not run
- Failure class: `<class or none>`
- Public summary: `<one to three sentences without host details or raw log lines>`
- Follow-up: `<issue link or blocker summary>`
```

Acceptable public summaries describe the stage and result, for example:

- `Boot loader appeared, kernel and initramfs loaded, and the live desktop target was reached.`
- `Kernel loaded, but the live root filesystem was not found; failure class live-rootfs.`
- `Boot was not run for this ISO candidate; validation remains gated.`

Do not include:

- hostnames or usernames
- local IP addresses, MAC addresses, interface names, serials, or machine IDs
- absolute personal paths
- raw kernel logs, systemd logs, or installer logs
- credentials, tokens, private certificates, or `.env` contents

## Release Notes Mapping

Update `docs/RELEASE_NOTES_TEMPLATE.md` for each artifact-bearing release:

- Set `VM boot` to `passed`, `failed`, `gated`, or `not run`.
- Record only the generic runtime class.
- Record the boot target.
- Record the failure class when the result is not `passed`.
- Keep installer, firstboot, and post-install health separate until their gates
  are run.

Do not mark a release as `Verified ISO` until VM boot, installer, firstboot, and
post-install health validation have all passed and are documented.
