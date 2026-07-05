# ADR 0001: Source-Only Public Alpha Starter

## Status

Accepted.

## Context

The public repository needs to be useful to outside readers and contributors
without publishing private lab state, generated artifacts, or incomplete
implementation paths that imply a supported distribution.

The source handoff includes verified package and ISO preparation facts, but the
public repository is not yet a complete clean-clone build tree.

## Decision

The public repository is maintained as a source-only public alpha starter until
the complete public source tree gate is passed.

The repository may include:

- Documentation.
- Sanitized examples.
- Package layout guidance.
- Release hygiene scripts.
- GitHub workflows and community process files.
- Source-only release tags.

The repository must not include:

- Generated ISO, package, kernel module, VM disk, trace, or cache artifacts.
- Private hostnames, local addresses, MAC addresses, serials, machine IDs, or
  personal paths.
- Credentials, tokens, SSH material, `.env` files, or private ops state.
- Unsupported claims that a public clone can build or install a verified
  StrayLight OS image.

## Consequences

- Public releases can be tagged and useful without attaching artifacts.
- The README may describe verified handoff facts only when the public gate is
  also clear.
- Contributors can work on documentation, examples, release hygiene, and source
  publication issues before the full implementation surface is public.
- Any future move beyond source-only status requires updating the validation
  matrix, release process, roadmap, and public source manifest.

## References

- `docs/PUBLIC_SOURCE_MANIFEST.md`
- `docs/VALIDATION_MATRIX.md`
- `docs/ROADMAP.md`
- `docs/RELEASE_PROCESS.md`
- `CHANGELOG.md`
