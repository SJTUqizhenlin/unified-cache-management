#ifndef UCM_SPDK_IPC_CLIENT_H
#define UCM_SPDK_IPC_CLIENT_H

#include <pthread.h>
#include <stddef.h>

#include "ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ucm_ipc_callback)(void *arg, int status);

struct ucm_ipc_cb_entry {
	uint64_t req_id;
	ucm_ipc_callback cb;
	void *arg;
	uint32_t next_free;
};

struct ucm_ipc_client {
	int control_fd;
	struct ucm_ipc_control *ctl;
	size_t control_size;
	uint32_t session_id;
	uint32_t qpair_depth;
	uint64_t sector_size;
	uint64_t max_xfer_size;
	uint64_t num_sectors;
	uint64_t next_req_id; /* Next generation; wire ID = generation * ring size + slot. */
	pthread_mutex_t cb_mu;
	struct ucm_ipc_cb_entry *cb_slots;
	uint32_t cb_free_head; /* UCM_IPC_RING_SIZE marks the end of the freelist. */
	uint32_t cb_free_count;
};

/* Rings are SPSC: externally serialize submit/poll/wait on one engine owner.
 * cb_mu protects bookkeeping only, not concurrent ring producers/consumers.
 * Callbacks may reenter submit/poll; their slot is released before invocation.
 * Request IDs are opaque, generation-tagged values, not contiguous counters.
 */

/* Connect, send the initial hello, and register the metadata region (region 0). */
int ucm_ipc_client_init(struct ucm_ipc_client *c, const char *socket_path,
			int metadata_fd, uint64_t metadata_size,
			uint64_t metadata_file_offset, uint32_t max_outstanding,
			int32_t numa_id, const char *worker_name,
			const char *store_name);

/* Register one additional data region. */
int ucm_ipc_client_register_region(struct ucm_ipc_client *c,
				   const char *socket_path, uint32_t region_id,
				   int data_fd, uint64_t data_size,
				   uint64_t data_file_offset);

int ucm_ipc_client_submit(struct ucm_ipc_client *c, uint32_t region_id,
			  uint16_t opcode, uint64_t offset_bytes,
			  uint64_t data_offset, uint32_t nbytes,
			  uint64_t *req_id);

/* A full callback table returns -EBUSY immediately without publishing an I/O. */
int ucm_ipc_client_submit_cb(struct ucm_ipc_client *c, uint32_t region_id,
			     uint16_t opcode, uint64_t offset_bytes,
			     uint64_t data_offset, uint32_t nbytes,
			     ucm_ipc_callback cb, void *arg, uint64_t *req_id);

/* Invoke callbacks for all currently available completions. */
int ucm_ipc_client_poll(struct ucm_ipc_client *c, uint32_t budget);

/* Dispatch callbacks while waiting for the specified completion. */
int ucm_ipc_client_wait(struct ucm_ipc_client *c, uint64_t req_id,
			uint64_t timeout_ms, int *status);

/* Best-effort graceful close; data fds remain owned by the caller. */
void ucm_ipc_client_close(struct ucm_ipc_client *c);

#ifdef __cplusplus
}
#endif

#endif
