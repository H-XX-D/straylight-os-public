# FAQ

## What is this repository?

This is the public alpha source repository for StrayLight OS. It contains
documentation, sanitized examples, package-layout guidance, release hygiene
scripts, community process files, package source payloads, and ISO source
paths.

It does not currently ship a verified package or ISO artifact.

## Is StrayLight OS a Linux distribution?

StrayLight OS is framed as a Debian/Ubuntu-compatible operating environment. The
current public docs describe package groups, kernel and eBPF surfaces, daemons,
CLIs, desktop apps, and an ISO build path on top of a Debian-compatible base.

The public source tree contains the package and ISO source paths needed for the
documented preflight checks. Package and ISO artifacts still require clean-host
build validation before they are treated as public release outputs.

## Can I build the ISO from this repository today?

Not until package builds have produced a local package repository under
`output/debs/`. The repository documents what is required to build the ISO and
contains the public ISO source paths, but generated packages and ISO artifacts
are intentionally excluded from source.

Use `docs/BUILD_ISO.md`, `docs/PUBLIC_SOURCE_MANIFEST.md`, and
`docs/ROADMAP.md` to understand the build requirements and promotion gates.

## Why does the README mention an ISO artifact?

The README records the verified state of the private/live preparation work while
also stating the public distribution gate. In the public repository, ISO-related
claims should be read as alpha/gated unless a GitHub release explicitly attaches
verified artifacts and validation notes.

## What does "public alpha source snapshot" mean?

It means the public repository is suitable for reviewing documentation,
examples, release hygiene, package source, ISO source layout, and build gates.
It is not a supported binary release and not a production distribution.

## What should contributors work on first?

Start with package build validation, ISO candidate validation, documentation
fixes, and sanitized examples. Keep generated artifacts, host details, and
private deployment data out of pull requests.

## What is the William Gibson / Neuromancer reference?

It is a design-lineage reference. StrayLight uses that aesthetic direction to
describe a workstation where cyberspace, hardware, agents, and machine state are
treated as one operable surface. It is not a claim of fictional capability.

## Are the swarm, GA-GPT2, AURA/XIT, and Device Memory ABI tracks finished?

No. They are active research tracks and should stay labeled as experiments or
scaffolds until their validation gates are defined and passed.

## Can I submit real hardware or network details in an issue?

Do not publish private hostnames, local addresses, MAC addresses, serials,
machine IDs, personal paths, logs, credentials, or private lab topology. Convert
examples to generic placeholders before opening an issue or pull request.

## How do I verify a public snapshot?

Run:

```bash
scripts/verify_public_snapshot.sh .
```

The verifier runs the release audit, Markdown link check, shell syntax check,
and clean working-tree check.

## Where should security-sensitive reports go?

Follow `SECURITY.md`. Do not put secrets, exploit payloads, private host data,
or unredacted logs in public issues.

## Why are GitHub issue templates and pull request templates excluded from source archives?

They are repository-hosting metadata rather than runtime or build inputs. The
repository keeps them in Git for collaboration, but `.gitattributes` excludes
them from generated source archives.
