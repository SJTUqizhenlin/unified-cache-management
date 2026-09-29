/*
 * ucm_spdk_store.c
 *
 * SPDK-backed UCM store backend prototype: reproduces the Posix store's
 * externally visible semantics on raw NVMe LBAs (no file system).
 *
 * Semantics mapping (Posix store -> this store):
 *   file per block        -> slot s owns [data_base + s*slot_sectors,
 *                                           data_base + (s+1)*slot_sectors)
 *   shard at file offset  -> LBA = slot_base + shard_index * shard_sectors
 *   .tmp + rename commit  -> a block becomes visible only after its last
 *                            shard write AND the new slot's index record
 *                            complete (the hash insert follows the record)
 *   unlink / GC           -> DELETED index record + slot back on the free
 *                            list (freeing is exact, no background GC)
 *   access() lookup       -> in-memory hash rebuilt from the index region
 *                            at init (recovery)
 *   index persistence     -> 1 sector per slot at index_base + s
 *
 * On-disk index record (1 sector, checksum over the first 40 bytes):
 *   magic | epoch | slot | flags | block_id[16] | checksum
 *
 * Crash consistency (overwrite = the rename analog), per commit:
 *   1. write the new slot's data shards
 *   2. write the new slot's record (epoch = ++store epoch)
 *   3. invalidate the old slot's record (DELETED flag, higher epoch)
 *   4. free the old slot (in-memory only)
 * Recovery keeps, per block id, the valid record with the highest epoch;
 * a DELETED winner means the id is absent. A crash before (2) leaves the
 * old mapping intact; between (2) and (3) both records are valid and the
 * higher epoch wins; (4) only affects memory.
 *
 * Data moves through internal DMA bounce buffers (copied in/out); a later
 * UCM integration can register the Cache stage's SHM buffers directly to
 * remove those copies. The poll loop also services the admin qpair
 * (fabrics keep-alive) and carries a 30s progress watchdog.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "spdk/config.h"
#include "spdk/stdinc.h"

#include <string.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#define MAX_QD           1024
#define IO_POOL_BUFS     256     /* shard-sized DMA bounce buffers */
#define NUM_IDX_BUFS     64      /* sector-sized index record buffers */
#define MAX_DUMP_CTX     1024    /* incomplete layerwise blocks per worker */
#define MAX_LOAD_CTX     16
#define GC_MAX_PASS      16      /* hard per-pass cap */
#define NUM_REC_OPS      64      /* in-flight durable invalidations */
#define MAX_PINV         (MAX_DUMP_CTX + GC_MAX_PASS)
#define RECOV_BATCH_MAX  64      /* sectors per recovery/format IO */
#define STORE_MAGIC      0x55434D5350444B31ULL /* "UCMSPDK1" */
#define FLAG_DELETED     0x1u
#define INVALID_SLOT     0xFFFFFFFFu

#define SB_MAGIC         0x55434D5353424C4BULL /* "UCMSBLK" */
#define SB_VERSION       2u

/*
 * On-disk layout:
 *
 *   [ superblock: 1 sector ]
 *   [ index region: fixed INDEX_RESERVED_BYTES (default 1 GiB ~ 2M slots) ]
 *   [ data pool: nslots x slot_sectors ]
 *
 * The metadata reservation is FIXED, so the index region never moves.
 * The active capacity (nslots) is a superblock field, not a layout
 * parameter: boot takes it from the superblock (passing -n requests an
 * in-place resize). Grow is always safe (the newly activated record
 * range is zeroed before the superblock is rewritten); shrink requires
 * that no live-flagged record exists beyond the new boundary (checked
 * conservatively, then the freed range is zeroed). Geometry
 * (slot/shard sectors) still pins the data pool layout and must match
 * exactly. One store per -O region; `reserved` leaves room for a
 * future partition table.
 */
#define INDEX_RESERVED_BYTES (1ULL << 30)    /* 1 GiB metadata reservation */

struct superblock {
	uint64_t	magic;
	uint32_t	version;
	uint32_t	index_slots;     /* fixed metadata capacity */
	uint32_t	nslots;          /* active capacity (authoritative) */
	uint32_t	slot_sectors;
	uint32_t	shard_sectors;
	uint32_t	reserved;        /* future partition table */
	uint64_t	checksum;        /* FNV over the first 32 bytes */
};
SPDK_STATIC_ASSERT(sizeof(struct superblock) == 40, "bad layout");
#define SB_COVER  32
#define DEFAULT_FORMAT_SLOTS 64

#define SHM_MAGIC        0x55434D5350444B53ULL /* "UCMSPDKS" */
#define SHM_VERSION      5u
#define MAX_WORKERS      64

#define GC_ENOSPC_BATCH  4       /* victims per on-demand GC pass */
SPDK_STATIC_ASSERT(MAX_DUMP_CTX + GC_MAX_PASS <= MAX_PINV,
                   "pending invalidation queue is undersized");

/*
 * The block index (hash table), free list, epoch, hotness and pin arrays
 * live in a shared memory segment so that every UCM process sees the
 * same mapping: worker processes commit dumps, the scheduler process
 * only looks up. A process-shared robust mutex serializes writers (and
 * readers); critical sections are pure memory ops (ns-scale). The
 * on-disk index region remains the crash ground truth: after a host
 * crash the segment is rebuilt by recovery.
 *
 * Deletion uses backward-shift (Knuth algorithm R) instead of tombstones:
 * the hole left by a removed entry is filled by shifting subsequent probe
 * chain members backward while they remain findable from their natural
 * home. Consequences: only used==0/1 states exist, miss probes stay O(1)
 * forever, and no runtime rehash/compaction is ever needed (recovery
 * remains the crash-time rebuild).
 *
 * GC (capacity management, the Posix store's background GC analog):
 * hotness[] (last-touch ms, stamped by lookup/load/commit from every
 * process) and pins[] (in-flight load counts) live in the shared segment.
 * When the free list is exhausted a dump triggers store_gc(), which
 * evicts the coldest unpinned blocks through the normal delete path
 * (invalidation record + exact free-list push); -ENOSPC only when GC
 * cannot free anything (all pinned). The Posix GC's expensive parts
 * (directory sampling, stat/mtime sorting, unlink storms) are replaced
 * by an O(1) free counter and shared-memory arrays.
 */
struct shm_header {
	uint64_t	magic;
	uint32_t	version;
	uint32_t	nslots;
	uint32_t	hash_size;
	uint32_t	hash_count;
	uint32_t	live_count;
	uint32_t	free_top;
	uint32_t	recovery_required;
	uint64_t	epoch;
	pid_t		workers[MAX_WORKERS];
	pthread_mutex_t	lock;
};

struct index_record {
	uint64_t	magic;
	uint64_t	epoch;
	uint32_t	slot;
	uint32_t	flags;
	uint8_t		block_id[16];
	uint64_t	checksum;
};
SPDK_STATIC_ASSERT(sizeof(struct index_record) == 48, "bad layout");
#define RECORD_COVER  40

struct hash_entry {
	uint8_t		id[16];
	uint32_t	slot;
	uint64_t	epoch;          /* recovery tie-break only */
	uint8_t		used;           /* 0 empty, 1 live (no tombstones:
					 * deletes backward-shift) */
	uint8_t		del_win;        /* recovery: highest-epoch record deleted */
};

struct dump_req {
	const void	*data;          /* caller buffer, held until completion */
	uint32_t	first;           /* first shard index of this segment */
	uint32_t	count;
	uint32_t	cursor;          /* shards submitted so far */
	uint32_t	acked;
	uint32_t	outstanding;
	void		(*cb)(void *arg, int status);
	void		*cb_arg;
	struct dump_req *next;
};

struct dump_ctx {
	int		active;
	int		failed;
	uint8_t		id[16];
	uint32_t	slot;
	uint32_t	old_slot;
	int		old_pinned;
	uint64_t	rec_epoch;
	uint32_t	nshards;
	uint8_t		*covered;       /* per-shard success flags */
	uint32_t	outstanding;    /* data commands accepted by NVMe */
	struct dump_req *req_head;
	struct dump_req *req_tail;
	int		commit_phase;   /* 0 data, 1 record write */
	int		rec_buf;        /* index buffer idx or -1 */
};

struct load_ctx {
	int		active;
	int		failed;
	uint32_t	slot;
	uint32_t	first;
	uint32_t	count;
	uint32_t	submitted;
	uint32_t	acked;
	uint32_t	outstanding;
	void		*dst;           /* caller buffer, held until completion */
	void		(*cb)(void *arg, int status);
	void		*cb_arg;
};

struct rec_op {
	int		active;
	int		waiting_pin;
	uint32_t	slot;
	int		free_on_done;
	int		buf;
};

struct shard_io {
	struct dump_ctx	*dc;
	struct dump_req	*dr;
	struct load_ctx	*lc;
	uint32_t	buf;         /* concurrency token index */
	uint32_t	shard;       /* physical shard index */
	uint32_t	offset;      /* relative shard index in caller buffer */
	int		bounce;      /* 1 = data in g_io_bufs[buf], copy on done */
};

struct pending_inv {
	uint8_t		id[16];
	uint32_t	slot;
	uint64_t	epoch;
};

typedef void (*store_cb)(void *arg, int status);

struct syncw {
	pthread_mutex_t	mu;
	pthread_cond_t	cv;
	int		done;
	int		status;
};

static void
syncw_init(struct syncw *w)
{
	w->done = 0;
	w->status = 0;
	pthread_mutex_init(&w->mu, NULL);
	pthread_cond_init(&w->cv, NULL);
}

static void
syncw_signal(struct syncw *w, int status)
{
	pthread_mutex_lock(&w->mu);
	w->status = status;
	w->done = 1;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
}

static int
syncw_wait(struct syncw *w)
{
	pthread_mutex_lock(&w->mu);
	while (!w->done) {
		pthread_cond_wait(&w->cv, &w->mu);
	}
	pthread_mutex_unlock(&w->mu);
	return w->status;
}

/* sync wrappers pass sync_cb as the op/ctx callback */
static void
sync_cb(void *arg, int status)
{
	syncw_signal(arg, status);
}

enum op_type {
	OP_DUMP,
	OP_LOAD,
	OP_DELETE,
	OP_BARRIER,         /* completes when the store drains */
	OP_CANCEL_PARTIAL,  /* cancel incomplete dump contexts, then drain */
};

struct store_op {
	int		type;
	uint8_t		id[16];
	/* dump */
	uint32_t	first;
	uint32_t	count;
	const void	*data;
	/* load */
	void		*dst;
	uint32_t	len;
	/* completion */
	store_cb	cb;
	void		*cb_arg;
	struct syncw	*sw;    /* sync wrappers wait on this */
	/* reactor-private state */
	int		gc_attempted;
};


static struct spdk_nvme_ctrlr		*g_ctrlr;
static struct spdk_nvme_ns		*g_ns;
static struct spdk_nvme_qpair		*g_qpair;
static struct spdk_nvme_transport_id	g_trid;

static uint32_t	g_sector_size;
static uint64_t	g_num_sectors;
static uint32_t	g_shard_sectors;
static uint32_t	g_slot_sectors;
static uint32_t	g_nshards;
static uint32_t	g_nslots;            /* resolved: SB value, or -n (format/resize) */
static uint32_t	g_index_slots;       /* fixed metadata capacity */
static uint64_t	g_data_base;         /* superblock LBA */
static uint64_t	g_index_base;        /* fixed: right after the superblock */
static uint64_t	g_pool_base;         /* data pool start, after the reservation */

static struct shm_header		*g_shm;
static uint8_t				*g_shm_base;
static size_t				g_shm_size;
static int				g_shm_fd = -1;
static int				g_lease_fd = -1;
static int				g_maintenance_fd = -1;
static int				g_shm_recovery_owner;
static int				g_shm_recovering;
static int				g_region_reset;   /* boot invalidated the region */
static int				g_shm_created;    /* this boot created the segment */
static pid_t				g_registered_pid;
static char				g_shm_name[64];
static char				g_lease_name[64];
static uint64_t				g_gc_passes;
static uint64_t				g_gc_evicted;
static uint32_t				g_layers = 27;
static uint32_t				g_shard_kb = 144;
static uint64_t				g_offset_gib;

/* Cross-process comparable wall clock (ms) for hotness; works in every
 * role including the device-less lookup process. */
static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Region identity = transport + LBA offset. Deliberately NOT the model
 * geometry: the scheduler process has no geometry at all (the Cache
 * stage exempts it from size checks) yet must attach the same shared
 * index, and a geometry change (model switch) reuses the same segment
 * and rebuilds it via recovery. */
static void
build_shm_names(void)
{
	uint64_t h = 1469598103934665603ULL;
	const uint8_t *p = (const uint8_t *)&g_trid;

	for (size_t i = 0; i < sizeof(g_trid); i++) {
		h = (h ^ p[i]) * 1099511628211ULL;
	}
	for (size_t i = 0; i < sizeof(g_offset_gib); i++) {
		h = (h ^ ((const uint8_t *)&g_offset_gib)[i]) * 1099511628211ULL;
	}
	snprintf(g_shm_name, sizeof(g_shm_name), "/ucm_spdk_%016" PRIx64, h);
	snprintf(g_lease_name, sizeof(g_lease_name), "/ucm_spdk_lock_%016" PRIx64, h);
}

static int
store_maintenance_open(int exclusive)
{
	g_maintenance_fd = shm_open("/ucm_spdk_maintenance_lock", O_CREAT | O_RDWR, 0600);
	if (g_maintenance_fd < 0) {
		return -errno;
	}
	if (flock(g_maintenance_fd, exclusive ? (LOCK_EX | LOCK_NB) : LOCK_SH) != 0) {
		int rc = -errno;

		close(g_maintenance_fd);
		g_maintenance_fd = -1;
		return rc;
	}
	return 0;
}

static int
store_maintenance_upgrade(void)
{
	if (g_maintenance_fd < 0) {
		return -EINVAL;
	}
	flock(g_maintenance_fd, LOCK_UN);
	if (flock(g_maintenance_fd, LOCK_EX | LOCK_NB) == 0) {
		return 0;
	}
	flock(g_maintenance_fd, LOCK_SH);
	return -EBUSY;
}

static void
store_maintenance_close(void)
{
	if (g_maintenance_fd >= 0) {
		flock(g_maintenance_fd, LOCK_UN);
		close(g_maintenance_fd);
		g_maintenance_fd = -1;
	}
}

static int
store_lease_open(void)
{
	g_lease_fd = shm_open(g_lease_name, O_CREAT | O_RDWR, 0600);
	if (g_lease_fd < 0) {
		return -errno;
	}
	if (flock(g_lease_fd, LOCK_EX | LOCK_NB) == 0) {
		g_shm_recovery_owner = 1;
		return 0;
	}
	if (errno != EWOULDBLOCK || flock(g_lease_fd, LOCK_SH) != 0) {
		int rc = -errno;

		close(g_lease_fd);
		g_lease_fd = -1;
		return rc;
	}
	g_shm_recovery_owner = 0;
	return 0;
}

/* Lookup (scheduler) processes never take the exclusive lease, even
 * when it is free: an exclusive scheduler would block a later worker's
 * recovery ownership forever. Shared-only means a scheduler may attach
 * either a live worker's index or the segment a cleanly-exited worker
 * left behind; it waits (blocking flock) while a booting worker holds
 * the exclusive lease for its initial recovery. */
static int
store_lease_open_shared(void)
{
	g_lease_fd = shm_open(g_lease_name, O_CREAT | O_RDWR, 0600);
	if (g_lease_fd < 0) {
		return -errno;
	}
	if (flock(g_lease_fd, LOCK_SH) != 0) {
		int rc = -errno;

		close(g_lease_fd);
		g_lease_fd = -1;
		return rc;
	}
	g_shm_recovery_owner = 0;
	return 0;
}

static void
store_lease_close(void)
{
	if (g_lease_fd >= 0) {
		flock(g_lease_fd, LOCK_UN);
		close(g_lease_fd);
		g_lease_fd = -1;
	}
	g_shm_recovery_owner = 0;
}

#define g_hash		((struct hash_entry *)(g_shm + 1))
#define g_hash_mask	(g_shm->hash_size - 1)
#define g_freelist	((uint32_t *)((struct hash_entry *)(g_shm + 1) + \
					g_shm->hash_size))
#define g_hotness	((uint64_t *)(g_freelist + g_shm->nslots))
#define g_pins		((uint16_t *)(g_hotness + g_shm->nslots))
#define g_hash_count	(g_shm->hash_count)
#define g_live_count	(g_shm->live_count)
#define g_free_top	(g_shm->free_top)
#define g_epoch		(g_shm->epoch)
static uint64_t	g_err_count;
static _Atomic int g_healthy = 1;

static void	**g_io_bufs;
static uint32_t	g_io_free[IO_POOL_BUFS];
static uint32_t	g_io_free_top;
static void	**g_idx_bufs;
static uint32_t	g_idx_free[NUM_IDX_BUFS];
static uint32_t	g_idx_free_top;

static struct dump_ctx	g_dctx[MAX_DUMP_CTX];
static struct load_ctx	g_lctx[MAX_LOAD_CTX];
static struct rec_op	g_rops[NUM_REC_OPS];
static struct shard_io	g_sios[IO_POOL_BUFS];
static struct pending_inv	g_pinv[MAX_PINV];
static uint32_t	g_pinv_n;
static uint32_t	g_pending_n;

static uint64_t	g_last_progress;
static int	g_stalled;

/* model defaults: DeepSeek-V2-Lite MLA (27 layers x 144KB) */
static uint32_t	g_nslots = 0;            /* 0 = unspecified (SB/format default) */
static uint32_t	g_recov_batch;
static uint64_t	g_pool_capacity;       /* data pool capacity in slots */
static uint32_t	g_seed = 1;
static int	g_only_test = 0;
static int	g_format = 0;
static int	g_check_hugepage = 0;   /* -H: assert hugepage-backed shm */

enum role { ROLE_WORKER = 0, ROLE_LOOKUP = 1 };
static int	g_role = ROLE_WORKER;

/* ---------------- shared-memory segment ---------------- */

/*
 * Check that the Cache stage's buffers are hugepage-backed. UCM gates
 * this with the shm_hugepage_advise config key (madvise on /dev/shm,
 * which must be mounted with huge=advise). Registering a 4K-backed
 * pool would create ~26M IOMMU mappings for 100 GiB; with 2MB pages
 * it is ~50k. Returns 0 when hugepages are (or will be) used, -1 with
 * a diagnostic otherwise.
 */
