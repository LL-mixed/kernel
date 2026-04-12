/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Service Layer
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "ub_sim_decoder.h"

int ub_sim_decoder_service_init(struct ub_sim_decoder_service *svc)
{
	if (!svc)
		return -EINVAL;

	INIT_LIST_HEAD(&svc->map_list);
	mutex_init(&svc->lock);
	svc->next_map_id = 1;
	svc->initialized = true;

	pr_info("UB SIM Decoder Service: initialized\n");
	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_service_init);

void ub_sim_decoder_service_exit(struct ub_sim_decoder_service *svc)
{
	struct ub_sim_dec_map_entry *entry, *tmp;

	if (!svc)
		return;

	mutex_lock(&svc->lock);
	list_for_each_entry_safe(entry, tmp, &svc->map_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&svc->lock);

	svc->initialized = false;
	pr_info("UB SIM Decoder Service: exited\n");
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_service_exit);

/* Find map entry by map_id */
static struct ub_sim_dec_map_entry *
find_map_entry(struct ub_sim_decoder_service *svc, u64 map_id)
{
	struct ub_sim_dec_map_entry *entry;

	list_for_each_entry(entry, &svc->map_list, list) {
		if (entry->map_id == map_id)
			return entry;
	}
	return NULL;
}

/* Check for overlapping mappings */
static bool check_overlap(struct ub_sim_decoder_service *svc,
			  struct sim_dec_map_req *req)
{
	struct ub_sim_dec_map_entry *entry;
	u64 req_end = req->local_pa + req->size;

	list_for_each_entry(entry, &svc->map_list, list) {
		u64 entry_end = entry->req.local_pa + entry->req.size;

		if (entry->active &&
		    !(req_end <= entry->req.local_pa || req->local_pa >= entry_end)) {
			pr_warn("UB SIM Decoder: PA overlap detected: "
				"new[%llx-%llx] existing[%llx-%llx]\n",
				req->local_pa, req_end,
				entry->req.local_pa, entry_end);
			return true;
		}
	}
	return false;
}

int ub_sim_decoder_map(struct ub_sim_decoder_service *svc,
		       struct sim_dec_map_req *req, u64 *map_id)
{
	struct ub_sim_dec_map_entry *entry;
	u64 new_id;
	int ret;

	if (!svc || !req || !map_id)
		return -EINVAL;

	if (!svc->initialized)
		return -ENODEV;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;

	/* Validate request */
	if (req->size == 0 || req->size > (1ULL << 40)) {
		pr_err("UB SIM Decoder: invalid size %llx\n", req->size);
		return -EINVAL;
	}

	if (req->token_id == 0) {
		pr_err("UB SIM Decoder: token_id cannot be 0\n");
		return -EINVAL;
	}

	mutex_lock(&svc->lock);

	/* Check for overlapping mappings */
	if (check_overlap(svc, req)) {
		mutex_unlock(&svc->lock);
		return -EBUSY;
	}
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_map(g_ub_sim_decoder, req, &new_id);
	if (ret) {
		pr_err("UB SIM Decoder: backend map failed: %pe\n", ERR_PTR(ret));
		return ret;
	}

	mutex_lock(&svc->lock);
	if (check_overlap(svc, req)) {
		mutex_unlock(&svc->lock);
		(void)ub_sim_dec_backend_unmap(g_ub_sim_decoder, req->scna, new_id);
		return -EBUSY;
	}

	/* Allocate new map entry */
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&svc->lock);
		(void)ub_sim_dec_backend_unmap(g_ub_sim_decoder, req->scna, new_id);
		return -ENOMEM;
	}

	entry->map_id = new_id;
	memcpy(&entry->req, req, sizeof(*req));
	entry->create_time = jiffies;
	entry->ref_count = 1;
	entry->active = true;

	list_add_tail(&entry->list, &svc->map_list);
	*map_id = new_id;

	mutex_unlock(&svc->lock);

	pr_info("UB SIM Decoder: map created id=%llx pa=%llx size=%llx "
		"remote_uba=%llx token=%u\n",
		new_id, req->local_pa, req->size,
		req->remote_uba, req->token_id);

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_map);

int ub_sim_decoder_unmap(struct ub_sim_decoder_service *svc, u64 map_id)
{
	struct ub_sim_dec_map_entry *entry;
	u32 scna;
	int ret;

	if (!svc)
		return -EINVAL;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;

	mutex_lock(&svc->lock);
	entry = find_map_entry(svc, map_id);
	if (!entry) {
		mutex_unlock(&svc->lock);
		pr_err("UB SIM Decoder: map_id %llx not found\n", map_id);
		return -ENOENT;
	}

	scna = entry->req.scna;
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_unmap(g_ub_sim_decoder, scna, map_id);
	if (ret)
		return ret;

	mutex_lock(&svc->lock);
	entry = find_map_entry(svc, map_id);
	if (!entry) {
		mutex_unlock(&svc->lock);
		return -ENOENT;
	}
	entry->active = false;
	entry->ref_count--;
	if (entry->ref_count == 0) {
		list_del(&entry->list);
		mutex_unlock(&svc->lock);
		pr_info("UB SIM Decoder: map freed id=%llx\n", map_id);
		kfree(entry);
	} else {
		mutex_unlock(&svc->lock);
		pr_info("UB SIM Decoder: map deactivated id=%llx ref=%u\n",
			map_id, entry->ref_count);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_unmap);

int ub_sim_decoder_sync(struct ub_sim_decoder_service *svc, u64 map_id,
			u64 offset, u64 len)
{
	struct ub_sim_dec_map_entry *entry;
	u32 scna;
	int ret;

	if (!svc)
		return -EINVAL;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;

	mutex_lock(&svc->lock);
	entry = find_map_entry(svc, map_id);
	if (!entry) {
		mutex_unlock(&svc->lock);
		return -ENOENT;
	}

	if (!entry->active) {
		mutex_unlock(&svc->lock);
		return -EINVAL;
	}

	/* Validate offset and length */
	if (offset + len > entry->req.size) {
		mutex_unlock(&svc->lock);
		return -EINVAL;
	}
	scna = entry->req.scna;

	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_sync(g_ub_sim_decoder, scna, map_id, offset,
				      len);
	if (ret)
		return ret;

	pr_debug("UB SIM Decoder: sync map_id=%llx offset=%llx len=%llx\n",
		 map_id, offset, len);
	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_sync);

/* Query map info (for debug) */
int ub_sim_decoder_query(struct ub_sim_decoder_service *svc, u64 map_id,
			 struct sim_dec_query_resp *resp)
{
	struct ub_sim_dec_map_entry *entry;
	u32 scna;
	int ret;

	if (!svc || !resp)
		return -EINVAL;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;

	mutex_lock(&svc->lock);
	entry = find_map_entry(svc, map_id);
	if (!entry) {
		mutex_unlock(&svc->lock);
		return -ENOENT;
	}
	scna = entry->req.scna;

	resp->local_pa = entry->req.local_pa;
	resp->size = entry->req.size;
	resp->status = entry->active ? 0 : 1;
	resp->ref_count = entry->ref_count;
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_query(g_ub_sim_decoder, scna, map_id, resp);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_query);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UB Simulation Decoder Service");
