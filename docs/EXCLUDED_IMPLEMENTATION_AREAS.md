# Excluded Implementation Areas

This document defines the public/private boundary for implementation areas that
are not included in the current public source snapshot.

The goal is to be explicit without exposing private lab details. Exclusions are
named by generic implementation class, not by private host, path, node,
interface, key, address, or operator workflow.

## Current Boundary

The current repository is a source-only public alpha tree. It contains public
documentation, sanitized examples, package layout guidance, package preflight
checks, release hygiene scripts, community process files, and the package source
payloads listed in `docs/PACKAGE_PAYLOAD_INVENTORY.md`.

It does not contain generated package artifacts, generated ISO artifacts,
private lab state, local experiment caches, raw operational traces, or
hardware-specific deployment state.

## Excluded Areas

| Area | Public-safe reason for exclusion | Gate before inclusion |
|------|----------------------------------|-----------------------|
| Private-only implementation remnants | Local-only helpers, abandoned experiments, and unpublished work that are not part of the current release profile need separate source, dependency, and privacy review. | Add to the package inventory and pass release audit before inclusion. |
| Private lab host inventory and deployment state | Hostnames, interface names, addresses, serials, machine IDs, and local topology can identify private infrastructure. | Replace with sanitized examples and pass release audit. |
| Generated package and ISO artifacts | `.deb`, package indexes, ISO images, VM disks, logs, and live-build working trees are build outputs, not source. | Attach only under the artifact policy after checksum and validation gates. |
| Build caches and transient working directories | Caches, traces, temporary outputs, and live-build chroots are not reproducible source inputs. | Keep ignored; regenerate from documented commands. |
| Raw logs, traces, and packet captures | Raw operational data can include local addresses, interface names, identifiers, payload fragments, or timing data. | Publish only sanitized summaries or synthetic examples. |
| Large model weights and experiment caches | Model artifacts and caches can be large, license-bound, unreproducible, or tied to private runs. | Publish only when license, provenance, checksums, and experiment protocol are documented. |
| Hardware-specific accelerator enablement | FPGA/STCR, SGX-dependent paths, advanced NIC policy, and device-specific probes need explicit safety and hardware gates. | Define public validation gates and sanitized probes before release claims. |
| XDP filter and redirect policy | Non-pass-through packet policy can affect live traffic and must be reviewed before public deployment guidance. | Publish policy-safe examples and runtime validation steps. |
| Private ops automation | Local timers, scripts, node maintenance flows, and one-off deployment helpers can encode private assumptions. | Convert into generic, documented, reviewed public scripts. |
| Secrets and credentials | Keys, tokens, credentials, certificates, authorized keys, and session material must never be committed. | Never include; use local secret management outside the repository. |

## What Can Be Published Instead

Use public-safe replacements:

- Repository-relative source paths.
- Sanitized examples under `examples/`.
- Synthetic hostnames such as `<controller>` and `<node>`.
- Fake addresses, fake hashes, and fake identifiers clearly marked as examples.
- Aggregated experiment summaries without private input data.
- Validation scripts that inspect repository-relative paths and documented host
  packages only.
- Release notes that distinguish verified behavior from gated or experimental
  behavior.

## Inclusion Checklist

Before moving an excluded implementation area into the public repository:

1. Map it to a package group in `docs/PACKAGE_PAYLOAD_INVENTORY.md`.
2. Remove private host, user, path, network, interface, hardware, and credential
   material.
3. Confirm generated artifacts remain ignored and absent from source.
4. Add or update package build, ISO build, or validation commands.
5. Update `docs/VALIDATION_MATRIX.md` with public evidence and the next gate.
6. Run `scripts/verify_public_snapshot.sh .` from a clean checkout.

## Relationship To Security Reports

Security reports for unpublished source trees, private infrastructure, or local
machine state cannot be handled through the public repository. Public reports
should use sanitized placeholders and refer only to files, examples, scripts, or
docs that are present in the public tree.