static int
check_shm_hugepage_advise(void)
{
	FILE *f;
	char line[256];
	unsigned long shmem_huge = 0, shmem_total = 0;
	int mount_advise = 0;

	f = fopen("/proc/meminfo", "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (sscanf(line, "ShmemHugePages: %lu", &shmem_huge) == 1) { continue; }
			if (sscanf(line, "HugePages_Total: %lu", &shmem_total) == 1) { continue; }
		}
		fclose(f);
	}
	f = fopen("/proc/mounts", "r");
	if (f != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (strstr(line, " /dev/shm ") != NULL &&
			    strstr(line, "huge=") != NULL) {
				mount_advise = 1;
				break;
			}
		}
		fclose(f);
	}

	if (!mount_advise) {
		fprintf(stderr, "shm hugepage check: /dev/shm is not mounted "
			"with a huge= option; MADV_HUGEPAGE would be a no-op. "
			"Set shm_hugepage_advise in the UCM config and remount "
			"/dev/shm with huge=advise, or the SPDK store falls "
			"back to per-4K-page registration (~26M mappings at "
			"100 GiB).\n");
		return -1;
	}
	if (shmem_total == 0) {
		fprintf(stderr, "shm hugepage check: no hugepages reserved "
			"(HugePages_Total=0). shm allocations stay 4K-backed.\n");
		return -1;
	}
	printf("shm hugepage check: /dev/shm huge=advise, %lu hugepages "
	       "free pool, ShmemHugePages=%lu in use\n",
	       shmem_total, shmem_huge);
	return 0;
}

static void
shm_lock(void)
{
	int rc = pthread_mutex_lock(&g_shm->lock);

	if (rc == EOWNERDEAD) {
		pthread_mutex_consistent(&g_shm->lock);
		if (g_shm_recovery_owner && g_shm_recovering) {
			g_shm->recovery_required = 0;
		} else {
			g_shm->recovery_required = 1;
			pthread_mutex_unlock(&g_shm->lock);
			fprintf(stderr, "shared index owner died; restart workers to recover from disk\n");
			abort();
		}
	} else if (rc != 0) {
		fprintf(stderr, "shm lock failed (%d)\n", rc);
		abort();
	}
	if (g_shm->recovery_required && !g_shm_recovering) {
		pthread_mutex_unlock(&g_shm->lock);
		fprintf(stderr, "shared index requires recovery; restart workers\n");
		abort();
	}
	if (!g_shm_recovering) {
		for (int i = 0; i < MAX_WORKERS; i++) {
			pid_t pid = g_shm->workers[i];

			if (pid > 0 && pid != getpid() && kill(pid, 0) != 0 && errno == ESRCH) {
				g_shm->recovery_required = 1;
				pthread_mutex_unlock(&g_shm->lock);
				fprintf(stderr, "worker %d died; restart workers to recover shared state\n", pid);
				abort();
			}
		}
	}
}

static void
shm_unlock(void)
{
	pthread_mutex_unlock(&g_shm->lock);
}

static uint32_t
hash_size_for(uint32_t nslots)
{
	uint32_t size = 1024;

	while (size < 2 * nslots) {
		size <<= 1;
	}
	return size;
}

static void shm_close_segment(void);

/* create=1: create (or replace) the segment; create=0: attach existing.
 * adopt_capacity=1 (lookup role): take nslots from the segment header;
 * otherwise the header must match the already-resolved g_nslots. */
static int
shm_open_segment(int create, int adopt_capacity)
{
	uint32_t hash_size;
	int fd;
	struct stat st;

	fd = shm_open(g_shm_name, O_RDWR | (create ? (O_CREAT | O_EXCL) : 0), 0600);
	if (fd < 0) {
		return errno == EEXIST ? -EEXIST : -ENOENT;
	}
	if (create) {
		hash_size = hash_size_for(g_nslots);
		g_shm_size = sizeof(struct shm_header) +
			     (size_t)hash_size * sizeof(struct hash_entry) +
			     (size_t)g_nslots * (sizeof(uint32_t) +
						 sizeof(uint64_t) +
						 sizeof(uint16_t));
		if (ftruncate(fd, (off_t)g_shm_size) != 0) {
			close(fd);
			return -ENOMEM;
		}
	} else if (fstat(fd, &st) != 0 ||
		   (size_t)st.st_size < sizeof(struct shm_header)) {
		close(fd);
		return -EINVAL;
	} else {
		g_shm_size = (size_t)st.st_size;
	}

	g_shm_base = mmap(NULL, g_shm_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if (g_shm_base == MAP_FAILED) {
		g_shm_base = NULL;
		close(fd);
		return -ENOMEM;
	}
	g_shm_fd = fd;
	g_shm = (struct shm_header *)g_shm_base;

	if (create) {
		pthread_mutexattr_t attr;

		memset(g_shm_base, 0, g_shm_size);
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
		pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
		pthread_mutex_init(&g_shm->lock, &attr);
		pthread_mutexattr_destroy(&attr);
		g_shm->magic = SHM_MAGIC;
		g_shm->version = SHM_VERSION;
		g_shm->nslots = g_nslots;
		g_shm->hash_size = hash_size;
		g_shm_created = 1;
		return 0;
	}
	{
		uint32_t n = g_shm->nslots;
		size_t expect = sizeof(struct shm_header) +
				(size_t)hash_size_for(n) * sizeof(struct hash_entry) +
				(size_t)n * (sizeof(uint32_t) + sizeof(uint64_t) +
					     sizeof(uint16_t));

		if (g_shm->magic != SHM_MAGIC || g_shm->version != SHM_VERSION ||
		    g_shm->hash_size != hash_size_for(n) ||
		    g_shm_size != expect ||
		    (!adopt_capacity && n != g_nslots)) {
			munmap(g_shm_base, g_shm_size);
			g_shm_base = NULL;
			g_shm = NULL;
			int can_replace = g_shm_recovery_owner;

			close(g_shm_fd);
			g_shm_fd = -1;
			if (can_replace) { shm_unlink(g_shm_name); }
			return -EINVAL;
		}
		if (adopt_capacity) {
			g_nslots = n;
		}
		if (g_shm->recovery_required && !g_shm_recovery_owner) {
			shm_close_segment();
			return -EBUSY;
		}
	}
	return 0;
}

static void
shm_recovery_done(void)
{
	if (g_shm_recovery_owner && g_lease_fd >= 0) {
		g_shm->recovery_required = 0;
		flock(g_lease_fd, LOCK_SH);
		g_shm_recovery_owner = 0;
	}
}

static int
shm_register_worker(void)
{
	pid_t pid = getpid();

	shm_lock();
	for (int i = 0; i < MAX_WORKERS; i++) {
		if (g_shm->workers[i] == 0 || g_shm->workers[i] == pid) {
			g_shm->workers[i] = pid;
			g_registered_pid = pid;
			shm_unlock();
			return 0;
		}
	}
	shm_unlock();
	return -ENOSPC;
}

static void
shm_unregister_worker(void)
{
	if (g_shm == NULL || g_registered_pid != getpid()) {
		return;
	}
	shm_lock();
	for (int i = 0; i < MAX_WORKERS; i++) {
		if (g_shm->workers[i] == g_registered_pid) {
			g_shm->workers[i] = 0;
			break;
		}
	}
	shm_unlock();
	g_registered_pid = 0;
}

static void
shm_close_segment(void)
{
	shm_unregister_worker();
	if (g_shm_base != NULL) {
		munmap(g_shm_base, g_shm_size);
		g_shm_base = NULL;
		g_shm = NULL;
	}
	g_shm_created = 0;
	if (g_shm_fd >= 0) {
		close(g_shm_fd);
		g_shm_fd = -1;
	}
	g_shm_recovering = 0;
}

/* ---------------- hashing ---------------- */

static uint32_t
hash_fn16(const uint8_t *id)
{
	uint32_t h = 2166136261u;

	for (int i = 0; i < 16; i++) {
		h ^= id[i];
		h *= 16777619u;
	}
	return h;
}

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

static void
build_superblock(struct superblock *sb)
{
	memset(sb, 0, g_sector_size);
	sb->magic = SB_MAGIC;
	sb->version = SB_VERSION;
	sb->index_slots = g_index_slots;
	sb->nslots = g_nslots;
	sb->slot_sectors = g_slot_sectors;
	sb->shard_sectors = g_shard_sectors;
	sb->checksum = (uint64_t)hash_fn_n(sb, SB_COVER);
}

static struct hash_entry *
hash_find(const uint8_t id[16])
{
	uint32_t i = hash_fn16(id) & g_hash_mask;

	while (g_hash[i].used != 0) {
		if (g_hash[i].used == 1 &&
		    memcmp(g_hash[i].id, id, 16) == 0) {
			return &g_hash[i];
		}
		i = (i + 1) & g_hash_mask;
	}
	return NULL;
}

static void
hash_insert(const uint8_t id[16], uint32_t slot, uint64_t epoch)
{
	uint32_t i = hash_fn16(id) & g_hash_mask;

	while (g_hash[i].used != 0) {
		if (memcmp(g_hash[i].id, id, 16) == 0) {
			g_hash[i].slot = slot;
			g_hash[i].epoch = epoch;
			return;
		}
		i = (i + 1) & g_hash_mask;
	}
	memcpy(g_hash[i].id, id, 16);
	g_hash[i].slot = slot;
	g_hash[i].epoch = epoch;
	g_hash[i].used = 1;
	g_hash_count++;
}

/*
 * Backward-shift deletion (Knuth algorithm R): fill the hole with later
 * chain members whose natural home lies at or before the hole, so every
 * survivor stays findable and no tombstone is ever created. The scan is
 * bounded: occupancy is at most nslots of a 2*nslots table, so an empty
 * slot always terminates it.
 */
static void
hash_delete_at(struct hash_entry *e)
{
	uint32_t i = (uint32_t)(e - g_hash);
	uint32_t j = (i + 1) & g_hash_mask;

	g_hash_count--;
	while (g_hash[j].used == 1) {
		uint32_t home = hash_fn16(g_hash[j].id) & g_hash_mask;

		/* entry j may fill hole i iff i lies on its probe path */
		if (((i - home) & g_hash_mask) < ((j - home) & g_hash_mask)) {
			g_hash[i] = g_hash[j];
			i = j;
		}
		j = (j + 1) & g_hash_mask;
	}
	g_hash[i].used = 0;
}

static void
hash_delete(const uint8_t id[16])
{
	struct hash_entry *e = hash_find(id);

	if (e != NULL) {
		hash_delete_at(e);
	}
}

/* ---------------- geometry ---------------- */

static uint64_t
sb_lba(void)
{
	return g_data_base;
}

static uint64_t
slot_lba(uint32_t slot)
{
	return g_pool_base + (uint64_t)slot * g_slot_sectors;
}

static uint64_t
shard_lba(uint32_t slot, uint32_t shard)
{
	return slot_lba(slot) + (uint64_t)shard * g_shard_sectors;
}

static uint64_t
record_lba(uint32_t slot)
{
	return g_index_base + slot;
}

static size_t
shard_bytes(void)
{
	return (size_t)g_shard_sectors * g_sector_size;
}

static size_t
slot_bytes(void)
{
	return (size_t)g_slot_sectors * g_sector_size;
}

/* ---------------- buffers ---------------- */

static int
io_buf_pop(void)
{
	if (g_io_free_top == 0) {
		return -1;
	}
	return (int)g_io_free[--g_io_free_top];
}

static void
io_buf_push(uint32_t idx)
{
	g_io_free[g_io_free_top++] = idx;
}

static int
idx_buf_pop(void)
{
	if (g_idx_free_top == 0) {
		return -1;
	}
	return (int)g_idx_free[--g_idx_free_top];
}

static void
idx_buf_push(uint32_t idx)
{
	g_idx_free[g_idx_free_top++] = idx;
}

/* ---------------- record write path ---------------- */

static void
build_record(struct index_record *rec, uint32_t slot, const uint8_t id[16],
	     uint64_t epoch, uint32_t flags)
{
	memset(rec, 0, g_sector_size);
	rec->magic = STORE_MAGIC;
	rec->epoch = epoch;
	rec->slot = slot;
	rec->flags = flags;
	memcpy(rec->block_id, id, 16);
	rec->checksum = (uint64_t)hash_fn_n(rec, RECORD_COVER);
}

static int
submit_record_write(uint32_t slot, const uint8_t id[16], uint64_t epoch,
		    uint32_t flags, void *buf, void *cb_arg,
		    void (*cb)(void *, const struct spdk_nvme_cpl *))
{
	build_record(buf, slot, id, epoch, flags);
	return spdk_nvme_ns_cmd_write(g_ns, g_qpair, buf, record_lba(slot), 1,
				      cb, cb_arg, 0);
}

static void
rec_op_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct rec_op *op = arg;

	g_last_progress = spdk_get_ticks();
	idx_buf_push((uint32_t)op->buf);
	op->buf = -1;
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_err_count++;
		g_healthy = 0;
		op->active = 0;
	} else if (op->free_on_done) {
		shm_lock();
		if (__atomic_load_n(&g_pins[op->slot], __ATOMIC_RELAXED) == 0) {
			g_freelist[g_free_top++] = op->slot;
			op->active = 0;
		} else {
			op->waiting_pin = 1;
		}
		shm_unlock();
	} else {
		op->active = 0;
	}
}

static void
advance_retired(void)
{
	for (int i = 0; i < NUM_REC_OPS; i++) {
		struct rec_op *op = &g_rops[i];

		if (!op->active || !op->waiting_pin) {
			continue;
		}
		shm_lock();
		if (__atomic_load_n(&g_pins[op->slot], __ATOMIC_RELAXED) == 0) {
			g_freelist[g_free_top++] = op->slot;
			op->waiting_pin = 0;
			op->active = 0;
		}
		shm_unlock();
	}
}

/*
 * Submit a DELETED record for slot. The epoch matters: an overwrite's
 * old-slot invalidation must reuse the new commit's epoch (a higher one
 * would suppress the id at recovery; recovery breaks epoch ties in
 * favor of non-deleted), while a real delete takes ++g_epoch so it can
 * never lose to the last live record.
 */
static int
submit_invalidation(const uint8_t id[16], uint32_t slot, uint64_t epoch)
{
	for (int i = 0; i < NUM_REC_OPS; i++) {
		if (!g_rops[i].active) {
			int buf = idx_buf_pop();

			if (buf < 0) {
				return -1;
			}
			g_rops[i].active = 1;
			g_rops[i].waiting_pin = 0;
			g_rops[i].slot = slot;
			g_rops[i].free_on_done = 1;
			g_rops[i].buf = buf;
			if (submit_record_write(slot, id, epoch,
						FLAG_DELETED, g_idx_bufs[buf],
						&g_rops[i], rec_op_done) < 0) {
				idx_buf_push((uint32_t)buf);
				g_rops[i].active = 0;
				return -1;
			}
			return 0;
		}
	}
	return -1;
}

static void
queue_invalidation(const uint8_t id[16], uint32_t slot, uint64_t epoch)
{
	if (g_pinv_n < MAX_PINV) {
		memcpy(g_pinv[g_pinv_n].id, id, 16);
		g_pinv[g_pinv_n].slot = slot;
		g_pinv[g_pinv_n].epoch = epoch;
		g_pinv_n++;
	} else {
		/* The queue is sized for every dump context plus one GC pass. */
		g_err_count++;
		g_healthy = 0;
	}
}

static void
advance_invs(void)
{
	for (uint32_t i = 0; i < g_pinv_n; ) {
		if (submit_invalidation(g_pinv[i].id, g_pinv[i].slot,
					g_pinv[i].epoch) == 0) {
			g_pinv[i] = g_pinv[--g_pinv_n];
		} else {
			i++;
		}
	}
}

/* ---------------- store operations ---------------- */

int
store_lookup(const uint8_t id[16])
{
	struct hash_entry *e;
	int r;

	shm_lock();
	e = hash_find(id);
	if (e != NULL) {
		g_hotness[e->slot] = now_ms();
		r = 1;
	} else {
		r = 0;
	}
	shm_unlock();
	return r;
}

uint32_t
store_lookup_prefix(const uint8_t ids[][16], uint32_t n)
{
	uint32_t i;
	uint64_t now = now_ms();

	shm_lock();
	for (i = 0; i < n; i++) {
		struct hash_entry *e = hash_find(ids[i]);

		if (e == NULL) {
			break;
		}
		g_hotness[e->slot] = now;
	}
	shm_unlock();
	return i;
}

static struct dump_ctx *
find_active_dump(const uint8_t id[16])
{
	for (int i = 0; i < MAX_DUMP_CTX; i++) {
		if (g_dctx[i].active && memcmp(g_dctx[i].id, id, 16) == 0) {
			return &g_dctx[i];
		}
	}
	return NULL;
}

/*
 * Submit a dump task covering shards [first, first+count) of block id.
 * The block becomes visible only when the slot's coverage is complete.
 * data must hold this segment's `count` shards contiguously (a caller
 * continuing a block passes data + first * shard_bytes()) and stays
 * valid until the commit callback. A later submit merges into the same
 * ctx. Every accepted segment owns an independent callback which is
 * signaled exactly once when the block commits or the ctx fails.
 */
static int store_gc(int max_victims);
static void dump_finish(struct dump_ctx *dc, int status);

/*
 * Reactor-side dump handler: runs on the reactor thread only.
 * Returns 0 = ctx registered (completion via ctx cb), -EINVAL/-EBUSY/
 * -ENOMEM = rejected, 1 = parked in the ENOSPC pending list (the
 * reactor loop runs GC once and retries allocation as invalidations
 * free slots; -ENOSPC only when fully drained).
 */
