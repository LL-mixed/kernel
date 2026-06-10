/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UB-Attached SSD UAPI.
 */

#ifndef _UAPI_UB_SSD_H
#define _UAPI_UB_SSD_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include "gsva.h"

/* SSD opcodes */
#define SSD_OP_BLOCK_WRITE	1
#define SSD_OP_BLOCK_READ	2
#define SSD_OP_BLOCK_SEAL	3
#define SSD_OP_BLOCK_TOMBSTONE	4
#define SSD_OP_FLUSH		5
#define SSD_OP_STAT		6

/* Completion status codes */
#define SSD_OK			0
#define SSD_ERR_BAD_VERSION	(-1)
#define SSD_ERR_BAD_OPCODE	(-2)
#define SSD_ERR_BAD_BLOCK	(-3)
#define SSD_ERR_BAD_DESCRIPTOR	(-4)
#define SSD_ERR_TOKEN_DENIED	(-5)
#define SSD_ERR_STALE_EPOCH	(-6)
#define SSD_ERR_SEGMENT_RETIRED	(-7)
#define SSD_ERR_COH_TIMEOUT	(-8)
#define SSD_ERR_DEVICE_BUSY	(-9)
#define SSD_ERR_CHECKSUM	(-10)
#define SSD_ERR_VERSION_CONFLICT (-11)
#define SSD_ERR_SEALED		(-12)
#define SSD_ERR_TOMBSTONED	(-13)
#define SSD_ERR_BACKEND_IO	(-14)

/* SSD block ref */
struct ub_ssd_block_ref_v1 {
	__u64	block_hi;
	__u64	block_lo;
	__u64	version;
	__u64	offset;
	__u64	bytes;
	__u64	checksum64;
};

/* SSD buffer descriptor */
struct ub_ssd_buffer_desc_v1 {
	__u64	gsva_base;
	__u64	bytes;
	struct gsva_key_v1 key;
	__u32	token_id;
	__u32	token_value;
};

/* SSD command */
struct ub_ssd_cmd_v1 {
	__u32	version;
	__u32	opcode;
	__u64	req_id;
	__u32	source_cna;
	__u32	target_ssd_cna;
	__u32	flags;
	struct ub_ssd_block_ref_v1 block_ref;
	struct ub_ssd_buffer_desc_v1 buffer;
};

/* SSD completion */
struct ub_ssd_cpl_v1 {
	__u32	version;
	__u32	status;
	__u64	req_id;
	struct ub_ssd_block_ref_v1 committed_ref;
	__u64	bytes_read;
	__u64	bytes_written;
	__u64	checksum64;
	__u64	error_detail;
};

/* SSD MMIO register offsets */
#define SSD_MMIO_SIZE		0x1000
#define SSD_CMD_SLOT_OFF	0x000
#define SSD_CPL_SLOT_OFF	0x400
#define SSD_CNA_OFF		0x500
#define SSD_STATUS_OFF		0x508
#define SSD_ERROR_OFF		0x50c
#define SSD_DOORBELL_OFF	0x510
#define SSD_CLEAR_CPL_OFF	0x514
#define SSD_LAST_REQ_ID_OFF	0x518
#define SSD_STATS_OFF		0x520

/* Status register bits */
#define SSD_STATUS_READY		(1u << 0)
#define SSD_STATUS_BUSY			(1u << 1)
#define SSD_STATUS_COMPLETION_VALID	(1u << 2)
#define SSD_STATUS_ERROR		(1u << 3)

/* ioctl numbers */
#define UB_SSD_IOC_MAGIC	'S'
#define UB_SSD_SUBMIT		_IOW(UB_SSD_IOC_MAGIC, 1, struct ub_ssd_cmd_v1)
#define UB_SSD_WAIT		_IOR(UB_SSD_IOC_MAGIC, 2, struct ub_ssd_cpl_v1)
#define UB_SSD_QUERY		_IOR(UB_SSD_IOC_MAGIC, 3, __u64[16])

#endif /* _UAPI_UB_SSD_H */
