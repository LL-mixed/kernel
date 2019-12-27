// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017-2018 Hisilicon Limited.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/esr.h>
#include <linux/mmu_context.h>

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/miscdevice.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/ptrace.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <linux/sched/mm.h>
#include <linux/msi.h>
#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#endif

#define SVM_DEVICE_NAME "svm"
#define ASID_SHIFT		48

#define SVM_IOCTL_PROCESS_BIND		0xffff
#define SVM_IOCTL_GET_PHYS			0xfff9
#ifdef CONFIG_ACPI
#define SVM_IOCTL_SET_RC			0xfffc
#else
#define SVM_IOCTL_GET_L2PTE_BASE	0xfffb
#define SVM_IOCTL_LOAD_FLAG			0xfffa
#define SVM_IOCTL_PIN_MEMORY		0xfff7
#define SVM_IOCTL_UNPIN_MEMORY		0xfff5
#define SVM_IOCTL_GETHUGEINFO		0xfff6
#define SVM_IOCTL_REMAP_PROC		0xfff4
#endif

#define SVM_REMAP_MEM_LEN_MAX		(16 * 1024 * 1024)

#ifndef CONFIG_ACPI
#define CORE_SID		0
static int probe_index;
#else
LIST_HEAD(child_list);
#endif
static DECLARE_RWSEM(svm_sem);
static DEFINE_SPINLOCK(svm_process_lock);
static struct rb_root svm_process_root = RB_ROOT;

struct core_device {
	struct device	dev;
	struct iommu_group	*group;
	struct iommu_domain	*domain;
	u8	smmu_bypass;
	struct list_head entry;
};

struct svm_device {
	unsigned long long	id;
	struct miscdevice	miscdev;
	struct device		*dev;
	phys_addr_t l2buff;
	unsigned long		l2size;
};

struct svm_bind_process {
	pid_t			vpid;
	u64			ttbr;
	u64			tcr;
	int			pasid;
	u32			flags;
#define SVM_BIND_PID		(1 << 0)
};

struct svm_process {
	struct pid		*pid;
	struct mm_struct	*mm;
	unsigned long		asid;
	struct kref		kref;
	struct rb_node		rb_node;
	struct mmu_notifier	notifier;
	/* For postponed release */
	struct rcu_head		rcu;
	struct list_head	contexts;
	int			pasid;
	struct mutex		mutex;
	struct rb_root		sdma_list;
};

/* keep the relationship of svm_process and svm_device */
struct svm_context {
	struct svm_process	*process;
	struct svm_device	*sdev;
	struct list_head	process_head;
	atomic_t		ref;
};

#ifndef CONFIG_ACPI
struct svm_sdma {
	struct rb_node node;
	unsigned long addr;
	int nr_pages;
	struct page **pages;
	atomic64_t ref;
};

struct svm_proc_mem {
	u32 dev_id;
	u32 len;
	u64 pid;
	u64 vaddr;
	u64 buf;
};

struct meminfo {
	unsigned long hugetlbfree;
	unsigned long hugetlbtotal;
};
#endif

static struct bus_type svm_bus_type = {
	.name		= "svm_bus",
};

static char *svm_cmd_to_string(unsigned int cmd)
{
	switch (cmd) {
	case SVM_IOCTL_PROCESS_BIND:
		return "bind";
	case SVM_IOCTL_GET_PHYS:
		return "get phys";
#ifdef CONFIG_ACPI
	case SVM_IOCTL_SET_RC:
		return "set rc";
#else
	case SVM_IOCTL_GET_L2PTE_BASE:
		return "get l2pte base";
	case SVM_IOCTL_PIN_MEMORY:
		return "pin memory";
	case SVM_IOCTL_UNPIN_MEMORY:
		return "unpin memory";
	case SVM_IOCTL_GETHUGEINFO:
		return "get hugeinfo";
	case SVM_IOCTL_REMAP_PROC:
		return "remap proc";
	case SVM_IOCTL_LOAD_FLAG:
		return "load flag";
#endif
	default:
		return "unsupported";
	}

	return NULL;
}

static struct svm_process *find_svm_process(unsigned long asid)
{
	struct rb_node *node = svm_process_root.rb_node;

	while (node) {
		struct svm_process *process = NULL;

		process = rb_entry(node, struct svm_process, rb_node);
		if (asid < process->asid)
			node = node->rb_left;
		else if (asid > process->asid)
			node = node->rb_right;
		else
			return process;
	}

	return NULL;
}

static void insert_svm_process(struct svm_process *process)
{
	struct rb_node **p = &svm_process_root.rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct svm_process *tmp_process = NULL;

		parent = *p;
		tmp_process = rb_entry(parent, struct svm_process, rb_node);
		if (process->asid < tmp_process->asid)
			p = &(*p)->rb_left;
		else if (process->asid > tmp_process->asid)
			p = &(*p)->rb_right;
		else {
			WARN_ON_ONCE("asid already in the tree");
			return;
		}
	}

	rb_link_node(&process->rb_node, parent, p);
	rb_insert_color(&process->rb_node, &svm_process_root);
}

static void delete_svm_process(struct svm_process *process)
{
	rb_erase(&process->rb_node, &svm_process_root);
	RB_CLEAR_NODE(&process->rb_node);
}

static struct svm_device *file_to_sdev(struct file *file)
{
	return container_of(file->private_data,
			struct svm_device, miscdev);
}

static int svm_open(struct inode *inode, struct file *file)
{
	return 0;
}

static inline struct core_device *to_core_device(struct device *d)
{
	return container_of(d, struct core_device, dev);
}

static void cdev_device_release(struct device *dev)
{
	struct core_device *cdev = to_core_device(dev);

#ifdef CONFIG_ACPI
	list_del(&cdev->entry);
#endif
	kfree(cdev);
}

static int svm_remove_core(struct device *dev, void *data)
{
	struct core_device *cdev = to_core_device(dev);

	if (!cdev->smmu_bypass) {
		iommu_detach_group(cdev->domain, cdev->group);
		iommu_group_put(cdev->group);
		iommu_domain_free(cdev->domain);
	}

	device_unregister(&cdev->dev);

	return 0;
}