static int
reactor_do_dump(struct store_op *op)
{
	struct dump_ctx *dc = find_active_dump(op->id);
	struct dump_req *req;
	struct hash_entry *e;
	uint32_t slot, old_slot;
	uint32_t first = op->first, count = op->count;

	if (op->data == NULL || count == 0 || first >= g_nshards ||
	    count > g_nshards - first) {
		return -EINVAL;
	}
	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->data = op->data;
	req->first = first;
	req->count = count;
	req->cb = op->cb;
	req->cb_arg = op->cb_arg;
	if (dc != NULL) {
		if (dc->commit_phase != 0 || dc->failed) {
			free(req);
			return -EBUSY;
		}
		if (dc->req_tail != NULL) {
			dc->req_tail->next = req;
		} else {
			dc->req_head = req;
		}
		dc->req_tail = req;
		return 0;
	}

	for (int i = 0; i < MAX_DUMP_CTX; i++) {
		if (!g_dctx[i].active) {
			dc = &g_dctx[i];
			break;
		}
	}
	if (dc == NULL) {
		free(req);
		return -EBUSY;
	}

	shm_lock();
	if (g_free_top == 0) {
		shm_unlock();
		free(req);
		/* ENOSPC handling lives in the reactor loop: park the op */
		return 1;
	}
	slot = g_freelist[--g_free_top];
	e = hash_find(op->id);
	old_slot = (e != NULL) ? e->slot : INVALID_SLOT;
	if (old_slot != INVALID_SLOT) {
		__atomic_fetch_add(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
	}
	shm_unlock();

	memset(dc, 0, sizeof(*dc));
	memcpy(dc->id, op->id, 16);
	dc->active = 1;
	dc->slot = slot;
	dc->nshards = g_nshards;
	dc->covered = calloc(g_nshards, 1);
	if (dc->covered == NULL) {
		shm_lock();
		g_freelist[g_free_top++] = slot;
		if (old_slot != INVALID_SLOT) {
			__atomic_fetch_sub(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
		}
		shm_unlock();
		dc->active = 0;
		free(req);
		return -ENOMEM;
	}
	dc->rec_buf = -1;
	dc->old_slot = old_slot;
	dc->old_pinned = old_slot != INVALID_SLOT;
	dc->req_head = req;
	dc->req_tail = req;
	return 0;
}

static void
dump_finish(struct dump_ctx *dc, int status)
{
	struct dump_req *req = dc->req_head;

	dc->active = 0;
	if (status != 0) {
		/* the commit record never became durable: reclaim the
		 * slot exactly (its previous record is already invalid) */
		shm_lock();
		g_freelist[g_free_top++] = dc->slot;
		shm_unlock();
	}
	if (dc->old_pinned) {
		__atomic_fetch_sub(&g_pins[dc->old_slot], 1, __ATOMIC_RELAXED);
		dc->old_pinned = 0;
	}
	while (req != NULL) {
		struct dump_req *next = req->next;

		if (req->cb != NULL) {
			req->cb(req->cb_arg, status);
		}
		free(req);
		req = next;
	}
	dc->req_head = NULL;
	dc->req_tail = NULL;
	free(dc->covered);
	dc->covered = NULL;
}

static void
dump_record_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct dump_ctx *dc = arg;
	int failed = 0;
	uint32_t replaced_slot = dc->old_slot;

	g_last_progress = spdk_get_ticks();
	idx_buf_push((uint32_t)dc->rec_buf);
	dc->rec_buf = -1;

	if (spdk_nvme_cpl_is_error(cpl)) {
		failed = 1;
		g_err_count++;
		g_healthy = 0;
	} else {
		shm_lock();
		{
			struct hash_entry *current = hash_find(dc->id);

			/* Same-ID transactions may complete out of order. The higher
			 * durable epoch wins both here and during recovery. */
			if (current != NULL && current->slot != dc->old_slot) {
				if (current->epoch >= dc->rec_epoch) {
					failed = 1;
				} else {
					replaced_slot = current->slot;
					hash_insert(dc->id, dc->slot, dc->rec_epoch);
				}
			} else if (dc->old_slot != INVALID_SLOT && current == NULL) {
				/* Delete/GC cannot remove a pinned old slot. */
				failed = 1;
			} else {
				hash_insert(dc->id, dc->slot, dc->rec_epoch);
			}
		}
		if (!failed && dc->old_slot == INVALID_SLOT && replaced_slot == INVALID_SLOT) {
			g_live_count++;
		}
		if (!failed) {
			g_hotness[dc->slot] = now_ms();
		}
		shm_unlock();
		if (!failed && replaced_slot != INVALID_SLOT) {
			if (submit_invalidation(dc->id, replaced_slot,
						dc->rec_epoch) != 0) {
				queue_invalidation(dc->id, replaced_slot,
						   dc->rec_epoch);
			}
		}
	}
	dump_finish(dc, failed ? -EIO : 0);
}

static void
dump_shard_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct shard_io *io = arg;
	struct dump_ctx *dc = io->dc;
	struct dump_req *req = io->dr;

	g_last_progress = spdk_get_ticks();
	io_buf_push(io->buf);   /* release the concurrency token */
	dc->outstanding--;
	req->outstanding--;
	req->acked++;
	if (spdk_nvme_cpl_is_error(cpl)) {
		dc->failed = 1;
		g_err_count++;
		g_healthy = 0;
	} else {
		dc->covered[io->shard] = 1;
	}
}

static void
complete_partial_dump_requests(struct dump_ctx *dc)
{
	struct dump_req **link = &dc->req_head;

	while (*link != NULL) {
		struct dump_req *req = *link;

		if (req->cursor != req->count || req->outstanding != 0) {
			link = &req->next;
			continue;
		}
		*link = req->next;
		if (dc->req_tail == req) {
			dc->req_tail = dc->req_head;
			while (dc->req_tail != NULL && dc->req_tail->next != NULL) {
				dc->req_tail = dc->req_tail->next;
			}
		}
		if (req->cb != NULL) {
			req->cb(req->cb_arg, 0);
		}
		free(req);
	}
}

static void
load_shard_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct shard_io *io = arg;
	struct load_ctx *lc = io->lc;

	g_last_progress = spdk_get_ticks();
	io_buf_push(io->buf);   /* release the concurrency token */
	lc->acked++;
	lc->outstanding--;
	if (spdk_nvme_cpl_is_error(cpl)) {
		lc->failed = 1;
		g_err_count++;
		g_healthy = 0;
	} else if (io->bounce) {
		/* fallback path: copy from bounce buffer */
		memcpy((uint8_t *)lc->dst +
		       (size_t)io->offset * shard_bytes(),
		       g_io_bufs[io->buf], shard_bytes());
	}
	/* direct path: NVMe DMA already wrote into lc->dst */
}

/* Reactor-side load handler: 0 = ctx registered, negative = rejected. */
static int
reactor_do_load(struct store_op *op)
{
	struct hash_entry *e;
	struct load_ctx *lc = NULL;
	int rc = -ENOENT;

	shm_lock();
	e = hash_find(op->id);
	if (e != NULL) {
		rc = -EINVAL;
		if (op->dst != NULL && op->count != 0 && op->first < g_nshards &&
		    op->count <= g_nshards - op->first &&
		    op->len >= (uint64_t)op->count * shard_bytes()) {
			rc = -EBUSY;
			for (int i = 0; i < MAX_LOAD_CTX; i++) {
				if (!g_lctx[i].active) {
					lc = &g_lctx[i];
					break;
				}
			}
			if (lc != NULL) {
				/* pin under the lock so GC cannot pick the
				 * slot between find and pin */
				__atomic_fetch_add(&g_pins[e->slot], 1,
						   __ATOMIC_RELAXED);
				g_hotness[e->slot] = now_ms();
				memset(lc, 0, sizeof(*lc));
				lc->active = 1;
				lc->slot = e->slot;
				lc->first = op->first;
				lc->count = op->count;
				lc->dst = op->dst;
				lc->cb = op->cb;
				lc->cb_arg = op->cb_arg;
				rc = 0;
			}
		}
	}
	shm_unlock();
	return rc;
}

/*
 * Evict up to max_victims coldest unpinned blocks through the normal
 * delete path (invalidation record + exact free-list push when the
 * record completes). Victim selection is one pure-memory scan under a
 * single lock hold; invalidations are submitted after unlock.
 * Production note: the walk is O(hash_size) — fine at prototype scale;
 * a large table wants bounded sampling (the posixGcShardSampleRatio
 * analog).
 */
static int
store_gc(int max_victims)
{
	struct {
		uint8_t		id[16];
		uint32_t	slot;
		uint64_t	epoch;
		uint64_t	hot;
	} vics[GC_MAX_PASS];
	int nvics = 0;

	if (max_victims > GC_MAX_PASS) {
		max_victims = GC_MAX_PASS;
	}

	shm_lock();
	for (uint32_t h = 0; h <= g_hash_mask; h++) {
		struct hash_entry *e = &g_hash[h];
		uint64_t hot;
		int k;

		if (e->used != 1) {
			continue;
		}
		if (__atomic_load_n(&g_pins[e->slot], __ATOMIC_RELAXED) != 0) {
			continue;
		}
		hot = g_hotness[e->slot];
		if (nvics < max_victims) {
			k = nvics++;
		} else if (hot < vics[nvics - 1].hot) {
			k = nvics - 1;
		} else {
			continue;
		}
		while (k > 0 && vics[k - 1].hot > hot) {
			vics[k] = vics[k - 1];
			k--;
		}
		memcpy(vics[k].id, e->id, 16);
		vics[k].slot = e->slot;
		vics[k].hot = hot;
	}
	for (int i = 0; i < nvics; i++) {
		vics[i].epoch = ++g_epoch;
		hash_delete(vics[i].id);
		g_live_count--;
	}
	shm_unlock();

	for (int i = 0; i < nvics; i++) {
		if (submit_invalidation(vics[i].id, vics[i].slot,
					vics[i].epoch) != 0) {
			queue_invalidation(vics[i].id, vics[i].slot,
					   vics[i].epoch);
		}
	}
	g_gc_passes++;
	g_gc_evicted += (uint64_t)nvics;
	return nvics;
}

/*
 * Fire-and-forget delete: visibility drops immediately, the invalidation
 * record completes asynchronously via store_poll().
 * Returns 1 deleted, 0 absent, negative on error.
 */
static int
reactor_do_delete(const uint8_t id[16])
{
	struct hash_entry *e;
	uint32_t slot;
	uint64_t epoch;
	int rc;

	shm_lock();
	e = hash_find(id);
	if (e == NULL) {
		shm_unlock();
		return 0;
	}
	slot = e->slot;
	if (__atomic_load_n(&g_pins[slot], __ATOMIC_RELAXED) != 0) {
		shm_unlock();
		return -EBUSY;
	}
	epoch = ++g_epoch;
	shm_unlock();

	/* Submit the invalidation first: on failure nothing changed and
	 * the id stays visible (recovery would resurrect it otherwise). */
	rc = submit_invalidation(id, slot, epoch);
	if (rc != 0) {
		return -EBUSY;
	}
	shm_lock();
	hash_delete(id);
	g_live_count--;
	shm_unlock();
	return 1;
}

static int ensure_registered(const void *addr, size_t len);

static void
advance_dumps(void)
{
	for (int i = 0; i < MAX_DUMP_CTX; i++) {
		struct dump_ctx *dc = &g_dctx[i];
		int all_reqs_issued = 1;

		if (!dc->active) {
			continue;
		}

		for (struct dump_req *req = dc->req_head;
		     req != NULL && !dc->failed; req = req->next) {

			while (req->cursor < req->count) {
				int tok = io_buf_pop();
				uint32_t shard;
				const void *src;
				int rc, reg_rc;

				if (tok < 0) {
					break;
				}
				shard = req->first + req->cursor;
				src = (const uint8_t *)req->data +
				      (size_t)req->cursor * shard_bytes();

				g_sios[tok].dc = dc;
				g_sios[tok].dr = req;
				g_sios[tok].lc = NULL;
				g_sios[tok].buf = (uint32_t)tok;
				g_sios[tok].shard = shard;
				g_sios[tok].offset = req->cursor;

				reg_rc = ensure_registered(src, shard_bytes());
				if (reg_rc == 0) {
					/* direct DMA to caller's buffer */
					g_sios[tok].bounce = 0;
					rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair,
								    src,
								    shard_lba(dc->slot, shard),
								    g_shard_sectors,
								    dump_shard_done,
								    &g_sios[tok], 0);
				} else if (reg_rc == 1) {
					/* fallback: bounce via internal buffer */
					g_sios[tok].bounce = 1;
					memcpy(g_io_bufs[tok], src, shard_bytes());
					rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair,
								    g_io_bufs[tok],
								    shard_lba(dc->slot, shard),
								    g_shard_sectors,
								    dump_shard_done,
								    &g_sios[tok], 0);
				} else {
					rc = reg_rc;
				}
				if (rc < 0) {
					io_buf_push((uint32_t)tok);
					dc->failed = 1;
					g_err_count++;
					g_healthy = 0;
					break;
				}
				req->cursor++;
				req->outstanding++;
				dc->outstanding++;
			}
			if (req->cursor < req->count) {
				all_reqs_issued = 0;
			}
		}

		if (dc->failed) {
			/* Drain in-flight IO, then fail the ctx. */
			if (dc->outstanding == 0) {
				dump_finish(dc, -EIO);
			}
			continue;
		}

		if (dc->commit_phase == 0 && all_reqs_issued &&
		    dc->outstanding == 0) {
			uint32_t done = 0;

			for (uint32_t s = 0; s < dc->nshards; s++) {
				done += dc->covered[s];
			}
			if (done == dc->nshards) {
				dc->commit_phase = 1;
			} else {
				complete_partial_dump_requests(dc);
			}
		}

		if (dc->commit_phase == 1 && dc->rec_buf < 0) {
			int buf = idx_buf_pop();
			int rc;

			if (buf < 0) {
				continue;
			}
			dc->rec_buf = buf;
			shm_lock();
			dc->rec_epoch = ++g_epoch;
			shm_unlock();
			rc = submit_record_write(dc->slot, dc->id, dc->rec_epoch,
						 0, g_idx_bufs[buf], dc,
						 dump_record_done);
			if (rc < 0) {
				idx_buf_push((uint32_t)buf);
				dc->rec_buf = -1;
				dc->failed = 1;
				g_err_count++;
				g_healthy = 0;
				dump_finish(dc, -EIO);
			}
		}
	}
}

static void
advance_loads(void)
{
	for (int i = 0; i < MAX_LOAD_CTX; i++) {
		struct load_ctx *lc = &g_lctx[i];

		if (!lc->active) {
			continue;
		}
		while (!lc->failed && lc->submitted < lc->count) {
			int tok = io_buf_pop();
			uint32_t offset = lc->submitted;
			uint32_t shard = lc->first + offset;
			void *dst;
			int rc, reg_rc;

			if (tok < 0) {
				break;
			}
			dst = (uint8_t *)lc->dst + (size_t)offset * shard_bytes();

			g_sios[tok].dc = NULL;
			g_sios[tok].dr = NULL;
			g_sios[tok].lc = lc;
			g_sios[tok].buf = (uint32_t)tok;
			g_sios[tok].shard = shard;
			g_sios[tok].offset = offset;

			reg_rc = ensure_registered(dst, shard_bytes());
			if (reg_rc == 0) {
				/* direct DMA into caller's buffer */
				g_sios[tok].bounce = 0;
				rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, dst,
							    shard_lba(lc->slot, shard),
							    g_shard_sectors,
							    load_shard_done,
							    &g_sios[tok], 0);
			} else if (reg_rc == 1) {
				/* fallback: bounce via internal buffer */
				g_sios[tok].bounce = 1;
				rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, g_io_bufs[tok],
							    shard_lba(lc->slot, shard),
							    g_shard_sectors,
							    load_shard_done,
							    &g_sios[tok], 0);
			} else {
				rc = reg_rc;
			}
			if (rc < 0) {
				io_buf_push((uint32_t)tok);
				lc->failed = 1;
				g_err_count++;
				g_healthy = 0;
				break;
			}
			lc->submitted++;
			lc->outstanding++;
		}
		if ((!lc->failed && lc->acked == lc->count &&
		     lc->submitted == lc->count) ||
		    (lc->failed && lc->outstanding == 0)) {
			lc->active = 0;
			__atomic_fetch_sub(&g_pins[lc->slot], 1, __ATOMIC_RELAXED);
			if (lc->cb != NULL) {
				lc->cb(lc->cb_arg, lc->failed ? -EIO : 0);
			}
		}
	}
}

static void
abort_stalled_io(void)
{
	for (int i = 0; i < MAX_DUMP_CTX; i++) {
		if (g_dctx[i].active) {
			g_dctx[i].failed = 1;
		}
	}
	for (int i = 0; i < MAX_LOAD_CTX; i++) {
		if (g_lctx[i].active) {
			g_lctx[i].failed = 1;
		}
	}
	/* Queued invalidations can no longer become durable. Recovery keeps
	 * their slots conservative; clear the queue so teardown can finish. */
	g_pinv_n = 0;
	if (g_qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
		g_qpair = NULL;
	}
}

static void
check_stall(void)
{
	static int had_inflight;
	uint64_t now = spdk_get_ticks();
	int inflight = g_io_free_top != IO_POOL_BUFS ||
		       g_idx_free_top != NUM_IDX_BUFS;

	if (!inflight) {
		had_inflight = 0;
		return;
	}
	if (!had_inflight) {
		had_inflight = 1;
		g_last_progress = now;
		return;
	}

	if (!g_stalled &&
	    now - g_last_progress > 30ull * spdk_get_ticks_hz()) {
		fprintf(stderr, "no IO completion for 30s, connection lost? aborting\n");
		g_err_count++;
		g_stalled = 1;
		g_healthy = 0;
		abort_stalled_io();
	}
}

/* One reactor step: completions + submissions + watchdog. Runs on the
 * reactor thread while live, on the main thread while paused (init,
 * recovery, tests with direct disk access). */
static void
store_poll(void)
{
	if (g_qpair != NULL) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}
	spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	advance_dumps();
	advance_loads();
	advance_retired();
	if (!g_stalled) {
		advance_invs();
		check_stall();
	}
}

/* True when every in-flight IO, record op and queued invalidation has
 * drained (partial dumps that have not committed hold no buffers). */
static int
store_io_drained(void)
{
	return g_io_free_top == IO_POOL_BUFS &&
	       g_idx_free_top == NUM_IDX_BUFS &&
	       g_pinv_n == 0;
}

static int
store_drained(void)
{
	return store_io_drained() && g_pending_n == 0;
}

/* ---------------- SHM registration (direct DMA, no bounce copy) ------------- */

void reactor_pause(void);
void reactor_resume(void);
int store_quiesce(void);
static int store_sync_op(int type);

/*
 * The Cache stage explicitly registers its complete SHM data region once
 * Setup has mmap'ed it.  NVMe DMA then goes directly to/from TaskDesc
 * addresses.  2MB backing (shm_hugepage_advise) reduces IOMMU mappings;
 * 4KB backing remains functionally valid, only more expensive.
 */

