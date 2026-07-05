// SPDX-License-Identifier: GPL-2.0
/*
 * StrayLight OS — XDP Packet Redirect
 * Copyright (C) 2026 StrayLight Systems
 *
 * Redirects packets to a target interface via bpf_redirect_map().
 * Supports VLAN-based and destination-IP-based routing decisions.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "xdp_maps.h"

struct vlan_hdr {
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

/* ---- Destination-based routing table ----------------------------------- */

/*
 * Maps destination IPv4 subnet (masked to /24) to a redirect port index.
 * This is a simple longest-prefix-match approximation using /24 aggregation.
 */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, __u32);             /* destination IP & 0xFFFFFF00 */
	__type(value, __u32);           /* port index into redirect_map */
} route_table SEC(".maps");

/* ---- VLAN to port mapping ---------------------------------------------- */

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 4096);
	__type(key, __u16);             /* VLAN ID */
	__type(value, __u32);           /* port index into redirect_map */
} vlan_map SEC(".maps");

/* ---- Helpers ----------------------------------------------------------- */

static __always_inline __u32 get_default_port(void)
{
	__u32 key = 0;
	struct redirect_config *cfg;

	cfg = bpf_map_lookup_elem(&redirect_cfg, &key);
	if (cfg)
		return cfg->default_port;

	return 0;
}

static __always_inline int do_redirect(struct xdp_md *ctx, __u32 port)
{
	int ret;

	ret = bpf_redirect_map(&redirect_map, port, XDP_PASS);
	if (ret == XDP_REDIRECT) {
		/* Record redirect in stats */
		__u32 action = XDP_REDIRECT;
		struct xdp_stats_entry *stats;

		stats = bpf_map_lookup_elem(&stats_map, &action);
		if (stats) {
			stats->tx_packets++;
			stats->tx_bytes += (__u64)(
				(void *)(unsigned long)ctx->data_end -
				(void *)(unsigned long)ctx->data);
			stats->redirected++;
		}
	}

	return ret;
}

/* ---- IPv4 routing ------------------------------------------------------ */

static __always_inline int redirect_ipv4(struct xdp_md *ctx,
					 void *data, void *data_end)
{
	struct iphdr *iph = data + sizeof(struct ethhdr);
	__u32 dst_subnet;
	__u32 *port;

	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;

	/* Mask to /24 for route lookup */
	dst_subnet = iph->daddr & bpf_htonl(0xFFFFFF00);

	port = bpf_map_lookup_elem(&route_table, &dst_subnet);
	if (port)
		return do_redirect(ctx, *port);

	/* No specific route — use default port */
	return do_redirect(ctx, get_default_port());
}

/* ---- VLAN routing ------------------------------------------------------ */

static __always_inline int redirect_vlan(struct xdp_md *ctx,
					 void *data, void *data_end)
{
	struct ethhdr *eth = data;
	struct vlan_hdr *vhdr;
	__u16 vlan_id;
	__u32 *port;

	if (eth->h_proto != bpf_htons(ETH_P_8021Q) &&
	    eth->h_proto != bpf_htons(ETH_P_8021AD))
		return -1; /* not a VLAN frame */

	vhdr = (void *)(eth + 1);
	if ((void *)(vhdr + 1) > data_end)
		return XDP_PASS;

	vlan_id = bpf_ntohs(vhdr->h_vlan_TCI) & 0x0FFF;

	port = bpf_map_lookup_elem(&vlan_map, &vlan_id);
	if (port)
		return do_redirect(ctx, *port);

	return -1; /* no VLAN mapping, fall through */
}

/* ---- XDP entry point --------------------------------------------------- */

SEC("xdp")
int straylight_xdp_redirect(struct xdp_md *ctx)
{
	void *data     = (void *)(unsigned long)ctx->data;
	void *data_end = (void *)(unsigned long)ctx->data_end;
	struct ethhdr *eth = data;
	__u16 eth_proto;
	int ret;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	eth_proto = bpf_ntohs(eth->h_proto);

	/* Try VLAN-based redirect first */
	if (eth_proto == ETH_P_8021Q || eth_proto == ETH_P_8021AD) {
		ret = redirect_vlan(ctx, data, data_end);
		if (ret >= 0)
			return ret;
		/* Fall through to IP-based redirect */
	}

	/* IP-based redirect */
	switch (eth_proto) {
	case ETH_P_IP:
		return redirect_ipv4(ctx, data, data_end);

	case ETH_P_IPV6:
		/*
		 * For IPv6, redirect to default port.
		 * Full IPv6 LPM can be added with BPF_MAP_TYPE_LPM_TRIE.
		 */
		return do_redirect(ctx, get_default_port());

	case ETH_P_ARP:
		/* ARP is broadcast — pass to kernel stack */
		return XDP_PASS;

	default:
		/* Unknown — redirect to default */
		return do_redirect(ctx, get_default_port());
	}
}

char _license[] SEC("license") = "GPL";
