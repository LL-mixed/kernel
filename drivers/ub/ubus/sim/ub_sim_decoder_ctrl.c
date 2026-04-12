/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Control Adapter
 * Bridges service layer to backend through hisi private message channel.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "../msg.h"
#include "../ubus_controller.h"
#include "../vendor/hisilicon/hisi-msg.h"
#include "ub_sim_decoder.h"

static int sim_dec_status_to_errno(u16 status)
{
	switch (status) {
	case SIM_DEC_STATUS_SUCCESS:
		return 0;
	case SIM_DEC_STATUS_INVALID_PARAM:
		return -EINVAL;
	case SIM_DEC_STATUS_RESOURCE_BUSY:
		return -EBUSY;
	case SIM_DEC_STATUS_BACKEND_ERROR:
		return -EIO;
	case SIM_DEC_STATUS_TIMEOUT:
		return -ETIMEDOUT;
	case SIM_DEC_STATUS_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

int ub_sim_dec_ctrl_adapter_init(struct ub_sim_dec_ctrl_adapter *adapter,
				 struct ub_sim_decoder_service *svc)
{
	if (!adapter || !svc)
		return -EINVAL;

	memset(adapter, 0, sizeof(*adapter));
	adapter->service = svc;
	mutex_init(&adapter->lock);
	adapter->seq = 1;
	adapter->connected = true;

	pr_info("UB SIM Decoder Ctrl: initialized\n");
	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_dec_ctrl_adapter_init);

void ub_sim_dec_ctrl_adapter_exit(struct ub_sim_dec_ctrl_adapter *adapter)
{
	if (!adapter)
		return;

	adapter->connected = false;
	pr_info("UB SIM Decoder Ctrl: exited\n");
}
EXPORT_SYMBOL_GPL(ub_sim_dec_ctrl_adapter_exit);

static int send_control_message(u32 scna, struct sim_dec_msg_hdr *hdr,
				void *payload,
				void *resp, u16 resp_len)
{
	struct ub_bus_controller *ubc;
	struct msg_info info = { 0 };
	struct sim_dec_msg_hdr *rsp_hdr;
	u8 *req_buf;
	u8 *rsp_buf;
	u16 req_total_len;
	u16 rsp_total_len;
	u16 copy_len;
	int ret;

	if (hdr->payload_len && !payload)
		return -EINVAL;

	ubc = ub_find_bus_controller_by_cna(scna);
	if (!ubc || !ubc->mdev || !ubc->uent) {
		pr_err("UB SIM Decoder Ctrl: scna=%#x not available\n", scna);
		return -ENODEV;
	}

	req_total_len = sizeof(*hdr) + hdr->payload_len;
	rsp_total_len = sizeof(*hdr) + resp_len;
	if (rsp_total_len < sizeof(*hdr))
		rsp_total_len = sizeof(*hdr);

	req_buf = kzalloc(req_total_len, GFP_KERNEL);
	rsp_buf = kzalloc(rsp_total_len, GFP_KERNEL);
	if (!req_buf || !rsp_buf) {
		ret = -ENOMEM;
		goto out_free;
	}

	memcpy(req_buf, hdr, sizeof(*hdr));
	if (hdr->payload_len)
		memcpy(req_buf + sizeof(*hdr), payload, hdr->payload_len);

	message_info_init(&info, ubc->uent, req_buf, rsp_buf,
			  (req_total_len << MSG_REQ_SIZE_OFFSET) |
			  rsp_total_len);
	ret = hi_message_private(ubc->mdev, &info, SIM_DEC_CTRL_PRIVATE_CMD);
	if (ret)
		goto out_free;

	if (info.actual_rsp_size < sizeof(*hdr)) {
		ret = -EIO;
		goto out_free;
	}

	rsp_hdr = (struct sim_dec_msg_hdr *)rsp_buf;
	if (rsp_hdr->version != SIM_DEC_PROTO_VERSION ||
	    rsp_hdr->opcode != hdr->opcode || rsp_hdr->seq != hdr->seq) {
		pr_err("UB SIM Decoder Ctrl: bad rsp v=%u op=%u seq=%u\n",
		       rsp_hdr->version, rsp_hdr->opcode, rsp_hdr->seq);
		ret = -EPROTO;
		goto out_free;
	}

	ret = sim_dec_status_to_errno(rsp_hdr->status);
	if (ret)
		goto out_free;

	if (resp && resp_len) {
		copy_len = min_t(u16, resp_len, rsp_hdr->payload_len);
		if (!copy_len) {
			ret = -EIO;
			goto out_free;
		}
		memcpy(resp, rsp_buf + sizeof(*rsp_hdr), copy_len);
	}

out_free:
	kfree(req_buf);
	kfree(rsp_buf);
	return ret;
}

int ub_sim_dec_send_cmd(struct ub_sim_dec_ctrl_adapter *adapter,
			u32 scna, u8 opcode, void *req, u16 req_len,
			void *resp, u16 resp_len)
{
	struct sim_dec_msg_hdr hdr;
	int ret;

	if (!adapter || !adapter->connected)
		return -EINVAL;

	mutex_lock(&adapter->lock);

	hdr.version = SIM_DEC_PROTO_VERSION;
	hdr.opcode = opcode;
	hdr.seq = adapter->seq++;
	hdr.status = 0;
	hdr.payload_len = req_len;

	ret = send_control_message(scna, &hdr, req, resp, resp_len);

	mutex_unlock(&adapter->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ub_sim_dec_send_cmd);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UB Simulation Decoder Control Adapter");