#define REG_PAGE_SIZE   4096ULL
#define REG_CACHE_MAX   16                          /* SHM pools per process */

struct reg_range {
	uint64_t start;                                 /* 4KB-aligned */
	uint64_t end;
	uint32_t refs;
	uint8_t owned;                                  /* unregister on fini */
};

static struct reg_range g_reg_cache[REG_CACHE_MAX];
static uint32_t g_reg_count;
static uint64_t g_reg_total_bytes;
static int g_direct_dma;   /* .so sets to 1 when buffers are SHM/hugepage */

/* Check if [addr, addr+len) is fully within a registered range. */
static int
is_registered(uint64_t addr, size_t len)
{
	uint64_t end = addr + len;

	for (uint32_t i = 0; i < g_reg_count; i++) {
		if (addr >= g_reg_cache[i].start && end <= g_reg_cache[i].end) {
			return 1;
		}
	}
	return 0;
}

static int
range_vtophys_valid(uint64_t addr, uint64_t len)
{
	while (len > 0) {
		uint64_t chunk = len;

		if (spdk_vtophys((void *)(uintptr_t)addr, &chunk) == SPDK_VTOPHYS_ERROR ||
		    chunk == 0) {
			return 0;
		}
		addr += chunk;
		len -= chunk;
	}
	return 1;
}

int
store_register_memory(void *addr, uint64_t size)
{
	uint64_t start, end;
	int rc;

	if (addr == NULL || size == 0) {
		return -EINVAL;
	}
	start = (uint64_t)(uintptr_t)addr & ~(REG_PAGE_SIZE - 1);
	end = ((uint64_t)(uintptr_t)addr + size + REG_PAGE_SIZE - 1)
	      & ~(REG_PAGE_SIZE - 1);

	for (uint32_t i = 0; i < g_reg_count; i++) {
		if (start == g_reg_cache[i].start && end == g_reg_cache[i].end) {
			g_reg_cache[i].refs++;
			g_direct_dma = 1;
			return 0;
		}
	}
	if (g_reg_count == REG_CACHE_MAX) {
		return -ENOSPC;
	}

	/* Reserve avoids allocation in map notification callbacks.  Park the
	 * reactor while translation maps change. */
	reactor_pause();
	rc = spdk_mem_reserve((void *)start, end - start);
	if (rc != 0 && rc != -EBUSY) {
		reactor_resume();
		return rc;
	}
	rc = spdk_mem_register((void *)start, end - start);
	if (rc == -EBUSY && range_vtophys_valid(start, end - start)) {
		/* Another subsystem already registered the full range; cache it
		 * as borrowed and never unregister it from here. */
		g_reg_cache[g_reg_count].owned = 0;
	} else if (rc != 0) {
		reactor_resume();
		return rc;
	} else {
		g_reg_cache[g_reg_count].owned = 1;
	}
	if (g_trid.trtype == SPDK_NVME_TRANSPORT_PCIE &&
	    !range_vtophys_valid(start, end - start)) {
		if (g_reg_cache[g_reg_count].owned) {
			spdk_mem_unregister((void *)start, end - start);
		}
		reactor_resume();
		return -ENOTSUP;
	}
	reactor_resume();
	g_reg_cache[g_reg_count].start = start;
	g_reg_cache[g_reg_count].end = end;
	g_reg_cache[g_reg_count].refs = 1;
	g_reg_count++;
	g_reg_total_bytes += end - start;
	g_direct_dma = 1;
	return 0;
}

int
store_unregister_memory(void *addr, uint64_t size)
{
	uint64_t start, end;

	if (addr == NULL || size == 0) {
		return -EINVAL;
	}
	start = (uint64_t)(uintptr_t)addr & ~(REG_PAGE_SIZE - 1);
	end = ((uint64_t)(uintptr_t)addr + size + REG_PAGE_SIZE - 1)
	      & ~(REG_PAGE_SIZE - 1);

	/* No new Cache work reaches the backend at this point. Cancel partial
	 * blocks that cannot commit without future submissions, then fence all
	 * callbacks and DMA before unregistering the range. */
	{
		int rc = store_sync_op(OP_CANCEL_PARTIAL);

		if (rc != 0 && !store_io_drained()) {
			return rc;
		}
	}
	for (uint32_t i = 0; i < g_reg_count; i++) {
		if (start != g_reg_cache[i].start || end != g_reg_cache[i].end) {
			continue;
		}
		if (--g_reg_cache[i].refs > 0) {
			return 0;
		}
		if (g_reg_cache[i].owned) {
			reactor_pause();
			int rc = spdk_mem_unregister((void *)start, end - start);
			reactor_resume();

			if (rc != 0) {
				g_reg_cache[i].refs = 1;
				return rc;
			}
		}
		g_reg_total_bytes -= end - start;
		g_reg_cache[i] = g_reg_cache[--g_reg_count];
		g_direct_dma = g_reg_count > 0;
		return 0;
	}
	return -ENOENT;
}

static int
ensure_registered(const void *addr, size_t len)
{
	if (!g_direct_dma) {
		return 1;            /* standalone tests use bounce buffers */
	}
	return is_registered((uint64_t)(uintptr_t)addr, len) ? 0 : -EFAULT;
}

/* ---------------- M2: reactor thread + op ring ---------------- */




#define OP_RING_SIZE    4096    /* power of 2 for spdk_ring */
#define MAX_PENDING     OP_RING_SIZE /* reactor-side backpressure */

static struct spdk_ring	*g_op_ring;
static pthread_t	g_reactor_tid;
static volatile int	g_reactor_stop;
static volatile int	g_reactor_pause_req;
static volatile int	g_reactor_paused;       /* set by reactor when parked */
static int		g_reactor_started;
static pthread_mutex_t	g_reactor_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	g_reactor_paused_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t	g_reactor_resume_cv = PTHREAD_COND_INITIALIZER;

/* parked ops owned by the reactor */
static struct store_op *g_pending[MAX_PENDING];
static struct store_op *g_barriers[MAX_PENDING];
static uint32_t		g_barriers_n;

static void
op_complete(struct store_op *op, int rc)
{
	if (op->cb != NULL) {
		op->cb(op->cb_arg, rc);
	}
	if (op->sw != NULL) {
		syncw_signal(op->sw, rc);
	}
}

static void
process_op(struct store_op *op)
{
	if (g_stalled && op->type != OP_BARRIER && op->type != OP_CANCEL_PARTIAL) {
		op_complete(op, -EIO);
		free(op);
		return;
	}
	switch (op->type) {
	case OP_DUMP: {
		int rc = reactor_do_dump(op);

		if (rc == 1 || rc == -EBUSY) {
			/* No slot or context yet: apply reactor-side backpressure. */
			op->gc_attempted = 0;
			if (g_pending_n < MAX_PENDING) {
				g_pending[g_pending_n++] = op;
				return;
			}
			op_complete(op, rc == 1 ? -ENOSPC : -EBUSY);
		} else if (rc != 0) {
			op_complete(op, rc);
		}
		/* rc == 0: ctx registered; completion via ctx cb, op freed */
		break;
	}
	case OP_LOAD: {
		int rc = reactor_do_load(op);

		if (rc == -EBUSY && g_pending_n < MAX_PENDING) {
			g_pending[g_pending_n++] = op;
			return;
		}
		if (rc != 0) {
			op_complete(op, rc);
		}
		break;
	}
	case OP_DELETE: {
		int rc = reactor_do_delete(op->id);

		if (rc == -EBUSY) {
			/* no invalidation slot free yet: retry next step */
			if (g_pending_n < MAX_PENDING) {
				g_pending[g_pending_n++] = op;
				return;
			}
		}
		op_complete(op, rc);
		break;
	}
	case OP_BARRIER:
	case OP_CANCEL_PARTIAL:
		if (op->type == OP_CANCEL_PARTIAL) {
			for (int i = 0; i < MAX_DUMP_CTX; i++) {
				if (g_dctx[i].active && g_dctx[i].commit_phase == 0) {
					g_dctx[i].failed = 1;
				}
			}
		}
		/* completes when drained, checked in the loop */
		if (g_barriers_n < MAX_PENDING) {
			g_barriers[g_barriers_n++] = op;
			return;
		}
		op_complete(op, -EBUSY);
		break;
	}
	free(op);
}

static void
drain_op_ring(void)
{
	void *ops[32];
	size_t n;

	for (;;) {
		n = spdk_ring_dequeue(g_op_ring, ops, 32);
		if (n == 0) {
			break;
		}
		for (size_t i = 0; i < n; i++) {
			process_op(ops[i]);
		}
	}
}

/* Retry parked ops: ENOSPC dumps (GC once, then wait for frees) and
 * EBUSY deletes. */
static void
advance_pending(void)
{
	for (uint32_t i = 0; i < g_pending_n; ) {
		struct store_op *op = g_pending[i];

		if (op->type == OP_DUMP) {
			if (g_stalled) {
				g_pending[i] = g_pending[--g_pending_n];
				op_complete(op, -EIO);
				free(op);
				continue;
			}
			int rc = reactor_do_dump(op);

			if (rc == 1) {
				/* still no free slot */
				if (!op->gc_attempted && store_io_drained()) {
					store_gc(GC_ENOSPC_BATCH);
					op->gc_attempted = 1;
				} else if (store_io_drained()) {
					/* GC done, nothing freed: hopeless */
					g_pending[i] = g_pending[--g_pending_n];
					op_complete(op, -ENOSPC);
					free(op);
					continue;
				}
				i++;
			} else if (rc == -EBUSY) {
				i++;
			} else if (rc != 0) {
				g_pending[i] = g_pending[--g_pending_n];
				op_complete(op, rc);
				free(op);
			} else {
				/* allocated */
				g_pending[i] = g_pending[--g_pending_n];
				free(op);
			}
		} else if (op->type == OP_LOAD) {
			if (g_stalled) {
				g_pending[i] = g_pending[--g_pending_n];
				op_complete(op, -EIO);
				free(op);
				continue;
			}
			int rc = reactor_do_load(op);

			if (rc == -EBUSY) {
				i++;
			} else {
				g_pending[i] = g_pending[--g_pending_n];
				if (rc != 0) {
					op_complete(op, rc);
				}
				free(op);
			}
		} else if (op->type == OP_DELETE) {
			if (g_stalled) {
				g_pending[i] = g_pending[--g_pending_n];
				op_complete(op, -EIO);
				free(op);
				continue;
			}
			int rc = reactor_do_delete(op->id);

			if (rc == -EBUSY) {
				i++;
			} else {
				g_pending[i] = g_pending[--g_pending_n];
				op_complete(op, rc);
				free(op);
			}
		} else {
			i++;
		}
	}
}

static void
advance_barriers(void)
{
	for (uint32_t i = 0; i < g_barriers_n; ) {
		if (store_drained() || (g_stalled && store_io_drained())) {
			struct store_op *op = g_barriers[i];

			g_barriers[i] = g_barriers[--g_barriers_n];
			op_complete(op, g_stalled ? -EIO : 0);
			free(op);
		} else {
			i++;
		}
	}
}

static void *
reactor_main(void *arg)
{
	pthread_setname_np(pthread_self(), "ucm_spdk_reactor");

	while (!g_reactor_stop) {
		/* pause handshake: park so the main thread may use the
		 * qpair directly (recovery, superblock ops, fork safety) */
		pthread_mutex_lock(&g_reactor_mu);
		if (g_reactor_pause_req) {
			g_reactor_paused = 1;
			pthread_cond_signal(&g_reactor_paused_cv);
			while (g_reactor_pause_req && !g_reactor_stop) {
				pthread_cond_wait(&g_reactor_resume_cv, &g_reactor_mu);
			}
			g_reactor_paused = 0;
		}
		pthread_mutex_unlock(&g_reactor_mu);
		if (g_reactor_stop) {
			break;
		}

		drain_op_ring();
		advance_pending();
		store_poll();
		advance_barriers();
	}
	return NULL;
}

/* ---------------- public API (any thread) ---------------- */

/*
 * All submit APIs enqueue an op and return immediately; completions
 * arrive via cb (reactor thread) and/or the syncw. Lookups stay
 * direct: they only take the SHM mutex and never touch the qpair, so
 * the scheduler-role process needs no reactor at all.
 */
static struct store_op *
op_new(int type, const uint8_t id[16], store_cb cb, void *arg,
       struct syncw *sw)
{
	struct store_op *op = calloc(1, sizeof(*op));

	if (op == NULL) {
		return NULL;
	}
	op->type = type;
	if (id != NULL) {
		memcpy(op->id, id, 16);
	}
	op->cb = cb;
	op->cb_arg = arg;
	op->sw = sw;
	return op;
}

static int
op_submit(struct store_op *op)
{
	size_t n = spdk_ring_enqueue(g_op_ring, (void **)&op, 1, NULL);

	if (n != 1) {
		free(op);
		return -EBUSY;
	}
	return 0;
}

int
store_dump_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		  const void *data, store_cb cb, void *arg)
{
	if (id == NULL || data == NULL || count == 0 || first >= g_nshards ||
	    count > g_nshards - first) {
		return -EINVAL;
	}
	{
		struct store_op *op = op_new(OP_DUMP, id, cb, arg, NULL);

		if (op == NULL) {
			return -ENOMEM;
		}
		op->first = first;
		op->count = count;
		op->data = data;
		return op_submit(op);
	}
}

int
store_load_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		  void *dst, uint32_t len,
		  store_cb cb, void *arg)
{
	if (id == NULL || dst == NULL || count == 0 || first >= g_nshards ||
	    count > g_nshards - first ||
	    len < (uint64_t)count * shard_bytes()) {
		return -EINVAL;
	}
	struct store_op *op = op_new(OP_LOAD, id, cb, arg, NULL);

	if (op == NULL) {
		return -ENOMEM;
	}
	op->first = first;
	op->count = count;
	op->dst = dst;
	op->len = len;
	return op_submit(op);
}

int
store_delete(const uint8_t id[16], store_cb cb, void *arg)
{
	struct store_op *op = op_new(OP_DELETE, id, cb, arg, NULL);

	if (op == NULL) {
		return -ENOMEM;
	}
	return op_submit(op);
}

/* Drain barrier: completes once every in-flight op has settled. */
static int
store_sync_op(int type)
{
	struct syncw w;
	struct store_op *op;
	int rc;

	syncw_init(&w);
	for (;;) {
		op = op_new(type, NULL, NULL, NULL, &w);
		if (op == NULL) {
			sched_yield();
			continue;
		}
		rc = op_submit(op);
		if (rc == 0) {
			break;
		}
		if (rc != -EBUSY) {
			goto out;
		}
		sched_yield();
	}
	rc = syncw_wait(&w);
out:
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	return rc;
}

int
store_quiesce(void)
{
	return store_sync_op(OP_BARRIER);
}

/*
 * Pause the reactor: it finishes the current step and parks, after
 * which the calling thread may use the qpair directly (store_poll,
 * sector_batch, store_recover, superblock ops) — the init-phase model.
 * Also used around fork() so the child cannot inherit a held SHM mutex.
 */
void
reactor_pause(void)
{
	if (!g_reactor_started) {
		return;
	}
	pthread_mutex_lock(&g_reactor_mu);
	g_reactor_pause_req = 1;
	while (!g_reactor_paused && !g_reactor_stop) {
		pthread_cond_wait(&g_reactor_paused_cv, &g_reactor_mu);
	}
	pthread_mutex_unlock(&g_reactor_mu);
}

void
reactor_resume(void)
{
	if (!g_reactor_started) {
		return;
	}
	pthread_mutex_lock(&g_reactor_mu);
	g_reactor_pause_req = 0;
	pthread_cond_signal(&g_reactor_resume_cv);
	pthread_mutex_unlock(&g_reactor_mu);
}

int
reactor_start(void)
{
	if (g_reactor_started) {
		return 0;
	}
	g_op_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, OP_RING_SIZE,
				     SPDK_ENV_NUMA_ID_ANY);
	if (g_op_ring == NULL) {
		return -ENOMEM;
	}
	g_reactor_stop = 0;
	g_reactor_pause_req = 0;
	if (pthread_create(&g_reactor_tid, NULL, reactor_main, NULL) != 0) {
		spdk_ring_free(g_op_ring);
		g_op_ring = NULL;
		return -errno;
	}
	g_reactor_started = 1;
	return 0;
}

void
reactor_stop(void)
{
	if (!g_reactor_started) {
		return;
	}
	pthread_mutex_lock(&g_reactor_mu);
	g_reactor_stop = 1;
	g_reactor_pause_req = 0;
	pthread_cond_broadcast(&g_reactor_resume_cv);
	pthread_mutex_unlock(&g_reactor_mu);
	pthread_join(g_reactor_tid, NULL);
	spdk_ring_free(g_op_ring);
	g_op_ring = NULL;
	g_reactor_started = 0;
}

/* ---------------- format / recovery ---------------- */

struct batch_io {
	int	done;
	int	status;
};

static void
batch_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct batch_io *b = arg;

	b->status = spdk_nvme_cpl_is_error(cpl) ? -EIO : 0;
	b->done = 1;
	g_last_progress = spdk_get_ticks();
}

static int
sector_batch(uint64_t lba, uint32_t nlbas, void *buf, int is_write)
{
	struct batch_io b = {0};
	int rc;

	if (is_write) {
		rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, buf, lba, nlbas,
					    batch_done, &b, 0);
	} else {
		rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, buf, lba, nlbas,
					   batch_done, &b, 0);
	}
	if (rc < 0) {
		return rc;
	}
	while (!b.done && !g_stalled) {
		store_poll();
	}
	return b.status;
}

/* Zero index records [from, to) in batches. */
static int store_sb_write(void);

static int
zero_records(uint32_t from, uint32_t to)
{
	int rbuf = io_buf_pop();
	uint64_t left = to - from;
	uint64_t lba = g_index_base + from;
	int rc = 0;

	if (rbuf < 0) {
		return -EBUSY;
	}
	memset(g_io_bufs[rbuf], 0, (size_t)g_recov_batch * g_sector_size);
	while (left > 0) {
		uint32_t n = (left < g_recov_batch) ? (uint32_t)left : g_recov_batch;

		rc = sector_batch(lba, n, g_io_bufs[rbuf], 1);
		if (rc != 0) {
			break;
		}
		lba += n;
		left -= n;
	}
	io_buf_push((uint32_t)rbuf);
	return rc;
}

