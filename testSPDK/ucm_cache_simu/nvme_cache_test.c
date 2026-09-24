/*
 * nvme_cache_test.c
 *
 * SPDK raw-NVMe variant of kernel_cache_test: reproduces the UCM POSIX
 * store (psync engine) I/O pattern for GLM-5.3 (78 layers, MLA,
 * kv_lora_rank=512 + rope=64; both -W modes are production configs)
 * without a file system.
 *
 * Mapping from the POSIX/file model to raw LBA:
 *   file per block       -> sequential LBA range per block, assigned at init
 *   shard at file offset -> LBA = block_base + shard_index * shard_sectors
 *   .tmp + rename commit -> hash-table insert when the last shard completes
 *   access() lookup      -> hash-table lookup before each load (miss skips)
 *   O_DIRECT             -> always (userspace NVMe)
 *   filename-as-index    -> persisted index region: block i's record lives at
 *                           LBA (index_base + i), written through on every NEW
 *                           hash insert (re-dumps of the same block are
 *                           idempotent no-ops). Recovery (scan + validate +
 *                           rebuild hash on startup) is future work; the
 *                           on-disk format below is designed for it.
 *
 * Layerwise ON  (-W 1, UCM default): one shard per layer, 78 per block.
 * Layerwise OFF (-W 0): the whole block is one shard (single command).
 *
 * Large commands are split by the SPDK NVMe driver at MDTS automatically,
 * matching the kernel path where a 13MB pwrite is split by the block layer.
 */

#include "spdk/config.h"
#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#define MAX_QD           1024
#define MAX_BLOCKS       100000
#define BLOCK_ID_HEX_LEN 32
#define HASH_SLOTS       (1u << 18)      /* open addressing, >= 2 * MAX_BLOCKS */
#define MAX_SLOTS_MEM    (512ULL << 20)  /* cap DMA buffer pool at 512 MiB */
#define QPAIR_REQUESTS   16384           /* request pool for MDTS-split cmds */
#define INDEX_MAGIC      0x55434D49445831ULL /* "UCMIDX1" */
#define NUM_INDEX_BUFS   32              /* in-flight index records */

/* Persisted index record, one per sector. The first 32 bytes are covered by
 * the checksum so recovery can detect torn/partial writes. */
struct index_record {
	uint64_t	magic;          /* INDEX_MAGIC */
	uint32_t	block_index;
	uint32_t	flags;          /* reserved for recovery */
	uint8_t		block_id[16];   /* binary block id */
	uint64_t	checksum;       /* hash_fn over the first 32 bytes */
};
SPDK_STATIC_ASSERT(offsetof(struct index_record, checksum) == 32, "bad layout");

struct idx_buf {
	void		*buf;
	uint64_t	submit_tsc;
};

struct idx_pending {
	uint32_t	block_index;
	uint8_t		block_id[16];
};

enum mode { MODE_DUMP, MODE_LOAD, MODE_MIXED };

struct io_slot {
	void		*buf;
	uint64_t	submit_tsc;
	uint32_t	block_index;
	int		is_write;
	int		in_flight;
};

struct shard_task {
	uint32_t	block_index;
	uint32_t	shard_index;
	int		is_write;
};

struct hash_entry {
	char		id[BLOCK_ID_HEX_LEN + 1];
	uint32_t	block_index;
	uint8_t		used;
};

static struct spdk_nvme_ctrlr		*g_ctrlr;
static struct spdk_nvme_ns		*g_ns;
static struct spdk_nvme_qpair		*g_qpair;
static struct spdk_nvme_transport_id	g_trid;

/* Model parameters (same defaults as kernel_cache_test) */
static uint32_t g_block_tokens    = 128;
static uint32_t g_num_layers      = 78;
static uint32_t g_kv_lora_rank    = 512;
static uint32_t g_qk_rope_dim     = 64;
static uint32_t g_bytes_per_elem  = 2;

/* Workload parameters */
static uint32_t g_num_blocks      = 500;
static uint32_t g_concurrency     = 32;
static enum mode g_mode           = MODE_MIXED;
static uint32_t g_write_pct       = 50;
static int      g_runtime_sec     = 30;
static int      g_layerwise       = 1;
static int      g_init_dump       = 1;
static uint32_t g_replace_pct     = 100;
static int      g_target_rand     = 1;
static uint32_t g_seed            = 1;

/* Device parameters */
static uint32_t g_qd              = 128;
static uint64_t g_offset_gib      = 0;

static uint32_t g_sector_size;
static uint64_t g_num_sectors;
static uint64_t g_base_lba;
static uint32_t g_shard_sectors;
static uint32_t g_block_sectors;

static size_t   g_kv_per_token;
static size_t   g_shard_size;
static size_t   g_block_file_size;

/* Block table: id -> sequential LBA */
static char     g_block_ids[MAX_BLOCKS][BLOCK_ID_HEX_LEN + 1];
static uint64_t g_block_lbas[MAX_BLOCKS];
static uint32_t g_block_outstanding[MAX_BLOCKS];

/* Hash table: block id -> present (models the Cache-layer Lookup) */
static struct hash_entry g_hash[HASH_SLOTS];
static uint32_t g_hash_count;

/* Persisted index region: [index_lba, index_lba + num_blocks) sectors */
static int      g_persist_index = 1;
static uint64_t g_index_lba;
static struct idx_buf g_idx[NUM_INDEX_BUFS];
static uint32_t g_idx_free[NUM_INDEX_BUFS];
static int      g_idx_free_top;
static struct idx_pending *g_idx_pending;
static uint32_t g_idx_pend_head, g_idx_pend_tail, g_idx_pend_cap;
static uint64_t g_idx_count;
static uint64_t g_idx_ticks;

