/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Driver - Header
 * Implements cross-node memory access for QEMU/UB simulation
 */

#ifndef __UB_SIM_DECODER_H__
#define __UB_SIM_DECODER_H__

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>

/* Control protocol version */
#define SIM_DEC_PROTO_VERSION	1
#define SIM_DEC_CTRL_PRIVATE_CMD 0x05

/* Control opcodes */
enum sim_dec_opcode {
	SIM_DEC_OP_MAP		= 0x01,
	SIM_DEC_OP_UNMAP	= 0x02,
	SIM_DEC_OP_SYNC		= 0x03,
	SIM_DEC_OP_QUERY	= 0x04,
	SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH = 0x05,
	SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP = 0x06,
};

/* Control command status */
enum sim_dec_status {
	SIM_DEC_STATUS_SUCCESS		= 0x00,
	SIM_DEC_STATUS_INVALID_PARAM	= 0x01,
	SIM_DEC_STATUS_RESOURCE_BUSY	= 0x02,
	SIM_DEC_STATUS_BACKEND_ERROR	= 0x03,
	SIM_DEC_STATUS_TIMEOUT		= 0x04,
	SIM_DEC_STATUS_NOT_SUPPORTED	= 0x05,
};

/* Control message header */
struct sim_dec_msg_hdr {
	u8	version;	/* Protocol version */
	u8	opcode;		/* SIM_DEC_OP_* */
	u16	seq;		/* Sequence number */
	u16	status;		/* Response status */
	u16	payload_len;	/* Length of payload following header */
};

/* MAP request payload */
struct sim_dec_map_req {
	u64	local_pa;	/* Local physical address */
	u64	size;		/* Mapping size */
	u64	remote_uba;	/* Remote UBA address */
	u32	token_id;
	u32	token_value;
	u32	scna;		/* Source CNA */
	u32	dcna;		/* Destination CNA */
	u8	seid[16];	/* Source EID */
	u8	deid[16];	/* Destination EID */
	u32	upi;
	u32	src_eid;
};

/* MAP response payload */
struct sim_dec_map_resp {
	u64	map_id;		/* Unique map identifier */
	u32	status;
	u32	rsvd;
};

/* UNMAP request payload */
struct sim_dec_unmap_req {
	u64	map_id;		/* Map ID from MAP response */
};

/* SYNC request payload */
struct sim_dec_sync_req {
	u64	map_id;
	u64	offset;
	u64	len;
};

/* QUERY request/response payload */
struct sim_dec_query_req {
	u64	map_id;
};

struct sim_dec_query_resp {
	u64	local_pa;
	u64	size;
	u32	status;
	u32	ref_count;
};

#define SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES 8

struct sim_dec_obmm_bootstrap_record {
	u64	export_mem_id;
	u64	remote_uba;
	u64	size;
	u64	generation;
	u64	flags;
	u32	node_id;
	u32	node_count;
	u32	export_cna;
	u32	token_id;
};

struct sim_dec_obmm_bootstrap_publish_req {
	struct sim_dec_obmm_bootstrap_record record;
};

struct sim_dec_obmm_bootstrap_lookup_req {
	u64	generation;
	u32	node_count;
	u32	rsvd;
};

struct sim_dec_obmm_bootstrap_lookup_resp {
	u32	count;
	u32	rsvd;
	struct sim_dec_obmm_bootstrap_record records[SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES];
};

/* Map entry maintained by service layer */
struct ub_sim_dec_map_entry {
	struct list_head	list;
	u64			map_id;
	struct sim_dec_map_req	req;
	unsigned long		create_time;
	u32			ref_count;
	bool			active;
};

/* Service layer context */
struct ub_sim_decoder_service {
	struct list_head	map_list;
	struct mutex		lock;
	u64			next_map_id;
	bool			initialized;
};

/* Control adapter context */
struct ub_sim_dec_ctrl_adapter {
	struct ub_sim_decoder_service	*service;
	struct mutex			lock;
	u16				seq;
	bool				connected;
};

/* Backend types */
enum ub_sim_dec_backend_type {
	UB_SIM_DEC_BACKEND_SIM,
	UB_SIM_DEC_BACKEND_HW,
};

/* Main decoder context */
struct ub_sim_decoder {
	struct ub_sim_decoder_service	service;
	struct ub_sim_dec_ctrl_adapter	adapter;
	enum ub_sim_dec_backend_type	backend_type;
	struct device			*dev;
	bool				enabled;
};

/* Service API */
int ub_sim_decoder_service_init(struct ub_sim_decoder_service *svc);
void ub_sim_decoder_service_exit(struct ub_sim_decoder_service *svc);
int ub_sim_decoder_map(struct ub_sim_decoder_service *svc,
		       struct sim_dec_map_req *req, u64 *map_id);
int ub_sim_decoder_unmap(struct ub_sim_decoder_service *svc, u64 map_id);
int ub_sim_decoder_sync(struct ub_sim_decoder_service *svc, u64 map_id,
			u64 offset, u64 len);
int ub_sim_decoder_query(struct ub_sim_decoder_service *svc, u64 map_id,
			 struct sim_dec_query_resp *resp);
int ub_sim_decoder_obmm_bootstrap_publish(u32 scna,
		const struct sim_dec_obmm_bootstrap_record *record);
int ub_sim_decoder_obmm_bootstrap_lookup(u32 scna, u32 node_count,
		u64 generation,
		struct sim_dec_obmm_bootstrap_lookup_resp *resp);

/* Control adapter API */
int ub_sim_dec_ctrl_adapter_init(struct ub_sim_dec_ctrl_adapter *adapter,
				 struct ub_sim_decoder_service *svc);
void ub_sim_dec_ctrl_adapter_exit(struct ub_sim_dec_ctrl_adapter *adapter);
int ub_sim_dec_send_cmd(struct ub_sim_dec_ctrl_adapter *adapter,
			u32 scna, u8 opcode, void *req, u16 req_len,
			void *resp, u16 resp_len);

/* Backend API */
int ub_sim_dec_backend_map(struct ub_sim_decoder *dec,
			   struct sim_dec_map_req *req, u64 *map_id);
int ub_sim_dec_backend_unmap(struct ub_sim_decoder *dec, u32 scna, u64 map_id);
int ub_sim_dec_backend_sync(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			    u64 offset, u64 len);
int ub_sim_dec_backend_query(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			     struct sim_dec_query_resp *resp);
int ub_sim_dec_backend_obmm_bootstrap_publish(struct ub_sim_decoder *dec,
		u32 scna, const struct sim_dec_obmm_bootstrap_record *record);
int ub_sim_dec_backend_obmm_bootstrap_lookup(struct ub_sim_decoder *dec,
		u32 scna, u32 node_count, u64 generation,
		struct sim_dec_obmm_bootstrap_lookup_resp *resp);

/* Global decoder instance */
extern struct ub_sim_decoder *g_ub_sim_decoder;

#endif /* __UB_SIM_DECODER_H__ */