/* First publication step: mark the region "format in progress" so a
 * crash before the final publish is recognized and finished by the
 * next boot instead of leaving a half-formatted store. The intended
 * capacity rides in `reserved` so the resume knows it. */
static int
store_sb_begin(void)
{
	int rbuf = io_buf_pop();
	struct superblock *sb;
	int rc;

	if (rbuf < 0) {
		return -EBUSY;
	}
	sb = g_io_bufs[rbuf];
	build_superblock(sb);
	sb->nslots = 0;          /* publication in progress */
	sb->reserved = g_nslots; /* intended capacity */
	sb->checksum = (uint64_t)hash_fn_n(sb, SB_COVER);
	rc = sector_batch(sb_lba(), 1, sb, 1);
	io_buf_push((uint32_t)rbuf);
	return rc;
}

static int
store_format(void)
{
	int rc;

	rc = store_sb_begin();
	if (rc != 0) {
		return rc;
	}
	rc = zero_records(0, g_nslots);
	if (rc != 0) {
		return rc;
	}
	/* Publish the valid geometry only after the active index is empty. */
	return store_sb_write();
}

/* Read the superblock. 0 = ok, -ENOENT = not a formatted store. */
static int
store_sb_read(struct superblock *out)
{
	int rbuf = io_buf_pop();
	struct superblock *sb;
	int rc;

	if (rbuf < 0) {
		return -EBUSY;
	}
	rc = sector_batch(sb_lba(), 1, g_io_bufs[rbuf], 0);
	if (rc != 0) {
		io_buf_push((uint32_t)rbuf);
		return rc;
	}
	sb = g_io_bufs[rbuf];
	if (sb->magic != SB_MAGIC ||
	    sb->checksum != (uint64_t)hash_fn_n(sb, SB_COVER)) {
		io_buf_push((uint32_t)rbuf);
		return -ENOENT;
	}
	*out = *sb;
	io_buf_push((uint32_t)rbuf);
	return 0;
}

/* Layout-critical fields must match exactly. A mismatch is a model
 * switch and is handled by store_resolve_region(), not a boot refusal. */
static int
store_sb_geometry_ok(const struct superblock *sb)
{
	if (sb->version != SB_VERSION ||
	    sb->index_slots != g_index_slots ||
	    sb->slot_sectors != g_slot_sectors ||
	    sb->shard_sectors != g_shard_sectors) {
		return -EINVAL;
	}
	return 0;
}

/*
 * Conservative shrink guard: refuse when any record in [from, to) is
 * valid and not DELETED. A losing-but-live-flagged record (crash in the
 * overwrite window) also blocks; that is intentional.
 */
static int
sb_range_has_live(uint32_t from, uint32_t to)
{
	int rbuf = io_buf_pop();
	uint32_t scanned = from;
	int found = 0;

	if (rbuf < 0) {
		return -1;
	}
	while (scanned < to && !found) {
		uint32_t n = (to - scanned < g_recov_batch)
			     ? (to - scanned) : g_recov_batch;
		int rc = sector_batch(g_index_base + scanned, n,
				      g_io_bufs[rbuf], 0);

		if (rc != 0) {
			io_buf_push((uint32_t)rbuf);
			return -1;
		}
		for (uint32_t i = 0; i < n; i++) {
			const struct index_record *rec =
				(const void *)((uint8_t *)g_io_bufs[rbuf] +
					       (size_t)i * g_sector_size);
			uint32_t slot = scanned + i;

			if (rec->magic == STORE_MAGIC && rec->slot == slot &&
			    rec->checksum == (uint64_t)hash_fn_n(rec, RECORD_COVER) &&
			    !(rec->flags & FLAG_DELETED)) {
				printf("live block (slot %u) beyond the new "
				       "boundary\n", slot);
				found = 1;
				break;
			}
		}
		scanned += n;
	}
	io_buf_push((uint32_t)rbuf);
	return found;
}

static int
store_sb_write(void)
{
	int rbuf = io_buf_pop();
	int rc;

	if (rbuf < 0) {
		return -EBUSY;
	}
	build_superblock(g_io_bufs[rbuf]);
	rc = sector_batch(sb_lba(), 1, g_io_bufs[rbuf], 1);
	io_buf_push((uint32_t)rbuf);
	return rc;
}

/*
 * Invalidate every block and re-publish the region with the runtime
 * geometry (a model switch). The byte capacity is preserved. Called
 * with the global maintenance lease held exclusively: no other SPDK
 * store process is alive, because a model switch is a rolling restart
 * by definition.
 */
static int
store_switch_region(const struct superblock *old)
{
	uint64_t old_bytes;
	uint32_t old_n = old->nslots ? old->nslots : old->reserved;
	uint32_t old_layers = 0, old_kb = 0;
	int rc;

	if (g_nslots == 0 && old_n > 0 && old->slot_sectors > 0) {
		old_bytes = (uint64_t)old_n * old->slot_sectors * g_sector_size;
		g_nslots = (uint32_t)(old_bytes /
				      ((uint64_t)g_slot_sectors * g_sector_size));
	}
	if (g_nslots == 0) {
		g_nslots = DEFAULT_FORMAT_SLOTS;
	}
	if (g_nslots > g_index_slots || g_nslots > g_pool_capacity) {
		return -EINVAL;
	}

	if (old->shard_sectors > 0 && old->slot_sectors >= old->shard_sectors) {
		old_layers = old->slot_sectors / old->shard_sectors;
		old_kb = (uint32_t)((uint64_t)old->shard_sectors * g_sector_size / 1024);
	}
	if (old_layers > 0 && old_kb > 0) {
		printf("model switch: shard %uKB x %u layers -> %uKB x %u "
		       "layers; invalidating all existing blocks "
		       "(byte capacity preserved)\n",
		       old_kb, old_layers, g_shard_kb, g_nshards);
	}
	/* The segment is region-scoped, so the old model's segment carries a
	 * stale header (old nslots) that a new-geometry attach would reject.
	 * We hold the exclusive maintenance lease: nothing else can have the
	 * segment mapped — drop it and let this boot create a fresh one that
	 * recovery rebuilds from the wiped region. The lease file itself is
	 * NOT unlinked: we hold its flock, and unlinking would fork the lock
	 * onto a new inode. */
	shm_unlink(g_shm_name);

	rc = store_format();
	if (rc == 0) {
		g_region_reset = 1;
	}
	return rc;
}

/*
 * Resolve the on-disk region against the runtime model geometry:
 *   - g_format: explicit wipe, publish the runtime geometry
 *   - sb.nslots == 0: an interrupted format/switch is finished
 *   - geometry mismatch: model switch -> all existing blocks are
 *     invalidated and the region is re-published with the runtime
 *     geometry (the store serves whatever the model declares)
 *   - geometry match: adopt the stored capacity; an explicit resize
 *     only happens when a different one is requested
 * Runs before the shared segment is opened; needs io buffers.
 */
static int
store_resolve_region(void)
{
	struct superblock sb;
	int rc;

	if (g_format) {
		if (g_nslots == 0) {
			g_nslots = DEFAULT_FORMAT_SLOTS;
		}
		if (g_nslots > g_index_slots || g_nslots > g_pool_capacity) {
			return -EINVAL;
		}
		rc = store_format();
		if (rc == 0) {
			g_region_reset = 1;
			printf("Formatted.\n");
		}
		return rc;
	}

	for (;;) {
		rc = store_sb_read(&sb);
		if (rc == -ENOENT) {
			fprintf(stderr, "region at offset %lu GiB is not a "
				"formatted store; first use needs -F / "
				"spdk_format\n",
				(unsigned long)g_offset_gib);
			return rc;
		}
		if (rc != 0) {
			return rc;
		}

		if (sb.nslots == 0) {
			/* interrupted format or model switch: finish it */
			if (store_maintenance_upgrade() != 0) {
				fprintf(stderr, "region format in progress "
					"(another process?); retry\n");
				return -EBUSY;
			}
			rc = store_sb_read(&sb);
			if (rc != 0) {
				flock(g_maintenance_fd, LOCK_SH);
				return rc;
			}
			if (sb.nslots != 0) {
				/* finished by another process meanwhile */
				flock(g_maintenance_fd, LOCK_SH);
				continue;
			}
			if (g_nslots == 0) {
				g_nslots = sb.reserved;
			}
			if (store_sb_geometry_ok(&sb) == 0) {
				/* same model: just finish publishing */
				if (g_nslots == 0 || g_nslots > g_index_slots ||
				    g_nslots > g_pool_capacity) {
					flock(g_maintenance_fd, LOCK_SH);
					return -EINVAL;
				}
				rc = zero_records(0, g_nslots);
				if (rc == 0) {
					rc = store_sb_write();
				}
				if (rc == 0) {
					g_region_reset = 1;
					printf("resumed interrupted format\n");
				}
			} else {
				/* the interrupted switch targeted another
				 * model: redo it against ours */
				rc = store_switch_region(&sb);
			}
			flock(g_maintenance_fd, LOCK_SH);
			return rc;
		}

		if (store_sb_geometry_ok(&sb) != 0) {
			/* model switch: the runtime geometry wins and all
			 * existing blocks are invalidated */
			if (store_maintenance_upgrade() != 0) {
				/* another process may have just finished
				 * the switch while we were booting */
				rc = store_sb_read(&sb);
				if (rc == 0 && sb.nslots != 0 &&
				    store_sb_geometry_ok(&sb) == 0) {
					continue;
				}
				fprintf(stderr, "model change requires all "
					"SPDK store processes to exit; "
					"retry\n");
				return -EBUSY;
			}
			rc = store_sb_read(&sb);
			if (rc != 0) {
				flock(g_maintenance_fd, LOCK_SH);
				return rc;
			}
			if (sb.nslots != 0 && store_sb_geometry_ok(&sb) == 0) {
				flock(g_maintenance_fd, LOCK_SH);
				continue;
			}
			rc = store_switch_region(&sb);
			flock(g_maintenance_fd, LOCK_SH);
			return rc;
		}

		/* geometry matches: adopt, or explicit resize */
		if (g_nslots == 0 || g_nslots == sb.nslots) {
			g_nslots = sb.nslots;
			return 0;
		}
		if (store_maintenance_upgrade() != 0) {
			fprintf(stderr, "resize requires all SPDK store "
				"processes to stop\n");
			return -EBUSY;
		}
		if (!g_shm_recovery_owner) {
			fprintf(stderr, "resize requires all other workers "
				"to stop\n");
			flock(g_maintenance_fd, LOCK_SH);
			return -EBUSY;
		}
		if (g_nslots > sb.nslots) {
			if (g_nslots > g_index_slots || g_nslots > g_pool_capacity) {
				flock(g_maintenance_fd, LOCK_SH);
				return -EINVAL;
			}
			rc = zero_records(sb.nslots, g_nslots);
			if (rc == 0) {
				rc = store_sb_write();
			}
			if (rc == 0) {
				printf("Grew capacity %u -> %u slots.\n",
				       sb.nslots, g_nslots);
			}
		} else {
			if (sb_range_has_live(g_nslots, sb.nslots)) {
				fprintf(stderr, "shrink %u -> %u refused: "
					"live blocks beyond the boundary\n",
					sb.nslots, g_nslots);
				flock(g_maintenance_fd, LOCK_SH);
				return -EBUSY;
			}
			rc = zero_records(g_nslots, sb.nslots);
			if (rc == 0) {
				rc = store_sb_write();
			}
			if (rc == 0) {
				printf("Shrunk capacity %u -> %u slots.\n",
				       sb.nslots, g_nslots);
			}
		}
		flock(g_maintenance_fd, LOCK_SH);
		return rc;
	}
}

/* Rebuild hash + freelist from the index region. Must be quiesced.
 * The whole rebuild runs under the shm lock: recovery is rare and the
 * poll loop invoked from the batch reads never touches shared state. */
static int
store_recover(void)
{
	int rbuf = io_buf_pop();
	uint64_t scanned = 0;
	uint64_t max_epoch_seen = 0;
	uint8_t *used;
	int rc = 0;

	if (rbuf < 0) {
		return -EBUSY;
	}
	used = calloc(g_nslots, 1);
	if (used == NULL) {
		io_buf_push((uint32_t)rbuf);
		return -ENOMEM;
	}

	shm_lock();
	memset(g_hash, 0, (size_t)(g_hash_mask + 1) * sizeof(*g_hash));
	memset(g_shm->workers, 0, sizeof(g_shm->workers));
	g_hash_count = 0;
	g_live_count = 0;
	g_epoch = 0;

	while (scanned < g_nslots && rc == 0) {
		uint32_t n = (g_nslots - scanned < g_recov_batch)
			     ? (uint32_t)(g_nslots - scanned) : g_recov_batch;

		rc = sector_batch(g_index_base + scanned, n, g_io_bufs[rbuf], 0);
		if (rc != 0) {
			break;
		}
		for (uint32_t i = 0; i < n; i++) {
			const struct index_record *rec =
				(const void *)((uint8_t *)g_io_bufs[rbuf] +
					       (size_t)i * g_sector_size);
			uint32_t slot = (uint32_t)scanned + i;
			struct hash_entry *e = NULL;
			uint32_t h;

			if (rec->magic != STORE_MAGIC ||
			    rec->slot != slot ||
			    rec->checksum != (uint64_t)hash_fn_n(rec, RECORD_COVER)) {
				continue;
			}
			if (rec->epoch > max_epoch_seen) {
				max_epoch_seen = rec->epoch;
			}

			h = hash_fn16(rec->block_id) & g_hash_mask;
			while (g_hash[h].used != 0) {
				if (g_hash[h].used == 1 &&
				    memcmp(g_hash[h].id, rec->block_id, 16) == 0) {
					e = &g_hash[h];
					break;
				}
				h = (h + 1) & g_hash_mask;
			}
			if (e == NULL) {
				memcpy(g_hash[h].id, rec->block_id, 16);
				g_hash[h].used = 1;
				g_hash[h].slot = rec->slot;
				g_hash[h].epoch = rec->epoch;
				g_hash[h].del_win = (rec->flags & FLAG_DELETED) ? 1 : 0;
				g_hash_count++;
			} else {
				int rec_del = (rec->flags & FLAG_DELETED) ? 1 : 0;
				int better;

				if (rec->epoch != e->epoch) {
					better = rec->epoch > e->epoch;
				} else {
					/* Equal epoch (an overwrite's
					 * old-slot invalidation reuses the
					 * commit epoch): live beats deleted. */
					better = e->del_win && !rec_del;
				}
				if (better) {
					e->slot = rec->slot;
					e->epoch = rec->epoch;
					e->del_win = (uint8_t)rec_del;
				}
			}
		}
		scanned += n;
	}
	io_buf_push((uint32_t)rbuf);
	if (rc != 0) {
		shm_unlock();
		free(used);
		return rc;
	}

	/* Rebuild instead of deleting in place: backward-shift can wrap a
	 * yet-unvisited entry into an already-visited bucket. */
	{
		uint32_t winners = 0;

		for (uint32_t h = 0; h <= g_hash_mask; h++) {
			if (g_hash[h].used == 1 && !g_hash[h].del_win) {
				winners++;
				used[g_hash[h].slot] = 1;
			}
		}
		for (uint32_t h = 0; h <= g_hash_mask; h++) {
			if (g_hash[h].used == 1 && g_hash[h].del_win) {
				g_hash[h].used = 0;
			}
		}
		if (winners != g_hash_count) {
			struct hash_entry *live = calloc(winners, sizeof(*live));
			uint32_t nlive = 0;

			if (live == NULL && winners != 0) {
				shm_unlock();
				free(used);
				return -ENOMEM;
			}
			for (uint32_t h = 0; h <= g_hash_mask; h++) {
				if (g_hash[h].used == 1) {
					live[nlive++] = g_hash[h];
				}
			}
			memset(g_hash, 0, (size_t)(g_hash_mask + 1) * sizeof(*g_hash));
			g_hash_count = 0;
			for (uint32_t i = 0; i < nlive; i++) {
				hash_insert(live[i].id, live[i].slot, live[i].epoch);
			}
			free(live);
		}
		g_live_count = winners;
	}

	g_free_top = 0;
	for (uint32_t s = 0; s < g_nslots; s++) {
		if (!used[s]) {
			g_freelist[g_free_top++] = s;
		}
	}
	g_epoch = max_epoch_seen + 1;
	shm_unlock();
	free(used);
	return 0;
}

/* ---------------- test helpers ---------------- */

static int delete_sync(const uint8_t id[16]);

static void
gen_test_id(uint8_t id[16], uint32_t test_no, uint32_t idx)
{
	uint32_t v = test_no * 1000003u + idx * 7919u + g_seed;

	for (int i = 0; i < 16; i++) {
		id[i] = (uint8_t)(v >> ((i % 4) * 8));
		v = v * 1103515245u + 12345u + i;
	}
}

static void
fill_pattern(uint8_t *buf, uint32_t shard, uint32_t seed, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(seed + shard * 131u + (uint32_t)i);
	}
}

static int
check_pattern(const uint8_t *buf, uint32_t shard, uint32_t seed, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != (uint8_t)(seed + shard * 131u + (uint32_t)i)) {
			return -1;
		}
	}
	return 0;
}

static int
dump_block_sync(const uint8_t id[16], const uint8_t *data)
{
	struct syncw w;
	int rc;

	syncw_init(&w);
	rc = store_dump_submit(id, 0, g_nshards, data, sync_cb, &w);
	if (rc != 0) {
		return rc;
	}
	rc = syncw_wait(&w);
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	return rc;
}

static int
load_block_sync(const uint8_t id[16], uint8_t *dst)
{
	struct syncw w;
	int rc;

	syncw_init(&w);
	rc = store_load_submit(id, 0, g_nshards, dst, slot_bytes(), sync_cb, &w);
	if (rc != 0) {
		return rc;
	}
	rc = syncw_wait(&w);
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	return rc;
}

/* ---------------- unit tests ---------------- */

static int
t1_consistency(int fresh)
{
	if (g_free_top + g_live_count != g_nslots) {
		printf("    free(%u) + live(%u) != nslots(%u)\n",
		       g_free_top, g_live_count, g_nslots);
		return -1;
	}
	if (fresh && (g_live_count != 0 || g_free_top != g_nslots)) {
		printf("    formatted store is not empty\n");
		return -1;
	}
	return 0;
}

