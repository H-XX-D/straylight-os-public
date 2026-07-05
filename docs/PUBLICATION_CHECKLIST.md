# Publication Checklist

Use this checklist before publishing a public StrayLight release or source
snapshot.

## Required Checks

```bash
scripts/straylight_release_audit.sh .
python3 scripts/check_markdown_links.py .
scripts/check_shell_syntax.sh .
git status --short
```

## Confirm

- README and docs describe alpha/gated work accurately.
- `.gitignore` excludes generated output, private ops files, credentials, and
  local machine state.
- No private hostnames, local IPs, MAC addresses, serials, or personal paths are
  present.
- Repository-local Markdown links and anchors resolve.
- Shipped shell scripts pass `bash -n`.
- Build and ISO commands are documented separately from verified release status.
- License text is present before accepting contributions or distributing source
  code beyond documentation/examples.
