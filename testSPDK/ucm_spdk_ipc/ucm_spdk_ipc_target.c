/*
 * ucm_spdk_ipc_target - local zero-copy NVMe executor for UCM workers.
 *
 * The target owns the local PCIe NVMe controller.  Every worker session gets
 * its own NVMe I/O qpair in this process, plus a shared-memory submission /
 * completion ring and a shared data buffer (normally the UCM Cache SHM).
 *
 * Data path:
 *   worker ring -> target reactor -> session qpair -> local NVMe DMA
 *   local NVMe DMA -> worker registered buffer -> target completion ring
 *
 * There is no NVMe-oF target and no target bounce buffer.
 */

#include "ipc_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
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

#include "spdk/env.h"
#include "spdk/nvme.h"

#define DEFAULT_TRID "trtype:PCIe traddr:0000:84:00.0"
#define DEFAULT_SOCK "/tmp/ucm_spdk_ipc.sock"
#define DEFAULT_CPUS \
	4, 36, 68, 100, 132, 164, 196, 228, \
	5, 37, 69, 101, 133, 165, 197, 229, \
	6, 38, 70, 102, 134, 166, 198, 230, \
	7, 39, 71, 103, 135, 167, 199, 231
#define DEFAULT_REACTORS 32u
#define DEFAULT_CPU_BASE 132
#define DEFAULT_QPAIR_DEPTH 1024u
#define DEFAULT_MEM_MB 8192
#define MAX_QPAIR_DEPTH 4096u
#define CLOSE_TIMEOUT_MS 5000

struct ipc_reactor;
struct ipc_session;

struct ipc_task {
	struct ipc_session *session;
	uint64_t req_id;
	uint64_t start_ticks;
	uint32_t nbytes;
	uint16_t opcode;
	uint8_t in_use;
};

struct ipc_region {
	struct data_mapping *mapping;
	uint8_t *data;
	size_t size;
};

struct data_mapping {
	dev_t dev;
	ino_t ino;
	off_t offset;
	size_t size;
	void *base;
	void *data;
	size_t map_size;
	int refs;
	struct data_mapping *next;
};

struct ipc_session {
	struct ucm_ipc_control *ctl;
	size_t control_size;
	struct ipc_region regions[UCM_IPC_MAX_REGIONS];
	struct spdk_nvme_qpair *qpair;
	struct ipc_reactor *reactor;
	uint32_t session_id;
	uint32_t max_outstanding;
	uint32_t qpair_depth;
	uint32_t outstanding;
	struct ipc_task *tasks;
	uint32_t *free_tasks;
	uint32_t free_top;
	bool close_started;
	uint64_t close_deadline_ms;
	pthread_mutex_t attach_mu;
	pthread_cond_t attach_cv;
	bool attach_done;
	int attach_status;
	struct ipc_session *next_pending;
	struct ipc_session *next_active;
};

struct ipc_reactor_stats {
	/* 128 bytes covers cache lines on both x86 and ARM targets. */
	_Alignas(128) _Atomic uint64_t bytes_read;
	_Atomic uint64_t bytes_written;
	_Atomic uint64_t read_ops;
	_Atomic uint64_t write_ops;
	_Atomic uint64_t flush_ops;
	_Atomic uint64_t read_latency_ticks;
	_Atomic uint64_t write_latency_ticks;
	_Atomic uint64_t completion_errors;
	_Atomic uint64_t submit_errors;
};

struct ipc_reactor {
	pthread_t thread;
	uint32_t index;
	uint32_t lcore_id;
	int cpu_id;
	bool admin;
	pthread_mutex_t mu;
	struct ipc_session *pending;
	struct ipc_session *active;
	/* Reactor-owned totals survive session detach and do not share cache lines. */
	struct ipc_reactor_stats stats;
};

struct target_args {
	const char *trid;
	const char *sock_path;
	const char *lcore_map;
	int cpu_base;
	int cpus[CPU_SETSIZE];
	uint32_t num_cpus;
	bool explicit_cpus;
	uint32_t reactors;
	uint32_t qpair_depth;
	int mem_mb;
	const char *iova_mode;
};

static struct target_args g_args = {
	.trid = DEFAULT_TRID,
	.sock_path = DEFAULT_SOCK,
	.lcore_map = NULL,
	.cpu_base = DEFAULT_CPU_BASE,
	.cpus = {DEFAULT_CPUS},
	.num_cpus = DEFAULT_REACTORS,
	.reactors = DEFAULT_REACTORS,
	.qpair_depth = DEFAULT_QPAIR_DEPTH,
	.mem_mb = DEFAULT_MEM_MB,
	.iova_mode = NULL,
};

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static uint32_t g_sector_size;
static uint64_t g_num_sectors;
static uint32_t g_max_xfer;
static volatile sig_atomic_t g_stop;
static bool g_env_initialized;
static uint32_t g_reactors_started;
static int g_listen_fd = -1;
static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
static struct data_mapping *g_mappings;
static pthread_mutex_t g_sessions_mu = PTHREAD_MUTEX_INITIALIZER;
static struct ipc_session *g_sessions;
static struct ipc_reactor *g_reactors;
static uint32_t g_nreactors;
static uint32_t g_next_reactor;
static _Atomic uint32_t g_next_session_id = 1;
static _Atomic uint32_t g_active_sessions;

static void
reactor_stats_init(struct ipc_reactor_stats *stats)
{
	atomic_init(&stats->bytes_read, 0);
	atomic_init(&stats->bytes_written, 0);
	atomic_init(&stats->read_ops, 0);
	atomic_init(&stats->write_ops, 0);
	atomic_init(&stats->flush_ops, 0);
	atomic_init(&stats->read_latency_ticks, 0);
	atomic_init(&stats->write_latency_ticks, 0);
	atomic_init(&stats->completion_errors, 0);
	atomic_init(&stats->submit_errors, 0);
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	if (g_listen_fd >= 0) {
		close(g_listen_fd);
		g_listen_fd = -1;
	}
}

static int
set_affinity(int cpu)
{
	cpu_set_t set;
	int rc;

	if (cpu < 0) {
		return 0;
	}
	if (cpu >= CPU_SETSIZE) {
		fprintf(stderr, "cpu id %d exceeds CPU_SETSIZE\n", cpu);
		return -1;
	}
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	rc = sched_setaffinity(0, sizeof(set), &set);
	return rc;
}

