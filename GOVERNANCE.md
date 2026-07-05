# Governance

StrayLight OS is currently maintained as an alpha public starter repository.
The governance model is intentionally simple until the project has a complete
public source tree and a broader contributor base.

## Maintainer Authority

The repository owner is the final decision maker for:

- Release scope and tagging.
- Public source inclusion.
- Security-sensitive changes.
- Claims about verified, alpha, experimental, or gated behavior.
- Issue and pull request triage.

## Decision Criteria

Changes are evaluated against:

- Technical accuracy.
- Public release hygiene.
- Clear alpha and gate labeling.
- Reproducibility from a clean Debian-compatible host.
- Avoidance of private infrastructure details.
- Alignment with the roadmap.

## Review Expectations

Pull requests should be small, documented, and verifiable. At minimum, public
changes should pass the checks listed in `CONTRIBUTING.md` unless the pull
request explains why a check is not applicable.

Security-sensitive changes may require additional review before merge. Do not
use public comments to share secrets, exploit payloads, or private deployment
details.

## Future Governance

If the project grows beyond a single-maintainer alpha snapshot, this file should
be updated to describe maintainer roles, release authority, review rules, and
how new maintainers are added.
