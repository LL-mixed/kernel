// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 */
#include <linux/mm.h>
#include <linux/memory.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <ub/ubus/ub-mem-decoder.h>
#include <linux/numa_remote.h>

#include "obmm_core.h"
#include "obmm_cache.h"
#include "obmm_import.h"
#include "obmm_preimport.h"
#include "obmm_resource.h"
#include "obmm_addr_check.h"
#include "obmm_shm_dev.h"
#include "obmm_sim_decoder.h"

static DEFINE_MUTEX(g_obmm_sim_dec_cb_lock);
static int (*g_obmm_import_cb)(void *);
static int (*g_obmm_unimport_cb)(void *);

int obmm_register_import_callback(int (*import_fn)(void *))
{
	int ret = 0;

	if (!import_fn)
		return -EINVAL;

	mutex_lock(&g_obmm_sim_dec_cb_lock);
	if (g_obmm_import_cb)
		ret = -EBUSY;
	else
		g_obmm_import_cb = import_fn;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(obmm_register_import_callback);

int obmm_unregister_import_callback(void)
{
	mutex_lock(&g_obmm_sim_dec_cb_lock);
	g_obmm_import_cb = NULL;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(obmm_unregister_import_callback);

int obmm_register_unimport_callback(int (*unimport_fn)(void *))
{
	int ret = 0;

	if (!unimport_fn)
		return -EINVAL;

	mutex_lock(&g_obmm_sim_dec_cb_lock);
	if (g_obmm_unimport_cb)
		ret = -EBUSY;
	else
		g_obmm_unimport_cb = unimport_fn;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(obmm_register_unimport_callback);

int obmm_unregister_unimport_callback(void)
{
	mutex_lock(&g_obmm_sim_dec_cb_lock);
	g_obmm_unimport_cb = NULL;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(obmm_unregister_unimport_callback);

static void obmm_sim_dec_parse_import_priv(const struct obmm_region *region,
					  u64 *remote_uba, u32 *token_value,
					  u64 *local_va, u64 *home_va,
					  u64 *pte_offset, u32 *vmid, u32 *asid,
					  u32 *tid, u32 *p_tag, u32 *cache_policy,
					  u32 *map_source, u32 *address_profile,
					  u32 *access_flags, u64 *gva_id)
{
	const struct obmm_sim_dec_import_priv_v2 *priv_v2;
	const struct obmm_sim_dec_import_priv_v1 *priv;

	*remote_uba = 0;
	*token_value = 0;
	*local_va = 0;
	*home_va = 0;
	*pte_offset = 0;
	*vmid = 0;
	*asid = 0;
	*tid = 0;
	*p_tag = 0;
	*cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
	*map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
	*address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
	*access_flags = 0;
	*gva_id = 0;

	if (region->priv_len < sizeof(*priv))
		return;

	priv = (const struct obmm_sim_dec_import_priv_v1 *)region->priv;
	if (priv->magic != OBMM_SIM_DEC_PRIV_MAGIC || priv->len < sizeof(*priv))
		return;

	if (priv->version == OBMM_SIM_DEC_PRIV_VER_1) {
		*remote_uba = priv->remote_uba;
		*token_value = priv->token_value;
		return;
	}

	if (priv->version == OBMM_SIM_DEC_PRIV_VER_2 &&
	    region->priv_len >= sizeof(*priv_v2) &&
	    priv->len >= sizeof(*priv_v2)) {
		priv_v2 = (const struct obmm_sim_dec_import_priv_v2 *)region->priv;
		*remote_uba = priv_v2->remote_uba;
		*token_value = priv_v2->token_value;
		*local_va = priv_v2->local_va;
		*home_va = priv_v2->home_va;
		*pte_offset = priv_v2->pte_offset;
		*vmid = priv_v2->vmid;
		*asid = priv_v2->asid;
		*tid = priv_v2->tid;
		*p_tag = priv_v2->p_tag;
		*cache_policy = priv_v2->cache_policy;
		*map_source = priv_v2->map_source;
		*address_profile = priv_v2->address_profile;
		*access_flags = priv_v2->access_flags;
		*gva_id = priv_v2->gva_id;
	}
}

static int obmm_sim_dec_map_import(struct obmm_import_region *i_reg)
{
	struct obmm_sim_dec_import_info info = { 0 };
	int (*cb)(void *) = NULL;
	u64 remote_uba;
	u32 token_value;
	u64 local_va = 0;
	u64 home_va = 0;
	u64 pte_offset = 0;
	u32 vmid = 0;
	u32 asid = 0;
	u32 tid = 0;
	u32 p_tag = 0;
	u32 cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
	u32 map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
	u32 address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
	u32 access_flags = 0;
	u64 gva_id = 0;
	int ret;

	mutex_lock(&g_obmm_sim_dec_cb_lock);
	cb = g_obmm_import_cb;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	if (!cb)
		return 0;

	obmm_sim_dec_parse_import_priv(&i_reg->region, &remote_uba, &token_value,
				      &local_va, &home_va, &pte_offset, &vmid,
				      &asid, &tid, &p_tag, &cache_policy,
				      &map_source, &address_profile, &access_flags,
				      &gva_id);
	if (!remote_uba) {
		pr_err("sim decoder map requires remote_uba in import priv.\n");
		return -EINVAL;
	}
	if (address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY &&
	    (pte_offset != 0 || local_va != home_va ||
	     local_va != remote_uba)) {
		pr_err("GSVA identity mapping requires local_va/home_va/remote_uba be equal and pte_offset 0: "
		       "local_va=%#llx home_va=%#llx remote_uba=%#llx pte_offset=%#llx\n",
		       local_va, home_va, remote_uba, pte_offset);
		return -EINVAL;
	}

	info.local_pa = i_reg->pa;
	info.size = i_reg->region.mem_size;
	info.remote_uba = remote_uba;
	info.token_id = i_reg->tokenid;
	info.token_value = token_value;
	info.scna = i_reg->scna;
	info.dcna = i_reg->dcna;
	memcpy(info.seid, i_reg->seid, sizeof(info.seid));
	memcpy(info.deid, i_reg->deid, sizeof(info.deid));
	info.upi = 0;
	info.src_eid = 0;
	info.local_va = local_va;
	info.home_va = home_va;
	info.pte_offset = pte_offset;
	info.vmid = vmid;
	info.asid = asid;
	info.tid = tid;
	info.p_tag = p_tag;
	info.cache_policy = cache_policy;
	info.map_source = map_source;
	info.address_profile = address_profile;
	info.access_flags = access_flags;
	info.gva_id = gva_id;

	ret = cb(&info);
	if (ret) {
		pr_err("sim decoder map callback failed: %pe.\n", ERR_PTR(ret));
		return ret;
	}

	i_reg->sim_dec_map_id = info.map_id;
	i_reg->sim_dec_mapped = true;
	return 0;
}

static int obmm_sim_dec_unmap_import(struct obmm_import_region *i_reg)
{
	struct obmm_sim_dec_unimport_info info;
	int (*cb)(void *) = NULL;
	int ret;

	if (!i_reg->sim_dec_mapped)
		return 0;

	mutex_lock(&g_obmm_sim_dec_cb_lock);
	cb = g_obmm_unimport_cb;
	mutex_unlock(&g_obmm_sim_dec_cb_lock);
	if (!cb)
		return 0;

	info.map_id = i_reg->sim_dec_map_id;
	info.scna = i_reg->scna;
	ret = cb(&info);
	if (ret) {
		pr_err("sim decoder unmap callback failed map_id=%#llx ret=%pe.\n",
		       info.map_id, ERR_PTR(ret));
		return ret;
	}

	i_reg->sim_dec_mapped = false;
	i_reg->sim_dec_map_id = 0;
	return 0;
}

static void set_import_region_datapath(const struct obmm_import_region *i_reg,
				       struct obmm_datapath *datapath)
{
	datapath->scna = i_reg->scna;
	datapath->dcna = i_reg->dcna;
	/* shallow copy */
	datapath->seid = i_reg->seid;
	datapath->deid = i_reg->deid;
}

static unsigned long get_pa_range_mem_cap(u32 scna, phys_addr_t pa, size_t size)
{
	phys_addr_t pa_start = pa;
	phys_addr_t pa_end = pa + size - 1;
	unsigned long mem_cap = 0;

	if (ub_memory_validate_pa(scna, pa_start, pa_end, true))
		mem_cap |= OBMM_MEM_ALLOW_CACHEABLE_MMAP;
	if (ub_memory_validate_pa(scna, pa_start, pa_end, false))
		mem_cap |= OBMM_MEM_ALLOW_NONCACHEABLE_MMAP;
	if (mem_cap == 0)
		pr_err("PA range invalid. Non-UBMEM memory cannot be mmaped as import memory\n");

	return mem_cap;
}

static int setup_pa(struct obmm_import_region *i_reg)
{
	int ret;
	phys_addr_t start, end;
	struct obmm_datapath datapath;

	i_reg->region.mem_cap =
		get_pa_range_mem_cap(i_reg->scna, i_reg->pa, i_reg->region.mem_size);
	if (i_reg->region.mem_cap == 0)
		return -EINVAL;

	if (!region_preimport(&i_reg->region)) {
		struct ubmem_resource *ubmem_res;

		ubmem_res = setup_ubmem_resource(i_reg->pa, i_reg->region.mem_size, false);
		if (IS_ERR(ubmem_res)) {
			pr_err("failed to setup ubmem resource: ret=%pe\n", ubmem_res);
			return PTR_ERR(ubmem_res);
		}
		i_reg->ubmem_res = ubmem_res;

		return 0;
	}

	start = i_reg->pa;
	end = i_reg->pa + i_reg->region.mem_size - 1;
	set_import_region_datapath(i_reg, &datapath);

	ret = preimport_commit_prefilled(start, end, &datapath, &i_reg->numa_id,
					  &i_reg->preimport_handle);
	if (ret)
		return ret;

	i_reg->ubmem_res = preimport_get_resource_prefilled(i_reg->preimport_handle);

	return 0;
}

/* NOTE: do not clear PA in the teardown process. Error rollback procedure may rely on it. */
static int teardown_pa(struct obmm_import_region *i_reg)
{
	bool preimport = region_preimport(&i_reg->region);

	if (!preimport)
		return release_ubmem_resource(i_reg->ubmem_res);
	/* prefilled and preimport */
	return preimport_uncommit_prefilled(i_reg->preimport_handle, i_reg->pa,
					    i_reg->pa + i_reg->region.mem_size - 1);
}

static int teardown_remote_numa(struct obmm_import_region *i_reg, bool force)
{
	int ret, this_ret;

	ret = lock_save_memdev_descendents(i_reg->ubmem_res);
	if (ret)
		return ret;

	pr_debug("call external: remove_memory_remote(nid=%d, size=%#llx)\n",
		i_reg->numa_id, i_reg->region.mem_size);
	ret = remove_memory_remote(i_reg->numa_id, i_reg->pa, i_reg->region.mem_size);
	pr_debug("external called: remove_memory_remote, ret=%pe\n", ERR_PTR(ret));
	/* a full rollback is still possible: check whether this is a full teardown */
	if (ret != 0 && !force) {
		pr_err("remove_memory_remote(nid=%d, size=%#llx) failed: ret=%pe.\n",
		       i_reg->numa_id, i_reg->region.mem_size, ERR_PTR(ret));
		goto out_recover_resource;
	}

	if (region_preimport(&i_reg->region)) {
		pr_debug("call external: add_memory_remote(nid=%d, size=0x%llx, flags=MEMORY_KEEP_ISOLATED)\n",
			i_reg->numa_id, i_reg->region.mem_size);
		this_ret = add_memory_remote(i_reg->numa_id, i_reg->pa, i_reg->region.mem_size,
					     MEMORY_KEEP_ISOLATED);
		pr_debug("external called: add_memory_remote() returned %d\n", this_ret);
		if (this_ret == NUMA_NO_NODE) {
			pr_err("failed to reset preimport memory.\n");
			ret = -ENOTRECOVERABLE;
		}
	}

out_recover_resource:
	restore_unlock_memdev_descendents(i_reg->ubmem_res);
	return ret;
}

static int setup_remote_numa(struct obmm_import_region *i_reg)
{
	int ret, flags;

	if (region_preimport(&i_reg->region))
		flags = 0;
	else
		flags = MEMORY_DIRECT_ONLINE;

	if (!(i_reg->region.mem_cap & OBMM_MEM_ALLOW_CACHEABLE_MMAP)) {
		pr_err("PA range invalid. Cacheable memory cannot be managed with numa.remote\n");
		return -EINVAL;
	}

	pr_debug("call external: add_memory_remote(nid=%d, flags=%d)\n",
		i_reg->numa_id, flags);
	ret = add_memory_remote(i_reg->numa_id, i_reg->pa, i_reg->region.mem_size, flags);
	pr_debug("external called: add_memory_remote() returned %d\n", ret);
	if (ret < 0) {
		pr_err("Remote NUMA creation failed: %d\n", ret);
		return -EPERM;
	}
	WARN_ON(i_reg->numa_id != NUMA_NO_NODE && i_reg->numa_id != ret);
	i_reg->numa_id = ret;

	if (!region_preimport(&i_reg->region)) {
		ret = obmm_set_numa_distance(i_reg->scna, i_reg->numa_id, i_reg->base_dist);
		if (ret < 0) {
			pr_err("Failed to set remote numa distance: %pe\n", ERR_PTR(ret));
			goto out_teardown_remote_numa;
		}
	}

	return 0;
out_teardown_remote_numa:
	WARN_ON(teardown_remote_numa(i_reg, true));
	return ret;
}

static inline int occupy_addr_range(const struct obmm_import_region *i_reg)
{
	struct obmm_pa_range pa;

	if (!region_preimport(&i_reg->region)) {
		pa.start = i_reg->pa;
		pa.end = i_reg->pa + i_reg->region.mem_size - 1;
		pa.info.user = OBMM_ADDR_USER_DIRECT_IMPORT;
		pa.info.data = (void *)i_reg;
		return occupy_pa_range(&pa);
	}

	/* preimport + decoder_prefilled: address conflicts managed by its perimport range */
	return 0;
}

static int free_addr_range(const struct obmm_import_region *i_reg)
{
	struct obmm_pa_range pa;

	if (!region_preimport(&i_reg->region)) {
		pa.start = i_reg->pa;
		pa.end = i_reg->pa + i_reg->region.mem_size - 1;
		return free_pa_range(&pa);
	}

	/* preimport + decoder_prefilled: address conflicts managed by its perimport range */
	return 0;
}

static int setup_iomem_resource(struct obmm_import_region *i_reg)
{
	struct resource *memdev_res;

	memdev_res = setup_memdev_resource(i_reg->ubmem_res, i_reg->pa,
					   i_reg->region.mem_size, i_reg->region.regionid);
	if (IS_ERR(memdev_res)) {
		pr_err("memid=%d: failed to setup memdev resource: %pe\n",
		       i_reg->region.regionid, memdev_res);
		return PTR_ERR(memdev_res);
	}

	i_reg->memdev_res = memdev_res;

	return 0;
}

static int teardown_iomem_resource(struct obmm_import_region *i_reg)
{
	int ret;

	ret = release_memdev_resource(i_reg->ubmem_res, i_reg->memdev_res);
	if (ret)
		pr_err("memid=%d: failed to release memdev resource: %pe\n",
		       i_reg->region.regionid, ERR_PTR(ret));

	return ret;
}

static int prepare_import_memory(struct obmm_import_region *i_reg)
{
	int ret, rollback_ret;

	if (region_numa_remote(&i_reg->region)) {
		if (!validate_scna(i_reg->scna))
			return -ENODEV;
	} else {
		if (!validate_scna_registered(i_reg->scna))
			return -ENODEV;
	}

	ret = occupy_addr_range(i_reg);
	if (ret)
		return ret;

	ret = setup_pa(i_reg);
	if (ret)
		goto out_free_addr_range;

	/* register numa node */
	if (region_numa_remote(&i_reg->region)) {
		ret = setup_remote_numa(i_reg);
		if (ret)
			goto out_teardown_pa;
	} else {
		i_reg->numa_id = NUMA_NO_NODE;
	}

	ret = setup_iomem_resource(i_reg);
	if (ret)
		goto out_teardown_numa;

	return 0;
out_teardown_numa:
	if (region_numa_remote(&i_reg->region)) {
		rollback_ret = teardown_remote_numa(i_reg, true);
		if (rollback_ret) {
			pr_err("failed to teardown remote numa on rollback, ret=%pe.\n",
			       ERR_PTR(rollback_ret));
			ret = -ENOTRECOVERABLE;
		}
	}
out_teardown_pa:
	rollback_ret = teardown_pa(i_reg);
	if (rollback_ret) {
		pr_err("failed to teardown PA level mapping on rollback, ret=%pe.\n",
		       ERR_PTR(rollback_ret));
		ret = -ENOTRECOVERABLE;
	}
out_free_addr_range:
	rollback_ret = free_addr_range(i_reg);
	if (rollback_ret) {
		pr_err("failed to free address range on rollback, ret=%pe.\n",
		       ERR_PTR(rollback_ret));
		ret = -ENOTRECOVERABLE;
	}
	return ret;
}

static int release_import_memory(struct obmm_import_region *i_reg)
{
	int ret, rollback_ret, old_numa_id;

	ret = obmm_sim_dec_unmap_import(i_reg);
	if (ret)
		return ret;

	ret = teardown_iomem_resource(i_reg);
	if (ret)
		return ret;

	if (region_numa_remote(&i_reg->region)) {
		old_numa_id = i_reg->numa_id;
		ret = teardown_remote_numa(i_reg, false);
		if (ret)
			goto err_teardown_numa;
	}

	ret = flush_import_region(i_reg, 0, i_reg->region.mem_size, OBMM_SHM_CACHE_INVAL);
	if (ret) {
		pr_err("failed to flush import region, ret=%pe.\n", ERR_PTR(ret));
		goto err_flush;
	}

	/* unplug memory */
	ret = teardown_pa(i_reg);
	if (ret) {
		pr_err("failed to release PA level mapping of region %d, ret=%pe.\n",
		       i_reg->region.regionid, ERR_PTR(ret));
		goto err_flush;
	}

	ret = free_addr_range(i_reg);
	if (ret)
		goto err_free_addr_range;

	return 0;

err_free_addr_range:
	rollback_ret = setup_pa(i_reg);
	if (rollback_ret) {
		pr_err("failed to restore PA level mapping, ret=%pe.\n", ERR_PTR(rollback_ret));
		return -ENOTRECOVERABLE; /* rollback cannot proceed */
	}
err_flush:
	if (region_numa_remote(&i_reg->region)) {
		i_reg->numa_id = old_numa_id;

		rollback_ret = setup_remote_numa(i_reg);
		if (rollback_ret) {
			pr_err("failed to restore remote NUMA, ret=%pe.\n", ERR_PTR(rollback_ret));
			return -ENOTRECOVERABLE; /* rollback cannot proceed */
		}
	}
err_teardown_numa:
	rollback_ret = setup_iomem_resource(i_reg);
	if (rollback_ret) {
		pr_err("failed to restore iomem resource on rollback, ret=%pe.\n",
		       ERR_PTR(rollback_ret));
		return -ENOTRECOVERABLE;
	}
	rollback_ret = obmm_sim_dec_map_import(i_reg);
	if (rollback_ret) {
		pr_err("failed to restore sim decoder map on rollback, ret=%pe.\n",
		       ERR_PTR(rollback_ret));
		return -ENOTRECOVERABLE;
	}
	return ret;
}

static bool validate_pa_range(phys_addr_t pa, size_t size)
{
	/* the PA alignment of OBMM_BASIC_GRANU might be an overkill if PAGE_SIZE is not 4K. But
	 * this is not be a common use case for now.
	 */
	if (!IS_ALIGNED(pa, OBMM_BASIC_GRANU) || !IS_ALIGNED(size, OBMM_BASIC_GRANU)) {
		pr_err("PA segments not aligned to OBMM basic granu: base=%#llx, size=%#zx, granularity=%#lx.\n",
		       pa, size, OBMM_BASIC_GRANU);
		return false;
	}

	if (pa == 0) {
		pr_err("PA=0 unexpected.\n");
		return false;
	}
	if (pa + size < pa) {
		pr_err("PA range overflow: base=%#llx, size=%#zx.\n", pa, size);
		return false;
	}

	return true;
}

static bool validate_import_region(const struct obmm_import_region *i_reg)
{
	bool preimport;

	/* size and alignment check */
	if (i_reg->region.mem_size == 0) {
		pr_err("Zero memory segment size is invalid\n");
		return false;
	}

	preimport = region_preimport(&i_reg->region);
	/* PA as parameter */
	if (!validate_pa_range(i_reg->pa, i_reg->region.mem_size))
		return false;
	return true;
}

static int import_to_region_flags(unsigned long *region_flags, unsigned long import_flags)
{
	*region_flags = 0;

	if (import_flags & (~OBMM_IMPORT_FLAG_MASK)) {
		pr_err("Invalid import flags %#lx (unknown flags: %#lx).\n", import_flags,
		       import_flags & (~OBMM_IMPORT_FLAG_MASK));
		return -EINVAL;
	}
	if (!!(import_flags & OBMM_IMPORT_FLAG_ALLOW_MMAP) +
	    !!(import_flags & OBMM_IMPORT_FLAG_NUMA_REMOTE) != 1) {
		pr_err("Exactly one of {ALLOW_MMAP, NUMA_REMOTE} must be specified as import flag.\n");
		return -EINVAL;
	}
	if ((import_flags & OBMM_IMPORT_FLAG_PREIMPORT) &&
	    !(import_flags & OBMM_IMPORT_FLAG_NUMA_REMOTE)) {
		pr_err("Preimport must be used with NUMA_REMOTE.\n");
		return -EINVAL;
	}

	if (import_flags & OBMM_IMPORT_FLAG_ALLOW_MMAP)
		*region_flags |= OBMM_REGION_FLAG_ALLOW_MMAP;
	if (import_flags & OBMM_IMPORT_FLAG_PREIMPORT)
		*region_flags |= OBMM_REGION_FLAG_PREIMPORT;
	if (import_flags & OBMM_IMPORT_FLAG_NUMA_REMOTE)
		*region_flags |= OBMM_REGION_FLAG_NUMA_REMOTE;

	return 0;
}

static int init_import_region_from_cmd(const struct obmm_cmd_import *param,
				       struct obmm_import_region *i_reg)
{
	int ret;
	bool config_numa_dist;
	struct obmm_region *region = &i_reg->region;

	i_reg->region.type = OBMM_IMPORT_REGION;
	i_reg->region.mem_size = param->length;
	/* set flags */
	ret = import_to_region_flags(&region->flags, param->flags);
	if (ret)
		return ret;

	i_reg->pa = param->addr;
	i_reg->tokenid = param->tokenid;
	i_reg->dcna = param->dcna;
	i_reg->scna = param->scna;
	memcpy(i_reg->deid, param->deid, sizeof(i_reg->deid));
	memcpy(i_reg->seid, param->seid, sizeof(i_reg->seid));
	i_reg->numa_id = region_numa_remote(&i_reg->region) ? param->numa_id : NUMA_NO_NODE;

	ret = set_obmm_region_priv(region, param->priv_len, param->priv);
	if (ret)
		return ret;

	if (param->priv_len >= sizeof(struct obmm_sim_dec_import_priv_v2)) {
		const struct obmm_sim_dec_import_priv_v2 *priv_v2 =
			(const struct obmm_sim_dec_import_priv_v2 *)region->priv;

		if (priv_v2->magic == OBMM_SIM_DEC_PRIV_MAGIC &&
		    priv_v2->version == OBMM_SIM_DEC_PRIV_VER_2 &&
		    priv_v2->len >= sizeof(*priv_v2) &&
		    priv_v2->address_profile ==
			    OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY) {
			if (priv_v2->pte_offset != 0 ||
			    priv_v2->local_va != priv_v2->home_va ||
			    priv_v2->local_va != priv_v2->remote_uba ||
			    !obmm_gsva_aperture_contains(priv_v2->remote_uba,
							 param->length)) {
				pr_err("GSVA identity import outside active aperture or not identity: "
				       "local_va=%#llx home_va=%#llx remote_uba=%#llx "
				       "pte_offset=%#llx length=%#llx\n",
				       priv_v2->local_va, priv_v2->home_va,
				       priv_v2->remote_uba, priv_v2->pte_offset,
				       param->length);
				return -EINVAL;
			}
			region->flags |= OBMM_REGION_FLAG_GSVA_SEGMENT;
		}
	}

	if (!validate_import_region(i_reg))
		return -EINVAL;

	config_numa_dist = region_numa_remote(&i_reg->region) && !region_preimport(&i_reg->region);
	if (config_numa_dist && !is_numa_base_dist_valid(param->base_dist))
		return -EINVAL;
	i_reg->base_dist = param->base_dist;
	i_reg->sim_dec_mapped = false;
	i_reg->sim_dec_map_id = 0;

	/* NOTE: this function initializes the data structure but not the device */
	return 0;
}

static void print_import_param(const struct obmm_cmd_import *cmd_import)
{
	pr_debug("obmm_import: scna=%#x {pa=%#llx length=%#llx} flags=%#llx nid=%d base_dist=%u seid="
		EID_FMT64 " priv_len=%u\n",
		cmd_import->scna, cmd_import->addr, cmd_import->length, cmd_import->flags,
		cmd_import->numa_id, cmd_import->base_dist, EID_ARGS64_H(cmd_import->seid),
		EID_ARGS64_L(cmd_import->seid), cmd_import->priv_len);
}

int obmm_import(struct obmm_cmd_import *cmd_import)
{
	int retval, rollback_ret, numa_id;
	struct obmm_import_region *i_reg;
	uint64_t mem_id;

	print_import_param(cmd_import);
	/* create obmm region */
	i_reg = kzalloc(sizeof(struct obmm_import_region), GFP_KERNEL);
	if (i_reg == NULL)
		return -ENOMEM;

	atomic_set(&i_reg->region.device_released, 1);

	/* arguments to region (logs produced by callee) */
	retval = init_import_region_from_cmd(cmd_import, i_reg);
	if (retval)
		goto out_free_ireg;

	retval = init_obmm_region(&i_reg->region);
	if (retval)
		goto out_free_ireg;

	retval = prepare_import_memory(i_reg);
	if (retval) {
		pr_err("Failed to prepare import memory: ret=%pe\n", ERR_PTR(retval));
		goto out_region_uninit;
	}

	retval = obmm_sim_dec_map_import(i_reg);
	if (retval) {
		pr_err("Failed to map sim decoder: ret=%pe\n", ERR_PTR(retval));
		goto out_release_memory;
	}

	numa_id = i_reg->numa_id;
	mem_id = (uint64_t)i_reg->region.regionid;

	retval = register_obmm_region(&i_reg->region);
	if (retval) {
		pr_err("Failed to create import device. ret=%pe\n", ERR_PTR(retval));
		goto out_release_memory;
	}
	activate_obmm_region(&i_reg->region);

	/* pass back output value */
	cmd_import->numa_id = numa_id;
	cmd_import->mem_id = mem_id;

	pr_debug("%s: mem_id=%llu online\n", __func__, cmd_import->mem_id);
	return 0;

out_release_memory:
	rollback_ret = obmm_sim_dec_unmap_import(i_reg);
	if (rollback_ret)
		pr_warn("Failed to unmap sim decoder on rollback, ret=%pe.\n",
			ERR_PTR(rollback_ret));
	rollback_ret = release_import_memory(i_reg);
	if (rollback_ret)
		pr_warn("Failed to release import memory on rollback, ret=%pe.\n",
			ERR_PTR(rollback_ret));
out_region_uninit:
	uninit_obmm_region(&i_reg->region);
out_free_ireg:
	wait_until_dev_released(&i_reg->region);
	kfree(i_reg);
	return retval;
}

/* NOTE: the operation order is not precisely the reverse order of initialization for the ease of
 * error rollback. Please make careful evaluation on modifications.
 */
int obmm_unimport(const struct obmm_cmd_unimport *cmd_unimport)
{
	int ret;
	struct obmm_region *reg;
	struct obmm_import_region *i_reg;

	pr_debug("%s: mem_id=%llu, flags=%#llx.\n", __func__, cmd_unimport->mem_id,
		cmd_unimport->flags);
	if (!validate_obmm_mem_id(cmd_unimport->mem_id))
		return -ENOENT;
	if (cmd_unimport->flags & (~OBMM_UNIMPORT_FLAG_MASK)) {
		pr_err("%s: invalid flags %#llx.\n", __func__, cmd_unimport->flags);
		return -EINVAL;
	}

	reg = search_deactivate_obmm_region(cmd_unimport->mem_id);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	if (reg->type != OBMM_IMPORT_REGION) {
		pr_err("%s: mem_id=%llu region type mismatched.\n", __func__, cmd_unimport->mem_id);
		ret = -EINVAL;
		goto err_unimport;
	}
	i_reg = container_of(reg, struct obmm_import_region, region);
	ret = release_import_memory(i_reg);
	if (ret)
		goto err_unimport;

	deregister_obmm_region(reg);
	uninit_obmm_region(reg);
	wait_until_dev_released(&i_reg->region);
	kfree(i_reg);

	pr_debug("%s: mem_id=%llu completed.\n", __func__, cmd_unimport->mem_id);
	return 0;

err_unimport:
	activate_obmm_region(reg);
	pr_err("%s: mem_id=%llu failed, %pe.\n", __func__, cmd_unimport->mem_id, ERR_PTR(ret));
	return ret;
}

int flush_import_region(struct obmm_import_region *i_reg, unsigned long offset,
			unsigned long length, unsigned long cache_ops)
{
	int ret;

	ret = flush_cache_by_pa(i_reg->pa + offset, length, cache_ops);
	if (ret)
		return ret;

	if (cache_ops == OBMM_SHM_CACHE_WB_INVAL || cache_ops == OBMM_SHM_CACHE_WB_ONLY)
		return ub_write_queue_flush(i_reg->scna);
	return 0;
}

int map_import_region(struct vm_area_struct *vma, struct obmm_import_region *i_reg,
		      enum obmm_mmap_granu mmap_granu)
{
	unsigned long pfn, size;

	size = vma->vm_end - vma->vm_start;
	pfn = __phys_to_pfn(i_reg->pa) + vma->vm_pgoff;
	if (mmap_granu == OBMM_MMAP_GRANU_PAGE)
		return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
	else if (mmap_granu == OBMM_MMAP_GRANU_PMD)
		return remap_pfn_range_try_pmd(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
	pr_err("invalid mmap granu %d\n", mmap_granu);

	return -EINVAL;
}

int get_pa_detail_import(const struct obmm_import_region *i_reg, unsigned long pa,
			 struct obmm_ext_addr *ext_addr)
{
	if (pa < i_reg->pa || pa >= i_reg->pa + i_reg->region.mem_size)
		return -EFAULT;

	ext_addr->region_type = OBMM_IMPORT_REGION;
	ext_addr->regionid = i_reg->region.regionid;
	ext_addr->offset = pa - i_reg->pa;
	ext_addr->tid = 0;
	ext_addr->uba = 0;
	ext_addr->numa_id = i_reg->numa_id;
	ext_addr->pa = pa;

	return 0;
}

int get_offset_detail_import(const struct obmm_import_region *i_reg, unsigned long offset,
			     struct obmm_ext_addr *ext_addr)
{
	if (offset >= i_reg->region.mem_size) {
		pr_err("%s: invalid offset 0x%lx\n", __func__, offset);
		return -EINVAL;
	}

	ext_addr->region_type = i_reg->region.type;
	ext_addr->regionid = i_reg->region.regionid;
	ext_addr->offset = offset;
	ext_addr->tid = 0;
	ext_addr->uba = 0;
	ext_addr->pa = i_reg->pa + offset;
	ext_addr->numa_id = i_reg->numa_id;

	return 0;
}
