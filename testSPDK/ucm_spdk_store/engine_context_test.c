/* Hardware-free engine regression test. Build/link this translation unit ONLY,
 * not ucm_spdk_store.c separately. Use the normal SPDK nvme/env static libraries
 * (same link flags as the standalone engine). No --wrap flags are required.
 * Build from this directory:
 *   make APP=engine_context_test SPDK_ROOT_DIR=/path/to/spdk
 * Run with a timeout: timeout 60s ./engine_context_test.
 *
 * Real SPDK environment (--no-pci --no-huge), DMA allocator, rings and ticks;
 * real engine startup, format/recovery, SHM/flocks, reactors and GC timers.
 * Also tests same-region deferred reclaim after B-first fini/destroy, with
 * A's partial-overwrite pin held locally and in an exec'd remote process.
 * Only controller/namespace/IO and external host registration are mocked.
 * No device is enumerated, connected, read, written or formatted.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define ENGINE_LIB 1
#define spdk_nvme_connect test_connect
#define spdk_nvme_ctrlr_get_data test_ctrlr_data
#define spdk_nvme_ctrlr_get_first_active_ns test_first_ns
#define spdk_nvme_ctrlr_get_next_active_ns test_next_ns
#define spdk_nvme_ctrlr_get_ns test_get_ns
#define spdk_nvme_ns_is_active test_ns_active
#define spdk_nvme_ns_get_sector_size test_sector_size
#define spdk_nvme_ns_get_num_sectors test_num_sectors
#define spdk_nvme_ns_get_max_io_xfer_size test_max_xfer
#define spdk_nvme_ctrlr_get_default_io_qpair_opts test_qpair_opts
#define spdk_nvme_ctrlr_alloc_io_qpair test_qpair_alloc
#define spdk_nvme_ctrlr_free_io_qpair test_qpair_free
#define spdk_nvme_qpair_process_completions test_completions
#define spdk_nvme_ctrlr_process_admin_completions test_admin
#define spdk_nvme_detach_async test_detach
#define spdk_nvme_ns_cmd_read test_read
#define spdk_nvme_ns_cmd_write test_write
#define spdk_mem_reserve test_reserve
#define spdk_mem_register test_register
#define spdk_mem_unregister test_unregister
#define spdk_zmalloc test_zmalloc
#define spdk_free test_free
#include "ucm_spdk_store.c"
#undef spdk_zmalloc
#undef spdk_free
void *spdk_zmalloc(size_t size, size_t align, uint64_t *unused, int numa, uint32_t flags);
void spdk_free(void *p);
#include <assert.h>
#include <stdatomic.h>
#include <sys/resource.h>

enum { TEST_SLOTS = 8, TEST_SECTOR = 512, TEST_MAX_SECTOR = 4096,
	TEST_SHARD_KB = 4, DMA_GUARD = 64 };
struct test_disk {
	uint8_t sb[TEST_MAX_SECTOR];
	uint8_t records[TEST_SLOTS * TEST_MAX_SECTOR];
	uint8_t data[TEST_SLOTS * 5 * TEST_SHARD_KB * 1024];
};
struct test_completion { spdk_nvme_cmd_cb cb; void *arg; };
struct test_qpair {
	struct spdk_store_engine *owner;
	struct test_completion pending[MAX_QD];
	unsigned in, out;
};
static struct test_disk *disks;
static struct spdk_nvme_ctrlr_data ctrlr_data;
static unsigned connects, detaches, registrations, unregistrations;
static _Atomic unsigned polls[2];
static char transport[512], session[64];
static char cleanup_names[2][3][80];
static char disk_name[80], self_path[4096], identity[32];
static int dma_fail_after = -1, metadata_only;
static size_t dma_bytes, dma_allocations, dma_attempts;
static uint32_t mock_sector = TEST_SECTOR;
static int hold_completions, fail_completion, fail_submission;
static const uint8_t *expected_payload;
static unsigned payload_ios;
/* Two fully populated engines, including their separate metadata buffers. */
static struct { void *ptr; size_t size; } dma_live[2 * (IO_POOL_BUFS + NUM_IDX_BUFS + 1)];

void *test_zmalloc(size_t size, size_t align, uint64_t *unused, int numa, uint32_t flags)
{
	dma_attempts++;
	if (dma_fail_after == 0) { return NULL; }
	if (dma_fail_after > 0) { dma_fail_after--; }
	void *p = spdk_zmalloc(size + DMA_GUARD, align, unused, numa, flags);
	if (p == NULL) { return NULL; }
	memset((uint8_t *)p + size, 0xcd, DMA_GUARD);
	for (unsigned i = 0; i < SPDK_COUNTOF(dma_live); i++) {
		if (dma_live[i].ptr == NULL) {
			dma_live[i].ptr = p;
			dma_live[i].size = size;
			dma_bytes += size;
			dma_allocations++;
			return p;
		}
	}
	assert(0);
	return NULL;
}

void test_free(void *p)
{
	for (unsigned i = 0; i < SPDK_COUNTOF(dma_live); i++) {
		if (dma_live[i].ptr == p && p != NULL) {
			for (unsigned j = 0; j < DMA_GUARD; j++) {
				assert(((uint8_t *)p)[dma_live[i].size + j] == 0xcd);
			}
			dma_bytes -= dma_live[i].size;
			dma_allocations--;
			dma_live[i].ptr = NULL;
			spdk_free(p);
			return;
		}
	}
	assert(0);
}

