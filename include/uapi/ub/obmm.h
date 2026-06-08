/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 */

#ifndef UAPI_OBMM_H
#define UAPI_OBMM_H

#include <linux/types.h>

#if defined(__cplusplus)
extern "C" {
#endif


#define OBMM_MAX_LOCAL_NUMA_NODES   16
#define MAX_NUMA_DIST               255
#define OBMM_MAX_PRIV_LEN           512
#define OBMM_MAX_VENDOR_LEN         128


#define OBMM_EXPORT_FLAG_ALLOW_MMAP 0x1UL
#define OBMM_EXPORT_FLAG_FAST       0x2UL
#define OBMM_EXPORT_FLAG_GSVA_FIXED_UBA 0x4UL
#define OBMM_EXPORT_FLAG_MASK       (OBMM_EXPORT_FLAG_ALLOW_MMAP | \
					 OBMM_EXPORT_FLAG_FAST | \
					 OBMM_EXPORT_FLAG_GSVA_FIXED_UBA)

struct obmm_cmd_export_pid {
	void *va;
	__u64 length;
	__u64 flags;
	__u64 uba;
	__u64 mem_id;
	__u32 tokenid;
	__s32 pid;
	__s32 pxm_numa;
	__u16 priv_len;
	__u16 vendor_len;
	__u8 deid[16];
	__u8 seid[16];
	const void *priv;
	const void *vendor_info;
} __attribute__((aligned(8)));

/* For ordinary register requests, @length and @flags are input arguments while
 * @tokenid, @uba and @mem_id are values set by obmm kernel module. For
 * register request, @length, @flags, @tokenid and @uba are input to obmm
 * kernel module. @mem_id is the only output.
 */
struct obmm_cmd_export {
	__u64 size[OBMM_MAX_LOCAL_NUMA_NODES];
	__u64 length;
	__u64 flags;
	__u64 uba;
	__u64 mem_id;
	__u32 tokenid;
	__s32 pxm_numa;
	__u16 priv_len;
	__u16 vendor_len;
	__u8 deid[16];
	__u8 seid[16];
	const void *vendor_info;
	const void *priv;
} __attribute__((aligned(8)));

#define OBMM_UNEXPORT_FLAG_MASK	(0UL)

struct obmm_cmd_unexport {
	__u64 mem_id;
	__u64 flags;
} __attribute__((aligned(8)));

enum obmm_query_key_type {
	OBMM_QUERY_BY_PA,
	OBMM_QUERY_BY_ID_OFFSET
};

struct obmm_cmd_addr_query {
	/* key type decides the input and output */
	enum obmm_query_key_type key_type;
	__u64 mem_id;
	__u64 offset;
	__u64 pa;
} __attribute__((aligned(8)));

#define OBMM_IMPORT_FLAG_ALLOW_MMAP	0x1UL
#define OBMM_IMPORT_FLAG_PREIMPORT	0x2UL
#define OBMM_IMPORT_FLAG_NUMA_REMOTE	0x4UL
#define OBMM_IMPORT_FLAG_MASK		(OBMM_IMPORT_FLAG_ALLOW_MMAP | \
					 OBMM_IMPORT_FLAG_PREIMPORT |  \
					 OBMM_IMPORT_FLAG_NUMA_REMOTE)


struct obmm_cmd_import {
	__u64 flags;
	__u64 mem_id;
	__u64 addr;
	__u64 length;
	__u32 tokenid;
	__u32 scna;
	__u32 dcna;
	__s32 numa_id;
	__u16 priv_len;
	__u8 base_dist;
	__u8 deid[16];
	__u8 seid[16];
	const void *priv;
} __attribute__((aligned(8)));

#define OBMM_UNIMPORT_FLAG_MASK	(0UL)

struct obmm_cmd_unimport {
	__u64 mem_id;
	__u64 flags;
} __attribute__((aligned(8)));

#define OBMM_BOOTSTRAP_MAX_NODES 8

struct obmm_bootstrap_record {
	__u64 export_mem_id;
	__u64 remote_uba;
	__u64 size;
	__u64 generation;
	__u64 flags;
	__u32 node_id;
	__u32 node_count;
	__u32 export_cna;
	__u32 token_id;
} __attribute__((aligned(8)));

struct obmm_cmd_bootstrap_publish {
	struct obmm_bootstrap_record record;
} __attribute__((aligned(8)));

struct obmm_cmd_bootstrap_lookup {
	__u64 generation;
	__u32 node_count;
	__u32 local_cna;
	__u32 count;
	__u32 rsvd;
	struct obmm_bootstrap_record records[OBMM_BOOTSTRAP_MAX_NODES];
} __attribute__((aligned(8)));

struct obmm_cmd_gsva_aperture {
	__u64 base;
	__u64 size;
	__u64 generation;
	__u64 flags;
	__u32 node_id;
	__u32 node_count;
	__u32 rsvd[4];
} __attribute__((aligned(8)));

#define OBMM_GSVA_APERTURE_F_ACTIVE	0x1UL

struct obmm_cmd_gsva_query_v1 {
	__u32 version;
	__u32 query_type;
	__u64 segment_id;
	__u64 home_va;
	__u8 resp_data[248];
} __attribute__((aligned(8)));

