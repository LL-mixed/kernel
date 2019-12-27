// SPDX-License-Identifier: GPL-2.0+
/*
 * ktask.c - framework to parallelize CPU-intensive kernel work
 *
 * For more information, see Documentation/core-api/ktask.rst.
 *
 * Copyright (c) 2018 Oracle Corporation
 * Author: Daniel Jordan <daniel.m.jordan@oracle.com>
 */

#define pr_fmt(fmt)	"ktask: " fmt

#include <linux/ktask.h>

#ifdef CONFIG_KTASK

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

/* Resource limits on the amount of workqueue items queued through ktask. */
static DEFINE_SPINLOCK(ktask_rlim_lock);
/* Work items queued on all nodes (includes NUMA_NO_NODE) */
static size_t ktask_rlim_cur;
static size_t ktask_rlim_max;
/* Work items queued per node */
static size_t *ktask_rlim_node_cur;
static size_t *ktask_rlim_node_max;

/* Allow only 80% of the cpus to be running additional ktask threads. */
#define	KTASK_CPUFRAC_NUMER	4
#define	KTASK_CPUFRAC_DENOM	5

/* Used to pass ktask data to the workqueue API. */
struct ktask_work {
	struct work_struct	kw_work;
	struct ktask_task	*kw_task;
	int			kw_ktask_node_i;
	int			kw_queue_nid;
	struct list_head	kw_list;	/* ktask_free_works linkage */
};

static LIST_HEAD(ktask_free_works);
static struct ktask_work *ktask_works;

/* Represents one task.  This is for internal use only. */
struct ktask_task {
	struct ktask_ctl	kt_ctl;
	size_t			kt_total_size;
	size_t			kt_chunk_size;
	/* protects this struct and struct ktask_work's of a running task */
	struct mutex		kt_mutex;
	struct ktask_node	*kt_nodes;
	size_t			kt_nr_nodes;
	size_t			kt_nr_nodes_left;
	size_t			kt_nworks;
	size_t			kt_nworks_fini;
	int			kt_error; /* first error from thread_func */
	struct completion	kt_ktask_done;
};

/*
 * Shrink the size of each job by this shift amount to load balance between the
 * worker threads.
 */
#define	KTASK_LOAD_BAL_SHIFT		2

#define	KTASK_DEFAULT_MAX_THREADS	4

/* Maximum number of threads for a single task. */
int ktask_max_threads = KTASK_DEFAULT_MAX_THREADS;

static struct workqueue_struct *ktask_wq;
static struct workqueue_struct *ktask_nonuma_wq;

static void ktask_thread(struct work_struct *work);

static void ktask_init_work(struct ktask_work *kw, struct ktask_task *kt,
			    size_t ktask_node_i, size_t queue_nid)
{
	INIT_WORK(&kw->kw_work, ktask_thread);
	kw->kw_task = kt;
	kw->kw_ktask_node_i = ktask_node_i;
	kw->kw_queue_nid = queue_nid;
}

static void ktask_queue_work(struct ktask_work *kw)
{
	struct workqueue_struct *wq;
	int cpu;

	if (kw->kw_queue_nid == NUMA_NO_NODE) {
		/*
		 * If no node is specified, use ktask_nonuma_wq to
		 * allow the thread to run on any node, but fall back
		 * to ktask_wq if we couldn't allocate ktask_nonuma_wq.
		 */
		cpu = WORK_CPU_UNBOUND;
		wq = (ktask_nonuma_wq) ?: ktask_wq;
	} else {
		/*
		 * WQ_UNBOUND workqueues, such as the one ktask uses,
		 * execute work on some CPU from the node of the CPU we
		 * pass to queue_work_on, so just pick any CPU to stand
		 * for the node on NUMA systems.
		 *
		 * On non-NUMA systems, cpumask_of_node becomes
		 * cpu_online_mask.
		 */
		cpu = cpumask_any(cpumask_of_node(kw->kw_queue_nid));
		wq = ktask_wq;
	}

	WARN_ON(!queue_work_on(cpu, wq, &kw->kw_work));
}

/* Returns true if we're migrating this part of the task to another node. */
static bool ktask_node_migrate(struct ktask_node *old_kn, struct ktask_node *kn,
			       size_t ktask_node_i, struct ktask_work *kw,
			       struct ktask_task *kt)
{
	int new_queue_nid;

	/*
	 * Don't migrate a user thread, otherwise migrate only if we're going
	 * to a different node.
	 */
	if (!IS_ENABLED(CONFIG_NUMA) || !(current->flags & PF_KTHREAD) ||
	    kn->kn_nid == old_kn->kn_nid || num_online_nodes() == 1)
		return false;

