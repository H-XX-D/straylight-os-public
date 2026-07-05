# StrayLight OS

> A Debian/Ubuntu-compatible operating environment for hardware-aware AI,
> systems automation, kernel-surface observability, high-performance networking,
> and recovery-focused workstation workflows.

StrayLight is to AI research and development as Kali is to cybersecurity. It installs kernel modules,
eBPF datapaths, daemons, CLIs, GUI applications, packaging metadata, and
operator documentation on top of a bare-bones Linux desktop or server stack. The
current release profile uses distribution-provided GNOME/GDM/Mutter rather than
shipping a custom StrayLight compositor or window manager.

StrayLight keeps a Linux machine understandable and controllable by exposing
kernel surfaces, daemons, CLIs, and apps as a coherent machine-readable control
plane, then translating that state back into concise operator-readable output.

| Status | Current state |
|--------|---------------|
| Release maturity | Alpha distribution-prep image |
| Base target | Debian Trixie-compatible amd64 |
| Install model | Debian package groups plus `straylight-os` metapackage |
| Desktop model | Distro GNOME/GDM/Mutter plus StrayLight apps |
| ISO artifact | `output/straylight-os-1.0.0-amd64.iso` |
| Verified package build | 8 package groups succeeded, 0 failed |
| Public distribution gate | VM boot, install, firstboot, and post-install health validation still required |

## Contents

