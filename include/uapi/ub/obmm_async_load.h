/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UAPI_UB_OBMM_ASYNC_LOAD_H
#define UAPI_UB_OBMM_ASYNC_LOAD_H

#include <linux/const.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define OBMM_ASYNC_LOAD_ABI_VERSION 4
#define OBMM_ASYNC_LOAD_EVENT_ABI_VERSION 3
#define OBMM_ASYNC_LOAD_MAX_CONTEXTS 64
#define OBMM_ASYNC_LOAD_MAX_PENDING_LOADS 64
#define OBMM_ASYNC_LOAD_MAX_EVENTS 128
#define OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES 832
#define OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES 64
#define OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES 128
#define OBMM_ASYNC_LOAD_EVENT_CONSUMER_BYTES 64
#define OBMM_ASYNC_LOAD_RESUME_SVC_IMM 0x5343
#define OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM 0x5345

#define OBMM_ASYNC_LOAD_CAP_SCALAR_1		_BITULL(0)
#define OBMM_ASYNC_LOAD_CAP_SCALAR_2		_BITULL(1)
#define OBMM_ASYNC_LOAD_CAP_SCALAR_4		_BITULL(2)
#define OBMM_ASYNC_LOAD_CAP_SCALAR_8		_BITULL(3)
#define OBMM_ASYNC_LOAD_CAP_XZR		_BITULL(4)
#define OBMM_ASYNC_LOAD_CAP_DIRECT_EL0_UPCALL	_BITULL(5)
#define OBMM_ASYNC_LOAD_CAP_EL0_RESUME		_BITULL(6)
#define OBMM_ASYNC_LOAD_CAP_FULL_CONTEXT	_BITULL(7)
#define OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE	_BITULL(8)
#define OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING _BITULL(9)
#define OBMM_ASYNC_LOAD_CAP_EL0_WAIT_WAKE	_BITULL(10)
#define OBMM_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER _BITULL(11)
#define OBMM_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY	_BITULL(12)
#define OBMM_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN	_BITULL(13)
#define OBMM_ASYNC_LOAD_CAP_SVC_CONTEXT_RESUME	_BITULL(14)
#define OBMM_ASYNC_LOAD_CAP_WFE_WAIT		_BITULL(15)
#define OBMM_ASYNC_LOAD_CAP_CACHEABLE_FILL_REPLAY _BITULL(16)

#define OBMM_ASYNC_LOAD_MAP_LOGICAL_MIXED	_BITUL(0)
#define OBMM_ASYNC_LOAD_EVENT_RETIRE_REPLAY	_BITUL(1)
#define OBMM_ASYNC_LOAD_EVENT_CACHEABLE_FILL	_BITUL(2)
#define OBMM_ASYNC_LOAD_START_REPLAY_RETIRE	_BITUL(0)
#define OBMM_ASYNC_LOAD_START_KERNEL_TASK	_BITUL(1)

enum obmm_async_load_event_kind {
	OBMM_ASYNC_LOAD_EVENT_PENDING = 1,
	OBMM_ASYNC_LOAD_EVENT_COMPLETE = 2,
	OBMM_ASYNC_LOAD_EVENT_FAULT = 3,
	OBMM_ASYNC_LOAD_EVENT_OWNER_STOP = 4,
};

enum obmm_async_load_status {
	OBMM_ASYNC_LOAD_STATUS_SUCCESS = 0,
	OBMM_ASYNC_LOAD_STATUS_TIMEOUT = 1,
	OBMM_ASYNC_LOAD_STATUS_PERMISSION = 2,
	OBMM_ASYNC_LOAD_STATUS_STALE_MAP = 3,
	OBMM_ASYNC_LOAD_STATUS_REMOTE_IO = 4,
	OBMM_ASYNC_LOAD_STATUS_CANCELLED = 5,
	OBMM_ASYNC_LOAD_STATUS_INTERNAL = 6,
};

/*
 * Exact state image consumed by the private SVC resume fast path. The guest
 * EL0 scheduler owns these images. The driver installs GPR/SP/PC/NZCV through
 * pt_regs; EL0 restores SIMD/FP/TPIDR state immediately before SVC.
 */
struct obmm_async_load_context_v2 {
	__u64 context_id;
	__u64 flags;
	__u64 x[31];
	__u64 sp;
	__u64 pc;
	__u64 nzcv;
	__u64 q[32][2];
	__u64 fpcr;
	__u64 fpsr;
	__u64 tpidr_el0;
	__u64 reserved;
};

struct obmm_async_load_caps_v4 {
	__u32 abi_version;
	__u16 context_entries;
	__u16 pending_load_entries;
	__u16 event_queue_depth;
	__u16 reserved0;
	__u32 context_state_bytes;
	__u64 capabilities;
	__u64 owner_generation;
	__u32 clock_mhz;
	__u32 resume_svc_imm;
	__u32 scheduler_enter_svc_imm;
	__u32 reserved1;
	__u32 event_slot_bytes;
	__u32 event_producer_header_bytes;
	__u64 event_ring_mmap_offset;
	__u64 event_ring_mmap_bytes;
	__u64 event_consumer_mmap_offset;
	__u64 event_consumer_mmap_bytes;
	__u64 reserved[3];
};

struct obmm_async_load_map_register_v1 {
	__u64 mem_id;
	__u64 gsva_base;
	__u64 mapped_addr;
	__u64 length;
	__u32 flags;
	__s32 mapping_fd;
	__u64 policy_id;
	__u64 map_generation;
	__u64 model_phase_generation;
};

