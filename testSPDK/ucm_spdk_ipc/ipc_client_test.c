/* Hardware-free client/protocol test using a fake in-process target. */
#include "ucm_spdk_ipc_client.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BATCH_REQUESTS 6u
#define BACKING_SIZE (256u << 10)
#define MAX_PAYLOAD 8192u

struct fake_target {
	const char *path;
	int listen_fd;
	struct ucm_ipc_control *ctl;
	atomic_int stop;
	atomic_int manual_io; /* 1 requests a pause, 2 acknowledges ring ownership. */
	pthread_t accept_tid;
	pthread_t io_tid;
	int io_started;
	uint32_t session_id;
	uint32_t last_region;
	unsigned submissions;
	uint8_t *regions[2];
	size_t region_sizes[2];
	atomic_int region_ready[2];
	uint8_t *storage;
};

static int
send_all(int fd, const void *buf, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t n = send(fd, (const uint8_t *)buf + done, size - done, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) { continue; }
			return -errno;
		}
		if (n == 0) { return -EIO; }
		done += (size_t)n;
	}
	return 0;
}

static int
recv_fds(int fd, void *buf, size_t cap, int fds[2], int *nfds)
{
	struct msghdr msg = {0};
	struct iovec iov = {.iov_base = buf, .iov_len = cap};
	char cbuf[CMSG_SPACE(sizeof(int) * 2)] = {0};
	struct cmsghdr *cmsg;
	ssize_t n;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	fds[0] = fds[1] = -1;
	*nfds = 0;
	do {
		n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC | MSG_WAITALL);
	} while (n < 0 && errno == EINTR);
	if (n <= 0) { return n < 0 ? -errno : -ECONNRESET; }
	assert(!(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)));
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			continue;
		}
		int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		int *p = (int *)CMSG_DATA(cmsg);
		assert(count <= 2 - *nfds);
		for (int i = 0; i < count; i++) {
			fds[(*nfds)++] = p[i];
		}
	}
	return (int)n;
}

static void *
fake_io(void *arg)
{
	struct fake_target *t = arg;

	while (!t->stop) {
		struct ucm_ipc_control *ctl = t->ctl;
		uint32_t head, tail;
		struct ucm_ipc_submit_entry entries[BATCH_REQUESTS];

		if (atomic_load_explicit(&t->manual_io, memory_order_acquire)) {
			atomic_store_explicit(&t->manual_io, 2, memory_order_release);
		}
		head = atomic_load_explicit(&ctl->submit_head, memory_order_acquire);
		tail = atomic_load_explicit(&ctl->submit_tail, memory_order_relaxed);
		if (!atomic_load_explicit(&t->manual_io, memory_order_acquire) &&
		    ucm_ipc_ring_used(head, tail) >= BATCH_REQUESTS) {
			for (unsigned i = 0; i < BATCH_REQUESTS; i++) {
				struct ucm_ipc_submit_entry e =
					ctl->submit[(tail + i) & UCM_IPC_RING_MASK];
				assert(e.region_id < 2);
				assert(atomic_load_explicit(&t->region_ready[e.region_id],
							    memory_order_acquire));
				assert(e.nbytes > 0 && e.nbytes <= MAX_PAYLOAD);
				assert(e.offset_bytes <= BACKING_SIZE - e.nbytes);
				assert(e.data_offset <= t->region_sizes[e.region_id] - e.nbytes);
				assert(e.offset_bytes % 512 == 0 && e.nbytes % 512 == 0);
				uint8_t *data = t->regions[e.region_id] + e.data_offset;
				if (e.opcode == UCM_IPC_OP_WRITE) {
					memcpy(t->storage + e.offset_bytes, data, e.nbytes);
				} else {
					assert(e.opcode == UCM_IPC_OP_READ);
					memcpy(data, t->storage + e.offset_bytes, e.nbytes);
				}
				entries[i] = e;
				t->submissions++;
			}
			atomic_store_explicit(&ctl->submit_tail, tail + BATCH_REQUESTS,
					      memory_order_release);
			uint32_t ch = atomic_load_explicit(&ctl->completion_head,
							   memory_order_relaxed);
			uint32_t ct = atomic_load_explicit(&ctl->completion_tail,
							   memory_order_acquire);
			assert(ucm_ipc_ring_used(ch, ct) + BATCH_REQUESTS < UCM_IPC_RING_SIZE);
			/* Execute in submission order, but deliberately complete in reverse. */
			for (unsigned i = 0; i < BATCH_REQUESTS; i++) {
				struct ucm_ipc_completion_entry *c =
					&ctl->completion[(ch + i) & UCM_IPC_RING_MASK];
				c->req_id = entries[BATCH_REQUESTS - 1 - i].req_id;
				c->status = 0;
				c->reserved = 0;
			}
			atomic_store_explicit(&ctl->completion_head, ch + BATCH_REQUESTS,
					      memory_order_release);
		}
		if (atomic_load_explicit(&ctl->closing, memory_order_acquire)) {
			atomic_store_explicit(&ctl->closed, 1, memory_order_release);
			while (!atomic_load_explicit(&ctl->close_ack, memory_order_acquire) &&
			       !t->stop) {
				usleep(1000);
			}
			break;
		}
		usleep(100);
	}
	return NULL;
}

