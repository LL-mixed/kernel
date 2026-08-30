/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2026. All rights reserved.
 */
#ifndef OBMM_SIM_DECODER_H
#define OBMM_SIM_DECODER_H

#include <linux/bits.h>
#include <linux/types.h>

#define OBMM_SIM_DEC_PRIV_MAGIC 0x53444950U /* "SDIP" */
#define OBMM_SIM_DEC_PRIV_VER_1 1
#define OBMM_SIM_DEC_PRIV_VER_2 2
#define OBMM_SIM_DEC_PRIV_VER_3 3

/* map_source values for GVA metadata */
#define OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM 1
#define OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER 2

/* address_profile values */
#define OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA 1
#define OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY 2

/* access/semantics profile */
#define OBMM_SIM_DEC_CACHE_POLICY_NC 0
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH 1
#define OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE 2
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK 3
#define OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI 4

#define OBMM_SIM_DEC_ACCESS_READ_ONLY BIT(0)
#define OBMM_SIM_DEC_ACCESS_EXPLICIT_SYNC BIT(1)
#define OBMM_SIM_DEC_ACCESS_FAULT_UPI_MISMATCH BIT(31)

struct obmm_sim_dec_import_priv_v2 {
	u32 magic;
	u16 version;
	u16 len;
	u64 remote_uba;
	u32 token_value;
	u32 flags;
	u32 map_source;
	u32 address_profile;
	u32 cache_policy;
	u32 vmid;
	u32 asid;
	u64 local_va;
	u64 home_va;
	u64 pte_offset;
	u32 tid;
	u32 p_tag;
	u32 access_flags;
	u64 gva_id;
	u64 segment_id;
	u64 epoch;
};

struct obmm_sim_dec_import_priv_v1 {
	u32 magic;
	u16 version;
	u16 len;
	u64 remote_uba;
	u32 token_value;
	u32 flags;
};

struct obmm_sim_dec_import_priv_v3 {
	u32 magic;
	u16 version;
	u16 len;
	u64 remote_uba;
	u32 token_value;
	u32 flags;
	u64 remote_export_mem_id;
	u64 remote_export_generation;
};

struct obmm_sim_dec_import_info {
	u64 local_pa;
	u64 size;
	u64 remote_uba;
	u64 remote_export_mem_id;
	u64 remote_export_generation;
	u32 token_id;
	u32 token_value;
	u32 scna;
	u32 dcna;
	u8 seid[16];
	u8 deid[16];
	u32 upi;
	u32 src_eid;
	u64 local_va;
	u64 home_va;
	u64 pte_offset;
	u32 vmid;
	u32 asid;
	u32 tid;
	u32 p_tag;
	u32 cache_policy;
	u32 map_source;
	u32 address_profile;
	u32 access_flags;
	u64 gva_id;
	u64 segment_id;
	u64 epoch;
	u64 map_id;
};

struct obmm_sim_dec_unimport_info {
	u64 map_id;
	u32 scna;
	bool is_gsva;
};

struct obmm_sim_dec_export_retire_info {
	u64 export_mem_id;
	u64 remote_uba;
	u64 size;
	u32 export_cna;
	u32 token_id;
};

int obmm_register_import_callback(int (*import_fn)(void *));
int obmm_unregister_import_callback(void);
int obmm_register_unimport_callback(int (*unimport_fn)(void *));
int obmm_unregister_unimport_callback(void);
int obmm_register_export_retire_callback(int (*retire_fn)(void *));
int obmm_unregister_export_retire_callback(void);
int obmm_sim_decoder_retire_export(
	const struct obmm_sim_dec_export_retire_info *info);

#endif /* OBMM_SIM_DECODER_H */
