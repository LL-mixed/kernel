/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 *
 * UB Simulation Decoder Service Layer
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include "ub_sim_decoder.h"
#include "../../obmm/obmm_sim_decoder.h"

static struct ub_sim_decoder_service *g_proc_service;
static struct proc_dir_entry *g_proc_dir;
static struct proc_dir_entry *g_proc_gva_routes;

static const char *ub_sim_dec_map_state_name(enum ub_sim_dec_map_state state)
{
	switch (state) {
	case UB_SIM_DEC_MAP_CREATING:
		return "creating";
	case UB_SIM_DEC_MAP_ACTIVE:
		return "active";
	case UB_SIM_DEC_MAP_STALE:
		return "stale";
	case UB_SIM_DEC_MAP_ERROR:
		return "error";
	case UB_SIM_DEC_MAP_RETIRED:
		return "retired";
	default:
		return "unknown";
	}
}

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

static int ub_sim_decoder_gva_routes_show(struct seq_file *m, void *v)
{
	struct ub_sim_decoder_service *svc = m->private;
	struct ub_sim_dec_map_entry *entry;

	seq_puts(m, "map_id state last_error active map_source address_profile vmid asid "
		 "local_pa size local_va home_va remote_uba pte_offset "
		 "scna dcna tid upi request_p_tag effective_p_tag "
		 "mp_ubc_port mp_lane mp_link_id token_id token_value "
		 "cache_policy access_flags gva_id\n");

	if (!svc)
		return 0;

	mutex_lock(&svc->lock);
	list_for_each_entry(entry, &svc->map_list, list) {
		const struct sim_dec_gva_map_req *req = &entry->req;

		seq_printf(m,
			   "%llx %s %d %u %u %u %u %u %llx %llx %llx %llx %llx %llx "
			   "%x %x %u %u %u %u %u %u %u %u %u %u %u %llx\n",
			   entry->map_id, ub_sim_dec_map_state_name(entry->state),
			   entry->last_error, entry->active ? 1 : 0,
			   req->map_source, req->address_profile,
			   req->vmid, req->asid, req->map_req.local_pa,
			   req->map_req.size, req->local_va, req->home_va,
			   req->map_req.remote_uba, req->pte_offset,
			   req->map_req.scna, req->map_req.dcna,
			   req->tid, req->map_req.upi, req->p_tag,
			   entry->effective_p_tag, entry->mp_ubc_port,
			   entry->mp_lane, entry->mp_link_id,
			   req->map_req.token_id, req->map_req.token_value,
			   req->cache_policy, req->access_flags, req->gva_id);
	}
	mutex_unlock(&svc->lock);

	return 0;
}

static int ub_sim_decoder_gva_routes_open(struct inode *inode,
					  struct file *file)
{
	return single_open(file, ub_sim_decoder_gva_routes_show, pde_data(inode));
}

