/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2026. All rights reserved.
 */
#ifndef OBMM_SIM_DECODER_H
#define OBMM_SIM_DECODER_H

#include <linux/types.h>

#define OBMM_SIM_DEC_PRIV_MAGIC 0x53444950U /* "SDIP" */
#define OBMM_SIM_DEC_PRIV_VER_1 1

struct obmm_sim_dec_import_priv_v1 {
	u32 magic;
	u16 version;
	u16 len;
	u64 remote_uba;
	u32 token_value;
	u32 flags;
};

struct obmm_sim_dec_import_info {
	u64 local_pa;
	u64 size;
	u64 remote_uba;
	u32 token_id;
	u32 token_value;
	u32 scna;
	u32 dcna;
	u8 seid[16];
	u8 deid[16];
	u32 upi;
	u32 src_eid;
	u64 map_id;
};

struct obmm_sim_dec_unimport_info {
	u64 map_id;
	u32 scna;
};

int obmm_register_import_callback(int (*import_fn)(void *));
int obmm_unregister_import_callback(void);
int obmm_register_unimport_callback(int (*unimport_fn)(void *));
int obmm_unregister_unimport_callback(void);

#endif /* OBMM_SIM_DECODER_H */
