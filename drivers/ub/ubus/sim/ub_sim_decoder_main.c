/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Main Module
 * Top-level driver with OBMM integration
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "ub_sim_decoder.h"
#include <uapi/ub/gsva.h>
#include "../../obmm/obmm_sim_decoder.h"

static struct ub_sim_decoder *g_decoder;

/*
 * OBMM Import Callback
 * Called by OBMM when user imports remote memory
 */
static int ub_sim_decoder_obmm_import(void *import_info)
{
	struct obmm_sim_dec_import_info *info = import_info;
	struct sim_dec_map_req map_req = { 0 };
	struct sim_dec_gva_map_req gva_req = { 0 };
	u64 map_id = 0;
	int ret;
	bool use_gva_map;

	if (!info || !g_decoder || !g_decoder->enabled)
		return -EINVAL;

	map_req.local_pa = info->local_pa;
	map_req.size = info->size;
	map_req.remote_uba = info->remote_uba;
	map_req.token_id = info->token_id;
	map_req.token_value = info->token_value;
	map_req.scna = info->scna;
	map_req.dcna = info->dcna;
	memcpy(map_req.seid, info->seid, sizeof(map_req.seid));
	memcpy(map_req.deid, info->deid, sizeof(map_req.deid));
	map_req.upi = info->upi;
	map_req.src_eid = info->src_eid;

	/* GSVA V1 path: use GSVA_MAP_V1 opcode for strict identity mappings */
	if (info->address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY) {
		struct sim_dec_gsva_map_req gsva_req = {0};
		struct sim_dec_gsva_map_resp gsva_resp = {0};

		gsva_req.version = 1;
		gsva_req.flags = 0;
		gsva_req.key.version = 1;
		gsva_req.key.segment_id = info->segment_id ?: (info->gva_id ?: 1);
		gsva_req.key.home_va = info->home_va;
		gsva_req.key.size = info->size;
		gsva_req.key.vmid = info->vmid;
		gsva_req.key.asid = info->asid;
		gsva_req.key.pte_offset = info->pte_offset;
		gsva_req.key.p_tag = info->p_tag;
		gsva_req.key.cache_policy = info->cache_policy;
		gsva_req.key.epoch = info->epoch ?: 1;
		gsva_req.local_pa = info->local_pa;
		gsva_req.local_va = info->local_va;
		gsva_req.remote_uba = info->remote_uba;
		gsva_req.token_id = info->token_id;
		gsva_req.token_value = info->token_value;
		gsva_req.source = OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER;
		gsva_req.address_profile = GSVA_ADDRESS_PROFILE_STRICT_GSVA;
		gsva_req.access_flags = info->access_flags;
		gsva_req.scna = info->scna;
		gsva_req.dcna = info->dcna;

		ret = ub_sim_dec_backend_gsva_map_v1(g_decoder, &gsva_req, &gsva_resp);
		if (ret) {
			pr_err("UB SIM Decoder: GSVA V1 map failed: %pe\n",
			       ERR_PTR(ret));
			return ret;
		}

		map_id = gsva_resp.map_id;
		info->map_id = map_id;
		pr_info("UB SIM Decoder: OBMM import GSVA V1 mapped map_id=%#llx\n",
			map_id);
		return 0;
	}

	use_gva_map =
		(info->map_source == OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER) ||
		(info->local_va != 0 || info->home_va != 0 ||
		 info->pte_offset != 0 || info->vmid != 0 || info->asid != 0 ||
		 info->tid != 0 || info->p_tag != 0 ||
		 info->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH ||
		 info->access_flags != 0 || info->gva_id != 0);

	if (use_gva_map) {
		gva_req.map_req = map_req;
		gva_req.local_va = info->local_va;
		gva_req.home_va = info->home_va;
		gva_req.pte_offset = info->pte_offset;
		gva_req.vmid = info->vmid;
		gva_req.asid = info->asid;
		gva_req.tid = info->tid;
		gva_req.p_tag = info->p_tag;
		gva_req.cache_policy = info->cache_policy;
		gva_req.map_source = info->map_source;
		gva_req.address_profile = info->address_profile;
		gva_req.access_flags = info->access_flags;
		gva_req.gva_id = info->gva_id;

		ret = ub_sim_decoder_gva_map(&g_decoder->service, &gva_req, &map_id);
		if (ret == -ENOTSUPP || ret == -EOPNOTSUPP) {
			pr_info("UB SIM Decoder: no GVA map backend, fallback to legacy map\n");
			ret = ub_sim_decoder_map(&g_decoder->service, &map_req,
						 0, 0, &map_id);
		}
	} else {
		ret = ub_sim_decoder_map(&g_decoder->service, &map_req,
					 info->remote_export_mem_id,
					 info->remote_export_generation, &map_id);
	}
	if (ret) {
		pr_err("UB SIM Decoder: OBMM import map failed: %pe\n",
		       ERR_PTR(ret));
		return ret;
	}

	info->map_id = map_id;
	pr_info("UB SIM Decoder: OBMM import mapped map_id=%#llx\n", map_id);
	return 0;
}

