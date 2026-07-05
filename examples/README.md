# Examples

These examples are sanitized starting points for a StrayLight-style source
release. They intentionally use placeholders instead of private hostnames,
interface names, serial numbers, local addresses, or machine IDs.

Use them as templates:

- `hardware-fabric.yaml` describes a controller-class workstation and gated
  accelerator surfaces.
- `xdp.conf` shows a stats/pass-through XDP stand-up configuration.
- `package-profile.json` records the expected package groups and release
  profile.
- `iso-build.env` captures the environment knobs used by the ISO build scripts.

Before publishing any adapted example, run:

```bash
scripts/straylight_release_audit.sh .
```
