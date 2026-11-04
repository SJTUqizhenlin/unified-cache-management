/*
 * Local shared-memory protocol between UCM workers and the IPC target.
 *
 * A session has one control region (rings) and up to four registered data
 * regions.  Typically region 0 is a small metadata hugetlbfs arena and region
 * 1 is the UCM Cache SHM.  Payload bytes never pass through the control ring.
 */
#ifndef UCM_SPDK_IPC_PROTOCOL_H
#define UCM_SPDK_IPC_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>

#define UCM_IPC_MAGIC 0x55434d49u /* "UCMI" */
#define UCM_IPC_VERSION 1u
#define UCM_IPC_RING_SIZE 1024u
#define UCM_IPC_RING_MASK (UCM_IPC_RING_SIZE - 1u)
#define UCM_IPC_MAX_NAME 64
#define UCM_IPC_MAX_STORE 16
#define UCM_IPC_MAX_REGIONS 4
#define UCM_IPC_HELLO_FDS 2   /* control + metadata */
#define UCM_IPC_REGION_FDS 1  /* one data region */

enum ucm_ipc_kind {
	UCM_IPC_KIND_HELLO = 1,
	UCM_IPC_KIND_REGION = 2,
};

enum ucm_ipc_opcode {
	UCM_IPC_OP_READ = 1,
	UCM_IPC_OP_WRITE = 2,
	UCM_IPC_OP_FLUSH = 3,
};

struct ucm_ipc_submit_entry {
	uint64_t req_id;
	uint64_t offset_bytes; /* byte offset in the NVMe namespace */
	uint64_t data_offset;  /* byte offset in the selected data region */
	uint32_t nbytes;
	uint16_t opcode;
	uint16_t flags;
	uint32_t region_id;
} __attribute__((packed));

struct ucm_ipc_completion_entry {
	uint64_t req_id;
	int32_t status;
	uint32_t reserved;
} __attribute__((packed));

struct ucm_ipc_control {
	uint32_t magic;
	uint16_t version;
	uint16_t header_size;
	uint32_t submit_size;
	uint32_t completion_size;
	uint32_t ring_size;
	uint32_t max_outstanding;
	uint64_t data_size;
	uint64_t sector_size;

	_Atomic uint32_t submit_head;
	_Atomic uint32_t submit_tail;
	_Atomic uint32_t completion_head;
	_Atomic uint32_t completion_tail;
	_Atomic uint32_t closing;
	_Atomic uint32_t closed;
	_Atomic uint32_t close_ack;

	uint32_t reserved[7];

	struct ucm_ipc_submit_entry submit[UCM_IPC_RING_SIZE];
	struct ucm_ipc_completion_entry completion[UCM_IPC_RING_SIZE];
};

/* Initial session handshake.  fds[0] = control memfd, fds[1] = metadata fd. */
struct ucm_ipc_hello {
	uint32_t magic;
	uint16_t version;
	uint16_t kind; /* UCM_IPC_KIND_HELLO */
	uint32_t size;
	uint32_t worker_pid;
	uint32_t worker_uid;
	int32_t numa_id;
	uint32_t max_outstanding;
	uint64_t control_size;
	uint64_t data_size;        /* metadata region size */
	uint64_t data_file_offset; /* metadata region offset in fd */
	uint64_t shard_size;
	uint64_t sector_size;
	char worker_name[UCM_IPC_MAX_NAME];
	char store_name[UCM_IPC_MAX_STORE];
} __attribute__((packed));

struct ucm_ipc_reply {
	uint32_t magic;
	int32_t status;
	uint32_t session_id;
	uint32_t qpair_depth;
	uint64_t sector_size;
	uint64_t max_xfer_size;
	uint64_t data_size;
	uint64_t num_sectors;
} __attribute__((packed));

/* Later data-region registration on a separate Unix socket connection. */
struct ucm_ipc_region_hello {
	uint32_t magic;
	uint16_t version;
	uint16_t kind; /* UCM_IPC_KIND_REGION */
	uint32_t size;
	uint32_t session_id;
	uint32_t region_id;
	uint32_t reserved;
	uint64_t data_size;
	uint64_t data_file_offset;
} __attribute__((packed));

struct ucm_ipc_region_reply {
	uint32_t magic;
	int32_t status;
	uint32_t session_id;
	uint32_t region_id;
	uint64_t data_size;
} __attribute__((packed));

_Static_assert(sizeof(struct ucm_ipc_submit_entry) == 36,
	       "submit entry wire layout changed");
_Static_assert(sizeof(struct ucm_ipc_completion_entry) == 16,
	       "completion entry wire layout changed");
_Static_assert(sizeof(struct ucm_ipc_hello) == 148,
	       "hello wire layout changed");
_Static_assert(sizeof(struct ucm_ipc_reply) == 48,
	       "reply wire layout changed");
_Static_assert(sizeof(struct ucm_ipc_region_hello) == 40,
	       "region hello wire layout changed");
_Static_assert(sizeof(struct ucm_ipc_region_reply) == 24,
	       "region reply wire layout changed");

static inline void ucm_ipc_control_init(struct ucm_ipc_control *ctl,
					uint32_t max_outstanding,
					uint64_t data_size)
{
	ctl->magic = UCM_IPC_MAGIC;
	ctl->version = UCM_IPC_VERSION;
	ctl->header_size = (uint16_t)sizeof(*ctl);
	ctl->submit_size = (uint32_t)sizeof(ctl->submit[0]);
	ctl->completion_size = (uint32_t)sizeof(ctl->completion[0]);
	ctl->ring_size = UCM_IPC_RING_SIZE;
	ctl->max_outstanding = max_outstanding;
	ctl->data_size = data_size;
	ctl->sector_size = 0;
	atomic_store_explicit(&ctl->submit_head, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->submit_tail, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->completion_head, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->completion_tail, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->closing, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->closed, 0, memory_order_relaxed);
	atomic_store_explicit(&ctl->close_ack, 0, memory_order_relaxed);
}

static inline uint32_t ucm_ipc_ring_used(uint32_t head, uint32_t tail)
{
	return head - tail;
}

#endif /* UCM_SPDK_IPC_PROTOCOL_H */