static int
send_reply(int fd, const struct ucm_ipc_reply *reply)
{
	ssize_t n;

	do {
		n = send(fd, reply, sizeof(*reply), MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	return n == (ssize_t)sizeof(*reply) ? 0 : -errno;
}

static int
recv_msg(int fd, void *buf, size_t cap, int fds[UCM_IPC_HELLO_FDS], int *nfd)
{
	struct msghdr msg = {0};
	struct iovec iov = {.iov_base = buf, .iov_len = cap};
	char cbuf[CMSG_SPACE(sizeof(int) * UCM_IPC_HELLO_FDS)] = {0};
	struct cmsghdr *cmsg;
	ssize_t n;
	int i;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	do {
		n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
	} while (n < 0 && errno == EINTR);
	if (n <= 0) {
		return n < 0 ? -errno : -EINVAL;
	}

	for (i = 0; i < UCM_IPC_HELLO_FDS; i++) {
		fds[i] = -1;
	}
	*nfd = 0;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			continue;
		}
		int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
		int *p = (int *)CMSG_DATA(cmsg);
		for (i = 0; i < count && *nfd < UCM_IPC_HELLO_FDS; i++) {
			fds[(*nfd)++] = p[i];
		}
		/* Close any extra fds we cannot track. */
		for (; i < count; i++) {
			close(p[i]);
		}
	}
	return (int)n;
}

static bool
range_vtophys_valid(uint64_t addr, uint64_t len)
{
	while (len > 0) {
		uint64_t chunk = len;

		if (spdk_vtophys((void *)(uintptr_t)addr, &chunk) == SPDK_VTOPHYS_ERROR ||
		    chunk == 0) {
			return false;
		}
		addr += chunk;
		len -= chunk;
	}
	return true;
}

static struct data_mapping *
data_mapping_get(int fd, uint64_t file_offset, uint64_t size, int *err)
{
	struct data_mapping *m;
	struct stat st;
	void *base;
	int rc;

	*err = 0;
	if (size == 0 || file_offset + size < file_offset) {
		*err = -EINVAL;
		return NULL;
	}
	if (fstat(fd, &st) != 0) {
		*err = -errno;
		return NULL;
	}
	if ((uint64_t)st.st_size < file_offset + size) {
		*err = -EINVAL;
		return NULL;
	}
	if ((file_offset & 4095u) != 0 || (size & 4095u) != 0) {
		*err = -EINVAL;
		return NULL;
	}

	pthread_mutex_lock(&g_map_mu);
	for (m = g_mappings; m != NULL; m = m->next) {
		if (m->dev == st.st_dev && m->ino == st.st_ino &&
		    m->offset == (off_t)file_offset && m->size == (size_t)size) {
			m->refs++;
			pthread_mutex_unlock(&g_map_mu);
			return m;
		}
	}
	pthread_mutex_unlock(&g_map_mu);

	base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		*err = -errno;
		return NULL;
	}
	rc = spdk_mem_reserve((uint8_t *)base + file_offset, (size_t)size);
	if (rc != 0 && rc != -EBUSY) {
		munmap(base, (size_t)st.st_size);
		*err = rc;
		return NULL;
	}
	rc = spdk_mem_register((uint8_t *)base + file_offset, (size_t)size);
	if (rc != 0) {
		munmap(base, (size_t)st.st_size);
		*err = rc;
		return NULL;
	}
	if (!range_vtophys_valid((uint64_t)(uintptr_t)((uint8_t *)base + file_offset), size)) {
		/* In vfio no-IOMMU/PA mode SPDK can only translate 2MB hugepage
		 * memory; a normal 4KB/memfd or THP-backed buffer is rejected. */
		spdk_mem_unregister((uint8_t *)base + file_offset, (size_t)size);
		munmap(base, (size_t)st.st_size);
		*err = -ENOTSUP;
		return NULL;
	}

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		spdk_mem_unregister((uint8_t *)base + file_offset, (size_t)size);
		munmap(base, (size_t)st.st_size);
		*err = -ENOMEM;
		return NULL;
	}
	m->dev = st.st_dev;
	m->ino = st.st_ino;
	m->offset = (off_t)file_offset;
	m->size = (size_t)size;
	m->base = base;
	m->data = (uint8_t *)base + file_offset;
	m->map_size = (size_t)st.st_size;
	m->refs = 1;

	pthread_mutex_lock(&g_map_mu);
	m->next = g_mappings;
	g_mappings = m;
	pthread_mutex_unlock(&g_map_mu);
	return m;
}

static void
data_mapping_put(struct data_mapping *m)
{
	struct data_mapping **p;
	bool free_it = false;
	void *base;
	void *data;
	size_t size;
	size_t map_size;

	if (m == NULL) {
		return;
	}
	pthread_mutex_lock(&g_map_mu);
	if (--m->refs == 0) {
		for (p = &g_mappings; *p != NULL; p = &(*p)->next) {
			if (*p == m) {
				*p = m->next;
				break;
			}
		}
		free_it = true;
	}
	base = m->base;
	data = m->data;
	size = m->size;
	map_size = m->map_size;
	pthread_mutex_unlock(&g_map_mu);

	if (!free_it) {
		return;
	}
	spdk_mem_unregister(data, size);
	munmap(base, map_size);
	free(m);
}

static struct ipc_session *
session_alloc(void)
{
	struct ipc_session *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		return NULL;
	}
	s->session_id = __atomic_add_fetch(&g_next_session_id, 1, __ATOMIC_RELAXED);
	pthread_mutex_init(&s->attach_mu, NULL);
	pthread_cond_init(&s->attach_cv, NULL);
	return s;
}

static void
session_registry_add(struct ipc_session *s)
{
	pthread_mutex_lock(&g_sessions_mu);
	/* Reuse a separate registry link because next_active belongs to a reactor. */
	s->next_pending = g_sessions;
	g_sessions = s;
	atomic_fetch_add_explicit(&g_active_sessions, 1, memory_order_relaxed);
	pthread_mutex_unlock(&g_sessions_mu);
}

static void
session_registry_remove(struct ipc_session *s)
{
	struct ipc_session **p;

	pthread_mutex_lock(&g_sessions_mu);
	for (p = &g_sessions; *p != NULL; p = &(*p)->next_pending) {
		if (*p == s) {
			*p = s->next_pending;
			atomic_fetch_sub_explicit(&g_active_sessions, 1, memory_order_relaxed);
			break;
		}
	}
	pthread_mutex_unlock(&g_sessions_mu);
}


