// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) HiSilicon Technologies Co., Ltd. 2025. All rights reserved.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/iommu.h>

#include "ubus_inner.h"

static const struct iommu_ops *ub_bus_type_iommu_ops;
void ub_bus_type_iommu_ops_set(const struct iommu_ops *ops)
{
	ub_bus_type_iommu_ops = ops;
}
EXPORT_SYMBOL_GPL(ub_bus_type_iommu_ops_set);

const struct iommu_ops *ub_bus_type_iommu_ops_get(void)
{
	return ub_bus_type_iommu_ops;
}
EXPORT_SYMBOL_GPL(ub_bus_type_iommu_ops_get);

static void ubct_dma_setup(struct device *dev)
{
	struct ub_entity *uent = to_ub_entity(dev);
	u64 end, mask, size;
	u8 addr_limit;

	addr_limit = uent->ubc->attr.mem_size_limit;
	if (!addr_limit) {
		pr_warn(FW_BUG "ub bus controller missing memory address limit\n");
		return;
	}

	size = (addr_limit >= SZ_64) ? U64_MAX : (1ULL << addr_limit);
	end = size - 1;
	mask = DMA_BIT_MASK(ilog2(end) + 1);
	dev->bus_dma_limit = end;
	dev->coherent_dma_mask = min(dev->coherent_dma_mask, mask);
	*dev->dma_mask = min(*dev->dma_mask, mask);
}

static inline const struct iommu_ops *ubct_iommu_fwspec_ops(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	return fwspec ? fwspec->ops : NULL;
}

static int ubct_iommu_fwspec_init(struct device *dev, u32 id,
				  struct fwnode_handle *fwnode,
				  const struct iommu_ops *ops)
{
	int ret = iommu_fwspec_init(dev, fwnode, ops);

	if (ret)
		return ret;

	return iommu_fwspec_add_ids(dev, &id, 1);
}

static int ubct_iommu_configure(struct device *dev)
{
	if (!ub_bus_type_iommu_ops) {
		dev_dbg(dev, "ubus's iommu_ops not ready, defer probe\n");
		return -EPROBE_DEFER;
	}

	/* input id is 0, fwnode is NULL, ummu not care */
	return ubct_iommu_fwspec_init(dev, 0, NULL, ub_bus_type_iommu_ops);
}

static const struct iommu_ops *handle_iommu_configure_error(struct device *dev, int err)
{
	if (err == -EPROBE_DEFER)
		return ERR_PTR(-EPROBE_DEFER);

	dev_warn(dev, "Adding to IOMMU failed: %d\n", err);
	return NULL;
}

static const struct iommu_ops *ub_hybrid_iommu_configure(struct device *dev)
{
	const struct iommu_ops *ops;
	int err;

	ops = ubct_iommu_fwspec_ops(dev);
	if (ops)
		return ops;

	err = ubct_iommu_configure(dev);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(dev, "viot iommu configure: %d\n", err);

		return handle_iommu_configure_error(dev, err);
	}

	if (dev->bus && !device_iommu_mapped(dev)) {
		err = iommu_probe_device(dev);
		if (err)
			return handle_iommu_configure_error(dev, err);
	}

	return ubct_iommu_fwspec_ops(dev);
}

