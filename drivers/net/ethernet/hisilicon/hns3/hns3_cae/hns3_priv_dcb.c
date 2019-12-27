// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kthread.h>

#include "hnae3.h"
#include "hclge_main.h"
#include "hns3_enet.h"
#include "hclge_tm.h"
#include "hclge_cmd.h"
#include "hns3_priv_dcb.h"

struct nictool_dcb_info dcb_all_info[20];
u8 curr_dev_index;
u8 max_index;

static void check_and_set_curr_dev(struct hns3_nic_priv *net_priv)
{
	int flag = false;
	int i;

	for (i = 0; i < max_index; i++) {
		if (dcb_all_info[i].net_priv != net_priv)
			continue;
		flag = true;
		curr_dev_index = i;
	}

	if (!flag) {
		max_index++;
		curr_dev_index = max_index - 1;
		dcb_all_info[curr_dev_index].net_priv = net_priv;
	}
}

int hns3_test_dcb_cfg(struct hns3_nic_priv *net_priv,
		      void *buf_in, u32 in_size, void *buf_out, u32 out_size)
{
	struct nictool_dcb_cfg_param *out_info;
	struct nictool_dcb_cfg_param *in_info;
	bool check;

	check = !buf_in || in_size < sizeof(struct nictool_dcb_cfg_param);
	if (check) {
		pr_err("input param buf_in error in %s function\n", __func__);
		return -EFAULT;
	}

	in_info = (struct nictool_dcb_cfg_param *)buf_in;
	out_info = (struct nictool_dcb_cfg_param *)buf_out;
	check_and_set_curr_dev(net_priv);
	if (in_info->is_read) {
		check = !buf_out ||
			out_size < sizeof(struct nictool_dcb_cfg_param);
		if (check) {
			pr_err("input param buf_out error in %s function\n",
			       __func__);
			return -EFAULT;
		}
		out_info->dcb_en =
		    dcb_all_info[curr_dev_index].dcb_cfg_info.dcb_en;
	} else {
		if (in_info->cfg_flag & NICTOOL_DCB_DCB_CFG_FLAG)
			dcb_all_info[curr_dev_index].dcb_cfg_info.dcb_en =
			    in_info->dcb_en;
	}

	return 0;
}

static int hns3_test_cfg_pfc_en(u8 is_read, struct hclge_dev *hdev,
				struct nictool_pfc_cfg_param *info)
{
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, NICTOOL_OPC_CFG_PFC_PAUSE_EN, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		pr_err("read pfc enable status fail!ret = %d\n", ret);
		return ret;
	}
	if (is_read) {
		info->prien = ((desc.data[0] & 0xff00) >> 8);
		info->pfc_en = ((desc.data[0] & 0x3) == 0x3);
	} else {
		hclge_cmd_reuse_desc(&desc, false);
		if (info->cfg_flag & NICTOOL_PFC_EN_CFG_FLAG) {
			desc.data[0] = (desc.data[0] & (~0x3)) |
				       (info->pfc_en << 0) |
				       (info->pfc_en << 1);
			dcb_all_info[curr_dev_index].pfc_cfg_info.pfc_en =
			    info->pfc_en;
		}
		if (info->cfg_flag & NICTOOL_PFC_PRIEN_CFG_FLAG) {
			desc.data[0] = (desc.data[0] & (~0xff00)) |
				       (info->prien << 8);
			dcb_all_info[curr_dev_index].pfc_cfg_info.prien =
			    info->prien;
		}
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			pr_err("set pfc cmd return fail!ret = %d\n", ret);
			return ret;
		}
	}

	return ret;
}

static int hns3_test_cfg_pause_param(struct hclge_dev *hdev,
				     struct nictool_pfc_cfg_param *info,
				     u8 is_read)
{
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, NICTOOL_OPC_CFG_PAUSE_PARAM, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		pr_err("pause param cfg cmd send fail\n");
		return ret;
	}

	if (is_read) {
		info->pause_time = desc.data[2] & 0xffff;
		info->pause_gap = (desc.data[1] & 0xff0000) >> 16;
		return 0;
	}

	if (info->cfg_flag & NICTOOL_PFC_TIME_CFG_FLAG)
		desc.data[2] = (desc.data[2] & (~0xffff)) | info->pause_time;

	if (info->cfg_flag & NICTOOL_PFC_GAP_CFG_FLAG)
		desc.data[1] = (desc.data[1] & (~0xff0000)) |
			       (info->pause_gap << 16);

	hclge_cmd_reuse_desc(&desc, false);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"mac pause param cfg fail, ret = %d.\n", ret);
		return ret;
	}
	return 0;
}

