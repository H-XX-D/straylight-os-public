# StrayLight App CLI Surface

The desktop app layer is exposed through a dispatcher and per-app launch shims.

Representative commands:

```bash
straylight-app-cli list --json
straylight-app-cli info <app-id>
straylight-app-cli doctor <app-id>
straylight-app-cli open <app-id>
straylight-app-cli doc <app-id>
```

Common verbs are `help`, `info`, `doctor`, `path`, `open`, `exec`, and `doc`.
