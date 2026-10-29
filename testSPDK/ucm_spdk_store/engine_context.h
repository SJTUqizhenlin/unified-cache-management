/* Private engine state. Included after the request/context type definitions. */
#ifndef UCM_SPDK_ENGINE_CONTEXT_H
#define UCM_SPDK_ENGINE_CONTEXT_H

#define REG_PAGE_SIZE 4096ULL
#define REG_CACHE_MAX 16
#define OP_RING_SIZE 4096
#define MAX_PENDING OP_RING_SIZE

struct reg_range {
	uint64_t start, end;
	uint32_t refs;
	uint8_t owned;
};

struct engine_controller {
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;
	unsigned refs;
	struct engine_controller *next;
};

struct spdk_store_engine {
	cpu_set_t reactor_affinity;
	int reactor_affinity_set;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_transport_id trid;
	struct engine_controller *controller;
	uint32_t sector_size, shard_sectors, slot_sectors, nshards, nslots, index_slots;
	uint64_t num_sectors, data_base, index_base, pool_base;
	struct shm_header *shm;
	uint8_t *shm_base;
	size_t shm_size;
	int shm_fd, lease_fd, maintenance_fd, startup_fd;
	int shm_recovery_owner, shm_recovering, region_reset, shm_created;
	pid_t registered_pid;
	int worker_slot;
	char shm_name[64], lease_name[64], maintenance_name[80];
	uint64_t gc_passes, gc_evicted;
	uint32_t layers, shard_kb;
	uint64_t offset_gib, err_count;
	_Atomic int healthy;
	void **io_bufs, **idx_bufs, *meta_buf;
	uint32_t io_free[IO_POOL_BUFS], io_free_top, io_pool_count;
	uint32_t bounce_free[IO_POOL_BUFS], bounce_free_top, bounce_pool_count;
	uint32_t idx_free[NUM_IDX_BUFS], idx_free_top;
	struct dump_ctx dctx[MAX_DUMP_CTX];
	struct load_ctx lctx[MAX_LOAD_CTX];
	struct rec_op rops[NUM_REC_OPS];
	struct shard_io sios[IO_POOL_BUFS];
	struct pending_inv pinv[MAX_PINV];
	uint32_t pinv_n, pending_n;
	uint64_t last_progress;
	int stalled, had_inflight;
	uint32_t recov_batch;
	uint64_t pool_capacity;
	uint32_t seed;
	int only_test, format, check_hugepage, role;
	struct reg_range reg_cache[REG_CACHE_MAX];
	uint32_t reg_count;
	uint64_t reg_total_bytes;
	int direct_dma, direct_only;
	struct spdk_ring *op_ring;
	pthread_t reactor_tid;
	_Atomic int reactor_stop;
	int reactor_pause_req, reactor_paused, reactor_started;
	pthread_mutex_t reactor_mu;
	pthread_cond_t reactor_paused_cv, reactor_resume_cv;
	uint32_t barriers_n;
	pthread_t gc_tid;
	pthread_mutex_t gc_mu;
	pthread_cond_t gc_cv;
	int gc_started, gc_stop_req, gc_enabled, gc_requested;
	double gc_threshold, gc_recycle;
	uint32_t gc_interval;
	int gc_fd, gc_owner;
	uint32_t gc_remaining;
	uint64_t gc_cycle_selected, gc_cycle_errors;
	struct store_op *pending[MAX_PENDING], *barriers[MAX_PENDING];
	_Atomic int initialized;
	struct spdk_store_engine *next;
};

struct spdk_store_engine *store_engine_create(void);
struct spdk_store_engine *store_engine_select(struct spdk_store_engine *engine);
int store_engine_set_cpu_affinity(const int *cores, uint32_t count);
void store_engine_destroy(struct spdk_store_engine *engine);
int store_engine_set_direct_only(int enabled);
int store_reactor_start_public(void);
void store_reactor_stop_public(void);

static struct spdk_store_engine default_engine = {
	.shm_fd = -1, .lease_fd = -1, .maintenance_fd = -1, .startup_fd = -1,
	.gc_fd = -1, .worker_slot = -1, .layers = 27, .shard_kb = 144,
	.seed = 1, .healthy = 1,
	.reactor_mu = PTHREAD_MUTEX_INITIALIZER,
	.reactor_paused_cv = PTHREAD_COND_INITIALIZER,
	.reactor_resume_cv = PTHREAD_COND_INITIALIZER,
	.gc_mu = PTHREAD_MUTEX_INITIALIZER, .gc_cv = PTHREAD_COND_INITIALIZER,
};
static _Thread_local struct spdk_store_engine *current_engine;
#define ENGINE (current_engine ? current_engine : &default_engine)

/* Keep the mature data path unchanged; every former store global is an
 * lvalue in the selected context. No SPDK object is duplicated by TLS. */