static void
session_free(struct ipc_session *s)
{
	if (s == NULL) {
		return;
	}
	pthread_mutex_destroy(&s->attach_mu);
	pthread_cond_destroy(&s->attach_cv);
	free(s->tasks);
	free(s->free_tasks);
	free(s);
}

static void
reactor_push_pending(struct ipc_reactor *r, struct ipc_session *s)
{
	pthread_mutex_lock(&r->mu);
	s->next_pending = r->pending;
	r->pending = s;
	pthread_mutex_unlock(&r->mu);
}

static void
session_add_active(struct ipc_reactor *r, struct ipc_session *s)
{
	s->reactor = r;
	s->next_active = r->active;
	r->active = s;
}

static void
reactor_detach_session(struct ipc_reactor *r, struct ipc_session *s)
{
	struct ipc_session **p;

	for (p = &r->active; *p != NULL; p = &(*p)->next_active) {
		if (*p == s) {
			*p = s->next_active;
			break;
		}
	}
	if (s->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(s->qpair);
		s->qpair = NULL;
	}
	session_registry_remove(s);
	for (uint32_t i = 0; i < UCM_IPC_MAX_REGIONS; i++) {
		data_mapping_put(s->regions[i].mapping);
		s->regions[i].mapping = NULL;
		s->regions[i].data = NULL;
		s->regions[i].size = 0;
	}
	if (s->ctl != NULL) {
		munmap(s->ctl, s->control_size);
		s->ctl = NULL;
	}
	session_free(s);
}

static int
reactor_attach_session(struct ipc_reactor *r, struct ipc_session *s)
{
	struct spdk_nvme_io_qpair_opts opts;
	uint32_t i;
	int rc;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &opts, sizeof(opts));
	opts.io_queue_size = s->qpair_depth;
	opts.io_queue_requests = 16384;
	s->qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &opts, sizeof(opts));
	if (s->qpair == NULL) {
		return -ENOMEM;
	}

	s->tasks = calloc(s->qpair_depth, sizeof(*s->tasks));
	s->free_tasks = calloc(s->qpair_depth, sizeof(*s->free_tasks));
	if (s->tasks == NULL || s->free_tasks == NULL) {
		spdk_nvme_ctrlr_free_io_qpair(s->qpair);
		s->qpair = NULL;
		free(s->tasks);
		free(s->free_tasks);
		s->tasks = NULL;
		s->free_tasks = NULL;
		return -ENOMEM;
	}
	s->free_top = s->qpair_depth;
	for (i = 0; i < s->qpair_depth; i++) {
		s->free_tasks[i] = s->qpair_depth - 1 - i;
		s->tasks[i].session = s;
	}

	session_add_active(r, s);
	(void)rc;
	return 0;
}

static void
reactor_drain_pending(struct ipc_reactor *r)
{
	struct ipc_session *s;
	struct ipc_session *pending;

	pthread_mutex_lock(&r->mu);
	pending = r->pending;
	r->pending = NULL;
	pthread_mutex_unlock(&r->mu);

	while (pending != NULL) {
		s = pending;
		pending = s->next_pending;
		s->next_pending = NULL;
		int rc = reactor_attach_session(r, s);
		pthread_mutex_lock(&s->attach_mu);
		s->attach_status = rc;
		s->attach_done = true;
		pthread_cond_signal(&s->attach_cv);
		pthread_mutex_unlock(&s->attach_mu);
	}
}

static struct ipc_task *
session_task_alloc(struct ipc_session *s, uint64_t req_id)
{
	struct ipc_task *t;
	uint32_t idx;

	if (s->free_top == 0) {
		return NULL;
	}
	idx = s->free_tasks[--s->free_top];
	t = &s->tasks[idx];
	t->req_id = req_id;
	t->in_use = 1;
	return t;
}

static void
session_task_free(struct ipc_session *s, struct ipc_task *t)
{
	uint32_t idx = (uint32_t)(t - s->tasks);

	t->in_use = 0;
	s->free_tasks[s->free_top++] = idx;
}

static uint32_t
completion_available(const struct ucm_ipc_control *ctl, uint32_t outstanding)
{
	uint32_t head = atomic_load_explicit(&ctl->completion_head, memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&ctl->completion_tail, memory_order_acquire);
	uint32_t used = ucm_ipc_ring_used(head, tail);
	uint32_t reserved = used + outstanding;

	if (reserved >= UCM_IPC_RING_SIZE - 1u) {
		return 0;
	}
	return UCM_IPC_RING_SIZE - 1u - reserved;
}

static void
session_complete(struct ipc_session *s, uint64_t req_id, int status)
{
	struct ucm_ipc_control *ctl = s->ctl;
	uint32_t head = atomic_load_explicit(&ctl->completion_head, memory_order_relaxed);

	ctl->completion[head & UCM_IPC_RING_MASK].req_id = req_id;
	ctl->completion[head & UCM_IPC_RING_MASK].status = status;
	ctl->completion[head & UCM_IPC_RING_MASK].reserved = 0;
	atomic_store_explicit(&ctl->completion_head, head + 1, memory_order_release);
}

static void
io_complete_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct ipc_task *t = arg;
	struct ipc_session *s = t->session;
	struct ipc_reactor_stats *stats = &s->reactor->stats;
	uint64_t now = spdk_get_ticks();
	uint64_t latency = now >= t->start_ticks ? now - t->start_ticks : 0;
	int status = 0;

	if (cpl->status.sct != 0 || cpl->status.sc != 0) {
		status = -EIO;
	}
	if (t->opcode == UCM_IPC_OP_READ) {
		atomic_fetch_add_explicit(&stats->bytes_read, t->nbytes, memory_order_relaxed);
		atomic_fetch_add_explicit(&stats->read_ops, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&stats->read_latency_ticks, latency, memory_order_relaxed);
	} else if (t->opcode == UCM_IPC_OP_WRITE) {
		atomic_fetch_add_explicit(&stats->bytes_written, t->nbytes, memory_order_relaxed);
		atomic_fetch_add_explicit(&stats->write_ops, 1, memory_order_relaxed);
		atomic_fetch_add_explicit(&stats->write_latency_ticks, latency, memory_order_relaxed);
	} else if (t->opcode == UCM_IPC_OP_FLUSH) {
		atomic_fetch_add_explicit(&stats->flush_ops, 1, memory_order_relaxed);
	}
	if (status != 0) {
		atomic_fetch_add_explicit(&stats->completion_errors, 1, memory_order_relaxed);
	}
	if (s->outstanding > 0) {
		s->outstanding--;
	}
	session_complete(s, t->req_id, status);
	session_task_free(s, t);
}