struct obmm_async_load_map_unregister_v1 {
	__u64 policy_id;
	__u64 map_generation;
};

struct obmm_async_load_start_v3 {
	__u32 home_cpu;
	__u32 flags;
	__u64 owner_generation;
	__u64 load_timeout_ns;
	__u64 upcall_entry;
	__u32 logical_contexts;
	__u32 reserved0;
};

struct obmm_async_load_event_producer_v3 {
	__u32 abi_version;
	__u16 event_depth;
	__u16 event_slot_bytes;
	__u64 owner_generation;
	__u64 producer_sequence;
	__u64 published_events;
	__u64 wait_wakeups;
	__u64 reserved[3];
};

struct obmm_async_load_event_consumer_v3 {
	__u32 abi_version;
	__u32 flags;
	__u64 owner_generation;
	__u64 consumer_sequence;
	__u64 wait_count;
	__u64 scheduler_enter_count;
	__u64 reserved[3];
};

struct obmm_async_load_event_v3 {
	__u64 sequence;
	__u64 owner_generation;
	__u64 context_id;
	/* ABI v3 field name; carries the generic wait_key for Cacheable fills. */
	__u64 plt_token;
	__u64 interrupted_pc;
	__u64 fault_pc;
	__u64 effective_va;
	__u64 value;
	__u64 map_id;
	__u64 map_generation;
	__u64 model_phase_generation;
	__u32 kind;
	__u32 status;
	__u16 rt;
	__u16 access_bytes;
	__u32 flags;
	__u64 reserved[3];
};

struct obmm_async_load_stats_v3 {
	__u64 pending_loads;
	__u64 completed_loads;
	/* Completion events handed to EL0; QEMU does not commit architectural state. */
	__u64 completion_events_delivered;
	__u64 faulted_loads;
	__u64 stale_completions;
	__u64 duplicate_completions;
	/* QEMU-owned context counters remain zero in ABI v3. */
	__u64 context_saves;
	__u64 context_restores;
	__u64 context_switches;
	__u64 context_bytes_moved;
	__u64 modeled_cycles;
	__u64 capacity_stalls;
	__u64 no_ready_idle;
	__u64 event_overflow;
	__u64 pending_high_water;
	__u64 ready_high_water;
	__u64 event_high_water;
	__u64 direct_upcalls;
	__u64 fail_stop;
};

struct obmm_async_load_observability_v3 {
	__u64 abi_version;
	__u64 async_load_pending_current;
	__u64 backend_pending_current;
	__u64 backend_accepted;
	__u64 backend_rejected;
	__u64 backend_delivered;
	__u64 backend_late;
	__u64 backend_duplicate;
	__u64 backend_capacity;
	__u64 backend_pending_high_water;
	__u64 backend_sink_copy_bytes;
	__u64 backend_sink_copy_ns;
	/* These four QEMU scheduler-cycle counters are zero in ABI v3. */
	__u64 save_cycles;
	__u64 schedule_cycles;
	__u64 restore_cycles;
	__u64 commit_cycles;
	__u64 logical_contexts;
	__u64 direct_upcalls;
};

struct obmm_async_load_replay_stats_v1 {
	__u64 replay_consumed;
	__u64 replay_mismatch;
	__u64 replay_ready_high_water;
	__u64 reserved;
};

struct obmm_async_load_kernel_task_stats_v1 {
	__u64 faults;
	__u64 pending_events;
	__u64 completion_events;
	__u64 task_sleeps;
	__u64 task_wakeups;
	__u64 protocol_errors;
	__u64 timeouts;
	__u64 interrupted_waits;
};

struct obmm_async_load_path_stats_v1 {
	__u64 nc_plt_allocations;
	__u64 nc_plt_pending_current;
	__u64 cacheable_fill_pending;
	__u64 cacheable_fill_completed;
	__u64 cacheable_replay_hits;
	__u64 cacheable_fill_bytes;
};

#define OBMM_ASYNC_LOAD_IOCTL_MAGIC 0xb8
#define OBMM_ASYNC_LOAD_IOCTL_QUERY_CAPS \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x00, struct obmm_async_load_caps_v4)
#define OBMM_ASYNC_LOAD_IOCTL_REGISTER_MAP \
	_IOWR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x01, struct obmm_async_load_map_register_v1)
#define OBMM_ASYNC_LOAD_IOCTL_UNREGISTER_MAP \
	_IOW(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x02, struct obmm_async_load_map_unregister_v1)
#define OBMM_ASYNC_LOAD_IOCTL_START \
	_IOWR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x06, struct obmm_async_load_start_v3)
#define OBMM_ASYNC_LOAD_IOCTL_STOP _IO(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x07)
#define OBMM_ASYNC_LOAD_IOCTL_GET_STATS \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x08, struct obmm_async_load_stats_v3)
#define OBMM_ASYNC_LOAD_IOCTL_GET_OBSERVABILITY \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x0c, struct obmm_async_load_observability_v3)
#define OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x0f, struct obmm_async_load_replay_stats_v1)
#define OBMM_ASYNC_LOAD_IOCTL_GET_KERNEL_TASK_STATS \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x10, \
	     struct obmm_async_load_kernel_task_stats_v1)
#define OBMM_ASYNC_LOAD_IOCTL_GET_PATH_STATS \
	_IOR(OBMM_ASYNC_LOAD_IOCTL_MAGIC, 0x11, \
	     struct obmm_async_load_path_stats_v1)

#endif
