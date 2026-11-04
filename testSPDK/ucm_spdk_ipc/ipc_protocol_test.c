/* Hardware-free checks for the shared-memory wire protocol. */
#include "ipc_protocol.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	struct ucm_ipc_control ctl;

	memset(&ctl, 0, sizeof(ctl));
	ucm_ipc_control_init(&ctl, 8, 2u << 20);
	assert(ctl.magic == UCM_IPC_MAGIC);
	assert(ctl.version == UCM_IPC_VERSION);
	assert(ctl.header_size == sizeof(ctl));
	assert(ctl.submit_size == sizeof(ctl.submit[0]));
	assert(ctl.completion_size == sizeof(ctl.completion[0]));
	assert(ctl.ring_size == UCM_IPC_RING_SIZE);
	assert(ctl.max_outstanding == 8);
	assert(ctl.data_size == (2u << 20));
	assert(atomic_load_explicit(&ctl.submit_head, memory_order_relaxed) == 0);
	assert(atomic_load_explicit(&ctl.submit_tail, memory_order_relaxed) == 0);
	assert(atomic_load_explicit(&ctl.completion_head, memory_order_relaxed) == 0);
	assert(atomic_load_explicit(&ctl.completion_tail, memory_order_relaxed) == 0);

	assert(ucm_ipc_ring_used(0, 0) == 0);
	assert(ucm_ipc_ring_used(3, 1) == 2);
	assert(ucm_ipc_ring_used(1, 3) == (uint32_t)-2);

	assert(sizeof(struct ucm_ipc_submit_entry) == 36);
	assert(sizeof(struct ucm_ipc_completion_entry) == 16);
	assert(sizeof(struct ucm_ipc_hello) == 148);
	assert(sizeof(struct ucm_ipc_reply) == 48);
	assert(sizeof(struct ucm_ipc_region_hello) == 40);
	assert(sizeof(struct ucm_ipc_region_reply) == 24);
	assert(UCM_IPC_MAX_REGIONS == 4);
	assert(UCM_IPC_RING_SIZE == (UCM_IPC_RING_MASK + 1));

	/* Region 1 is the UCM Cache SHM; region 0 is metadata. */
	assert(UCM_IPC_KIND_HELLO == 1);
	assert(UCM_IPC_KIND_REGION == 2);
	assert(UCM_IPC_OP_READ == 1);
	assert(UCM_IPC_OP_WRITE == 2);
	assert(UCM_IPC_OP_FLUSH == 3);

	printf("PASS: IPC protocol layout, control init, ring arithmetic\n");
	return 0;
}