static void
session_complete_invalid(struct ipc_session *s, uint64_t req_id, int status)
{
	session_complete(s, req_id, status);
}

static void
session_handle_op(struct ipc_session *s, const struct ucm_ipc_submit_entry *e)
{
	struct ipc_task *t;
	uint64_t offset = e->offset_bytes;
	uint64_t data_offset = e->data_offset;
	uint32_t nbytes = e->nbytes;
	uint64_t lba;
	uint32_t lba_count;
	struct ipc_region *region;
	int rc;

	if (e->opcode == UCM_IPC_OP_FLUSH) {
		t = session_task_alloc(s, e->req_id);
		if (t == NULL) {
			session_complete_invalid(s, e->req_id, -EBUSY);
			return;
		}
		t->opcode = UCM_IPC_OP_FLUSH;
		t->nbytes = 0;
		t->start_ticks = spdk_get_ticks();
		s->outstanding++;
		rc = spdk_nvme_ns_cmd_flush(g_ns, s->qpair, io_complete_cb, t);
		if (rc != 0) {
			atomic_fetch_add_explicit(&s->reactor->stats.submit_errors, 1,
					  memory_order_relaxed);
			s->outstanding--;
			session_task_free(s, t);
			session_complete_invalid(s, e->req_id, rc);
		}
		return;
	}

	if (e->region_id >= UCM_IPC_MAX_REGIONS ||
	    s->regions[e->region_id].data == NULL ||
	    s->regions[e->region_id].size == 0) {
		session_complete_invalid(s, e->req_id, -EINVAL);
		return;
	}
	region = &s->regions[e->region_id];
	if (nbytes == 0 || nbytes % g_sector_size != 0 ||
	    offset % g_sector_size != 0 ||
	    data_offset % g_sector_size != 0 ||
	    data_offset + nbytes < data_offset ||
	    data_offset + nbytes > region->size ||
	    offset + nbytes < offset ||
	    offset + nbytes > g_num_sectors * (uint64_t)g_sector_size) {
		session_complete_invalid(s, e->req_id, -EINVAL);
		return;
	}

	lba = offset / g_sector_size;
	lba_count = nbytes / g_sector_size;
	t = session_task_alloc(s, e->req_id);
	if (t == NULL) {
		session_complete_invalid(s, e->req_id, -EBUSY);
		return;
	}
	t->opcode = e->opcode;
	t->nbytes = nbytes;
	t->start_ticks = spdk_get_ticks();

	s->outstanding++;
	if (e->opcode == UCM_IPC_OP_READ) {
		rc = spdk_nvme_ns_cmd_read(g_ns, s->qpair, region->data + data_offset, lba,
					   lba_count, io_complete_cb, t, 0);
	} else if (e->opcode == UCM_IPC_OP_WRITE) {
		rc = spdk_nvme_ns_cmd_write(g_ns, s->qpair, region->data + data_offset, lba,
					    lba_count, io_complete_cb, t, 0);
	} else {
		rc = -EINVAL;
	}
	if (rc != 0) {
		atomic_fetch_add_explicit(&s->reactor->stats.submit_errors, 1,
					  memory_order_relaxed);
		s->outstanding--;
		session_task_free(s, t);
		session_complete_invalid(s, e->req_id, rc);
	}
}

static void
session_process_submissions(struct ipc_session *s)
{
	struct ucm_ipc_control *ctl = s->ctl;
	uint32_t head;
	uint32_t tail;

	head = atomic_load_explicit(&ctl->submit_head, memory_order_acquire);
	tail = atomic_load_explicit(&ctl->submit_tail, memory_order_relaxed);

	while (tail != head) {
		struct ucm_ipc_submit_entry e;

		if (s->outstanding >= s->max_outstanding ||
		    completion_available(ctl, s->outstanding) == 0) {
			break;
		}
		e = ctl->submit[tail & UCM_IPC_RING_MASK];
		tail++;
		atomic_store_explicit(&ctl->submit_tail, tail, memory_order_release);
		session_handle_op(s, &e);
		head = atomic_load_explicit(&ctl->submit_head, memory_order_acquire);
	}
}

static void
reactor_run(struct ipc_reactor *r)
{
	if (set_affinity(r->cpu_id) != 0) {
		fprintf(stderr, "reactor %u: failed to pin CPU %d: %s\n",
			r->index, r->cpu_id, strerror(errno));
		g_stop = 1;
		return;
	}

	while (!g_stop) {
		struct ipc_session *s;
		struct ipc_session *next;

		reactor_drain_pending(r);
		for (s = r->active; s != NULL; s = next) {
			next = s->next_active;

			if (atomic_load_explicit(&s->ctl->closing, memory_order_acquire) &&
			    !s->close_started) {
				if (s->outstanding == 0) {
					atomic_store_explicit(&s->ctl->closed, 1,
							      memory_order_release);
					s->close_started = true;
					s->close_deadline_ms = now_ms() + CLOSE_TIMEOUT_MS;
				}
			}

			if (s->close_started) {
				if (atomic_load_explicit(&s->ctl->close_ack,
							 memory_order_acquire) ||
				    now_ms() >= s->close_deadline_ms) {
					reactor_detach_session(r, s);
					continue;
				}
			}

			session_process_submissions(s);
			spdk_nvme_qpair_process_completions(s->qpair, 0);
		}

		if (r->admin) {
			spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
		}
	}
}

static void *reactor_thread(void *arg)
{
	struct ipc_reactor *r = arg;

	reactor_run(r);
	return NULL;
}

static struct ipc_reactor *
choose_reactor(int32_t numa_id)
{
	uint32_t i;

	if (numa_id >= 0) {
		for (i = 0; i < g_nreactors; i++) {
			if (spdk_env_get_numa_id(g_reactors[i].lcore_id) == numa_id) {
				return &g_reactors[i];
			}
		}
	}
	return &g_reactors[g_next_reactor++ % g_nreactors];
}

