# Validation Matrix

This matrix maps public StrayLight OS claims to their current evidence state and
next validation gate. It is intended to keep public documentation precise while
the repository remains a source-only alpha starter.

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
| Public starter repository exists | Verified | README, public release, source manifest, snapshot verifier | Keep `scripts/verify_public_snapshot.sh .` green |
| MIT-licensed source snapshot | Verified | `LICENSE`, README license section | Keep license file present before accepting source contributions |
| Source-only alpha release | Verified | `CHANGELOG.md`, `docs/RELEASE_PROCESS.md`, `v0.1.0-alpha` release | Do not attach artifacts until artifact policy and validation gates pass |
| Package groups built in private/source handoff | Verified for handoff | README current verified state, `docs/STRAYLIGHT_CURRENT_STATUS.md` | Publish complete public source tree and clean-clone package checks |
| Public package build from fresh clone | Gated | `packaging/STRAYLIGHT_PACKAGE_SPLIT.md`, `docs/PACKAGE_PAYLOAD_INVENTORY.md`, `docs/CLEAN_CLONE_PACKAGE_CHECK.md`, `docs/PACKAGE_BUILD_WRAPPER.md`, `scripts/check_package_dependencies.sh`, `scripts/build-packages.sh`, roadmap milestone | Complete `v0.2.0-alpha` milestone issues |
| Public/private implementation boundary | Documented | `docs/PUBLIC_SOURCE_MANIFEST.md`, `docs/EXCLUDED_IMPLEMENTATION_AREAS.md`, `SECURITY.md` | Keep excluded areas generic and promote only through roadmap gates |
| ISO build requirements documented | Documented | `docs/BUILD_ISO.md`, `docs/LIVE_BUILD_REQUIREMENTS.md`, `docs/RELEASE_PROCESS.md`, `scripts/check_iso_candidate_requirements.sh` | Add complete public source and live-build configuration |
| Package repository generation | Documented | `docs/PACKAGE_REPOSITORY.md`, `scripts/generate_package_repo.sh`, `.gitignore`, `.gitattributes` | Generate `Packages.gz` from public package outputs before ISO candidate builds |
| ISO candidate checksum and release notes | Documented | `docs/ISO_CANDIDATE_RELEASE.md`, `docs/RELEASE_NOTES_TEMPLATE.md`, `scripts/generate_iso_checksum.sh` | Generate and verify checksum before attaching ISO candidate artifacts |
| ISO artifact is publicly verified | Gated | README marks ISO as alpha test media, `docs/ARTIFACT_POLICY.md` defines attachment requirements | Build reproducible ISO candidate and pass VM boot/install gates |
| Public source snapshot hygiene | Verified | `.gitattributes`, `docs/PUBLIC_SOURCE_MANIFEST.md`, verifier script | Keep CI and local verifier passing |

## Runtime And Surface Claims

| Claim | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Layered control-plane architecture | Documented | README architecture section, `docs/STRAYLIGHT_SURFACE_MAP.md` | Publish implementation source for each layer |
| Kernel module surfaces exist in source handoff | Verified for handoff | README kernel and datapath table | Publish public module source and build checks |
| XDP/eBPF stand-up exists in source handoff | Verified for handoff | README XDP notes, `docs/straylight-network.md` | Publish policy-safe XDP examples and runtime validation |
| Daemons consume kernel and datapath surfaces | Verified for handoff | README service/app propagation notes | Publish daemon source and post-install health checks |
| CLI and app surfaces are documented | Documented | `docs/straylight-app-clis.md`, README CLI examples | Publish implementation source and runnable smoke checks |
| Hardware-specific accelerator paths | Gated | README limitations, roadmap research tracks | Define hardware validation gates and sanitized probes |

## Distribution Claims

| Claim | Current status | Public evidence | Next gate |
|-------|----------------|-----------------|-----------|
| Debian/Trixie-compatible target | Documented | README status table, build docs | Validate package and ISO flow on a clean public build host |
| Distro GNOME/GDM/Mutter desktop profile | Documented | README desktop profile notes | Validate installed desktop behavior from public ISO candidate |
| Installer path | Gated | Roadmap, release process | Complete VM installer validation issue |
| Firstboot path | Gated | Roadmap, release process | Complete firstboot validation issue |
| Post-install health | Gated | Roadmap, seeded issues | Complete post-install health checklist issue |

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