	/* Adjust resource limits. */
	spin_lock(&ktask_rlim_lock);
	if (kw->kw_queue_nid != NUMA_NO_NODE)
		--ktask_rlim_node_cur[kw->kw_queue_nid];

	if (kn->kn_nid != NUMA_NO_NODE &&
	    ktask_rlim_node_cur[kw->kw_queue_nid] <
	    ktask_rlim_node_max[kw->kw_queue_nid]) {
		new_queue_nid = kn->kn_nid;
		++ktask_rlim_node_cur[new_queue_nid];
	} else {
		new_queue_nid = NUMA_NO_NODE;
	}
	spin_unlock(&ktask_rlim_lock);

	ktask_init_work(kw, kt, ktask_node_i, new_queue_nid);
	ktask_queue_work(kw);

	return true;
}

static void ktask_thread(struct work_struct *work)
{
	struct ktask_work  *kw = container_of(work, struct ktask_work, kw_work);
	struct ktask_task  *kt = kw->kw_task;
	struct ktask_ctl   *kc = &kt->kt_ctl;
	struct ktask_node  *kn = &kt->kt_nodes[kw->kw_ktask_node_i];
	bool               done;

	mutex_lock(&kt->kt_mutex);

	while (kt->kt_total_size > 0 && kt->kt_error == KTASK_RETURN_SUCCESS) {
		void *start, *end;
		size_t size;
		int ret;

		if (kn->kn_task_size == 0) {
			/* The current node is out of work; pick a new one. */
			size_t remaining_nodes_seen = 0;
			size_t new_idx = prandom_u32_max(kt->kt_nr_nodes_left);
			struct ktask_node *old_kn;
			size_t i;

			WARN_ON(kt->kt_nr_nodes_left == 0);
			WARN_ON(new_idx >= kt->kt_nr_nodes_left);
			for (i = 0; i < kt->kt_nr_nodes; ++i) {
				if (kt->kt_nodes[i].kn_task_size == 0)
					continue;

				if (remaining_nodes_seen >= new_idx)
					break;

				++remaining_nodes_seen;
			}
			/* We should have found work on another node. */
			WARN_ON(i >= kt->kt_nr_nodes);

			old_kn = kn;
			kn = &kt->kt_nodes[i];

			/* Start another worker on the node we've chosen. */
			if (ktask_node_migrate(old_kn, kn, i, kw, kt)) {
				mutex_unlock(&kt->kt_mutex);
				return;
			}
		}

		start = kn->kn_start;
		size = min(kt->kt_chunk_size, kn->kn_task_size);
		end = kc->kc_iter_func(start, size);
		kn->kn_start = end;
		kn->kn_task_size -= size;
		WARN_ON(kt->kt_total_size < size);
		kt->kt_total_size -= size;
		if (kn->kn_task_size == 0) {
			WARN_ON(kt->kt_nr_nodes_left == 0);
			kt->kt_nr_nodes_left--;
		}

		mutex_unlock(&kt->kt_mutex);

		ret = kc->kc_thread_func(start, end, kc->kc_func_arg);

		mutex_lock(&kt->kt_mutex);

		/* Save first error code only. */
		if (kt->kt_error == KTASK_RETURN_SUCCESS && ret != kt->kt_error)
			kt->kt_error = ret;
	}

	WARN_ON(kt->kt_nr_nodes_left > 0 &&
		kt->kt_error == KTASK_RETURN_SUCCESS);

	++kt->kt_nworks_fini;
	WARN_ON(kt->kt_nworks_fini > kt->kt_nworks);
	done = (kt->kt_nworks_fini == kt->kt_nworks);
	mutex_unlock(&kt->kt_mutex);

	if (done)
		complete(&kt->kt_ktask_done);
}

/*
 * Returns the number of chunks to break this task into.
 *
 * The number of chunks will be at least the number of works, but in the common
 * case of a large task, the number of chunks will be greater to load balance
 * between the workqueue threads in case some of them finish more quickly than
 * others.
 */
static size_t ktask_chunk_size(size_t task_size, size_t min_chunk_size,
			       size_t nworks)
{
	size_t chunk_size;

	if (nworks == 1)
		return task_size;

	chunk_size = (task_size / nworks) >> KTASK_LOAD_BAL_SHIFT;

	/*
	 * chunk_size should be a multiple of min_chunk_size for tasks that
	 * need to operate in fixed-size batches.
	 */
	if (chunk_size > min_chunk_size)
		chunk_size = rounddown(chunk_size, min_chunk_size);

	return max(chunk_size, min_chunk_size);
}