static int
setup_listener(const char *path)
{
	struct sockaddr_un addr;
	int fd;
	int rc;

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
	unlink(path);
	rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	if (listen(fd, 64) != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	chmod(path, 0666);
	return fd;
}

static int
handle_hello(int fd, const struct ucm_ipc_hello *hello, int fds[UCM_IPC_HELLO_FDS],
	     int nfds)
{
	struct ucm_ipc_reply reply = {.magic = UCM_IPC_MAGIC, .status = 0};
	struct ipc_session *s = NULL;
	struct ipc_reactor *r;
	void *ctl_map = NULL;
	bool attached = false;
	int rc;

	if (nfds != UCM_IPC_HELLO_FDS) {
		rc = -EINVAL;
		goto fail_no_fds;
	}
	if (hello->magic != UCM_IPC_MAGIC || hello->version != UCM_IPC_VERSION ||
	    hello->kind != UCM_IPC_KIND_HELLO || hello->size != sizeof(*hello) ||
	    hello->control_size < sizeof(struct ucm_ipc_control) ||
	    hello->data_size == 0 || hello->max_outstanding == 0 ||
	    hello->max_outstanding > UCM_IPC_RING_SIZE - 1u ||
	    (hello->sector_size != 0 && hello->sector_size != g_sector_size)) {
		rc = -EINVAL;
		goto fail_no_fds;
	}

	ctl_map = mmap(NULL, (size_t)hello->control_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fds[0], 0);
	if (ctl_map == MAP_FAILED) {
		ctl_map = NULL;
		rc = -errno;
		goto fail_no_fds;
	}
	{
		struct ucm_ipc_control *ctl = ctl_map;
		if (ctl->magic != UCM_IPC_MAGIC || ctl->version != UCM_IPC_VERSION ||
		    ctl->header_size != sizeof(*ctl) || ctl->ring_size != UCM_IPC_RING_SIZE ||
		    ctl->submit_size != sizeof(ctl->submit[0]) ||
		    ctl->completion_size != sizeof(ctl->completion[0]) ||
		    ctl->max_outstanding != hello->max_outstanding ||
		    ctl->data_size != hello->data_size) {
			rc = -EINVAL;
			goto fail_no_fds;
		}
		ctl->sector_size = g_sector_size;
	}

	s = session_alloc();
	if (s == NULL) {
		rc = -ENOMEM;
		goto fail_no_fds;
	}
	s->control_size = (size_t)hello->control_size;
	s->ctl = ctl_map;
	ctl_map = NULL;
	s->max_outstanding = hello->max_outstanding;
	s->qpair_depth = g_args.qpair_depth;
	if (s->qpair_depth < s->max_outstanding) {
		s->qpair_depth = s->max_outstanding;
	}
	if (s->qpair_depth > MAX_QPAIR_DEPTH) {
		rc = -EINVAL;
		goto fail;
	}
	s->regions[0].mapping = data_mapping_get(fds[1], hello->data_file_offset,
						 hello->data_size, &rc);
	if (s->regions[0].mapping == NULL) {
		goto fail;
	}
	s->regions[0].data = s->regions[0].mapping->data;
	s->regions[0].size = hello->data_size;

	r = choose_reactor(hello->numa_id);
	reactor_push_pending(r, s);
	pthread_mutex_lock(&s->attach_mu);
	while (!s->attach_done) {
		pthread_cond_wait(&s->attach_cv, &s->attach_mu);
	}
	rc = s->attach_status;
	pthread_mutex_unlock(&s->attach_mu);
	if (rc != 0) {
		data_mapping_put(s->regions[0].mapping);
		s->regions[0].mapping = NULL;
		munmap(s->ctl, s->control_size);
		s->ctl = NULL;
		session_free(s);
		s = NULL;
		goto fail_no_fds;
	}
	attached = true;
	session_registry_add(s);

	reply.session_id = s->session_id;
	reply.qpair_depth = s->qpair_depth;
	reply.sector_size = g_sector_size;
	reply.max_xfer_size = g_max_xfer;
	reply.data_size = hello->data_size;
	reply.num_sectors = g_num_sectors;
	rc = send_reply(fd, &reply);
	if (rc != 0) {
		atomic_store_explicit(&s->ctl->closing, 1, memory_order_release);
	}
	close(fds[0]);
	close(fds[1]);
	close(fd);
	return rc;

fail:
	reply.status = rc;
	if (rc == -ENOTSUP) {
		fprintf(stderr, "target: data buffer not DMA-mappable in PA mode; "
			"use 2MB hugepages (MFD_HUGETLB) or enable IOMMU/VA\n");
	}
	if (attached) {
		atomic_store_explicit(&s->ctl->closing, 1, memory_order_release);
	} else if (s != NULL) {
		data_mapping_put(s->regions[0].mapping);
		if (s->ctl != NULL) {
			munmap(s->ctl, s->control_size);
		}
		session_free(s);
	} else if (ctl_map != NULL) {
		munmap(ctl_map, (size_t)hello->control_size);
	}
fail_no_fds:
	reply.status = rc;
	for (int i = 0; i < nfds && i < UCM_IPC_HELLO_FDS; i++) {
		if (fds[i] >= 0) {
			close(fds[i]);
		}
	}
	if (!attached) {
		(void)send_reply(fd, &reply);
	}
	close(fd);
	return rc;
}

static int
handle_region(int fd, const struct ucm_ipc_region_hello *rh,
	      int fds[UCM_IPC_HELLO_FDS], int nfds)
{
	struct ucm_ipc_region_reply reply = {
		.magic = UCM_IPC_MAGIC,
		.status = 0,
		.session_id = rh->session_id,
		.region_id = rh->region_id,
	};
	struct ipc_session *s;
	struct data_mapping *mapping;
	int rc = 0;

	if (nfds != UCM_IPC_REGION_FDS ||
	    rh->magic != UCM_IPC_MAGIC || rh->version != UCM_IPC_VERSION ||
	    rh->kind != UCM_IPC_KIND_REGION || rh->size != sizeof(*rh) ||
	    rh->region_id == 0 || rh->region_id >= UCM_IPC_MAX_REGIONS ||
	    rh->data_size == 0) {
		rc = -EINVAL;
		goto out;
	}

	pthread_mutex_lock(&g_sessions_mu);
	for (s = g_sessions; s != NULL; s = s->next_pending) {
		if (s->session_id == rh->session_id) {
			break;
		}
	}
	if (s == NULL) {
		pthread_mutex_unlock(&g_sessions_mu);
		rc = -ENOENT;
		goto out;
	}
	if (s->regions[rh->region_id].mapping != NULL) {
		pthread_mutex_unlock(&g_sessions_mu);
		rc = -EBUSY;
		goto out;
	}
	mapping = data_mapping_get(fds[0], rh->data_file_offset, rh->data_size, &rc);
	if (mapping != NULL) {
		s->regions[rh->region_id].mapping = mapping;
		s->regions[rh->region_id].data = mapping->data;
		s->regions[rh->region_id].size = rh->data_size;
		reply.data_size = rh->data_size;
	}
	pthread_mutex_unlock(&g_sessions_mu);
	if (rc == -ENOTSUP) {
		fprintf(stderr, "target: region %u not DMA-mappable in PA mode; "
			"use 2MB hugepages\n", rh->region_id);
	}

out:
	reply.status = rc;
	if (nfds > 0 && fds[0] >= 0) {
		close(fds[0]);
	}
	{
		ssize_t n;
		do {
			n = send(fd, &reply, sizeof(reply), MSG_NOSIGNAL);
		} while (n < 0 && errno == EINTR);
	}
	close(fd);
	return rc;
}

static int
handle_stats(int fd)
{
	char buf[2048];
	uint64_t tick_rate = spdk_get_ticks_hz();
	uint64_t ticks = spdk_get_ticks();
	uint64_t bytes_read = 0, bytes_written = 0;
	uint64_t read_ops = 0, write_ops = 0, flush_ops = 0;
	uint64_t read_latency_ticks = 0, write_latency_ticks = 0;
	uint64_t completion_errors = 0, submit_errors = 0;
	int n;
	size_t done = 0;

	/* Relaxed snapshots, as before; reactors remain allocated while serving stats. */
	for (uint32_t i = 0; i < g_nreactors; i++) {
		const struct ipc_reactor_stats *stats = &g_reactors[i].stats;

		bytes_read += atomic_load_explicit(&stats->bytes_read, memory_order_relaxed);
		bytes_written += atomic_load_explicit(&stats->bytes_written, memory_order_relaxed);
		read_ops += atomic_load_explicit(&stats->read_ops, memory_order_relaxed);
		write_ops += atomic_load_explicit(&stats->write_ops, memory_order_relaxed);
		flush_ops += atomic_load_explicit(&stats->flush_ops, memory_order_relaxed);
		read_latency_ticks += atomic_load_explicit(&stats->read_latency_ticks,
							 memory_order_relaxed);
		write_latency_ticks += atomic_load_explicit(&stats->write_latency_ticks,
							  memory_order_relaxed);
		completion_errors += atomic_load_explicit(&stats->completion_errors,
							memory_order_relaxed);
		submit_errors += atomic_load_explicit(&stats->submit_errors, memory_order_relaxed);
	}

	n = snprintf(buf, sizeof(buf),
		     "{\"version\":1,\"name\":\"ipc-target\","
		     "\"tick_rate\":%" PRIu64 ",\"ticks\":%" PRIu64 ","
		     "\"bytes_read\":%" PRIu64 ",\"bytes_written\":%" PRIu64 ","
		     "\"num_read_ops\":%" PRIu64 ",\"num_write_ops\":%" PRIu64 ","
		     "\"num_flush_ops\":%" PRIu64 ","
		     "\"read_latency_ticks\":%" PRIu64 ","
		     "\"write_latency_ticks\":%" PRIu64 ","
		     "\"completion_errors\":%" PRIu64 ","
		     "\"submit_errors\":%" PRIu64 ","
		     "\"connections\":%u,\"qpairs\":%u,"
		     "\"num_sectors\":%" PRIu64 ",\"sector_size\":%u}\n",
		     tick_rate, ticks,
		     bytes_read, bytes_written, read_ops, write_ops, flush_ops,
		     read_latency_ticks, write_latency_ticks, completion_errors, submit_errors,
		     atomic_load_explicit(&g_active_sessions, memory_order_relaxed),
		     atomic_load_explicit(&g_active_sessions, memory_order_relaxed),
		     g_num_sectors, g_sector_size);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		close(fd);
		return -EOVERFLOW;
	}
	while (done < (size_t)n) {
		ssize_t written = send(fd, buf + done, (size_t)n - done, MSG_NOSIGNAL);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			return -errno;
		}
		done += (size_t)written;
	}
	close(fd);
	return 0;
}