struct spdk_nvme_ctrlr *test_connect(const struct spdk_nvme_transport_id *t,
		const struct spdk_nvme_ctrlr_opts *o, size_t n)
{
	assert(process_env_ready && t->trtype == SPDK_NVME_TRANSPORT_RDMA);
	connects++;
	return (void *)&ctrlr_data;
}
const struct spdk_nvme_ctrlr_data *test_ctrlr_data(struct spdk_nvme_ctrlr *c)
{ return &ctrlr_data; }
uint32_t test_first_ns(struct spdk_nvme_ctrlr *c) { return 1; }
uint32_t test_next_ns(struct spdk_nvme_ctrlr *c, uint32_t n) { return 0; }
struct spdk_nvme_ns *test_get_ns(struct spdk_nvme_ctrlr *c, uint32_t n)
{ return (void *)&disks; }
bool test_ns_active(struct spdk_nvme_ns *n) { return true; }
uint32_t test_sector_size(struct spdk_nvme_ns *n) { return mock_sector; }
uint64_t test_num_sectors(struct spdk_nvme_ns *n) { return 16ULL << 21; }
uint32_t test_max_xfer(struct spdk_nvme_ns *n) { return 128 * 1024; }
void test_qpair_opts(struct spdk_nvme_ctrlr *c, struct spdk_nvme_io_qpair_opts *o, size_t n)
{ memset(o, 0, n); }
struct spdk_nvme_qpair *test_qpair_alloc(struct spdk_nvme_ctrlr *c,
		const struct spdk_nvme_io_qpair_opts *o, size_t n)
{
	struct test_qpair *q = calloc(1, sizeof(*q));
	assert(q);
	uint64_t split = (shard_bytes() + test_max_xfer(NULL) - 1) / test_max_xfer(NULL);
	uint64_t need = g_io_pool_count * split + NUM_IDX_BUFS + 16;
	assert(o->io_queue_size == (need > 1024 ? 1024 : need));
	q->owner = ENGINE;
	return (void *)q;
}
int test_qpair_free(struct spdk_nvme_qpair *p)
{
	struct test_qpair *q = (void *)p;
	assert(q->owner == ENGINE && q->in == q->out);
	free(q);
	return 0;
}
int32_t test_completions(struct spdk_nvme_qpair *p, uint32_t max)
{
	struct test_qpair *q = (void *)p;
	assert(q->owner == ENGINE);
	if (hold_completions) { return 0; }
	struct spdk_nvme_cpl cpl = {0};
	unsigned count = 0;
	while (q->in != q->out && (!max || count < max)) {
		struct test_completion c = q->pending[q->out++ % MAX_QD];
		cpl.status.sc = fail_completion ? SPDK_NVME_SC_INTERNAL_DEVICE_ERROR : 0;
		fail_completion = 0;
		c.cb(c.arg, &cpl);
		count++;
	}
	return count;
}
int32_t test_admin(struct spdk_nvme_ctrlr *c)
{
	assert(c == ENGINE->ctrlr);
	atomic_fetch_add(&polls[g_offset_gib != 0], 1);
	return 0;
}
int test_detach(struct spdk_nvme_ctrlr *c, struct spdk_nvme_detach_ctx **ctx)
{ detaches++; *ctx = NULL; return 0; }

static int test_io(struct spdk_nvme_qpair *p, void *buf, uint64_t lba,
		uint32_t count, spdk_nvme_cmd_cb cb, void *arg, int write)
{
	struct test_qpair *q = (void *)p;
	assert(q->owner == ENGINE && q->in - q->out < MAX_QD);
	struct test_disk *d = &disks[g_offset_gib != 0];
	uint8_t *mem;
	size_t bytes = (size_t)count * mock_sector;
	for (unsigned i = 0; i < SPDK_COUNTOF(dma_live); i++) {
		uintptr_t start = (uintptr_t)dma_live[i].ptr, address = (uintptr_t)buf;
		if (start && address >= start && address - start < dma_live[i].size) {
			assert(bytes <= dma_live[i].size - (address - start));
		}
	}
	if (lba == g_data_base) {
		assert(count == 1);
		mem = d->sb;
	} else if (lba >= g_index_base && lba < g_index_base + TEST_SLOTS) {
		assert(lba + count <= g_index_base + TEST_SLOTS);
		mem = d->records + (lba - g_index_base) * mock_sector;
	} else {
		assert(!metadata_only);
		assert(lba >= g_pool_base &&
		       (lba + count - g_pool_base) * mock_sector <= sizeof(d->data));
		mem = d->data + (lba - g_pool_base) * mock_sector;
		struct shard_io *io = arg;
		assert(count == g_shard_sectors);
		if (expected_payload) {
			assert(io->bounce == -1);
			assert(buf == expected_payload + (size_t)io->offset * shard_bytes());
		} else {
			assert(io->bounce >= 0 && (uint32_t)io->bounce < g_bounce_pool_count);
			assert(buf == g_io_bufs[io->bounce]);
		}
		payload_ios++;
		if (fail_submission) { fail_submission = 0; return -EIO; }
	}
	if (write) { memcpy(mem, buf, bytes); } else { memcpy(buf, mem, bytes); }
	q->pending[q->in++ % MAX_QD] = (struct test_completion){cb, arg};
	return 0;
}
int test_read(struct spdk_nvme_ns *n, struct spdk_nvme_qpair *p, void *buf,
		uint64_t lba, uint32_t count, spdk_nvme_cmd_cb cb, void *arg, uint32_t flags)
{ return test_io(p, buf, lba, count, cb, arg, 0); }
int test_write(struct spdk_nvme_ns *n, struct spdk_nvme_qpair *p, void *buf,
		uint64_t lba, uint32_t count, spdk_nvme_cmd_cb cb, void *arg, uint32_t flags)
{ return test_io(p, buf, lba, count, cb, arg, 1); }
int test_reserve(void *p, size_t n) { return 0; }
int test_register(void *p, size_t n) { registrations++; return 0; }
int test_unregister(void *p, size_t n) { unregistrations++; return 0; }

