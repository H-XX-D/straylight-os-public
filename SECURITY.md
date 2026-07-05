# Security Policy

StrayLight OS is currently an alpha public starter repository. It includes
documentation, sanitized examples, package layout guidance, release hygiene
scripts, and build requirements. It does not yet represent a supported
production distribution.

## Supported Scope

Security review is appropriate for:

- Public documentation that could cause unsafe deployment assumptions.
- Release hygiene scripts and CI checks.
- Sanitized example manifests and configuration snippets.
- Packaging or ISO build guidance published in this repository.

Security reports about private lab infrastructure, unpublished source trees, or
local machine state cannot be handled through this public repository.
See `docs/EXCLUDED_IMPLEMENTATION_AREAS.md` for the public/private
implementation boundary.

## Reporting A Vulnerability

Use GitHub's private vulnerability reporting for this repository when available.
If private reporting is not available, open a public issue only when the report
does not include secrets, exploit details for an unpatched issue, private host
data, or personal infrastructure details.

Do not include:

- Credentials, tokens, keys, or session material.
- Private IP addresses, MAC addresses, hostnames, paths, serials, or machine IDs.
- Weaponized exploit payloads.
- Logs or traces from systems you do not own or administer.

## What To Include

For a useful report, include:

- A concise summary of the issue.
- Affected file, section, script, or example.
- Why the behavior is unsafe or misleading.
- Minimal reproduction steps using sanitized placeholders.
- Suggested mitigation, if known.

## Alpha Status

Because this project is alpha, security-sensitive claims should be treated as
gated until verified. Documentation should clearly distinguish working build
paths, experimental research tracks, and unsupported deployment scenarios.