int hns3_test_dcb_pfc_cfg(struct hns3_nic_priv *net_priv,
			  void *buf_in, u32 in_size,
			  void *buf_out, u32 out_size)
{
	struct nictool_pfc_cfg_param *out_info;
	struct nictool_pfc_cfg_param *in_info;
	struct net_device *ndev;
	struct hclge_vport *vport;
	struct hnae3_handle *h;
	struct hclge_dev *hdev;
	bool check;
	int ret;

	check = !buf_in || in_size < sizeof(struct nictool_pfc_cfg_param);
	if (check) {
		pr_err("input param buf_in error in %s function\n", __func__);
		return -EFAULT;
	}

	check_and_set_curr_dev(net_priv);
	h = net_priv->ae_handle;
	vport = hclge_get_vport(h);
	ndev = h->netdev;
	hdev = vport->back;
	in_info = (struct nictool_pfc_cfg_param *)buf_in;
	out_info = (struct nictool_pfc_cfg_param *)buf_out;

	if (!in_info->is_read &&
	    !dcb_all_info[curr_dev_index].dcb_cfg_info.dcb_en) {
		pr_err("please enable dcb cfg first!\n");
		return -1;
	}

	if (!hnae3_dev_dcb_supported(hdev) || vport->vport_id != 0) {
		pr_err("this device doesn't support dcb!\n");
		return -1;
	}

	if (in_info->is_read) {
		check = !buf_out ||
			out_size < sizeof(struct nictool_pfc_cfg_param);
		if (check) {
			pr_err("input param buf_out error in %s function\n",
			       __func__);
			return -EFAULT;
		}
		ret = hns3_test_cfg_pfc_en(in_info->is_read, hdev, out_info);
		if (ret)
			return ret;
		ret = hns3_test_cfg_pause_param(hdev, out_info, true);
		if (ret)
			return ret;
	} else {
		struct ieee_pfc pfc = {0};

		if (in_info->cfg_flag & NICTOOL_PFC_PRIEN_CFG_FLAG) {
			pfc.pfc_en = in_info->prien;
			dcb_all_info[curr_dev_index].pfc_cfg_info.prien =
			    in_info->prien;
			if (ndev->dcbnl_ops->ieee_setpfc) {
				rtnl_lock();
				ret = ndev->dcbnl_ops->ieee_setpfc(ndev, &pfc);
				rtnl_unlock();
				if (ret)
					return ret;
			}
		}

		if ((in_info->cfg_flag & NICTOOL_PFC_TIME_CFG_FLAG) ||
		    (in_info->cfg_flag & NICTOOL_PFC_GAP_CFG_FLAG)) {
			ret = hns3_test_cfg_pause_param(hdev, in_info, false);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void hns3_test_disable_ets_cfg(struct hclge_dev *hdev,
				      struct ieee_ets *ets)
{
	u8 percent = 0;
	int i;

	for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++) {
		ets->prio_tc[i] = hdev->tm_info.prio_tc[i];
		ets->tc_tsa[i] = IEEE_8021QAZ_TSA_ETS;
		dcb_all_info[curr_dev_index].ets_cfg_info.schedule[i] = 0;
	}
	for (i = 0; i < hdev->tm_info.num_tc; i++) {
		ets->tc_tx_bw[i] = 100 / hdev->tm_info.num_tc;
		dcb_all_info[curr_dev_index].ets_cfg_info.bw[i] =
		    ets->tc_tx_bw[i];
		percent += ets->tc_tx_bw[i];
	}
	if (percent != 100) {
		ets->tc_tx_bw[i - 1] += (100 - percent);
		dcb_all_info[curr_dev_index].ets_cfg_info.bw[i - 1] =
		    ets->tc_tx_bw[i - 1];
	}
}

static void hns3_test_enable_ets_cfg(struct hclge_dev *hdev,
				     struct ieee_ets *ets,
				     struct nictool_ets_cfg_param *info)
{
	int i;

	if (info->cfg_flag & NICTOOL_ETS_UP2TC_CFG_FLAG) {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++) {
			ets->prio_tc[i] = info->up2tc[i];
			dcb_all_info[curr_dev_index].ets_cfg_info.up2tc[i] =
			    info->up2tc[i];
		}
	} else {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++)
			ets->prio_tc[i] = hdev->tm_info.prio_tc[i];
	}

	if (info->cfg_flag & NICTOOL_ETS_BANDWIDTH_CFG_FLAG) {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++) {
			ets->tc_tx_bw[i] = info->bw[i];
			dcb_all_info[curr_dev_index].ets_cfg_info.bw[i] =
			    info->bw[i];
		}
	} else {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++)
			ets->tc_tx_bw[i] = hdev->tm_info.pg_info[0].tc_dwrr[i];
	}

	if (info->cfg_flag & NICTOOL_ETS_SCHEDULE_CFG_FLAG) {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++) {
			ets->tc_tsa[i] = info->schedule[i] ?
			    IEEE_8021QAZ_TSA_STRICT : IEEE_8021QAZ_TSA_ETS;
			dcb_all_info[curr_dev_index].ets_cfg_info.schedule[i] =
				info->schedule[i];
		}
	} else {
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++)
			ets->tc_tsa[i] = hdev->tm_info.tc_info[i].tc_sch_mode ?
			    IEEE_8021QAZ_TSA_ETS : IEEE_8021QAZ_TSA_STRICT;
	}
}