#ifndef CONFIG_ACPI
static struct svm_sdma *svm_find_sdma(struct svm_process *process,
				unsigned long addr, int nr_pages)
{
	struct rb_node *node = process->sdma_list.rb_node;

	mutex_lock(&process->mutex);
	while (node) {
		struct svm_sdma *sdma = NULL;

		sdma = rb_entry(node, struct svm_sdma, node);
		if (addr < sdma->addr)
			node = node->rb_left;
		else if (addr > sdma->addr)
			node = node->rb_right;
		else if (nr_pages < sdma->nr_pages)
			node = node->rb_left;
		else if (nr_pages > sdma->nr_pages)
			node = node->rb_right;
		else {
			mutex_unlock(&process->mutex);
			return sdma;
		}
	}
	mutex_unlock(&process->mutex);

	return NULL;
}

static int svm_insert_sdma(struct svm_process *process, struct svm_sdma *sdma)
{
	struct rb_node **p = &process->sdma_list.rb_node;
	struct rb_node *parent = NULL;

	mutex_lock(&process->mutex);
	while (*p) {
		struct svm_sdma *tmp_sdma = NULL;

		parent = *p;
		tmp_sdma = rb_entry(parent, struct svm_sdma, node);
		if (sdma->addr < tmp_sdma->addr)
			p = &(*p)->rb_left;
		else if (sdma->addr > tmp_sdma->addr)
			p = &(*p)->rb_right;
		else if (sdma->nr_pages < tmp_sdma->nr_pages)
			p = &(*p)->rb_left;
		else if (sdma->nr_pages > tmp_sdma->nr_pages)
			p = &(*p)->rb_right;
		else {
			/*
			 * add reference count and return -EBUSY
			 * to free former alloced one.
			 */
			atomic64_inc(&tmp_sdma->ref);
			mutex_unlock(&process->mutex);
			return -EBUSY;
		}
	}

	rb_link_node(&sdma->node, parent, p);
	rb_insert_color(&sdma->node, &process->sdma_list);

	mutex_unlock(&process->mutex);

	return 0;
}

static void svm_remove_sdma(struct svm_process *process,
			    struct svm_sdma *sdma, bool try_rm)
{
	int null_count = 0;

	mutex_lock(&process->mutex);

	if (try_rm && (!atomic64_dec_and_test(&sdma->ref))) {
		mutex_unlock(&process->mutex);
		return;
	}

	rb_erase(&sdma->node, &process->sdma_list);
	RB_CLEAR_NODE(&sdma->node);
	mutex_unlock(&process->mutex);

	while (sdma->nr_pages--) {
		if (sdma->pages[sdma->nr_pages] == NULL) {
			pr_err("null pointer, nr_pages:%d.\n", sdma->nr_pages);
			null_count++;
			continue;
		}

		put_page(sdma->pages[sdma->nr_pages]);
	}

	if (null_count)
		dump_stack();

	kfree(sdma->pages);
	kfree(sdma);
}

static int svm_pin_pages(unsigned long addr, int nr_pages,
			 struct page **pages)
{
	int err;

	err = get_user_pages_fast(addr, nr_pages, 1, pages);
	if (err > 0 && err < nr_pages) {
		while (err--)
			put_page(pages[err]);
		err = -EFAULT;
	} else if (err == 0) {
		err = -EFAULT;
	}

	return err;
}

static int svm_add_sdma(struct svm_process *process,
			unsigned long addr, unsigned long size)
{
	int err;
	struct svm_sdma *sdma = NULL;

	sdma = kzalloc(sizeof(struct svm_sdma), GFP_KERNEL);
	if (sdma == NULL)
		return -ENOMEM;

	atomic64_set(&sdma->ref, 1);
	sdma->addr = addr & PAGE_MASK;
	sdma->nr_pages = (PAGE_ALIGN(size + sdma->addr) >> PAGE_SHIFT) -
			 (addr >> PAGE_SHIFT);
	sdma->pages = kcalloc(sdma->nr_pages, sizeof(char *), GFP_KERNEL);
	if (sdma->pages == NULL) {
		err = -ENOMEM;
		goto err_free_sdma;
	}

	/*
	 * If always pin the same addr with the same nr_pages, pin pages
	 * maybe should move after insert sdma with mutex lock.
	 */
	err = svm_pin_pages(sdma->addr, sdma->nr_pages, sdma->pages);
	if (err < 0) {
		pr_err("%s: failed to pin pages addr 0x%lx, size 0x%lx\n",
		       __func__, addr, size);
		goto err_free_pages;
	}

	err = svm_insert_sdma(process, sdma);
	if (err < 0) {
		err = 0;
		pr_debug("%s: sdma already exist!\n", __func__);
		goto err_unpin_pages;
	}

	return err;

err_unpin_pages:
	while (sdma->nr_pages--)
		put_page(sdma->pages[sdma->nr_pages]);
err_free_pages:
	kfree(sdma->pages);
err_free_sdma:
	kfree(sdma);

	return err;
}

static int svm_pin_memory(unsigned long __user *arg)
{
	int err;
	struct svm_process *process = NULL;
	unsigned long addr, size, asid;

	if (arg == NULL)
		return -EINVAL;

	if (get_user(addr, arg))
		return -EFAULT;

	if (get_user(size, arg + 1))
		return -EFAULT;

	if ((addr + size <= addr) || (size >= (u64)UINT_MAX) || (addr == 0))
		return -EINVAL;

	asid = mm_context_get(current->mm);
	if (!asid)
		return -ENOSPC;

	spin_lock(&svm_process_lock);
	process = find_svm_process(asid);
	if (process == NULL) {
		spin_unlock(&svm_process_lock);
		err = -ESRCH;
		goto out;
	}
	spin_unlock(&svm_process_lock);

	err = svm_add_sdma(process, addr, size);

out:
	mm_context_put(current->mm);

	return err;
}

static int svm_unpin_memory(unsigned long __user *arg)
{
	int err = 0, nr_pages;
	struct svm_sdma *sdma = NULL;
	unsigned long addr, size, asid;
	struct svm_process *process = NULL;

	if (arg == NULL)
		return -EINVAL;

	if (get_user(addr, arg))
		return -EFAULT;

	if (get_user(size, arg + 1))
		return -EFAULT;

	asid = mm_context_get(current->mm);
	if (!asid)
		return -ENOSPC;

	addr &= PAGE_MASK;
	nr_pages = (PAGE_ALIGN(size + addr) >> PAGE_SHIFT) -
		   (addr >> PAGE_SHIFT);

	spin_lock(&svm_process_lock);
	process = find_svm_process(asid);
	if (process == NULL) {
		spin_unlock(&svm_process_lock);
		err = -ESRCH;
		goto out;
	}
	spin_unlock(&svm_process_lock);

	sdma = svm_find_sdma(process, addr, nr_pages);
	if (sdma == NULL) {
		err = -ESRCH;
		goto out;
	}

	svm_remove_sdma(process, sdma, true);

out:
	mm_context_put(current->mm);

	return err;
}

