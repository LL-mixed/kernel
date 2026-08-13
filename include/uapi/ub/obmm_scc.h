/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UAPI_UB_OBMM_SCC_H
#define UAPI_UB_OBMM_SCC_H

#include <linux/const.h>
#include <linux/ioctl.h>
#include <linux/types.h>

#define OBMM_SCC_ABI_VERSION 2
#define OBMM_SCC_MAX_CONTEXTS 64
#define OBMM_SCC_MAX_PENDING_LOADS 64
#define OBMM_SCC_MAX_EVENTS 128
#define OBMM_SCC_CONTEXT_STATE_BYTES 832
#define OBMM_SCC_RESUME_HLT_IMM 0x5343

#define OBMM_SCC_CAP_SCALAR_1		_BITULL(0)
#define OBMM_SCC_CAP_SCALAR_2		_BITULL(1)
#define OBMM_SCC_CAP_SCALAR_4		_BITULL(2)
#define OBMM_SCC_CAP_SCALAR_8		_BITULL(3)
#define OBMM_SCC_CAP_XZR		_BITULL(4)
#define OBMM_SCC_CAP_DIRECT_EL0_UPCALL	_BITULL(5)
#define OBMM_SCC_CAP_EL0_RESUME		_BITULL(6)
#define OBMM_SCC_CAP_FULL_CONTEXT	_BITULL(7)

#define OBMM_SCC_MAP_LOGICAL_MIXED	_BITUL(0)
#define OBMM_SCC_EVENT_GET_WAIT		_BITUL(0)

enum obmm_scc_event_kind {
	OBMM_SCC_EVENT_PENDING = 1,
	OBMM_SCC_EVENT_COMPLETE = 2,
	OBMM_SCC_EVENT_FAULT = 3,
	OBMM_SCC_EVENT_OWNER_STOP = 4,
};

enum obmm_scc_status {
	OBMM_SCC_STATUS_SUCCESS = 0,
	OBMM_SCC_STATUS_TIMEOUT = 1,
	OBMM_SCC_STATUS_PERMISSION = 2,
	OBMM_SCC_STATUS_STALE_MAP = 3,
	OBMM_SCC_STATUS_REMOTE_IO = 4,
	OBMM_SCC_STATUS_CANCELLED = 5,
	OBMM_SCC_STATUS_INTERNAL = 6,
};

/*
 * Exact state image consumed by the simulated HLT #0x5343 resume primitive.
 * The guest EL0 scheduler owns these images. QEMU only installs the selected
 * image atomically; it neither stores nor chooses coroutine contexts.
 */
struct obmm_scc_context_v2 {
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

struct obmm_scc_caps_v2 {
	__u32 abi_version;
	__u16 context_entries;
	__u16 pending_load_entries;
	__u16 event_queue_depth;
	__u16 reserved0;
	__u32 context_state_bytes;
	__u64 capabilities;
	__u64 owner_generation;
	__u32 clock_mhz;
	__u32 resume_hlt_imm;
	__u64 reserved[3];
};

struct obmm_scc_map_register_v1 {
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

struct obmm_scc_map_unregister_v1 {
	__u64 policy_id;
	__u64 map_generation;
};

struct obmm_scc_start_v2 {
	__u32 home_cpu;
	__u32 flags;
	__u64 owner_generation;
	__u64 load_timeout_ns;
	__u64 upcall_entry;
	__u32 logical_contexts;
	__u32 reserved0;
};

struct obmm_scc_event_v2 {
	__u64 sequence;
	__u64 context_id;
	__u64 plt_token;
	__u64 interrupted_pc;
	__u64 fault_pc;
	__u64 effective_va;
	__u64 value;
	__u32 kind;
	__u32 status;
	__u16 rt;
	__u16 access_bytes;
	__u32 flags;
};

struct obmm_scc_stats_v2 {
	__u64 pending_loads;
	__u64 completed_loads;
	/* Completion events handed to EL0; QEMU does not commit architectural state. */
	__u64 completion_events_delivered;
	__u64 faulted_loads;
	__u64 stale_completions;
	__u64 duplicate_completions;
	/* QEMU-owned context counters remain zero in ABI v2. */
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

struct obmm_scc_observability_v2 {
	__u64 abi_version;
	__u64 scc_pending_current;
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
	/* These four QEMU scheduler-cycle counters are zero in ABI v2. */
	__u64 save_cycles;
	__u64 schedule_cycles;
	__u64 restore_cycles;
	__u64 commit_cycles;
	__u64 logical_contexts;
	__u64 direct_upcalls;
};

#define OBMM_SCC_IOCTL_MAGIC 0xb8
#define OBMM_SCC_IOCTL_QUERY_CAPS \
	_IOR(OBMM_SCC_IOCTL_MAGIC, 0x00, struct obmm_scc_caps_v2)
#define OBMM_SCC_IOCTL_REGISTER_MAP \
	_IOWR(OBMM_SCC_IOCTL_MAGIC, 0x01, struct obmm_scc_map_register_v1)
#define OBMM_SCC_IOCTL_UNREGISTER_MAP \
	_IOW(OBMM_SCC_IOCTL_MAGIC, 0x02, struct obmm_scc_map_unregister_v1)
#define OBMM_SCC_IOCTL_START \
	_IOWR(OBMM_SCC_IOCTL_MAGIC, 0x06, struct obmm_scc_start_v2)
#define OBMM_SCC_IOCTL_STOP _IO(OBMM_SCC_IOCTL_MAGIC, 0x07)
#define OBMM_SCC_IOCTL_GET_STATS \
	_IOR(OBMM_SCC_IOCTL_MAGIC, 0x08, struct obmm_scc_stats_v2)
#define OBMM_SCC_IOCTL_GET_OBSERVABILITY \
	_IOR(OBMM_SCC_IOCTL_MAGIC, 0x0c, struct obmm_scc_observability_v2)
#define OBMM_SCC_IOCTL_GET_EVENT \
	_IOWR(OBMM_SCC_IOCTL_MAGIC, 0x0d, struct obmm_scc_event_v2)

#endif
