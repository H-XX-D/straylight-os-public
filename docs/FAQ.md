# FAQ

## What is this repository?

This is the public alpha starter repository for StrayLight OS. It contains
documentation, sanitized examples, package-layout guidance, release hygiene
scripts, and community process files.

It is not yet a complete public source tree and does not currently ship a
verified ISO or package artifact.

## Is StrayLight OS a Linux distribution?

StrayLight OS is framed as a Debian/Ubuntu-compatible operating environment. The
current public docs describe package groups, kernel and eBPF surfaces, daemons,
CLIs, desktop apps, and an ISO build path on top of a Debian-compatible base.

The public starter repository does not yet contain every implementation file
needed to produce a full distribution image from a fresh clone.

## Can I build the ISO from this repository today?

Not from this public starter alone. The repository documents what is required to
build the ISO and what source areas must exist, but the current public snapshot
is intentionally source-only and incomplete.

Use `docs/BUILD_ISO.md`, `docs/PUBLIC_SOURCE_MANIFEST.md`, and
`docs/ROADMAP.md` to understand the build requirements and promotion gates.

## Why does the README mention an ISO artifact?

The README records the verified state of the private/live preparation work while
also stating the public distribution gate. In the public repository, ISO-related
claims should be read as alpha/gated unless a GitHub release explicitly attaches
verified artifacts and validation notes.

## What does "source-only public alpha starter" mean?

It means the public repository is suitable for reviewing documentation, examples,
release hygiene, and source layout. It is not a supported binary release, not a
complete build tree, and not a production distribution.

## What should contributors work on first?

Start with the open issues under the `v0.2.0-alpha: Complete Public Source Tree`
milestone. Those issues focus on package payload inventory, clean-clone checks,
excluded implementation documentation, and public build wrappers.

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