static void svm_unpin_all(struct svm_process *process)
{
	struct rb_node *node = NULL;

	while ((node = rb_first(&process->sdma_list)))
		svm_remove_sdma(process,
				rb_entry(node, struct svm_sdma, node),
				false);
}
#endif

static int svm_bind_core(
#ifndef CONFIG_ACPI
		struct device *dev,
#else
		struct core_device *cdev,
#endif
	void *data)
{
	int err;
	struct task_struct *task = NULL;
	struct svm_process *process = data;
#ifndef CONFIG_ACPI
	struct core_device *cdev = to_core_device(dev);
#endif

	if (cdev->smmu_bypass)
		return 0;

	task = get_pid_task(process->pid, PIDTYPE_PID);
	if (!task) {
		pr_err("failed to get task_struct\n");
		return -ESRCH;
	}

	err = iommu_sva_bind_device(&cdev->dev, task->mm,
			 &process->pasid, IOMMU_SVA_FEAT_IOPF, NULL);
	if (err)
		pr_err("failed to get the pasid\n");

	put_task_struct(task);

	return err;
}

static int svm_unbind_core(
#ifndef CONFIG_ACPI
	struct device *dev,
#else
	struct core_device *cdev,
#endif
	void *data)
{
	struct svm_process *process = data;
#ifndef CONFIG_ACPI
	struct core_device *cdev = to_core_device(dev);
#endif

	if (cdev->smmu_bypass)
		return 0;

	iommu_sva_unbind_device(&cdev->dev, process->pasid);
	return 0;
}

static void svm_process_free(struct rcu_head *rcu)
{
	struct svm_process *process = NULL;

	process = container_of(rcu, struct svm_process, rcu);
#ifndef CONFIG_ACPI
	svm_unpin_all(process);
#endif
	mm_context_put(process->mm);
	kfree(process);
}

static void svm_process_release(struct kref *kref)
{
	struct svm_process *process = NULL;

	process = container_of(kref, struct svm_process, kref);

	delete_svm_process(process);
	put_pid(process->pid);

	/*
	 * If we're being released from process exit, the notifier callback
	 * ->release has already been called. Otherwise we don't need to go
	 * through there, the process isn't attached to anything anymore. Hence
	 * no_release.
	 */
	mmu_notifier_unregister_no_release(&process->notifier, process->mm);

	/*
	 * We can't free the structure here, because ->release might be
	 * attempting to grab it concurrently. And in the other case, if the
	 * structure is being released from within ->release, then
	 * __mmu_notifier_release expects to still have a valid mn when
	 * returning. So free the structure when it's safe, after the RCU grace
	 * period elapsed.
	 */
	mmu_notifier_call_srcu(&process->rcu, svm_process_free);
}

static int svm_process_get_locked(struct svm_process *process)
{
	if (process)
		return kref_get_unless_zero(&process->kref);

	return 0;
}

static void svm_process_put_locked(struct svm_process *process)
{
	if (process)
		kref_put(&process->kref, svm_process_release);
}

static void svm_context_free(struct svm_context *context)
{
	struct svm_process *process = context->process;
#ifndef CONFIG_ACPI
	struct svm_device *sdev = context->sdev;
#endif

#ifdef CONFIG_ACPI
	struct core_device *pos = NULL;

	list_for_each_entry(pos, &child_list, entry) {
		svm_unbind_core(pos, process);
	}
#else
	spin_unlock(&svm_process_lock);
	device_for_each_child(sdev->dev, process, svm_unbind_core);
	spin_lock(&svm_process_lock);
#endif
	list_del(&context->process_head);

	svm_process_put_locked(context->process);

	kfree(context);
}

static void svm_notifier_release(struct mmu_notifier *mn,
					struct mm_struct *mm)
{
	struct svm_process *process = NULL;
	struct svm_context *context = NULL;
	struct svm_context *next = NULL;

	process = container_of(mn, struct svm_process, notifier);

	spin_lock(&svm_process_lock);
	if (!svm_process_get_locked(process)) {
		/* Someone's already taking care of it. */
		spin_unlock(&svm_process_lock);
		return;
	}

	list_for_each_entry_safe(context, next,
				 &process->contexts, process_head) {
		/*
		 * Should notify the device cpu release something,
		 * if context ref is not 0?
		 */
		svm_context_free(context);
	}

	svm_process_put_locked(process);
	spin_unlock(&svm_process_lock);
}

static struct mmu_notifier_ops svm_process_mmu_notifier = {
	.release	= svm_notifier_release,
};

static struct svm_process *svm_process_alloc(struct pid *pid,
		struct mm_struct *mm, unsigned long asid)
{
	int err;

	struct svm_process *process = kzalloc(sizeof(*process), GFP_KERNEL);

	if (!process)
		return ERR_PTR(-ENOMEM);

	process->pid = pid;
	process->mm = mm;
	process->asid = asid;
	process->sdma_list = RB_ROOT; //lint !e64
	mutex_init(&process->mutex);
	INIT_LIST_HEAD(&process->contexts);
	process->notifier.ops = &svm_process_mmu_notifier;

	spin_lock(&svm_process_lock);
	insert_svm_process(process);
	kref_init(&process->kref);
	spin_unlock(&svm_process_lock);

	err = mmu_notifier_register(&process->notifier, mm);
	if (err)
		goto free_process;

	/* A mm_count reference is kept by the caller */
	mmput(process->mm);

	return process;

free_process:
	kfree(process);

	return ERR_PTR(err);
}

static struct svm_context *svm_process_attach(struct svm_process *process,
		struct svm_device *sdev)
{
	struct svm_context *context = NULL;
#ifdef CONFIG_ACPI
	struct core_device *pos = NULL;
#endif

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return ERR_PTR(-ENOMEM);

	context->process = process;
	context->sdev = sdev;
	atomic_set(&context->ref, 1);
#ifdef CONFIG_ACPI
	list_for_each_entry(pos, &child_list, entry) {
		svm_bind_core(pos, process);
	}
#else
	spin_unlock(&svm_process_lock);
	device_for_each_child(sdev->dev, process, svm_bind_core);
	spin_lock(&svm_process_lock);
#endif
	list_add(&context->process_head, &process->contexts);

	return context;
}

static struct task_struct *svm_get_task(struct svm_bind_process params)
{
	struct task_struct *task = NULL;