static void *
fake_accept(void *arg)
{
	struct fake_target *t = arg;

	while (!t->stop) {
		struct pollfd pfd = {.fd = t->listen_fd, .events = POLLIN};
		if (poll(&pfd, 1, 100) <= 0) { continue; }
		int fd = accept(t->listen_fd, NULL, NULL);
		if (fd < 0) { continue; }
		union {
			struct ucm_ipc_hello hello;
			struct ucm_ipc_region_hello region;
		} msg;
		int fds[2], nfds = 0;
		/* Read the common header first so stream fragmentation is harmless. */
		int n = recv_fds(fd, &msg, 12, fds, &nfds);
		assert(n == 12);
		uint16_t kind;
		memcpy(&kind, (uint8_t *)&msg + 6, sizeof(kind));
		assert(kind == UCM_IPC_KIND_HELLO || kind == UCM_IPC_KIND_REGION);
		size_t size = kind == UCM_IPC_KIND_HELLO ? sizeof(msg.hello) : sizeof(msg.region);
		while ((size_t)n < size) {
			ssize_t got = recv(fd, (uint8_t *)&msg + n, size - (size_t)n, 0);
			if (got < 0 && errno == EINTR) { continue; }
			assert(got > 0);
			n += (int)got;
		}
		assert(msg.hello.magic == UCM_IPC_MAGIC);
		assert(msg.hello.version == UCM_IPC_VERSION);
		assert(msg.hello.size == size);
		if (kind == UCM_IPC_KIND_HELLO) {
			struct ucm_ipc_hello *hello = &msg.hello;
			assert(nfds == UCM_IPC_HELLO_FDS && t->ctl == NULL);
			assert(hello->control_size == sizeof(struct ucm_ipc_control));
			struct ucm_ipc_reply reply = {
				.magic = UCM_IPC_MAGIC,
				.status = 0,
				.session_id = ++t->session_id,
				.qpair_depth = 64,
				.sector_size = 512,
				.max_xfer_size = 131072,
				.data_size = hello->data_size,
				.num_sectors = BACKING_SIZE / 512,
			};
			t->ctl = mmap(NULL, (size_t)hello->control_size,
				      PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
			assert(t->ctl != MAP_FAILED);
			t->region_sizes[0] = (size_t)hello->data_size;
			t->regions[0] = mmap(NULL, t->region_sizes[0], PROT_READ | PROT_WRITE,
					     MAP_SHARED, fds[1], (off_t)hello->data_file_offset);
			assert(t->regions[0] != MAP_FAILED);
			atomic_store_explicit(&t->region_ready[0], 1, memory_order_release);
			assert(send_all(fd, &reply, sizeof(reply)) == 0);
			assert(pthread_create(&t->io_tid, NULL, fake_io, t) == 0);
			t->io_started = 1;
		} else if (kind == UCM_IPC_KIND_REGION) {
			struct ucm_ipc_region_hello *rh = &msg.region;
			assert(nfds == UCM_IPC_REGION_FDS);
			assert(rh->session_id == t->session_id && rh->region_id == 1);
			assert(t->regions[1] == NULL);
			struct ucm_ipc_region_reply reply = {
				.magic = UCM_IPC_MAGIC,
				.status = 0,
				.session_id = rh->session_id,
				.region_id = rh->region_id,
				.data_size = rh->data_size,
			};
			t->last_region = rh->region_id;
			t->region_sizes[1] = (size_t)rh->data_size;
			t->regions[1] = mmap(NULL, t->region_sizes[1], PROT_READ | PROT_WRITE,
					     MAP_SHARED, fds[0], (off_t)rh->data_file_offset);
			assert(t->regions[1] != MAP_FAILED);
			atomic_store_explicit(&t->region_ready[1], 1, memory_order_release);
			assert(send_all(fd, &reply, sizeof(reply)) == 0);
		}
		for (int i = 0; i < nfds; i++) { close(fds[i]); }
		close(fd);
	}
	return NULL;
}

static int
start_fake(struct fake_target *t, const char *path)
{
	struct sockaddr_un addr;
	int fd;
	int rc;

	memset(t, 0, sizeof(*t));
	atomic_init(&t->stop, 0);
	atomic_init(&t->manual_io, 0);
	for (unsigned i = 0; i < 2; i++) { atomic_init(&t->region_ready[i], 0); }
	t->path = path;
	t->listen_fd = -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) { return -errno; }
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	assert(strlen(path) < sizeof(addr.sun_path));
	memcpy(addr.sun_path, path, strlen(path) + 1);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 8) != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	t->listen_fd = fd;
	t->storage = calloc(1, BACKING_SIZE);
	assert(t->storage != NULL);
	assert(pthread_create(&t->accept_tid, NULL, fake_accept, t) == 0);
	return 0;
}