/* IO slots */
static struct io_slot g_slots[MAX_QD];
static uint32_t g_nslots;
static uint32_t g_free_stack[MAX_QD];
static int      g_free_top;

static struct shard_task *g_tasks;
static uint32_t g_load_remaining;

static uint64_t g_start_tsc;
static uint64_t g_deadline_tsc;
static uint64_t g_tsc_hz;
static int      g_stop;

static uint64_t g_completed_shards;
static uint64_t g_completed_blocks;
static uint64_t g_total_bytes;
static uint64_t g_total_lat_ticks;
static uint64_t g_lookup_ticks;
static uint64_t g_lookup_count;
static uint64_t g_lookup_miss;
static uint64_t g_commit_ticks;
static uint64_t g_commit_count;
static uint64_t g_dump_skip;
static uint64_t g_dup_skip;
static uint8_t *g_batch_slots;
static uint64_t g_replace_count;
static uint64_t g_err_count;
static uint64_t g_last_progress_tsc;
static int      g_stalled;

static uint32_t
nshards(void)
{
	return g_layerwise ? g_num_layers : 1;
}

static void
gen_random_block_id(char *out, uint32_t seed_val)
{
	uint8_t id[16];
	for (int i = 0; i < 16; i++) {
		seed_val = seed_val * 1103515245u + 12345u;
		id[i] = (uint8_t)((seed_val >> 16) & 0xff);
	}
	for (int i = 0; i < 16; i++) {
		sprintf(out + i * 2, "%02x", id[i]);
	}
	out[32] = '\0';
}

static void
gen_block_id_array(uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		gen_random_block_id(g_block_ids[i], g_seed + i * 17 + 1);
	}
}

static uint32_t
hash_fn(const char *id)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < BLOCK_ID_HEX_LEN; i++) {
		h ^= (uint8_t)id[i];
		h *= 16777619u;
	}
	return h;
}

/* FNV-1a over n bytes: record checksum + block-id hashing. */
static uint32_t
hash_fn_n(const void *data, int n)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261u;

	for (int i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

/* Models UCM SpaceLayout::CommitFile: the block becomes visible to
 * lookups only after its last shard is dumped. Returns 1 when the entry is
 * new or its mapping changed (i.e. must be persisted), 0 otherwise.
 * used: 0 = free, 1 = live, 2 = tombstone (probe chains continue through
 * tombstones; inserts may reuse them). */
static int
hash_insert(const char *id, uint32_t block_index)
{
	uint32_t i = hash_fn(id) & (HASH_SLOTS - 1);
	uint32_t first_tomb = HASH_SLOTS;

	while (g_hash[i].used != 0) {
		if (g_hash[i].used == 1) {
			if (memcmp(g_hash[i].id, id, BLOCK_ID_HEX_LEN + 1) == 0) {
				if (g_hash[i].block_index == block_index) {
					return 0;
				}
				g_hash[i].block_index = block_index;
				return 1;
			}
		} else if (first_tomb == HASH_SLOTS) {
			first_tomb = i;
		}
		i = (i + 1) & (HASH_SLOTS - 1);
	}
	if (first_tomb != HASH_SLOTS) {
		i = first_tomb;
	}
	memcpy(g_hash[i].id, id, BLOCK_ID_HEX_LEN + 1);
	g_hash[i].block_index = block_index;
	g_hash[i].used = 1;
	g_hash_count++;
	return 1;
}

/* Eviction: tombstone the entry for id. Safe only when the id's IOs all
 * completed (caller guarantees it). */
static void
hash_delete_by_id(const char *id)
{
	uint32_t i = hash_fn(id) & (HASH_SLOTS - 1);

	while (g_hash[i].used != 0) {
		if (g_hash[i].used == 1 &&
		    memcmp(g_hash[i].id, id, BLOCK_ID_HEX_LEN + 1) == 0) {
			g_hash[i].used = 2;
			g_hash_count--;
			return;
		}
		i = (i + 1) & (HASH_SLOTS - 1);
	}
}

/* Models UCM SpaceManager::Lookup (access() on the final file). */
static int
hash_lookup(const char *id)
{
	uint32_t i = hash_fn(id) & (HASH_SLOTS - 1);

	while (g_hash[i].used != 0) {
		if (g_hash[i].used == 1 &&
		    memcmp(g_hash[i].id, id, BLOCK_ID_HEX_LEN + 1) == 0) {
			return 1;
		}
		i = (i + 1) & (HASH_SLOTS - 1);
	}
	return 0;
}

/* ---- persisted index region (write-through on new inserts) ---- */

static void index_flush(void);

static void
index_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct idx_buf *ib = arg;

	g_idx_free[g_idx_free_top++] = (uint32_t)(ib - g_idx);
	g_last_progress_tsc = spdk_get_ticks();
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_err_count++;
		return;
	}
	g_idx_count++;
	g_idx_ticks += spdk_get_ticks() - ib->submit_tsc;
}

/* Queue one record; actual NVMe writes happen from index_flush(), which is
 * invoked from every polling site, so this never blocks the commit path. */
static void
index_enqueue(uint32_t block_index, const char *id_hex)
{
	struct idx_pending *p;
	int i;

	if (!g_persist_index) {
		return;
	}
	if (g_idx_pend_tail - g_idx_pend_head >= g_idx_pend_cap) {
		/* Cannot happen with the fixed id pool (each block inserts once);
		 * guard anyway so a future fresh-id workload fails loudly. */
		fprintf(stderr, "index pending queue full, record lost\n");
		g_err_count++;
		return;
	}
	p = &g_idx_pending[g_idx_pend_tail % g_idx_pend_cap];
	p->block_index = block_index;
	for (i = 0; i < 16; i++) {
		int hi = id_hex[2 * i];
		int lo = id_hex[2 * i + 1];

		hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
		lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
		p->block_id[i] = (uint8_t)((hi << 4) | lo);
	}
	g_idx_pend_tail++;
}