	if (params.flags & ~SVM_BIND_PID)
		return ERR_PTR(-EINVAL);

	if (params.flags & SVM_BIND_PID) {
		struct mm_struct *mm = NULL;

		rcu_read_lock();
		task = find_task_by_vpid(params.vpid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (task == NULL)
			return ERR_PTR(-ESRCH);

		/* check the permission */
		mm = mm_access(task, PTRACE_MODE_ATTACH_REALCREDS);
		if (IS_ERR_OR_NULL(mm)) {
			pr_err("cannot access mm\n");
			put_task_struct(task);
			return ERR_PTR(-ESRCH);
		}

		mmput(mm);
	} else {
		get_task_struct(current);
		task = current;
	}

	return task;
}

static int svm_process_bind(struct task_struct *task,
		struct svm_device *sdev, u64 *ttbr, u64 *tcr, int *pasid)
{
	int err;
	unsigned long asid;
	struct pid *pid = NULL;
	struct svm_context *context = NULL;
	struct svm_process *process = NULL;
	struct mm_struct *mm = NULL;

	if ((ttbr == NULL) || (tcr == NULL) || (pasid == NULL))
		return -EINVAL;

	pid = get_task_pid(task, PIDTYPE_PID);
	if (pid == NULL)
		return -EINVAL;

	mm = get_task_mm(task);
	if (!mm) {
		err = -EINVAL;
		goto err_put_pid;
	}

	asid = mm_context_get(mm);
	if (!asid) {
		err = -ENOSPC;
		goto err_put_mm;
	}

	/* If a svm_process already exists, use it */
	spin_lock(&svm_process_lock);
	process = find_svm_process(asid);
	if (process) {
		struct svm_context *cur_context = NULL;

		if (!svm_process_get_locked(process)) {
			/* ref is 0, svm_process is defunct or not exist */
			process = NULL;
			spin_unlock(&svm_process_lock);
			goto new_process;
		}

		list_for_each_entry(cur_context,
				    &process->contexts,
				    process_head) {
			if (cur_context->sdev != sdev)
				continue;

			context = cur_context;
			*ttbr = virt_to_phys(mm->pgd) | asid << ASID_SHIFT;
			*tcr  = read_sysreg(tcr_el1);
			*pasid = process->pasid;
			atomic_inc(&context->ref);
			/* One context keep a ref of process */
			svm_process_put_locked(process);

			break;
		}
	}
	spin_unlock(&svm_process_lock);

new_process:
	if (process == NULL) {
		process = svm_process_alloc(pid, mm, asid);
		if (IS_ERR(process)) {
			err = PTR_ERR(process);
			goto err_put_mm_context;
		}
	} else {
		 /* just keep a ref count for single process */
		mm_context_put(mm);
		mmput(mm);
		put_pid(pid);
	}

	if (context)
		return 0;

	spin_lock(&svm_process_lock);
	context = svm_process_attach(process, sdev);
	if (IS_ERR(context)) {
		svm_process_put_locked(process);
		spin_unlock(&svm_process_lock);
		return PTR_ERR(context);
	}
	spin_unlock(&svm_process_lock);

	*ttbr = virt_to_phys(mm->pgd) | asid << ASID_SHIFT;
	*tcr  = read_sysreg(tcr_el1);
	*pasid = process->pasid;

	return 0;

err_put_mm_context:
	mm_context_put(mm);
err_put_mm:
	mmput(mm);
err_put_pid:
	put_pid(pid);

	return err;
}

#ifdef CONFIG_ACPI
static int svm_acpi_add_core(struct svm_device *sdev,
		struct acpi_device *children, int id)
{
	int err;
	struct core_device *cdev = NULL;
	char *name = NULL;
	enum dev_dma_attr attr;

	name = devm_kasprintf(sdev->dev, GFP_KERNEL, "svm_child_dev%d", id);
	if (name == NULL)
		return -ENOMEM;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (cdev == NULL)
		return -ENOMEM;
	cdev->dev.fwnode = &children->fwnode;
	cdev->dev.parent = sdev->dev;
	cdev->dev.bus = &svm_bus_type;
	cdev->dev.release = cdev_device_release;
	list_add(&cdev->entry, &child_list);
	dev_set_name(&cdev->dev, "%s", name);

	err = device_register(&cdev->dev);
	if (err) {
		dev_info(&cdev->dev, "core_device register failed\n");
		put_device(&cdev->dev);
		list_del(&cdev->entry);
		kfree(cdev);
		return err;
	}

	attr = acpi_get_dma_attr(children);
	if (attr != DEV_DMA_NOT_SUPPORTED) {
		err = acpi_dma_configure(&cdev->dev, attr);
		if (err) {
			dev_dbg(&cdev->dev, "of_dma_configure failed\n");
			goto err_unregister_dev;
		}
	}

	err = acpi_dev_prop_read_single(children, "hisi,smmu-bypass",
			DEV_PROP_U8, &cdev->smmu_bypass);
	if (err) {
		dev_info(&children->dev, "read smmu bypass failed\n");
		goto err_unregister_dev;
	}

	cdev->group = iommu_group_get(&cdev->dev);
	if (IS_ERR_OR_NULL(cdev->group)) {
		err = -ENXIO;
		dev_err(&cdev->dev, "smmu is not right configured\n");
		goto err_unregister_dev;
	}

	cdev->domain = iommu_domain_alloc(sdev->dev->bus);
	if (cdev->domain == NULL) {
		err = -ENOMEM;
		dev_info(&cdev->dev, "failed to alloc domain\n");
		goto err_unregister_dev;
	}

	err = iommu_attach_group(cdev->domain, cdev->group);
	if (err) {
		dev_err(&cdev->dev, "failed group to domain\n");
		goto err_free_domain;
	}

	err = iommu_sva_device_init(&cdev->dev, IOMMU_SVA_FEAT_IOPF,
			UINT_MAX, 0);
	if (err) {
		dev_err(&cdev->dev, "failed to init sva device\n");
		goto err_detach_group;
	}

	return 0;

err_detach_group:
	iommu_detach_group(cdev->domain, cdev->group);
err_free_domain:
	iommu_domain_free(cdev->domain);
err_unregister_dev:
	device_unregister(&cdev->dev);

