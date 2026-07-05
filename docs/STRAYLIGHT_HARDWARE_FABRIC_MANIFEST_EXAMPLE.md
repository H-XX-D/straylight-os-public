# Hardware Fabric Manifest Example

This is a sanitized example. Do not replace placeholders with private hostnames,
MAC addresses, local IP addresses, serials, or machine IDs in public docs.

```yaml
schema: straylight.hardware_fabric_manifest.v1
controller: <controller>
compute:
  cpu_class: dual-socket-xeon-class
  cores: 40
  threads: 80
  numa_nodes: 2
  memory_gib: 93
accelerators:
  gpu:
    class: local-inference-workstation-gpu
    vram_gib: 16
  fpga_stcr:
    state: gated
    note: endpoint probes and safe-controller gates required before operation
network:
  switch_facing:
    class: 10gbe
    interface: <switch-facing-interface>
storage:
  nvme: 2
  sata: 2
surfaces:
  hypervisor: online
  vpu: online
  scheduler: online
  entropy: online
  enclave: online_without_hardware_sgx_in_sample
  xdp: stats_pass_through
```