static int
handle_connection(int fd)
{
	union {
		struct ucm_ipc_hello hello;
		struct ucm_ipc_region_hello region;
		uint8_t raw[512];
	} msg;
	int fds[UCM_IPC_HELLO_FDS] = {-1, -1};
	int nfds = 0;
	int n;
	uint32_t magic;
	uint16_t version;
	uint16_t kind;

	n = recv_msg(fd, msg.raw, sizeof(msg.raw), fds, &nfds);
	if (n < 0) {
		goto err;
	}
	if (n >= 9 && memcmp(msg.raw, "UCM_STATS", 9) == 0) {
		for (int i = 0; i < nfds && i < UCM_IPC_HELLO_FDS; i++) {
			if (fds[i] >= 0) { close(fds[i]); }
		}
		return handle_stats(fd);
	}
	if (n < 8) {
		goto err;
	}
	memcpy(&magic, msg.raw, sizeof(magic));
	memcpy(&version, msg.raw + 4, sizeof(version));
	memcpy(&kind, msg.raw + 6, sizeof(kind));
	if (magic != UCM_IPC_MAGIC || version != UCM_IPC_VERSION) {
		goto err;
	}
	if (kind == UCM_IPC_KIND_HELLO) {
		if (n < (int)sizeof(msg.hello)) { goto err; }
		return handle_hello(fd, &msg.hello, fds, nfds);
	}
	if (kind == UCM_IPC_KIND_REGION) {
		if (n < (int)sizeof(msg.region)) { goto err; }
		return handle_region(fd, &msg.region, fds, nfds);
	}
err:
	for (int i = 0; i < nfds && i < UCM_IPC_HELLO_FDS; i++) {
		if (fds[i] >= 0) { close(fds[i]); }
	}
	close(fd);
	return -EINVAL;
}