#define g_ctrlr (ENGINE->ctrlr)
#define g_ns (ENGINE->ns)
#define g_qpair (ENGINE->qpair)
#define g_trid (ENGINE->trid)
#define g_sector_size (ENGINE->sector_size)
#define g_num_sectors (ENGINE->num_sectors)
#define g_shard_sectors (ENGINE->shard_sectors)
#define g_slot_sectors (ENGINE->slot_sectors)
#define g_nshards (ENGINE->nshards)
#define g_nslots (ENGINE->nslots)
#define g_index_slots (ENGINE->index_slots)
#define g_data_base (ENGINE->data_base)
#define g_index_base (ENGINE->index_base)
#define g_pool_base (ENGINE->pool_base)
#define g_shm (ENGINE->shm)
#define g_shm_base (ENGINE->shm_base)
#define g_shm_size (ENGINE->shm_size)
#define g_shm_fd (ENGINE->shm_fd)
#define g_lease_fd (ENGINE->lease_fd)
#define g_maintenance_fd (ENGINE->maintenance_fd)
#define g_startup_fd (ENGINE->startup_fd)
#define g_shm_recovery_owner (ENGINE->shm_recovery_owner)
#define g_shm_recovering (ENGINE->shm_recovering)
#define g_region_reset (ENGINE->region_reset)
#define g_shm_created (ENGINE->shm_created)
#define g_registered_pid (ENGINE->registered_pid)
#define g_shm_name (ENGINE->shm_name)
#define g_lease_name (ENGINE->lease_name)
#define g_gc_passes (ENGINE->gc_passes)
#define g_gc_evicted (ENGINE->gc_evicted)
#define g_layers (ENGINE->layers)
#define g_shard_kb (ENGINE->shard_kb)
#define g_offset_gib (ENGINE->offset_gib)
#define g_err_count (ENGINE->err_count)
#define g_healthy (ENGINE->healthy)
#define g_io_bufs (ENGINE->io_bufs)
#define g_io_free (ENGINE->io_free)
#define g_io_free_top (ENGINE->io_free_top)
#define g_io_pool_count (ENGINE->io_pool_count)
#define g_bounce_free (ENGINE->bounce_free)
#define g_bounce_free_top (ENGINE->bounce_free_top)
#define g_bounce_pool_count (ENGINE->bounce_pool_count)
#define g_meta_buf (ENGINE->meta_buf)
#define g_idx_bufs (ENGINE->idx_bufs)
#define g_idx_free (ENGINE->idx_free)
#define g_idx_free_top (ENGINE->idx_free_top)
#define g_dctx (ENGINE->dctx)
#define g_lctx (ENGINE->lctx)
#define g_rops (ENGINE->rops)
#define g_sios (ENGINE->sios)
#define g_pinv (ENGINE->pinv)
#define g_pinv_n (ENGINE->pinv_n)
#define g_pending_n (ENGINE->pending_n)
#define g_last_progress (ENGINE->last_progress)
#define g_stalled (ENGINE->stalled)
#define g_recov_batch (ENGINE->recov_batch)
#define g_pool_capacity (ENGINE->pool_capacity)
#define g_seed (ENGINE->seed)
#define g_only_test (ENGINE->only_test)
#define g_format (ENGINE->format)
#define g_check_hugepage (ENGINE->check_hugepage)
#define g_role (ENGINE->role)
#define g_reg_cache (ENGINE->reg_cache)
#define g_reg_count (ENGINE->reg_count)
#define g_reg_total_bytes (ENGINE->reg_total_bytes)
#define g_direct_dma (ENGINE->direct_dma)
#define g_direct_only (ENGINE->direct_only)
#define g_op_ring (ENGINE->op_ring)
#define g_reactor_tid (ENGINE->reactor_tid)
#define g_reactor_stop (ENGINE->reactor_stop)
#define g_reactor_pause_req (ENGINE->reactor_pause_req)
#define g_reactor_paused (ENGINE->reactor_paused)
#define g_reactor_started (ENGINE->reactor_started)
#define g_reactor_mu (ENGINE->reactor_mu)
#define g_reactor_paused_cv (ENGINE->reactor_paused_cv)
#define g_reactor_resume_cv (ENGINE->reactor_resume_cv)
#define g_barriers_n (ENGINE->barriers_n)
#define g_gc_tid (ENGINE->gc_tid)
#define g_gc_mu (ENGINE->gc_mu)
#define g_gc_cv (ENGINE->gc_cv)
#define g_gc_started (ENGINE->gc_started)
#define g_gc_stop_req (ENGINE->gc_stop_req)
#define g_gc_enabled (ENGINE->gc_enabled)
#define g_gc_requested (ENGINE->gc_requested)
#define g_gc_threshold (ENGINE->gc_threshold)
#define g_gc_recycle (ENGINE->gc_recycle)
#define g_gc_interval (ENGINE->gc_interval)
#define g_gc_fd (ENGINE->gc_fd)
#define g_gc_owner (ENGINE->gc_owner)
#define g_gc_remaining (ENGINE->gc_remaining)
#define g_gc_cycle_selected (ENGINE->gc_cycle_selected)
#define g_gc_cycle_errors (ENGINE->gc_cycle_errors)
#define g_pending (ENGINE->pending)
#define g_barriers (ENGINE->barriers)

#endif