static void
index_flush(void)
{
	if (!g_persist_index) {
		return;
	}
	while (g_idx_pend_head != g_idx_pend_tail && g_idx_free_top > 0) {
		struct idx_pending *p = &g_idx_pending[g_idx_pend_head % g_idx_pend_cap];
		struct idx_buf *ib = &g_idx[g_idx_free[--g_idx_free_top]];
		struct index_record *rec = ib->buf;

		memset(rec, 0, g_sector_size);
		rec->magic = INDEX_MAGIC;
		rec->block_index = p->block_index;
		memcpy(rec->block_id, p->block_id, 16);
		rec->checksum = (uint64_t)hash_fn_n(rec, 32);

		ib->submit_tsc = spdk_get_ticks();
		if (spdk_nvme_ns_cmd_write(g_ns, g_qpair, ib->buf,
					   g_index_lba + p->block_index, 1,
					   index_complete, ib, 0) < 0) {
			g_err_count++;
			g_idx_free[g_idx_free_top++] = (uint32_t)(ib - g_idx);
		}
		g_idx_pend_head++;
	}
}

/* Drain condition: all data IOs, all pending records and all in-flight
 * index writes finished. No-op when persistence is disabled (buffers are
 * not allocated then). */
static int
index_busy(void)
{
	if (!g_persist_index) {
		return 0;
	}
	return (g_idx_pend_head != g_idx_pend_tail) ||
	       (g_idx_free_top < NUM_INDEX_BUFS);
}

/* Poll the IO qpair, the admin qpair and the index flusher. The admin poll
 * is mandatory on fabrics transports: SPDK negotiates a 10s keep-alive by
 * default and only services it from spdk_nvme_ctrlr_process_admin_completions;
 * without it an nvmf target drops the connection mid-run and every wait
 * loop below spins forever. */
static void
poll_all(void)
{
	spdk_nvme_qpair_process_completions(g_qpair, 0);
	spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	index_flush();
}

/* Watchdog: 30s without any completion means the connection is dead with
 * IOs outstanding; abort instead of spinning forever. */
static void
check_stall(void)
{
	uint64_t now = spdk_get_ticks();

	if (!g_stalled && now - g_last_progress_tsc > 30ull * spdk_get_ticks_hz()) {
		fprintf(stderr, "no IO completion for 30s, connection lost? aborting\n");
		g_err_count++;
		g_stalled = 1;
		g_stop = 1;
	}
}

static void
shard_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_slot *s = arg;
	uint32_t slot_idx = (uint32_t)(s - g_slots);

	s->in_flight = 0;
	g_free_stack[g_free_top++] = slot_idx;
	g_last_progress_tsc = spdk_get_ticks();
	if (!s->is_write) {
		g_load_remaining--;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		g_err_count++;
		return;
	}

	g_completed_shards++;
	g_total_bytes += g_shard_size;
	g_total_lat_ticks += spdk_get_ticks() - s->submit_tsc;

	/* Commit on last shard: a failed dump leaves the block invisible.
	 * New/changed entries are persisted to the index region. */
	if (s->is_write && --g_block_outstanding[s->block_index] == 0) {
		uint64_t t0 = spdk_get_ticks();
		int changed = hash_insert(g_block_ids[s->block_index], s->block_index);
		g_commit_ticks += spdk_get_ticks() - t0;
		g_commit_count++;
		if (changed) {
			index_enqueue(s->block_index, g_block_ids[s->block_index]);
		}
	}
}

static int
submit_task(struct io_slot *s, const struct shard_task *t)
{
	uint64_t lba = g_block_lbas[t->block_index] +
		       (uint64_t)t->shard_index * g_shard_sectors;
	int rc;

	s->block_index = t->block_index;
	s->is_write = t->is_write;
	s->submit_tsc = spdk_get_ticks();
	s->in_flight = 1;

	if (t->is_write) {
		rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, s->buf, lba, g_shard_sectors,
					    shard_complete, s, 0);
	} else {
		rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, s->buf, lba, g_shard_sectors,
					   shard_complete, s, 0);
	}
	if (rc < 0) {
		s->in_flight = 0;
		g_err_count++;
		if (!t->is_write) {
			g_load_remaining--;
		}
		return -1;
	}
	return 0;
}

/* Submit one task; spins on completions until an IO slot frees up (this is
 * the natural backpressure for async dumps: slot count bounds the backlog). */
static void
submit_async(const struct shard_task *t)
{
	while (g_free_top == 0 && !g_stalled) {
		poll_all();
		check_stall();
	}
	if (g_stalled) {
		/* Connection lost: drop the task, keeping the load accounting
		 * consistent so run_step's wait loop terminates. */
		if (!t->is_write) {
			g_load_remaining--;
		}
		return;
	}
	uint32_t idx = g_free_stack[--g_free_top];
	if (submit_task(&g_slots[idx], t) != 0) {
		g_free_stack[g_free_top++] = idx;
	}
}

/* Submit a step's tasks and wait only for its loads, mirroring UCM:
 * loads gate the step (wait_for_layer_load), dumps drain in the background
 * across steps (_pending_dump_tasks). */
static void
run_step(struct shard_task *tasks, uint32_t count)
{
	uint32_t nload = 0;

	for (uint32_t i = 0; i < count; i++) {
		if (tasks[i].is_write) {
			submit_async(&tasks[i]);
		} else {
			nload++;
		}
	}
	g_load_remaining = nload;
	for (uint32_t i = 0; i < count; i++) {
		if (!tasks[i].is_write) {
			submit_async(&tasks[i]);
		}
	}
	while (g_load_remaining > 0 && !g_stalled) {
		poll_all();
		check_stall();
	}
}

