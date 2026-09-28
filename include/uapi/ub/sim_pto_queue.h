/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_UB_SIM_PTO_QUEUE_H
#define _UAPI_UB_SIM_PTO_QUEUE_H

#if defined(__linux__) || defined(__KERNEL__)
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __u32 ub_sim_pto_u32;
typedef __u64 ub_sim_pto_u64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t ub_sim_pto_u32;
typedef uint64_t ub_sim_pto_u64;
#endif

#define UB_SIM_PTO_QUEUE_ABI_VERSION 1U
#define UB_SIM_PTO_QUEUE_DEVICE "/dev/ub_sim_pto_queue"

struct ub_sim_pto_queue_info {
	ub_sim_pto_u32 version;
	ub_sim_pto_u32 page_bytes;
	ub_sim_pto_u64 cmdq_pa;
	ub_sim_pto_u64 cq_pa;
};

#define UB_SIM_PTO_QUEUE_GET_INFO \
	_IOR('Q', 0x01, struct ub_sim_pto_queue_info)

#endif
