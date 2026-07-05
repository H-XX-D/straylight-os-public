/* SPDX-License-Identifier: GPL-2.0 */
/*
 * StrayLight OS — XDP BPF Map Definitions
 * Copyright (C) 2026 StrayLight Systems
 *
 * Shared map definitions for the XDP filter, redirect, and stats programs.
 */

#ifndef _STRAYLIGHT_XDP_MAPS_H
#define _STRAYLIGHT_XDP_MAPS_H

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* ---- IP blocklist ------------------------------------------------------ */
/*
 * Hash map keyed by IPv4 address (network byte order).
 * Value: u64 timestamp of when the entry was added (ns since boot).
 * Max 65536 entries — large enough for production blocklists.
 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, __u32);             /* IPv4 address in network order */
	__type(value, __u64);           /* timestamp_ns                  */
} blocklist SEC(".maps");

/* ---- IPv6 blocklist ---------------------------------------------------- */
/*
 * Hash map keyed by IPv6 address (16 bytes).
 */
struct ipv6_addr {
	__u8 addr[16];
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 32768);
	__type(key, struct ipv6_addr);
	__type(value, __u64);
} blocklist_v6 SEC(".maps");

/* ---- Redirect target map ----------------------------------------------- */
/*
 * DEVMAP: maps logical port index to network interface ifindex.
 * Used by xdp_redirect.bpf.c for bpf_redirect_map().
 */
struct {
	__uint(type, BPF_MAP_TYPE_DEVMAP);
	__uint(max_entries, 256);
	__type(key, __u32);             /* logical port index */
	__type(value, __u32);           /* target ifindex     */
} redirect_map SEC(".maps");

/* ---- Configuration map ------------------------------------------------- */
/*
 * Array map with a single entry containing redirect configuration:
 *   [0] = default redirect target port index
 */
struct redirect_config {
	__u32 default_port;
	__u32 flags;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct redirect_config);
} redirect_cfg SEC(".maps");

/* ---- Per-CPU packet/byte statistics ------------------------------------ */

struct xdp_stats_entry {
	__u64 rx_packets;
	__u64 rx_bytes;
	__u64 tx_packets;
	__u64 tx_bytes;
	__u64 dropped;
	__u64 redirected;
	__u64 errors;
	__u64 passed;
};

/*
 * PERCPU_ARRAY: one stats entry per XDP action type.
 * Index:
 *   0 = XDP_ABORTED
 *   1 = XDP_DROP
 *   2 = XDP_PASS
 *   3 = XDP_TX
 *   4 = XDP_REDIRECT
 */
#define XDP_ACTION_MAX  5

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, XDP_ACTION_MAX);
	__type(key, __u32);
	__type(value, struct xdp_stats_entry);
} stats_map SEC(".maps");

/* ---- Rate limiting map ------------------------------------------------- */
/*
 * Per-source-IP rate counters for DDoS mitigation.
 * Key: source IPv4 address.
 * Value: packet count in current window.
 */
struct rate_limit_entry {
	__u64 packet_count;
	__u64 window_start_ns;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 131072);
	__type(key, __u32);
	__type(value, struct rate_limit_entry);
} rate_limit_map SEC(".maps");

/* Rate limit config (packets per second) */
struct rate_limit_config {
	__u64 pps_limit;
	__u64 window_ns;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct rate_limit_config);
} rate_limit_cfg SEC(".maps");

#endif /* _STRAYLIGHT_XDP_MAPS_H */
