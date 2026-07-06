# Validation Matrix

This matrix maps public StrayLight OS claims to their current evidence state and
next validation gate. It is intended to keep public documentation precise while
the repository remains an alpha source tree.

## Status Levels

| Status | Meaning |
|--------|---------|
| Verified | Evidence exists in the source handoff or current public repository, but scope still matters. |
| Documented | Public docs describe requirements, shape, or commands, but complete public implementation is not present. |
| Gated | Work is not public-release ready until the named validation gate passes. |
| Research | Active experiment or design track; not a supported release capability. |

## Release And Build Claims

| Claim | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Public source repository exists | Verified | README, public release, source manifest, snapshot verifier | Keep `scripts/verify_public_snapshot.sh .` green |
| MIT-licensed source snapshot | Verified | `LICENSE`, README license section | Keep license file present before accepting source contributions |
| Source-only alpha release | Verified | `CHANGELOG.md`, `docs/RELEASE_PROCESS.md`, `v0.1.0-alpha` release | Do not attach artifacts until artifact policy and validation gates pass |
| Package groups built in private/source handoff | Verified for handoff | README current verified state, `docs/STRAYLIGHT_CURRENT_STATUS.md` | Re-run package build from clean public clone |
| Public package source payload from fresh clone | Verified | `docs/PACKAGE_PAYLOAD_INVENTORY.md`, package source tree, `scripts/check_package_dependencies.sh --source-only .` | Keep source payload check green |
| Public package build from fresh clone | Verified | `scripts/build-packages.sh --clean --no-sign`, `output/debs/Packages.gz`, `docs/PACKAGE_BUILD_WRAPPER.md` | Keep package build validation green on a prepared Debian build host |
| Public/private implementation boundary | Documented | `docs/PUBLIC_SOURCE_MANIFEST.md`, `docs/EXCLUDED_IMPLEMENTATION_AREAS.md`, `SECURITY.md` | Keep excluded areas generic and promote only through roadmap gates |
| ISO source payload from fresh clone | Verified | `scripts/check_iso_candidate_requirements.sh --source-only .`, `scripts/build-iso.sh`, `iso/live-build/`, `iso/calamares/` | Keep ISO source payload check green |
| ISO build requirements documented | Verified | `docs/BUILD_ISO.md`, `docs/LIVE_BUILD_REQUIREMENTS.md`, `scripts/check_iso_candidate_requirements.sh`, `scripts/build-iso.sh`, generated ISO candidate | Keep ISO build validation green after package repository generation |
| Package repository generation | Verified | `docs/PACKAGE_REPOSITORY.md`, `scripts/generate_package_repo.sh`, `output/debs/Packages.gz`, `.gitignore`, `.gitattributes` | Keep generated repository output under `output/debs/` |
| ISO candidate checksum and release notes | Verified | `docs/ISO_CANDIDATE_RELEASE.md`, `docs/RELEASE_NOTES_TEMPLATE.md`, `docs/STRAYLIGHT_OS_1_0_0_ISO_CANDIDATE.md`, `scripts/generate_iso_checksum.sh`, checksum sidecar | Keep checksum verification green before attaching ISO candidate artifacts |
| VM boot validation flow | Verified | `docs/VM_BOOT_VALIDATION.md`, `docs/BUILD_ISO.md`, UEFI amd64 QEMU/KVM boot reached the GNOME live session | Keep VM boot validation green for each ISO candidate |
| Installer and firstboot validation flow | Verified | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, `docs/BUILD_ISO.md`, `docs/RELEASE_NOTES_TEMPLATE.md`, clean-disk UEFI amd64 VM install completed and firstboot reached graphical login | Run post-install health validation on the installed VM disk |
| Post-install health checklist | Documented | `docs/POST_INSTALL_HEALTH_CHECKLIST.md`, `docs/STRAYLIGHT_SURFACE_MAP.md`, `docs/straylight-app-clis.md`, README installed-system verification | Run post-install health validation on an installed VM disk |
| ISO artifact is publicly verified | Gated | README marks ISO as alpha test media, `docs/ARTIFACT_POLICY.md` defines attachment requirements, VM boot, installer, and firstboot passed | Pass post-install health gate and publish artifact release notes |
| Public source snapshot hygiene | Verified | `.gitattributes`, `docs/PUBLIC_SOURCE_MANIFEST.md`, verifier script | Keep CI and local verifier passing |

## Runtime And Surface Claims

| Claim | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Layered control-plane architecture | Documented | README architecture section, `docs/STRAYLIGHT_SURFACE_MAP.md`, public source tree | Add runnable smoke checks for each layer |
| Kernel module surfaces exist in source | Verified | `kernel/`, README kernel and datapath table | Run DKMS/module package build on a prepared Debian build host |
| XDP/eBPF stand-up exists in source | Verified | `kernel/xdp/`, `bin/xdp/`, README XDP notes, `docs/straylight-network.md` | Publish runtime validation from an installed VM |
| Daemons consume kernel and datapath surfaces | Verified | `services/`, README service/app propagation notes | Run post-install health checks on an installed VM |
| CLI and app surfaces are present in source | Verified | `tools/`, `apps/`, `docs/straylight-app-clis.md`, README CLI examples | Add runnable smoke checks |
| Hardware-specific accelerator paths | Gated | README limitations, roadmap research tracks | Define hardware validation gates and sanitized probes |

## Distribution Claims

| Claim | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Debian/Trixie-compatible target | Verified for ISO candidate | README status table, build docs, package, ISO, installer, and firstboot validation | Run post-install health validation on installed system behavior |
| Distro GNOME/GDM/Mutter desktop profile | Verified for installed VM firstboot | README desktop profile notes, UEFI VM boot reached GNOME live session, installed disk reached graphical login | Run post-install desktop health checks from the installed system |
| VM boot path | Verified | `docs/VM_BOOT_VALIDATION.md`, UEFI amd64 QEMU/KVM boot reached GNOME live session | Keep boot validation green for each ISO candidate |
| Installer path | Verified | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, release notes template, clean-disk UEFI VM install reached completion screen | Keep installer validation green for each ISO candidate |
| Firstboot path | Verified | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, release notes template, installed VM booted without ISO to graphical login | Run post-install health validation on the installed VM disk |
| Post-install health | Documented | `docs/POST_INSTALL_HEALTH_CHECKLIST.md`, release notes template | Run post-install health validation on an installed VM disk |

## Research Claims

| Track | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Swarm scheduling and coordination | Research | README research tracks, FAQ, glossary | Define public experiment protocol and validation criteria |
| GA-GPT2 transformer experiments | Research | README research tracks, FAQ, glossary | Publish sanitized experiment notes after source/privacy review |
| AURA/XIT packet work | Research | README research tracks, FAQ, glossary | Define packet model, examples, and safety boundaries |
| Device Memory ABI | Research | README research tracks, FAQ, glossary | Define public ABI sketch and validation plan |
| Ledger work | Research | README research notes boundary | Publish only after separate source and privacy review |

## Public Issue Mapping

| Gate | Public tracker |
|------|----------------|
| Complete public source tree | Milestone `v0.2.0-alpha: Complete Public Source Tree` |
| Reproducible ISO candidate | Milestone `v0.3.0-alpha: Reproducible ISO Candidate` |
| VM boot and installer validation | Milestone `v0.4.0-alpha: VM Boot And Installer Validation` |
| Verified public release | Milestone `v1.0.0-rc: Verified Public Release` |

## Maintenance Rule

When adding or changing a public capability claim, update this matrix in the
same pull request or commit. Every new claim should have a status, public
evidence, and next validation gate.
