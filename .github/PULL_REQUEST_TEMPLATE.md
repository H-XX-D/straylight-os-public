## Summary

Describe the change and why it belongs in the public starter repository.

## Scope

- [ ] Documentation
- [ ] Sanitized example
- [ ] Packaging or build guidance
- [ ] Release hygiene or CI
- [ ] Other

## Verification

Paste the commands you ran:

```bash
scripts/straylight_release_audit.sh .
python3 scripts/check_markdown_links.py .
scripts/check_shell_syntax.sh .
```

## Public Release Hygiene

- [ ] No personal paths, private hostnames, local IPs, MAC addresses, serials, or machine IDs.
- [ ] No credentials, tokens, SSH material, `.env` files, or generated lab output.
- [ ] Claims are labeled as verified, alpha, experimental, or gated.
- [ ] Build or ISO statements include the relevant prerequisites or validation gate.
