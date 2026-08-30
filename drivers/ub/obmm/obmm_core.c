// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2025. All rights reserved.
 * Description：OBMM Framework's implementations.
 */

#include <linux/align.h>
#include <linux/miscdevice.h>
#include <linux/err.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/rwlock.h>
#include <linux/idr.h>
#include <linux/acpi.h>
#include <linux/overflow.h>

#include <ub/ubus/ub-mem-decoder.h>
#include <ub/ubus/ubus.h>

#include "obmm_shm_dev.h"
#include "obmm_cache.h"
#include "obmm_export_region_ops.h"
#include "ubmempool_allocator.h"
#include "../ubus/sim/ub_sim_decoder.h"
#include "obmm_import.h"
#include "obmm_ownership.h"
#include "obmm_lowmem.h"
#include "obmm_preimport.h"
#include "obmm_addr_check.h"
#include "obmm_sysfs.h"
#include "obmm_export.h"
#include "obmm_core.h"
#include <uapi/ub/gsva.h>
#include "../ubus/ubus_controller.h"
#include "../ubus/sim/ub_sim_decoder.h"

size_t __obmm_memseg_size;
static DEFINE_MUTEX(obmm_gsva_aperture_lock);
static struct obmm_cmd_gsva_aperture obmm_gsva_aperture;

bool obmm_gsva_aperture_overlaps(unsigned long start, unsigned long end)
{
	u64 base, size, aperture_end;
	bool overlaps = false;

	if (start >= end)
		return false;

	mutex_lock(&obmm_gsva_aperture_lock);
	if (!(obmm_gsva_aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE))
		goto out;
	base = obmm_gsva_aperture.base;
	size = obmm_gsva_aperture.size;
	if (check_add_overflow(base, size, &aperture_end))
		goto out;
	overlaps = (u64)start < aperture_end && (u64)end > base;
out:
	mutex_unlock(&obmm_gsva_aperture_lock);
	return overlaps;
}

bool obmm_gsva_aperture_contains(u64 base, u64 size)
{
	u64 end, aperture_base, aperture_end;
	bool contains = false;

	if (!size || check_add_overflow(base, size, &end))
		return false;

	mutex_lock(&obmm_gsva_aperture_lock);
	if (!(obmm_gsva_aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE))
		goto out;
	aperture_base = obmm_gsva_aperture.base;
	if (check_add_overflow(aperture_base, obmm_gsva_aperture.size,
			       &aperture_end))
		goto out;
	contains = base >= aperture_base && end <= aperture_end;
out:
	mutex_unlock(&obmm_gsva_aperture_lock);
	return contains;
}

void obmm_gsva_aperture_snapshot(struct obmm_cmd_gsva_aperture *cmd)
{
	if (!cmd)
		return;

	mutex_lock(&obmm_gsva_aperture_lock);
	*cmd = obmm_gsva_aperture;
	mutex_unlock(&obmm_gsva_aperture_lock);
}

static int obmm_gsva_validate_aperture(const struct obmm_cmd_gsva_aperture *cmd)
{
	u64 end;

	if (!cmd->size || !PAGE_ALIGNED(cmd->base) || !PAGE_ALIGNED(cmd->size))
		return -EINVAL;
	if (!cmd->node_count || cmd->node_id >= cmd->node_count ||
	    cmd->node_count > OBMM_BOOTSTRAP_MAX_NODES)
		return -EINVAL;
	if (check_add_overflow(cmd->base, cmd->size, &end))
		return -EINVAL;
	return 0;
}

static int obmm_gsva_aperture_register(const struct obmm_cmd_gsva_aperture *cmd)
{
	int ret;

	ret = obmm_gsva_validate_aperture(cmd);
	if (ret)
		return ret;
	ret = gsva_reserved_aperture_register((unsigned long)cmd->base,
					      (unsigned long)cmd->size,
					      cmd->generation);
	if (ret)
		return ret;

	mutex_lock(&obmm_gsva_aperture_lock);
	if ((obmm_gsva_aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE) &&
	    (obmm_gsva_aperture.base != cmd->base ||
	     obmm_gsva_aperture.size != cmd->size ||
	     obmm_gsva_aperture.generation != cmd->generation)) {
		mutex_unlock(&obmm_gsva_aperture_lock);
		(void)gsva_reserved_aperture_clear(cmd->generation);
		return -EBUSY;
	}

	obmm_gsva_aperture = *cmd;
	obmm_gsva_aperture.flags |= OBMM_GSVA_APERTURE_F_ACTIVE;
	mutex_unlock(&obmm_gsva_aperture_lock);

	pr_info("GSVA aperture registered base=%#llx size=%#llx generation=%#llx node=%u/%u\n",
		cmd->base, cmd->size, cmd->generation, cmd->node_id,
		cmd->node_count);
	return 0;
}

static void obmm_gsva_aperture_query(struct obmm_cmd_gsva_aperture *cmd)
{
	obmm_gsva_aperture_snapshot(cmd);
}

static int obmm_gsva_aperture_clear(const struct obmm_cmd_gsva_aperture *cmd)
{
	int ret;

	mutex_lock(&obmm_gsva_aperture_lock);
	if ((obmm_gsva_aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE) &&
	    cmd->generation && obmm_gsva_aperture.generation != cmd->generation) {
		mutex_unlock(&obmm_gsva_aperture_lock);
		return -EINVAL;
	}
	ret = gsva_reserved_aperture_clear(cmd->generation);
	if (ret) {
		mutex_unlock(&obmm_gsva_aperture_lock);
		return ret;
	}
	memset(&obmm_gsva_aperture, 0, sizeof(obmm_gsva_aperture));
	mutex_unlock(&obmm_gsva_aperture_lock);
	return 0;
}