static int ub_hybrid_dma_configure(struct device *dev, enum dev_dma_attr attr)
{
	const struct iommu_ops *iommu;

	if (attr == DEV_DMA_NOT_SUPPORTED) {
		set_dma_ops(dev, &dma_dummy_ops);
		return 0;
	}

	ubct_dma_setup(dev);

	iommu = ub_hybrid_iommu_configure(dev);
	if (PTR_ERR(iommu) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	arch_setup_dma_ops(dev, 0, U64_MAX, attr == DEV_DMA_COHERENT);

	return 0;
}

/* Translate from FW */
static enum dev_dma_attr ub_dma_attr_trans(u16 dma_attr)
{
	switch (dma_attr) {
	case 0:
		return DEV_DMA_NON_COHERENT;
	case 1:
		return DEV_DMA_COHERENT;
	default:
		return DEV_DMA_NOT_SUPPORTED;
	}
}

static int ub_dma_configure(struct device *dev)
{
	struct ub_driver *udrv = to_ub_driver(dev->driver);
	struct ub_entity *uent = to_ub_entity(dev);
	struct ub_bus_controller *ubc;
	int ret;

	ubc = ub_ubc_get(uent->ubc);

	ret = ub_hybrid_dma_configure(dev,
				      ub_dma_attr_trans(ubc->attr.dma_cca));
	if (!ret && !udrv->driver_managed_dma) {
		ret = iommu_device_use_default_domain(dev);
		if (ret)
			arch_teardown_dma_ops(dev);
	}

	ub_ubc_put(ubc);
	dev_info(dev, "dma_configure ret = %d\n", ret);
	return ret;
}

static void ub_dma_cleanup(struct device *dev)
{
	struct ub_driver *driver = to_ub_driver(dev->driver);

	if (!driver->driver_managed_dma)
		iommu_device_unuse_default_domain(dev);

	dev_dbg(dev, "dma_cleanup\n");
}

struct bus_type ub_bus_type = {
	.name = "ub",
	.dma_configure = ub_dma_configure,
	.dma_cleanup = ub_dma_cleanup,
};
EXPORT_SYMBOL_GPL(ub_bus_type);

struct ub_dynid {
	struct list_head node;
	struct ub_device_id id;
};

static void ub_free_dynids(struct ub_driver *drv)
{
	struct ub_dynid *dynid, *n;

	spin_lock(&drv->dynids.lock);
	list_for_each_entry_safe(dynid, n, &drv->dynids.list, node) {
		list_del(&dynid->node);
		kfree(dynid);
	}
	spin_unlock(&drv->dynids.lock);
}

static bool ub_driver_match_one_id(const struct ub_device_id *id,
				   const struct ub_entity *uent)
{
	return (id->vendor == UB_ANY_ID || id->vendor == uent_vendor(uent)) &&
	       (id->device == UB_ANY_ID || id->device == uent_device(uent)) &&
	       (id->mod_vendor == UB_ANY_ID || id->mod_vendor == uent->mod_vendor) &&
	       (id->module == UB_ANY_ID || id->module == uent->module) &&
	       !((id->class_code ^ uent_class(uent)) & id->class_mask);
}

static bool ub_driver_matches_entity(struct ub_driver *drv,
				     const struct ub_entity *uent)
{
	const struct ub_device_id *id;

	for (id = drv->id_table; id && (id->vendor || id->mod_vendor || id->class_mask); id++) {
		if (ub_driver_match_one_id(id, uent))
			return true;
	}

	return false;
}

static int ub_try_rebind_from_generic(struct device *dev, void *data)
{
	struct ub_driver *new_drv = data;
	struct ub_entity *uent = to_ub_entity(dev);
	int ret;

	if (!dev->driver || dev->driver == &new_drv->driver)
		return 0;

	if (strcmp(dev->driver->name, "ub_generic_component"))
		return 0;

	if (!uent->match_driver)
		return 0;

	if (!ub_driver_matches_entity(new_drv, uent))
		return 0;

	ret = driver_set_override(dev, &uent->driver_override, new_drv->name,
				  strlen(new_drv->name));
	if (ret)
		return 0;

	device_release_driver(dev);
	ret = device_attach(dev);
	if (ret <= 0)
		dev_warn(dev, "late rebind to %s failed, ret=%d\n",
			 new_drv->name, ret);
	else
		dev_info(dev, "late rebind from ub_generic_component to %s\n",
			 new_drv->name);

	driver_set_override(dev, &uent->driver_override, "", 0);

	return 0;
}

int __ub_register_driver(struct ub_driver *drv, struct module *owner,
			 const char *mod_name)
{
	int ret;

	if (!drv)
		return -EINVAL;

	drv->driver.name = drv->name;
	drv->driver.bus = &ub_bus_type;
	drv->driver.owner = owner;
	drv->driver.mod_name = mod_name;
	drv->driver.groups = drv->groups;

	spin_lock_init(&drv->dynids.lock);
	INIT_LIST_HEAD(&drv->dynids.list);

	ret = driver_register(&drv->driver);
	if (ret)
		return ret;

	/*
	 * In built-in ubus/vendor configurations, ub_host_probe can enumerate
	 * entities before functional modules like ubase are loaded. Those
	 * entities may already be bound to ub_generic_component, so a plain
	 * late driver_register() is not enough to hand them over.
	 */
	bus_for_each_dev(&ub_bus_type, NULL, drv, ub_try_rebind_from_generic);

	return 0;
}
EXPORT_SYMBOL_GPL(__ub_register_driver);

void ub_unregister_driver(struct ub_driver *drv)
{
	if (!drv)
		return;

	driver_unregister(&drv->driver);
	ub_free_dynids(drv);
}
EXPORT_SYMBOL_GPL(ub_unregister_driver);

static int __init ub_driver_init(void)
{
	return bus_register(&ub_bus_type);
}
postcore_initcall(ub_driver_init);
