# Post-Install Health Checklist

This document defines the public post-install health checklist for a StrayLight
OS ISO candidate after VM boot, installer, and firstboot validation have
completed.

Post-install health validation proves that the installed system exposes the
expected StrayLight package, service, CLI, app, and surface checks without using
private machine identifiers or lab-only assumptions. It does not prove
production support status or hardware-specific accelerator readiness.

## Scope

Run this checklist on the installed system after firstboot reaches the target
documented in `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`.

This gate verifies:

- The installed system is the expected architecture and release class.
- Required StrayLight packages are installed.
- Required systemd services are not failed.
- Core StrayLight CLIs return usable output.
- Kernel and proc/sys surfaces are present when the matching package or module
  is expected for the release profile.
- Desktop app CLI inventory is available for the desktop profile.
- Optional hardware, XDP, and accelerator checks are categorized separately.
- Results are summarized without private host details or raw logs.

This gate does not verify:

- Real-hardware compatibility beyond the test VM.
- Datacenter-scale GPU or accelerator behavior.
- Production XDP filter or redirect policy.
- Research-track functionality as supported release behavior.
- Upgrade behavior from an older StrayLight install.

## Privacy Rules

Do not publish raw command output when it contains private host details. Public
release notes may include command names, pass/fail status, generic failure
classes, and one to three sentence summaries.

Do not include:

- hostnames or usernames
- local IP addresses, MAC addresses, interface names, serials, or machine IDs
- absolute personal paths
- raw kernel logs, systemd logs, installer logs, packet captures, or traces
- credentials, tokens, private certificates, or `.env` contents

Use placeholders such as `<validated-interface>`, `<service>`, and `<app-id>`
when examples need a local value.

## Required Command Checklist

Run commands from a terminal on the installed system. Use `sudo` only where the
command requires it on the target distribution.

| Area | Command | Expected success criteria |
|------|---------|---------------------------|
| Architecture | `uname -m` | Prints `x86_64` for the amd64 release profile. |
| Package presence | `dpkg-query -W straylight-os` | Package is installed and reports a version. |
| Package state | `dpkg-query -f '${db:Status-Abbrev} ${binary:Package}\n' -W 'straylight-*'` | Required StrayLight packages show installed state; missing optional packages are listed as alpha limitations only when not required by the release profile. |
| System state | `systemctl is-system-running` | Returns `running`; `degraded` is acceptable only when every failed unit is documented as a known alpha limitation and not required for the release gate. |
| Failed services | `systemctl --failed --no-pager` | No required StrayLight or base desktop services are failed. |
| StrayLight services | `systemctl list-units 'straylight-*' --type=service --all --no-pager` | Expected services are present for installed package groups; disabled optional services are categorized as alpha limitations when not required. |
| Health CLI | `straylight-health status --json` | Exits 0 and returns parseable JSON with no blocker status for required checks. |
| App inventory | `straylight-app-cli list --json` | Exits 0 and returns parseable JSON for the desktop app catalog. |
| App doctor | `straylight-app-cli doctor <app-id>` | Representative required apps return success or a documented non-blocking warning. |
| Enclave CLI | `straylight-enclave info` | Exits 0 and separates kernel device state from hardware SGX availability. Lack of hardware SGX is an alpha limitation when the VM CPU does not expose it. |
| Scheduler surface | `test -r /proc/straylight/sched/status && cat /proc/straylight/sched/status` | Surface is readable when the scheduler package/module is required; missing surface is a blocker for profiles that require it. |
| Entropy surface | `test -r /proc/straylight/entropy && cat /proc/straylight/entropy` | Surface is readable when the entropy package/module is required; command output does not need to be published raw. |
| Hypervisor surface | `test -r /proc/straylight/hv/stats && cat /proc/straylight/hv/stats` | Surface is readable when the hypervisor package/module is required; no active VM count is required. |
| Kernel modules | `lsmod | awk '$1 ~ /^straylight/ {print $1}'` | Required StrayLight modules appear for the selected release profile. |
| VPU device | `test -e /dev/straylight-vpu` | Device node exists when the VPU package/module is required. |
| XDP units | `systemctl list-units 'straylight-xdp@*.service' --all --no-pager` | XDP units are present only when configured; absence is not a blocker unless the release profile requires XDP. |
| XDP stats | `straylight-xdp stats --iface <validated-interface> --skb` | Optional unless a public release profile declares a validated interface and pass-through policy. Do not publish the local interface name. |
| Critical boot errors | `journalctl -b -p err --no-pager` | No repeated required-service failures or kernel errors that block the release profile. Summarize categories only. |