/* GSVA segment tracking */
struct obmm_gsva_segment {
	struct obmm_gsva_segment_desc_v1 desc;
	struct list_head node;
};
static DEFINE_MUTEX(obmm_gsva_segment_lock);
static LIST_HEAD(obmm_gsva_segments);
static u64 obmm_gsva_segment_counter;

static u32 obmm_gsva_next_token_id(void)
{
	static atomic_t counter = ATOMIC_INIT(1);
	return (u32)atomic_inc_return(&counter);
}

static u32 obmm_gsva_next_token_value(void)
{
	static atomic_t counter = ATOMIC_INIT(1);
	return (u32)atomic_inc_return(&counter);
}

static struct obmm_gsva_segment *
obmm_gsva_find_segment_by_id(u64 segment_id)
{
	struct obmm_gsva_segment *seg;

	list_for_each_entry(seg, &obmm_gsva_segments, node) {
		if (seg->desc.segment_id == segment_id &&
		    !(seg->desc.flags & OBMM_GSVA_SEG_F_RETIRED))
			return seg;
	}
	return NULL;
}

static struct obmm_gsva_segment *
obmm_gsva_find_segment_by_va(u64 home_va)
{
	struct obmm_gsva_segment *seg;

	list_for_each_entry(seg, &obmm_gsva_segments, node) {
		if (seg->desc.home_va == home_va &&
		    !(seg->desc.flags & OBMM_GSVA_SEG_F_RETIRED))
			return seg;
	}
	return NULL;
}

static int obmm_gsva_alloc_segment(struct obmm_cmd_gsva_alloc_segment_v1 *cmd)
{
	struct obmm_gsva_segment *seg;
	u64 home_va;
	u32 home_cna;
	struct ub_entity *ubc_ents[1] = {NULL};
	unsigned int ubc_count = 0;
	int ret;

	if (cmd->version != OBMM_GSVA_ABI_VERSION)
		return -EINVAL;
	if (cmd->size == 0 || !PAGE_ALIGNED(cmd->size))
		return -EINVAL;

	ret = ub_get_bus_controller(ubc_ents, 1, &ubc_count);
	if (ret || ubc_count == 0 || !ubc_ents[0])
		return -ENODEV;
	home_cna = ubc_ents[0]->cna;

	if (cmd->requested_home_va) {
		if (!PAGE_ALIGNED(cmd->requested_home_va))
			return -EINVAL;
		if (!obmm_gsva_aperture_contains(cmd->requested_home_va, cmd->size))
			return -EINVAL;
		home_va = cmd->requested_home_va;
	} else {
		if (!(obmm_gsva_aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE))
			return -EINVAL;
		home_va = obmm_gsva_aperture.base +
			  (obmm_gsva_segment_counter %
			   (obmm_gsva_aperture.size / cmd->size)) *
			  cmd->size;
		if (!obmm_gsva_aperture_contains(home_va, cmd->size))
			return -EINVAL;
	}

	seg = kzalloc(sizeof(*seg), GFP_KERNEL);
	if (!seg)
		return -ENOMEM;

	mutex_lock(&obmm_gsva_segment_lock);
	obmm_gsva_segment_counter++;
	seg->desc.version = OBMM_GSVA_ABI_VERSION;
	seg->desc.flags = OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
			  OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED |
			  OBMM_GSVA_SEG_F_ACTIVE;
	seg->desc.segment_id = ((u64)home_cna << 48) | obmm_gsva_segment_counter;
	seg->desc.home_va = home_va;
	seg->desc.size = PAGE_ALIGN(cmd->size);
	seg->desc.epoch = 1;
	seg->desc.home_cna = home_cna;
	seg->desc.owner_node_id = cmd->home_node_id;
	seg->desc.node_count = obmm_gsva_aperture.node_count;
	seg->desc.cache_policy = cmd->cache_policy;
	seg->desc.p_tag = (cmd->requested_p_tag != OBMM_GSVA_P_TAG_AUTO)
			  ? cmd->requested_p_tag
			  : (home_cna & 0x00ffffffu);
	seg->desc.access_flags = cmd->access_flags;
	seg->desc.token_id = obmm_gsva_next_token_id();
	seg->desc.token_value = obmm_gsva_next_token_value();

	list_add_tail(&seg->node, &obmm_gsva_segments);
	mutex_unlock(&obmm_gsva_segment_lock);

	cmd->desc = seg->desc;

	pr_info("GSVA segment allocated: segment_id=%#llx home_va=%#llx size=%#llx epoch=%llu p_tag=%u token_id=%u\n",
		seg->desc.segment_id, seg->desc.home_va, seg->desc.size,
		seg->desc.epoch, seg->desc.p_tag, seg->desc.token_id);
	return 0;
}

static int obmm_gsva_query_segment(struct obmm_cmd_gsva_query_segment_v1 *cmd)
{
	struct obmm_gsva_segment *seg;

	if (cmd->version != OBMM_GSVA_ABI_VERSION)
		return -EINVAL;

	mutex_lock(&obmm_gsva_segment_lock);
	if (cmd->segment_id)
		seg = obmm_gsva_find_segment_by_id(cmd->segment_id);
	else if (cmd->home_va)
		seg = obmm_gsva_find_segment_by_va(cmd->home_va);
	else
		seg = NULL;

	if (!seg) {
		mutex_unlock(&obmm_gsva_segment_lock);
		return -ENOENT;
	}
	cmd->desc = seg->desc;
	mutex_unlock(&obmm_gsva_segment_lock);
	return 0;
}

