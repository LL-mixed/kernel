// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.
 * Description: Demand IOVA related API. Due to their limited flexibility and
 * genericity, they are not currently fit in the DMA framework.
 */

#define pr_fmt(fmt) "[UMMU_CORE][IOVA]:" fmt

#include <linux/dma-map-ops.h>
#include <linux/iova.h>

#include "../../dma-iommu.h"
#include "../../iommu-priv.h"
#include "ummu_core_priv.h"

struct iova_slot {
	struct iommu_domain *domain;
	unsigned long *bitmap;
	struct device *dev;
	struct xarray xa;
	dma_addr_t iova;
	unsigned long nr_pages;
	int prot;
};

static int __attrs_to_prot(bool coherent, unsigned long attrs)
{
	int prot = coherent ? IOMMU_CACHE : 0;

	if (attrs & DMA_ATTR_PRIVILEGED)
		prot |= IOMMU_PRIV;

	return prot | IOMMU_READ | IOMMU_WRITE;
}

static inline unsigned long iova_bitmap_offset(struct iova_slot *slot,
					       dma_addr_t iova)
{
	return (iova - slot->iova) >> PAGE_SHIFT;
}

static bool check_iova_bitmap_region(struct iova_slot *slot,
				     unsigned long offset, size_t bits,
				     bool is_empty)
{
	if (is_empty)
		return (offset + bits) <= find_next_bit(slot->bitmap, slot->nr_pages, offset);
	else
		return (offset + bits) <= find_next_zero_bit(slot->bitmap, slot->nr_pages, offset);
}

static inline bool check_iova_range(struct iova_slot *slot, dma_addr_t iova,
				    unsigned long nr_pages)
{
	return (iova >= slot->iova) &&
	       ((iova + (nr_pages << PAGE_SHIFT)) <=
		(slot->iova + (slot->nr_pages << PAGE_SHIFT)));
}

static dma_addr_t domain_alloc_iova(struct iommu_domain *domain, size_t size,
				   u64 limit, struct device *dev)
{
	unsigned long iova_size, iova_pfn, shift;
	struct iova_domain *iovad;

	iovad = iommu_get_iova_domain(domain);
	if (!iovad) {
		pr_err("cannot find iova domain.\n");
		return 0;
	}

	shift = iova_shift(iovad);
	if (shift > PAGE_SHIFT) {
		pr_err("only supports page size based iova domain.\n");
		return 0;
	}
	iova_size = size >> shift;
	limit = min_not_zero(limit, dev->bus_dma_limit);
	if (domain->geometry.force_aperture)
		limit = min_t(u64, limit, (u64)domain->geometry.aperture_end);

	iova_pfn = alloc_iova_fast(iovad, iova_size, limit >> shift, true);
	return (dma_addr_t)(iova_pfn << shift);
}

static void domain_free_iova(struct iommu_domain *domain, dma_addr_t iova,
			    size_t size)
{
	struct iova_domain *iovad;

	iovad = iommu_get_iova_domain(domain);
	free_iova_fast(iovad, iova_pfn(iovad, iova), size >> iova_shift(iovad));
}

static struct page **allocate_pages(struct device *dev, unsigned long npages, gfp_t gfp)
{
	unsigned int i, index = 0, order, max_order = MAX_ORDER;
	unsigned long nr_pages = npages;
	unsigned long alloc_size;
	struct page **pages;
	struct page *page;
	gfp_t alloc_flag;

	pages = kvcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;

	while (nr_pages) {
		for (order = max_order; order != UINT_MAX; order--) {
			/*  when 0 order finished the loop, it flip to UINT_MAX */
			alloc_size = 1U << order;
			if (alloc_size > nr_pages)
				continue;
			alloc_flag = gfp | __GFP_NOWARN;
			if (order)
				alloc_flag |= __GFP_NORETRY;
			page = alloc_pages_node(dev_to_node(dev), alloc_flag, order);
			if (!page)
				continue;
			if (order)
				split_page(page, order);
			max_order = order;
			break;
		}
		if (!page)
			goto err_free;
		nr_pages -= alloc_size;
		while (alloc_size--)
			pages[index++] = page++;
		page = NULL;
	}
	return pages;

err_free:
	for (i = 0; i < index; i++)
		__free_page(pages[i]);
	kvfree(pages);
	return NULL;
}

static int map_pages(struct iova_slot *slot, dma_addr_t iova, unsigned long nr_pages,
		     struct page **pages)
{
	struct sg_table *sgt;
	unsigned long i, j;
	ssize_t mapped;
	int ret;