static int
parse_cpu_list(const char *list, struct target_args *args)
{
	cpu_set_t seen;
	int cpus[CPU_SETSIZE];
	uint32_t count = 0;
	const char *p = list;

	CPU_ZERO(&seen);
	if (p == NULL || *p == '\0') {
		return -1;
	}
	for (;;) {
		unsigned cpu = 0;

		if (*p < '0' || *p > '9') {
			return -1;
		}
		do {
			cpu = cpu * 10u + (unsigned)(*p++ - '0');
			if (cpu >= CPU_SETSIZE) {
				return -1;
			}
		} while (*p >= '0' && *p <= '9');
		if (CPU_ISSET(cpu, &seen) || count == CPU_SETSIZE) {
			return -1;
		}
		CPU_SET(cpu, &seen);
		cpus[count++] = (int)cpu;
		if (*p == '\0') {
			break;
		}
		if (*p++ != ',') {
			return -1;
		}
	}
	memcpy(args->cpus, cpus, count * sizeof(cpus[0]));
	args->num_cpus = count;
	args->explicit_cpus = true;
	return 0;
}

static int
reactor_cpu(const struct target_args *args, uint32_t index)
{
	if (args->num_cpus != 0) {
		return args->cpus[index];
	}
	return args->cpu_base >= 0 ? args->cpu_base + (int)index : -1;
}

static int
generate_lcore_map(const struct target_args *args, char *map, size_t size)
{
	size_t used = 0;

	for (uint32_t i = 0; i < args->reactors; i++) {
		int cpu = reactor_cpu(args, i);
		int n;

		/* With legacy pinning disabled, let DPDK use the logical CPU ID. */
		if (cpu < 0) {
			cpu = (int)i;
		}
		n = snprintf(map + used, size - used, "%s%u@%d", i ? "," : "", i, cpu);
		if (n < 0 || (size_t)n >= size - used) {
			return -1;
		}
		used += (size_t)n;
	}
	return 0;
}

