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
| Public package build from fresh clone | Gated | `packaging/STRAYLIGHT_PACKAGE_SPLIT.md`, `docs/CLEAN_CLONE_PACKAGE_CHECK.md`, `docs/PACKAGE_BUILD_WRAPPER.md`, `scripts/check_package_dependencies.sh`, `scripts/build-packages.sh`, roadmap milestone | Run package build on a prepared Debian build host |
| Public/private implementation boundary | Documented | `docs/PUBLIC_SOURCE_MANIFEST.md`, `docs/EXCLUDED_IMPLEMENTATION_AREAS.md`, `SECURITY.md` | Keep excluded areas generic and promote only through roadmap gates |
| ISO source payload from fresh clone | Verified | `scripts/check_iso_candidate_requirements.sh --source-only .`, `scripts/build-iso.sh`, `iso/live-build/`, `iso/calamares/` | Keep ISO source payload check green |
| ISO build requirements documented | Documented | `docs/BUILD_ISO.md`, `docs/LIVE_BUILD_REQUIREMENTS.md`, `docs/RELEASE_PROCESS.md`, `scripts/check_iso_candidate_requirements.sh`, `iso/live-build/`, `iso/calamares/` | Build packages and generate package repository before ISO candidate build |
| Package repository generation | Documented | `docs/PACKAGE_REPOSITORY.md`, `scripts/generate_package_repo.sh`, `.gitignore`, `.gitattributes` | Generate `Packages.gz` from public package outputs before ISO candidate builds |
| ISO candidate checksum and release notes | Documented | `docs/ISO_CANDIDATE_RELEASE.md`, `docs/RELEASE_NOTES_TEMPLATE.md`, `scripts/generate_iso_checksum.sh` | Generate and verify checksum before attaching ISO candidate artifacts |
| VM boot validation flow | Documented | `docs/VM_BOOT_VALIDATION.md`, `docs/BUILD_ISO.md`, `docs/RELEASE_NOTES_TEMPLATE.md` | Run VM boot validation on an ISO candidate |
| Installer and firstboot validation flow | Documented | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, `docs/BUILD_ISO.md`, `docs/RELEASE_NOTES_TEMPLATE.md` | Run installer and firstboot validation on an ISO candidate |
| Post-install health checklist | Documented | `docs/POST_INSTALL_HEALTH_CHECKLIST.md`, `docs/STRAYLIGHT_SURFACE_MAP.md`, `docs/straylight-app-clis.md`, README installed-system verification | Run post-install health validation on an installed VM disk |
| ISO artifact is publicly verified | Gated | README marks ISO as alpha test media, `docs/ARTIFACT_POLICY.md` defines attachment requirements | Build reproducible ISO candidate and pass VM boot/install gates |
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
| Debian/Trixie-compatible target | Documented | README status table, build docs | Validate package and ISO flow on a clean public build host |
| Distro GNOME/GDM/Mutter desktop profile | Documented | README desktop profile notes | Validate installed desktop behavior from public ISO candidate |
| VM boot path | Documented | `docs/VM_BOOT_VALIDATION.md`, release notes template | Run VM boot validation on an ISO candidate |
| Installer path | Documented | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, release notes template | Run VM installer validation on an ISO candidate |
| Firstboot path | Documented | `docs/INSTALLER_FIRSTBOOT_VALIDATION.md`, release notes template | Run firstboot validation on an installed VM disk |
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
