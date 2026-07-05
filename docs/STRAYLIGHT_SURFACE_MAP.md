# StrayLight Surface Map

StrayLight is organized as a layered control plane.

| Surface | Public role |
|---------|-------------|
| Kernel modules | Hypervisor, VPU, scheduler, entropy, and enclave observability/control surfaces |
| XDP/eBPF | Explicit datapath attachment and stats/pass-through stand-up |
| Daemons | Health, policy, prediction, quota, splice, mesh, fabric, power, thermal, and related services |
| CLIs | `straylight-*` entrypoints for machine-readable state and actions |
| Apps/widgets | Operator-facing system panels |
| Packaging/ISO | Debian package groups and live-build integration |

Treat hardware-specific and accelerator paths as gated until their probes and
runtime validation pass on the target installation.