	sgt = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_out;
	}
	ret = sg_alloc_table_from_pages(sgt, pages, nr_pages, 0, nr_pages << PAGE_SHIFT,
					GFP_KERNEL);
	if (ret)
		goto err_release_sgt;

	mapped = iommu_map_sg(slot->domain, iova, sgt->sgl, sgt->orig_nents,
			      slot->prot, GFP_KERNEL);
	if (mapped < 0 || (size_t)mapped < nr_pages) {
		ret = -EFAULT;
		goto err_free_sgt;
	}
	for (i = 0; i < nr_pages; i++) {
		ret = xa_err(xa_store(&slot->xa,
				      iova_bitmap_offset(slot, iova) + i,
				      pages[i], GFP_KERNEL));
		if (ret)
			goto err_unmap;
	}
	sg_free_table(sgt);
	kfree(sgt);
	return 0;

err_unmap:
	for (j = 0; j < i; j++)
		xa_erase(&slot->xa, j);
	iommu_unmap(slot->domain, iova, (size_t)i << PAGE_SHIFT);
err_free_sgt:
	sg_free_table(sgt);
err_release_sgt:
	kfree(sgt);
err_out:
	return ret;
}

static void drain_pages(struct iova_slot *slot, dma_addr_t iova,
					unsigned long nr_pages)
{
	unsigned long curr_index, next_index, target_index, count;
	struct page *pg;
	size_t mapped;

	curr_index = iova_bitmap_offset(slot, iova);
	target_index = curr_index + nr_pages;
	while (curr_index < target_index) {
		curr_index = find_next_bit(slot->bitmap, target_index, curr_index);
		if (curr_index >= target_index)
			return;

		next_index = find_next_zero_bit(slot->bitmap, target_index,
						curr_index);
		count = next_index - curr_index;
		mapped = iommu_unmap(slot->domain,
				     slot->iova + (curr_index << PAGE_SHIFT),
				     count << PAGE_SHIFT);
		WARN_ON(mapped != (count << PAGE_SHIFT));
		while (curr_index < next_index) {
			pg = xa_erase(&slot->xa, curr_index);
			curr_index++;
			if (WARN_ON(!pg))
				continue;

			__free_page(pg);
		}
	}
}

static struct iova_slot *alloc_iova_slot(struct device *dev, dma_addr_t iova,
					 size_t size, unsigned long attrs)
{
	bool coherent = dev_is_dma_coherent(dev);
	struct iova_slot *slot;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return NULL;

	slot->prot = __attrs_to_prot(coherent, attrs);
	slot->domain = iommu_get_domain_for_dev(dev);
	slot->iova = iova;
	slot->dev = dev;
	slot->nr_pages = size >> PAGE_SHIFT;
	xa_init(&slot->xa);
	slot->bitmap = kvcalloc(BITS_TO_LONGS(slot->nr_pages), sizeof(unsigned long),
				GFP_KERNEL);
	if (!slot->bitmap) {
		xa_destroy(&slot->xa);
		kfree(slot);
		return NULL;
	}
	return slot;
}

static void free_iova_slot(struct iova_slot *slot)
{
	kvfree(slot->bitmap);
	xa_destroy(&slot->xa);
	kfree(slot);
}

static DEFINE_MUTEX(ensure_dma_domain_lock);

/*
 * If the device's current domain is not DMA-capable (e.g. identity),
 * allocate a proper DMA domain, initialise its IOVA allocator, and
 * attach the device to it so that dma_alloc_iova() can manage IOVA
 * ranges for the device.
 */
static struct iommu_domain *ensure_dma_domain(struct device *dev)
{
	struct iommu_domain *domain;
	const struct iommu_ops *ops;
	struct iova_domain *iovad;
	unsigned long order;
	int ret;

	domain = iommu_get_domain_for_dev(dev);
	if (domain && iommu_is_dma_domain(domain))
		return domain;

	if (!domain)
		return ERR_PTR(-ENODEV);

	mutex_lock(&ensure_dma_domain_lock);

	/* Re-check after lock */
	domain = iommu_get_domain_for_dev(dev);
	if (domain && iommu_is_dma_domain(domain)) {
		mutex_unlock(&ensure_dma_domain_lock);
		return domain;
	}

	ops = dev_iommu_ops(dev);
	if (!ops) {
		pr_err("no iommu ops for dev\n");
		ret = -ENODEV;
		goto err_unlock;
	}

	if (ops->domain_alloc_paging)
		domain = ops->domain_alloc_paging(dev);
	else if (ops->domain_alloc)
		domain = ops->domain_alloc(IOMMU_DOMAIN_DMA);
	else {
		pr_err("no domain_alloc op\n");
		ret = -EOPNOTSUPP;
		goto err_unlock;
	}

	if (IS_ERR_OR_NULL(domain)) {
		ret = domain ? PTR_ERR(domain) : -ENOMEM;
		pr_err("domain_alloc failed: %d\n", ret);
		goto err_unlock;
	}

	domain->type = IOMMU_DOMAIN_DMA;
	domain->owner = ops;
	if (!domain->pgsize_bitmap)
		domain->pgsize_bitmap = ops->pgsize_bitmap;
	if (!domain->ops)
		domain->ops = ops->default_domain_ops;