static int obmm_gsva_retire_segment(struct obmm_cmd_gsva_retire_segment_v1 *cmd)
{
	struct obmm_gsva_segment *seg;

	if (cmd->version != OBMM_GSVA_ABI_VERSION)
		return -EINVAL;

	mutex_lock(&obmm_gsva_segment_lock);
	seg = obmm_gsva_find_segment_by_id(cmd->segment_id);
	if (!seg) {
		mutex_unlock(&obmm_gsva_segment_lock);
		return -ENOENT;
	}
	if (seg->desc.epoch != cmd->epoch) {
		mutex_unlock(&obmm_gsva_segment_lock);
		cmd->error = GSVA_ERR_STALE_EPOCH;
		cmd->status = OBMM_GSVA_RETIRE_ABORTED;
		return -EINVAL;
	}
	if (seg->desc.flags & OBMM_GSVA_SEG_F_RETIRED) {
		mutex_unlock(&obmm_gsva_segment_lock);
		cmd->status = OBMM_GSVA_RETIRE_ABORTED;
		return -EALREADY;
	}

	seg->desc.flags &= ~OBMM_GSVA_SEG_F_ACTIVE;
	seg->desc.flags |= OBMM_GSVA_SEG_F_RETIRED;
	cmd->committed_epoch = seg->desc.epoch;
	cmd->status = OBMM_GSVA_RETIRE_COMMITTED;
	cmd->error = 0;
	mutex_unlock(&obmm_gsva_segment_lock);

	pr_info("GSVA segment retired: segment_id=%#llx epoch=%llu status=COMMITTED\n",
		cmd->segment_id, cmd->committed_epoch);
	return 0;
}

/*
 * OBMM centers around regions -- "struct obmm_region". Each region represents
 * a chunk of memory. OBMM exposes its interface to user space through the
 * device interface. Users may manipulate the memory region through ioctl to
 * master device /dev/obmm, and access each memory region through standard file
 * operations like open, close and mmap.
 *
 * To support remote memory access via UB, OBMM models two different types of
 * regions, the export region and the import region. As the name suggests, the
 * export region is physically located on this host (local), while the import
 * region is physically attached to another host (remote).
 *
 * All /dev/obmm operations are essentially region creation and deletion.
 * Currently, a linked list is used to keep track of all active regions.
 *
 * All region device (/dev/obmm_shmdev{region_id}) operations access its own
 * region only. To keep our management in accordance with Linux standard device
 * file, each device file's life cycle should be decided only by its reference
 * counts. Therefore, the master device cannot forcefully remove a region in
 * use. This complicates concurrency control and region life cycle management.
 *
 * concurrency control: when region is created, the only accessor to the region
 * is its creator, and there is no concurrency issues to worry about. The
 * concurrent access starts when we "publish" the region on the region list.
 *
 * All new accessors get the pointer to the region from the region list,
 * directly or indirectly. Most accessors merely read some region attributes.
 * Their read-only nature simplifies concurrency control, and all we need to do
 * is to guarantee that the region will not be freed by others during their
 * access. This is done by the "refcnt" reference counter. Using the conditional
 * atomic instructions, "refcnt" is also in charge of guarding against access
 * before initialization is completed, access during destruction and double-free
 * problems.
 */

static struct obmm_ctx_info g_obmm_ctx_info;
static DEFINE_IDA(g_obmm_region_ida);

/* Return the pointer to region only if the region is active: not in initialization or
 * destruction process.
 */
struct obmm_region *try_get_obmm_region(struct obmm_region *region)
{
	if (region && refcount_inc_not_zero(&region->refcnt))
		return region;
	return NULL;
}
void put_obmm_region(struct obmm_region *region)
{
	if (region)
		refcount_dec(&region->refcnt);
}
void activate_obmm_region(struct obmm_region *region)
{
	refcount_set(&region->refcnt, 1);
}
/* Return whether the disable is success. disable succeed only when the region is active and idle */
static inline bool disable_obmm_region_get(struct obmm_region *region)
{
	return refcount_dec_if_one(&region->refcnt);
}

static struct obmm_region *_search_obmm_region(int regionid)
{
	struct obmm_region *region_now;

	list_for_each_entry(region_now, &g_obmm_ctx_info.regions, node) {
		if (region_now->regionid == regionid)
			return region_now;
	}
	return NULL;
}

struct obmm_region *search_get_obmm_region(int regionid)
{
	struct obmm_region *region;
	unsigned long flags;
	spinlock_t *lock;

	lock = &g_obmm_ctx_info.lock;
	spin_lock_irqsave(lock, flags);
	region = _search_obmm_region(regionid);
	region = try_get_obmm_region(region);
	spin_unlock_irqrestore(lock, flags);

	return region;
}

struct obmm_region *search_deactivate_obmm_region(int regionid)
{
	struct obmm_region *region;
	unsigned long flags;
	spinlock_t *lock;
	bool success;

	lock = &g_obmm_ctx_info.lock;
	spin_lock_irqsave(lock, flags);
	region = _search_obmm_region(regionid);
	success = region && disable_obmm_region_get(region);
	spin_unlock_irqrestore(lock, flags);

	if (!region) {
		pr_err("failed to deactivate: region with mem_id=%d not found.\n", regionid);
		return ERR_PTR(-ENOENT);
	}

	if (!success) {
		pr_err("failed to deactivate: region %d is being used or in creation/destruction process.\n",
		       region->regionid);
		return ERR_PTR(-EBUSY);
	}

	return region;
}