/*
 * Returns the number of works to be used in the task.  This number includes
 * the current thread, so a return value of 1 means no extra threads are
 * started.
 */
static size_t ktask_init_works(struct ktask_node *nodes, size_t nr_nodes,
			       struct ktask_task *kt,
			       struct list_head *works_list)
{
	size_t i, nr_works, nr_works_check;
	size_t min_chunk_size = kt->kt_ctl.kc_min_chunk_size;
	size_t max_threads    = kt->kt_ctl.kc_max_threads;

	if (!ktask_wq)
		return 1;

	if (max_threads == 0)
		max_threads = ktask_max_threads;

	/* Ensure at least one thread when task_size < min_chunk_size. */
	nr_works_check = DIV_ROUND_UP(kt->kt_total_size, min_chunk_size);
	nr_works_check = min_t(size_t, nr_works_check, num_online_cpus());
	nr_works_check = min_t(size_t, nr_works_check, max_threads);

	/*
	 * Use at least the current thread for this task; check whether
	 * ktask_rlim allows additional work items to be queued.
	 */
	nr_works = 1;
	spin_lock(&ktask_rlim_lock);
	for (i = nr_works; i < nr_works_check; ++i) {
		/* Allocate works evenly over the task's given nodes. */
		size_t ktask_node_i = i % nr_nodes;
		struct ktask_node *kn = &nodes[ktask_node_i];
		struct ktask_work *kw;
		int nid = kn->kn_nid;
		int queue_nid;

		WARN_ON(ktask_rlim_cur > ktask_rlim_max);
		if (ktask_rlim_cur == ktask_rlim_max)
			break;	/* No more work items allowed to be queued. */

		/* Allowed to queue on requested node? */
		if (nid != NUMA_NO_NODE &&
		    ktask_rlim_node_cur[nid] < ktask_rlim_node_max[nid]) {
			WARN_ON(ktask_rlim_node_cur[nid] > ktask_rlim_cur);
			++ktask_rlim_node_cur[nid];
			queue_nid = nid;
		} else {
			queue_nid = NUMA_NO_NODE;
		}

		WARN_ON(list_empty(&ktask_free_works));
		kw = list_first_entry(&ktask_free_works, struct ktask_work,
				      kw_list);
		list_move_tail(&kw->kw_list, works_list);
		ktask_init_work(kw, kt, ktask_node_i, queue_nid);

		++ktask_rlim_cur;
		++nr_works;
	}
	spin_unlock(&ktask_rlim_lock);

	return nr_works;
}

static void ktask_fini_works(struct ktask_task *kt,
			     struct list_head *works_list)
{
	struct ktask_work *work;

	spin_lock(&ktask_rlim_lock);

	/* Put the works back on the free list, adjusting rlimits. */
	list_for_each_entry(work, works_list, kw_list) {
		if (work->kw_queue_nid != NUMA_NO_NODE) {
			WARN_ON(ktask_rlim_node_cur[work->kw_queue_nid] == 0);
			--ktask_rlim_node_cur[work->kw_queue_nid];
		}
		WARN_ON(ktask_rlim_cur == 0);
		--ktask_rlim_cur;
	}
	list_splice(works_list, &ktask_free_works);

	spin_unlock(&ktask_rlim_lock);
}

int ktask_run_numa(struct ktask_node *nodes, size_t nr_nodes,
		   struct ktask_ctl *ctl)
{
	size_t i;
	struct ktask_work kw;
	struct ktask_work *work;
	LIST_HEAD(works_list);
	struct ktask_task kt = {
		.kt_ctl             = *ctl,
		.kt_total_size      = 0,
		.kt_nodes           = nodes,
		.kt_nr_nodes        = nr_nodes,
		.kt_nr_nodes_left   = nr_nodes,
		.kt_nworks_fini     = 0,
		.kt_error           = KTASK_RETURN_SUCCESS,
	};

	for (i = 0; i < nr_nodes; ++i) {
		kt.kt_total_size += nodes[i].kn_task_size;
		if (nodes[i].kn_task_size == 0)
			kt.kt_nr_nodes_left--;

		WARN_ON(nodes[i].kn_nid >= MAX_NUMNODES);
	}

	if (kt.kt_total_size == 0)
		return KTASK_RETURN_SUCCESS;

	mutex_init(&kt.kt_mutex);
	init_completion(&kt.kt_ktask_done);

	kt.kt_nworks = ktask_init_works(nodes, nr_nodes, &kt, &works_list);
	kt.kt_chunk_size = ktask_chunk_size(kt.kt_total_size,
					    ctl->kc_min_chunk_size,
					    kt.kt_nworks);