static void select_region(struct spdk_store_engine *e, unsigned region)
{
	store_engine_select(e);
	assert(store_parse_region(transport, region * 4) == 0);
}

static void cleanup(void)
{
	for (unsigned r = 0; r < 2; r++) {
		for (unsigned n = 0; n < 3; n++) { shm_unlink(cleanup_names[r][n]); }
		char gc[96];
		snprintf(gc, sizeof(gc), "%.79s_gc", cleanup_names[r][0]);
		shm_unlink(gc);
	}
	shm_unlink(disk_name);
}

/* Run before DPDK creates threads. Child opens independent file descriptions;
 * inherited flock descriptions are never explicitly unlocked in the child. */
static void test_region_leases(void)
{
	struct spdk_store_engine *fa = store_engine_create(), *wa = store_engine_create();
	assert(fa && wa);
	for (unsigned r = 0; r < 2; r++) {
		select_region(r ? wa : fa, r);
		snprintf(cleanup_names[r][0], 80, "%s", g_shm_name);
		snprintf(cleanup_names[r][1], 80, "%s", g_lease_name);
		snprintf(cleanup_names[r][2], 80, "%s", ENGINE->maintenance_name);
	}
	atexit(cleanup);
	select_region(fa, 0);
	assert(store_maintenance_open(1) == 0);
	assert(store_lease_open() == 0);
	assert(store_session_publish(g_lease_fd, session) == 0);
	assert(flock(g_maintenance_fd, LOCK_SH) == 0);
	assert(flock(g_lease_fd, LOCK_SH) == 0);
	store_startup_close(&g_startup_fd);
	/* Fresh WA format upgrade must succeed while FA's shared leases live. */
	select_region(wa, 1);
	assert(store_maintenance_open(0) == 0);
	assert(store_maintenance_upgrade() == 0);
	assert(store_lease_open() == 0);
	assert(store_session_publish(g_lease_fd, session) == 0);
	assert(flock(g_maintenance_fd, LOCK_SH) == 0);
	assert(flock(g_lease_fd, LOCK_SH) == 0);
	store_startup_close(&g_startup_fd);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		alarm(10);
		struct spdk_store_engine *join = store_engine_create();
		assert(join);
		for (unsigned r = 0; r < 2; r++) {
			select_region(join, r);
			assert(store_maintenance_open(0) == 0);
			assert(store_lease_open() == 0 && !g_shm_recovery_owner);
			assert(store_session_matches(g_lease_fd, session) == 1);
			assert(store_maintenance_upgrade() == -EBUSY);
			store_lease_close();
			store_maintenance_close();
		}
		_exit(0);
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	store_engine_destroy(wa);
	store_engine_destroy(fa);
}

static void callback(void *arg, int status)
{
	assert(status == 0 && ENGINE == arg);
	struct spdk_store_engine *saved = store_engine_select(NULL);
	assert(ENGINE == &default_engine);
	assert(store_engine_select(saved) == NULL);
}

struct registration_job { struct spdk_store_engine *e; void *mem; };
static void *register_thread(void *arg)
{
	struct registration_job *job = arg;
	assert(current_engine == NULL && ENGINE == &default_engine);
	store_engine_select(job->e);
	assert(store_register_memory(job->mem, 8192) == 0);
	return NULL;
}