int obmm_query_by_offset(struct obmm_region *reg, unsigned long offset,
			 struct obmm_ext_addr *ext_addr)
{
	int ret;
	struct obmm_export_region *e_reg;
	struct obmm_import_region *i_reg;

	if (reg->type == OBMM_EXPORT_REGION) {
		e_reg = container_of(reg, struct obmm_export_region, region);
		ret = get_offset_detail_export_region(e_reg, offset, ext_addr);
	} else {
		i_reg = container_of(reg, struct obmm_import_region, region);
		ret = get_offset_detail_import(i_reg, offset, ext_addr);
	}
	return ret;
}

int obmm_query_by_pa(unsigned long pa, struct obmm_ext_addr *ext_addr)
{
	int ret = -ENOENT;
	struct obmm_region *region;
	unsigned long flags;
	spinlock_t *lock;

	lock = &g_obmm_ctx_info.lock;

	spin_lock_irqsave(lock, flags);
	list_for_each_entry(region, &g_obmm_ctx_info.regions, node) {
		if (!try_get_obmm_region(region))
			continue;
		if (region->type == OBMM_IMPORT_REGION) {
			struct obmm_import_region *i_reg;

			i_reg = container_of(region, struct obmm_import_region, region);
			ret = get_pa_detail_import(i_reg, pa, ext_addr);
		}
		if (region->type == OBMM_EXPORT_REGION) {
			struct obmm_export_region *e_reg;

			e_reg = container_of(region, struct obmm_export_region, region);
			ret = get_pa_detail_export_region(e_reg, pa, ext_addr);
		}
		put_obmm_region(region);
		if (ret == 0)
			break;
	}
	spin_unlock_irqrestore(lock, flags);

	if (ret)
		return -ENOENT;
	return 0;
}

static int nid_to_package_id(int nid)
{
	const struct cpumask *cpumask;
	int cpu;

	/* the check guard against the dynamic online / offline of local node */
	if (!is_online_local_node(nid))
		return -1;

	/* currently we cannot handle CPU-less local memory node */
	cpumask = cpumask_of_node(nid);
	if (cpumask_empty(cpumask))
		return -1;

	cpu = (int)cpumask_first(cpumask);
	return topology_physical_package_id(cpu);
}

/* return -1 when any of the node is not online or is in different packages (sockets) */
static int get_nodes_package(const nodemask_t *nodes)
{
	int nid, package_id, this_package_id;

	package_id = -1;
	for_each_node_mask(nid, *nodes) {
		this_package_id = nid_to_package_id(nid);
		if (this_package_id == -1)
			return -1;
		if (package_id == -1)
			package_id = this_package_id;
		else if (package_id != this_package_id)
			return -1;
	}
	return package_id;
}

bool nodes_on_same_package(const nodemask_t *nodes)
{
	return get_nodes_package(nodes) != -1;
}

bool validate_scna_registered(u32 scna)
{
	struct ub_bus_controller *ubc;

	ubc = ub_find_bus_controller_by_cna(scna);
	if (!ubc) {
		pr_err("%#x is not a registered primary scna\n", scna);
		return false;
	}

	return true;
}

bool validate_scna(u32 scna)
{
	struct ub_bus_controller *ubc;
	int nid;

	ubc = ub_find_bus_controller_by_cna(scna);
	if (!ubc) {
		pr_err("%#x is not a registered primary scna\n", scna);
		return false;
	}

	nid = pxm_to_node(ubc->attr.proximity_domain);
	if (nid < 0) {
		pr_err("%#x has no local NUMA node for proximity_domain=%u\n",
		       scna, ubc->attr.proximity_domain);
		return false;
	}

	return true;
}

bool validate_obmm_mem_id(__u64 mem_id)
{
	bool valid;

	valid = mem_id >= OBMM_MIN_VALID_REGIONID && mem_id <= OBMM_MAX_VALID_REGIONID;
	if (!valid)
		pr_err("mem_id=%llu is out of valid mem_id range.\n", mem_id);
	return valid;
}