static void
stop_fake(struct fake_target *t)
{
	t->stop = 1;
	pthread_join(t->accept_tid, NULL);
	if (t->io_started) { pthread_join(t->io_tid, NULL); }
	if (t->listen_fd >= 0) { close(t->listen_fd); t->listen_fd = -1; }
	if (t->ctl != NULL) {
		munmap(t->ctl, sizeof(struct ucm_ipc_control));
		t->ctl = NULL;
	}
	for (unsigned i = 0; i < 2; i++) {
		if (t->regions[i] != NULL) {
			assert(munmap(t->regions[i], t->region_sizes[i]) == 0);
			t->regions[i] = NULL;
		}
	}
	free(t->storage);
	t->storage = NULL;
	unlink(t->path);
}

struct callback_check {
	unsigned index;
	unsigned calls;
	unsigned *completed;
	const uint8_t *actual;
	const uint8_t *expected;
	size_t nbytes;
	uint8_t guard;
};

static void
check_callback(void *arg, int status)
{
	struct callback_check *check = arg;

	assert(status == 0);
	assert(check->calls++ == 0);
	assert(check->index == BATCH_REQUESTS - 1 - *check->completed);
	assert(memcmp(check->actual, check->expected, check->nbytes) == 0);
	assert(check->actual[-1] == check->guard);
	assert(check->actual[check->nbytes] == check->guard);
	(*check->completed)++;
}

