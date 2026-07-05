# StrayLight Package Split

This file lists the public binary package groups. See
[`docs/PACKAGE_PAYLOAD_INVENTORY.md`](../docs/PACKAGE_PAYLOAD_INVENTORY.md) for
the source payload paths required before each group can be built from a clean
public clone.

| Package | Purpose |
|---------|---------|
| `libstraylight-common1` | Shared runtime library |
| `libstraylight-common-dev` | Development headers and CMake metadata |
| `straylight-core` | Core daemons, bus-facing services, entropy, scheduler, and control tools |
| `straylight-kernel` | DKMS module sources and module-load paths |
| `straylight-ml` | ML, prediction, model, quantum, photonics, and SNN subsystems |
| `straylight-network` | XDP, DPDK/RDMA, network, bridge, swarm, and transport tooling |
| `straylight-exotic` | Enclave, PMEM, FUSE, sandbox, and advanced subsystems |
| `straylight-desktop` | Apps, app CLI layer, OOBE, wizard, widgets, and firstboot helpers |
| `straylight-os` | Complete install-profile metapackage |