static const struct proc_ops ub_sim_decoder_gva_routes_ops = {
	.proc_open = ub_sim_decoder_gva_routes_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

int ub_sim_decoder_proc_init(struct ub_sim_decoder_service *svc)
{
	if (!svc)
		return -EINVAL;

	if (g_proc_gva_routes)
		return -EBUSY;

	g_proc_dir = proc_mkdir("ub_sim_decoder", NULL);
	if (!g_proc_dir)
		return -ENOMEM;

	g_proc_gva_routes = proc_create_data("gva_routes", 0444, g_proc_dir,
					     &ub_sim_decoder_gva_routes_ops, svc);
	if (!g_proc_gva_routes) {
		remove_proc_entry("ub_sim_decoder", NULL);
		g_proc_dir = NULL;
		return -ENOMEM;
	}

	g_proc_service = svc;
	pr_info("UB SIM Decoder: proc diagnostics ready at /proc/ub_sim_decoder/gva_routes\n");
	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_proc_init);

void ub_sim_decoder_proc_exit(void)
{
	if (g_proc_gva_routes) {
		remove_proc_entry("gva_routes", g_proc_dir);
		g_proc_gva_routes = NULL;
	}
	if (g_proc_dir) {
		remove_proc_entry("ub_sim_decoder", NULL);
		g_proc_dir = NULL;
	}
	g_proc_service = NULL;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_proc_exit);

void ub_sim_decoder_service_exit(struct ub_sim_decoder_service *svc)
{
	struct ub_sim_dec_map_entry *entry, *tmp;

	if (!svc)
		return;

	if (g_proc_service == svc)
		ub_sim_decoder_proc_exit();

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
static bool check_overlap_except(struct ub_sim_decoder_service *svc,
				 struct sim_dec_map_req *req,
				 const struct ub_sim_dec_map_entry *skip)
{
	struct ub_sim_dec_map_entry *entry;
	u64 req_end = req->local_pa + req->size;

	list_for_each_entry(entry, &svc->map_list, list) {
		u64 entry_end = entry->req.map_req.local_pa +
			entry->req.map_req.size;

		if (entry != skip &&
		    (entry->state == UB_SIM_DEC_MAP_CREATING ||
		     entry->state == UB_SIM_DEC_MAP_ACTIVE ||
		     entry->state == UB_SIM_DEC_MAP_STALE ||
		     entry->state == UB_SIM_DEC_MAP_ERROR) &&
		    !(req_end <= entry->req.map_req.local_pa ||
		      req->local_pa >= entry_end)) {
			pr_warn("UB SIM Decoder: PA overlap detected: "
				"new[%llx-%llx] existing[%llx-%llx]\n",
				req->local_pa, req_end,
				entry->req.map_req.local_pa, entry_end);
			return true;
		}
	}
	return false;
}

static bool check_overlap(struct ub_sim_decoder_service *svc,
			  struct sim_dec_map_req *req)
{
	return check_overlap_except(svc, req, NULL);
}

static bool gva_route_blocks_overlap(const struct ub_sim_dec_map_entry *entry)
{
	return entry &&
		(entry->state == UB_SIM_DEC_MAP_CREATING ||
		 entry->state == UB_SIM_DEC_MAP_ACTIVE ||
		 entry->state == UB_SIM_DEC_MAP_STALE ||
		 entry->state == UB_SIM_DEC_MAP_ERROR);
}

static bool is_explicit_gva_route(const struct sim_dec_gva_map_req *req)
{
	return req &&
		(req->map_source == OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER ||
		 req->address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY);
}

static bool check_gva_route_overlap_except(struct ub_sim_decoder_service *svc,
					   const struct sim_dec_gva_map_req *req,
					   const struct ub_sim_dec_map_entry *skip)
{
	struct ub_sim_dec_map_entry *entry;
	u64 req_end;

	if (!is_explicit_gva_route(req))
		return false;

	req_end = req->map_req.remote_uba + req->map_req.size;
	list_for_each_entry(entry, &svc->map_list, list) {
		const struct sim_dec_gva_map_req *entry_req = &entry->req;
		u64 entry_end;

		if (entry == skip || !gva_route_blocks_overlap(entry) ||
		    !is_explicit_gva_route(entry_req) ||
		    entry_req->vmid != req->vmid ||
		    entry_req->asid != req->asid)
			continue;

		entry_end = entry_req->map_req.remote_uba +
			entry_req->map_req.size;
		if (!(req_end <= entry_req->map_req.remote_uba ||
		      req->map_req.remote_uba >= entry_end)) {
			pr_warn("UB SIM Decoder: GVA route overlap detected: "
				"vmid=%u asid=%u new_uba[%llx-%llx] existing_uba[%llx-%llx]\n",
				req->vmid, req->asid, req->map_req.remote_uba,
				req_end, entry_req->map_req.remote_uba,
				entry_end);
			return true;
		}
	}
	return false;
}

static bool check_gva_route_overlap(struct ub_sim_decoder_service *svc,
				    const struct sim_dec_gva_map_req *req)
{
	return check_gva_route_overlap_except(svc, req, NULL);
}

int ub_sim_decoder_map(struct ub_sim_decoder_service *svc,
		       struct sim_dec_map_req *req,
		       u64 remote_export_mem_id,
		       u64 remote_export_generation, u64 *map_id)
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

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->map_id = 0;
	memset(&entry->req, 0, sizeof(entry->req));
	memcpy(&entry->req.map_req, req, sizeof(*req));
	entry->req.local_va = 0;
	entry->req.home_va = 0;
	entry->req.pte_offset = 0;
	entry->req.vmid = 0;
	entry->req.asid = 0;
	entry->req.tid = 0;
	entry->req.p_tag = 0;
	entry->req.cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
	entry->req.map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
	entry->req.address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
	entry->req.access_flags = 0;
	entry->req.gva_id = 0;
	entry->create_time = jiffies;
	entry->ref_count = 1;
	entry->state = UB_SIM_DEC_MAP_CREATING;
	entry->last_error = 0;
	entry->active = false;

	mutex_lock(&svc->lock);
	if (check_overlap(svc, req)) {
		mutex_unlock(&svc->lock);
		kfree(entry);
		return -EBUSY;
	}
	list_add_tail(&entry->list, &svc->map_list);
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_map(g_ub_sim_decoder, req,
				     remote_export_mem_id,
				     remote_export_generation, &new_id);
	if (ret) {
		pr_err("UB SIM Decoder: backend map failed: %pe\n", ERR_PTR(ret));
		mutex_lock(&svc->lock);
		list_del(&entry->list);
		mutex_unlock(&svc->lock);
		kfree(entry);
		return ret;
	}

	mutex_lock(&svc->lock);
	if (check_overlap_except(svc, req, entry)) {
		int rollback_ret;

		entry->map_id = new_id;
		entry->state = UB_SIM_DEC_MAP_ERROR;
		entry->last_error = -EBUSY;
		mutex_unlock(&svc->lock);
		rollback_ret = ub_sim_dec_backend_unmap(g_ub_sim_decoder,
							 req->scna, new_id);
		if (rollback_ret) {
			mutex_lock(&svc->lock);
			entry->last_error = rollback_ret;
			mutex_unlock(&svc->lock);
			return -EBUSY;
		}
		mutex_lock(&svc->lock);
		list_del(&entry->list);
		mutex_unlock(&svc->lock);
		kfree(entry);
		return -EBUSY;
	}

	entry->map_id = new_id;
	entry->state = UB_SIM_DEC_MAP_ACTIVE;
	entry->last_error = 0;
	entry->active = true;
	*map_id = new_id;

	mutex_unlock(&svc->lock);

	pr_info("UB SIM Decoder: map created id=%llx pa=%llx size=%llx "
		"remote_uba=%llx token=%u\n",
		new_id, req->local_pa, req->size,
		req->remote_uba, req->token_id);

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_map);

int ub_sim_decoder_gva_map(struct ub_sim_decoder_service *svc,
			   struct sim_dec_gva_map_req *req, u64 *map_id)
{
	struct ub_sim_dec_map_entry *entry;
	struct sim_dec_map_resp backend_resp = {0};
	u64 new_id;
	int ret;

	if (!svc || !req || !map_id)
		return -EINVAL;

	if (!svc->initialized)
		return -ENODEV;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;

	/* Validate request */
	if (req->map_req.size == 0 || req->map_req.size > (1ULL << 40)) {
		pr_err("UB SIM Decoder: invalid size %llx\n", req->map_req.size);
		return -EINVAL;
	}

	if (req->map_req.token_id == 0) {
		pr_err("UB SIM Decoder: token_id cannot be 0\n");
		return -EINVAL;
	}

	if (req->address_profile != OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA &&
	    req->address_profile != OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY) {
		pr_err("UB SIM Decoder: unsupported address_profile %u\n",
		       req->address_profile);
		return -EINVAL;
	}

	if (req->address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY &&
	    (req->pte_offset != 0 || req->local_va == 0 ||
	     req->home_va != req->local_va || req->home_va != req->map_req.remote_uba)) {
		pr_err("UB SIM Decoder: GSVA profile requires local_va/home_va/remote_uba equal and pte_offset=0\n");
		return -EINVAL;
	}

	if (req->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_NC &&
	    req->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH &&
	    req->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE &&
	    req->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK &&
	    req->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI) {
		pr_err("UB SIM Decoder: unsupported GVA cache_policy %u\n",
		       req->cache_policy);
		return -EINVAL;
	}

	if (req->cache_policy == OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE &&
	    !(req->access_flags & OBMM_SIM_DEC_ACCESS_READ_ONLY)) {
		pr_err("UB SIM Decoder: read_cache requires READ_ONLY access_flags\n");
		return -EINVAL;
	}

	if (req->cache_policy == OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK &&
	    !(req->access_flags & OBMM_SIM_DEC_ACCESS_EXPLICIT_SYNC)) {
		pr_err("UB SIM Decoder: write_back requires EXPLICIT_SYNC access_flags\n");
		return -EINVAL;
	}

	if (req->map_source == 0 ||
	    req->map_source == OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM) {
		/* allow explicit legacy profile */
	} else if (req->map_source == OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER) {
		if (req->local_va == 0) {
			pr_err("UB SIM Decoder: GVA manager map requires local_va\n");
			return -EINVAL;
		}
	} else {
		pr_err("UB SIM Decoder: unsupported map_source %u\n", req->map_source);
		return -EINVAL;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->map_id = 0;
	memcpy(&entry->req, req, sizeof(*req));
	entry->create_time = jiffies;
	entry->ref_count = 1;
	entry->state = UB_SIM_DEC_MAP_CREATING;
	entry->last_error = 0;
	entry->active = false;

	mutex_lock(&svc->lock);
	if (check_overlap(svc, &req->map_req) ||
	    check_gva_route_overlap(svc, req)) {
		mutex_unlock(&svc->lock);
		kfree(entry);
		return -EBUSY;
	}
	list_add_tail(&entry->list, &svc->map_list);
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_gva_map(g_ub_sim_decoder, req, &new_id,
					 &backend_resp);
	if (ret) {
		pr_err("UB SIM Decoder: backend GVA map failed: %pe\n", ERR_PTR(ret));
		mutex_lock(&svc->lock);
		list_del(&entry->list);
		mutex_unlock(&svc->lock);
		kfree(entry);
		return ret;
	}

	mutex_lock(&svc->lock);
	if (check_overlap_except(svc, &req->map_req, entry) ||
	    check_gva_route_overlap_except(svc, req, entry)) {
		int rollback_ret;

		entry->map_id = new_id;
		entry->state = UB_SIM_DEC_MAP_ERROR;
		entry->last_error = -EBUSY;
		mutex_unlock(&svc->lock);
		rollback_ret = ub_sim_dec_backend_unmap(g_ub_sim_decoder,
							 req->map_req.scna,
							 new_id);
		if (rollback_ret) {
			mutex_lock(&svc->lock);
			entry->last_error = rollback_ret;
			mutex_unlock(&svc->lock);
			return -EBUSY;
		}
		mutex_lock(&svc->lock);
		list_del(&entry->list);
		mutex_unlock(&svc->lock);
		kfree(entry);
		return -EBUSY;
	}

	entry->map_id = new_id;
	entry->state = UB_SIM_DEC_MAP_ACTIVE;
	entry->last_error = 0;
	entry->effective_p_tag = backend_resp.p_tag;
	entry->mp_ubc_port = backend_resp.mp_ubc_port;
	entry->mp_lane = backend_resp.mp_lane;
	entry->mp_link_id = backend_resp.mp_link_id;
	entry->active = true;
	*map_id = new_id;

	mutex_unlock(&svc->lock);

	pr_info("UB SIM Decoder: gva map created id=%llx pa=%llx size=%llx "
		"remote_uba=%llx token=%u vmid=%u asid=%u local_va=%llx home_va=%llx "
		"pte_offset=%llx address_profile=%u request_p_tag=%u effective_p_tag=%u "
		"mp_ubc_port=%u mp_lane=%u mp_link_id=%u\n",
		new_id, req->map_req.local_pa, req->map_req.size,
		req->map_req.remote_uba, req->map_req.token_id,
		req->vmid, req->asid, req->local_va, req->home_va,
		req->pte_offset, req->address_profile, req->p_tag,
		entry->effective_p_tag, entry->mp_ubc_port, entry->mp_lane,
		entry->mp_link_id);

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_gva_map);

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

	scna = entry->req.map_req.scna;
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_unmap(g_ub_sim_decoder, scna, map_id);
	if (ret) {
		mutex_lock(&svc->lock);
		entry = find_map_entry(svc, map_id);
		if (entry) {
			entry->state = UB_SIM_DEC_MAP_ERROR;
			entry->last_error = ret;
			entry->active = false;
		}
		mutex_unlock(&svc->lock);
		pr_err("UB SIM Decoder: backend unmap failed map_id=%llx: %pe\n",
		       map_id, ERR_PTR(ret));
		return ret;
	}

	mutex_lock(&svc->lock);
	entry = find_map_entry(svc, map_id);
	if (!entry) {
		mutex_unlock(&svc->lock);
		return -ENOENT;
	}
	entry->state = UB_SIM_DEC_MAP_RETIRED;
	entry->last_error = 0;
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

	/* Validate offset and length without allowing u64 wraparound */
	if (offset > entry->req.map_req.size ||
	    len > entry->req.map_req.size - offset) {
		mutex_unlock(&svc->lock);
		return -EINVAL;
	}
	scna = entry->req.map_req.scna;

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
	scna = entry->req.map_req.scna;

	resp->local_pa = entry->req.map_req.local_pa;
	resp->size = entry->req.map_req.size;
	resp->status = entry->active ? 0 : 1;
	resp->ref_count = entry->ref_count;
	mutex_unlock(&svc->lock);

	ret = ub_sim_dec_backend_query(g_ub_sim_decoder, scna, map_id, resp);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_query);

int ub_sim_decoder_obmm_bootstrap_publish(u32 scna,
		const struct sim_dec_obmm_bootstrap_record *record)
{
	if (!record)
		return -EINVAL;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;
	if (record->node_count < 2 ||
	    record->node_count > SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES ||
	    record->node_id >= record->node_count ||
	    record->export_cna == 0 ||
	    record->remote_uba == 0 || record->size == 0 ||
	    record->generation == 0)
		return -EINVAL;

	return ub_sim_dec_backend_obmm_bootstrap_publish(g_ub_sim_decoder,
							 scna, record);
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_obmm_bootstrap_publish);

int ub_sim_decoder_obmm_bootstrap_lookup(u32 scna, u32 node_count,
		u64 generation,
		struct sim_dec_obmm_bootstrap_lookup_resp *resp)
{
	if (!resp)
		return -EINVAL;
	if (!g_ub_sim_decoder || !g_ub_sim_decoder->enabled)
		return -ENODEV;
	if (node_count < 2 || node_count > SIM_DEC_OBMM_BOOTSTRAP_MAX_NODES)
		return -EINVAL;
	if (generation == 0)
		return -EINVAL;

	memset(resp, 0, sizeof(*resp));
	return ub_sim_dec_backend_obmm_bootstrap_lookup(g_ub_sim_decoder, scna,
							node_count, generation,
							resp);
}
EXPORT_SYMBOL_GPL(ub_sim_decoder_obmm_bootstrap_lookup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("UB Simulation Decoder Service");