static int
parse_args(int argc, char **argv)
{
	static const struct option opts[] = {
		{"trid", required_argument, NULL, 'r'},
		{"socket", required_argument, NULL, 's'},
		{"lcore-map", required_argument, NULL, 'l'},
		{"reactors", required_argument, NULL, 'R'},
		{"cpu-base", required_argument, NULL, 'c'},
		{"cpus", required_argument, NULL, 'C'},
		{"qpair-depth", required_argument, NULL, 'd'},
		{"mem-size", required_argument, NULL, 'm'},
		{"iova-mode", required_argument, NULL, 'i'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int c;

	while ((c = getopt_long(argc, argv, "r:s:l:R:c:C:d:m:i:h", opts, NULL)) != -1) {
		switch (c) {
		case 'r':
			g_args.trid = optarg;
			break;
		case 's':
			g_args.sock_path = optarg;
			break;
		case 'l':
			g_args.lcore_map = optarg;
			break;
		case 'R':
			{
				char *end;
				unsigned long n;

				errno = 0;
				n = strtoul(optarg, &end, 10);
				if (errno || *optarg < '0' || *optarg > '9' || *end ||
				    n == 0 || n > CPU_SETSIZE) {
					fprintf(stderr, "invalid --reactors: %s\n", optarg);
					return -1;
				}
				g_args.reactors = (uint32_t)n;
			}
			break;
		case 'c':
			{
				char *end;
				long cpu;

				errno = 0;
				cpu = strtol(optarg, &end, 0);
				if (errno || end == optarg || *end || cpu < -1 || cpu >= CPU_SETSIZE) {
					fprintf(stderr, "invalid --cpu-base: %s\n", optarg);
					return -1;
				}
				g_args.cpu_base = (int)cpu;
				if (!g_args.explicit_cpus) {
					g_args.num_cpus = 0;
				}
			}
			break;
		case 'C':
			if (parse_cpu_list(optarg, &g_args) != 0) {
				fprintf(stderr, "invalid --cpus: expected unique CPU IDs in [0,%d)\n",
					CPU_SETSIZE);
				return -1;
			}
			break;
		case 'd':
			g_args.qpair_depth = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'm':
			g_args.mem_mb = (int)strtol(optarg, NULL, 0);
			break;
		case 'i':
			g_args.iova_mode = optarg;
			break;
		case 'h':
			printf("Usage: %s [options]\n"
			       "  -r, --trid TRID        default: %s\n"
			       "  -s, --socket PATH      default: %s\n"
			       "  -l, --lcore-map MAP    default: generated one-to-one from CPUs\n"
			       "  -R, --reactors N       default: %u\n"
			       "  -C, --cpus LIST        comma-separated CPU IDs, one per reactor;\n"
			       "                         default: 4-7,36-39,...,228-231, NUMA-interleaved\n"
			       "  -c, --cpu-base CPU     legacy contiguous CPUs (base default: %d);\n"
			       "                         clears default CPUs; -1 disables pinning; --cpus wins\n"
			       "  -d, --qpair-depth N    default: %u\n"
			       "  -m, --mem-size MB      default: %d\n"
			       "  -i, --iova-mode va|pa  default: library default\n",
			       argv[0], DEFAULT_TRID, DEFAULT_SOCK,
			       DEFAULT_REACTORS, DEFAULT_CPU_BASE, DEFAULT_QPAIR_DEPTH,
			       DEFAULT_MEM_MB);
			return 1;
		default:
			return -1;
		}
	}
	if (g_args.reactors == 0 || g_args.qpair_depth == 0 ||
	    g_args.qpair_depth > MAX_QPAIR_DEPTH) {
		fprintf(stderr, "invalid reactors/qpair-depth\n");
		return -1;
	}
	if ((g_args.num_cpus != 0 && g_args.num_cpus != g_args.reactors) ||
	    (g_args.num_cpus == 0 && g_args.cpu_base >= 0 &&
	     g_args.reactors > (uint32_t)(CPU_SETSIZE - g_args.cpu_base))) {
		fprintf(stderr, "CPU count must equal --reactors and CPUs must be below CPU_SETSIZE\n");
		return -1;
	}
	return 0;
}

static int
setup_spdk(void)
{
	struct spdk_env_opts env_opts;
	struct spdk_nvme_transport_id trid;
	static char generated_map[CPU_SETSIZE * 24];
	cpu_set_t caller_cpus;
	uint32_t i;
	uint32_t count;

	if (sched_getaffinity(0, sizeof(caller_cpus), &caller_cpus) != 0) {
		fprintf(stderr, "cannot read caller affinity: %s\n", strerror(errno));
		return -1;
	}
	for (i = 0; i < g_args.reactors; i++) {
		int cpu = reactor_cpu(&g_args, i);

		if (cpu >= 0 && (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &caller_cpus))) {
			fprintf(stderr, "reactor %u: CPU %d is outside allowed affinity\n", i, cpu);
			return -1;
		}
	}
	if (g_args.lcore_map == NULL) {
		if (generate_lcore_map(&g_args, generated_map, sizeof(generated_map)) != 0) {
			fprintf(stderr, "cannot generate lcore-map\n");
			return -1;
		}
		g_args.lcore_map = generated_map;
	}

	spdk_env_opts_init(&env_opts);
	env_opts.name = "ucm_spdk_ipc_target";
	/* spdk_env_opts_init() installs a default core_mask; exactly one of
	 * core_mask/lcore_map may be provided. */
	env_opts.core_mask = NULL;
	env_opts.lcore_map = g_args.lcore_map;
	env_opts.mem_size = g_args.mem_mb;
	if (g_args.iova_mode != NULL && g_args.iova_mode[0] != '\0') {
		env_opts.iova_mode = g_args.iova_mode;
	}
	env_opts.opts_size = sizeof(env_opts);
	{
		if (spdk_env_init(&env_opts) < 0) {
			fprintf(stderr, "spdk_env_init failed\n");
			return -1;
		}
		/* DPDK may pin the main thread to its main lcore. */
		if (sched_setaffinity(0, sizeof(caller_cpus), &caller_cpus) != 0) {
			fprintf(stderr, "warning: failed to restore caller affinity\n");
		}
	}
	g_env_initialized = true;

	memset(&trid, 0, sizeof(trid));
	if (spdk_nvme_transport_id_parse(&trid, g_args.trid) != 0) {
		fprintf(stderr, "invalid trid: %s\n", g_args.trid);
		return -1;
	}
	g_ctrlr = spdk_nvme_connect(&trid, NULL, 0);
	if (g_ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_connect failed: %s\n", g_args.trid);
		return -1;
	}

	count = spdk_nvme_ctrlr_get_num_ns(g_ctrlr);
	for (i = 1; i <= count; i++) {
		g_ns = spdk_nvme_ctrlr_get_ns(g_ctrlr, i);
		if (g_ns != NULL && spdk_nvme_ns_get_sector_size(g_ns) != 0) {
			break;
		}
	}
	if (g_ns == NULL) {
		fprintf(stderr, "no usable namespace\n");
		return -1;
	}
	g_sector_size = spdk_nvme_ns_get_sector_size(g_ns);
	g_num_sectors = spdk_nvme_ns_get_num_sectors(g_ns);
	g_max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
	fprintf(stderr, "target: ns=%u sector=%u sectors=%" PRIu64 " max_xfer=%u\n",
		i, g_sector_size, g_num_sectors, g_max_xfer);

	count = spdk_env_get_core_count();
	if (count == 0) {
		fprintf(stderr, "no SPDK lcores\n");
		return -1;
	}
	if (g_args.reactors > count) {
		fprintf(stderr, "lcore-map has fewer cores than --reactors\n");
		return -1;
	}
	g_nreactors = g_args.reactors;
	if (posix_memalign((void **)&g_reactors, _Alignof(struct ipc_reactor),
			   (size_t)g_nreactors * sizeof(*g_reactors)) != 0) {
		return -1;
	}
	memset(g_reactors, 0, (size_t)g_nreactors * sizeof(*g_reactors));
	for (i = 0; i < g_nreactors; i++) {
		reactor_stats_init(&g_reactors[i].stats);
		g_reactors[i].index = i;
		g_reactors[i].admin = (i == 0);
		pthread_mutex_init(&g_reactors[i].mu, NULL);
	}
	{
		uint32_t core = spdk_env_get_first_core();
		for (i = 0; i < g_nreactors && core != UINT32_MAX; i++) {
			g_reactors[i].lcore_id = core;
			g_reactors[i].cpu_id = reactor_cpu(&g_args, i);
			core = spdk_env_get_next_core(core);
		}
		if (i != g_nreactors) {
			fprintf(stderr, "lcore-map has fewer cores than --reactors\n");
			return -1;
		}
	}
	return 0;
}

static void
teardown(void)
{
	uint32_t i;

	if (g_reactors != NULL) {
		for (i = 0; i < g_nreactors; i++) {
			pthread_join(g_reactors[i].thread, NULL);
		}
		for (i = 0; i < g_nreactors; i++) {
			struct ipc_session *s;
			while ((s = g_reactors[i].active) != NULL) {
				reactor_detach_session(&g_reactors[i], s);
			}
			pthread_mutex_destroy(&g_reactors[i].mu);
		}
		free(g_reactors);
		g_reactors = NULL;
	}
	if (g_ctrlr != NULL) {
		spdk_nvme_detach(g_ctrlr);
		g_ctrlr = NULL;
	}
	if (g_env_initialized) {
		spdk_env_fini();
		g_env_initialized = false;
	}
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	int rc;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc > 0 ? 0 : 2;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (setup_spdk() != 0) {
		teardown();
		return 1;
	}

	g_listen_fd = setup_listener(g_args.sock_path);
	if (g_listen_fd < 0) {
		fprintf(stderr, "listen %s failed: %s\n", g_args.sock_path, strerror(-g_listen_fd));
		teardown();
		return 1;
	}
	fprintf(stderr, "target ready: %s (lcore_map=%s, reactors=%u, cpu_base=%d)\n",
		g_args.sock_path, g_args.lcore_map, g_nreactors, g_args.cpu_base);

	for (uint32_t i = 0; i < g_nreactors; i++) {
		if (pthread_create(&g_reactors[i].thread, NULL, reactor_thread,
				   &g_reactors[i]) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			g_stop = 1;
			break;
		}
		g_reactors_started++;
	}

	while (!g_stop) {
		int fd = accept(g_listen_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == EBADF || errno == EINVAL) {
				break;
			}
			continue;
		}
		handle_connection(fd);
	}

	teardown();
	unlink(g_args.sock_path);
	return 0;
}
