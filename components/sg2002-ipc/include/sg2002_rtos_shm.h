#pragma once

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/stddef.h>
#include <linux/types.h>
typedef __u8 sg2002_rtos_u8;
typedef __u16 sg2002_rtos_u16;
typedef __u32 sg2002_rtos_u32;
typedef __aligned_u64 sg2002_rtos_aligned_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t sg2002_rtos_u8;
typedef uint16_t sg2002_rtos_u16;
typedef uint32_t sg2002_rtos_u32;
typedef uint64_t sg2002_rtos_aligned_u64 __attribute__((aligned(8)));
#ifdef __linux__
#include <sys/ioctl.h>
#endif
#endif

#define SG2002_RTOS_SHM_MAGIC 0x53475251U
#define SG2002_RTOS_SHM_ABI_VERSION 2U
#define SG2002_RTOS_SHM_FEATURE_GENERATION (1U << 0)
#define SG2002_RTOS_SHM_FEATURES SG2002_RTOS_SHM_FEATURE_GENERATION

#define SG2002_RTOS_SHM_CACHE_LINE_SIZE 64U
#define SG2002_RTOS_SHM_CONTROL_SIZE 0x1000U
#define SG2002_RTOS_SHM_SLOT_SIZE 1024U
#define SG2002_RTOS_SHM_SLOT_COUNT 64U
#define SG2002_RTOS_SHM_SLOT_HEADER_SIZE 6U
#define SG2002_RTOS_SHM_PAYLOAD_SIZE \
	(SG2002_RTOS_SHM_SLOT_SIZE - SG2002_RTOS_SHM_SLOT_HEADER_SIZE)
#define SG2002_RTOS_SHM_REQUEST_OFFSET SG2002_RTOS_SHM_CONTROL_SIZE
#define SG2002_RTOS_SHM_RING_SIZE \
	(SG2002_RTOS_SHM_SLOT_SIZE * SG2002_RTOS_SHM_SLOT_COUNT)
#define SG2002_RTOS_SHM_RESPONSE_OFFSET \
	(SG2002_RTOS_SHM_REQUEST_OFFSET + SG2002_RTOS_SHM_RING_SIZE)
#define SG2002_RTOS_SHM_REGION_SIZE \
	(SG2002_RTOS_SHM_RESPONSE_OFFSET + SG2002_RTOS_SHM_RING_SIZE)

/* The final 7-bit CMDQU value is reserved for transport notifications. */
#define SG2002_RTOS_SHM_DOORBELL_COMMAND 0x7fU

#define SG2002_RTOS_SHM_STATE_OFFLINE 0U
#define SG2002_RTOS_SHM_STATE_READY 1U
#define SG2002_RTOS_SHM_STATE_ERROR 2U

#define SG2002_RTOS_SHM_DEFAULT_TIMEOUT_MS 3000U

struct sg2002_rtos_shm_slot {
	sg2002_rtos_u32 sequence;
	sg2002_rtos_u16 length;
	sg2002_rtos_u8 payload[SG2002_RTOS_SHM_PAYLOAD_SIZE];
};

struct sg2002_rtos_shm_layout_line {
	sg2002_rtos_u32 magic;
	sg2002_rtos_u16 abi_version;
	sg2002_rtos_u16 control_size;
	sg2002_rtos_u32 region_size;
	sg2002_rtos_u16 slot_size;
	sg2002_rtos_u16 slot_count;
	sg2002_rtos_u32 request_offset;
	sg2002_rtos_u32 response_offset;
	sg2002_rtos_u32 generation;
	sg2002_rtos_u32 features;
	sg2002_rtos_u8 reserved[32];
};

struct sg2002_rtos_shm_peer_line {
	volatile sg2002_rtos_u32 state;
	volatile sg2002_rtos_u32 generation;
	sg2002_rtos_u8 reserved[56];
};

struct sg2002_rtos_shm_counter_line {
	volatile sg2002_rtos_u32 value;
	sg2002_rtos_u8 reserved[60];
};

struct sg2002_rtos_shm_control {
	struct sg2002_rtos_shm_layout_line layout;
	struct sg2002_rtos_shm_peer_line linux_peer;
	struct sg2002_rtos_shm_peer_line rtos_peer;
	struct sg2002_rtos_shm_counter_line request_producer;
	struct sg2002_rtos_shm_counter_line request_consumer;
	struct sg2002_rtos_shm_counter_line response_producer;
	struct sg2002_rtos_shm_counter_line response_consumer;
	sg2002_rtos_u8 reserved[SG2002_RTOS_SHM_CONTROL_SIZE -
				 SG2002_RTOS_SHM_CACHE_LINE_SIZE * 7U];
};

