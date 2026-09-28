// SPDX-License-Identifier: GPL-2.0+
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/ub/sim_pto_queue.h>

struct ub_sim_pto_queue {
	struct page *pages;
};

static int ub_sim_pto_queue_open(struct inode *inode, struct file *file)
{
	struct ub_sim_pto_queue *queue;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	if (!queue)
		return -ENOMEM;
	queue->pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (!queue->pages) {
		kfree(queue);
		return -ENOMEM;
	}
	file->private_data = queue;
	return 0;
}

static int ub_sim_pto_queue_release(struct inode *inode, struct file *file)
{
	struct ub_sim_pto_queue *queue = file->private_data;

	if (queue) {
		__free_pages(queue->pages, 1);
		kfree(queue);
	}
	return 0;
}

static long ub_sim_pto_queue_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct ub_sim_pto_queue *queue = file->private_data;
	struct ub_sim_pto_queue_info info = {
		.version = UB_SIM_PTO_QUEUE_ABI_VERSION,
		.page_bytes = PAGE_SIZE,
		.cmdq_pa = page_to_phys(queue->pages),
		.cq_pa = page_to_phys(queue->pages) + PAGE_SIZE,
	};

	if (cmd != UB_SIM_PTO_QUEUE_GET_INFO)
		return -ENOTTY;
	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int ub_sim_pto_queue_mmap(struct file *file,
				 struct vm_area_struct *vma)
{
	struct ub_sim_pto_queue *queue = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || size != 2 * PAGE_SIZE ||
	    !(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start, page_to_pfn(queue->pages),
			       size, vma->vm_page_prot);
}

static const struct file_operations ub_sim_pto_queue_fops = {
	.owner = THIS_MODULE,
	.open = ub_sim_pto_queue_open,
	.release = ub_sim_pto_queue_release,
	.unlocked_ioctl = ub_sim_pto_queue_ioctl,
	.compat_ioctl = ub_sim_pto_queue_ioctl,
	.mmap = ub_sim_pto_queue_mmap,
	.llseek = no_llseek,
};

static struct miscdevice ub_sim_pto_queue_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ub_sim_pto_queue",
	.fops = &ub_sim_pto_queue_fops,
};

int ub_sim_pto_queue_register(void)
{
	return misc_register(&ub_sim_pto_queue_device);
}

void ub_sim_pto_queue_unregister(void)
{
	misc_deregister(&ub_sim_pto_queue_device);
}