- [Design Lineage](#design-lineage)
- [What StrayLight Provides](#what-straylight-provides)
- [Current Verified State](#current-verified-state)
- [Architecture](#architecture)
- [Kernel And Datapath Surfaces](#kernel-and-datapath-surfaces)
- [Research Tracks](#research-tracks)
- [Package Groups](#package-groups)
- [Examples](#examples)
- [Build From Source](#build-from-source)
- [ISO Build Requirements](#iso-build-requirements)
- [Installed-System Verification](#installed-system-verification)
- [Limitations](#limitations)
- [Documentation](#documentation)

## Design Lineage

StrayLight is intentionally a William Gibson / *Neuromancer* throwback: a
workstation that treats cyberspace, hardware, agents, and machine state as one
operable surface instead of separate abstractions. The reference is aesthetic
and directional, not a claim of fictional capability.

In practical terms, that means visible control planes, addressable machine
surfaces, typed packets, and operator-readable traces. Prose is used for
inspection and control. The operational state is carried by devices, buffers,
routes, ledgers, hashes, metrics, and policy decisions.

## What StrayLight Provides

StrayLight makes the machine communicate in the forms the machine actually
understands, then exposes those states through human-readable tools and panels.

| Layer | Role |
|-------|------|
| Kernel and eBPF | Hypervisor, VPU allocator, scheduler, entropy, enclave, and XDP datapath surfaces |
| Daemons | Health, scheduler, entropy, mesh, fabric, predict, quota, splice, policy, power, thermal, and related runtime services |
| CLIs | `straylight-*` tools for structured state inspection and machine actions |
| Apps and widgets | Operator-facing panels for system state, diagnostics, and workflows |
| Packaging and ISO | Debian package groups and a live-build ISO path |

The control layer favors structured output where it matters. Most operational
commands expose JSON for automation and compact text for direct operator use.

```bash
straylight-health status --json
straylight-app-cli list --json
straylight-intent "<question or request>"
```

## Current Verified State

The current source tree has completed package and ISO build preparation for an
alpha test image.

| Area | Verified state |
|------|----------------|
| Package build | 8 package groups succeeded, 0 failed |
| Package repository | 15 `.deb` packages plus `Packages.gz` |
| ISO artifact | `output/straylight-os-1.0.0-amd64.iso` |
| ISO checksum | `output/straylight-os-1.0.0-amd64.iso.sha256` |
| ISO label | `STRAYLIGHT_1_0_0` |
| Release audit | Clean after package and ISO builds |
| Desktop profile | Distro GNOME/GDM/Mutter with StrayLight apps and app CLI |

The ISO is alpha test media. Treat it as distribution-prep output until VM boot,
installer, firstboot, and post-install health gates pass.

## Architecture

StrayLight is organized around explicit machine surfaces rather than a single
monolithic application.

1. Kernel modules and eBPF programs expose low-level state through `/dev`,
   `/proc/straylight`, `/sys/kernel/straylight-vpu`, and systemd-managed XDP
   attachment points.
2. Daemons consume those surfaces and publish service-level health, scheduling,
   policy, network, prediction, quota, and recovery state.
3. CLIs provide repeatable commands for automation, diagnostics, and operator
   actions.
4. GUI apps and widgets present the same state in desktop workflows.
5. Debian packages and live-build configuration assemble the current release
   profile.

The result is a layered control plane: kernel and datapath surfaces at the
bottom, services and policy in the middle, and human/operator interfaces at the
top.

## Kernel And Datapath Surfaces

The live controller has six low-level surfaces represented as first-class
architecture.

| Surface | Runtime path | Verified condition |
|---------|--------------|--------------------|
| Hypervisor | `/dev/straylight-hv`, `/proc/straylight/hv/stats` | Loaded with KVM/VMX available; no active VMs in the sample state |
| VPU allocator | `/dev/straylight-vpu`, `/sys/kernel/straylight-vpu` | Loaded; exposes slab usage and GPU metadata |
| Scheduler | `/proc/straylight/sched/*` | Loaded; reports 80 CPUs and 2 NUMA nodes in the sample state |
| Entropy | `/proc/straylight/entropy` | Loaded; RDRAND, RDSEED, and jitter are available in the sample state |
| Enclave | `/dev/straylight-enclave`, `/dev/straylight/sgx` | Device is online; the sample CPU does not expose hardware SGX |
| XDP/eBPF | `/usr/lib/straylight/bpf/*.bpf.o`, `straylight-xdp@<iface>.service` | Stats/pass-through program active on a 10 GbE switch-facing interface in the sample state |

XDP is eBPF, not a `.ko`. The XDP DKMS entry is a build-validation hook only.
The `xdp_stats` program is pass-through and suitable for initial stand-up.
`xdp_filter` and `xdp_redirect` require explicit packet policy before use.

### Service And App Propagation

The kernel surfaces are wired into runtime services and operator-facing tools;
they are not isolated demonstrations.

- The Hub service panel lists the hypervisor, VPU, scheduler `.ko`, entropy
  `.ko`, enclave `.ko`, and XDP surfaces.
- The entropy daemon reports `kernel_source` data from
  `/proc/straylight/entropy`.
- The entropy widget reads live kernel entropy state.
- The scheduler widget reads
  `/proc/straylight/sched/{status,policy,tasks}` when scheduler IPC is absent.
- The enclave CLI reports kernel device state, `/dev/straylight/sgx`, and CPU
  SGX support separately.
- Quota, splice, predict, and preloader prefer live
  `/sys/kernel/straylight-vpu` paths and keep older `/sys/class` fallbacks.

### Hardware Fabric Example

A controller-class StrayLight installation can coordinate CPU, memory, GPU,
storage, virtualization, and network surfaces through the same control-plane
model.

The current sanitized live-controller example includes:

- Dual Intel Xeon Gold 5218R class CPUs.
- 40 physical cores and 80 threads.
- 12 total memory channels, split across two NUMA nodes.
- Approximately 93 GiB RAM in the sample system.
- NVIDIA GeForce RTX 5060 Ti 16 GiB in the sample system.
- Two 2 TB NVMe drives and two 3 TB SATA disks in the sample system.
- One 10 GbE switch-facing NIC plus additional 1 GbE links.
- KVM/VMX and the StrayLight hypervisor surface online.

The FPGA/STCR accelerator path is documented as a gated accelerator surface. It
is part of the StrayLight fabric model and scheduler planning, but it is not
represented as production-online until endpoint devices and probes pass.

## Research Tracks

StrayLight has been used as a live systems lab for swarm coordination,
transformer-control experiments, and machine-native packet routing. These are
measured research results and scaffolds, not production guarantees.

### Swarm And Fleet Coordination

- A a reference controller plus Jetson Nano swarm was used for staged distributed rendering: the Z6
  coordinator compiled and staged a C++ worker on each Nano, each Nano rendered
  a horizontal image band locally, and the the controller pulled the RGB tiles back for
  stitching.
- The bridge dataplane schedule-fleet proof path validates XIT sidecar receipts
  for routes such as `<controller>.nvidia.bridge_dataplane.scheduler.stamp.fleet`,
  records proof events into a swarm ledger, verifies ledger anchors, and
  publishes cached status for the UI.
- The UI data adapter and shell expose swarm readiness, schedule-fleet proof
  state, ledger anchor status, node counts, and VPU readiness as operator-facing
  state.

### GA-GPT2 And Transformer Control

- Geometric Algebra transformer experiments showed the strongest current win at
  vector-level steering of the attention path, not prose-level orchestration.
  The GA path operates over Q/K/V, value vectors, rotor/wedge transforms, KV
  cache, and hidden-vector surfaces.
- Historical GPT-2 runs showed that GA zero-rotor / wedge paths can prevent
  repetition collapse. The strongest recorded result was GPT-2 XL / 1558M Tier
  2 with 3/3 runs avoiding repetition collapse and 3/3 reaching natural EOS.
- Stronger reference-controller route tests found high-gain steering around scale `50.0` to be
  the first tested range that meaningfully moved the distribution while keeping
  readable prose. Extreme scales collapsed coherence.
- Layer-masked KV rebase in the `.cog` build was more useful than all-layer KV
  injection. A quick pass found selected masks such as `4,5,6,10,28` more
  interesting than injecting every layer.
- Rotor banks and WEBM routes remain experimental. Some historical rotor runs
  were promising, but current reproducibility work must log exact model hashes,
  rotor files, prompts, seeds, scales, sampling settings, and trace metrics.
- Sidecar evidence packets improved exact-answer performance for `dolphin3:8b`
  in a small reference controller ablation, but the same eval shape did not produce a clear win
  for GA-GPT2. GA-GPT2 behaved like a completion model, so future tests should
  use completion/cloze tasks or an adapter that matches that engine.

### AURA / XIT Packet Work

- Native Exchange CPU code parses `XIT1` frames, summarizes route/function,
  source/target, payload size, hashes, content address, and AURA-Micro LUT
  handshake metadata without storing raw payload bytes in durable ledgers.
- The scheduler can choose metadata-first routing, stage encoded bytes near a
  VPU, FPGA, NIC lane, or target, and materialize payloads on demand when route
  policy permits.
- Hardware-facing paths can append compact `exchange.cpu_ledger_event.v1`
  records directly from C, giving Python and higher-level services replayable
  facts without requiring Python to produce the native packets.
- The Device Memory ABI extends the same idea to vector/tensor work: CPU, GPU,
  FPGA, storage, and graph stages exchange buffer references, dtype/shape,
  hashes, metrics, and control flags while prose remains an inspection layer.
  CPU and CUDA `copy_surface` / `norm_delta` baselines have been validated on
  the reference controller; XRT/HBM and real transformer KV/residual integration remain future work.

## Package Groups

| Package | Purpose |
|---------|---------|
| `libstraylight-common1` | Shared runtime library used by StrayLight tools and apps |
| `libstraylight-common-dev` | Development headers and CMake package metadata |
| `straylight-core` | Core daemons, bus-facing services, entropy, scheduler, and base control tools |
| `straylight-kernel` | DKMS module sources and module-load paths |
| `straylight-ml` | ML, model, prediction, quantum, photonics, and SNN subsystems |
| `straylight-network` | XDP, DPDK/RDMA paths, network, bridge, swarm, and transport tooling |
| `straylight-exotic` | Enclave, PMEM, FUSE, sandbox, RHEM, and related advanced subsystems |
| `straylight-desktop` | Graphical apps, app CLI layer, OOBE, wizard, widgets, and firstboot helpers |
| `straylight-os` | Metapackage for the complete install profile |

## Examples

The `examples/` directory contains sanitized templates people can adapt when
building their own StrayLight-style source tree:

- [hardware-fabric.yaml](examples/hardware-fabric.yaml) for controller,
  accelerator, network, and kernel-surface inventory.
- [xdp.conf](examples/xdp.conf) for safe stats/pass-through XDP stand-up.
- [package-profile.json](examples/package-profile.json) for package groups,
  build order, release outputs, and gates.
- [iso-build.env](examples/iso-build.env) for build-script environment knobs.

Keep examples generic. Replace placeholders only in private deployment repos,
and run the release audit before publishing adapted files.

## App And CLI Surface

StrayLight desktop apps ship with a native multicall CLI layer.

- Main dispatcher: `straylight-app-cli`
- Current release app catalog: 45 app launch shims
- Common commands: `help`, `info`, `doctor`, `path`, `open`, `exec`, `doc`
- Coverage: every executable in the current release desktop build is represented
  in the app catalog

Some app source directories are present in the repository but are not part of
the current release launcher path. See the surface map for the release app and
service inventory.

## Build From Source

Build on a Debian Bookworm or Trixie compatible amd64 host with Debian
packaging and live-build tooling installed. The current live-build target is
Trixie.

This public starter repository documents the release shape and hygiene rules.
A working ISO build requires the full StrayLight source tree, package-specific
`debian/` directories, live-build configuration, installer configuration,
service assets, and locally built runtime `.deb` packages. See
[Build The ISO](docs/BUILD_ISO.md) for the complete checklist.

### Install Build Tools

Package build tools:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config rsync \
  dpkg-dev debhelper dh-cmake devscripts \
  dkms linux-headers-amd64
```

ISO build tools:

```bash
sudo apt-get install -y \
  live-build debootstrap squashfs-tools xorriso \
  grub-pc-bin grub-efi-amd64-bin
```

### Build Packages

Run the release audit and dependency check before building the package set:

```bash
scripts/straylight_release_audit.sh .
scripts/build-packages.sh --check-deps --no-sign
```

Build every package group:

```bash
scripts/build-packages.sh --clean --no-sign
```

Build a single package group:

```bash
scripts/build-packages.sh --no-sign straylight-desktop
```

The package builder writes `.deb` files and a local APT `Packages.gz` index
under `output/debs/`.

## ISO Build Requirements

A buildable ISO source tree must include:

- package rules under `packaging/<package>/debian/`
- `scripts/build-packages.sh`, `scripts/build-iso.sh`, and the release audit
- `iso/live-build/auto/config`
- live-build package lists for the live base and `straylight-os`
- Calamares installer config under `iso/calamares/`
- service D-Bus policy, udev rules, and optional theme/icon assets
- locally built runtime packages under `output/debs/`

The ISO build is root-only because live-build creates and configures a chroot.
Run `scripts/build-packages.sh --clean --no-sign` before `build-iso.sh` so local
runtime `.deb` packages are available for injection.

See [Build The ISO](docs/BUILD_ISO.md) for the full dependency and validation
checklist.

### Build The ISO

Run live-build configuration only when validating release sanitization and ISO
staging:

```bash
sudo scripts/build-iso.sh --clean --config-only
```

The default live-build distribution is `trixie`, matching the current package
ABI. Override it only when rebuilding all packages for a different target
distribution.

After package and configuration gates pass, build the ISO:

```bash
sudo scripts/build-iso.sh --clean
sha256sum -c output/straylight-os-1.0.0-amd64.iso.sha256
```

Expected outputs:

- `output/straylight-os-1.0.0-amd64.iso`
- `output/straylight-os-1.0.0-amd64.iso.sha256`
- `output/iso-build-<timestamp>.log`

## Installed-System Verification

Use these checks on an installed system or live controller:

```bash
lsmod | grep '^straylight'
cat /proc/straylight/sched/status
cat /proc/straylight/entropy
cat /proc/straylight/hv/stats
straylight-enclave info
systemctl status 'straylight-xdp@*.service'
straylight-xdp stats --iface <switch-facing-interface> --skb
straylight-health status --json
straylight-app-cli list --json
```

Recommended package payload checks:

```bash
zgrep -h '^Package:' output/debs/Packages.gz
dpkg-deb -c output/debs/straylight-desktop_1.0.0-1_amd64.deb | grep straylight-app-cli
```

The release package set and ISO desktop path should not contain or require
`straylight-compositor` or `straylight-shell`.

## Limitations

StrayLight is alpha software. The package and ISO layouts are buildable and
sanitized for test-image validation, but public distribution still requires VM
boot testing, installer testing, firstboot validation, post-install health
checks, and a staged source snapshot that excludes generated output and
transient files.

Current boundaries:

- The ISO still needs boot and install validation.
- Generated artifacts and working files must be excluded from public source
  snapshots.
- The custom compositor and shell are not part of the current release profile.
- Hardware SGX depends on CPU support; the sample controller runs enclave
  surfaces without CPU SGX.
- FPGA/STCR is gated and in progress.
- XDP filter and redirect modes require explicit packet policy before
  production use.
- The GPU path is suitable for local inference and workstation acceleration,
  not a claim of datacenter-scale training capability.
- The swarm, GA-GPT2, AURA/XIT, and Device Memory ABI work are active research
  tracks; they should be presented as measured experiments and scaffolds unless
  a specific deployment gate has passed.

## Documentation

- [Getting started](docs/GETTING_STARTED.md)
- [Build the ISO](docs/BUILD_ISO.md)
- [Examples](examples/README.md)
- [Current release condition](docs/STRAYLIGHT_CURRENT_STATUS.md)
- [Component surface map](docs/STRAYLIGHT_SURFACE_MAP.md)
- [Hardware fabric manifest example](docs/STRAYLIGHT_HARDWARE_FABRIC_MANIFEST_EXAMPLE.md)
- [Network and XDP notes](docs/straylight-network.md)
- [App CLI reference](docs/straylight-app-clis.md)
- [Privacy and sanitization](docs/PRIVACY_AND_SANITIZATION.md)
- [Publication checklist](docs/PUBLICATION_CHECKLIST.md)
- [Package split](packaging/STRAYLIGHT_PACKAGE_SPLIT.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Code of conduct](CODE_OF_CONDUCT.md)

Additional research notes for GA-GPT2, sidecar evals, Device Memory ABI, and
Ledger should be published only after separate source and privacy review.

## Project Hygiene

Before publishing a public source snapshot:

- Remove generated build output, live machine state, transient working files,
  and host-specific ops material.
- Keep private hostnames, local addresses, interface identifiers, serials, and
  machine IDs out of public docs.
- Keep research results labeled as experiments unless the matching deployment
  gate has passed.

## License

This public starter snapshot is released under the [MIT License](LICENSE).