struct sg2002_rtos_shm_info {
	sg2002_rtos_u32 abi_version;
	sg2002_rtos_u32 region_size;
	sg2002_rtos_u32 control_size;
	sg2002_rtos_u32 slot_size;
	sg2002_rtos_u32 slot_count;
	sg2002_rtos_u32 payload_size;
	sg2002_rtos_u32 generation;
	sg2002_rtos_u32 features;
};

struct sg2002_rtos_shm_transfer {
	sg2002_rtos_u32 timeout_ms;
	struct sg2002_rtos_shm_slot message;
};

/* Fixed-width descriptor; slots_ptr addresses slot[count] in user memory. */
struct sg2002_rtos_shm_batch {
	sg2002_rtos_aligned_u64 slots_ptr;
	sg2002_rtos_u32 timeout_ms;
	sg2002_rtos_u32 generation;
	sg2002_rtos_u16 count;
	sg2002_rtos_u16 completed;
	sg2002_rtos_u32 flags;
	sg2002_rtos_u32 reserved[2];
};

#ifdef __cplusplus
#define SG2002_RTOS_SHM_STATIC_ASSERT static_assert
#else
#define SG2002_RTOS_SHM_STATIC_ASSERT _Static_assert
#endif

SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_slot) ==
	       SG2002_RTOS_SHM_SLOT_SIZE,
	       "SG2002 shared-memory slot must be exactly 1024 bytes");
SG2002_RTOS_SHM_STATIC_ASSERT(
	offsetof(struct sg2002_rtos_shm_slot, payload) ==
		SG2002_RTOS_SHM_SLOT_HEADER_SIZE,
	"SG2002 shared-memory slot header must be exactly 6 bytes");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_transfer) == 1028U,
	"SG2002 shared-memory transfer must be exactly 1028 bytes");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_batch) == 32U,
	"SG2002 shared-memory batch descriptor must be exactly 32 bytes");
SG2002_RTOS_SHM_STATIC_ASSERT(
	offsetof(struct sg2002_rtos_shm_batch, slots_ptr) == 0U &&
	offsetof(struct sg2002_rtos_shm_batch, timeout_ms) == 8U &&
	offsetof(struct sg2002_rtos_shm_batch, generation) == 12U &&
	offsetof(struct sg2002_rtos_shm_batch, count) == 16U &&
	offsetof(struct sg2002_rtos_shm_batch, completed) == 18U &&
	offsetof(struct sg2002_rtos_shm_batch, flags) == 20U &&
	offsetof(struct sg2002_rtos_shm_batch, reserved) == 24U,
	"SG2002 shared-memory batch descriptor layout changed");
SG2002_RTOS_SHM_STATIC_ASSERT(
	(SG2002_RTOS_SHM_SLOT_COUNT & (SG2002_RTOS_SHM_SLOT_COUNT - 1U)) == 0U,
	"SG2002 shared-memory slot count must be a power of two");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_layout_line) ==
	       SG2002_RTOS_SHM_CACHE_LINE_SIZE,
	       "SG2002 layout must occupy one cache line");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_peer_line) ==
	       SG2002_RTOS_SHM_CACHE_LINE_SIZE,
	       "SG2002 peer state must occupy one cache line");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_counter_line) ==
	       SG2002_RTOS_SHM_CACHE_LINE_SIZE,
	       "SG2002 counter must occupy one cache line");
SG2002_RTOS_SHM_STATIC_ASSERT(sizeof(struct sg2002_rtos_shm_control) ==
	       SG2002_RTOS_SHM_CONTROL_SIZE,
	       "SG2002 control page must be exactly 4 KiB");

#undef SG2002_RTOS_SHM_STATIC_ASSERT

#ifdef __linux__
#define SG2002_RTOS_SHM_IOCTL_MAGIC 's'
#define SG2002_RTOS_SHM_ACQUIRE \
	_IOR(SG2002_RTOS_SHM_IOCTL_MAGIC, 0, struct sg2002_rtos_shm_info)
#define SG2002_RTOS_SHM_SEND \
	_IOWR(SG2002_RTOS_SHM_IOCTL_MAGIC, 1, struct sg2002_rtos_shm_transfer)
#define SG2002_RTOS_SHM_RECEIVE \
	_IOWR(SG2002_RTOS_SHM_IOCTL_MAGIC, 2, struct sg2002_rtos_shm_transfer)
#define SG2002_RTOS_SHM_SUBMIT_BATCH \
	_IOWR(SG2002_RTOS_SHM_IOCTL_MAGIC, 3, struct sg2002_rtos_shm_batch)
#define SG2002_RTOS_SHM_REAP_BATCH \
	_IOWR(SG2002_RTOS_SHM_IOCTL_MAGIC, 4, struct sg2002_rtos_shm_batch)
#endif
