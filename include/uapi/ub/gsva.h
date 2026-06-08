/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * GSVA UAPI -- stable ABI for GSVA segment lifecycle and queries.
 *
 * This header is the source of truth for GSVA ABI definitions.
 * Internal and app headers mirror only the constants they need.
 */

#ifndef _UAPI_UB_GSVA_H
#define _UAPI_UB_GSVA_H

#include <linux/types.h>

/* ABI version */
#define OBMM_GSVA_ABI_VERSION		1

/* GSVA opcode values (SIM_DEC protocol) */
#define SIM_DEC_OP_GSVA_MAP_V1		0x09
#define SIM_DEC_OP_GSVA_UNMAP_V1	0x0a
#define SIM_DEC_OP_GSVA_EVENT_V1	0x0b
#define SIM_DEC_OP_GSVA_QUERY_V1	0x0c

/* GSVA key version 1 */
struct gsva_key_v1 {
	__u32	version;	/* must be 1 */
	__u32	flags;
	__u64	segment_id;
	__u64	home_va;
	__u64	size;
	__u64	vmid;
	__u64	asid;
	__u64	pte_offset;
	__u32	p_tag;
	__u32	cache_policy;
	__u64	epoch;
};

/* GSVA source values */
#define GSVA_SOURCE_IMPORT_PA_WINDOW	1
#define GSVA_SOURCE_ARM_MMU		2
#define GSVA_SOURCE_QUERY_DRY_RUN	3

/* GSVA address profile values */
#define GSVA_ADDRESS_PROFILE_LEGACY_RELOCATABLE	0
#define GSVA_ADDRESS_PROFILE_STRICT_GSVA	1
#define GSVA_ADDRESS_PROFILE_COMPAT_GSVA	2

/* GSVA cache policy (mirrors existing OBMM values) */
#define GSVA_CACHE_POLICY_NC			0
#define GSVA_CACHE_POLICY_WRITE_THROUGH		1
#define GSVA_CACHE_POLICY_READ_CACHE		2
#define GSVA_CACHE_POLICY_WRITE_BACK		3
#define GSVA_CACHE_POLICY_DIRECTORY_MESI	4

/* GSVA query types */
#define GSVA_QUERY_CAPS		1
#define GSVA_QUERY_ROUTE	2
#define GSVA_QUERY_COHERENCE	3
#define GSVA_QUERY_SEGMENT	4

/* GSVA capability flags */
#define GSVA_CAP_STRICT_ADDRESS_IDENTITY	(1u << 0)
#define GSVA_CAP_ROUTE_LAYER			(1u << 1)
#define GSVA_CAP_COHERENCE_LAYER		(1u << 2)
#define GSVA_CAP_ARM_MMU_MODE			(1u << 3)
#define GSVA_CAP_RETIRE_REUSE_TXN		(1u << 4)

/* GSVA error codes */
#define GSVA_OK				0
#define GSVA_ERR_BAD_VERSION		(-1)
#define GSVA_ERR_KEY_MISMATCH		(-2)
#define GSVA_ERR_STALE_EPOCH		(-3)
#define GSVA_ERR_TOKEN_DENIED		(-4)
#define GSVA_ERR_ROUTE_MISSING		(-5)
#define GSVA_ERR_COH_PENDING		(-6)
#define GSVA_ERR_COH_TIMEOUT		(-7)
#define GSVA_ERR_TLB_STALE		(-8)
#define GSVA_ERR_SEGMENT_RETIRED	(-9)
#define GSVA_ERR_UNSUPPORTED_POLICY	(-10)
#define GSVA_ERR_STRICT_ADDRESS		(-11)
#define GSVA_ERR_FEATURE_MISSING	(-12)

/* GSVA event types */
#define GSVA_EVENT_MAP			1
#define GSVA_EVENT_MAP_UPDATE		2
#define GSVA_EVENT_UNMAP		3
#define GSVA_EVENT_SEGMENT_RETIRE	4
#define GSVA_EVENT_SEGMENT_REUSE	5
#define GSVA_EVENT_TOKEN_CHANGE		6
#define GSVA_EVENT_CACHE_POLICY_CHANGE	7
#define GSVA_EVENT_TLB_FLUSH		8

/* GSVA coherence event sub-ops for OBMM_CMD_GSVA_EVENT_V1 */
#define OBMM_GSVA_EVENT_READ_ACQUIRE	1
#define OBMM_GSVA_EVENT_WRITE_ACQUIRE	2
#define OBMM_GSVA_EVENT_RETIRE		3
#define OBMM_GSVA_EVENT_INV_ACK		4
#define OBMM_GSVA_EVENT_RETRY		5
#define OBMM_GSVA_EVENT_TOKEN_CHANGE	6
#define OBMM_GSVA_EVENT_FENCE		7

/* GSVA segment flags */
#define OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY	(1u << 0)
#define OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED	(1u << 1)
#define OBMM_GSVA_SEG_F_ACTIVE			(1u << 2)
#define OBMM_GSVA_SEG_F_RETIRED		(1u << 3)

/* GSVA access flags */
#define OBMM_GSVA_ACCESS_READ		(1u << 0)
#define OBMM_GSVA_ACCESS_WRITE		(1u << 1)

/* Auto-derive p_tag */
#define OBMM_GSVA_P_TAG_AUTO		0xffffffffu

/* GSVA segment descriptor */
struct obmm_gsva_segment_desc_v1 {
	__u32	version;
	__u32	flags;
	__u64	segment_id;
	__u64	home_va;
	__u64	size;
	__u64	epoch;
	__u32	home_cna;
	__u32	owner_node_id;
	__u32	node_count;
	__u32	cache_policy;
	__u32	p_tag;
	__u32	access_flags;
	__u32	token_id;
	__u32	token_value;
};

/* GSVA segment allocation command */
struct obmm_cmd_gsva_alloc_segment_v1 {
	__u32	version;
	__u32	flags;
	/* input */
	__u64	size;
	__u64	alignment;
	__u64	requested_home_va;
	__u32	home_node_id;
	__u32	cache_policy;
	__u32	requested_p_tag;
	__u32	access_flags;
	/* output */
	struct obmm_gsva_segment_desc_v1 desc;
};

/* GSVA segment query command */
struct obmm_cmd_gsva_query_segment_v1 {
	__u32	version;
	__u32	flags;
	/* input: either segment_id or home_va must be non-zero */
	__u64	segment_id;
	__u64	home_va;
	/* output */
	struct obmm_gsva_segment_desc_v1 desc;
};

/* GSVA segment retire command */
struct obmm_cmd_gsva_retire_segment_v1 {
	__u32	version;
	__u32	flags;
	/* input */
	__u64	segment_id;
	__u64	epoch;
	__u32	timeout_ms;
	__u32	reserved;
	/* output */
	__u64	committed_epoch;
	__u32	status;
	__u32	error;
};

/* GSVA coherence event command */
struct obmm_cmd_gsva_event_v1 {
	__u32	version;
	__u32	flags;
	/* input */
	__u32	sub_op;
	__u32	requester_cna;
	__u32	token_id;
	__u32	token_value;
	struct gsva_key_v1 key;
	/* output: GSVA_OK or GSVA_ERR_* */
	__s32	error;
	__u32	reserved;
};

/* Retire status values */
#define OBMM_GSVA_RETIRE_COMMITTED		1
#define OBMM_GSVA_RETIRE_ABORTED		2
#define OBMM_GSVA_RETIRE_PENDING_TIMEOUT	3

#endif /* _UAPI_UB_GSVA_H */