	return err;
}

static int svm_init_core(struct svm_device *sdev)
{
	int err = 0;
	struct device *dev = sdev->dev;
	struct acpi_device *adev = ACPI_COMPANION(sdev->dev);
	struct acpi_device *cdev = NULL;
	int id = 0;

	down_write(&svm_sem);
	if (!svm_bus_type.iommu_ops) {
		err = bus_register(&svm_bus_type);
		if (err) {
			up_write(&svm_sem);
			dev_err(dev, "failed to register svm_bus_type\n");
			return err;
		}

		err = bus_set_iommu(&svm_bus_type, dev->bus->iommu_ops);
		if (err) {
			up_write(&svm_sem);
			dev_err(dev, "failed to set iommu for svm_bus_type\n");
			goto err_unregister_bus;
		}
	} else if (svm_bus_type.iommu_ops != dev->bus->iommu_ops) {
		err = -EBUSY;
		up_write(&svm_sem);
		dev_err(dev, "iommu_ops configured, but changed!\n");
		goto err_unregister_bus;
	}
	up_write(&svm_sem);

	list_for_each_entry(cdev, &adev->children, node) {
		err = svm_acpi_add_core(sdev, cdev, id++);
		if (err)
			device_for_each_child(dev, NULL, svm_remove_core);
	}

	return err;

err_unregister_bus:
	bus_unregister(&svm_bus_type);

	return err;
}
#else
static int svm_of_add_core(struct svm_device *sdev, struct device_node *np)
{
	int err;
	struct resource res;
	struct core_device *cdev = NULL;
	char *name = NULL;

	name = devm_kasprintf(sdev->dev, GFP_KERNEL, "svm%llu_%s",
			sdev->id, np->name);
	if (name == NULL)
		return -ENOMEM;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (cdev == NULL)
		return -ENOMEM;

	cdev->dev.of_node = np;
	cdev->dev.parent = sdev->dev;
	cdev->dev.bus = &svm_bus_type;
	cdev->dev.release = cdev_device_release;
	cdev->smmu_bypass = of_property_read_bool(np, "hisi,smmu_bypass");
	dev_set_name(&cdev->dev, "%s", name);

	err = device_register(&cdev->dev);
	if (err) {
		dev_info(&cdev->dev, "core_device register failed\n");
		put_device(&cdev->dev);
		kfree(cdev);
		return err;
	}

	err = of_dma_configure(&cdev->dev, np, true);
	if (err) {
		dev_dbg(&cdev->dev, "of_dma_configure failed\n");
		goto err_unregister_dev;
	}

	err = of_address_to_resource(np, 0, &res);
	if (err) {
		dev_info(&cdev->dev, "no reg, FW should install the sid\n");
	} else {
		/* If the reg specified, install sid for the core */
		void __iomem *core_base = NULL;
		int sid = cdev->dev.iommu_fwspec->ids[0];

		core_base = ioremap(res.start, resource_size(&res));
		if (core_base == NULL) {
			err = -ENOMEM;
			dev_err(&cdev->dev, "ioremap failed\n");
			goto err_unregister_dev;
		}

		writel_relaxed(sid, core_base + CORE_SID);
		iounmap(core_base);
	}

	/* If core device is smmu bypass, request direct map. */
	if (cdev->smmu_bypass) {
		err = iommu_request_dm_for_dev(&cdev->dev);
		if (err)
			goto err_unregister_dev;

		return 0;
	}

	cdev->group = iommu_group_get(&cdev->dev);
	if (IS_ERR_OR_NULL(cdev->group)) {
		err = -ENXIO;
		dev_err(&cdev->dev, "smmu is not right configured\n");
		goto err_unregister_dev;
	}

	cdev->domain = iommu_domain_alloc(sdev->dev->bus);
	if (cdev->domain == NULL) {
		err = -ENOMEM;
		dev_info(&cdev->dev, "failed to alloc domain\n");
		goto err_unregister_dev;
	}

	err = iommu_attach_group(cdev->domain, cdev->group);
	if (err) {
		dev_err(&cdev->dev, "failed group to domain\n");
		goto err_free_domain;
	}

	err = iommu_sva_device_init(&cdev->dev, IOMMU_SVA_FEAT_IOPF,
			UINT_MAX, 0);
	if (err) {
		dev_err(&cdev->dev, "failed to init sva device\n");
		goto err_detach_group;
	}

	return 0;

err_detach_group:
	iommu_detach_group(cdev->domain, cdev->group);
err_free_domain:
	iommu_domain_free(cdev->domain);
err_unregister_dev:
	device_unregister(&cdev->dev);

	return err;
}

static int svm_init_core(struct svm_device *sdev, struct device_node *np)
{
	int err = 0;
	struct device_node *child = NULL;
	struct device *dev = sdev->dev;

	down_write(&svm_sem);
	if (svm_bus_type.iommu_ops == NULL) {
		err = bus_register(&svm_bus_type);
		if (err) {
			up_write(&svm_sem);
			dev_err(dev, "failed to register svm_bus_type\n");
			return err;
		}

		err = bus_set_iommu(&svm_bus_type, dev->bus->iommu_ops);
		if (err) {
			up_write(&svm_sem);
			dev_err(dev, "failed to set iommu for svm_bus_type\n");
			goto err_unregister_bus;
		}
	} else if (svm_bus_type.iommu_ops != dev->bus->iommu_ops) {
		err = -EBUSY;
		up_write(&svm_sem);
		dev_err(dev, "iommu_ops configured, but changed!\n");
		goto err_unregister_bus;
	}
	up_write(&svm_sem);

	for_each_available_child_of_node(np, child) {
		err = svm_of_add_core(sdev, child);
		if (err)
			device_for_each_child(dev, NULL, svm_remove_core);
	}

	return err;

err_unregister_bus:
	bus_unregister(&svm_bus_type);

	return err;
}
#endif

static pte_t *svm_get_pte(struct vm_area_struct *vma,
			  pud_t *pud,
			  unsigned long addr,
			  unsigned long *page_size,
			  unsigned long *offset)
{
	pte_t *pte = NULL;
	unsigned long size = 0;

	if (is_vm_hugetlb_page(vma)) {
		if (pud_present(*pud)) {
			if (pud_huge(*pud)) {
				pte = (pte_t *)pud;
				*offset = addr & (PUD_SIZE - 1);
				size = PUD_SIZE;
			} else {
				pte = (pte_t *)pmd_offset(pud, addr);
				*offset = addr & (PMD_SIZE - 1);
				size = PMD_SIZE;
			}
		} else {
			pr_err("%s:hugetlb but pud not present\n", __func__);
		}
	} else {
		pmd_t *pmd = pmd_offset(pud, addr);

		if (pmd_none(*pmd))
			return NULL;

		if (pmd_trans_huge(*pmd)) {
			pte = (pte_t *)pmd;
			*offset = addr & (PMD_SIZE - 1);
			size = PMD_SIZE;
		} else if (pmd_trans_unstable(pmd)) {
			pr_warn("%s: thp unstable\n", __func__);
		} else {
			pte = pte_offset_map(pmd, addr);
			*offset = addr & (PAGE_SIZE - 1);
			size = PAGE_SIZE;
		}
	}

	if (page_size)
		*page_size = size;

	return pte;
}

static pte_t *svm_walk_pt(unsigned long addr, unsigned long *page_size,
			  unsigned long *offset)
{
	pgd_t *pgd = NULL;
	pud_t *pud = NULL;
	pte_t *pte = NULL;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = NULL;

	vma = find_vma(mm, addr);
	if (!vma)
		return NULL;

	pgd = pgd_offset(mm, addr);
	if (pgd_none_or_clear_bad(pgd))
		return NULL;

	pud = pud_offset(pgd, addr);
	if (pud_none_or_clear_bad(pud))
		return NULL;

	pte = svm_get_pte(vma, pud, addr, page_size, offset);

	return pte;
}

static int svm_get_phys(unsigned long __user *arg)
{
	pte_t *pte = NULL;
	unsigned long addr, phys, offset;

	if (arg == NULL)
		return -EINVAL;

	if (get_user(addr, arg))
		return -EFAULT;

	pte = svm_walk_pt(addr, NULL, &offset);
	if (pte && pte_present(*pte)) {
		phys = PFN_PHYS(pte_pfn(*pte)) + offset;
		return put_user(phys, arg);
	}

	return -EINVAL;
}

int svm_get_pasid(pid_t vpid, int dev_id __maybe_unused)
{
	int pasid;
	unsigned long asid;
	struct task_struct *task = NULL;
	struct mm_struct *mm = NULL;
	struct svm_process *process = NULL;
	struct svm_bind_process params;

	params.flags = SVM_BIND_PID;
	params.vpid = vpid;
	params.pasid = -1;
	params.ttbr = 0;
	params.tcr = 0;
	task = svm_get_task(params);
	if (IS_ERR(task))
		return PTR_ERR(task);

	mm = get_task_mm(task);
	if (mm == NULL) {
		pasid = -EINVAL;
		goto put_task;
	}

	asid = mm_context_get(mm);
	if (!asid) {
		pasid = -ENOSPC;
		goto put_mm;
	}

	spin_lock(&svm_process_lock);
	process = find_svm_process(asid);
	spin_unlock(&svm_process_lock);
	if (process)
		pasid = process->pasid;
	else
		pasid = -ESRCH;

	mm_context_put(mm);
put_mm:
	mmput(mm);
put_task:
	put_task_struct(task);

	return pasid;
}
EXPORT_SYMBOL_GPL(svm_get_pasid);

#ifdef CONFIG_ACPI
static int svm_set_rc(unsigned long __user *arg)
{
	unsigned long addr, size, rc;
	unsigned long end, page_size, offset;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = NULL;
	pte_t *pte = NULL;

	if (arg == NULL)
		return -EINVAL;

	if (get_user(addr, arg))
		return -EFAULT;

	if (get_user(size, arg + 1))
		return -EFAULT;

	if (get_user(rc, arg + 2))
		return -EFAULT;

	vma = find_vma(mm, addr);
	if (!vma)
		return -ESRCH;

	end = addr + size;
	if (addr >= end)
		return -EINVAL;

	while (addr < end) {
		pte = svm_walk_pt(addr, &page_size, &offset);
		if (!pte)
			return -ESRCH;
		pte->pte |= (rc & (u64)0x0f) << 59;
		addr += page_size - offset;
	}

	return 0;
}
#else
static int svm_get_l2pte_base(struct svm_device *sdev,
			      unsigned long __user *arg)
{
	int i = 0, err = -EINVAL;
	unsigned long *base = NULL;
	unsigned long vaddr, size;
	struct mm_struct *mm = current->mm;

	if (arg == NULL)
		return -EINVAL;

	if (get_user(vaddr, arg))
		return -EFAULT;

	if (!IS_ALIGNED(vaddr, sdev->l2size))
		return -EINVAL;

	if (get_user(size, arg + 1))
		return -EFAULT;

	if (size != sdev->l2size)
		return -EINVAL;

	size = ALIGN(size, PMD_SIZE) / PMD_SIZE;
	base = kmalloc_array(size, sizeof(*base), GFP_KERNEL);
	if (base == NULL)
		return -ENOMEM;

	while (size) {
		pgd_t *pgd = NULL;
		pud_t *pud = NULL;
		pmd_t *pmd = NULL;

		pgd = pgd_offset(mm, vaddr);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto err_out;

		pud = pud_offset(pgd, vaddr);
		if (pud_none(*pud) || pud_bad(*pud))
			goto err_out;

		pmd = pmd_offset(pud, vaddr);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			goto err_out;

		/*
		 * For small page base address, it should use pte_pfn
		 * instead of pmd_pfn.
		 */
		base[i] = PFN_PHYS(pte_pfn(*((pte_t *)pmd)));
		vaddr += PMD_SIZE;
		size--;
		i++;
	}

	/* lint !e647 */
	err = copy_to_user((void __user *)arg, base, i * sizeof(*base));
err_out:
	kfree(base);
	return err;
}

static long svm_get_hugeinfo(unsigned long __user *arg)
{
	long err = -EINVAL;
	struct hstate *h = &default_hstate;
	struct meminfo info;

	if (arg == NULL)
		return err;

	if (!hugepages_supported())
		return -ENOTSUPP;

	info.hugetlbfree = h->free_huge_pages;
	info.hugetlbtotal = h->nr_huge_pages;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	pr_info("svm get hugetlb info: order(%u), max_huge_pages(%lu),"
			"nr_huge_pages(%lu), free_huge_pages(%lu), resv_huge_pages(%lu)",
			h->order,
			h->max_huge_pages,
			h->nr_huge_pages,
			h->free_huge_pages,
			h->resv_huge_pages);

	return 0;
}

static long svm_remap_get_phys(struct mm_struct *mm, struct vm_area_struct *vma,
			       unsigned long addr, unsigned long *phys,
			       unsigned long *page_size, unsigned long *offset)
{
	long err = -EINVAL;
	pgd_t *pgd = NULL;
	pud_t *pud = NULL;
	pte_t *pte = NULL;

	if (mm == NULL || vma == NULL || phys == NULL ||
	    page_size == NULL || offset == NULL)
		return err;

	pgd = pgd_offset(mm, addr);
	if (pgd_none_or_clear_bad(pgd))
		return err;

	pud = pud_offset(pgd, addr);
	if (pud_none_or_clear_bad(pud))
		return err;

	pte = svm_get_pte(vma, pud, addr, page_size, offset);
	if (pte && pte_present(*pte)) {
		*phys = PFN_PHYS(pte_pfn(*pte));
		return 0;
	}

	return err;
}

static long svm_remap_proc(unsigned long __user *arg)
{
	long ret = -EINVAL;
	struct svm_proc_mem pmem;
	struct task_struct *ptask = NULL;
	struct mm_struct *pmm = NULL, *mm = current->mm;
	struct vm_area_struct *pvma = NULL, *vma = NULL;
	unsigned long end, vaddr, phys, buf, offset, pagesize;

	if (arg == NULL) {
		pr_err("arg is invalid.\n");
		return ret;
	}

	ret = copy_from_user(&pmem, (void __user *)arg, sizeof(pmem));
	if (ret) {
		pr_err("failed to copy args from user space.\n");
		return -EFAULT;
	}

	if (pmem.buf & (PAGE_SIZE - 1)) {
		pr_err("address is not aligned with page size, addr:%llx.\n",
		       pmem.buf);
		return -EINVAL;
	}

	ptask = pid_task(find_vpid((int)pmem.pid), PIDTYPE_PID);
	if (ptask == NULL) {
		pr_err("cannot find the task of pid:%d.\n", (int)pmem.pid);
		return -EINVAL;
	}

	get_task_struct(ptask);
	rcu_read_unlock();
	pmm = ptask->mm;

	down_read(&mm->mmap_sem);
	down_read(&pmm->mmap_sem);

	pvma = find_vma(pmm, pmem.vaddr);
	if (pvma == NULL) {
		ret = -ESRCH;
		goto err;
	}

	vma = find_vma(mm, pmem.buf);
	if (vma == NULL) {
		ret = -ESRCH;
		goto err;
	}

	if (pmem.len > SVM_REMAP_MEM_LEN_MAX) {
		ret = -EINVAL;
		pr_err("too large length of memory.\n");
		goto err;
	}
	vaddr = pmem.vaddr;
	end = vaddr + pmem.len;
	buf = pmem.buf;
	vma->vm_flags |= VM_SHARED;
	if (end > pvma->vm_end || end < vaddr) {
		ret = -EINVAL;
		pr_err("memory length is out of range, vaddr:%lx, len:%u.\n",
		       vaddr, pmem.len);
		goto err;
	}

	do {
		ret = svm_remap_get_phys(pmm, pvma, vaddr,
					 &phys, &pagesize, &offset);
		if (ret) {
			ret = -EINVAL;
			goto err;
		}

		vaddr += pagesize - offset;

		do {
			if (remap_pfn_range(vma, buf, phys >> PAGE_SHIFT,
				PAGE_SIZE,
				__pgprot(vma->vm_page_prot.pgprot |
					 PTE_DIRTY))) {

				ret = -ESRCH;
				goto err;
			}

			offset += PAGE_SIZE;
			buf += PAGE_SIZE;
			phys += PAGE_SIZE;
		} while (offset < pagesize);

	} while (vaddr < end);

err:
	up_read(&pmm->mmap_sem);
	up_read(&mm->mmap_sem);
	return ret;
}

static int svm_proc_load_flag(int __user *arg)
{
	static atomic_t l2buf_load_flag = ATOMIC_INIT(0);
	int flag;

	if (arg == NULL)
		return -EINVAL;

	if (0 == (atomic_cmpxchg(&l2buf_load_flag, 0, 1)))
		flag = 0;
	else
		flag = 1;

	return put_user(flag, arg);
}

static unsigned long svm_get_unmapped_area(struct file *file,
		unsigned long addr0, unsigned long len,
		unsigned long pgoff, unsigned long flags)
{
	unsigned long addr = addr0;
	struct mm_struct *mm = current->mm;
	struct vm_unmapped_area_info info;
	struct svm_device *sdev = file_to_sdev(file);

	if (len != sdev->l2size) {
		dev_err(sdev->dev, "Just map the size of L2BUFF %ld\n",
			sdev->l2size);
		return -EINVAL; //lint !e570
	}

	if (flags & MAP_FIXED) {
		if (IS_ALIGNED(addr, len))
			return addr;

		dev_err(sdev->dev, "MAP_FIXED but not aligned\n");
		return -EINVAL; //lint !e570
	}

	if (addr) {
		struct vm_area_struct *vma = NULL;

		addr = ALIGN(addr, len);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr && addr >= mmap_min_addr &&
		   (vma == NULL || addr + len <= vm_start_gap(vma)))
			return addr;
	}

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	info.low_limit = max(PAGE_SIZE, mmap_min_addr);
	info.high_limit = mm->mmap_base;
	info.align_mask = ((len >> PAGE_SHIFT) - 1) << PAGE_SHIFT;
	info.align_offset = pgoff << PAGE_SHIFT;
	addr = vm_unmapped_area(&info);