/* Submit a setup batch (init-dump) and wait for every IO to finish. */
static void
run_batch_sync(struct shard_task *tasks, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		submit_async(&tasks[i]);
	}
	while ((g_free_top < (int)g_nslots || index_busy()) && !g_stalled) {
		poll_all();
		check_stall();
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;

	g_ctrlr = ctrlr;
	printf("Attached: %s  %.40s FW %.8s\n", trid->traddr,
	       spdk_nvme_ctrlr_get_data(ctrlr)->mn,
	       spdk_nvme_ctrlr_get_data(ctrlr)->fr);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		g_ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (g_ns != NULL && spdk_nvme_ns_is_active(g_ns)) {
			break;
		}
		g_ns = NULL;
	}
}

static void
usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  SPDK raw-NVMe variant of kernel_cache_test: same UCM POSIX-store\n");
	printf("  I/O pattern for GLM-5.3 (78-layer MLA), blocks at pre-allocated LBAs,\n");
	printf("  lookup via hash table instead of access() on files.\n");
	printf("\n");
	printf("  Device:\n");
	printf("    -r <trid>     Transport ID, e.g. 'trtype:PCIe traddr:0000:84:00.0'\n");
	printf("                  or RDMA: 'trtype:RDMA adrfam:IPv4 traddr:IP trsvcid:4420'\n");
	printf("                  (default: trtype:PCIe traddr:0000:84:00.0)\n");
	printf("    -q <depth>    Queue depth (default 128, mimics UCM dataTransConcurrency;\n");
	printf("                  replaces -T of kernel_cache_test, max %d)\n", MAX_QD);
	printf("    -O <GiB>      LBA offset of the block pool (default 0)\n");
	printf("\n");
	printf("  Model parameters (same as kernel_cache_test):\n");
	printf("    -B <tokens>   vLLM block_size in tokens (default 128)\n");
	printf("    -L <layers>   num_hidden_layers (default 78, GLM-5.3)\n");
	printf("    -K <bytes>    kv_lora_rank (default 512)\n");
	printf("    -R <bytes>    qk_rope_head_dim (default 64)\n");
	printf("    -E <bytes>    bytes per element (default 2, bf16)\n");
	printf("\n");
	printf("  Workload parameters:\n");
	printf("    -n <blocks>   Total blocks in the random ID pool (default 500)\n");
	printf("    -c <concur>   Load blocks per scheduler step (default 32, prefill-scale batch);\n");
	printf("                  dump I/O is submitted async across steps (UCM semantics)\n");
	printf("    -m <mode>     dump | load | mixed (default mixed)\n");
	printf("    -M <pct>      Write %% for mixed mode (default 50)\n");
	printf("    -F <pct>      Id-replacement %% for dumps: chance a dumped block slot\n");
	printf("                  gets a fresh random id (eviction; index record and data\n");
	printf("                  rewritten, no LRU) (default 100: real KV-cache logic,\n");
	printf("                  every dump is new content; use 0 for a stable id pool)\n");
	printf("    -S <mode>     Slot targeting: rand = random slot per operation (request-\n");
	printf("                  driven access, default) | seq = sequential cursor scan\n");
	printf("    -t <sec>      Run time in seconds (default 30)\n");
	printf("    -W <0|1>      Layerwise: 1=per-layer shard I/O, 0=whole-block single I/O (default 1, UCM default)\n");
	printf("    -C <0|1>      Init-dump (pre-write) all blocks on startup (default 1;\n");
	printf("                  -C 0 allows only dump mode)\n");
	printf("    -I <0|1>      Persist index records to a dedicated LBA region on new\n");
	printf("                  hash inserts (default 1); recovery not implemented yet\n");
	printf("\n");
	printf("  Storage:\n");
	printf("    -s <seed>     PRNG seed (default 1)\n");
	printf("    -h            Show this help\n");
}