static int ub_sim_decoder_obmm_unimport(void *unimport_info)
{
	struct obmm_sim_dec_unimport_info *info = unimport_info;
	struct sim_dec_gsva_unmap_req gsva_req = {0};
	struct sim_dec_gsva_unmap_resp gsva_resp = {0};
	int ret;

	if (!info || !g_decoder || !g_decoder->enabled)
		return -EINVAL;

	if (info->is_gsva) {
		gsva_req.version = 1;
		gsva_req.map_id = info->map_id;
		ret = ub_sim_dec_backend_gsva_unmap_v1(g_decoder, info->scna,
						       &gsva_req, &gsva_resp);
		if (ret == 0 || gsva_resp.error == GSVA_ERR_ROUTE_MISSING) {
			pr_info("UB SIM Decoder: OBMM unimport GSVA V1 unmapped map_id=%#llx\n",
				info->map_id);
			return 0;
		}
	}

	ret = ub_sim_decoder_unmap(&g_decoder->service, info->map_id);
	if (ret) {
		pr_err("UB SIM Decoder: OBMM unimport unmap failed map_id=%#llx: %pe\n",
		       info->map_id, ERR_PTR(ret));
		return ret;
	}

	pr_info("UB SIM Decoder: OBMM unimport unmapped map_id=%#llx\n",
		info->map_id);
	return 0;
}

static int ub_sim_decoder_obmm_export_retire_cb(void *retire_info)
{
	const struct obmm_sim_dec_export_retire_info *info = retire_info;
	struct sim_dec_obmm_export_retire_req req = { 0 };
	int ret;

	if (!info || !g_decoder || !g_decoder->enabled)
		return -EINVAL;
	req.export_mem_id = info->export_mem_id;
	req.remote_uba = info->remote_uba;
	req.size = info->size;
	req.export_cna = info->export_cna;
	req.token_id = info->token_id;
	ret = ub_sim_dec_backend_obmm_export_retire(g_decoder,
						     info->export_cna, &req);
	if (ret) {
		pr_err("UB SIM Decoder: OBMM export retire failed mem_id=%#llx: %pe\n",
		       info->export_mem_id, ERR_PTR(ret));
		return ret;
	}
	pr_info("UB SIM Decoder: OBMM export retired mem_id=%#llx uba=%#llx\n",
		info->export_mem_id, info->remote_uba);
	return 0;
}

static int ub_sim_decoder_init(void)
{
	int ret;

	g_decoder = kzalloc(sizeof(*g_decoder), GFP_KERNEL);
	if (!g_decoder)
		return -ENOMEM;
	g_ub_sim_decoder = g_decoder;

	/* Initialize backend type from module param */
	g_decoder->backend_type = UB_SIM_DEC_BACKEND_SIM;

	/* Initialize service layer */
	ret = ub_sim_decoder_service_init(&g_decoder->service);
	if (ret < 0) {
		pr_err("UB SIM Decoder: failed to init service\n");
		goto err_free;
	}

	/* Initialize control adapter */
	ret = ub_sim_dec_ctrl_adapter_init(&g_decoder->adapter,
					   &g_decoder->service);
	if (ret < 0) {
		pr_err("UB SIM Decoder: failed to init adapter\n");
		goto err_service;
	}

	ret = ub_sim_decoder_proc_init(&g_decoder->service);
	if (ret < 0) {
		pr_err("UB SIM Decoder: failed to init proc diagnostics: %pe\n",
		       ERR_PTR(ret));
		goto err_adapter;
	}

	/* Register OBMM callback */
	ret = obmm_register_import_callback(ub_sim_decoder_obmm_import);
	if (ret < 0) {
		pr_warn("UB SIM Decoder: failed to register OBMM callback (%d)\n",
			ret);
	}
	ret = obmm_register_unimport_callback(ub_sim_decoder_obmm_unimport);
	if (ret < 0)
		pr_warn("UB SIM Decoder: failed to register OBMM unimport callback (%d)\n",
			ret);
	ret = obmm_register_export_retire_callback(
		ub_sim_decoder_obmm_export_retire_cb);
	if (ret < 0)
		pr_warn("UB SIM Decoder: failed to register OBMM export retire callback (%d)\n",
			ret);

	g_decoder->enabled = true;
	pr_info("UB SIM Decoder: module loaded\n");

	return 0;

err_adapter:
	ub_sim_dec_ctrl_adapter_exit(&g_decoder->adapter);
err_service:
	ub_sim_decoder_service_exit(&g_decoder->service);
err_free:
	g_ub_sim_decoder = NULL;
	kfree(g_decoder);
	g_decoder = NULL;
	return ret;
}

static void ub_sim_decoder_exit(void)
{
	if (!g_decoder)
		return;

	obmm_unregister_export_retire_callback();
	obmm_unregister_unimport_callback();
	obmm_unregister_import_callback();

	ub_sim_decoder_proc_exit();
	ub_sim_dec_ctrl_adapter_exit(&g_decoder->adapter);
	ub_sim_decoder_service_exit(&g_decoder->service);

	g_ub_sim_decoder = NULL;
	kfree(g_decoder);
	g_decoder = NULL;

	pr_info("UB SIM Decoder: module unloaded\n");
}

module_init(ub_sim_decoder_init);
module_exit(ub_sim_decoder_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("HiSilicon");
MODULE_DESCRIPTION("UB Simulation Decoder Driver");
MODULE_VERSION("1.0");