	if (offset_in_page(addr)) {
		VM_BUG_ON(addr != -ENOMEM);
		info.flags = 0;
		info.low_limit = TASK_UNMAPPED_BASE;
		info.high_limit = TASK_SIZE;
		addr = vm_unmapped_area(&info);
	}

	return addr;
}

static int svm_mmap(struct file *file, struct vm_area_struct *vma)
{
	int err;
	struct svm_device *sdev = file_to_sdev(file);

	if ((vma->vm_end < vma->vm_start) ||
	    ((vma->vm_end - vma->vm_start) > sdev->l2size))
		return -EINVAL;

	vma->vm_page_prot = __pgprot((~PTE_SHARED) & vma->vm_page_prot.pgprot);

	err = remap_pfn_range(vma, vma->vm_start, sdev->l2buff >> PAGE_SHIFT,
			vma->vm_end - vma->vm_start,
			__pgprot(vma->vm_page_prot.pgprot | PTE_DIRTY));

	if (err)
		dev_err(sdev->dev, "fail to remap 0x%lx err = %d\n",
			vma->vm_start, err);

	return err;
}
#endif
/*svm ioctl will include some case for HI1980 and HI1910*/
static long svm_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	int err = -EINVAL;
	struct svm_bind_process params;
	struct svm_device *sdev = file_to_sdev(file);
	struct task_struct *task;

	if (!arg)
		return -EINVAL;

	if (cmd == SVM_IOCTL_PROCESS_BIND) {
		err = copy_from_user(&params, (void __user *)arg,
				sizeof(params));
		if (err) {
			dev_err(sdev->dev, "fail to copy params %d\n", err);
			return -EFAULT;
		}
	}

	switch (cmd) {
	case SVM_IOCTL_PROCESS_BIND:
		task = svm_get_task(params);
		if (IS_ERR(task)) {
			dev_err(sdev->dev, "failed to get task\n");
			return PTR_ERR(task);
		}

		err = svm_process_bind(task, sdev, &params.ttbr,
				&params.tcr, &params.pasid);
		if (err) {
			put_task_struct(task);
			dev_err(sdev->dev, "failed to bind task %d\n", err);
			return err;
		}

		put_task_struct(task);
		err = copy_to_user((void __user *)arg, &params,
				sizeof(params));
		if (err) {
			dev_err(sdev->dev, "failed to copy to user!\n");
			return -EFAULT;
		}
		break;
	case SVM_IOCTL_GET_PHYS:
		err = svm_get_phys((unsigned long __user *)arg);
		break;
#ifdef CONFIG_ACPI
	case SVM_IOCTL_SET_RC:
		err = svm_set_rc((unsigned long __user *)arg);
		break;
#else
	case SVM_IOCTL_GET_L2PTE_BASE:
		err = svm_get_l2pte_base(sdev, (unsigned long __user *)arg);
		break;
	case SVM_IOCTL_PIN_MEMORY:
		err = svm_pin_memory((unsigned long __user *)arg);
		break;
	case SVM_IOCTL_UNPIN_MEMORY:
		err = svm_unpin_memory((unsigned long __user *)arg);
		break;
	case SVM_IOCTL_GETHUGEINFO:
		err = svm_get_hugeinfo((unsigned long __user *)arg);
		break;
	case SVM_IOCTL_REMAP_PROC:
		err = svm_remap_proc((unsigned long __user *)arg);
		break;
	case SVM_IOCTL_LOAD_FLAG:
		err = svm_proc_load_flag((int __user *)arg);
		break;
#endif
	default:
			err = -EINVAL;
		}

		if (err)
			dev_err(sdev->dev, "%s: %s failed err = %d\n", __func__,
					svm_cmd_to_string(cmd), err);

	return err;
}

