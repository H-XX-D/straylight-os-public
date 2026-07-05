# Publication Checklist

Use this checklist before publishing a public StrayLight release or source
snapshot.

## Required Checks

```bash
scripts/verify_public_snapshot.sh .
```

## Confirm

- README and docs describe alpha/gated work accurately.
- `.gitignore` excludes generated output, private ops files, credentials, and
  local machine state.
- No private hostnames, local IPs, MAC addresses, serials, or personal paths are
  present.
- Repository-local Markdown links and anchors resolve.
- Shipped shell scripts pass `bash -n`.
- Public snapshot verification passes from a clean working tree.
- Build and ISO commands are documented separately from verified release status.
- License text is present before accepting contributions or distributing source
  code beyond documentation/examples.