## Optional Parse Checks

When `jq` is available, these checks make JSON validation explicit:

```bash
straylight-health status --json | jq .
straylight-app-cli list --json | jq .
```

Expected result: each command exits 0 and `jq` parses the output successfully.

## Blockers

Treat these as release blockers for a verified ISO:

- `straylight-os` metapackage is missing.
- Required StrayLight package groups are not installed.
- `systemctl is-system-running` reports failed state or required services are
  failed.
- `straylight-health status --json` fails, returns invalid JSON, or reports a
  blocker for required checks.
- `straylight-app-cli list --json` fails or returns invalid JSON for a desktop
  profile.
- Required kernel, proc, sysfs, or device surfaces are missing for the selected
  release profile.
- Required firstboot units remain failed after firstboot.
- Kernel errors or service failures prevent the documented desktop or console
  profile from operating.
- Health evidence cannot be summarized without private host details.

## Known Alpha Limitations

These may remain non-blocking when they do not prevent the required installed
system profile from operating:

- Hardware SGX is not available in the VM CPU, while enclave software reports
  the limitation cleanly.
- Optional accelerator, FPGA, GPU, RDMA, or PMEM paths are absent in the VM.
- XDP filter or redirect policy is not enabled because no public packet policy
  was selected.
- XDP stats are not run because no public-safe interface name is declared.
- A non-critical app doctor check returns a warning after app inventory and
  launch metadata are available.
- Cosmetic desktop branding, icon, theme, or wallpaper differences.
- Slow service startup under software-emulated virtualization.
- Research-track features remain gated or experimental.

Reclassify an alpha limitation as a blocker if it prevents package integrity,
required services, required CLIs, required surfaces, user access, or the
documented desktop or console profile.

## Public Result Summary

Use this release-note shape for post-install health:

```markdown
### Post-Install Health

- Runtime: installed amd64 VM, BIOS or UEFI, generic vCPU/RAM class
- Commands: package, systemd, health CLI, app CLI, required surface checks
- Result: passed, failed, gated, or not run
- Failure class: `<class or none>`
- Blockers: `<public blocker summary or none>`
- Known alpha limitations: `<non-blocking limitations or none>`
- Public summary: `<one to three sentences without host details or raw log lines>`
```

Failure classes:

| Failure class | Meaning |
|---------------|---------|
| `package-state` | Required StrayLight package or metapackage is missing or not installed cleanly |
| `service-state` | Required systemd service is failed or blocks the release profile |
| `health-cli` | `straylight-health` fails, returns invalid JSON, or reports a blocker |
| `app-cli` | Desktop app CLI inventory or representative doctor check fails |
| `kernel-surface` | Required kernel/proc/sys/device surface is missing |
| `xdp-required` | XDP is required by the release profile but cannot be validated |
| `boot-errors` | Kernel or boot-session errors block the installed profile |
| `privacy-report` | Available evidence contains private host details and cannot be published |
| `not-run` | Post-install health validation has not been attempted for this ISO candidate |

## Release Notes Mapping

Update `docs/RELEASE_NOTES_TEMPLATE.md` for each artifact-bearing release:

- Set `Post-install health` to `passed`, `failed`, `gated`, or `not run`.
- List command categories instead of raw logs.
- Record the failure class when the result is not `passed`.
- Put release blockers in `Gated Or Experimental Behavior` or a dedicated
  blocker list.
- Put non-blocking issues in `Known Limitations`.

Do not mark a release as `Verified ISO` until VM boot, installer, firstboot, and
post-install health validation have all passed and are documented.
