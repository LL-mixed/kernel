/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Backend Selection
 * Routes operations to QEMU simulation backend or hardware backend
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include "ub_sim_decoder.h"

/* Module parameter for backend selection */
static int backend_type = UB_SIM_DEC_BACKEND_SIM;
module_param(backend_type, int, 0644);
MODULE_PARM_DESC(backend_type, "Decoder backend: 0=SIM, 1=HW");

/* Global decoder instance */
struct ub_sim_decoder *g_ub_sim_decoder;

static int sim_dec_status_to_errno(u32 status)
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
		return -ENOTSUPP;
	default:
		return -EIO;
	}
}

/* Backend-specific implementations */
static int sim_backend_map(struct ub_sim_decoder *dec,
			   struct sim_dec_map_req *req, u64 *map_id)
{
	struct sim_dec_map_resp resp = {0};
	int ret;

	/* Send MAP command through control adapter */
	ret = ub_sim_dec_send_cmd(&dec->adapter, req->scna, SIM_DEC_OP_MAP,
				  req, sizeof(*req), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	if (resp.status != SIM_DEC_STATUS_SUCCESS) {
		pr_err("UB SIM Decoder: backend MAP failed status=%u\n",
			resp.status);
		return sim_dec_status_to_errno(resp.status);
	}

	*map_id = resp.map_id;
	return 0;
}

static int sim_backend_gva_map(struct ub_sim_decoder *dec,
			       struct sim_dec_gva_map_req *req, u64 *map_id,
			       struct sim_dec_map_resp *resp_out)
{
	struct sim_dec_map_resp resp = {0};
	int ret;

	ret = ub_sim_dec_send_cmd(&dec->adapter, req->map_req.scna,
				  SIM_DEC_OP_GVA_MAP, req, sizeof(*req),
				  &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	if (resp.status != SIM_DEC_STATUS_SUCCESS) {
		pr_err("UB SIM Decoder: backend GVA MAP failed status=%u\n",
		       resp.status);
		return sim_dec_status_to_errno(resp.status);
	}

	*map_id = resp.map_id;
	if (resp_out)
		*resp_out = resp;
	return 0;
}

static int sim_backend_unmap(struct ub_sim_decoder *dec, u32 scna, u64 map_id)
{
	struct sim_dec_unmap_req req = { .map_id = map_id };
	int ret;

	ret = ub_sim_dec_send_cmd(&dec->adapter, scna, SIM_DEC_OP_UNMAP,
				  &req, sizeof(req), NULL, 0);
	return ret;
}

static int sim_backend_sync(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			    u64 offset, u64 len)
{
	struct sim_dec_sync_req req = {
		.map_id = map_id,
		.offset = offset,
		.len = len
	};
	int ret;

	ret = ub_sim_dec_send_cmd(&dec->adapter, scna, SIM_DEC_OP_SYNC,
				  &req, sizeof(req), NULL, 0);
	return ret;
}

static int sim_backend_coh_fence(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
				  u64 offset, u64 len)
{
	struct sim_dec_sync_req req = {
		.map_id = map_id,
		.offset = offset,
		.len = len
	};

	return ub_sim_dec_send_cmd(&dec->adapter, scna, SIM_DEC_OP_COH_FENCE,
				   &req, sizeof(req), NULL, 0);
}

static int sim_backend_query(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			     struct sim_dec_query_resp *resp)
{
	struct sim_dec_query_req req = { .map_id = map_id };
	int ret;

	ret = ub_sim_dec_send_cmd(&dec->adapter, scna, SIM_DEC_OP_QUERY,
				  &req, sizeof(req), resp, sizeof(*resp));
	return ret;
}

static int sim_backend_obmm_bootstrap_publish(struct ub_sim_decoder *dec,
		u32 scna, const struct sim_dec_obmm_bootstrap_record *record)
{
	struct sim_dec_obmm_bootstrap_publish_req req = {0};

	req.record = *record;
	return ub_sim_dec_send_cmd(&dec->adapter, scna,
				   SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH,
				   &req, sizeof(req), NULL, 0);
}

static int sim_backend_obmm_bootstrap_lookup(struct ub_sim_decoder *dec,
		u32 scna, u32 node_count, u64 generation,
		struct sim_dec_obmm_bootstrap_lookup_resp *resp)
{
	struct sim_dec_obmm_bootstrap_lookup_req req = {
		.node_count = node_count,
		.generation = generation,
	};