static int
t2_dump_and_lookup(void)
{
	uint8_t *data = malloc(slot_bytes());
	uint8_t id[16];
	int rc = 0;

	if (data == NULL) {
		return -1;
	}
	for (uint32_t b = 0; b < 8; b++) {
		gen_test_id(id, 2, b);
		for (uint32_t s = 0; s < g_nshards; s++) {
			fill_pattern(data + s * shard_bytes(), s, b * 17 + 3,
				     shard_bytes());
		}
		rc = dump_block_sync(id, data);
		if (rc != 0 || !store_lookup(id)) {
			printf("    dump/lookup failed at block %u (rc=%d)\n", b, rc);
			rc = -1;
			break;
		}
	}
	free(data);
	return rc;
}

static int
t3_load_verify(void)
{
	uint8_t *dst = malloc(slot_bytes());
	uint8_t *one = malloc(shard_bytes() + 2);
	uint8_t id[16];
	int rc = 0;

	if (dst == NULL || one == NULL) {
		free(dst);
		free(one);
		return -1;
	}
	for (uint32_t b = 0; b < 8 && rc == 0; b++) {
		gen_test_id(id, 2, b);
		rc = load_block_sync(id, dst);
		if (rc != 0) {
			printf("    load failed at block %u (rc=%d)\n", b, rc);
			break;
		}
		for (uint32_t s = 0; s < g_nshards; s++) {
			if (check_pattern(dst + s * shard_bytes(), s, b * 17 + 3,
					  shard_bytes()) != 0) {
				printf("    content mismatch block %u shard %u\n", b, s);
				rc = -1;
				break;
			}
		}
	}
	if (rc == 0) {
		struct syncw w;
		uint32_t shard = g_nshards / 2;

		memset(one, 0xa5, shard_bytes() + 2);
		syncw_init(&w);
		gen_test_id(id, 2, 0);
		rc = store_load_submit(id, shard, 1, one + 1, shard_bytes(), sync_cb, &w);
		if (rc == 0) {
			rc = syncw_wait(&w);
		}
		pthread_mutex_destroy(&w.mu);
		pthread_cond_destroy(&w.cv);
		if (rc != 0 || one[0] != 0xa5 || one[shard_bytes() + 1] != 0xa5 ||
		    check_pattern(one + 1, shard, 3, shard_bytes()) != 0) {
			printf("    indexed load failed at shard %u (rc=%d)\n", shard, rc);
			rc = -1;
		}
	}
	free(dst);
	free(one);
	return rc;
}

static int
t4_prefix_semantics(void)
{
	uint8_t ids[4][16];

	gen_test_id(ids[0], 2, 0);
	gen_test_id(ids[1], 2, 1);
	gen_test_id(ids[2], 4, 0);
	gen_test_id(ids[3], 2, 2);
	if (store_lookup_prefix(ids, 4) != 2) {
		printf("    prefix count != 2\n");
		return -1;
	}
	gen_test_id(ids[0], 4, 1);
	if (store_lookup_prefix(ids, 4) != 0) {
		printf("    prefix count != 0\n");
		return -1;
	}
	return 0;
}

static int
t5_commit_on_last_shard(void)
{
	uint8_t *data = malloc(slot_bytes());
	struct syncw *waiters = calloc(g_nshards, sizeof(*waiters));
	uint8_t id[16];
	uint32_t k = g_nshards / 2;
	uint32_t initialized = 0;
	int rc = 0;

	if (data == NULL || waiters == NULL) {
		free(data);
		free(waiters);
		return -1;
	}
	gen_test_id(id, 5, 0);
	if (store_lookup(id)) {
		/* re-run without -F: clear the leftover from a previous run */
		delete_sync(id);
	}
	for (uint32_t s = 0; s < g_nshards; s++) {
		fill_pattern(data + s * shard_bytes(), s, 55, shard_bytes());
	}

	for (uint32_t s = 0; s < k; s++) {
		syncw_init(&waiters[s]);
		initialized++;
		rc = store_dump_submit(id, s, 1,
				       data + (size_t)s * shard_bytes(),
				       sync_cb, &waiters[s]);
		if (rc != 0) {
			printf("    partial shard %u submit failed (%d)\n", s, rc);
			goto out;
		}
	}
	store_quiesce();
	if (store_lookup(id)) {
		printf("    block visible before last shard!\n");
		rc = -1;
		goto out;
	}

	for (uint32_t s = k; s < g_nshards; s++) {
		syncw_init(&waiters[s]);
		initialized++;
		rc = store_dump_submit(id, s, 1,
				       data + (size_t)s * shard_bytes(),
				       sync_cb, &waiters[s]);
		if (rc != 0) {
			printf("    final shard %u submit failed (%d)\n", s, rc);
			goto out;
		}
	}
	for (uint32_t s = 0; s < g_nshards; s++) {
		int shard_rc = syncw_wait(&waiters[s]);

		if (rc == 0 && shard_rc != 0) {
			rc = shard_rc;
		}
	}
	if (rc != 0 || !store_lookup(id)) {
		printf("    block not visible after commit (rc=%d)\n", rc);
		rc = -1;
	}
	if (rc == 0) {
		uint8_t partial_id[16];
		uint32_t free_before = g_free_top;

		gen_test_id(partial_id, 5, 1);
		rc = store_dump_submit(partial_id, 0, 1, data, NULL, NULL);
		if (rc == 0) {
			store_quiesce();
			rc = store_sync_op(OP_CANCEL_PARTIAL);
		}
		if (rc != 0 || store_lookup(partial_id) || g_free_top != free_before) {
			printf("    partial dump cancellation failed (rc=%d)\n", rc);
			rc = -1;
		}
	}
out:
	for (uint32_t s = 0; s < initialized; s++) {
		pthread_mutex_destroy(&waiters[s].mu);
		pthread_cond_destroy(&waiters[s].cv);
	}
	free(waiters);
	free(data);
	return rc;
}

