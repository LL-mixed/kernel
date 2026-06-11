/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UB-Attached NPU UAPI.
 */

#ifndef _UAPI_UB_NPU_H
#define _UAPI_UB_NPU_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include "gsva.h"

/* NPU opcodes */
#define NPU_OP_NOOP		0
#define NPU_OP_MEMCOPY		1
#define NPU_OP_FILL		2
#define NPU_OP_VECTOR_ADD_U32	3
#define NPU_OP_CHECKSUM64	4

/* Buffer descriptor roles */
#define NPU_BUF_INPUT		0
#define NPU_BUF_WEIGHT		1
#define NPU_BUF_OUTPUT		2
#define NPU_BUF_SCRATCH		3

/* Buffer access flags */
#define NPU_ACCESS_READ		1
#define NPU_ACCESS_WRITE	2
#define NPU_ACCESS_READ_WRITE	3

/* Command flags */
#define NPU_CMD_ALLOW_TRUNCATE	(1u << 0)

/* Completion status codes */
#define NPU_OK			0
#define NPU_ERR_BAD_VERSION	(-1)
#define NPU_ERR_BAD_OPCODE	(-2)
#define NPU_ERR_BAD_DESCRIPTOR	(-3)
#define NPU_ERR_TOKEN_DENIED	(-4)
#define NPU_ERR_STALE_EPOCH	(-5)
#define NPU_ERR_SEGMENT_RETIRED	(-6)
#define NPU_ERR_COH_TIMEOUT	(-7)
#define NPU_ERR_DEVICE_BUSY	(-8)

/* NPU buffer descriptor */
struct ub_npu_buffer_desc_v1 {
	__u32	role;
	__u32	access;
	__u64	gsva_base;
	__u64	bytes;
	struct gsva_key_v1 key;
	__u32	token_id;
	__u32	token_value;
};

#define NPU_MAX_DESCS	4

/* NPU command */
struct ub_npu_cmd_v1 {
	__u32	version;
	__u32	opcode;
	__u64	req_id;
	__u32	source_cna;
	__u32	target_npu_cna;
	__u32	flags;
	__u32	desc_count;
	struct ub_npu_buffer_desc_v1 descs[NPU_MAX_DESCS];
	__u64	scalar0;
	__u64	scalar1;
};

/* NPU completion */
struct ub_npu_cpl_v1 {
	__u32	version;
	__u32	status;
	__u64	req_id;
	__u64	bytes_read;
	__u64	bytes_written;
	__u64	checksum64;
	__u64	error_detail;
};

/* NPU query */
#define UB_QUERY_NPU_CAPS		1

struct ub_npu_query_status_v1 {
	__u32	version;
	__u32	status_reg;
	__u32	error_reg;
	__u32	reserved;
	__u64	last_req_id;
	struct ub_npu_cpl_v1 completion;
	__u64	backend_profile;
	__u64	supported_commands;
	__u64	reserved2;
};

struct ub_npu_query_v1 {
	__u32	version;
	__u32	type;
	union {
		struct ub_npu_query_status_v1 status;
		__u64	raw[11];
	} u;
};

/* NPU MMIO register offsets */
#define NPU_MMIO_SIZE		0x1000
#define NPU_CMD_SLOT_OFF	0x000
#define NPU_CPL_SLOT_OFF	0x400
#define NPU_CNA_OFF		0x500
#define NPU_STATUS_OFF		0x508
#define NPU_ERROR_OFF		0x50c
#define NPU_DOORBELL_OFF	0x510
#define NPU_CLEAR_CPL_OFF	0x514
#define NPU_LAST_REQ_ID_OFF	0x518
#define NPU_STATS_OFF		0x520

/* Status register bits */
#define NPU_STATUS_READY		(1u << 0)
#define NPU_STATUS_BUSY			(1u << 1)
#define NPU_STATUS_COMPLETION_VALID	(1u << 2)
#define NPU_STATUS_ERROR		(1u << 3)

/* ioctl numbers */
#define UB_NPU_IOC_MAGIC	'N'
#define UB_NPU_SUBMIT		_IOW(UB_NPU_IOC_MAGIC, 1, struct ub_npu_cmd_v1)
#define UB_NPU_WAIT		_IOR(UB_NPU_IOC_MAGIC, 2, struct ub_npu_cpl_v1)
#define UB_NPU_QUERY		_IOR(UB_NPU_IOC_MAGIC, 3, struct ub_npu_query_v1)

#endif /* _UAPI_UB_NPU_H */