int hns3_test_dcb_ets_cfg(struct hns3_nic_priv *net_priv,
			  void *buf_in, u32 in_size,
			  void *buf_out, u32 out_size)
{
	struct nictool_ets_cfg_param *out_info;
	struct nictool_ets_cfg_param *in_info;
	struct hclge_vport *vport;
	struct net_device *ndev;
	struct hclge_dev *hdev;
	struct hclge_desc desc;
	struct hnae3_handle *h;
	bool check;
	int ret;
	int i;

	check = !buf_in || in_size < sizeof(struct nictool_ets_cfg_param) ||
		!buf_out || out_size < sizeof(struct nictool_ets_cfg_param);
	if (check) {
		pr_err("input parameter error in %s function\n", __func__);
		return -EFAULT;
	}

	check_and_set_curr_dev(net_priv);
	h = net_priv->ae_handle;
	vport = hclge_get_vport(h);
	ndev = h->netdev;
	hdev = vport->back;
	in_info = (struct nictool_ets_cfg_param *)buf_in;
	out_info = (struct nictool_ets_cfg_param *)buf_out;

	if (!in_info->is_read &&
	    !dcb_all_info[curr_dev_index].dcb_cfg_info.dcb_en) {
		pr_err("please enable dcb cfg first!\n");
		return -1;
	}

	if (!hnae3_dev_dcb_supported(hdev) || vport->vport_id != 0) {
		pr_err("this device doesn't support dcb!\n");
		return -1;
	}

	if (in_info->is_read) {
		hclge_cmd_setup_basic_desc(&desc,
					   NICTOOL_OPC_PRI_TO_TC_MAPPING, true);
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			pr_err("read up2tc mapping fail!\n");
			return ret;
		}
		out_info->ets_en =
		    dcb_all_info[curr_dev_index].ets_cfg_info.ets_en;
		for (i = 0; i < NICTOOL_ETS_MAC_TC_NUM; i++) {
			out_info->up2tc[i] =
			    (desc.data[0] & (0xf << (4 * i))) >> (4 * i);
			dcb_all_info[curr_dev_index].ets_cfg_info.up2tc[i] =
			    out_info->up2tc[i];
			out_info->bw[i] = hdev->tm_info.pg_info[0].tc_dwrr[i];
			dcb_all_info[curr_dev_index].ets_cfg_info.bw[i] =
			    hdev->tm_info.pg_info[0].tc_dwrr[i];
			out_info->schedule[i] =
			    !hdev->tm_info.tc_info[i].tc_sch_mode;
			dcb_all_info[curr_dev_index].ets_cfg_info.schedule[i] =
			    !hdev->tm_info.tc_info[i].tc_sch_mode;
		}
	} else {
		struct ieee_ets ets = {0};

		if (in_info->cfg_flag & NICTOOL_ETS_EN_CFG_FLAG)
			dcb_all_info[curr_dev_index].ets_cfg_info.ets_en =
			    in_info->ets_en;

		if (!dcb_all_info[curr_dev_index].ets_cfg_info.ets_en)
			hns3_test_disable_ets_cfg(hdev, &ets);
		else
			hns3_test_enable_ets_cfg(hdev, &ets, in_info);

		if (ndev->dcbnl_ops->ieee_setets) {
			rtnl_lock();
			ret = ndev->dcbnl_ops->ieee_setets(ndev, &ets);
			rtnl_unlock();
			if (ret)
				return ret;
		}

		out_info->cfg_flag = in_info->cfg_flag;
		out_info->is_read = in_info->is_read;
		out_info->ets_en =
		    dcb_all_info[curr_dev_index].ets_cfg_info.ets_en;
	}

	return 0;
}