	ret = iommu_get_dma_cookie(domain);
	if (ret) {
		pr_err("iommu_get_dma_cookie failed: %d\n", ret);
		goto err_free;
	}

	iovad = iommu_get_iova_domain(domain);
	if (!iovad) {
		pr_err("iommu_get_iova_domain returned NULL\n");
		ret = -EFAULT;
		goto err_put_cookie;
	}

	order = __ffs(domain->pgsize_bitmap);
	init_iova_domain(iovad, 1UL << order, 1);
	ret = iova_domain_init_rcaches(iovad);
	if (ret) {
		pr_err("iova_domain_init_rcaches failed: %d\n", ret);
		goto err_put_cookie;
	}

	ret = iommu_attach_device(domain, dev);
	if (ret) {
		pr_err("iommu_attach_device failed: %d\n", ret);
		goto err_put_cookie;
	}

	pr_info("allocated DMA domain for %s\n", dev_name(dev));
	mutex_unlock(&ensure_dma_domain_lock);
	return domain;

err_put_cookie:
	iommu_put_dma_cookie(domain);
err_free:
	domain->ops->free(domain);
err_unlock:
	mutex_unlock(&ensure_dma_domain_lock);
	return ERR_PTR(ret);
}

struct iova_slot *dma_alloc_iova(struct device *dev, size_t size,
				 unsigned long attrs, dma_addr_t *iovap,
				 size_t *sizep)
{
	struct iommu_domain *domain;
	struct iova_slot *slot;
	dma_addr_t iova;
	int ret;

	size = PAGE_ALIGN(size);
	domain = iommu_get_domain_for_dev(dev);
	pr_info("dma_alloc_iova: dev=%s domain=%p type=0x%x\n",
		dev ? dev_name(dev) : "NULL", domain,
		domain ? domain->type : 0);
	if (!domain) {
		ret = -ENODEV;
		goto err_out;
	}
	if (!iommu_is_dma_domain(domain)) {
		pr_info("dma_alloc_iova: not DMA domain, calling ensure_dma_domain\n");
		domain = ensure_dma_domain(dev);
		if (IS_ERR(domain)) {
			pr_err("dma_alloc_iova: ensure_dma_domain failed: %ld\n",
			       PTR_ERR(domain));
			ret = PTR_ERR(domain);
			goto err_out;
		}
	}
	iova = domain_alloc_iova(domain, size, dev->coherent_dma_mask, dev);
	if (!iova) {
		ret = -EFAULT;
		goto err_out;
	}

	slot = alloc_iova_slot(dev, iova, size, attrs);
	if (!slot) {
		ret = -ENOMEM;
		goto err_free_iova;
	}

	*iovap = iova;
	*sizep = size;
	return slot;

err_free_iova:
	domain_free_iova(domain, iova, size);
err_out:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(dma_alloc_iova);

void dma_free_iova(struct iova_slot *slot)
{
	drain_pages(slot, slot->iova, slot->nr_pages);
	domain_free_iova(slot->domain, slot->iova, slot->nr_pages << PAGE_SHIFT);
	free_iova_slot(slot);
}
EXPORT_SYMBOL_GPL(dma_free_iova);

int ummu_fill_pages(struct iova_slot *slot, dma_addr_t iova, unsigned long nr_pages)
{
	struct page **pages;
	unsigned long i;
	int ret;

	if (!IS_ALIGNED(iova, PAGE_SIZE) || !nr_pages)
		return -EINVAL;

	if (!check_iova_range(slot, iova, nr_pages))
		return -EINVAL;

	if (!check_iova_bitmap_region(slot, iova_bitmap_offset(slot, iova), nr_pages, true))
		return -EEXIST;

	pages = allocate_pages(slot->dev, nr_pages, GFP_KERNEL | __GFP_ZERO);
	if (!pages)
		return -ENOMEM;

	ret = map_pages(slot, iova, nr_pages, pages);
	if (ret != 0)
		goto err_free_pages;

	bitmap_set(slot->bitmap, iova_bitmap_offset(slot, iova), nr_pages);
	kvfree(pages);
	return 0;

err_free_pages:
	for (i = 0; i < nr_pages; i++)
		__free_page(pages[i]);
	kvfree(pages);
	return ret;
}
EXPORT_SYMBOL_GPL(ummu_fill_pages);

int ummu_drain_pages(struct iova_slot *slot, dma_addr_t iova, unsigned long nr_pages)
{
	if (!IS_ALIGNED(iova, PAGE_SIZE) || !nr_pages)
		return -EINVAL;

	if (!check_iova_range(slot, iova, nr_pages))
		return -EINVAL;

	if (!check_iova_bitmap_region(slot, iova_bitmap_offset(slot, iova), nr_pages, false))
		return -EEXIST;

	drain_pages(slot, iova, nr_pages);
	bitmap_clear(slot->bitmap, iova_bitmap_offset(slot, iova), nr_pages);
	return 0;
}
EXPORT_SYMBOL_GPL(ummu_drain_pages);