static void
complete_batch(struct ucm_ipc_client *client, unsigned *completed)
{
	struct timespec deadline, now;
	uint32_t tail = atomic_load_explicit(&client->ctl->completion_tail,
					     memory_order_relaxed);

	assert(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
	deadline.tv_sec += 5;
	while (ucm_ipc_ring_used(atomic_load_explicit(&client->ctl->completion_head,
						   memory_order_acquire), tail) < BATCH_REQUESTS) {
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		assert(now.tv_sec < deadline.tv_sec ||
		       (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
		usleep(100);
	}
	assert(*completed == 0);
	assert(ucm_ipc_client_poll(client, 2) == 2);
	assert(*completed == 2);
	assert(ucm_ipc_client_poll(client, 0) == BATCH_REQUESTS - 2);
	assert(*completed == BATCH_REQUESTS);
	assert(ucm_ipc_client_poll(client, 0) == 0);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE);
}

struct slot_check {
	unsigned calls;
	int status;
	struct ucm_ipc_client *reenter;
	struct slot_check *child;
	uint64_t child_id;
	int poll_nested;
};

static void
slot_callback(void *arg, int status)
{
	struct slot_check *check = arg;

	assert(check->calls++ == 0);
	assert(status == check->status);
	if (check->child != NULL) {
		/* The table was full: only releasing this callback's slot permits submit. */
		assert(check->reenter->cb_free_count == 1);
		assert(ucm_ipc_client_submit_cb(check->reenter, 0, UCM_IPC_OP_READ,
			0, 0, 512, slot_callback, check->child, &check->child_id) == 0);
	}
	if (check->poll_nested) {
		assert(ucm_ipc_client_poll(check->reenter, 1) == 1);
	}
}

static void
push_completion(struct ucm_ipc_client *client, uint64_t id, int status)
{
	struct ucm_ipc_control *ctl = client->ctl;
	uint32_t head = atomic_load_explicit(&ctl->completion_head, memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&ctl->completion_tail, memory_order_acquire);

	assert(ucm_ipc_ring_used(head, tail) < UCM_IPC_RING_SIZE - 1);
	ctl->completion[head & UCM_IPC_RING_MASK] =
		(struct ucm_ipc_completion_entry){.req_id = id, .status = status};
	atomic_store_explicit(&ctl->completion_head, head + 1, memory_order_release);
}

static void
consume_submissions(struct ucm_ipc_client *client)
{
	uint32_t head = atomic_load_explicit(&client->ctl->submit_head, memory_order_acquire);
	atomic_store_explicit(&client->ctl->submit_tail, head, memory_order_release);
}

static void
test_callback_slots(struct ucm_ipc_client *client, struct fake_target *fake)
{
	struct slot_check checks[UCM_IPC_RING_SIZE + 1] = {0};
	uint64_t ids[UCM_IPC_RING_SIZE];
	struct timespec start, end;
	uint64_t rejected_id = UINT64_MAX;
	uint64_t next_id;
	uint32_t head;
	int status = 1;

	atomic_store_explicit(&fake->manual_io, 1, memory_order_release);
	assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (atomic_load_explicit(&fake->manual_io, memory_order_acquire) != 2) {
		assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
		assert(end.tv_sec - start.tv_sec < 5);
		usleep(100);
	}
	for (unsigned i = 0; i < UCM_IPC_RING_SIZE; i++) {
		assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
			slot_callback, &checks[i], &ids[i]) == 0);
		consume_submissions(client);
	}
	assert(client->cb_free_count == 0);
	assert(client->cb_free_head == UCM_IPC_RING_SIZE);
	head = atomic_load_explicit(&client->ctl->submit_head, memory_order_relaxed);
	next_id = client->next_req_id;
	assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
		slot_callback, &checks[UCM_IPC_RING_SIZE], &rejected_id) == -EBUSY);
	assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	assert((end.tv_sec - start.tv_sec) * 1000000000LL +
	       end.tv_nsec - start.tv_nsec < 1000000000LL);
	assert(rejected_id == UINT64_MAX && client->next_req_id == next_id);
	assert(atomic_load_explicit(&client->ctl->submit_head, memory_order_relaxed) == head);

	/* Callback-free requests do not need a slot, even with a full table. */
	assert(ucm_ipc_client_submit(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
		&rejected_id) == 0);
	consume_submissions(client);
	push_completion(client, rejected_id, -EIO);
	assert(ucm_ipc_client_poll(client, 0) == 1);
	assert(client->cb_free_count == 0);

	checks[37].status = -EIO;
	checks[37].reenter = client;
	checks[37].child = &checks[UCM_IPC_RING_SIZE];
	push_completion(client, ids[37], -EIO);
	assert(ucm_ipc_client_poll(client, 1) == 1);
	assert(checks[37].calls == 1 && client->cb_free_count == 0);
	assert((checks[37].child_id & UCM_IPC_RING_MASK) == (ids[37] & UCM_IPC_RING_MASK));
	assert(checks[37].child_id != ids[37]);
	consume_submissions(client);
	/* The old ID must not complete the new occupant of the reused slot. */
	push_completion(client, ids[37], 0);
	push_completion(client, 0, 0);
	push_completion(client, UINT64_MAX, 0);
	assert(ucm_ipc_client_poll(client, 0) == 3);
	assert(checks[UCM_IPC_RING_SIZE].calls == 0 && client->cb_free_count == 0);
	for (unsigned i = UCM_IPC_RING_SIZE; i-- > 0;) {
		if (i == 37) { continue; }
		push_completion(client, ids[i], 0);
		assert(ucm_ipc_client_poll(client, 1) == 1);
		assert(checks[i].calls == 1);
	}
	push_completion(client, checks[37].child_id, 0);
	push_completion(client, checks[37].child_id, -EIO);
	assert(ucm_ipc_client_poll(client, 0) == 2);
	assert(checks[UCM_IPC_RING_SIZE].calls == 1);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE);

	/* Nested poll must not make the outer poll consume a completion twice. */
	memset(checks, 0, sizeof(checks));
	checks[0].reenter = client;
	checks[0].poll_nested = 1;
	for (unsigned i = 0; i < 2; i++) {
		assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
			slot_callback, &checks[i], &ids[i]) == 0);
		push_completion(client, ids[i], 0);
	}
	assert(ucm_ipc_client_poll(client, 0) == 1);
	assert(checks[0].calls == 1 && checks[1].calls == 1);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE);
	consume_submissions(client);

	/* wait also dispatches unrelated callbacks instead of leaking their slots. */
	memset(checks, 0, sizeof(checks));
	for (unsigned i = 0; i < 2; i++) {
		assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
			slot_callback, &checks[i], &ids[i]) == 0);
		push_completion(client, ids[i], 0);
	}
	assert(ucm_ipc_client_wait(client, ids[1], 100, &status) == 0 && status == 0);
	assert(checks[0].calls == 1 && checks[1].calls == 1);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE);
	consume_submissions(client);

	/* Exhausted generations must not wrap into an old request ID. */
	next_id = client->next_req_id;
	client->next_req_id = UINT64_MAX / UCM_IPC_RING_SIZE + 1;
	head = atomic_load_explicit(&client->ctl->submit_head, memory_order_relaxed);
	assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
		slot_callback, &checks[0], NULL) == -EOVERFLOW);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE);
	assert(atomic_load_explicit(&client->ctl->submit_head, memory_order_relaxed) == head);
	client->next_req_id = next_id;

	/* Close must also release a table containing an outstanding callback. */
	assert(ucm_ipc_client_submit_cb(client, 0, UCM_IPC_OP_READ, 0, 0, 512,
		slot_callback, &checks[0], NULL) == 0);
	assert(client->cb_free_count == UCM_IPC_RING_SIZE - 1);
}

