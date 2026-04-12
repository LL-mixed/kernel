// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * Description: ipourma address resolution
 */
#include <linux/netdevice.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
#include <linux/in6.h>
#include <linux/if_arp.h>
#include "ipourma_err.h"
#include "ipourma_addr_res.h"

static void ipourma_resolve_eids_from_ipv6(struct net_device *dev,
	struct in6_addr *src, struct in6_addr *dst,
	union ubcore_eid *src_eid, union ubcore_eid *dst_eid)
{
	memcpy(src_eid, src, UBCORE_EID_SIZE);
	memcpy(dst_eid, dst, UBCORE_EID_SIZE);
}

static void ipourma_map_ipv4_to_eid(const __be32 *src, const __be32 *dst,
	union ubcore_eid *src_eid, union ubcore_eid *dst_eid)
{
	memset(src_eid, 0, UBCORE_EID_SIZE);
	src_eid->raw[0] = 0xfe;
	src_eid->raw[1] = 0x80;
	memcpy(&src_eid->raw[12], src, sizeof(*src));

	memset(dst_eid, 0, UBCORE_EID_SIZE);
	dst_eid->raw[0] = 0xfe;
	dst_eid->raw[1] = 0x80;
	memcpy(&dst_eid->raw[12], dst, sizeof(*dst));
}

static void ipourma_resolve_eids_from_arp(struct sk_buff *skb,
	union ubcore_eid *src_eid, union ubcore_eid *dst_eid)
{
	struct arphdr *arp;
	u8 *arp_ptr;
	__be32 src_ip;
	__be32 dst_ip;
	u32 need_len;

	if (unlikely(skb->len < sizeof(struct arphdr))) {
		memset(src_eid, 0, UBCORE_EID_SIZE);
		memset(dst_eid, 0, UBCORE_EID_SIZE);
		return;
	}

	arp = (struct arphdr *)skb->data;
	if (unlikely(arp->ar_hln == 0 || arp->ar_pln != 4)) {
		memset(src_eid, 0, UBCORE_EID_SIZE);
		memset(dst_eid, 0, UBCORE_EID_SIZE);
		return;
	}

	need_len = sizeof(struct arphdr) + 2 * (arp->ar_hln + arp->ar_pln);
	if (unlikely(skb->len < need_len)) {
		memset(src_eid, 0, UBCORE_EID_SIZE);
		memset(dst_eid, 0, UBCORE_EID_SIZE);
		return;
	}

	arp_ptr = (u8 *)(arp + 1);
	/* SHA */
	arp_ptr += arp->ar_hln;
	/* SPA */
	memcpy(&src_ip, arp_ptr, sizeof(src_ip));
	arp_ptr += arp->ar_pln;
	/* THA */
	arp_ptr += arp->ar_hln;
	/* TPA */
	memcpy(&dst_ip, arp_ptr, sizeof(dst_ip));

	ipourma_map_ipv4_to_eid(&src_ip, &dst_ip, src_eid, dst_eid);
}

void ipourma_resolve_eids(struct net_device *dev, struct sk_buff *skb, u16 proto,
			  union ubcore_eid *src_eid, union ubcore_eid *dst_eid)
{
	struct ipv6hdr *ipv6h;
	struct iphdr *ipv4h;

	switch (proto) {
	case ETH_P_IPV6:
		ipv6h = (struct ipv6hdr *)skb_network_header(skb);
		ipourma_resolve_eids_from_ipv6(dev, &ipv6h->saddr,
						&ipv6h->daddr, src_eid, dst_eid);
		break;
	case ETH_P_IP:
		ipv4h = (struct iphdr *)skb_network_header(skb);
		ipourma_map_ipv4_to_eid(&ipv4h->saddr, &ipv4h->daddr, src_eid, dst_eid);
		break;
	case ETH_P_ARP:
		ipourma_resolve_eids_from_arp(skb, src_eid, dst_eid);
		break;
	default:
		netdev_dbg(dev, "%s\n", ipourma_err_desc(IPOURMA_UNSUPPORTED_ETH_PROTO));
		break;
	}
}

static int ipourma_resolve_ipv6_from_eids(struct net_device *dev,
	union ubcore_eid *eid, struct in6_addr *addr)
{
	memcpy(addr, eid, UBCORE_EID_SIZE);
	return IPOURMA_OK;
}

int ipourma_resolve_ipaddr(struct net_device *dev, u16 proto,
			   union ubcore_eid *eid, struct in6_addr *addr)
{
	switch (proto) {
	case ETH_P_IPV6:
		return ipourma_resolve_ipv6_from_eids(dev, eid, addr);
	case ETH_P_IP:
		/* For IPv4, extract from EID fe80::xxxx mapped format */
		memcpy(addr, eid, UBCORE_EID_SIZE);
		return IPOURMA_OK;
	default:
		netdev_dbg(dev, "%s\n", ipourma_err_desc(IPOURMA_UNSUPPORTED_ETH_PROTO));
		return IPOURMA_UNSUPPORTED_ETH_PROTO;
	}
	return IPOURMA_OK;
}