static void test_engines(void)
{
	struct spdk_store_engine *fa = store_engine_create(), *wa = store_engine_create();
	assert(fa && wa);
	assert(store_engine_select(fa) == NULL);
	assert(store_engine_init_for_session(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1, session) == 0);
	assert(fa->io_pool_count == IO_POOL_BUFS);
	/* exec gives the worker its own SPDK environment and file descriptions.
	 * It joins live FA and freshly formats WA while FA stays live here. */
	pid_t worker = fork();
	assert(worker >= 0);
	if (worker == 0) {
		execl(self_path, self_path, "--worker", identity, (char *)NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(store_engine_select(wa) == fa);
	/* Exercise fresh format on this host thread too, not just the child's
	 * fresh format followed by the parent's same-session join. */
	snprintf(session, sizeof(session), "parent-wa-%s", identity);
	assert(store_engine_init_for_session(transport, TEST_SLOTS, 5, TEST_SHARD_KB, 4, 1, session) == 0);
	assert(connects == 1 && fa->qpair != wa->qpair && fa->controller == wa->controller);
	assert(strcmp(fa->shm_name, wa->shm_name) != 0);
	assert(store_slot_sectors() == 40);
	struct spdk_store_engine *conflict = store_engine_create();
	store_engine_select(conflict);
	assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1) == -EWOULDBLOCK);
	assert(store_engine_init_for_session(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1,
					     "different-session") == -EBUSY);
	store_engine_destroy(conflict);
	store_engine_select(wa);
	uint8_t id[16] = {42};
	uint8_t data[5 * TEST_SHARD_KB * 1024], out[sizeof(data)];
	memset(data, 0x5a, sizeof(data));
	assert(dump_block_sync(id, data) == 0);
	store_engine_select(fa);
	assert(!store_lookup(id) && store_slot_sectors() == 16);
	memset(data, 0xa5, sizeof(data));
	assert(dump_block_sync(id, data) == 0);
	assert(load_block_sync(id, out) == 0 && out[0] == 0xa5);
	store_engine_select(wa);
	assert(load_block_sync(id, out) == 0 && out[0] == 0x5a);
	uint8_t only_wa[16] = {43};
	assert(dump_block_sync(only_wa, data) == 0);
	/* Recovery really reads the memory-backed on-disk index, not a copy
	 * of the in-memory hash. Neither region may recover the other's ID. */
	for (unsigned r = 0; r < 2; r++) {
		store_engine_select(r ? wa : fa);
		assert(store_quiesce() == 0);
		reactor_pause();
		memset(g_hash, 0, g_shm->hash_size * sizeof(*g_hash));
		assert(store_recover() == 0 && store_lookup(id));
		assert(store_lookup(only_wa) == (int)r);
		/* Recovery deliberately clears registrations under exclusive ownership. */
		assert(shm_register_worker() == 0);
		reactor_resume();
	}
	/* Same-region scheduler attachment blocks maintenance, not another region. */
	struct spdk_store_engine *lookup = store_engine_create();
	store_engine_select(lookup);
	assert(store_lookup_init_region(transport, 0) == 0 && store_lookup(id));
	assert(lookup->worker_slot != fa->worker_slot);
	store_engine_select(fa);
	assert(store_maintenance_upgrade() == -EBUSY);
	store_engine_select(lookup);
	store_engine_destroy(lookup);
	assert(fa->shm->workers[fa->worker_slot] == getpid());
	store_engine_select(fa);
	assert(store_maintenance_upgrade() == 0);
	assert(flock(g_maintenance_fd, LOCK_SH) == 0);
	/* Overlap must fail before any formatting write. */
	reactor_pause();
	uint64_t base = g_data_base, pool = g_pool_base;
	g_data_base = wa->data_base;
	g_pool_base = wa->pool_base;
	assert(store_region_available(TEST_SLOTS) == -EBUSY);
	g_data_base = base;
	g_pool_base = pool;
	reactor_resume();
	struct syncw w;
	syncw_init(&w);
	assert(op_submit(op_new(OP_BARRIER, NULL, callback, fa, &w)) == 0);
	assert(syncw_wait(&w) == 0);
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	reactor_pause();
	g_gc_cycle_errors = UINT64_MAX;
	reactor_resume();
	assert(store_gc_start(0.9, 0.1, 1) == 0);
	store_engine_select(wa);
	reactor_pause();
	g_gc_cycle_errors = UINT64_MAX;
	reactor_resume();
	assert(store_gc_start(0.8, 0.2, 1) == 0);
	/* Fence each timer's initial request through its own reactor. */
	for (unsigned r = 0; r < 2; r++) {
		store_engine_select(r ? wa : fa);
		for (;;) {
			reactor_pause();
			int checked = g_gc_cycle_errors != UINT64_MAX && !g_gc_owner;
			reactor_resume();
			if (checked) { break; }
			sched_yield();
		}
	}
	store_engine_select(wa);
	/* Concurrent registrations exercise process_mu + pause-all and TLS
	 * defaults on newly created host threads. Rings/reactors are real. */
	void *mem;
	assert(posix_memalign(&mem, 4096, 16384) == 0);
	struct registration_job jobs[] = {{fa, mem}, {wa, mem}};
	pthread_t threads[2];
	for (unsigned i = 0; i < 2; i++) { assert(pthread_create(&threads[i], NULL, register_thread, &jobs[i]) == 0); }
	for (unsigned i = 0; i < 2; i++) { assert(pthread_join(threads[i], NULL) == 0); }
	assert(registrations == 1);
	assert(store_register_memory((char *)mem + 4096, 8192) == -EINVAL);
	assert(store_register_memory((char *)mem + 4096, 4096) == 0);
	assert(store_unregister_memory((char *)mem + 4096, 4096) == 0);
	assert(unregistrations == 0);
	store_engine_destroy(fa);
	assert(detaches == 0 && unregistrations == 0);
	assert(store_registered_bytes() == 8192 && store_lookup(id));
	assert(store_quiesce() == 0);
	assert(atomic_load(&polls[0]) && atomic_load(&polls[1]));
	store_engine_destroy(wa);
	assert(current_engine == NULL && detaches == 1 && unregistrations == 1);
	assert(!default_engine.gc_requested && !default_engine.gc_started);
	free(mem);
}

static void test_default_context(void)
{
	assert(current_engine == NULL && ENGINE == &default_engine);
	assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 0) == 0);
	struct spdk_store_engine *wa = store_engine_create();
	store_engine_select(wa);
	assert(store_engine_init(transport, TEST_SLOTS, 5, TEST_SHARD_KB, 4, 0) == 0);
	void *mem;
	assert(posix_memalign(&mem, 4096, 8192) == 0);
	struct registration_job jobs[] = {{NULL, mem}, {wa, mem}};
	pthread_t threads[2];
	for (unsigned i = 0; i < 2; i++) { assert(pthread_create(&threads[i], NULL, register_thread, &jobs[i]) == 0); }
	for (unsigned i = 0; i < 2; i++) { assert(pthread_join(threads[i], NULL) == 0); }
	assert(registrations == 2);
	store_engine_select(NULL);
	assert(store_unregister_memory(mem, 8192) == 0 && unregistrations == 1);
	store_engine_fini();
	store_engine_select(wa);
	assert(store_quiesce() == 0 && store_registered_bytes() == 8192);
	store_engine_destroy(wa);
	assert(detaches == 2 && unregistrations == 2);
	free(mem);
}

