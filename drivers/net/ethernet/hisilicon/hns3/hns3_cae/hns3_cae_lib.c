// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.


#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kthread.h>

#include "hnae3.h"
#include "hclge_cmd.h"
#include "hclge_main.h"
#include "hns3_enet.h"
#include "hns3_cae_cmd.h"
#include "hns3_cae_lib.h"

int hns3_cae_common_cmd_send(struct hns3_nic_priv *net_priv, void *buf_in,
			     u32 in_size, void *buf_out, u32 out_size)
{
#define MAX_DESC_DATA_LEN       6
	struct cmd_desc_param *param_in;
	struct hclge_vport *vport;
	struct hclge_dev *hdev;
	struct hclge_desc desc;
	bool check;
	int ret;
	int i;

	check = !buf_in || in_size < sizeof(struct cmd_desc_param);
	vport = hns3_cae_get_vport(net_priv->ae_handle);
	hdev = vport->back;

	param_in = (struct cmd_desc_param *)buf_in;
	hns3_cae_cmd_setup_basic_desc(&desc, param_in->fw_dw_opcode,
				      param_in->is_read);
	for (i = 0; i < MAX_DESC_DATA_LEN; i++)
		desc.data[i] = param_in->reg_desc.data[i];
	ret = hns3_cae_cmd_send(hdev, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "%s, ret is %d.\n", __func__,
			ret);
		return ret;
	}
	if (param_in->is_read) {
		struct cmd_desc_param *param_out =
					(struct cmd_desc_param *)buf_out;

		check = !buf_out || out_size < sizeof(struct cmd_desc_param);
		if (check) {
			pr_err("input param buf_out error in %s function\n",
			       __func__);
			return -EFAULT;
		}
		for (i = 0; i < MAX_DESC_DATA_LEN; i++)
			param_out->reg_desc.data[i] = desc.data[i];
	}

	return 0;
}

int hns3_m7_cmd_handle(struct hns3_nic_priv *nic_dev, void *buf_in, u32 in_size,
		       void *buf_out, u32 out_size)
{
	struct hclge_vport *vport = hns3_cae_get_vport(nic_dev->ae_handle);
	struct m7_cmd_para *cmd_para = (struct m7_cmd_para *)buf_in;
	struct hclge_dev *hdev = vport->back;
	struct hclge_desc *desc;
	u32 bd_size;
	bool check;
	int ret;

	check = !buf_in || in_size < sizeof(struct m7_cmd_para);
	if (check) {
		pr_err("input param buf_in error in %s function\n", __func__);
		return -EFAULT;
	}

	bd_size = sizeof(struct hclge_desc) * cmd_para->bd_count;
	desc = kzalloc(bd_size, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(desc)) {
		pr_err("desc kzalloc failed in m7_cmd_handle function\n");
		return -ENOMEM;
	}
	if (copy_from_user((void *)desc, cmd_para->bd_data, bd_size)) {
		pr_err("copy from user failed in m7_cmd_handle function\n");
		kfree(desc);
		return -EFAULT;
	}

	ret = hns3_cae_cmd_send(hdev, desc, cmd_para->bd_count);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"generic cmd send fail, ret is %d.\n", ret);
		kfree(desc);
		return ret;
	}

	if (desc->flag & HCLGE_CMD_FLAG_WR) {
		if (!buf_out || out_size < bd_size) {
			pr_err("input param buf_out error in %s function\n",
			       __func__);
			kfree(desc);
			return -EFAULT;
		}
		memcpy(buf_out, desc, bd_size);
	}

	kfree(desc);

	return 0;
}