static int insert_obmm_region(struct obmm_region *reg)
{
	struct obmm_region *region_now;
	unsigned long flags;
	spinlock_t *lock;

	lock = &g_obmm_ctx_info.lock;
	spin_lock_irqsave(lock, flags);

	region_now = _search_obmm_region(reg->regionid);
	if (region_now != NULL) {
		spin_unlock_irqrestore(lock, flags);
		pr_err("obmm region already exist, mem_id = %d\n", reg->regionid);
		return -EEXIST;
	}

	list_add(&reg->node, &g_obmm_ctx_info.regions);
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

static void remove_obmm_region(struct obmm_region *reg)
{
	unsigned long flags;
	spinlock_t *lock;

	lock = &g_obmm_ctx_info.lock;

	spin_lock_irqsave(lock, flags);

	list_del(&reg->node);

	spin_unlock_irqrestore(lock, flags);
}

void uninit_obmm_region(struct obmm_region *region)
{
	if (region->ownership_info)
		release_ownership_info(region);
	ida_free(&g_obmm_region_ida, region->regionid);
	mutex_destroy(&region->state_mutex);
}

int init_obmm_region(struct obmm_region *region)
{
	int retval;

	refcount_set(&region->refcnt, 0);
	mutex_init(&region->state_mutex);
	INIT_LIST_HEAD(&region->node);

	retval = ida_alloc_range(&g_obmm_region_ida, OBMM_MIN_VALID_REGIONID,
				 OBMM_MAX_VALID_REGIONID, GFP_KERNEL);
	if (retval < 0) {
		pr_err("Failed to allocate mem_id, ret=%pe\n", ERR_PTR(retval));
		return retval;
	}
	region->regionid = retval;

	return 0;
}

int register_obmm_region(struct obmm_region *region)
{
	int retval;

	/* create device */
	retval = obmm_shm_dev_add(region);
	if (retval) {
		pr_err("Failed to create device %d. ret=%pe\n", region->regionid, ERR_PTR(retval));
		return retval;
	}

	/* insert OBMM_region */
	retval = insert_obmm_region(region);
	if (retval < 0) {
		pr_err("Failed to insert obmm region %d on creation. ret=%pe\n", region->regionid,
		       ERR_PTR(retval));
		obmm_shm_dev_del(region);
		return retval;
	}

	return 0;
}

void deregister_obmm_region(struct obmm_region *region)
{
	remove_obmm_region(region);
	obmm_shm_dev_del(region);
}

int set_obmm_region_priv(struct obmm_region *region, unsigned int priv_len, const void __user *priv)
{
	region->priv_len = 0;
	if (priv_len > OBMM_MAX_PRIV_LEN) {
		pr_err("priv_len=%u too large (limit=%u).\n", priv_len, OBMM_MAX_PRIV_LEN);
		return -EINVAL;
	}

	if (copy_from_user(region->priv, priv, priv_len)) {
		pr_err("failed to save private data.\n");
		return -EFAULT;
	}
	region->priv_len = priv_len;
	return 0;
}

static int obmm_addr_query(struct obmm_cmd_addr_query *cmd_addr_query)
{
	int ret;
	struct obmm_ext_addr ext_addr;
	struct obmm_region *region;

	if (cmd_addr_query->key_type == OBMM_QUERY_BY_PA) {
		pr_debug("obmm_query_by_pa: pa=%#llx\n", cmd_addr_query->pa);
		ret = obmm_query_by_pa(cmd_addr_query->pa, &ext_addr);
		if (ret == 0) {
			cmd_addr_query->mem_id = ext_addr.regionid;
			cmd_addr_query->offset = ext_addr.offset;
		}
		return ret;
	} else if (cmd_addr_query->key_type == OBMM_QUERY_BY_ID_OFFSET) {
		pr_debug("obmm_query_by_id_offset: mem_id=%llu offset=%#llx\n",
			 cmd_addr_query->mem_id, cmd_addr_query->offset);
		if (!validate_obmm_mem_id(cmd_addr_query->mem_id))
			return -ENOENT;
		region = search_get_obmm_region(cmd_addr_query->mem_id);
		if (region == NULL) {
			pr_err("region %llu not found.\n", cmd_addr_query->mem_id);
			return -ENOENT;
		}
		ret = obmm_query_by_offset(region, cmd_addr_query->offset, &ext_addr);
		if (ret == 0)
			cmd_addr_query->pa = ext_addr.pa;
		put_obmm_region(region);
		return ret;
	}
	pr_err("invalid query key type: %u.\n", cmd_addr_query->key_type);
	return -EINVAL;
}

static int obmm_dev_open(struct inode *inode __always_unused, struct file *file __always_unused)
{
	return 0;
}

static int obmm_dev_flush(struct file *file __always_unused, fl_owner_t owner __always_unused)
{
	return 0;
}

static void obmm_bootstrap_to_sim(
		const struct obmm_bootstrap_record *src,
		struct sim_dec_obmm_bootstrap_record *dst)
{
	dst->export_mem_id = src->export_mem_id;
	dst->remote_uba = src->remote_uba;
	dst->backing_uba = 0;
	dst->size = src->size;
	dst->generation = src->generation;
	dst->flags = src->flags;
	dst->node_id = src->node_id;
	dst->node_count = src->node_count;
	dst->export_cna = src->export_cna;
	dst->token_id = src->token_id;
}

static void obmm_bootstrap_from_sim(
		const struct sim_dec_obmm_bootstrap_record *src,
		struct obmm_bootstrap_record *dst)
{
	dst->export_mem_id = src->export_mem_id;
	dst->remote_uba = src->remote_uba;
	dst->size = src->size;
	dst->generation = src->generation;
	dst->flags = src->flags;
	dst->node_id = src->node_id;
	dst->node_count = src->node_count;
	dst->export_cna = src->export_cna;
	dst->token_id = src->token_id;
}

static long obmm_dev_ioctl(struct file *file __always_unused, unsigned int cmd, unsigned long arg)
{
	int ret;
	union {
		struct obmm_cmd_export create;
		struct obmm_cmd_import import;
		struct obmm_cmd_unexport unexport;
		struct obmm_cmd_unimport unimport;
		struct obmm_cmd_addr_query query;
		struct obmm_cmd_export_pid export_pid;
		struct obmm_cmd_preimport preimport;
		struct obmm_cmd_bootstrap_publish bootstrap_publish;
		struct obmm_cmd_bootstrap_lookup bootstrap_lookup;
		struct obmm_cmd_gsva_aperture gsva_aperture;
		struct obmm_cmd_gsva_query_v1 gsva_query;
		struct obmm_cmd_gsva_alloc_segment_v1 gsva_alloc_seg;
		struct obmm_cmd_gsva_query_segment_v1 gsva_query_seg;
		struct obmm_cmd_gsva_retire_segment_v1 gsva_retire_seg;
		struct obmm_cmd_gsva_event_v1 gsva_event;
	} cmd_param;

	switch (cmd) {
	case OBMM_CMD_EXPORT: {
		ret = (int)copy_from_user(&cmd_param.create, (void __user *)arg,
					  sizeof(struct obmm_cmd_export));
		if (ret) {
			pr_err("failed to load export argument\n");
			return -EFAULT;
		}

		ret = obmm_export_from_pool(&cmd_param.create);
		if (ret)
			return ret;

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.create,
					sizeof(struct obmm_cmd_export));
		if (ret) {
			pr_err("failed to write export result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_IMPORT: {
		ret = (int)copy_from_user(&cmd_param.import, (void __user *)arg,
					  sizeof(struct obmm_cmd_import));
		if (ret) {
			pr_err("failed to load import argument\n");
			return -EFAULT;
		}

		ret = obmm_import(&cmd_param.import);
		if (ret)
			return ret;

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.import,
					sizeof(struct obmm_cmd_import));
		if (ret) {
			pr_err("failed to write import result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_UNEXPORT: {
		ret = (int)copy_from_user(&cmd_param.unexport, (void __user *)arg,
					  sizeof(struct obmm_cmd_unexport));
		if (ret) {
			pr_err("failed to load unexport argument\n");
			return -EFAULT;
		}

		ret = obmm_unexport(&cmd_param.unexport);
	} break;
	case OBMM_CMD_UNIMPORT: {
		ret = (int)copy_from_user(&cmd_param.unimport, (void __user *)arg,
					  sizeof(struct obmm_cmd_unimport));
		if (ret) {
			pr_err("failed to load unimport argument\n");
			return -EFAULT;
		}

		ret = obmm_unimport(&cmd_param.unimport);
	} break;
	case OBMM_CMD_ADDR_QUERY: {
		ret = (int)copy_from_user(&cmd_param.query, (void __user *)arg,
					  sizeof(struct obmm_cmd_addr_query));
		if (ret) {
			pr_err("failed to load addr_query argument\n");
			return -EFAULT;
		}

		ret = obmm_addr_query(&cmd_param.query);
		if (ret)
			return ret;

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.query,
					sizeof(struct obmm_cmd_addr_query));
		if (ret) {
			pr_err("failed to write obmm_query result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_EXPORT_PID: {
		ret = (int)copy_from_user(&cmd_param.export_pid, (void __user *)arg,
					  sizeof(struct obmm_cmd_export_pid));
		if (ret) {
			pr_err("Failed to load export_pid param.\n");
			return -EFAULT;
		}

		ret = obmm_export_pid(&cmd_param.export_pid);
		if (ret)
			return ret;

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.export_pid,
					sizeof(struct obmm_cmd_export_pid));
		if (ret) {
			pr_err("failed to write export_pid result.\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_DECLARE_PREIMPORT: {
		ret = (int)copy_from_user(&cmd_param.preimport, (void __user *)arg,
					  sizeof(struct obmm_cmd_preimport));
		if (ret) {
			pr_err("failed to load preimport argument\n");
			return -EFAULT;
		}

		ret = obmm_preimport(&cmd_param.preimport);
		if (ret)
			return ret;

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.preimport,
					sizeof(struct obmm_cmd_preimport));
		if (ret) {
			pr_err("failed to write preimport result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_UNDECLARE_PREIMPORT: {
		ret = (int)copy_from_user(&cmd_param.preimport, (void __user *)arg,
					  sizeof(struct obmm_cmd_preimport));
		if (ret) {
			pr_err("failed to load preimport argument\n");
			return -EFAULT;
		}

		ret = obmm_unpreimport(&cmd_param.preimport);
	} break;
	case OBMM_CMD_BOOTSTRAP_PUBLISH: {
		struct sim_dec_obmm_bootstrap_record record = {0};
		struct obmm_region *region;

		ret = (int)copy_from_user(&cmd_param.bootstrap_publish,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_bootstrap_publish));
		if (ret) {
			pr_err("failed to load bootstrap publish argument\n");
			return -EFAULT;
		}

		obmm_bootstrap_to_sim(&cmd_param.bootstrap_publish.record,
				      &record);
		region = search_get_obmm_region(record.export_mem_id);
		if (region && region->type == OBMM_EXPORT_REGION) {
			struct obmm_export_region *e_reg =
				container_of(region,
					     struct obmm_export_region,
					     region);
			record.backing_uba = sg_phys(e_reg->sgt.sgl);
		}
		ret = ub_sim_decoder_obmm_bootstrap_publish(record.export_cna,
							    &record);
		if (!ret && region && region->type == OBMM_EXPORT_REGION) {
			struct obmm_export_region *e_reg =
				container_of(region,
					     struct obmm_export_region,
					     region);

			e_reg->sim_export_cna = record.export_cna;
			e_reg->sim_bootstrap_published = true;
		}
		if (region)
			put_obmm_region(region);
	} break;
	case OBMM_CMD_BOOTSTRAP_LOOKUP: {
		struct sim_dec_obmm_bootstrap_lookup_resp resp = {0};
		u32 i;

		ret = (int)copy_from_user(&cmd_param.bootstrap_lookup,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_bootstrap_lookup));
		if (ret) {
			pr_err("failed to load bootstrap lookup argument\n");
			return -EFAULT;
		}

		ret = ub_sim_decoder_obmm_bootstrap_lookup(
			cmd_param.bootstrap_lookup.local_cna,
			cmd_param.bootstrap_lookup.node_count,
			cmd_param.bootstrap_lookup.generation, &resp);
		if (ret)
			return ret;

		memset(&cmd_param.bootstrap_lookup.records, 0,
		       sizeof(cmd_param.bootstrap_lookup.records));
		cmd_param.bootstrap_lookup.count = resp.count;
		for (i = 0; i < resp.count && i < OBMM_BOOTSTRAP_MAX_NODES; i++)
			obmm_bootstrap_from_sim(&resp.records[i],
						&cmd_param.bootstrap_lookup.records[i]);

		ret = (int)copy_to_user((void __user *)arg,
					&cmd_param.bootstrap_lookup,
					sizeof(struct obmm_cmd_bootstrap_lookup));
		if (ret) {
			pr_err("failed to write bootstrap lookup result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_GSVA_APERTURE_REGISTER: {
		ret = (int)copy_from_user(&cmd_param.gsva_aperture,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_gsva_aperture));
		if (ret) {
			pr_err("failed to load gsva aperture argument\n");
			return -EFAULT;
		}

		ret = obmm_gsva_aperture_register(&cmd_param.gsva_aperture);
	} break;
	case OBMM_CMD_GSVA_APERTURE_QUERY: {
		obmm_gsva_aperture_query(&cmd_param.gsva_aperture);

		ret = (int)copy_to_user((void __user *)arg,
					&cmd_param.gsva_aperture,
					sizeof(struct obmm_cmd_gsva_aperture));
		if (ret) {
			pr_err("failed to write gsva aperture query result\n");
			return -EFAULT;
		}
	} break;
	case OBMM_CMD_GSVA_APERTURE_CLEAR: {
		ret = (int)copy_from_user(&cmd_param.gsva_aperture,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_gsva_aperture));
		if (ret) {
			pr_err("failed to load gsva aperture clear argument\n");
			return -EFAULT;
		}

		ret = obmm_gsva_aperture_clear(&cmd_param.gsva_aperture);
	} break;
	case OBMM_CMD_GSVA_QUERY_V1: {
		struct ub_sim_decoder *dec = g_ub_sim_decoder;
		struct sim_dec_gsva_query_req qreq = {0};
		struct sim_dec_gsva_query_resp qresp = {0};
		struct ub_entity *ubc_ents[1] = {NULL};
		unsigned int ubc_count = 0;
		u32 query_cna = 0;

		ret = (int)copy_from_user(&cmd_param.gsva_query,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_gsva_query_v1));
		if (ret) {
			pr_err("failed to load gsva query argument\n");
			return -EFAULT;
		}

		if (!dec || !dec->enabled) {
			pr_err("gsva query: sim decoder not available\n");
			return -ENODEV;
		}

		ret = ub_get_bus_controller(ubc_ents, 1, &ubc_count);
		if (ret || ubc_count == 0 || !ubc_ents[0]) {
			pr_err("gsva query: no bus controller available\n");
			return -ENODEV;
		}
		query_cna = ubc_ents[0]->cna;

		qreq.version = cmd_param.gsva_query.version;
		qreq.query_type = cmd_param.gsva_query.query_type;
		qreq.key.version = OBMM_GSVA_ABI_VERSION;
		qreq.key.segment_id = cmd_param.gsva_query.segment_id;
		qreq.key.home_va = cmd_param.gsva_query.home_va;

		ret = ub_sim_dec_backend_gsva_query_v1(dec, query_cna, &qreq, &qresp);
		if (ret) {
			pr_err("gsva query failed: %d\n", ret);
			return ret;
		}

		memcpy(cmd_param.gsva_query.resp_data, &qresp,
		       min_t(size_t, sizeof(qresp),
			     sizeof(cmd_param.gsva_query.resp_data)));

		ret = (int)copy_to_user((void __user *)arg, &cmd_param.gsva_query,
					sizeof(struct obmm_cmd_gsva_query_v1));
		if (ret)
			return -EFAULT;
	} break;
	case OBMM_CMD_GSVA_EVENT_V1: {
		struct ub_sim_decoder *dec = g_ub_sim_decoder;
		struct sim_dec_gsva_event_req ereq = {0};
		struct sim_dec_gsva_event_resp eresp = {0};
		struct ub_entity *ubc_ents[1] = {NULL};
		unsigned int ubc_count = 0;
		u32 query_cna = 0;

		ret = (int)copy_from_user(&cmd_param.gsva_event,
					  (void __user *)arg,
					  sizeof(struct obmm_cmd_gsva_event_v1));
		if (ret) {
			pr_err("failed to load gsva event argument\n");
			return -EFAULT;
		}

		if (cmd_param.gsva_event.version != OBMM_GSVA_ABI_VERSION)
			return -EINVAL;

		if (!dec || !dec->enabled) {
			pr_err("gsva event: sim decoder not available\n");
			return -ENODEV;
		}

		ret = ub_get_bus_controller(ubc_ents, 1, &ubc_count);
		if (ret || ubc_count == 0 || !ubc_ents[0]) {
			pr_err("gsva event: no bus controller available\n");
			return -ENODEV;
		}
		query_cna = ubc_ents[0]->cna;

		ereq.sub_op = cmd_param.gsva_event.sub_op;
		ereq.requester_cna = cmd_param.gsva_event.requester_cna ?
			cmd_param.gsva_event.requester_cna : query_cna;
		ereq.token_id = cmd_param.gsva_event.token_id;
		ereq.token_value = cmd_param.gsva_event.token_value;
		ereq.key.version = cmd_param.gsva_event.key.version;
		ereq.key.flags = cmd_param.gsva_event.key.flags;
		ereq.key.segment_id = cmd_param.gsva_event.key.segment_id;
		ereq.key.home_va = cmd_param.gsva_event.key.home_va;
		ereq.key.size = cmd_param.gsva_event.key.size;
		ereq.key.vmid = cmd_param.gsva_event.key.vmid;
		ereq.key.asid = cmd_param.gsva_event.key.asid;
		ereq.key.pte_offset = cmd_param.gsva_event.key.pte_offset;
		ereq.key.p_tag = cmd_param.gsva_event.key.p_tag;
		ereq.key.cache_policy = cmd_param.gsva_event.key.cache_policy;
		ereq.key.epoch = cmd_param.gsva_event.key.epoch;

		ret = ub_sim_dec_backend_gsva_event_v1(dec, query_cna,
						       &ereq, &eresp);
		if (ret) {
			pr_err("gsva event failed: %d\n", ret);
			return ret;
		}

		cmd_param.gsva_event.error = eresp.error;
		ret = (int)copy_to_user((void __user *)arg,
					&cmd_param.gsva_event,
					sizeof(struct obmm_cmd_gsva_event_v1));
		if (ret)
			return -EFAULT;
	} break;
		case OBMM_CMD_GSVA_ALLOC_SEGMENT: {
			ret = (int)copy_from_user(&cmd_param.gsva_alloc_seg,
						  (void __user *)arg,
						  sizeof(struct obmm_cmd_gsva_alloc_segment_v1));
			if (ret) {
				pr_err("failed to load gsva alloc segment argument\n");
				return -EFAULT;
			}
			ret = obmm_gsva_alloc_segment(&cmd_param.gsva_alloc_seg);
			if (ret)
				return ret;
			ret = (int)copy_to_user((void __user *)arg,
						&cmd_param.gsva_alloc_seg,
						sizeof(struct obmm_cmd_gsva_alloc_segment_v1));
			if (ret)
				return -EFAULT;
		} break;
		case OBMM_CMD_GSVA_QUERY_SEGMENT: {
			ret = (int)copy_from_user(&cmd_param.gsva_query_seg,
						  (void __user *)arg,
						  sizeof(struct obmm_cmd_gsva_query_segment_v1));
			if (ret) {
				pr_err("failed to load gsva query segment argument\n");
				return -EFAULT;
			}
			ret = obmm_gsva_query_segment(&cmd_param.gsva_query_seg);
			if (ret)
				return ret;
			ret = (int)copy_to_user((void __user *)arg,
						&cmd_param.gsva_query_seg,
						sizeof(struct obmm_cmd_gsva_query_segment_v1));
			if (ret)
				return -EFAULT;
		} break;
		case OBMM_CMD_GSVA_RETIRE_SEGMENT: {
			ret = (int)copy_from_user(&cmd_param.gsva_retire_seg,
						  (void __user *)arg,
						  sizeof(struct obmm_cmd_gsva_retire_segment_v1));
			if (ret) {
				pr_err("failed to load gsva retire segment argument\n");
				return -EFAULT;
			}
			ret = obmm_gsva_retire_segment(&cmd_param.gsva_retire_seg);
			if (ret && ret != -EALREADY)
				return ret;
			ret = (int)copy_to_user((void __user *)arg,
						&cmd_param.gsva_retire_seg,
						sizeof(struct obmm_cmd_gsva_retire_segment_v1));
			if (ret)
				return -EFAULT;
		} break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

const struct file_operations obmm_dev_fops = { .owner = THIS_MODULE,
					       .unlocked_ioctl = obmm_dev_ioctl,
					       .open = obmm_dev_open,
					       .flush = obmm_dev_flush };

static struct miscdevice obmm_dev_handle = { .minor = MISC_DYNAMIC_MINOR,
					     .name = OBMM_DEV_NAME,
					     .fops = &obmm_dev_fops };

static int __init obmm_init(void)
{
	int ret;

	pr_info("obmm_module: init started\n");

	ret = ubmempool_allocator_init();
	if (ret) {
		pr_err("Failed to init allocator. ret=%pe\n", ERR_PTR(ret));
		return ret;
	}

	ret = misc_register(&obmm_dev_handle);
	if (ret) {
		pr_err("Failed to register root device. ret=%pe\n", ERR_PTR(ret));
		goto out_allocator_exit;
	}

	spin_lock_init(&g_obmm_ctx_info.lock);
	INIT_LIST_HEAD(&g_obmm_ctx_info.regions);

	ret = obmm_shm_dev_init();
	if (ret) {
		pr_err("failed to initialize obmm_shm_dev. ret=%pe\n", ERR_PTR(ret));
		goto out_misc_deregister;
	}

	module_addr_check_init();

	ret = module_preimport_init();
	if (ret) {
		pr_err("failed to initialize preimport range manager. ret=%pe.\n", ERR_PTR(ret));
		goto out_addr_check_exit;
	}

	ret = lowmem_notify_init();
	if (ret) {
		pr_err("failed to initialize lowmem handler. ret=%pe\n", ERR_PTR(ret));
		goto out_module_import_exit;
	}

	pr_info("obmm_module: init completed\n");
	return ret;

out_module_import_exit:
	module_preimport_exit();
out_addr_check_exit:
	module_addr_check_exit();
	obmm_shm_dev_exit();
out_misc_deregister:
	misc_deregister(&obmm_dev_handle);
out_allocator_exit:
	ubmempool_allocator_exit();
	return ret;
}

static void __exit obmm_exit(void)
{
	pr_info("obmm_module: exit started\n");

	lowmem_notify_exit();
	module_preimport_exit();
	module_addr_check_exit();
	obmm_shm_dev_exit();
	misc_deregister(&obmm_dev_handle);
	ubmempool_allocator_exit();

	pr_info("obmm_module: exit completed\n");
}

module_init(obmm_init);
module_exit(obmm_exit);

MODULE_DESCRIPTION("OBMM Framework's implementations.");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_LICENSE("GPL");