static void check_slot_accounting(uint32_t retired_slot, int pinned)
{
	shm_lock();
	uint8_t seen[TEST_SLOTS] = {0};
	for (uint32_t i = 0; i < g_free_top; i++) {
		assert(g_freelist[i] < TEST_SLOTS && !seen[g_freelist[i]]++);
	}
	assert(g_retired[retired_slot] == pinned);
	assert(seen[retired_slot] == !pinned);
	if (!pinned) { assert(g_free_top + g_live_count == TEST_SLOTS); }
	shm_unlock();
}

static uint32_t pin_partial_overwrite(const uint8_t id[16], const void *data)
{
	uint32_t old_slot;
	shm_lock();
	old_slot = hash_find(id)->slot;
	shm_unlock();
	struct syncw w;
	syncw_init(&w);
	assert(store_dump_submit(id, 0, 1, data, sync_cb, &w) == 0);
	assert(syncw_wait(&w) == 0);
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	assert(store_quiesce() == 0);
	return old_slot;
}

static void test_retirement(void)
{
	uint8_t id[16] = {77}, data[2 * TEST_SHARD_KB * 1024];
	memset(data, 0x77, sizeof(data));
	/* A holds an incomplete overwrite, B durably replaces the same old slot.
	 * Close B FIRST with GC enabled. Neither shutdown nor GC_STOP can await
	 * A's pin, and A must reclaim the orphaned retirement on cancellation. */
	for (int fini = 0; fini < 2; fini++) {
		struct spdk_store_engine *a = store_engine_create(), *b = store_engine_create();
		store_engine_select(a);
		assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1) == 0);
		assert(dump_block_sync(id, data) == 0);
		uint32_t old_slot = pin_partial_overwrite(id, data);
		store_engine_select(b);
		assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 0) == 0);
		assert(store_gc_start(0.99, 0.1, 1) == 0);
		assert(dump_block_sync(id, data) == 0);
		assert(store_quiesce() == 0);
		check_slot_accounting(old_slot, 1);
		reactor_pause();
		assert(gc_invalidations_drained());
		reactor_resume();
		if (fini) { store_engine_fini(); }
		store_engine_destroy(b);
		store_engine_select(a);
		check_slot_accounting(old_slot, 1);
		if (!fini) {
			assert(store_sync_op(OP_CANCEL_PARTIAL) == 0);
			check_slot_accounting(old_slot, 0);
		}
		/* Fini A and inspect via a scheduler mapping, proving the slot is
		 * not only temporarily correct before another cleanup resets state. */
		struct spdk_store_engine *lookup = store_engine_create();
		store_engine_select(lookup);
		assert(store_lookup_init_region(transport, 0) == 0);
		store_engine_select(a);
		store_engine_fini();
		store_engine_destroy(a);
		store_engine_select(lookup);
		check_slot_accounting(old_slot, 0);
		store_engine_destroy(lookup);
	}
	/* Same dependency across processes. The remote pin holder stays alive
	 * until the invalidating process has finished teardown, then reclaims. */
	struct spdk_store_engine *b = store_engine_create();
	store_engine_select(b);
	assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1) == 0);
	assert(dump_block_sync(id, data) == 0);
	int ready[2], release[2];
	assert(pipe(ready) == 0 && pipe(release) == 0);
	char out_fd[24], in_fd[24];
	snprintf(out_fd, sizeof(out_fd), "%d", ready[1]);
	snprintf(in_fd, sizeof(in_fd), "%d", release[0]);
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		execl(self_path, self_path, "--retire-worker", identity, out_fd, in_fd, (char *)NULL);
		_exit(127);
	}
	close(ready[1]);
	close(release[0]);
	char msg;
	assert(read(ready[0], &msg, 1) == 1);
	assert(store_gc_start(0.99, 0.1, 1) == 0);
	assert(dump_block_sync(id, data) == 0);
	assert(store_quiesce() == 0);
	store_engine_destroy(b);
	assert(write(release[1], "x", 1) == 1);
	int status;
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(ready[0]);
	close(release[1]);
}

struct io_result { unsigned calls; int status; };

static void io_callback(void *arg, int status)
{
	struct io_result *result = arg;
	assert(result->calls++ == 0);
	result->status = status;
}

