# Contributing To StrayLight OS

Thanks for helping improve the public StrayLight starter repository.

This repository is an alpha public snapshot. It is meant to document the system
shape, package layout, examples, build requirements, and release hygiene for a
StrayLight-style Debian/Ubuntu-compatible operating environment. It is not yet a
complete public source release.

## Good First Contributions

Good public contributions include:

- Documentation fixes and clearer build notes.
- Sanitized examples that use placeholders instead of real host data.
- Packaging layout improvements.
- Release hygiene checks.
- Reproducibility notes for Debian-compatible build hosts.

Avoid submitting:

- Private hostnames, user paths, local IP addresses, MAC addresses, serials, or
  machine IDs.
- SSH keys, API tokens, credentials, `.env` files, or generated lab output.
- Large binary artifacts such as ISO images, package outputs, kernel modules,
  VM disks, traces, or caches.
- Claims that a hardware, kernel, network, or installer path is production-ready
  unless it has a documented validation gate.

## Before Opening A Pull Request

Run the same checks used by CI:

```bash
scripts/verify_public_snapshot.sh .
```

For documentation-only changes, these checks are usually enough. For source or
packaging changes in a complete tree, also include the package or ISO commands
you ran and the exact host profile used.

## Writing Documentation

Use plain, falsifiable language:

- Say what is verified.
- Say what is alpha, experimental, or gated.
- Prefer relative paths and generic placeholders.
- Keep build commands separate from release-status claims.
- Link to the relevant example or checklist when describing a workflow.

## Pull Request Scope

Keep pull requests small and reviewable. A good pull request should have one
clear purpose, a short summary, and a verification section.

If a change touches security-sensitive areas, networking, kernel surfaces,
packaging, or installer behavior, describe the trust boundary and the validation
performed.

## License

By contributing, you agree that your contribution is provided under the MIT
License used by this repository.
