// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 *
 * Description: ipourma ip configuration support
 */

#include <net/netlink.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/inet.h>
#include "ipourma_addr_res.h"
#include "ipourma_ip.h"
#include "ipourma_netdev.h"

#define IPV6_PREFIX_LEN 64

/* send ipv6 address by netlink */
int ipourma_send_ipv6_netlink(struct net_device *dev, union ubcore_eid *eid, int msg_type)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct ifaddrmsg *ifa;
	struct in6_addr addr;
	int ret;

	ret = ipourma_resolve_ipaddr(dev, ETH_P_IPV6, eid, &addr);
	if (ret != 0)
		return ret;

	size_t msg_size = NLMSG_SPACE(sizeof(struct ifaddrmsg))
					+ (uint32_t)nla_total_size(sizeof(struct in6_addr));
	skb = nlmsg_new(msg_size, GFP_KERNEL);
	if (IS_ERR_OR_NULL(skb)) {
		pr_err("send_ipv6_netlink:alloc skb failed\n");
		return -ENOMEM;
	}

	nlh = nlmsg_put(skb, 0, 0, msg_type, sizeof(struct ifaddrmsg),
		NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL);
	if (IS_ERR_OR_NULL(nlh)) {
		kfree_skb(skb);
		pr_err("send_ipv6_netlink:construct netlink msg failed\n");
		return -IPOURMA_NLMSG_ERR;
	}

	NETLINK_CB(skb).portid = 0;
	NETLINK_CB(skb).dst_group = 0;
	NETLINK_CB(skb).flags = NETLINK_SKB_DST;

	ifa = nlmsg_data(nlh);
	ifa->ifa_family = AF_INET6;
	ifa->ifa_prefixlen = IPV6_PREFIX_LEN;
	ifa->ifa_index = (uint32_t)dev->ifindex;
	ifa->ifa_scope = RT_SCOPE_UNIVERSE;
	ifa->ifa_flags = IFA_F_PERMANENT;

	ret = nla_put(skb, IFA_ADDRESS, sizeof(struct in6_addr), &addr);
	if (ret != 0) {
		kfree_skb(skb);
		pr_err("send_ipv6_netlink:construct netlink data failed: %d\n", ret);
		return -IPOURMA_NLDATA_ERR;
	}
	nlmsg_end(skb, nlh);

	/*
	 * send a netlink message to the kernel to simulate the ${ip addr add} command
	 * in the user space and trigger the
	 * kernel to invoke the private function ipv6_add_addr.
	 */
	ret = rtnl_unicast(skb, dev_net(dev), 0);
	if (ret != 0) {
		kfree_skb(skb);
		pr_err("send_ipv6_netlink:send netlink msg failed: %d\n", ret);
		return -IPOURMA_NL_SEND_ERR;
	}
	pr_debug("send_ipv6_netlink success\n");
	return 0;
}

void ipourma_init_ipv6_addr(struct work_struct *work)
{
	struct ipourma_dev_priv *priv;
	struct net_device *dev;
	int ret = 0;
	int i;

	priv = container_of(work, struct ipourma_dev_priv, set_ip);
	dev = priv->dev;

	/*
	 * Re-discover EIDs from eid_table in case the EID_CHANGE async event
	 * was dispatched before our handler was registered (race during probe).
	 * This also creates URMA resources (jfr, jetty) and sets the IPv6
	 * address for each discovered EID via ipourma_create_new_eid().
	 */
	if (priv->eid_count == 0 && priv->urma_dev != NULL) {
		struct ubcore_device *urma_dev = priv->urma_dev;
		uint32_t discovered = 0;

		spin_lock(&urma_dev->eid_table.lock);
		if (!IS_ERR_OR_NULL(urma_dev->eid_table.eid_entries)) {
			uint32_t max_cnt = urma_dev->attr.dev_cap.max_eid_cnt;

			for (i = 0; i < max_cnt && i < IPOURMA_MAX_EID_CNT; i++) {
				if (urma_dev->eid_table.eid_entries[i].valid &&
				    !eid_is_empty(&urma_dev->eid_table.eid_entries[i].eid) &&
				    eid_is_empty(&priv->eid_info[i].eid)) {
					priv->eid_info[i].eid =
						urma_dev->eid_table.eid_entries[i].eid;
					priv->eid_info[i].eid_index = i;
					discovered++;
				}
			}
		}
		spin_unlock(&urma_dev->eid_table.lock);

		if (discovered > 0) {
			pr_info("[ipourma] re-discovered %u EID(s) from eid_table\n",
				discovered);
			for (i = 0; i < IPOURMA_MAX_EID_CNT; i++) {
				if (eid_is_empty(&priv->eid_info[i].eid))
					continue;
				priv->eid_count++;
				atomic_add(1, &priv->need_set_ip_route);
				ipourma_create_new_eid(priv, (u32)i);
			}
			return; /* ipourma_create_new_eid handles IPv6 setup */
		}
	}

	pr_info("[ipourma] init_ipv6_addr: eid_count=%d urma_dev=%p\n",
		priv->eid_count, priv->urma_dev);

	for (i = 0; i < IPOURMA_MAX_EID_CNT; i++) {
		if (eid_is_empty(&priv->eid_info[i].eid))
			continue;
		pr_info("[ipourma] init_ipv6_addr: send ipv6 for eid_idx=%d\n", i);
		ret = ipourma_send_ipv6_netlink(dev, &(priv->eid_info[i].eid), RTM_NEWADDR);
		pr_info("[ipourma] init_ipv6_addr: send_ipv6 ret=%d\n", ret);
		if (ret != 0)
			goto ipv6_uninit;
	}

	pr_info("init_ipv6_addr success\n");
	return;

ipv6_uninit:
	for (i--; i >= 0; i--)
		ipourma_send_ipv6_netlink(dev, &(priv->eid_info[i].eid), RTM_DELADDR);

	pr_err("init_ipv6_addr failed\n");
}