	return ub_sim_dec_send_cmd(&dec->adapter, scna,
				   SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP,
				   &req, sizeof(req), resp, sizeof(*resp));
}

/* Hardware backend stubs */
static int hw_backend_map(struct ub_sim_decoder *dec,
			  struct sim_dec_map_req *req, u64 *map_id)
{
	/* Direct hardware decoder configuration */
	pr_info("UB SIM Decoder: HW backend MAP (stub)\n");
	return -ENOTSUPP;
}

static int hw_backend_unmap(struct ub_sim_decoder *dec, u64 map_id)
{
	pr_info("UB SIM Decoder: HW backend UNMAP (stub)\n");
	return -ENOTSUPP;
}

static int hw_backend_sync(struct ub_sim_decoder *dec, u64 map_id,
			   u64 offset, u64 len)
{
	pr_info("UB SIM Decoder: HW backend SYNC (stub)\n");
	return -ENOTSUPP;
}

static int hw_backend_query(struct ub_sim_decoder *dec, u64 map_id,
			    struct sim_dec_query_resp *resp)
{
	pr_info("UB SIM Decoder: HW backend QUERY (stub)\n");
	return -ENOTSUPP;
}

/* Backend API entry points */
int ub_sim_dec_backend_map(struct ub_sim_decoder *dec,
			   struct sim_dec_map_req *req, u64 *map_id)
{
	if (!dec || !req || !map_id)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_map(dec, req, map_id);
	case UB_SIM_DEC_BACKEND_HW:
		return hw_backend_map(dec, req, map_id);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_map);

int ub_sim_dec_backend_gva_map(struct ub_sim_decoder *dec,
			       struct sim_dec_gva_map_req *req, u64 *map_id,
			       struct sim_dec_map_resp *resp_out)
{
	if (!dec || !req || !map_id)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_gva_map(dec, req, map_id, resp_out);
	case UB_SIM_DEC_BACKEND_HW:
		return -ENOTSUPP;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_gva_map);

int ub_sim_dec_backend_unmap(struct ub_sim_decoder *dec, u32 scna, u64 map_id)
{
	if (!dec)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_unmap(dec, scna, map_id);
	case UB_SIM_DEC_BACKEND_HW:
		return hw_backend_unmap(dec, map_id);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_unmap);

int ub_sim_dec_backend_sync(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			    u64 offset, u64 len)
{
	if (!dec)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_sync(dec, scna, map_id, offset, len);
	case UB_SIM_DEC_BACKEND_HW:
		return hw_backend_sync(dec, map_id, offset, len);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_sync);

int ub_sim_dec_backend_coh_fence(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
				  u64 offset, u64 len)
{
	if (!dec)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_coh_fence(dec, scna, map_id, offset, len);
	default:
		return -ENOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_coh_fence);

int ub_sim_dec_backend_query(struct ub_sim_decoder *dec, u32 scna, u64 map_id,
			     struct sim_dec_query_resp *resp)
{
	if (!dec || !resp)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_query(dec, scna, map_id, resp);
	case UB_SIM_DEC_BACKEND_HW:
		return hw_backend_query(dec, map_id, resp);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_query);

int ub_sim_dec_backend_obmm_bootstrap_publish(struct ub_sim_decoder *dec,
		u32 scna, const struct sim_dec_obmm_bootstrap_record *record)
{
	if (!dec || !record)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_obmm_bootstrap_publish(dec, scna, record);
	case UB_SIM_DEC_BACKEND_HW:
		return -ENOTSUPP;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_obmm_bootstrap_publish);

int ub_sim_dec_backend_obmm_bootstrap_lookup(struct ub_sim_decoder *dec,
		u32 scna, u32 node_count, u64 generation,
		struct sim_dec_obmm_bootstrap_lookup_resp *resp)
{
	if (!dec || !resp)
		return -EINVAL;

	switch (dec->backend_type) {
	case UB_SIM_DEC_BACKEND_SIM:
		return sim_backend_obmm_bootstrap_lookup(dec, scna, node_count,
							 generation, resp);
	case UB_SIM_DEC_BACKEND_HW:
		return -ENOTSUPP;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(ub_sim_dec_backend_obmm_bootstrap_lookup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UB Simulation Decoder Backend");