	list_for_each_entry(work, &works_list, kw_list)
		ktask_queue_work(work);

	/* Use the current thread, which saves starting a workqueue worker. */
	ktask_init_work(&kw, &kt, 0, nodes[0].kn_nid);
	ktask_thread(&kw.kw_work);

	/* Wait for all the jobs to finish. */
	wait_for_completion(&kt.kt_ktask_done);

	ktask_fini_works(&kt, &works_list);
	mutex_destroy(&kt.kt_mutex);

	return kt.kt_error;
}
EXPORT_SYMBOL_GPL(ktask_run_numa);

int ktask_run(void *start, size_t task_size, struct ktask_ctl *ctl)
{
	struct ktask_node node;

	node.kn_start = start;
	node.kn_task_size = task_size;
	node.kn_nid = numa_node_id();

	return ktask_run_numa(&node, 1, ctl);
}
EXPORT_SYMBOL_GPL(ktask_run);

/*
 * Initialize internal limits on work items queued.  Work items submitted to
 * cmwq capped at 80% of online cpus both system-wide and per-node to maintain
 * an efficient level of parallelization at these respective levels.
 */
static bool __init ktask_rlim_init(void)
{
	int node, nr_cpus;
	unsigned int nr_node_cpus;

	nr_cpus = num_online_cpus();

	/* XXX Handle CPU hotplug. */
	if (nr_cpus == 1)
		return false;

	ktask_rlim_node_cur = kcalloc(num_possible_nodes(), sizeof(size_t),
				      GFP_KERNEL);

	ktask_rlim_node_max = kmalloc_array(num_possible_nodes(),
					    sizeof(size_t), GFP_KERNEL);

	ktask_rlim_max = mult_frac(nr_cpus, KTASK_CPUFRAC_NUMER,
				   KTASK_CPUFRAC_DENOM);
	for_each_node(node) {
		nr_node_cpus = cpumask_weight(cpumask_of_node(node));
		ktask_rlim_node_max[node] = mult_frac(nr_node_cpus,
						      KTASK_CPUFRAC_NUMER,
						      KTASK_CPUFRAC_DENOM);
	}

	return true;
}

void __init ktask_init(void)
{
	struct workqueue_attrs *attrs;
	int i, ret;

	if (!ktask_rlim_init())
		goto out;

	ktask_works = kmalloc_array(ktask_rlim_max, sizeof(struct ktask_work),
				    GFP_KERNEL);
	for (i = 0; i < ktask_rlim_max; ++i)
		list_add_tail(&ktask_works[i].kw_list, &ktask_free_works);

	ktask_wq = alloc_workqueue("ktask_wq", WQ_UNBOUND, 0);
	if (!ktask_wq) {
		pr_warn("disabled (failed to alloc ktask_wq)");
		goto out;
	}

	/*
	 * Threads executing work from this workqueue can run on any node on
	 * the system.  If we get any failures below, use ktask_wq in its
	 * place.  It's better than nothing.
	 */
	ktask_nonuma_wq = alloc_workqueue("ktask_nonuma_wq", WQ_UNBOUND, 0);
	if (!ktask_nonuma_wq) {
		pr_warn("disabled (failed to alloc ktask_nonuma_wq)");
		goto alloc_fail;
	}

	attrs = alloc_workqueue_attrs(GFP_KERNEL);
	if (!attrs) {
		pr_warn("disabled (couldn't alloc wq attrs)");
		goto alloc_fail;
	}

	attrs->no_numa = true;

	ret = apply_workqueue_attrs(ktask_nonuma_wq, attrs);
	if (ret != 0) {
		pr_warn("disabled (couldn't apply attrs to ktask_nonuma_wq)");
		goto apply_fail;
	}

	free_workqueue_attrs(attrs);
out:
	return;

apply_fail:
	free_workqueue_attrs(attrs);
alloc_fail:
	if (ktask_wq)
		destroy_workqueue(ktask_wq);
	if (ktask_nonuma_wq)
		destroy_workqueue(ktask_nonuma_wq);
	ktask_wq = NULL;
	ktask_nonuma_wq = NULL;
}

#endif /* CONFIG_KTASK */

/*
 * This function is defined outside CONFIG_KTASK so it can be called in the
 * !CONFIG_KTASK versions of ktask_run and ktask_run_numa.
 */
void *ktask_iter_range(void *position, size_t size)
{
	return (char *)position + size;
}
EXPORT_SYMBOL_GPL(ktask_iter_range);