static void test_payload_io(void)
{
	for (unsigned sector = TEST_SECTOR; sector <= TEST_MAX_SECTOR; sector *= 8) {
	 mock_sector = sector;
	 for (int direct = 0; direct <= 1; direct++) {
		struct spdk_store_engine *e = store_engine_create();
		assert(e);
		store_engine_select(e);
		assert(store_engine_set_direct_only(direct) == 0);
		assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1) == 0);
		assert(g_recov_batch * g_sector_size > shard_bytes());
		uint8_t *data, *out;
		size_t bytes = slot_bytes();
		assert(posix_memalign((void **)&data, 4096, bytes) == 0);
		assert(posix_memalign((void **)&out, 4096, bytes) == 0);
		for (size_t i = 0; i < bytes; i++) { data[i] = (uint8_t)(i * 17 + i / 4096); }
		if (direct) {
			assert(store_register_memory(data, bytes) == 0);
			assert(store_register_memory(out, bytes) == 0);
		}
		assert(store_quiesce() == 0);
		reactor_pause();
		assert(ensure_registered(data, bytes) == (direct ? 0 : 1));
		uint8_t invalid[4096];
		if (direct) { assert(ensure_registered(invalid, sizeof(invalid)) == -EFAULT); }
		struct test_qpair *q = (void *)g_qpair;
		size_t attempts = dma_attempts, allocations = dma_allocations, allocated = dma_bytes;
		dma_fail_after = 0;
		uint8_t id[16] = {91};
		/* Successful dump/load and failed submission/payload completion
		 * must all return every resource. */
		for (unsigned scenario = 0; scenario < 6; scenario++) {
			int load = scenario == 1 || scenario == 4 || scenario == 5;
			struct io_result result = {0};
			memset(out, 0, bytes);
			expected_payload = direct ? (load ? out : data) : NULL;
			payload_ios = 0;
			unsigned held = 0;
			int bounces[IO_POOL_BUFS];
			/* Leave one bounce available: token availability alone must not
			 * permit the second shard to overwrite an in-flight payload. */
			if (!direct) {
				while (g_bounce_free_top > 1) { bounces[held++] = bounce_buf_pop(); }
			}
			hold_completions = 1;
			fail_submission = scenario == 2 || scenario == 4;
			assert((load ? store_load_submit(id, 0, 2, out, bytes, io_callback, &result) :
				store_dump_submit(id, 0, 2, data, io_callback, &result)) == 0);
			drain_op_ring();
			store_poll();
			if (scenario == 2 || scenario == 4) {
				assert(result.calls == 1 && result.status == -EIO && q->in == q->out);
			} else {
				assert(!result.calls && q->in - q->out == (direct ? 2 : 1));
				assert(g_io_free_top == 256 - (direct ? 2 : 1));
				if (!direct) {
					assert(!g_bounce_free_top);
					unsigned submitted = payload_ios;
					store_poll();
					assert(payload_ios == submitted && !result.calls);
					if (load) {
						for (size_t i = 0; i < bytes; i++) { assert(out[i] == 0); }
					}
				} else if (load) {
					/* Mock DMA has already populated the caller, before the
					 * completion callback could perform any fallback copy. */
					assert(memcmp(data, out, bytes) == 0);
				}
				fail_completion = scenario == 3 || scenario == 5;
			}
			hold_completions = 0;
			for (unsigned step = 0; !result.calls && step < 16; step++) { store_poll(); }
			assert(result.calls == 1 && result.status == (scenario >= 2 ? -EIO : 0));
			if (scenario < 2) { assert(payload_ios == 2); }
			assert(q->in == q->out && g_io_free_top == 256 && g_idx_free_top == NUM_IDX_BUFS);
			if (scenario == 1) { assert(memcmp(data, out, bytes) == 0); }
			if (!direct) {
				assert(g_bounce_free_top == 1);
				while (held) { bounce_buf_push(bounces[--held]); }
			}
			assert(store_io_drained());
			shm_lock();
			assert(g_free_top + g_live_count == TEST_SLOTS);
			for (unsigned i = 0; i < TEST_SLOTS; i++) { assert(g_pins[i] == 0); }
			shm_unlock();
			assert(dma_attempts == attempts && dma_allocations == allocations && dma_bytes == allocated);
		}
		/* The index buffer remains owned until the delayed commit fails. */
		struct io_result commit = {0};
		uint8_t other[16] = {92};
		expected_payload = direct ? data : NULL;
		assert(store_dump_submit(other, 0, 2, data, io_callback, &commit) == 0);
		drain_op_ring();
		store_poll();
		store_poll();
		assert(!commit.calls && g_idx_free_top == NUM_IDX_BUFS - 1);
		fail_completion = 1;
		store_poll();
		assert(commit.calls == 1 && commit.status == -EIO && !store_lookup(other));
		assert(store_io_drained() && g_free_top + g_live_count == TEST_SLOTS);
		if (direct) {
			for (int load = 0; load <= 1; load++) {
				struct io_result rejected = {0};
				unsigned submitted = payload_ios;
				assert(ensure_registered(invalid, shard_bytes()) == -EFAULT);
				assert((load ? store_load_submit(id, 0, 1, invalid, sizeof(invalid), io_callback, &rejected) :
					store_dump_submit(other, 0, 1, invalid, io_callback, &rejected)) == 0);
				drain_op_ring();
				store_poll();
				/* The data path maps registration errors to callback EIO. */
				assert(rejected.calls == 1 && rejected.status == -EIO);
				assert(payload_ios == submitted && q->in == q->out && store_io_drained());
			}
		}
		assert(dma_attempts == attempts);
		dma_fail_after = -1;
		expected_payload = NULL;
		reactor_resume();
		store_engine_destroy(e);
		free(out);
		free(data);
		assert(!dma_allocations && !dma_bytes);
	 }
	}
	mock_sector = TEST_SECTOR;
}

