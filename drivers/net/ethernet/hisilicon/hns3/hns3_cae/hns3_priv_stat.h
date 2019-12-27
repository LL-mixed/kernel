/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2016-2019 Hisilicon Limited. */

#ifndef __HNS3_PRIV_STAT_H__
#define __HNS3_PRIV_STAT_H__

#include "hclge_main.h"
#include "hclge_cmd.h"
#include "hns3_enet.h"

struct stat_sw_mode_param {
	u64 data;
	u32 ring_idx;
	u8 val_name[24];
	u8 is_read;
	u8 is_rx;
};

enum stats_name_type {
	IO_ERR_CNT = 1,
	SW_ERR_CNT,
	SEG_PKT_CNT,
	TX_PKTS,
	TX_BYTES,
	TX_ERR_CNT,
	RESTART_QUEUE,
	TX_BUSY,
	RX_PKTS,
	RX_BYTES,
	RX_ERR_CNT,
	REUSE_PG_CNT,
	ERR_PKT_LEN,
	ERR_BD_NUM,
	L2_ERR,
	L3L4_CSUM_ERR,
	RX_MULTICAST,
};

struct ring_stats_name {
	u8 stats_name[24];
	u32 stats_namd_id;
};

int hns3_stat_mode_cfg(struct hns3_nic_priv *nic_dev,
		       void *buf_in, u16 in_size,
		       void *buf_out, u16 *out_size);
#endif
