# ADR 0002: Gate-Based Release Model

## Status

Accepted.

## Context

StrayLight OS includes package, kernel, networking, desktop, installer, ISO, and
research surfaces. Some facts are verified in source handoff, some are only
documented in the public starter, and some remain active research.

A date-based roadmap would blur those states. A stable-looking release number
without validation would also overstate readiness.

## Decision

Public releases use gate-based promotion.

The major gates are:

1. Complete public source tree.
2. Reproducible ISO candidate.
3. VM boot and installer validation.
4. Verified public release.

Each public capability claim should identify:

- Current status.
- Public evidence.
- Next validation gate.

GitHub milestones track the public gates, and `docs/VALIDATION_MATRIX.md`
captures the current claim state.

## Consequences

- Alpha tags can ship source and documentation snapshots without implying
  binary distribution support.
- Artifact-bearing releases require explicit validation notes and checksums.
- Research tracks remain labeled as research until their own public validation
  criteria exist.
- Public issues can be organized by gate rather than by aspirational deadlines.

## References

- `docs/ROADMAP.md`
- `docs/VALIDATION_MATRIX.md`
- `docs/RELEASE_PROCESS.md`
- GitHub milestones for `v0.2.0-alpha`, `v0.3.0-alpha`, `v0.4.0-alpha`, and
  `v1.0.0-rc`