#define OBMM_CMD_EXPORT      _IOWR('x', 0, struct obmm_cmd_export)
#define OBMM_CMD_IMPORT      _IOWR('x', 1, struct obmm_cmd_import)
#define OBMM_CMD_UNEXPORT    _IOW('x', 2, struct obmm_cmd_unexport)
#define OBMM_CMD_UNIMPORT    _IOW('x', 3, struct obmm_cmd_unimport)
#define OBMM_CMD_ADDR_QUERY  _IOWR('x', 4, struct obmm_cmd_addr_query)
#define OBMM_CMD_EXPORT_PID  _IOWR('x', 5, struct obmm_cmd_export_pid)
#define OBMM_CMD_DECLARE_PREIMPORT   _IOWR('x', 6, struct obmm_cmd_preimport)
#define OBMM_CMD_UNDECLARE_PREIMPORT _IOW('x', 7, struct obmm_cmd_preimport)
#define OBMM_CMD_BOOTSTRAP_PUBLISH _IOW('x', 8, struct obmm_cmd_bootstrap_publish)
#define OBMM_CMD_BOOTSTRAP_LOOKUP _IOWR('x', 9, struct obmm_cmd_bootstrap_lookup)
#define OBMM_CMD_GSVA_APERTURE_REGISTER _IOW('x', 10, struct obmm_cmd_gsva_aperture)
#define OBMM_CMD_GSVA_APERTURE_QUERY _IOWR('x', 11, struct obmm_cmd_gsva_aperture)
#define OBMM_CMD_GSVA_APERTURE_CLEAR _IOW('x', 12, struct obmm_cmd_gsva_aperture)
#define OBMM_CMD_GSVA_QUERY_V1 _IOWR('x', 13, struct obmm_cmd_gsva_query_v1)
#define OBMM_CMD_GSVA_ALLOC_SEGMENT _IOWR('x', 14, struct obmm_cmd_gsva_alloc_segment_v1)
#define OBMM_CMD_GSVA_QUERY_SEGMENT _IOWR('x', 15, struct obmm_cmd_gsva_query_segment_v1)
#define OBMM_CMD_GSVA_RETIRE_SEGMENT _IOWR('x', 16, struct obmm_cmd_gsva_retire_segment_v1)
#define OBMM_CMD_GSVA_EVENT_V1 _IOWR('x', 17, struct obmm_cmd_gsva_event_v1)

/* 2bits */
#define OBMM_SHM_MEM_CACHE_RESV     0x0
#define OBMM_SHM_MEM_NORMAL         0x1
#define OBMM_SHM_MEM_NORMAL_NC      0x2
#define OBMM_SHM_MEM_DEVICE         0x3
#define OBMM_SHM_MEM_CACHE_MASK     0b11
/* 2bits */
#define OBMM_SHM_MEM_READONLY       0x0
#define OBMM_SHM_MEM_READEXEC       0x4
#define OBMM_SHM_MEM_READWRITE      0x8
#define OBMM_SHM_MEM_NO_ACCESS      0xc
#define OBMM_SHM_MEM_ACCESS_MASK    0b1100

/* cache maintenance operations (not states) */
/* no cache maintenance (nops) */
#define OBMM_SHM_CACHE_NONE             0x0
/* invalidate only (in-cache modifications may not be written back to DRAM) */
#define OBMM_SHM_CACHE_INVAL            0x1
/* write back and invalidate */
#define OBMM_SHM_CACHE_WB_INVAL         0x2
/* write back only */
#define OBMM_SHM_CACHE_WB_ONLY         0x3
/* Automatically choose the cache maintenance action depending on the memory
 * state. The resulting choice always make sure no data would be lost, and might
 * be more conservative than necessary.
 */
#define OBMM_SHM_CACHE_INFER            0x4

struct obmm_cmd_update_range {
	/* address range to manipulate: [start, end) */
	__u64 start;
	__u64 end;
	__u8  mem_state;
	__u8  cache_ops;
} __attribute__((aligned(8)));

#define OBMM_SHMDEV_UPDATE_RANGE	_IOW('X', 0, struct obmm_cmd_update_range)

struct obmm_cmd_sync_import_range {
	/* byte range within imported shmdev mapping: [offset, offset + length) */
	__u64 offset;
	__u64 length;
} __attribute__((aligned(8)));
typedef struct obmm_cmd_sync_import_range obmm_cmd_sync_remote_range;

#define OBMM_SHMDEV_SYNC_IMPORT_RANGE _IOW('X', 1, struct obmm_cmd_sync_import_range)
#define OBMM_SHMDEV_SYNC_REMOTE_RANGE OBMM_SHMDEV_SYNC_IMPORT_RANGE

struct obmm_cmd_preimport {
	__u64 pa;
	__u64 length;
	__u64 flags;
	__u32 scna;
	__u32 dcna;
	__s32 numa_id;
	__u16 priv_len;
	__u8 base_dist;
	__u8 deid[16];
	__u8 seid[16];
	const void *priv;
} __attribute__((aligned(16), packed));

#define OBMM_PREIMPORT_FLAG_MASK	(0UL)
#define OBMM_UNPREIMPORT_FLAG_MASK	(0UL)

#define OBMM_MMAP_FLAG_HUGETLB_PMD (1UL << 63)
#define OBMM_MMAP_FLAG_GSVA        (1UL << 62)

#if defined(__cplusplus)
}
#endif

#endif /* UAPI_OBMM_H */