static void test_bounded_pools(void)
{
	/* Real flattened FA/WA geometry. Only metadata reaches the small mock
	 * disk; full-size buffers still come from the real SPDK DMA allocator. */
	const uint32_t kb[] = {3112, 6344};
	const uint32_t counts[] = {21, 10};
	struct spdk_store_engine *e[] = {store_engine_create(), store_engine_create()};
	assert(e[0] && e[1] && dma_bytes == 0 && dma_allocations == 0);
	metadata_only = 1;
	for (unsigned sector = TEST_SECTOR; sector <= TEST_MAX_SECTOR; sector *= 8) {
	 mock_sector = sector;
	 for (int direct = 1; direct >= 0; direct--) {
	  for (int format = 1; format >= 0; format--) {
		for (unsigned r = 0; r < 2; r++) {
			store_engine_select(e[r]);
			assert(store_engine_set_direct_only(direct) == 0);
			assert(store_engine_init(transport, TEST_SLOTS, 1, kb[r], r * 4, format) == 0);
			assert(store_engine_set_direct_only(!direct) == -EBUSY);
			assert(g_io_pool_count == 256 && IO_POOL_BUFS == 256);
			assert(g_bounce_pool_count == (direct ? 0 : counts[r]));
			assert((uint64_t)g_bounce_pool_count * shard_bytes() <= IO_POOL_BYTES);
			assert(g_recov_batch == 64 && NUM_IDX_BUFS == 64 && g_meta_buf);
			assert((g_io_bufs == NULL) == direct);
			assert(store_quiesce() == 0);
			reactor_pause();
			assert(g_io_free_top == g_io_pool_count && store_io_drained());
			check_stall();
			assert(!ENGINE->had_inflight);
			uint8_t seen[IO_POOL_BUFS] = {0};
			for (uint32_t i = 0; i < g_io_pool_count; i++) {
				int token = io_buf_pop();
				assert(token >= 0 && (uint32_t)token < g_io_pool_count && !seen[token]++);
			}
			assert(io_buf_pop() == -1 && !store_io_drained());
			memset(seen, 0, sizeof(seen));
			for (uint32_t i = 0; i < g_bounce_pool_count; i++) {
				int bounce = bounce_buf_pop();
				assert(bounce >= 0 && (uint32_t)bounce < g_bounce_pool_count && !seen[bounce]++);
			}
			assert(bounce_buf_pop() == -1);
			for (uint32_t i = 0; i < 256; i++) { io_buf_push(i); }
			assert(store_io_drained() == direct);
			for (uint32_t i = 0; i < g_bounce_pool_count; i++) { bounce_buf_push(i); }
			assert(store_io_drained());
			/* Keep both reactors paused for cross-engine pool checks below. */
			assert(e[0]->io_free_top == 256);
		}
		for (unsigned r = 0; r < 2; r++) {
			store_engine_select(e[r]);
			for (unsigned i = 0; i < 256; i++) { assert(io_buf_pop() >= 0); }
			assert(io_buf_pop() == -1 && !store_io_drained());
			assert(e[1 - r]->io_free_top == (r ? 0 : 256));
		}
		assert(!e[0]->io_free_top && !e[1]->io_free_top);
		for (unsigned r = 0; r < 2; r++) {
			store_engine_select(e[r]);
			for (unsigned i = 0; i < 256; i++) { io_buf_push(i); }
			assert(store_io_drained());
		}
		assert(dma_bytes == (direct ? 0 : 21ULL * 3186688 + 10ULL * 6496256) +
				    2 * (64 + 64) * sector);
		if (direct && sector == 512) { assert(dma_bytes == 128 * 1024); }
		assert(dma_allocations == (direct ? 0 : 21 + 10) + 2 * (1 + 64));
		for (unsigned r = 0; r < 2; r++) {
			store_engine_select(e[r]);
			reactor_resume();
			store_engine_fini();
			assert(g_io_pool_count == 0 && g_io_free_top == 0 && !g_io_bufs && !g_idx_bufs && !g_meta_buf);
			assert(!g_bounce_pool_count && !g_bounce_free_top);
		}
		assert(dma_bytes == 0 && dma_allocations == 0);
	  }
	 }
	}
	store_engine_select(e[1]);
	/* Fail metadata, first/partial bounce, and first/partial/final index DMA. */
	for (int direct = 0; direct <= 1; direct++) {
		assert(store_engine_set_direct_only(direct) == 0);
		const int failures[] = {0, 1, 4, 10, 11, 14, direct ? 64 : 74};
		for (unsigned i = 0; i < SPDK_COUNTOF(failures); i++) {
			dma_fail_after = failures[i];
			assert(store_engine_init(transport, TEST_SLOTS, 1, kb[1], 4, 0) == -ENOMEM);
			assert(!g_io_bufs && !g_idx_bufs && !g_meta_buf && !g_qpair && !ENGINE->initialized);
			assert(!g_io_pool_count && !g_io_free_top && !g_bounce_pool_count && !g_bounce_free_top);
			assert(!g_idx_free_top && dma_bytes == 0 && dma_allocations == 0);
		}
		dma_fail_after = -1;
		assert(store_engine_init(transport, TEST_SLOTS, 1, kb[1], 4, 0) == 0);
		assert(g_io_pool_count == 256 && store_quiesce() == 0);
		store_engine_fini();
	}
	assert(store_engine_set_direct_only(0) == 0);
	size_t attempts = dma_attempts;
	assert(store_engine_init(transport, TEST_SLOTS, 1, 65540, 4, 1) == -E2BIG);
	assert(dma_attempts == attempts && !g_qpair && !g_meta_buf && !g_io_pool_count);
	assert(store_engine_set_direct_only(1) == 0);
	assert(store_engine_init(transport, TEST_SLOTS, 1, 65540, 4, 1) == 0);
	assert(g_io_pool_count == 256 && !g_bounce_pool_count && !g_io_bufs && shard_bytes() > IO_POOL_BYTES);
	assert(dma_bytes == 128 * mock_sector && dma_allocations == 65);
	assert(store_quiesce() == 0);
	store_engine_destroy(e[1]);
	store_engine_destroy(e[0]);
	assert(dma_bytes == 0 && dma_allocations == 0);
	metadata_only = 0;
	mock_sector = TEST_SECTOR;
}