static int
t6_overwrite(void)
{
	uint8_t *data = malloc(slot_bytes());
	uint8_t id[16];
	uint32_t free_before;
	uint32_t old_slot;
	int test_pin = 0;
	int rc;

	if (data == NULL) {
		return -1;
	}
	gen_test_id(id, 6, 0);
	if (store_lookup(id)) {
		/* re-run without -F: clear the leftover from a previous run */
		delete_sync(id);
	}
	free_before = g_free_top;
	memset(data, 0x11, slot_bytes());
	rc = dump_block_sync(id, data);
	if (rc != 0 || !store_lookup(id)) {
		printf("    first dump failed\n");
		rc = -1;
		goto out;
	}
	shm_lock();
	old_slot = hash_find(id)->slot;
	__atomic_fetch_add(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
	test_pin = 1;
	shm_unlock();
	memset(data, 0x22, slot_bytes());
	rc = dump_block_sync(id, data);
	if (rc != 0 || !store_lookup(id)) {
		printf("    overwrite dump failed\n");
		rc = -1;
		goto out;
	}
	store_quiesce();
	if (g_free_top != free_before - 2) {
		printf("    pinned old slot was reused early\n");
		__atomic_fetch_sub(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
		test_pin = 0;
		rc = -1;
		goto out;
	}
	__atomic_fetch_sub(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
	test_pin = 0;
	store_quiesce();
	/* one live block remains: net free must drop by exactly one */
	if (g_free_top != free_before - 1) {
		printf("    free count wrong: %u -> %u (want %u)\n",
		       free_before, g_free_top, free_before - 1);
		rc = -1;
		goto out;
	}
	{
		uint8_t *dst = malloc(slot_bytes());

		if (dst == NULL) {
			rc = -1;
			goto out;
		}
		rc = load_block_sync(id, dst);
		if (rc == 0) {
			for (size_t i = 0; i < slot_bytes(); i++) {
				if (dst[i] != 0x22) {
					printf("    stale content at offset %zu\n", i);
					rc = -1;
					break;
				}
			}
		}
		free(dst);
	}
out:
	if (test_pin) {
		__atomic_fetch_sub(&g_pins[old_slot], 1, __ATOMIC_RELAXED);
	}
	free(data);
	return rc;
}

static int
t7_delete(void)
{
	uint8_t *data = malloc(slot_bytes());
	uint8_t id[16];
	uint32_t free_before;
	int rc;

	if (data == NULL) {
		return -1;
	}
	gen_test_id(id, 7, 0);
	memset(data, 0x55, slot_bytes());
	rc = dump_block_sync(id, data);
	free(data);
	if (rc != 0 || !store_lookup(id)) {
		printf("    setup dump failed\n");
		return -1;
	}

	free_before = g_free_top;
	rc = delete_sync(id);
	if (rc != 1) {
		printf("    delete returned %d\n", rc);
		return -1;
	}
	if (store_lookup(id)) {
		printf("    block still visible after delete\n");
		return -1;
	}
	if (g_free_top != free_before + 1) {
		printf("    slot not freed: %u -> %u\n", free_before, g_free_top);
		return -1;
	}
	return 0;
}

static int
t8_recovery_rebuild(void)
{
	uint8_t id[16];
	uint8_t *dst = malloc(slot_bytes());
	int rc;

	if (dst == NULL) {
		return -1;
	}
	reactor_pause();
	rc = store_recover();
	reactor_resume();
	if (rc != 0) {
		printf("    recovery failed (%d)\n", rc);
		free(dst);
		return -1;
	}
	if (g_free_top + g_live_count != g_nslots) {
		printf("    inconsistent counts after recovery\n");
		free(dst);
		return -1;
	}
	for (uint32_t b = 0; b < 8; b++) {
		gen_test_id(id, 2, b);
		if (!store_lookup(id)) {
			printf("    block %u lost after recovery\n", b);
			free(dst);
			return -1;
		}
		rc = load_block_sync(id, dst);
		if (rc != 0) {
			printf("    load after recovery failed (%d)\n", rc);
			free(dst);
			return -1;
		}
		for (uint32_t s = 0; s < g_nshards; s++) {
			if (check_pattern(dst + s * shard_bytes(), s, b * 17 + 3,
					  shard_bytes()) != 0) {
				printf("    content mismatch after recovery\n");
				free(dst);
				return -1;
			}
		}
	}
	gen_test_id(id, 7, 0);
	if (store_lookup(id)) {
		printf("    deleted block resurrected\n");
		free(dst);
		return -1;
	}
	free(dst);
	return 0;
}

static int
t9_corrupt_record(void)
{
	uint8_t id[16];
	int rc;

	gen_test_id(id, 9, 0);
	{
		uint8_t *data = malloc(slot_bytes());
		struct hash_entry *e;
		int jbuf;

		if (data == NULL) {
			return -1;
		}
		memset(data, 0x33, slot_bytes());
		rc = dump_block_sync(id, data);
		free(data);
		if (rc != 0) {
			return -1;
		}
		shm_lock();
		e = hash_find(id);
		shm_unlock();
		if (e == NULL) {
			return -1;
		}
		jbuf = io_buf_pop();
		if (jbuf < 0) {
			return -1;
		}
		memset(g_io_bufs[jbuf], 0xA5, g_sector_size);
		reactor_pause();
		rc = sector_batch(record_lba(e->slot), 1, g_io_bufs[jbuf], 1);
		reactor_resume();
		io_buf_push((uint32_t)jbuf);
		if (rc != 0) {
			return -1;
		}
	}
	reactor_pause();
	rc = store_recover();
	reactor_resume();
	if (rc != 0) {
		printf("    recovery failed (%d)\n", rc);
		return -1;
	}
	if (store_lookup(id)) {
		printf("    corrupted record survived recovery\n");
		return -1;
	}
	gen_test_id(id, 2, 0);
	if (!store_lookup(id)) {
		printf("    unrelated block damaged by corruption\n");
		return -1;
	}
	return 0;
}

static int
delete_sync(const uint8_t id[16])
{
	struct syncw w;
	int rc;

	syncw_init(&w);
	rc = store_delete(id, sync_cb, &w);
	if (rc == 0) {
		syncw_wait(&w);
		rc = w.status;
	}
	pthread_mutex_destroy(&w.mu);
	pthread_cond_destroy(&w.cv);
	if (rc >= 0) {
		store_quiesce();
	}
	return rc;
}

static void
pin_all_live(int delta)
{
	shm_lock();
	for (uint32_t h = 0; h <= g_hash_mask; h++) {
		if (g_hash[h].used == 1) {
			__atomic_fetch_add(&g_pins[g_hash[h].slot], (uint16_t)delta,
					   __ATOMIC_RELAXED);
		}
	}
	shm_unlock();
}

static void
stamp_all_live(uint64_t hot)
{
	shm_lock();
	for (uint32_t h = 0; h <= g_hash_mask; h++) {
		if (g_hash[h].used == 1) {
			g_hotness[g_hash[h].slot] = hot;
		}
	}
	shm_unlock();
}

static void
set_hotness_of(const uint8_t id[16], uint64_t hot)
{
	shm_lock();
	{
		struct hash_entry *e = hash_find(id);

		if (e != NULL) {
			g_hotness[e->slot] = hot;
		}
	}
	shm_unlock();
}

static int
t10_gc_capacity(void)
{
	uint8_t *data = malloc(slot_bytes());
	uint8_t id[16];
	uint32_t free_before = g_free_top;
	uint32_t live_before = g_live_count;
	uint32_t nfilled = 0;
	int rc = 0;

	if (data == NULL) {
		return -1;
	}
	memset(data, 0x44, slot_bytes());

	/* 1. fill every free slot */
	while (g_free_top > 0) {
		gen_test_id(id, 10, nfilled);
		rc = dump_block_sync(id, data);
		if (rc != 0) {
			printf("    fill dump failed at %u (%d)\n", nfilled, rc);
			rc = -1;
			goto out;
		}
		nfilled++;
	}
	if (nfilled < GC_ENOSPC_BATCH + 1) {
		printf("    not enough slots to exercise GC (%u)\n", nfilled);
		rc = -1;
		goto out;
	}

	/* 2. all live blocks pinned: GC finds nothing -> -ENOSPC */
	pin_all_live(1);
	gen_test_id(id, 10, nfilled);
	rc = dump_block_sync(id, data);
	if (rc != -ENOSPC) {
		printf("    dump with all pinned returned %d (want -ENOSPC)\n", rc);
		pin_all_live(-1);
		rc = -1;
		goto out;
	}

	/* 3. unpin; designate GC_ENOSPC_BATCH victims (coldest) and
	 *    mark everything else clearly hot */
	pin_all_live(-1);
	stamp_all_live(now_ms() + 1000000);
	for (uint32_t b = 0; b < GC_ENOSPC_BATCH; b++) {
		gen_test_id(id, 10, b);
		set_hotness_of(id, b + 1);
	}

	/* 4. the dump must succeed by evicting exactly the victims */
	gen_test_id(id, 10, nfilled);
	rc = dump_block_sync(id, data);
	if (rc != 0) {
		printf("    dump via GC failed (%d)\n", rc);
		rc = -1;
		goto out;
	}

	/* 5. verify: victims gone, everyone else alive, counts intact */
	for (uint32_t b = 0; b < GC_ENOSPC_BATCH; b++) {
		gen_test_id(id, 10, b);
		if (store_lookup(id)) {
			printf("    victim %u survived GC\n", b);
			rc = -1;
			goto out;
		}
	}
	gen_test_id(id, 10, nfilled);
	if (!store_lookup(id)) {
		printf("    new block missing after GC dump\n");
		rc = -1;
		goto out;
	}
	for (uint32_t b = GC_ENOSPC_BATCH; b < nfilled; b++) {
		gen_test_id(id, 10, b);
		if (!store_lookup(id)) {
			printf("    non-victim %u evicted\n", b);
			rc = -1;
			goto out;
		}
	}
	if (g_live_count != live_before + nfilled - GC_ENOSPC_BATCH + 1) {
		printf("    live count wrong: %u (want %u)\n", g_live_count,
		       live_before + nfilled - GC_ENOSPC_BATCH + 1);
		rc = -1;
		goto out;
	}
	if (g_free_top + g_live_count != g_nslots) {
		printf("    free+live != nslots after GC\n");
		rc = -1;
		goto out;
	}

	/* 6. cleanup: delete survivors + the new block */
	for (uint32_t b = GC_ENOSPC_BATCH; b <= nfilled; b++) {
		gen_test_id(id, 10, b);
		if (delete_sync(id) < 0) {
			printf("    cleanup delete %u failed\n", b);
			rc = -1;
			goto out;
		}
	}
	if (g_free_top != free_before) {
		printf("    free count not restored: %u -> %u\n",
		       free_before, g_free_top);
		rc = -1;
		goto out;
	}
	rc = 0;
out:
	free(data);
	return rc;
}

static int
t11_interleaved_dumps(void)
{
	enum { N = 20 };
	uint8_t *data[N];
	uint8_t id[N][16];
	struct syncw w[N];
	uint32_t free_before;
	int nloads = 0;
	int rc = 0;

	for (int i = 0; i < N; i++) {
		data[i] = malloc(slot_bytes());
		syncw_init(&w[i]);
		if (data[i] == NULL) {
			for (int j = 0; j < i; j++) {
				free(data[j]);
			}
			return -1;
		}
		gen_test_id(id[i], 11, (uint32_t)i);
		/* re-run without -F: clear leftovers so these are fresh dumps */
		if (store_lookup(id[i])) {
			delete_sync(id[i]);
		}
	}
	store_quiesce();
	free_before = g_free_top;

	for (int i = 0; i < N; i++) {
		for (uint32_t s = 0; s < g_nshards; s++) {
			fill_pattern(data[i] + s * shard_bytes(), s,
				     (uint32_t)i * 91 + 7, shard_bytes());
		}
		rc = store_dump_submit(id[i], 0, 1, data[i],
				       sync_cb, &w[i]);
		if (rc != 0) {
			printf("    submit %d failed (%d)\n", i, rc);
			rc = -1;
			break;
		}
	}
	if (rc == 0) {
		for (int i = 0; i < N; i++) {
			if (syncw_wait(&w[i]) != 0) {
				printf("    first layer %d failed\n", i);
				rc = -1;
			}
		}
	}
	if (rc == 0) {
		for (int i = 0; i < N; i++) {
			pthread_mutex_destroy(&w[i].mu);
			pthread_cond_destroy(&w[i].cv);
			syncw_init(&w[i]);
			rc = store_dump_submit(id[i], 1, g_nshards - 1,
					       data[i] + shard_bytes(), sync_cb, &w[i]);
			if (rc != 0) {
				printf("    remaining layers %d submit failed (%d)\n", i, rc);
				rc = -1;
				break;
			}
		}
	}
	if (rc == 0) {
		for (int i = 0; i < N; i++) {
			if (syncw_wait(&w[i]) != 0) {
				printf("    remaining layers %d failed\n", i);
				rc = -1;
			}
		}
	}
	if (rc == 0) {
		for (int i = 0; i < N; i++) {
			pthread_mutex_destroy(&w[i].mu);
			pthread_cond_destroy(&w[i].cv);
			syncw_init(&w[i]);
			memset(data[i], 0, slot_bytes());
			rc = store_load_submit(id[i], 0, g_nshards, data[i],
					       slot_bytes(), sync_cb, &w[i]);
			if (rc != 0) {
				printf("    load submit %d failed (%d)\n", i, rc);
				rc = -1;
				break;
			}
			nloads++;
		}
	}
	for (int i = 0; i < nloads; i++) {
		if (syncw_wait(&w[i]) != 0) {
			printf("    load %d failed\n", i);
			rc = -1;
		}
	}
	for (int i = 0; i < N; i++) {
		if (rc == 0) {
			if (!store_lookup(id[i])) {
				printf("    block %d not visible\n", i);
				rc = -1;
			} else {
				for (uint32_t s = 0; s < g_nshards; s++) {
					if (check_pattern(data[i] + s * shard_bytes(),
							  s, (uint32_t)i * 91 + 7,
							  shard_bytes()) != 0) {
						printf("    content mismatch %d shard %u\n", i, s);
						rc = -1;
						break;
					}
				}
			}
		}
		pthread_mutex_destroy(&w[i].mu);
		pthread_cond_destroy(&w[i].cv);
		free(data[i]);
	}
	if (rc == 0 && g_free_top + N != free_before) {
		printf("    free count wrong: %u + %d != %u\n",
		       g_free_top, N, free_before);
		rc = -1;
	}
	return rc;
}

/*
 * Scheduler-role check: attach the segment as an independent process
 * (no device, no reactor — lookups only) and verify that the worker's
 * commits are visible cross-process.
 */
static int
lookup_role_check(void)
{
	uint8_t ids[8][16];
	uint8_t miss[16];

	/* simulate an independent attach: drop any inherited mapping */
	if (g_shm_base != NULL) {
		shm_close_segment();
	}
	if (shm_open_segment(0, 1) != 0) {
		printf("    (lookup role) no valid shared segment\n");
		return -1;
	}
	printf("    (lookup role) attached: %u live, %u free, epoch %ju\n",
	       g_live_count, g_free_top, (uintmax_t)g_epoch);

	for (uint32_t b = 0; b < 8; b++) {
		gen_test_id(ids[b], 2, b);
	}
	gen_test_id(miss, 4, 0);

	if (store_lookup_prefix(ids, 8) != 8) {
		printf("    (lookup role) worker blocks not all visible\n");
		return -1;
	}
	if (store_lookup(miss)) {
		printf("    (lookup role) unknown block visible\n");
		return -1;
	}
	return 0;
}

static int
t12_cross_process_lookup(void)
{
	pid_t pid;
	int status;

	/* pause the reactor so the child cannot inherit a held SHM mutex */
	reactor_pause();
	pid = fork();
	reactor_resume();
	if (pid < 0) {
		printf("    fork failed\n");
		return -1;
	}
	if (pid == 0) {
		int rc = lookup_role_check();

		fflush(stdout);
		_exit(rc == 0 ? 0 : 1);
	}
	if (waitpid(pid, &status, 0) != pid) {
		printf("    waitpid failed\n");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		return -1;
	}
	return 0;
}

static int
t13_backward_shift_stress(void)
{
	uint8_t *data = malloc(slot_bytes());
	uint8_t id[16];
	uint32_t free_before = g_free_top;
	int rc = 0;

	if (data == NULL) {
		return -1;
	}
	memset(data, 0x66, slot_bytes());

	/* batch 1: 16 blocks */
	for (uint32_t b = 0; b < 16; b++) {
		gen_test_id(id, 13, b);
		rc = dump_block_sync(id, data);
		if (rc != 0) {
			printf("    dump %u failed (%d)\n", b, rc);
			rc = -1;
			goto out;
		}
	}
	/* delete evens, verify odds survive */
	for (uint32_t b = 0; b < 16; b += 2) {
		gen_test_id(id, 13, b);
		if (delete_sync(id) != 1) {
			printf("    delete even %u failed\n", b);
			rc = -1;
			goto out;
		}
	}
	for (uint32_t b = 0; b < 16; b++) {
		gen_test_id(id, 13, b);
		if (store_lookup(id) != (b % 2)) {
			printf("    visibility wrong at %u after first deletes\n", b);
			rc = -1;
			goto out;
		}
	}
	/* batch 2: 8 more (reuse the freed slots), then delete odds of 1 */
	for (uint32_t b = 16; b < 24; b++) {
		gen_test_id(id, 13, b);
		rc = dump_block_sync(id, data);
		if (rc != 0) {
			printf("    dump %u failed (%d)\n", b, rc);
			rc = -1;
			goto out;
		}
	}
	for (uint32_t b = 1; b < 16; b += 2) {
		gen_test_id(id, 13, b);
		if (delete_sync(id) != 1) {
			printf("    delete odd %u failed\n", b);
			rc = -1;
			goto out;
		}
	}
	/* batch 1 fully gone, batch 2 alive, earlier tests untouched */
	for (uint32_t b = 0; b < 16; b++) {
		gen_test_id(id, 13, b);
		if (store_lookup(id)) {
			printf("    batch1 block %u still visible\n", b);
			rc = -1;
			goto out;
		}
	}
	for (uint32_t b = 16; b < 24; b++) {
		gen_test_id(id, 13, b);
		if (!store_lookup(id)) {
			printf("    batch2 block %u missing\n", b);
			rc = -1;
			goto out;
		}
	}
	gen_test_id(id, 2, 0);
	if (!store_lookup(id)) {
		printf("    unrelated block lost\n");
		rc = -1;
		goto out;
	}

	/* structural invariants: every live entry findable at its own slot,
	 * live count matches the table scan (no tombstones can exist) */
	{
		uint32_t cnt = 0;

		shm_lock();
		for (uint32_t h = 0; h <= g_hash_mask; h++) {
			struct hash_entry *e = &g_hash[h];

			if (e->used == 1) {
				struct hash_entry *f = hash_find(e->id);

				if (f == NULL || f->slot != e->slot) {
					printf("    entry at %u not findable\n", h);
					rc = -1;
					break;
				}
				cnt++;
			}
		}
		if (rc == 0 && cnt != g_hash_count) {
			printf("    live scan %u != hash_count %u\n", cnt, g_hash_count);
			rc = -1;
		}
		shm_unlock();
	}
	if (rc != 0) {
		goto out;
	}

	/* spot-check content through a load */
	{
		uint8_t *dst = malloc(slot_bytes());

		gen_test_id(id, 13, 20);
		if (dst != NULL && load_block_sync(id, dst) == 0) {
			for (size_t i = 0; i < slot_bytes(); i++) {
				if (dst[i] != 0x66) {
					printf("    content mismatch at %zu\n", i);
					rc = -1;
					break;
				}
			}
		} else {
			printf("    load spot-check failed\n");
			rc = -1;
		}
		free(dst);
	}

	/* cleanup: delete batch 2 */
	for (uint32_t b = 16; b < 24; b++) {
		gen_test_id(id, 13, b);
		if (delete_sync(id) != 1) {
			printf("    cleanup delete %u failed\n", b);
			rc = -1;
			goto out;
		}
	}
	if (g_free_top != free_before) {
		printf("    free count not restored: %u -> %u\n",
		       free_before, g_free_top);
		rc = -1;
		goto out;
	}
	rc = 0;
out:
	free(data);
	return rc;
}

static int
t14_model_switch(void)
{
	uint8_t *data = malloc(slot_bytes());
	struct superblock disk;
	uint8_t id[16];
	int rbuf, rc;

	if (data == NULL) {
		return -1;
	}

	reactor_pause();

	/* 1. the store's own superblock reads and validates */
	rc = store_sb_read(&disk);
	if (rc != 0 || store_sb_geometry_ok(&disk) != 0) {
		printf("    sb read/geometry failed on valid store (%d)\n", rc);
		goto fail_paused;
	}
	if (disk.nslots != g_nslots) {
		printf("    sb capacity %u != active %u\n", disk.nslots, g_nslots);
		goto fail_paused;
	}

	/* 2. garbage reads as "not formatted" */
	rbuf = io_buf_pop();
	if (rbuf < 0) {
		goto fail_paused;
	}
	memset(g_io_bufs[rbuf], 0, g_sector_size);
	rc = sector_batch(sb_lba(), 1, g_io_bufs[rbuf], 1);
	io_buf_push((uint32_t)rbuf);
	if (rc != 0) {
		goto fail_paused;
	}
	if (store_sb_read(&disk) != -ENOENT) {
		printf("    unformatted region not detected\n");
		goto fail_paused;
	}

	/* 3. restore the superblock */
	rc = store_sb_write();
	if (rc != 0 || store_sb_read(&disk) != 0 ||
	    store_sb_geometry_ok(&disk) != 0) {
		printf("    restored superblock does not validate\n");
		goto fail_paused;
	}

	/* 4. forge a leftover superblock from a different model */
	rbuf = io_buf_pop();
	if (rbuf < 0) {
		goto fail_paused;
	}
	{
		struct superblock *sb = g_io_bufs[rbuf];

		build_superblock(sb);
		sb->shard_sectors = g_shard_sectors * 2;
		sb->slot_sectors = g_slot_sectors * 2;
		sb->nslots = g_nslots / 2;    /* same byte capacity */
		sb->checksum = (uint64_t)hash_fn_n(sb, SB_COVER);
		rc = sector_batch(sb_lba(), 1, sb, 1);
	}
	io_buf_push((uint32_t)rbuf);
	if (rc != 0) {
		goto fail_paused;
	}

	/* 5. re-run region resolution: the runtime model must win and the
	 *    whole store is invalidated in place (g_format is cleared so
	 *    the normal boot path, not the explicit-format path, runs) */
	{
		int saved_format = g_format;

		g_format = 0;
		g_region_reset = 0;
		rc = store_resolve_region();
		g_format = saved_format;
	}
	if (rc != 0) {
		printf("    region resolution failed (%d)\n", rc);
		goto fail_paused;
	}
	if (!g_region_reset) {
		printf("    model switch not detected\n");
		goto fail_paused;
	}

	/* 6. rebuild the shared index exactly like a post-switch boot: the
	 *    switch dropped the stale segment, so create a fresh one and
	 *    recover it from the wiped region */
	shm_close_segment();
	rc = shm_open_segment(1, 0);
	if (rc != 0) {
		printf("    segment re-create failed (%d)\n", rc);
		goto fail_paused;
	}
	rc = store_recover();
	if (rc != 0) {
		printf("    recovery after switch failed (%d)\n", rc);
		goto fail_paused;
	}
	reactor_resume();
	shm_register_worker();

	/* 7. every pre-switch block is gone; counts are consistent */
	for (uint32_t b = 0; b < 8; b++) {
		gen_test_id(id, 2, b);
		if (store_lookup(id)) {
			printf("    pre-switch block %u survived\n", b);
			goto fail;
		}
	}
	if (g_live_count != 0 || g_free_top != g_nslots) {
		printf("    counts wrong after switch: live %u free %u nslots %u\n",
		       g_live_count, g_free_top, g_nslots);
		goto fail;
	}

	/* 8. the store serves the current model again */
	memset(data, 0x77, slot_bytes());
	gen_test_id(id, 14, 0);
	rc = dump_block_sync(id, data);
	if (rc != 0) {
		printf("    dump after switch failed (%d)\n", rc);
		goto fail;
	}
	rc = load_block_sync(id, data);
	if (rc != 0) {
		printf("    load after switch failed (%d)\n", rc);
		goto fail;
	}
	for (size_t i = 0; i < slot_bytes(); i++) {
		if (data[i] != 0x77) {
			printf("    content mismatch after switch\n");
			goto fail;
		}
	}
	free(data);
	return 0;

fail_paused:
	reactor_resume();
fail:
	free(data);
	return -1;
}

static int
t15_shm_direct_dma(void)
{
	char name[64];
	size_t one = slot_bytes();
	size_t size = 2 * one;
	uint8_t *mem;
	uint8_t id[16];
	int fd, rc = -1;

	snprintf(name, sizeof(name), "/ucm_spdk_reg_test_%d", getpid());
	shm_unlink(name);
	fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
		printf("    shm create failed\n");
		if (fd >= 0) { close(fd); }
		return -1;
	}
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (mem == MAP_FAILED) {
		shm_unlink(name);
		return -1;
	}
	madvise(mem, size, MADV_HUGEPAGE);
	memset(mem, 0x6a, one);          /* fault source pages */
	memset(mem + one, 0, one);       /* fault destination pages */

	rc = store_register_memory(mem, size);
	if (rc == -ENOTSUP && g_trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		printf("    PCIe SHM pages are not vtophys/DMA-mappable; "
		       "direct-DMA test skipped (nvmf accepts 4K pages)\n");
		rc = 0;
		goto out;
	}
	if (rc != 0) {
		printf("    spdk_mem_register(SHM) failed (%d)\n", rc);
		goto out;
	}
	gen_test_id(id, 15, 0);
	rc = dump_block_sync(id, mem);
	if (rc == 0) { rc = load_block_sync(id, mem + one); }
	if (rc == 0 && memcmp(mem, mem + one, one) != 0) {
		printf("    direct-DMA content mismatch\n");
		rc = -1;
	}
	if (store_unregister_memory(mem, size) != 0) {
		printf("    spdk_mem_unregister(SHM) failed\n");
		rc = -1;
	}
	out:
	munmap(mem, size);
	shm_unlink(name);
	return rc;
}

/* ---------------- main ---------------- */

static void
usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  SPDK-backed UCM store prototype with built-in unit tests.\n");
	printf("\n");
	printf("  -r <trid>     Transport ID, e.g. 'trtype:PCIe traddr:0000:84:00.0'\n");
	printf("                or RDMA: 'trtype:RDMA adrfam:IPv4 traddr:IP trsvcid:4420'\n");
	printf("  -O <GiB>      LBA offset of the slot pool (default 0)\n");
	printf("  -L <layers>   shards per block (default 27, DeepSeek-V2-Lite MLA)\n");
	printf("                the geometry is a runtime model property: a change\n");
	printf("                on an existing region invalidates all blocks and\n");
	printf("                reformats in place (byte capacity preserved)\n");
	printf("  -K <KB>       shard size in KB (default 144)\n");
	printf("  -n <slots>    capacity in blocks. Format (-F): sets it (default 64).\n");
	printf("                Normal boot: capacity comes from the superblock;\n");
	printf("                passing -n requests an in-place resize (grow always\n");
	printf("                safe; shrink refused while live blocks sit beyond)\n");
	printf("                Lookup role: not needed (taken from the segment)\n");
	printf("  -s <seed>     PRNG seed for test block ids (default 1)\n");
	printf("  -m <role>     worker (default): full store + tests;\n");
	printf("                lookup: attach the shared index only, no device,\n");
	printf("                verify cross-process visibility (no geometry needed;\n");
	printf("                the segment is region-scoped, like the UCM scheduler)\n");
	printf("  -T <n>        run only test n (1-15); default all\n");
	printf("  -F            format: write the superblock, zero the index region\n");
	printf("  -H            check /dev/shm is hugepage-capable (UCM config must set\n");
	printf("                shm_hugepage_advise; direct registration of 4K-backed\n");
	printf("                buffers is impractical at 100 GiB scale)\n");
	printf("  -h            Show this help\n");
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
store_device_cleanup(void)
{
	reactor_stop();
	while (g_reg_count > 0) {
		struct reg_range r = g_reg_cache[g_reg_count - 1];

		if (r.owned && spdk_mem_unregister((void *)r.start, r.end - r.start) != 0) {
			break;
		}
		g_reg_total_bytes -= r.end - r.start;
		g_reg_count--;
	}
	g_direct_dma = 0;
	shm_close_segment();
	store_lease_close();
	store_maintenance_close();
	for (int i = 0; i < IO_POOL_BUFS; i++) {
		if (g_io_bufs != NULL && g_io_bufs[i] != NULL) {
			spdk_free(g_io_bufs[i]);
		}
	}
	for (int i = 0; i < NUM_IDX_BUFS; i++) {
		if (g_idx_bufs != NULL && g_idx_bufs[i] != NULL) {
			spdk_free(g_idx_bufs[i]);
		}
	}
	free(g_io_bufs);
	free(g_idx_bufs);
	g_io_bufs = NULL;
	g_idx_bufs = NULL;
	g_io_free_top = 0;
	g_idx_free_top = 0;
	if (g_qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
		g_qpair = NULL;
	}
	if (g_ctrlr != NULL) {
		struct spdk_nvme_detach_ctx *detach_ctx = NULL;

		spdk_nvme_detach_async(g_ctrlr, &detach_ctx);
		if (detach_ctx) {
			spdk_nvme_detach_poll(detach_ctx);
		}
		g_ctrlr = NULL;
		g_ns = NULL;
	}
}

/*
 * store_engine_init — programmatic initialization (extracted from main).
 *
 * Called by the UCM .so wrapper (spdk_store.cc → engine_shim.c). Sets up
 * SPDK env, probes the device, configures geometry, opens/creates the SHM
 * segment, runs superblock validation/recovery and starts the reactor.
 *
 * Returns 0 on success; -ENOENT when the device is unavailable (caller
 * should fall back to lookup-only mode); negative errno otherwise.
 */
int
store_engine_init(const char *trid_str, uint32_t nslots_req,
		  uint32_t layers, uint32_t shard_kb, uint64_t offset_gib,
		  int format)
{
	struct spdk_env_opts opts;
	int rc;
	int env_started = 0;

	/* 1. parameters (from config, not CLI) */
	g_layers = layers;
	g_shard_kb = shard_kb;
	g_offset_gib = offset_gib;
	g_nslots = nslots_req;
	g_format = format;

	if (g_layers == 0 || g_layers > 256 || g_shard_kb == 0) {
		return -EINVAL;
	}
	if (g_shard_kb * 1024 % 512 != 0) {
		return -EINVAL;
	}

	/* 2. SPDK env (idempotent — second call is a no-op if same opts) */
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "ucm_spdk_store";
	if (geteuid() != 0) {
		/* No pagemap access without root: DPDK cannot use physical
		 * addresses. Virtual-address IOVA is the standard mode for
		 * RDMA (ibv_reg_mr maps VA directly); direct PCIe keeps the
		 * PA default and is root-only anyway (vfio). */
		opts.iova_mode = "va";
	}
	if (spdk_env_init(&opts) < 0) {
		return -ENOMEM;
	}
	env_started = 1;

	/* 3. parse trid */
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	if (trid_str != NULL && trid_str[0] != '\0') {
		if (spdk_nvme_transport_id_parse(&g_trid, trid_str) != 0) {
			rc = -EINVAL;
			goto fail;
		}
	}
	/* 4. probe */
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0 || g_ctrlr == NULL || g_ns == NULL) {
		rc = -ENODEV;
		goto fail;   /* device unavailable → lookup-only */
	}

	/* 5. geometry */
	g_sector_size = spdk_nvme_ns_get_sector_size(g_ns);
	g_num_sectors = spdk_nvme_ns_get_num_sectors(g_ns);
	if (g_sector_size == 0 || ((uint64_t)g_shard_kb * 1024) % g_sector_size != 0) {
		rc = -EINVAL;
		goto fail;
	}
	g_shard_sectors = (uint32_t)((uint64_t)g_shard_kb * 1024 / g_sector_size);
	g_slot_sectors = g_shard_sectors * g_layers;
	g_nshards = g_layers;
	g_data_base = g_offset_gib * (1024ULL * 1024 * 1024) / g_sector_size;
	g_index_slots = (uint32_t)(INDEX_RESERVED_BYTES / g_sector_size);
	g_index_base = g_data_base + 1;
	g_pool_base = g_index_base + g_index_slots;
	g_recov_batch = RECOV_BATCH_MAX;
	if (g_shard_sectors < g_recov_batch) {
		g_recov_batch = g_shard_sectors;
	}

	if (g_data_base + 1 + g_index_slots >= g_num_sectors) {
		rc = -ENOMEM;
		goto fail;
	}
	{
		uint64_t pool_sectors = g_num_sectors - g_pool_base;

		g_pool_capacity = pool_sectors / g_slot_sectors;
	}
	build_shm_names();
	rc = store_maintenance_open(g_format);
	if (rc != 0) {
		goto fail;
	}
	rc = store_lease_open();
	if (rc != 0) {
		goto fail;
	}

	/* 6. qpair + buffers */
	{
		struct spdk_nvme_io_qpair_opts qopts;
		uint32_t max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
		uint32_t split = (shard_bytes() + max_xfer - 1) / max_xfer;
		uint32_t need = IO_POOL_BUFS * split + NUM_IDX_BUFS + 16;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts, sizeof(qopts));
		if (need > qopts.io_queue_size) {
			qopts.io_queue_size = (need > 1024) ? 1024 : need;
		}
		if (qopts.io_queue_requests < 16384) {
			qopts.io_queue_requests = 16384;
		}
		g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qopts, sizeof(qopts));
	}
	if (g_qpair == NULL) {
		rc = -ENOMEM;
		goto fail;
	}

	g_io_bufs = calloc(IO_POOL_BUFS, sizeof(void *));
	g_idx_bufs = calloc(NUM_IDX_BUFS, sizeof(void *));
	if (g_io_bufs == NULL || g_idx_bufs == NULL) {
		rc = -ENOMEM;
		goto fail;
	}
	for (int i = 0; i < IO_POOL_BUFS; i++) {
		g_io_bufs[i] = spdk_zmalloc(shard_bytes(), 4096, NULL,
					    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_io_bufs[i] == NULL) {
			rc = -ENOMEM;
			goto fail;
		}
		g_io_free[g_io_free_top++] = (uint32_t)i;
	}
	for (int i = 0; i < NUM_IDX_BUFS; i++) {
		g_idx_bufs[i] = spdk_zmalloc(g_sector_size, 4096, NULL,
					     SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_idx_bufs[i] == NULL) {
			rc = -ENOMEM;
			goto fail;
		}
		g_idx_free[g_idx_free_top++] = (uint32_t)i;
	}

	/* 7. region resolution / SHM / recovery
	 * The store serves the runtime model geometry: a geometry change
	 * (model switch) invalidates all existing blocks in place instead
	 * of refusing to boot. */
	g_last_progress = spdk_get_ticks();
	rc = store_resolve_region();
	if (rc != 0) { goto fail; }

	if (shm_open_segment(0, 0) != 0) {
		rc = shm_open_segment(1, 0);
		if (rc == -EEXIST) {
			rc = shm_open_segment(0, 0);
		}
		if (rc != 0) {
			rc = -ENOMEM;
			goto fail;
		}
	}

	if (g_shm_recovery_owner || g_shm_created) {
		g_shm_recovering = 1;
		rc = store_recover();
		g_shm_recovering = 0;
		if (rc != 0) { goto fail; }
		shm_recovery_done();
	}
	rc = shm_register_worker();
	if (rc != 0) { goto fail; }

	/* 8. reactor */
	rc = reactor_start();
	if (rc != 0) { goto fail; }

	return 0;

