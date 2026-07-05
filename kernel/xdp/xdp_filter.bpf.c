// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — XDP Packet Filter
 * Copyright (C) 2026 StrayLight Systems
 *
 * Drops packets from blocked IPs (IPv4 + IPv6), applies per-source
 * rate limiting, passes all other traffic.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "xdp_maps.h"

/* ---- IPv4 filter ------------------------------------------------------- */

static __always_inline int filter_ipv4(struct xdp_md *ctx,
				       void *data, void *data_end)
{
	struct iphdr *iph = data + sizeof(struct ethhdr);
	__u32 src_ip;
	__u64 *blocked_ts;
	struct rate_limit_entry *rl;
	struct rate_limit_config *rl_cfg;
	__u32 cfg_key = 0;

	/* Bounds check */
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;

	src_ip = iph->saddr;

	/* Check blocklist */
	blocked_ts = bpf_map_lookup_elem(&blocklist, &src_ip);
	if (blocked_ts)
		return XDP_DROP;

	/* Rate limiting */
	rl_cfg = bpf_map_lookup_elem(&rate_limit_cfg, &cfg_key);
	if (rl_cfg && rl_cfg->pps_limit > 0) {
		__u64 now = bpf_ktime_get_ns();

		rl = bpf_map_lookup_elem(&rate_limit_map, &src_ip);
		if (rl) {
			/* Check if we're still in the same window */
			if ((now - rl->window_start_ns) < rl_cfg->window_ns) {
				rl->packet_count++;
				if (rl->packet_count > rl_cfg->pps_limit)
					return XDP_DROP;
			} else {
				/* New window */
				rl->packet_count = 1;
				rl->window_start_ns = now;
			}
		} else {
			/* First packet from this source */
			struct rate_limit_entry new_entry = {
				.packet_count = 1,
				.window_start_ns = now,
			};
			bpf_map_update_elem(&rate_limit_map, &src_ip,
					    &new_entry, BPF_ANY);
		}
	}

	return XDP_PASS;
}

/* ---- IPv6 filter ------------------------------------------------------- */

static __always_inline int filter_ipv6(struct xdp_md *ctx,
				       void *data, void *data_end)
{
	struct ipv6hdr *ip6h = data + sizeof(struct ethhdr);
	struct ipv6_addr src_addr;
	__u64 *blocked_ts;

	/* Bounds check */
	if ((void *)(ip6h + 1) > data_end)
		return XDP_PASS;

	__builtin_memcpy(&src_addr.addr, &ip6h->saddr, 16);

	/* Check IPv6 blocklist */
	blocked_ts = bpf_map_lookup_elem(&blocklist_v6, &src_addr);
	if (blocked_ts)
		return XDP_DROP;

	return XDP_PASS;
}

/* ---- Stats recording helper -------------------------------------------- */

static __always_inline void record_action(__u32 action, __u32 pkt_len)
{
	struct xdp_stats_entry *stats;

	if (action >= XDP_ACTION_MAX)
		return;

	stats = bpf_map_lookup_elem(&stats_map, &action);
	if (stats) {
		stats->rx_packets++;
		stats->rx_bytes += pkt_len;

		switch (action) {
		case XDP_DROP:
			stats->dropped++;
			break;
		case XDP_PASS:
			stats->passed++;
			break;
		case XDP_REDIRECT:
			stats->redirected++;
			break;
		default:
			break;
		}
	}
}

/* ---- XDP entry point --------------------------------------------------- */

SEC("xdp")
int straylight_xdp_filter(struct xdp_md *ctx)
{
	void *data     = (void *)(unsigned long)ctx->data;
	void *data_end = (void *)(unsigned long)ctx->data_end;
	struct ethhdr *eth = data;
	__u16 eth_proto;
	__u32 pkt_len = 0;
	int action = XDP_PASS;

	/* Ethernet header bounds check */
	if ((void *)(eth + 1) > data_end) {
		action = XDP_ABORTED;
		goto out;
	}

	pkt_len = (__u32)(data_end - data);
	eth_proto = bpf_ntohs(eth->h_proto);

	switch (eth_proto) {
	case ETH_P_IP:
		action = filter_ipv4(ctx, data, data_end);
		break;
	case ETH_P_IPV6:
		action = filter_ipv6(ctx, data, data_end);
		break;
	case ETH_P_ARP:
		/* Always pass ARP */
		action = XDP_PASS;
		break;
	default:
		/* Unknown protocol — pass through */
		action = XDP_PASS;
		break;
	}

out:
	record_action(action, pkt_len);
	return action;
}

char _license[] SEC("license") = "GPL";