static const struct file_operations svm_fops = {
	.owner			= THIS_MODULE,
	.open			= svm_open,
#ifndef CONFIG_ACPI
	.mmap			= svm_mmap,
	.get_unmapped_area = svm_get_unmapped_area,
#endif
	.unlocked_ioctl		= svm_ioctl,
};

#ifndef CONFIG_ACPI
static int svm_setup_l2buff(struct svm_device *sdev, struct device_node *np)
{
	struct device_node *l2buff = of_parse_phandle(np, "memory-region", 0);

	if (l2buff) {
		struct resource r;
		int err = of_address_to_resource(l2buff, 0, &r);

		if (err) {
			of_node_put(l2buff);
			return err;
		}

		sdev->l2buff = r.start;
		sdev->l2size = resource_size(&r);
	}

	of_node_put(l2buff);
	return 0;
}
#endif

/*svm device probe this is init the svm device*/
static int svm_device_probe(struct platform_device *pdev)
{
	int err = -1;
	struct device *dev = &pdev->dev;
	struct svm_device *sdev = NULL;
#ifndef CONFIG_ACPI
	struct device_node *np = dev->of_node;
	int alias_id;

	if (np == NULL)
		return -ENODEV;
#endif

	if (!dev->bus->iommu_ops) {
		dev_dbg(dev, "defer probe svm device\n");
		return -EPROBE_DEFER;
	}

	sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
	if (sdev == NULL)
		return -ENOMEM;

#ifdef CONFIG_ACPI
	err = device_property_read_u64(dev, "svmid", &sdev->id);
	if (err) {
		dev_err(dev, "failed to get this svm device id\n");
		return err;
	}
#else
	alias_id = of_alias_get_id(np, "svm");
	if (alias_id < 0)
		sdev->id = probe_index;
	else
		sdev->id = alias_id;
#endif

	sdev->dev = dev;
	sdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	sdev->miscdev.fops = &svm_fops;
	sdev->miscdev.name = devm_kasprintf(dev, GFP_KERNEL,
			SVM_DEVICE_NAME"%llu", sdev->id);
	if (sdev->miscdev.name == NULL)
		err = -ENOMEM;

	dev_set_drvdata(dev, sdev);
	err = misc_register(&sdev->miscdev);
	if (err) {
		dev_err(dev, "Unable to register misc device\n");
		return err;
	}
#ifdef CONFIG_ACPI
	err = svm_init_core(sdev);
#else
	/*
	 * Get the l2buff phys address and size, if it do not exist
	 * just warn and continue, and runtime can not use L2BUFF.
	 */
	err = svm_setup_l2buff(sdev, np);
	if (err)
		dev_warn(dev, "Cannot get l2buff\n");

	err = svm_init_core(sdev, np);
#endif

	if (err) {
		dev_err(dev, "failed to init cores\n");
		goto err_unregister_misc;
	}

#ifndef CONFIG_ACPI
	probe_index++;
#endif

	return err;

err_unregister_misc:
	misc_deregister(&sdev->miscdev);

	return err;
}
/*svm device remove this is device remove*/
static int svm_device_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct svm_device *sdev = dev_get_drvdata(dev);

	device_for_each_child(sdev->dev, NULL, svm_remove_core);
	misc_deregister(&sdev->miscdev);

	return 0;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id svm_acpi_match[] = {
	{ "HSVM1980", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, svm_acpi_match);
#else
static const struct of_device_id svm_of_match[] = {
	{ .compatible = "hisilicon,svm" },
	{ }
};
MODULE_DEVICE_TABLE(of, svm_of_match);
#endif

/*svm acpi probe and remove*/
static struct platform_driver svm_driver = {
	.probe	=	svm_device_probe,
	.remove	=	svm_device_remove,
	.driver	=	{
		.name = SVM_DEVICE_NAME,
#ifdef CONFIG_ACPI
		.acpi_match_table = ACPI_PTR(svm_acpi_match),
#else
		.of_match_table = svm_of_match,
#endif
	},
};

module_platform_driver(svm_driver);

MODULE_DESCRIPTION("Hisilicon SVM driver");
MODULE_AUTHOR("JianKang Chen <chenjiankang1@huawei.com>");
MODULE_LICENSE("GPL v2");
