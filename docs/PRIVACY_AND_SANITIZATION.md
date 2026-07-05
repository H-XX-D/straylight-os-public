# Privacy And Sanitization

Before publishing a StrayLight source snapshot, remove anything that identifies
a private machine, network, user, filesystem, or live operational environment.

Do not publish:

- Personal filesystem paths such as absolute personal home-directory paths.
- Local IP addresses, MAC addresses, serial numbers, machine IDs, or host keys.
- Private hostnames or interface names from a live lab.
- SSH keys, API tokens, `.env` files, credentials, secrets, or authorized keys.
- Generated ISO, package, kernel, benchmark, trace, cache, and live-build output.
- Lab-only service units, private ops scripts, one-off scheduler timers, or node
  inventory files unless converted into generic examples.

Prefer:

- Relative paths.
- Placeholder hostnames such as `<controller>` or `<node>`.
- Sanitized examples with fake hashes and fake identifiers.
- Explicit alpha/gated language for unfinished hardware paths.

Run the release audit before committing:

```bash
scripts/straylight_release_audit.sh .
```