int
main(int argc, char **argv)
{
	int op, rc = 0, i;
	struct spdk_env_opts opts;
	struct spdk_nvme_io_qpair_opts qopts;
	const char *mode_names[] = {"dump", "load", "mixed"};
	uint64_t end_tsc, elapsed_ticks, need_sectors;
	uint64_t snap_shards, snap_blocks, snap_bytes, snap_lat;
	uint64_t snap_lookup, snap_lookup_cnt, snap_lookup_miss;
	uint64_t snap_commit, snap_commit_cnt, snap_idx, snap_idx_cnt, snap_err;
	uint32_t batch_cap;
	double elapsed_sec, iops, mibs, blocks_per_sec, avg_lat_us;
	size_t alloc_bytes = 0;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	srand(g_seed);
	while ((op = getopt(argc, argv, "r:q:O:B:L:K:R:E:n:c:m:M:t:W:C:I:F:S:s:h")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "bad trid: %s\n", optarg);
				return 1;
			}
			break;
		case 'q': g_qd = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'O': g_offset_gib = (uint64_t)spdk_strtol(optarg, 10); break;
		case 'B': g_block_tokens = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'L': g_num_layers = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'K': g_kv_lora_rank = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'R': g_qk_rope_dim = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'E': g_bytes_per_elem = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'n': g_num_blocks = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'c': g_concurrency = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'm':
			if (strcmp(optarg, "dump") == 0) { g_mode = MODE_DUMP; }
			else if (strcmp(optarg, "load") == 0) { g_mode = MODE_LOAD; }
			else if (strcmp(optarg, "mixed") == 0) { g_mode = MODE_MIXED; }
			else { fprintf(stderr, "bad mode: %s\n", optarg); return 1; }
			break;
		case 'M': g_write_pct = (uint32_t)spdk_strtol(optarg, 10); break;
		case 't': g_runtime_sec = spdk_strtol(optarg, 10); break;
		case 'W': g_layerwise = spdk_strtol(optarg, 10); break;
		case 'C': g_init_dump = spdk_strtol(optarg, 10); break;
		case 'I': g_persist_index = spdk_strtol(optarg, 10); break;
		case 'F': g_replace_pct = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'S':
			if (strcmp(optarg, "rand") == 0) { g_target_rand = 1; }
			else if (strcmp(optarg, "seq") == 0) { g_target_rand = 0; }
			else { fprintf(stderr, "bad target mode: %s (rand|seq)\n", optarg); return 1; }
			break;
		case 's':
			g_seed = (uint32_t)spdk_strtol(optarg, 10);
			srand(g_seed);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (op == 'h') ? 0 : 1;
		}
	}

	if (!g_init_dump && g_mode != MODE_DUMP) {
		fprintf(stderr, "ERROR: mode '%s' requires init-dump; with -C 0 only dump mode is allowed "
			"(use -m dump or -C 1)\n", mode_names[g_mode]);
		return 1;
	}
	if (g_block_tokens == 0 || g_num_layers == 0) {
		fprintf(stderr, "invalid block_tokens/layers\n");
		return 1;
	}
	if (g_qd == 0 || g_qd > MAX_QD) {
		fprintf(stderr, "invalid queue depth %u (1-%d)\n", g_qd, MAX_QD);
		return 1;
	}
	if (g_num_blocks == 0 || g_num_blocks > MAX_BLOCKS) {
		fprintf(stderr, "num_blocks %u exceeds max %d\n", g_num_blocks, MAX_BLOCKS);
		return 1;
	}
	if (g_concurrency == 0 || g_concurrency > g_num_blocks) {
		g_concurrency = g_num_blocks;
	}

	/* Sizes, identical to kernel_cache_test */
	g_kv_per_token = (g_kv_lora_rank + g_qk_rope_dim) * g_bytes_per_elem;
	g_block_file_size = g_kv_per_token * g_block_tokens * g_num_layers;
	g_shard_size = g_layerwise ? g_kv_per_token * g_block_tokens : g_block_file_size;
	if (g_shard_size % 4096 != 0) {
		size_t aligned = (g_shard_size + 4095) / 4096 * 4096;
		fprintf(stderr, "WARNING: shard_size %zu not 4K-aligned, rounding to %zu\n",
			g_shard_size, aligned);
		g_shard_size = aligned;
		g_block_file_size = g_layerwise ? g_shard_size * g_num_layers : g_shard_size;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nvme_cache_test";

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Probing NVMe at %s ...\n", g_trid.traddr);
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0 || g_ctrlr == NULL || g_ns == NULL) {
		fprintf(stderr, "no NVMe controller/namespace attached\n");
		rc = 1;
		goto exit_fini;
	}

	g_sector_size = spdk_nvme_ns_get_sector_size(g_ns);
	g_num_sectors = spdk_nvme_ns_get_num_sectors(g_ns);
	g_base_lba = g_offset_gib * (1024ULL * 1024 * 1024) / g_sector_size;

	if (g_shard_size % g_sector_size != 0 ||
	    g_block_file_size % g_sector_size != 0) {
		fprintf(stderr, "shard/block size not multiple of sector size %u\n",
			g_sector_size);
		rc = 1;
		goto exit_fini;
	}
	g_shard_sectors = (uint32_t)(g_shard_size / g_sector_size);
	g_block_sectors = (uint32_t)(g_block_file_size / g_sector_size);

	/* Sequential LBA assignment: block b owns [base + b*bs, base + (b+1)*bs).
	 * A dedicated index region of one sector per block follows the data pool. */
	need_sectors = (uint64_t)g_num_blocks * (g_block_sectors + 1);
	if (g_base_lba >= g_num_sectors || need_sectors > g_num_sectors - g_base_lba) {
		fprintf(stderr, "block pool + index region (%.2f GiB) does not fit namespace "
			"at offset %lu GiB (available %.2f GiB)\n",
			(double)need_sectors * g_sector_size / (1024.0 * 1024 * 1024),
			(unsigned long)g_offset_gib,
			(double)(g_num_sectors - g_base_lba) * g_sector_size / (1024.0 * 1024 * 1024));
		rc = 1;
		goto exit_fini;
	}
	g_index_lba = g_base_lba + (uint64_t)g_num_blocks * g_block_sectors;
	gen_block_id_array(g_num_blocks);
	for (uint32_t b = 0; b < g_num_blocks; b++) {
		g_block_lbas[b] = g_base_lba + (uint64_t)b * g_block_sectors;
	}

	g_nslots = g_qd;
	if ((uint64_t)g_nslots * g_shard_size > MAX_SLOTS_MEM) {
		g_nslots = (uint32_t)(MAX_SLOTS_MEM / g_shard_size);
		printf("NOTE: queue depth clamped to %u slots (buffer cap %lu MiB)\n",
		       g_nslots, (unsigned long)(MAX_SLOTS_MEM >> 20));
	}
	if (g_nslots == 0) {
		fprintf(stderr, "shard_size %zu exceeds buffer cap\n", g_shard_size);
		rc = 1;
		goto exit_fini;
	}

	printf("Model: GLM-5.3 | layers=%u | MLA(kv_lora_rank=%u, rope=%u) | %uB/elem\n",
	       g_num_layers, g_kv_lora_rank, g_qk_rope_dim, g_bytes_per_elem);
	printf("vLLM block_size=%u tokens | kv_per_token=%zu B | shard_size=%zu B (%.1f KB) | "
	       "block_file_size=%zu B (%.2f MB)\n",
	       g_block_tokens, g_kv_per_token, g_shard_size,
	       (double)g_shard_size / 1024.0, g_block_file_size,
	       (double)g_block_file_size / (1024.0 * 1024.0));
	printf("Layerwise: %s | shards/block=%u | MDTS=%u B\n",
	       g_layerwise ? "on (per-layer shard)" : "off (whole block)",
	       nshards(), spdk_nvme_ns_get_max_io_xfer_size(g_ns));
	printf("Workload: %s | blocks=%u | load-step=%u (dump=async) | QD=%u | data LBA [%ju, %ju) ~%.2f GiB\n",
	       mode_names[g_mode], g_num_blocks, g_concurrency, g_nslots,
	       (uintmax_t)g_base_lba,
	       (uintmax_t)(g_base_lba + (uint64_t)g_num_blocks * g_block_sectors),
	       (double)((uint64_t)g_num_blocks * g_block_sectors) * g_sector_size
	       / (1024.0 * 1024 * 1024));
	printf("Index persist     : %s", g_persist_index ? "on" : "off");
	if (g_persist_index) {
		printf(" (LBA [%ju, %ju), 1 sector/block)",
		       (uintmax_t)g_index_lba, (uintmax_t)(g_index_lba + g_num_blocks));
	}
	printf("\n");
	if (g_replace_pct > 0) {
		printf("Id replacement   : %u%% of dumped blocks get a fresh id\n",
		       g_replace_pct);
	}
	printf("Slot targeting   : %s\n",
	       g_target_rand ? "rand (per-op random slot)" : "seq (cursor scan)");
	printf("Init-dump: %s | Run time: %d s\n\n",
	       g_init_dump ? "on (pre-write)" : "off", g_runtime_sec);

	spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts, sizeof(qopts));
	/* Explicit SQ sizing (DEV_GUIDE 坑 3/坑 4): every slot's shard may be
	 * split at MDTS (144KB shard vs 128KB MDTS = 2 children) and index
	 * records share the same SQ; the 256-entry default is exactly full at
	 * QD 128 with zero headroom for index writes. Capped at 1024, the
	 * nvmf RDMA loopback transport's --max-queue-depth. */
	{
		uint32_t max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
		uint32_t split = (uint32_t)((g_shard_size + max_xfer - 1) / max_xfer);
		uint32_t need = g_nslots * split + NUM_INDEX_BUFS + 1;

		if (need > qopts.io_queue_size) {
			qopts.io_queue_size = (need > 1024) ? 1024 : need;
		}
	}
	if (qopts.io_queue_requests < QPAIR_REQUESTS) {
		qopts.io_queue_requests = QPAIR_REQUESTS;
	}
	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qopts, sizeof(qopts));
	if (g_qpair == NULL) {
		fprintf(stderr, "alloc_io_qpair failed\n");
		rc = 1;
		goto exit_fini;
	}

	for (i = 0; i < (int)g_nslots; i++) {
		g_slots[i].buf = spdk_zmalloc(g_shard_size, 4096, NULL,
					      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_slots[i].buf == NULL) {
			fprintf(stderr, "buffer alloc failed at slot %d\n", i);
			rc = 1;
			goto exit_slots;
		}
		memset(g_slots[i].buf, 0xaa, g_shard_size);
		alloc_bytes += g_shard_size;
		g_free_stack[g_free_top++] = (uint32_t)i;
	}
	printf("Allocated %u IO slots, %.1f MiB DMA buffers\n\n",
	       g_nslots, (double)alloc_bytes / (1024.0 * 1024.0));

	if (g_persist_index) {
		for (i = 0; i < NUM_INDEX_BUFS; i++) {
			g_idx[i].buf = spdk_zmalloc(g_sector_size, 4096, NULL,
						    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
			if (g_idx[i].buf == NULL) {
				fprintf(stderr, "index buffer alloc failed at %d\n", i);
				rc = 1;
				goto exit_slots;
			}
			g_idx_free[g_idx_free_top++] = (uint32_t)i;
		}
		g_idx_pend_cap = 2 * g_num_blocks + 64;
		g_idx_pending = calloc(g_idx_pend_cap, sizeof(struct idx_pending));
		if (g_idx_pending == NULL) {
			fprintf(stderr, "index pending alloc failed\n");
			rc = 1;
			goto exit_slots;
		}
	}

	batch_cap = g_concurrency * nshards();
	g_tasks = calloc(batch_cap, sizeof(struct shard_task));
	g_batch_slots = calloc(g_num_blocks, 1);
	if (g_tasks == NULL) {
		fprintf(stderr, "task array alloc failed\n");
		rc = 1;
		goto exit_slots;
	}

	/* Init-dump: pre-write every block; commit-on-last-shard fills the hash */
	if (g_init_dump) {
		printf("Init-dump: pre-writing all %u blocks (%.2f GiB)...\n",
		       g_num_blocks,
		       (double)g_num_blocks * g_block_file_size / (1024.0 * 1024 * 1024));
		g_last_progress_tsc = spdk_get_ticks();
		for (uint32_t b = 0; b < g_num_blocks; b += g_concurrency) {
			uint32_t n = g_concurrency;
			uint32_t ntask = 0;

			if (b + n > g_num_blocks) { n = g_num_blocks - b; }
			for (uint32_t j = 0; j < n; j++) {
				uint32_t bid = b + j;
				g_block_outstanding[bid] = nshards();
				for (uint32_t s = 0; s < nshards(); s++) {
					g_tasks[ntask].block_index = bid;
					g_tasks[ntask].shard_index = s;
					g_tasks[ntask].is_write = 1;
					ntask++;
				}
			}
			run_batch_sync(g_tasks, ntask);
			if ((b / g_concurrency) % 50 == 0) {
				printf("\r  Progress: %u/%u blocks", b + n, g_num_blocks);
				fflush(stdout);
			}
		}
		printf("\nInit-dump done: %u blocks written, %u in hash.\n\n",
		       g_num_blocks, g_hash_count);

		g_completed_shards = 0;
		g_completed_blocks = 0;
		g_total_bytes = 0;
		g_total_lat_ticks = 0;
		g_lookup_ticks = 0;
		g_lookup_count = 0;
		g_lookup_miss = 0;
		g_commit_ticks = 0;
		g_commit_count = 0;
		g_idx_count = 0;
		g_idx_ticks = 0;
		g_dump_skip = 0;
		g_dup_skip = 0;
		g_replace_count = 0;
		g_err_count = 0;
	}

	/* Run loop: same batch/barrier structure as kernel_cache_test */
	g_tsc_hz = spdk_get_ticks_hz();
	g_start_tsc = spdk_get_ticks();
	g_deadline_tsc = g_start_tsc + (uint64_t)g_runtime_sec * g_tsc_hz;
	g_last_progress_tsc = g_start_tsc;

	{
		uint32_t block_idx = 0;

		while (!g_stop && spdk_get_ticks() < g_deadline_tsc) {
			uint32_t nwrite_blocks = 0, nread_blocks = 0, ntask = 0;

			/* Reap async dumps between steps: keeps g_block_outstanding
			 * fresh for selection and prevents an empty-step livelock
			 * (a step whose every op is skipped would otherwise spin
			 * without ever polling in-flight IOs). */
			poll_all();

			if (block_idx >= g_num_blocks) {
				if (g_mode == MODE_DUMP || g_mode == MODE_MIXED) {
					break;
				}
				block_idx = 0;
			}

			for (uint32_t j = 0; j < g_concurrency; j++) {
				uint32_t bid;
				int is_write;

				if (g_target_rand) {
					bid = (uint32_t)rand() % g_num_blocks;
				} else {
					bid = block_idx + j;
					if (bid >= g_num_blocks) { break; }
				}
				/* Same-slot dedup within a step: UCM never loads and
				 * dumps the same block in one step. */
				if (g_batch_slots[bid]) {
					g_dup_skip++;
					continue;
				}

				if (g_mode == MODE_DUMP) {
					is_write = 1;
				} else if (g_mode == MODE_LOAD) {
					is_write = 0;
				} else {
					is_write = ((uint32_t)rand() % 100 < g_write_pct);
				}

				if (!is_write) {
					/* Cache-layer lookup before load */
					uint64_t t0 = spdk_get_ticks();
					int hit = hash_lookup(g_block_ids[bid]);
					g_lookup_ticks += spdk_get_ticks() - t0;
					g_lookup_count++;
					if (!hit) {
						g_lookup_miss++;
						continue;
					}
				}

				if (is_write) {
					/* UCM never re-dumps a block whose previous
					 * generation is still in flight. */
					if (g_block_outstanding[bid] > 0) {
						g_dump_skip++;
						continue;
					}
					/* Optional eviction: with probability g_replace_pct
					 * the block slot is re-assigned to a fresh random
					 * id (hash entry tombstoned now, new index record
					 * + data written at commit). Models steady-state
					 * new-content arrivals without a real LRU. */
					if (g_replace_pct > 0 &&
					    (uint32_t)rand() % 100 < g_replace_pct) {
						char old_id[BLOCK_ID_HEX_LEN + 1];

						memcpy(old_id, g_block_ids[bid],
						       BLOCK_ID_HEX_LEN + 1);
						gen_random_block_id(g_block_ids[bid],
								    g_seed + (uint32_t)g_replace_count * 7919u + 3);
						hash_delete_by_id(old_id);
						g_replace_count++;
					}
					g_block_outstanding[bid] = nshards();
					nwrite_blocks++;
				} else {
					nread_blocks++;
				}
				g_batch_slots[bid] = 1;
				for (uint32_t s = 0; s < nshards(); s++) {
					g_tasks[ntask].block_index = bid;
					g_tasks[ntask].shard_index = s;
					g_tasks[ntask].is_write = is_write;
					ntask++;
				}
			}

			run_step(g_tasks, ntask);
			memset(g_batch_slots, 0, g_num_blocks);

			g_completed_blocks += nwrite_blocks + nread_blocks;
			/* shard/byte counters are maintained in shard_complete() only */

			block_idx += g_concurrency;
			if (block_idx >= g_num_blocks) {
				block_idx = 0;
			}

			if (g_completed_blocks % 100 == 0) {
				printf("\r  Progress: %ju blocks", (uintmax_t)g_completed_blocks);
				fflush(stdout);
			}
		}
	}
	printf("\n");

	end_tsc = spdk_get_ticks();
	if (end_tsc > g_deadline_tsc) {
		end_tsc = g_deadline_tsc;
	}

	/* Snapshot counters at deadline: the drain below finishes async dumps
	 * (and their index writes) OUTSIDE the measured window, but the
	 * completion callbacks keep incrementing the live counters during it. */
	snap_shards = g_completed_shards;
	snap_blocks = g_completed_blocks;
	snap_bytes = g_total_bytes;
	snap_lat = g_total_lat_ticks;
	snap_lookup = g_lookup_ticks;
	snap_lookup_cnt = g_lookup_count;
	snap_lookup_miss = g_lookup_miss;
	snap_commit = g_commit_ticks;
	snap_commit_cnt = g_commit_count;
	snap_idx = g_idx_ticks;
	snap_idx_cnt = g_idx_count;
	snap_err = g_err_count;

	/* UCM semantics: async dumps can still be in flight after the last step;
	 * drain them (outside the measured window) before printing results.
	 * This also drains the pending index records and their writes. */
	g_last_progress_tsc = spdk_get_ticks();
	while ((g_free_top < (int)g_nslots || index_busy()) && !g_stalled) {
		poll_all();
		check_stall();
	}
	elapsed_ticks = end_tsc - g_start_tsc;
	elapsed_sec = (double)elapsed_ticks / (double)g_tsc_hz;
	iops = (elapsed_sec > 0) ? (double)snap_shards / elapsed_sec : 0;
	blocks_per_sec = (elapsed_sec > 0) ? (double)snap_blocks / elapsed_sec : 0;
	mibs = (elapsed_sec > 0) ? (double)snap_bytes / elapsed_sec / (1024.0 * 1024.0) : 0;
	avg_lat_us = (snap_shards > 0)
		     ? (double)snap_lat / (double)snap_shards * 1e6 / (double)g_tsc_hz
		     : 0;

	printf("\n========== Results ==========\n");
	printf("Shard IOPS        : %.2f\n", iops);
	printf("Block ops/s       : %.2f\n", blocks_per_sec);
	printf("Throughput        : %.2f MiB/s (%.2f GiB/s)\n", mibs, mibs / 1024.0);
	printf("Completed         : %ju shards, %ju blocks in %.2f s\n",
	       (uintmax_t)snap_shards, (uintmax_t)snap_blocks, elapsed_sec);
	printf("Device usage      : %.2f GiB (%u blocks x %zu B) @ LBA [%ju, %ju)\n",
	       (double)((uint64_t)g_num_blocks * g_block_sectors) * g_sector_size
	       / (1024.0 * 1024 * 1024),
	       g_num_blocks, g_block_file_size,
	       (uintmax_t)g_base_lba, (uintmax_t)(g_base_lba + need_sectors));
	if (g_persist_index) {
		printf("Index region      : LBA [%ju, %ju) ~%u KiB, 1 sector/block, write-through\n",
		       (uintmax_t)g_index_lba,
		       (uintmax_t)(g_index_lba + g_num_blocks),
		       (uint32_t)((uint64_t)g_num_blocks * g_sector_size / 1024));
	}
	printf("\n");
	printf("--- Per-shard breakdown (avg) ---\n");
	printf("I/O (read/write)  : %.2f us\n", avg_lat_us);
	if (snap_lookup_cnt > 0) {
		printf("lookup (hash)     : %.2f us avg  (%ju calls, %ju miss-skip)\n",
		       (double)snap_lookup / (double)snap_lookup_cnt * 1e6 / (double)g_tsc_hz,
		       (uintmax_t)snap_lookup_cnt, (uintmax_t)snap_lookup_miss);
	}
	if (snap_commit_cnt > 0) {
		printf("commit (hash ins) : %.2f us per block  (%ju commits)\n",
		       (double)snap_commit / (double)snap_commit_cnt * 1e6 / (double)g_tsc_hz,
		       (uintmax_t)snap_commit_cnt);
	}
	if (snap_idx_cnt > 0) {
		printf("index wr (commit) : %.2f us avg  (%ju records)\n",
		       (double)snap_idx / (double)snap_idx_cnt * 1e6 / (double)g_tsc_hz,
		       (uintmax_t)snap_idx_cnt);
	}
	if (g_dump_skip > 0) {
		printf("dump-skip (busy)  : %ju blocks (re-dump skipped while in flight)\n",
		       (uintmax_t)g_dump_skip);
	}
	if (g_dup_skip > 0) {
		printf("batch-dup skip    : %ju ops (same slot twice in one step)\n",
		       (uintmax_t)g_dup_skip);
	}
	if (g_replace_count > 0) {
		printf("id-replace        : %ju blocks (fresh id, index record rewritten)\n",
		       (uintmax_t)g_replace_count);
	}
	printf("\n");
	printf("--- Totals ---\n");
	printf("I/O total         : %.3f ms\n",
	       (double)snap_lat / (double)g_tsc_hz * 1000.0);
	printf("lookup total      : %.3f ms (%ju calls)\n",
	       (double)snap_lookup / (double)g_tsc_hz * 1000.0, (uintmax_t)snap_lookup_cnt);
	printf("commit total      : %.3f ms (%ju inserts)\n",
	       (double)snap_commit / (double)g_tsc_hz * 1000.0, (uintmax_t)snap_commit_cnt);
	printf("index wr total    : %.3f ms (%ju records)\n",
	       (double)snap_idx / (double)g_tsc_hz * 1000.0, (uintmax_t)snap_idx_cnt);
	if (snap_err) {
		printf("Errors            : %ju\n", (uintmax_t)snap_err);
		rc = 1;
	}
	printf("=============================\n");

	free(g_tasks);
	free(g_batch_slots);
	free(g_idx_pending);

exit_slots:
	for (i = 0; i < (int)g_nslots; i++) {
		if (g_slots[i].buf != NULL) {
			spdk_free(g_slots[i].buf);
			g_slots[i].buf = NULL;
		}
	}
	for (i = 0; i < NUM_INDEX_BUFS; i++) {
		if (g_idx[i].buf != NULL) {
			spdk_free(g_idx[i].buf);
			g_idx[i].buf = NULL;
		}
	}
	if (g_qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	}
exit_fini:
	{
		struct spdk_nvme_detach_ctx *detach_ctx = NULL;
		fflush(stdout);
		spdk_nvme_detach_async(g_ctrlr, &detach_ctx);
		if (detach_ctx) {
			spdk_nvme_detach_poll(detach_ctx);
		}
	}
	spdk_env_fini();
	return rc;
}