int
main(void)
{
	char path[128];
	struct fake_target fake;
	struct ucm_ipc_client client;
	int fds[2];
	const size_t sizes[2] = {2u << 20, 4u << 20};
	const uint8_t guards[2] = {0xa5, 0x5a};
	const uint32_t lengths[BATCH_REQUESTS] = {512, 1024, 4096, 1536, 2048, 8192};
	uint8_t *maps[2], *regions[2];
	size_t offsets[2], map_sizes[2];
	uint8_t expected[BATCH_REQUESTS][MAX_PAYLOAD];
	struct callback_check checks[BATCH_REQUESTS];
	unsigned completed = 0;
	uint64_t previous_req = 0;
	long page_size = sysconf(_SC_PAGESIZE);
	int rc;

	snprintf(path, sizeof(path), "/tmp/ucm_ipc_client_test_%ld.sock", (long)getpid());
	assert(page_size > 0);
	fds[0] = memfd_create("ipc_meta_test", MFD_CLOEXEC);
	fds[1] = memfd_create("ipc_cache_test", MFD_CLOEXEC);
	for (unsigned i = 0; i < 2; i++) {
		assert(fds[i] >= 0);
		offsets[i] = (1 + 2 * i) * (size_t)page_size;
		map_sizes[i] = offsets[i] + sizes[i] + (size_t)page_size;
		assert(ftruncate(fds[i], (off_t)map_sizes[i]) == 0);
		maps[i] = mmap(NULL, map_sizes[i], PROT_READ | PROT_WRITE,
			       MAP_SHARED, fds[i], 0);
		assert(maps[i] != MAP_FAILED);
		memset(maps[i], guards[i], map_sizes[i]);
		regions[i] = maps[i] + offsets[i];
	}

	assert(start_fake(&fake, path) == 0);
	rc = ucm_ipc_client_init(&client, path, fds[0], sizes[0], offsets[0], 8, -1,
				 "client-test", "META");
	assert(rc == 0);
	assert(client.session_id == 1);
	assert(client.qpair_depth == 64);
	assert(client.sector_size == 512);
	assert(client.max_xfer_size == 131072);
	assert(client.num_sectors == BACKING_SIZE / 512);

	rc = ucm_ipc_client_register_region(&client, path, 1, fds[1], sizes[1], offsets[1]);
	assert(rc == 0);

	for (unsigned i = 0; i < BATCH_REQUESTS; i++) {
		for (unsigned j = 0; j < lengths[i]; j++) {
			expected[i][j] = (uint8_t)(17 * i + 13 * j + (j >> 8));
		}
		memcpy(regions[i % 2] + 4096 + i * 32768, expected[i], lengths[i]);
	}
	for (unsigned phase = 0; phase < 2; phase++) {
		completed = 0;
		for (unsigned i = 0; i < BATCH_REQUESTS; i++) {
			unsigned region = (i + phase) % 2;
			uint64_t data_offset = (phase == 0 ? 4096 : 16384) + i * 32768;
			uint64_t req;
			checks[i] = (struct callback_check) {
				.index = i, .completed = &completed,
				.actual = regions[region] + data_offset,
				.expected = expected[i], .nbytes = lengths[i],
				.guard = guards[region],
			};
			rc = ucm_ipc_client_submit_cb(&client, region,
				phase == 0 ? UCM_IPC_OP_WRITE : UCM_IPC_OP_READ,
				512 + i * 16384, data_offset, lengths[i],
				check_callback, &checks[i], &req);
			assert(rc == 0 && req > previous_req);
			previous_req = req;
		}
		complete_batch(&client, &completed);
		for (unsigned i = 0; i < BATCH_REQUESTS; i++) {
			assert(checks[i].calls == 1);
			if (phase == 0) {
				/* Reads use the opposite region, after destroying the write source. */
				memset(regions[i % 2] + 4096 + i * 32768, 0xcc, lengths[i]);
			}
		}
	}

	test_callback_slots(&client, &fake);
	ucm_ipc_client_close(&client);
	assert(client.cb_slots == NULL && client.cb_free_count == 0);
	ucm_ipc_client_close(&client);
	stop_fake(&fake);
	assert(fake.session_id == client.session_id && fake.last_region == 1);
	assert(fake.submissions == 2 * BATCH_REQUESTS);
	assert(fake.region_sizes[0] == sizes[0] && fake.region_sizes[1] == sizes[1]);
	/* A failed handshake must release the preallocated table and control map. */
	assert(ucm_ipc_client_init(&client, "", fds[0], sizes[0], offsets[0], 8, -1,
		"failed-client", "META") < 0);
	assert(client.cb_slots == NULL && client.ctl == NULL && client.control_fd == -1);
	ucm_ipc_client_close(&client);
	for (unsigned i = 0; i < 2; i++) {
		for (size_t j = 0; j < offsets[i]; j++) { assert(maps[i][j] == guards[i]); }
		for (size_t j = offsets[i] + sizes[i]; j < map_sizes[i]; j++) {
			assert(maps[i][j] == guards[i]);
		}
		assert(munmap(maps[i], map_sizes[i]) == 0);
		assert(close(fds[i]) == 0);
	}
	printf("PASS: offset metadata/cache FD mappings, payload write/read coherence, "
	       "reverse-order callbacks, slot reuse, stale/duplicate rejection, immediate "
	       "slot backpressure, reentrant callbacks and cleanup\n");
	return 0;
}
