# Glossary

This glossary defines terms used in the public StrayLight OS starter
repository. Definitions are intentionally scoped to this repository and should
not be read as claims that every implementation path is public or complete.

## AURA/XIT Packets

A research track for packet-oriented system communication and typed exchange
surfaces. In this public repository, AURA/XIT is documented as active research,
not a completed public dataplane.

## Alpha

An early release state where behavior, interfaces, packaging, and validation
gates may still change. Alpha documentation should clearly distinguish verified
behavior from experimental or gated work.

## Build Gate

A validation point that must pass before a release claim is promoted. Examples
include clean-clone package checks, ISO build completion, VM boot, installer
validation, firstboot validation, and post-install health checks.

## Clean Clone

A fresh checkout without private local files, generated artifacts, caches,
machine-specific configuration, or uncommitted changes.

## Complete Public Source Tree

A future roadmap state where enough public implementation source exists for
outside users to run package build checks from a clean clone.

## Device Memory ABI

A research track for describing device-addressable memory surfaces and
interfaces. It is listed as active research until public source and validation
gates are defined.

## eBPF

Linux kernel technology used to run restricted programs in kernel contexts. The
public StrayLight docs mention eBPF with XDP networking and kernel observability
surfaces.

## GA-GPT2

A research track involving genetic-algorithm-style experimentation around GPT-2
transformer workflows. In this repository it is documented as experimental, not
a production model-training claim.

## ISO Candidate

An ISO image that can be built from a complete source tree but has not yet
passed every boot, install, firstboot, and post-install health gate required for
a verified public release.

## Live-Build

Debian tooling used to assemble live system images. The public docs describe the
host packages and source tree areas needed before StrayLight ISO candidates can
be built.

## Package Group

A logical Debian packaging area in the StrayLight package split, such as common,
kernel, core, ML, network, exotic, desktop, or metapackage layers.

## Public Release Hygiene

The checks and practices that keep the public repository free of private paths,
local addresses, credentials, generated artifacts, and misleading release
claims.

## Source-Only Snapshot

A GitHub release or tag that publishes source and documentation state without
attached ISO, package, VM disk, kernel module, trace, or generated binary
artifacts.

## StrayLight OS

A Debian/Ubuntu-compatible operating environment for hardware-aware AI, systems
automation, kernel-surface observability, high-performance networking, and
recovery-focused workstation workflows.

## Swarm

The StrayLight research track for coordinating multiple nodes, workers, or
agents. Public claims should remain experimental until validation gates are
documented and passed.

## Verified Public Release

A future release state where source, package, and ISO paths are reproducible and
release artifacts include documented checksums, boot/install validation,
firstboot validation, and post-install health validation.

## XDP

The Linux eXpress Data Path. StrayLight uses XDP language for high-performance
packet filtering and redirect concepts. Public XDP examples are sanitized and
use placeholders.
