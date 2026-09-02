/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UAPI_UB_OBMM_ASYNC_H
#define UAPI_UB_OBMM_ASYNC_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define OBMM_ASYNC_ABI_VERSION 1
#define OBMM_ASYNC_QUEUE_DEPTH 64
#define OBMM_ASYNC_SLOT_BYTES 64
#define OBMM_ASYNC_MAX_BUFFER_BYTES 65536
#define OBMM_ASYNC_MAX_BUFFERS 64

#define OBMM_ASYNC_SQ_READ 1
#define OBMM_ASYNC_CQ_READ_COMPLETE 0x81

enum obmm_async_status {
	OBMM_ASYNC_STATUS_OK = 0,
	OBMM_ASYNC_STATUS_INVALID = 1,
	OBMM_ASYNC_STATUS_NO_MAP = 2,
	OBMM_ASYNC_STATUS_BOUNDS = 3,
	OBMM_ASYNC_STATUS_PERMISSION = 4,
	OBMM_ASYNC_STATUS_STALE = 5,
	OBMM_ASYNC_STATUS_RETIRED = 6,
	OBMM_ASYNC_STATUS_TIMEOUT = 7,
	OBMM_ASYNC_STATUS_REMOTE_IO = 8,
	OBMM_ASYNC_STATUS_CHECKSUM = 9,
	OBMM_ASYNC_STATUS_CANCELLED = 10,
	OBMM_ASYNC_STATUS_UNSUPPORTED = 11,
};

struct obmm_async_sq_entry_v1 {
	__u16 abi_version;
	__u8 opcode;
	__u8 flags;
	__u32 length;
	__u64 token;
	__u64 map_id;
	__u64 map_generation;
	__u64 remote_offset;
	__u32 dst_buffer_id;
	__u32 dst_offset;
	__u64 deadline_ns;
	__u64 user_data;
} __attribute__((packed, aligned(8)));

struct obmm_async_cq_entry_v1 {
	__u16 abi_version;
	__u8 opcode;
	__u8 flags;
	__s32 status;
	__u64 token;
	__u64 user_data;
	__u32 bytes_done;
	__u32 provider_status;
	__u64 checksum64;
	__u64 completed_ns;
	__u64 map_generation;
	__u64 reserved;
} __attribute__((packed, aligned(8)));

struct obmm_async_info_v1 {
	__u32 abi_version;
	__u32 queue_id;
	__u32 queue_depth;
	__u32 slot_bytes;
	__u64 queue_mmap_offset;
	__u64 queue_mmap_bytes;
	__u64 buffer_mmap_offset;
	__u64 buffer_mmap_bytes;
	__u64 capabilities;
};

struct obmm_async_buffer_alloc_v1 {
	__u64 length;
	__u32 buffer_id;
	__u32 generation;
	__u64 arena_offset;
};

struct obmm_async_buffer_free_v1 {
	__u32 buffer_id;
	__u32 generation;
};

struct obmm_async_map_register_v1 {
	__u64 mem_id;
	__u64 mapped_addr;
	__u64 length;
	__u32 flags;
	__u32 reserved;
	__u64 map_id;
	__u64 map_generation;
};

struct obmm_async_map_register_v2 {
	__u64 mem_id;
	__u64 mapped_addr;
	__u64 length;
	__u32 flags;
	__u32 reserved;
	__u64 map_id;
	__u64 map_generation;
	__u64 local_pa;
};

struct obmm_async_map_unregister_v1 {
	__u64 map_id;
	__u64 map_generation;
};

struct obmm_async_kick_v1 {
	__u64 sq_tail;
	__u64 cq_head;
	__u64 cq_tail;
	__u64 last_error;
	__u64 guest_monotonic_ns;
	__u64 sq_head;
};

struct obmm_async_cancel_v1 {
	__u64 token;
};

struct obmm_async_observability_v1 {
	__u64 model_accepted;
	__u64 model_service_ns;
	__u64 model_accept_publish_ns;
	__u64 model_pending;
	__u64 model_completed;
	__u64 model_capacity_rejected;
	__u64 model_dropped;
	__u64 model_errored;
	__u64 model_duplicated;
	__u64 model_published;
	__u64 model_duplicate_published;
	__u64 backend_accepted;
	__u64 backend_rejected;
	__u64 backend_delivered;
	__u64 backend_late;
	__u64 backend_duplicate;
	__u64 backend_capacity;
	__u64 backend_pending;
	__u64 backend_pending_high_water;
	__u64 backend_sink_copy_bytes;
	__u64 backend_sink_copy_ns;
};

#define OBMM_ASYNC_IOCTL_MAGIC 0xb7
#define OBMM_ASYNC_IOCTL_GET_INFO \
	_IOR(OBMM_ASYNC_IOCTL_MAGIC, 0x00, struct obmm_async_info_v1)
#define OBMM_ASYNC_IOCTL_BUFFER_ALLOC \
	_IOWR(OBMM_ASYNC_IOCTL_MAGIC, 0x01, struct obmm_async_buffer_alloc_v1)
#define OBMM_ASYNC_IOCTL_BUFFER_FREE \
	_IOW(OBMM_ASYNC_IOCTL_MAGIC, 0x02, struct obmm_async_buffer_free_v1)
#define OBMM_ASYNC_IOCTL_MAP_REGISTER \
	_IOWR(OBMM_ASYNC_IOCTL_MAGIC, 0x03, struct obmm_async_map_register_v1)
#define OBMM_ASYNC_IOCTL_MAP_UNREGISTER \
	_IOW(OBMM_ASYNC_IOCTL_MAGIC, 0x04, struct obmm_async_map_unregister_v1)
#define OBMM_ASYNC_IOCTL_KICK \
	_IOWR(OBMM_ASYNC_IOCTL_MAGIC, 0x05, struct obmm_async_kick_v1)
#define OBMM_ASYNC_IOCTL_CANCEL \
	_IOW(OBMM_ASYNC_IOCTL_MAGIC, 0x06, struct obmm_async_cancel_v1)
#define OBMM_ASYNC_IOCTL_GET_OBSERVABILITY \
	_IOR(OBMM_ASYNC_IOCTL_MAGIC, 0x07, \
	     struct obmm_async_observability_v1)
#define OBMM_ASYNC_IOCTL_RESET_OBSERVABILITY \
	_IO(OBMM_ASYNC_IOCTL_MAGIC, 0x08)
#define OBMM_ASYNC_IOCTL_MAP_REGISTER_V2 \
	_IOWR(OBMM_ASYNC_IOCTL_MAGIC, 0x09, struct obmm_async_map_register_v2)

#endif
