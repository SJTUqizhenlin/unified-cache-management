#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ucm_spdk_ipc_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
connect_target(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -errno;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return -ENAMETOOLONG;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		int rc = -errno;
		close(fd);
		return rc;
	}
	return fd;
}

static int
send_with_fds(int fd, const void *payload, size_t payload_size,
	      const int *fds, size_t nfds, size_t expected_reply)
{
	struct msghdr msg = {0};
	struct iovec iov = {.iov_base = (void *)payload, .iov_len = payload_size};
	char cbuf[CMSG_SPACE(sizeof(int) * UCM_IPC_HELLO_FDS)] = {0};
	struct cmsghdr *cmsg;
	ssize_t n;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (nfds > 0) {
		msg.msg_control = cbuf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
	}
	do {
		n = sendmsg(fd, &msg, MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	if (n != (ssize_t)payload_size) {
		return n < 0 ? -errno : -EIO;
	}
	(void)expected_reply;
	return 0;
}

static int
recv_exact(int fd, void *buf, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t n = recv(fd, (uint8_t *)buf + done, size - done, 0);
		if (n < 0) {
			if (errno == EINTR) { continue; }
			return -errno;
		}
		if (n == 0) {
			return -ECONNRESET;
		}
		done += (size_t)n;
	}
	return 0;
}

int
ucm_ipc_client_init(struct ucm_ipc_client *c, const char *socket_path,
		    int metadata_fd, uint64_t metadata_size,
		    uint64_t metadata_file_offset, uint32_t max_outstanding,
		    int32_t numa_id, const char *worker_name,
		    const char *store_name)
{
	struct ucm_ipc_hello hello = {0};
	struct ucm_ipc_reply reply;
	int fds[UCM_IPC_HELLO_FDS];
	int sock = -1;
	int rc;

	memset(c, 0, sizeof(*c));
	c->control_fd = -1;
	rc = pthread_mutex_init(&c->cb_mu, NULL);
	if (rc != 0) {
		return -rc;
	}
	c->cb_slots = calloc(UCM_IPC_RING_SIZE, sizeof(*c->cb_slots));
	if (c->cb_slots == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	for (uint32_t i = 0; i < UCM_IPC_RING_SIZE; i++) {
		c->cb_slots[i].next_free = i + 1;
	}
	c->cb_free_count = UCM_IPC_RING_SIZE;
	c->next_req_id = 1;
	c->control_size = sizeof(struct ucm_ipc_control);
	c->control_fd = memfd_create("ucm_ipc_control", MFD_CLOEXEC);
	if (c->control_fd < 0) {
		rc = -errno;
		goto fail;
	}
	if (ftruncate(c->control_fd, (off_t)c->control_size) != 0) {
		rc = -errno;
		goto fail;
	}
	c->ctl = mmap(NULL, c->control_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      c->control_fd, 0);
	if (c->ctl == MAP_FAILED) {
		c->ctl = NULL;
		rc = -errno;
		goto fail;
	}
	ucm_ipc_control_init(c->ctl, max_outstanding, metadata_size);

	hello.magic = UCM_IPC_MAGIC;
	hello.version = UCM_IPC_VERSION;
	hello.kind = UCM_IPC_KIND_HELLO;
	hello.size = sizeof(hello);
	hello.worker_pid = (uint32_t)getpid();
	hello.worker_uid = (uint32_t)getuid();
	hello.numa_id = numa_id;
	hello.max_outstanding = max_outstanding;
	hello.control_size = c->control_size;
	hello.data_size = metadata_size;
	hello.data_file_offset = metadata_file_offset;
	hello.shard_size = 0;
	hello.sector_size = 0;
	if (worker_name != NULL) {
		snprintf(hello.worker_name, sizeof(hello.worker_name), "%s", worker_name);
	}
	if (store_name != NULL) {
		snprintf(hello.store_name, sizeof(hello.store_name), "%s", store_name);
	}

	sock = connect_target(socket_path);
	if (sock < 0) {
		rc = sock;
		goto fail;
	}
	fds[0] = c->control_fd;
	fds[1] = metadata_fd;
	rc = send_with_fds(sock, &hello, sizeof(hello), fds, UCM_IPC_HELLO_FDS,
			   sizeof(reply));
	if (rc != 0) {
		goto fail;
	}
	rc = recv_exact(sock, &reply, sizeof(reply));
	if (rc != 0) {
		goto fail;
	}
	close(sock);
	sock = -1;
	if (reply.magic != UCM_IPC_MAGIC || reply.status != 0) {
		rc = reply.status != 0 ? reply.status : -EINVAL;
		goto fail;
	}
	c->session_id = reply.session_id;
	c->qpair_depth = reply.qpair_depth;
	c->sector_size = reply.sector_size;
	c->max_xfer_size = reply.max_xfer_size;
	c->num_sectors = reply.num_sectors;
	return 0;

fail:
	if (sock >= 0) { close(sock); }
	if (c->ctl != NULL) {
		munmap(c->ctl, c->control_size);
		c->ctl = NULL;
	}
	if (c->control_fd >= 0) {
		close(c->control_fd);
		c->control_fd = -1;
	}
	pthread_mutex_destroy(&c->cb_mu);
	free(c->cb_slots);
	c->cb_slots = NULL;
	c->cb_free_count = 0;
	c->cb_free_head = UCM_IPC_RING_SIZE;
	return rc;
}

int
ucm_ipc_client_register_region(struct ucm_ipc_client *c, const char *socket_path,
			       uint32_t region_id, int data_fd, uint64_t data_size,
			       uint64_t data_file_offset)
{
	struct ucm_ipc_region_hello hello = {0};
	struct ucm_ipc_region_reply reply;
	int fds[1] = {data_fd};
	int sock;
	int rc;

	if (c == NULL || c->ctl == NULL) {
		return -EINVAL;
	}
	hello.magic = UCM_IPC_MAGIC;
	hello.version = UCM_IPC_VERSION;
	hello.kind = UCM_IPC_KIND_REGION;
	hello.size = sizeof(hello);
	hello.session_id = c->session_id;
	hello.region_id = region_id;
	hello.data_size = data_size;
	hello.data_file_offset = data_file_offset;

	sock = connect_target(socket_path);
	if (sock < 0) {
		return sock;
	}
	rc = send_with_fds(sock, &hello, sizeof(hello), fds, 1, sizeof(reply));
	if (rc != 0) {
		close(sock);
		return rc;
	}
	rc = recv_exact(sock, &reply, sizeof(reply));
	close(sock);
	if (rc != 0) {
		return rc;
	}
	if (reply.magic != UCM_IPC_MAGIC || reply.status != 0 ||
	    reply.session_id != c->session_id || reply.region_id != region_id) {
		return reply.status != 0 ? reply.status : -EINVAL;
	}
	return 0;
}

static int
client_submit_internal(struct ucm_ipc_client *c, uint32_t region_id,
		       uint16_t opcode, uint64_t offset_bytes,
		       uint64_t data_offset, uint32_t nbytes,
		       ucm_ipc_callback cb, void *arg, uint64_t *req_id)
{
	struct ucm_ipc_control *ctl = c->ctl;
	struct ucm_ipc_cb_entry *entry;
	uint32_t slot = 0;
	uint32_t head;
	uint32_t tail;
	uint64_t id;

	if (ctl == NULL) {
		return -EINVAL;
	}
	/* Never wrap the generation and let an old completion match a new I/O. */
	if (c->next_req_id > UINT64_MAX / UCM_IPC_RING_SIZE) {
		return -EOVERFLOW;
	}
	if (cb != NULL) {
		pthread_mutex_lock(&c->cb_mu);
		if (c->cb_free_count == 0) {
			pthread_mutex_unlock(&c->cb_mu);
			return -EBUSY;
		}
		/* The serialized SPSC owner never polls during the ring wait below.
		 * Keep one lock through slot initialization; the target needs no cb_mu.
		 */
	}
	head = atomic_load_explicit(&ctl->submit_head, memory_order_relaxed);
	tail = atomic_load_explicit(&ctl->submit_tail, memory_order_acquire);
	if (ucm_ipc_ring_used(head, tail) >= UCM_IPC_RING_SIZE - 1u) {
		uint64_t deadline = now_ms() + 5000u;

		do {
			if (now_ms() >= deadline) {
				if (cb != NULL) {
					pthread_mutex_unlock(&c->cb_mu);
				}
				return -EBUSY;
			}
			usleep(100);
			tail = atomic_load_explicit(&ctl->submit_tail, memory_order_acquire);
		} while (ucm_ipc_ring_used(head, tail) >= UCM_IPC_RING_SIZE - 1u);
	}
	if (cb != NULL) {
		slot = c->cb_free_head;
		entry = &c->cb_slots[slot];
		c->cb_free_head = entry->next_free;
		c->cb_free_count--;
	}
	/* Low bits directly address the slot; upper bits prevent stale-ID reuse. */
	id = c->next_req_id++ * UCM_IPC_RING_SIZE + slot;
	if (cb != NULL) {
		entry->req_id = id;
		entry->cb = cb;
		entry->arg = arg;
		pthread_mutex_unlock(&c->cb_mu);
	}
	ctl->submit[head & UCM_IPC_RING_MASK].req_id = id;
	ctl->submit[head & UCM_IPC_RING_MASK].offset_bytes = offset_bytes;
	ctl->submit[head & UCM_IPC_RING_MASK].data_offset = data_offset;
	ctl->submit[head & UCM_IPC_RING_MASK].nbytes = nbytes;
	ctl->submit[head & UCM_IPC_RING_MASK].opcode = opcode;
	ctl->submit[head & UCM_IPC_RING_MASK].flags = 0;
	ctl->submit[head & UCM_IPC_RING_MASK].region_id = region_id;
	atomic_store_explicit(&ctl->submit_head, head + 1, memory_order_release);
	if (req_id != NULL) {
		*req_id = id;
	}
	return 0;
}

int
ucm_ipc_client_submit(struct ucm_ipc_client *c, uint32_t region_id,
		      uint16_t opcode, uint64_t offset_bytes,
		      uint64_t data_offset, uint32_t nbytes, uint64_t *req_id)
{
	return client_submit_internal(c, region_id, opcode, offset_bytes,
				      data_offset, nbytes, NULL, NULL, req_id);
}

int
ucm_ipc_client_submit_cb(struct ucm_ipc_client *c, uint32_t region_id,
			 uint16_t opcode, uint64_t offset_bytes,
			 uint64_t data_offset, uint32_t nbytes,
			 ucm_ipc_callback cb, void *arg, uint64_t *req_id)
{
	return client_submit_internal(c, region_id, opcode, offset_bytes,
				      data_offset, nbytes, cb, arg, req_id);
}

static void
complete_callback(struct ucm_ipc_client *c,
		  const struct ucm_ipc_completion_entry *completion)
{
	uint32_t slot = completion->req_id & UCM_IPC_RING_MASK;
	struct ucm_ipc_cb_entry *entry;
	ucm_ipc_callback cb = NULL;
	void *arg = NULL;

	pthread_mutex_lock(&c->cb_mu);
	entry = &c->cb_slots[slot];
	if (entry->cb != NULL && entry->req_id == completion->req_id) {
		cb = entry->cb;
		arg = entry->arg;
		entry->cb = NULL;
		entry->arg = NULL;
		entry->next_free = c->cb_free_head;
		c->cb_free_head = slot;
		c->cb_free_count++;
	}
	pthread_mutex_unlock(&c->cb_mu);
	if (cb != NULL) {
		cb(arg, completion->status);
	}
}

int
ucm_ipc_client_poll(struct ucm_ipc_client *c, uint32_t budget)
{
	struct ucm_ipc_control *ctl = c->ctl;
	uint32_t done = 0;

	if (ctl == NULL) {
		return -EINVAL;
	}
	while (budget == 0 || done < budget) {
		uint32_t head = atomic_load_explicit(&ctl->completion_head,
						     memory_order_acquire);
		uint32_t tail = atomic_load_explicit(&ctl->completion_tail,
						     memory_order_relaxed);

		if (tail == head) {
			break;
		}
		struct ucm_ipc_completion_entry e =
			ctl->completion[tail & UCM_IPC_RING_MASK];
		atomic_store_explicit(&ctl->completion_tail, tail + 1,
				      memory_order_release);
		complete_callback(c, &e);
		/* Reload the ring tail next time: a callback may itself poll. */
		done++;
	}
	return (int)done;
}

int
ucm_ipc_client_wait(struct ucm_ipc_client *c, uint64_t req_id,
		    uint64_t timeout_ms, int *status)
{
	struct ucm_ipc_control *ctl = c->ctl;
	uint64_t deadline = now_ms() + timeout_ms;

	if (ctl == NULL) {
		return -EINVAL;
	}
	while (now_ms() < deadline) {
		uint32_t head = atomic_load_explicit(&ctl->completion_head,
						     memory_order_acquire);
		uint32_t tail = atomic_load_explicit(&ctl->completion_tail,
						     memory_order_relaxed);
		if (tail != head) {
			struct ucm_ipc_completion_entry e =
				ctl->completion[tail & UCM_IPC_RING_MASK];
			tail++;
			atomic_store_explicit(&ctl->completion_tail, tail,
					      memory_order_release);
			complete_callback(c, &e);
			if (e.req_id == req_id) {
				if (status != NULL) { *status = e.status; }
				return 0;
			}
			continue;
		}
		usleep(100);
	}
	return -ETIMEDOUT;
}

void
ucm_ipc_client_close(struct ucm_ipc_client *c)
{
	uint64_t deadline;

	if (c == NULL || c->ctl == NULL) {
		return;
	}
	atomic_store_explicit(&c->ctl->closing, 1, memory_order_release);
	deadline = now_ms() + 5000u;
	while (!atomic_load_explicit(&c->ctl->closed, memory_order_acquire)) {
		if (now_ms() >= deadline) {
			break;
		}
		usleep(1000);
	}
	atomic_store_explicit(&c->ctl->close_ack, 1, memory_order_release);
	usleep(20000);
	free(c->cb_slots);
	c->cb_slots = NULL;
	c->cb_free_head = UCM_IPC_RING_SIZE;
	c->cb_free_count = 0;
	pthread_mutex_destroy(&c->cb_mu);
	munmap(c->ctl, c->control_size);
	c->ctl = NULL;
	if (c->control_fd >= 0) {
		close(c->control_fd);
		c->control_fd = -1;
	}
}
