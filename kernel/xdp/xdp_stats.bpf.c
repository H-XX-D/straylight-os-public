// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — XDP Statistics Collector
 * Copyright (C) 2026 StrayLight Systems
 *
 * Per-CPU packet and byte counters, protocol breakdown, latency tracking.
 * Intended to be loaded alongside the filter/redirect programs.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "xdp_maps.h"

/* ---- Per-protocol counters --------------------------------------------- */

enum proto_idx {
	PROTO_TCP     = 0,
	PROTO_UDP     = 1,
	PROTO_ICMP    = 2,
	PROTO_OTHER   = 3,
	PROTO_ARP     = 4,
	PROTO_IPV6    = 5,
	PROTO_MAX     = 6,
};

struct proto_stats {
	__u64 packets;
	__u64 bytes;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, PROTO_MAX);
	__type(key, __u32);
	__type(value, struct proto_stats);
} proto_stats_map SEC(".maps");

/* ---- Packet size histogram --------------------------------------------- */

/*
 * Bucket boundaries (bytes):
 * [0]    0-63
 * [1]   64-127
 * [2]  128-255
 * [3]  256-511
 * [4]  512-1023
 * [5] 1024-1517
 * [6] 1518+  (jumbo)
 */
#define HISTOGRAM_BUCKETS       7

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, HISTOGRAM_BUCKETS);
	__type(key, __u32);
	__type(value, __u64);
} pkt_size_hist SEC(".maps");

/* ---- Per-interface counters -------------------------------------------- */

struct if_stats {
	__u64 rx_packets;
	__u64 rx_bytes;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_HASH);
	__uint(max_entries, 256);
	__type(key, __u32);             /* ifindex */
	__type(value, struct if_stats);
} if_stats_map SEC(".maps");

/* ---- Helpers ----------------------------------------------------------- */

static __always_inline __u32 pkt_size_bucket(__u32 len)
{
	if (len < 64)
		return 0;
	if (len < 128)
		return 1;
	if (len < 256)
		return 2;
	if (len < 512)
		return 3;
	if (len < 1024)
		return 4;
	if (len < 1518)
		return 5;
	return 6;
}

static __always_inline __u32 get_l4_proto(void *data, void *data_end,
					  __u16 eth_proto)
{
	if (eth_proto == ETH_P_IP) {
		struct iphdr *iph = data + sizeof(struct ethhdr);

		if ((void *)(iph + 1) > data_end)
			return PROTO_OTHER;

		switch (iph->protocol) {
		case IPPROTO_TCP:
			return PROTO_TCP;
		case IPPROTO_UDP:
			return PROTO_UDP;
		case IPPROTO_ICMP:
			return PROTO_ICMP;
		default:
			return PROTO_OTHER;
		}
	}

	if (eth_proto == ETH_P_IPV6) {
		struct ipv6hdr *ip6h = data + sizeof(struct ethhdr);

		if ((void *)(ip6h + 1) > data_end)
			return PROTO_IPV6;

		switch (ip6h->nexthdr) {
		case IPPROTO_TCP:
			return PROTO_TCP;
		case IPPROTO_UDP:
			return PROTO_UDP;
		case IPPROTO_ICMPV6:
			return PROTO_ICMP;
		default:
			return PROTO_IPV6;
		}
	}

	if (eth_proto == ETH_P_ARP)
		return PROTO_ARP;

	return PROTO_OTHER;
}

/* ---- XDP entry point --------------------------------------------------- */

SEC("xdp")
int straylight_xdp_stats(struct xdp_md *ctx)
{
	void *data     = (void *)(unsigned long)ctx->data;
	void *data_end = (void *)(unsigned long)ctx->data_end;
	struct ethhdr *eth = data;
	__u32 pkt_len;
	__u16 eth_proto;
	__u32 proto_idx;
	__u32 bucket;
	__u32 action_key;
	__u32 ifindex;

	/* Ethernet header check */
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	pkt_len   = (__u32)(data_end - data);
	eth_proto = bpf_ntohs(eth->h_proto);

	/* ---- Global XDP action stats ---- */
	action_key = XDP_PASS; /* We always pass — this is a stats-only prog */
	{
		struct xdp_stats_entry *st;

		st = bpf_map_lookup_elem(&stats_map, &action_key);
		if (st) {
			st->rx_packets++;
			st->rx_bytes += pkt_len;
			st->passed++;
		}
	}

	/* ---- Protocol stats ---- */
	proto_idx = get_l4_proto(data, data_end, eth_proto);
	{
		struct proto_stats *ps;

		ps = bpf_map_lookup_elem(&proto_stats_map, &proto_idx);
		if (ps) {
			ps->packets++;
			ps->bytes += pkt_len;
		}
	}

	/* ---- Packet size histogram ---- */
	bucket = pkt_size_bucket(pkt_len);
	{
		__u64 *count;

		count = bpf_map_lookup_elem(&pkt_size_hist, &bucket);
		if (count)
			(*count)++;
	}

	/* ---- Per-interface stats ---- */
	ifindex = ctx->ingress_ifindex;
	{
		struct if_stats *ifs;

		ifs = bpf_map_lookup_elem(&if_stats_map, &ifindex);
		if (ifs) {
			ifs->rx_packets++;
			ifs->rx_bytes += pkt_len;
		} else {
			struct if_stats new_ifs = {
				.rx_packets = 1,
				.rx_bytes   = pkt_len,
			};
			bpf_map_update_elem(&if_stats_map, &ifindex,
					    &new_ifs, BPF_ANY);
		}
	}

	/* Always pass — this program is statistics-only */
	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