fail:
	store_device_cleanup();
	if (env_started) {
		spdk_env_fini();
	}
	return rc;
}

void
store_engine_fini(void)
{
	store_device_cleanup();
	spdk_env_fini();
}

/* Scheduler role: no device, qpair or reactor; attach the worker-built
 * shared index and serve lookup calls only. Shared-only lease: never
 * the recovery owner, never blocking a worker's boot. */
int
store_lookup_init(void)
{
	int rc;

	build_shm_names();
	rc = store_maintenance_open(0);
	if (rc != 0) {
		return rc;
	}
	rc = store_lease_open_shared();
	if (rc != 0) {
		store_maintenance_close();
		return rc;
	}
	rc = shm_open_segment(0, 1);
	if (rc != 0) {
		store_lease_close();
		store_maintenance_close();
		return rc;
	}
	rc = shm_register_worker();
	if (rc != 0) {
		shm_close_segment();
		store_lease_close();
		store_maintenance_close();
	}
	return rc;
}

int
store_lookup_init_region(const char *trid_str, uint64_t offset_gib)
{
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	if (trid_str != NULL && trid_str[0] != '\0' &&
	    spdk_nvme_transport_id_parse(&g_trid, trid_str) != 0) {
		return -EINVAL;
	}
	g_offset_gib = offset_gib;
	return store_lookup_init();
}

void
store_lookup_fini(void)
{
	shm_close_segment();
	store_lease_close();
	store_maintenance_close();
}

/* Public accessors for the .so wrapper (engine_shim.c) */
uint64_t store_live_count(void) { return g_live_count; }
uint32_t store_nslots(void) { return g_nslots; }
uint32_t store_slot_sectors(void) { return g_slot_sectors; }
uint32_t store_sector_size(void) { return g_sector_size; }
int store_is_healthy(void) { return g_healthy; }
uint64_t store_epoch_value(void) { return g_epoch; }
int store_gc_public(int max_victims) { return store_gc(max_victims); }
void store_set_direct_dma(int enable) { g_direct_dma = enable; }
uint64_t store_registered_bytes(void) { return g_reg_total_bytes; }

#ifndef ENGINE_LIB
/* standalone test binary entry point; excluded when built as a library */
int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	int op, rc, i;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:O:L:K:n:s:m:T:FHh")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "bad trid: %s\n", optarg);
				return 1;
			}
			break;
		case 'O': g_offset_gib = (uint64_t)spdk_strtol(optarg, 10); break;
		case 'L': g_layers = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'K': g_shard_kb = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'n': g_nslots = (uint32_t)spdk_strtol(optarg, 10); break;
		case 's': g_seed = (uint32_t)spdk_strtol(optarg, 10); break;
		case 'm':
			if (strcmp(optarg, "lookup") == 0) {
				g_role = ROLE_LOOKUP;
			} else if (strcmp(optarg, "worker") == 0) {
				g_role = ROLE_WORKER;
			} else {
				fprintf(stderr, "bad role: %s (worker|lookup)\n", optarg);
				return 1;
			}
			break;
		case 'T': g_only_test = (int)spdk_strtol(optarg, 10); break;
		case 'F': g_format = 1; break;
		case 'H': g_check_hugepage = 1; break;
		case 'h':
		default:
			usage(argv[0]);
			return (op == 'h') ? 0 : 1;
		}
	}

	if (g_layers == 0 || g_layers > 256 || g_shard_kb == 0) {
		fprintf(stderr, "invalid layers/shard size\n");
		return 1;
	}
	if (g_nslots > 10000000u) {
		fprintf(stderr, "invalid slot count\n");
		return 1;
	}

	if (g_check_hugepage) {
		/* the SPDK store requires shm_hugepage_advise + huge=advise:
		 * fail fast when the deployment forgot either */
		if (check_shm_hugepage_advise() != 0) {
			return 1;
		}
	}

	if (g_role == ROLE_LOOKUP) {
		/* scheduler-role process: shared index only, no device;
		 * the segment's own header decides the capacity */
		rc = store_lookup_init();
		if (rc != 0) {
			fprintf(stderr, "no valid shared segment at %s (%d; "
				"start a worker first)\n", g_shm_name, rc);
			return 1;
		}
		printf("Lookup role: %u live, %u free slots, epoch %ju.\n",
		       g_live_count, g_free_top, (uintmax_t)g_epoch);
		rc = lookup_role_check();
		printf("Lookup check: %s\n\n", rc == 0 ? "PASS" : "FAIL");
		shm_close_segment();
		store_lease_close();
		store_maintenance_close();
		return rc == 0 ? 0 : 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "ucm_spdk_store";
	if (geteuid() != 0) {
		/* see store_engine_init(): non-root must use VA IOVA */
		opts.iova_mode = "va";
	}
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
	if ((uint64_t)g_shard_kb * 1024 % g_sector_size != 0) {
		fprintf(stderr, "shard size not sector-aligned\n");
		rc = 1;
		goto exit_fini;
	}
	g_shard_sectors = (uint32_t)((uint64_t)g_shard_kb * 1024 / g_sector_size);
	g_slot_sectors = g_shard_sectors * g_layers;
	g_nshards = g_layers;
	g_data_base = g_offset_gib * (1024ULL * 1024 * 1024) / g_sector_size;
	g_index_slots = (uint32_t)(INDEX_RESERVED_BYTES / g_sector_size);
	g_index_base = g_data_base + 1;
	g_pool_base = g_index_base + g_index_slots;
	g_recov_batch = RECOV_BATCH_MAX;
	if (g_shard_sectors < g_recov_batch) {
		g_recov_batch = g_shard_sectors;
	}

	if (g_data_base + 1 + g_index_slots >= g_num_sectors) {
		fprintf(stderr, "metadata reservation (%lu MiB) does not fit "
			"at offset %lu GiB\n",
			(unsigned long)(INDEX_RESERVED_BYTES >> 20),
			(unsigned long)g_offset_gib);
		rc = 1;
		goto exit_fini;
	}
	build_shm_names();
	rc = store_maintenance_open(g_format);
	if (rc != 0) {
		fprintf(stderr, "store maintenance lease failed (%d)\n", rc);
		rc = 1;
		goto exit_free;
	}
	rc = store_lease_open();
	if (rc != 0) {
		fprintf(stderr, "store lease failed (%d)\n", rc);
		rc = 1;
		goto exit_free;
	}
	{
		uint64_t pool_sectors = g_num_sectors - g_pool_base;

		g_pool_capacity = pool_sectors / g_slot_sectors;
	}

	{
		struct spdk_nvme_io_qpair_opts qopts;
		uint32_t max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
		uint32_t split = (shard_bytes() + max_xfer - 1) / max_xfer;
		uint32_t need = IO_POOL_BUFS * split + NUM_IDX_BUFS + 16;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts, sizeof(qopts));
		if (need > qopts.io_queue_size) {
			qopts.io_queue_size = (need > 1024) ? 1024 : need;
		}
		if (qopts.io_queue_requests < 16384) {
			qopts.io_queue_requests = 16384;
		}
		g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qopts, sizeof(qopts));
	}
	if (g_qpair == NULL) {
		fprintf(stderr, "alloc_io_qpair failed\n");
		rc = 1;
		goto exit_fini;
	}

	g_io_bufs = calloc(IO_POOL_BUFS, sizeof(void *));
	g_idx_bufs = calloc(NUM_IDX_BUFS, sizeof(void *));
	if (g_io_bufs == NULL || g_idx_bufs == NULL) {
		fprintf(stderr, "buffer array alloc failed\n");
		rc = 1;
		goto exit_free;
	}

	for (i = 0; i < IO_POOL_BUFS; i++) {
		g_io_bufs[i] = spdk_zmalloc(shard_bytes(), 4096, NULL,
					    SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_io_bufs[i] == NULL) {
			fprintf(stderr, "io buffer alloc failed at %d\n", i);
			rc = 1;
			goto exit_free;
		}
		g_io_free[g_io_free_top++] = (uint32_t)i;
	}
	for (i = 0; i < NUM_IDX_BUFS; i++) {
		g_idx_bufs[i] = spdk_zmalloc(g_sector_size, 4096, NULL,
					     SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_idx_bufs[i] == NULL) {
			fprintf(stderr, "index buffer alloc failed at %d\n", i);
			rc = 1;
			goto exit_free;
		}
		g_idx_free[g_idx_free_top++] = (uint32_t)i;
	}

	/* The store serves the runtime model geometry: a geometry change
	 * (model switch) invalidates all existing blocks in place instead
	 * of refusing to boot. Capacity is resolved from the region. */
	g_last_progress = spdk_get_ticks();
	rc = store_resolve_region();
	if (rc != 0) {
		rc = 1;
		goto exit_free;
	}

	printf("Store: capacity %u slots x %u sectors (%.2f GiB pool) | "
	       "index %lu MiB fixed | shard %uKB x %u | MDTS %uB\n",
	       g_nslots, g_slot_sectors,
	       (double)g_nslots * g_slot_sectors * g_sector_size
	       / (1024.0 * 1024 * 1024),
	       (unsigned long)(INDEX_RESERVED_BYTES >> 20),
	       g_shard_kb, g_nshards, spdk_nvme_ns_get_max_io_xfer_size(g_ns));

	/* Shared index: attach if a compatible segment exists, else create.
	 * Runs only after the on-disk geometry is confirmed. */
	if (shm_open_segment(0, 0) == 0) {
		printf("Attached shared index (%u live, %u free, epoch %ju).\n",
		       g_live_count, g_free_top, (uintmax_t)g_epoch);
	} else {
		rc = shm_open_segment(1, 0);
		if (rc == -EEXIST) {
			rc = shm_open_segment(0, 0);
		}
		if (rc != 0) {
			fprintf(stderr, "shared segment create failed\n");
			rc = 1;
			goto exit_free;
		}
	}

	if (g_shm_recovery_owner || g_shm_created) {
		g_shm_recovering = 1;
		rc = store_recover();
		g_shm_recovering = 0;
		if (rc != 0) {
			fprintf(stderr, "recovery failed (%d)\n", rc);
			rc = 1;
			goto exit_free;
		}
		shm_recovery_done();
	}
	if (shm_register_worker() != 0) {
		fprintf(stderr, "too many workers\n");
		rc = 1;
		goto exit_free;
	}
	printf("Recovered: %u live, %u free slots, epoch %ju.\n\n",
	       g_live_count, g_free_top, (uintmax_t)g_epoch);

	rc = reactor_start();
	if (rc != 0) {
		fprintf(stderr, "reactor start failed (%d)\n", rc);
		rc = 1;
		goto exit_free;
	}

	{
		int fresh = g_format;
		struct { int no; int (*fn)(void); int (*fn_fresh)(int); } cases[] = {
			{1,  NULL,               t1_consistency},
			{2,  t2_dump_and_lookup, NULL},
			{3,  t3_load_verify,     NULL},
			{4,  t4_prefix_semantics, NULL},
			{5,  t5_commit_on_last_shard, NULL},
			{6,  t6_overwrite,       NULL},
			{7,  t7_delete,          NULL},
			{8,  t8_recovery_rebuild, NULL},
			{9,  t9_corrupt_record,  NULL},
			{10, t10_gc_capacity,  NULL},
			{11, t11_interleaved_dumps, NULL},
			{12, t12_cross_process_lookup, NULL},
			{13, t13_backward_shift_stress, NULL},
			{14, t14_model_switch, NULL},
			{15, t15_shm_direct_dma, NULL},
		};
		int npass = 0, nfail = 0;

		for (size_t t = 0; t < sizeof(cases) / sizeof(cases[0]); t++) {
			int trc;

			if (g_only_test != 0 && g_only_test != cases[t].no) {
				continue;
			}
			printf("Test %2d: ", cases[t].no);
			fflush(stdout);
			if (cases[t].fn != NULL) {
				trc = cases[t].fn();
			} else {
				trc = cases[t].fn_fresh(fresh);
				fresh = 0;
			}
			if (trc == 0) {
				printf("PASS\n");
				npass++;
			} else {
				printf("FAIL\n");
				nfail++;
			}
		}

		printf("\n========== Summary ==========\n");
		printf("Tests       : %d pass, %d fail\n", npass, nfail);
		printf("Store       : %u/%u slots live, epoch %ju\n",
		       g_live_count, g_nslots, (uintmax_t)g_epoch);
		printf("Usage       : %.2f / %.2f GiB\n",
		       (double)g_live_count * slot_bytes() / (1024.0 * 1024 * 1024),
		       (double)g_nslots * slot_bytes() / (1024.0 * 1024 * 1024));
		printf("GC          : %ju passes, %ju blocks evicted\n",
		       (uintmax_t)g_gc_passes, (uintmax_t)g_gc_evicted);
		printf("Health      : %s, errors %ju\n",
		       g_healthy ? "OK" : "degraded", (uintmax_t)g_err_count);
		printf("=============================\n");
		rc = (nfail > 0 || g_err_count > 0) ? 1 : 0;
	}

	reactor_stop();

exit_free:
	for (i = 0; i < IO_POOL_BUFS; i++) {
		if (g_io_bufs != NULL && g_io_bufs[i] != NULL) {
			spdk_free(g_io_bufs[i]);
		}
	}
	for (i = 0; i < NUM_IDX_BUFS; i++) {
		if (g_idx_bufs != NULL && g_idx_bufs[i] != NULL) {
			spdk_free(g_idx_bufs[i]);
		}
	}
	free(g_io_bufs);
	free(g_idx_bufs);
	shm_close_segment();
	store_lease_close();
	store_maintenance_close();
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
#endif /* !ENGINE_LIB */