static void test_load_admission_and_affinity(void)
{
	struct spdk_store_engine *e = store_engine_create();
	store_engine_select(e);
	int cpu = sched_getcpu(), invalid = CPU_SETSIZE;
	assert(store_engine_set_cpu_affinity(&invalid, 1) == -EINVAL);
	assert(store_engine_set_cpu_affinity(NULL, 1) == -EINVAL);
	assert(store_engine_set_cpu_affinity(&cpu, 1) == 0);
	assert(store_engine_init(transport, TEST_SLOTS, 1, TEST_SHARD_KB, 0, 1) == 0);
	cpu_set_t actual;
	assert(pthread_getaffinity_np(g_reactor_tid, sizeof(actual), &actual) == 0);
	assert(CPU_COUNT(&actual) == 1 && CPU_ISSET(cpu, &actual));
	assert(store_engine_set_cpu_affinity(&cpu, 1) == -EBUSY);
	assert(MAX_LOAD_CTX == 128);
	assert(store_quiesce() == 0);
	reactor_pause();
	struct store_op op = {.type = OP_LOAD};
	for (int i = 0; i < MAX_LOAD_CTX; i++) { g_lctx[i].active = 1; }
	/* This would deadlock if full-context admission tried the shared mutex. */
	pthread_mutex_lock(&g_shm->lock);
	assert(reactor_do_load(&op) == -EBUSY);
	pthread_mutex_unlock(&g_shm->lock);
	/* The last of all 128 slots is usable, not just the original first 16. */
	g_lctx[MAX_LOAD_CTX - 1].active = 0;
	op.dst = (void *)1;
	op.count = 1;
	op.len = TEST_SHARD_KB * 1024;
	shm_lock();
	hash_insert(op.id, 0, 1);
	shm_unlock();
	assert(reactor_do_load(&op) == 0);
	assert(g_lctx[MAX_LOAD_CTX - 1].active);
	slot_unpin(0);
	memset(g_lctx, 0, sizeof(g_lctx));
	reactor_resume();
	store_engine_fini();
	/* An unusable affinity must fail startup, not silently run unbound. */
	CPU_ZERO(&e->reactor_affinity);
	e->reactor_affinity_set = 1;
	assert(store_engine_init(transport, TEST_SLOTS, 1, TEST_SHARD_KB, 0, 0) == -EINVAL);
	assert(!g_reactor_started && !g_op_ring && !g_qpair);
	assert(store_engine_set_cpu_affinity(&cpu, 1) == 0);
	assert(store_engine_init(transport, TEST_SLOTS, 1, TEST_SHARD_KB, 0, 0) == 0);
	store_engine_destroy(e);
}

int main(int argc, char **argv)
{
	struct rlimit no_core = {0, 0};
	assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
	alarm(60);
	int retirement_worker = argc == 5 && strcmp(argv[1], "--retire-worker") == 0;
	int worker = retirement_worker || (argc == 3 && strcmp(argv[1], "--worker") == 0);
	snprintf(identity, sizeof(identity), "%s", worker ? argv[2] : "");
	if (!worker) { snprintf(identity, sizeof(identity), "%ld", (long)getpid()); }
	assert(realpath(argv[0], self_path) != NULL);
	snprintf(transport, sizeof(transport),
		 "trtype:RDMA adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:context-test-%s", identity);
	snprintf(session, sizeof(session), "context-test-%s", identity);
	if (!worker) { test_region_leases(); }
	snprintf(session, sizeof(session), "format-test-%s", identity);
	snprintf(disk_name, sizeof(disk_name), "/ucm_engine_mock_disk_%s", identity);
	int fd = shm_open(disk_name, O_RDWR | (worker ? 0 : O_CREAT | O_EXCL), 0600);
	assert(fd >= 0);
	if (!worker) { assert(ftruncate(fd, 2 * sizeof(*disks)) == 0); }
	disks = mmap(NULL, 2 * sizeof(*disks), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(disks != MAP_FAILED);
	close(fd);
	struct spdk_env_opts opts = {.opts_size = sizeof(opts)};
	spdk_env_opts_init(&opts);
	opts.name = "ucm_engine_context_test";
	opts.no_pci = true;
	opts.no_huge = true;
	opts.mem_size = 256;
	char map[32];
	snprintf(map, sizeof(map), "0@%d", sched_getcpu());
	opts.core_mask = NULL;
	opts.lcore_map = map;
	assert(spdk_env_init(&opts) == 0);
	process_env_ready = 1;
	if (retirement_worker) {
		struct spdk_store_engine *a = store_engine_create();
		store_engine_select(a);
		assert(store_engine_init(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 0) == 0);
		uint8_t id[16] = {77}, data[2 * TEST_SHARD_KB * 1024] = {0};
		uint32_t old_slot = pin_partial_overwrite(id, data);
		int out = atoi(argv[3]), in = atoi(argv[4]);
		assert(write(out, "x", 1) == 1);
		char msg;
		assert(read(in, &msg, 1) == 1);
		check_slot_accounting(old_slot, 1);
		assert(store_sync_op(OP_CANCEL_PARTIAL) == 0);
		check_slot_accounting(old_slot, 0);
		store_engine_destroy(a);
		return 0;
	}
	if (worker) {
		struct spdk_store_engine *fa = store_engine_create(), *wa = store_engine_create();
		store_engine_select(fa);
		assert(store_engine_init_for_session(transport, TEST_SLOTS, 2, TEST_SHARD_KB, 0, 1, session) == 0);
		store_engine_select(wa);
		assert(store_engine_init_for_session(transport, TEST_SLOTS, 5, TEST_SHARD_KB, 4, 1, session) == 0);
		store_engine_destroy(fa);
		store_engine_destroy(wa);
		return 0;
	}
	test_engines();
	test_default_context();
	test_retirement();
	test_payload_io();
	test_bounded_pools();
	test_load_admission_and_affinity();
	puts("PASS: 512/4096-sector direct/bounce IO, 256 independent FA+WA tokens, bounded bounce pools, guarded metadata, allocation/IO failure cleanup, region/session leases, real rings/reactors/GC, SHM recovery and local/remote deferred reclaim");
	return 0;
}
