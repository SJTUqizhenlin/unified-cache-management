/*
 * kernel_cache_test.c
 *
 * Simulates the disk I/O pattern of UCM (Unified Cache Management) POSIX
 * store (psync engine) for the GLM-5.3 model (78 layers, MLA,
 * kv_lora_rank=512 + rope=64). GLM-5.3 layers are homogeneous, so both
 * transfer modes are production configurations (UCM cache_posix_bw uses
 * glm + layerwise as its reference profile).
 *
 * Layerwise ON  (-W 1, UCM default): one shard per layer per block file.
 *   DUMP: open(.tmp) -> pwrite(shard) -> close  x78  (commit: rename on last)
 *   LOAD: access(final) [Cache-layer lookup] -> open(final) -> pread -> close x78
 * Layerwise OFF (-W 0): whole block transferred as a single shard.
 *   DUMP: open(.tmp) -> pwrite(whole block) -> close -> rename(.tmp -> final)
 *   LOAD: access(final) -> open(final) -> pread(whole block) -> close
 *
 * Mirrors UCM TransQueue: two engines selectable with -e.
 *   psync (default): separate dump/load thread pools (dataTransConcurrency
 *   each), workers block in pread/pwrite, commit on the worker finishing
 *   the last shard, no ftruncate, no fsync, O_DIRECT optional.
 *   aio: mirrors UCM's IoEngineAio — 32-thread open pool, libaio submit
 *   (one iocb per shard), a single completion thread harvesting
 *   io_getevents, and a 4-thread commit pool for rename/remove. Opened
 *   fds live from the open pool until the completion callback closes
 *   them, bounded by the aio context depth (4096, capped by RLIMIT_NOFILE).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <inttypes.h>
#include <ftw.h>
#include <libaio.h>

#define MAX_THREADS    256
#define MAX_BLOCKS     100000
#define BLOCK_ID_HEX_LEN 32

enum mode { MODE_DUMP, MODE_LOAD, MODE_MIXED };

struct shard_task {
    uint32_t block_index;
    uint32_t shard_index;
    int      is_write;
    int      is_last_shard;
    char     tmp_path[512];
    char     final_path[512];
    void    *buf;
    size_t   shard_size;
    uint64_t file_offset;
    struct timespec submit_ts;
    struct timespec open_ts;
    struct timespec io_ts;
    struct timespec close_ts;
    int      err;
};

struct thread_pool {
    pthread_t       threads[MAX_THREADS];
    int             num_threads;
    int             shutdown;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    struct shard_task *queue;
    int             queue_head;
    int             queue_tail;
    int             queue_size;
    int             queue_cap;
    int             active;
    pthread_mutex_t done_mtx;
    pthread_cond_t  done_cv;
    uint64_t        submitted;    /* monotonic task count */
    uint64_t        completed;    /* monotonic task count */
    uint64_t        wait_target;  /* completed >= wait_target wakes waiter, 0=none */
    uint64_t        waited_upto;  /* consumed by pool_wait_n() */
};

static uint32_t g_block_tokens    = 128;
static uint32_t g_num_layers      = 78;
static uint32_t g_kv_lora_rank    = 512;
static uint32_t g_qk_rope_dim     = 64;
static uint32_t g_bytes_per_elem  = 2;
static uint32_t g_num_blocks      = 500;
static uint32_t g_concurrency     = 32;
static uint32_t g_num_threads     = 128;
static int      g_io_direct       = 1;
static int      g_dir_shard       = 1;
static uint32_t g_dir_shard_bytes = 3;
static int      g_init_dump       = 1;
static int      g_layerwise       = 1;
static uint32_t g_replace_pct     = 100;
static int      g_target_rand     = 1;
static enum mode g_mode           = MODE_MIXED;
static uint32_t g_write_pct       = 50;
static int      g_runtime_sec     = 30;
static const char *g_storage_dir  = "/home/qizhenlin/nvme_test/cache_blocks";
static uint32_t g_seed            = 1;

static size_t   g_shard_size;
static size_t   g_block_file_size;
static size_t   g_kv_per_token;

static uint64_t g_completed_shards;
static uint64_t g_completed_blocks;
static uint64_t g_err_count;
static uint64_t g_total_open_ns;
static uint64_t g_total_io_ns;
static uint64_t g_total_close_ns;
static uint64_t g_total_rename_ns;
static uint64_t g_total_lookup_ns;
static uint64_t g_lookup_count;
static uint64_t g_lookup_miss;
static uint64_t g_replace_count;
static uint64_t g_evict_ns;
static uint64_t g_evict_count;
static uint64_t g_dup_skip;
static uint8_t *g_batch_slots;
static uint64_t g_du_bytes;
static uint64_t g_du_alloc;
static uint64_t g_du_files;
static uint64_t g_du_tmp_files;
static uint64_t g_total_bytes;

static char g_block_ids[MAX_BLOCKS][BLOCK_ID_HEX_LEN + 1];
static uint32_t g_dump_outstanding[MAX_BLOCKS];
static uint64_t g_dump_skip;

static void *
xmalloc(size_t sz)
{
    void *p = malloc(sz);
    if (!p) {
        fprintf(stderr, "malloc failed for %zu bytes\n", sz);
        abort();
    }
    return p;
}

static void *
xmemalign(size_t align, size_t sz)
{
    void *p;
    if (posix_memalign(&p, align, sz) != 0) {
        fprintf(stderr, "posix_memalign failed for %zu bytes\n", sz);
        abort();
    }
    return p;
}

static uint64_t
ts_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
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

static void
build_paths(const char *storage_dir, const char *block_id,
            char *final_path, size_t final_cap,
            char *tmp_path, size_t tmp_cap)
{
    if (g_dir_shard) {
        char prefix[8];
        uint32_t plen = g_dir_shard_bytes;
        if (plen > 7) { plen = 7; }
        memcpy(prefix, block_id, plen);
        prefix[plen] = '\0';
        snprintf(final_path, final_cap, "%s/%s/%s", storage_dir, prefix, block_id);
        snprintf(tmp_path, tmp_cap, "%s/%s/%s.tmp", storage_dir, prefix, block_id);
    } else {
        snprintf(final_path, final_cap, "%s/%s", storage_dir, block_id);
        snprintf(tmp_path, tmp_cap, "%s/%s.tmp", storage_dir, block_id);
    }
}

static int rename_block(const char *tmp_path, const char *final_path);
static int commit_block(struct shard_task *task, int success);

static int
do_shard_io(struct shard_task *task)
{
    int fd;
    int flags;
    ssize_t ret;
    struct timespec t1, t2;

    if (task->is_write) {
        flags = O_CREAT | O_WRONLY;
        if (g_io_direct) flags |= O_DIRECT;
    } else {
        flags = O_RDONLY;
        if (g_io_direct) flags |= O_DIRECT;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    fd = open(task->is_write ? task->tmp_path : task->final_path, flags, 0644);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                task->is_write ? task->tmp_path : task->final_path, strerror(errno));
        task->err = errno;
        commit_block(task, 0);
        return -1;
    }
    task->open_ts = t2;
    g_total_open_ns += ts_to_ns(&t2) - ts_to_ns(&t1);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (task->is_write) {
        ret = pwrite(fd, task->buf, task->shard_size, (off_t)task->file_offset);
    } else {
        ret = pread(fd, task->buf, task->shard_size, (off_t)task->file_offset);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    task->io_ts = t2;

    if (ret != (ssize_t)task->shard_size) {
        fprintf(stderr, "%s failed: ret=%zd, %s\n",
                task->is_write ? "pwrite" : "pread", ret, strerror(errno));
        task->err = errno;
        close(fd);
        commit_block(task, 0);
        return -1;
    }
    g_total_io_ns += ts_to_ns(&t2) - ts_to_ns(&t1);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(fd);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    task->close_ts = t2;
    g_total_close_ns += ts_to_ns(&t2) - ts_to_ns(&t1);

    if (commit_block(task, 1) != 0) {
        return -1;
    }
    return 0;
}

static int
rename_block(const char *tmp_path, const char *final_path)
{
    struct timespec t1, t2;
    int ret;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ret = rename(tmp_path, final_path);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    g_total_rename_ns += ts_to_ns(&t2) - ts_to_ns(&t1);
    if (ret != 0) {
        fprintf(stderr, "rename %s -> %s failed: %s\n", tmp_path, final_path, strerror(errno));
    }
    return ret;
}

/* Mimics UCM SpaceLayout::CommitFile: only the worker finishing the last
 * shard of a block commits; success renames .tmp -> final, failure removes
 * the .tmp file. */
static int
commit_block(struct shard_task *task, int success)
{
    if (!task->is_write || !task->is_last_shard) {
        return 0;
    }
    if (success && rename_block(task->tmp_path, task->final_path) == 0) {
        return 0;
    }
    remove(task->tmp_path);
    return -1;
}

static void *
worker_loop(void *arg)
{
    struct thread_pool *pool = (struct thread_pool *)arg;
    struct shard_task task;

    for (;;) {
        pthread_mutex_lock(&pool->mtx);
        while (pool->queue_head == pool->queue_tail && !pool->shutdown) {
            pthread_cond_wait(&pool->cv, &pool->mtx);
        }
        if (pool->shutdown && pool->queue_head == pool->queue_tail) {
            pthread_mutex_unlock(&pool->mtx);
            break;
        }
        /* Copy the task out of the ring: the slot is logically free once
         * popped and can be reused by later submissions while this task
         * executes (async dump backlog wraps the ring). */
        task = pool->queue[pool->queue_head % pool->queue_cap];
        pool->queue_head++;
        pool->active++;
        pthread_mutex_unlock(&pool->mtx);

        int rc = do_shard_io(&task);

        if (task.is_write) {
            if (__sync_fetch_and_add(&g_dump_outstanding[task.block_index], -1) == 1
                && rc == 0) {
                /* last shard of this dump generation completed */
                __sync_fetch_and_add(&g_completed_blocks, 1);
            }
        }
        if (rc == 0) {
            /* Count at completion, not submission: async dumps may finish
             * after the measured window. */
            __sync_fetch_and_add(&g_completed_shards, 1);
            __sync_fetch_and_add(&g_total_bytes, task.shard_size);
        }

        pthread_mutex_lock(&pool->mtx);
        pool->active--;
        pthread_mutex_unlock(&pool->mtx);

        pthread_mutex_lock(&pool->done_mtx);
        pool->completed++;
        if (pool->wait_target != 0 && pool->completed >= pool->wait_target) {
            pthread_cond_signal(&pool->done_cv);
        }
        pthread_mutex_unlock(&pool->done_mtx);

        if (rc != 0) {
            __sync_fetch_and_add(&g_err_count, 1);
        }
    }
    return NULL;
}

static void
pool_init(struct thread_pool *pool, int num_threads, int queue_cap)
{
    memset(pool, 0, sizeof(*pool));
    pool->num_threads = num_threads;
    pool->queue_cap = queue_cap;
    pool->queue = (struct shard_task *)xmalloc(sizeof(struct shard_task) * queue_cap);
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pthread_mutex_init(&pool->mtx, NULL);
    pthread_cond_init(&pool->cv, NULL);
    pthread_mutex_init(&pool->done_mtx, NULL);
    pthread_cond_init(&pool->done_cv, NULL);

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_loop, pool);
    }
}

static void
pool_submit(struct thread_pool *pool, struct shard_task *task)
{
    pthread_mutex_lock(&pool->done_mtx);
    pool->submitted++;
    pthread_mutex_unlock(&pool->done_mtx);

    pthread_mutex_lock(&pool->mtx);
    int idx = pool->queue_tail % pool->queue_cap;
    pool->queue[idx] = *task;
    pool->queue_tail++;
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mtx);
}

/* Wait for n more completions (relative to previous pool_wait_n calls). */
static void
pool_wait_n(struct thread_pool *pool, uint64_t n)
{
    pthread_mutex_lock(&pool->done_mtx);
    uint64_t target = pool->waited_upto + n;
    pool->wait_target = target;
    while (pool->completed < target) {
        pthread_cond_wait(&pool->done_cv, &pool->done_mtx);
    }
    pool->waited_upto = target;
    pool->wait_target = 0;
    pthread_mutex_unlock(&pool->done_mtx);
}

/* Backpressure for async submissions: block until the ring cannot overflow.
 * Ring usage = submitted - popped <= outstanding (completed lags popped by the
 * in-flight tasks, at most num_threads of them), so requiring
 * outstanding + n + margin <= cap with margin = num_threads + 1 guarantees
 * tail%cap never catches head%cap on a full ring. */
static void
pool_reserve(struct thread_pool *pool, int n)
{
    uint64_t margin = (uint64_t)pool->num_threads + 1;

    pthread_mutex_lock(&pool->done_mtx);
    while ((uint64_t)pool->queue_cap <
           (pool->submitted - pool->completed) + (uint64_t)n + margin) {
        uint64_t target = pool->submitted - ((uint64_t)pool->queue_cap - margin - (uint64_t)n);
        pool->wait_target = target;
        while (pool->completed < target) {
            pthread_cond_wait(&pool->done_cv, &pool->done_mtx);
        }
        pool->wait_target = 0;
    }
    pthread_mutex_unlock(&pool->done_mtx);
}

/* Wait for every submitted task of the pool. */
static void
pool_drain(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->done_mtx);
    uint64_t target = pool->submitted;
    pool->wait_target = target;
    while (pool->completed < target) {
        pthread_cond_wait(&pool->done_cv, &pool->done_mtx);
    }
    pool->waited_upto = target;
    pool->wait_target = 0;
    pthread_mutex_unlock(&pool->done_mtx);
}

static void
pool_destroy(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->mtx);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mtx);
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->queue);
    pthread_mutex_destroy(&pool->mtx);
    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->done_mtx);
    pthread_cond_destroy(&pool->done_cv);
}

/* ================= aio engine (mirrors UCM IoEngineAio) =================
 * Pipeline per shard: main -> open pool (32 threads, ::open) ->
 * io_submit (libaio, one iocb per shard) -> completion thread
 * (io_getevents, close) -> commit pool (4 threads, rename/remove for the
 * last shard of a dump). In-flight is bounded by the slot pool (aio ctx
 * depth), each slot holding one fd from open until completion.
 */

#define AIO_DEFAULT_DEPTH 4096
#define AIO_OPEN_THREADS  32      /* UCM openConcurrency */
#define AIO_COMMIT_THREADS 4      /* UCM commitConcurrency */
#define AIO_EVENT_BATCH   128

static uint32_t nshards(void);
static void *g_shard_buf;

struct aio_slot {
    struct iocb    iocb;
    int            fd;
    uint32_t       block_index;
    int            is_write;
    int            is_last_shard;
    size_t         shard_size;
    struct timespec submit_ts;
    char           tmp_path[512];
    char           final_path[512];
};

struct aio_commit_task {
    char tmp_path[512];
    char final_path[512];
    int  do_rename;   /* 0 = remove (failed dump) */
};

static int                 g_aio_mode;
static unsigned            g_open_conc = AIO_OPEN_THREADS;
static unsigned            g_commit_conc = AIO_COMMIT_THREADS;
static io_context_t        g_aio_ctx;
static struct aio_slot    *g_aio_slots;
static int                 g_aio_depth;
static int                *g_aio_free;
static int                 g_aio_free_top;
static uint64_t            g_aio_inflight;
static int                 g_aio_stop;
static pthread_mutex_t     g_aio_mtx = PTHREAD_MUTEX_INITIALIZER;

/* open queue (slot pointers) */
static struct aio_slot   **g_oq;
static int                 g_oq_cap, g_oq_head, g_oq_tail, g_oq_count;
static pthread_mutex_t     g_oq_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      g_oq_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t      g_oq_not_full = PTHREAD_COND_INITIALIZER;

/* commit queue */
static struct aio_commit_task *g_ctq;
static int                 g_ctq_cap, g_ctq_head, g_ctq_tail, g_ctq_count;
static pthread_mutex_t     g_ctq_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      g_ctq_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t      g_ctq_not_full = PTHREAD_COND_INITIALIZER;

/* load barrier */
static pthread_mutex_t     g_load_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      g_load_cv = PTHREAD_COND_INITIALIZER;
static uint64_t            g_load_remaining;

static pthread_t           g_aio_open_thr[AIO_OPEN_THREADS];
static pthread_t           g_aio_commit_thr[AIO_COMMIT_THREADS];
static pthread_t           g_aio_harvest_thr;

static void aio_finish(struct aio_slot *s, long res);

static void
aio_push_commit(const char *tmp, const char *final, int do_rename)
{
    pthread_mutex_lock(&g_ctq_mtx);
    while (g_ctq_count == g_ctq_cap && !g_aio_stop) {
        pthread_cond_wait(&g_ctq_not_full, &g_ctq_mtx);
    }
    if (g_ctq_count < g_ctq_cap) {
        struct aio_commit_task *t = &g_ctq[g_ctq_tail % g_ctq_cap];
        snprintf(t->tmp_path, sizeof(t->tmp_path), "%s", tmp);
        snprintf(t->final_path, sizeof(t->final_path), "%s", final);
        t->do_rename = do_rename;
        g_ctq_tail++;
        g_ctq_count++;
        pthread_cond_signal(&g_ctq_not_empty);
    }
    pthread_mutex_unlock(&g_ctq_mtx);
}

static void *
aio_commit_loop(void *arg)
{
    (void)arg;
    for (;;) {
        struct aio_commit_task t;
        pthread_mutex_lock(&g_ctq_mtx);
        while (g_ctq_count == 0 && !g_aio_stop) {
            pthread_cond_wait(&g_ctq_not_empty, &g_ctq_mtx);
        }
        if (g_ctq_count == 0 && g_aio_stop) {
            pthread_mutex_unlock(&g_ctq_mtx);
            break;
        }
        t = g_ctq[g_ctq_head % g_ctq_cap];
        g_ctq_head++;
        g_ctq_count--;
        pthread_cond_signal(&g_ctq_not_full);
        pthread_mutex_unlock(&g_ctq_mtx);

        if (t.do_rename) {
            if (rename_block(t.tmp_path, t.final_path) != 0) {
                remove(t.tmp_path);
                __sync_fetch_and_add(&g_err_count, 1);
            }
        } else {
            remove(t.tmp_path);
        }
    }
    return NULL;
}

static void *
aio_open_loop(void *arg)
{
    (void)arg;
    for (;;) {
        struct aio_slot *s;
        struct timespec t1, t2;
        int flags, rc;
        struct iocb *list;

        pthread_mutex_lock(&g_oq_mtx);
        while (g_oq_count == 0 && !g_aio_stop) {
            pthread_cond_wait(&g_oq_not_empty, &g_oq_mtx);
        }
        if (g_oq_count == 0 && g_aio_stop) {
            pthread_mutex_unlock(&g_oq_mtx);
            break;
        }
        s = g_oq[g_oq_head % g_oq_cap];
        g_oq_head++;
        g_oq_count--;
        pthread_cond_signal(&g_oq_not_full);
        pthread_mutex_unlock(&g_oq_mtx);

        if (s->is_write) {
            flags = O_CREAT | O_WRONLY;
            if (g_io_direct) { flags |= O_DIRECT; }
        } else {
            flags = O_RDONLY;
            if (g_io_direct) { flags |= O_DIRECT; }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        s->fd = open(s->is_write ? s->tmp_path : s->final_path, flags, 0644);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        __sync_add_and_fetch(&g_total_open_ns, ts_to_ns(&t2) - ts_to_ns(&t1));
        if (s->fd < 0) {
            fprintf(stderr, "open %s failed: %s\n",
                    s->is_write ? s->tmp_path : s->final_path, strerror(errno));
            __sync_fetch_and_add(&g_err_count, 1);
            aio_finish(s, -1);
            continue;
        }
        if (s->is_write) {
            io_prep_pwrite(&s->iocb, s->fd, g_shard_buf, s->shard_size,
                           (long long)(s->iocb.u.c.offset));
        } else {
            io_prep_pread(&s->iocb, s->fd, g_shard_buf, s->shard_size,
                          (long long)(s->iocb.u.c.offset));
        }
        s->iocb.data = s;
        clock_gettime(CLOCK_MONOTONIC, &s->submit_ts);
        list = &s->iocb;
        rc = io_submit(g_aio_ctx, 1, &list);
        if (rc != 1) {
            fprintf(stderr, "io_submit failed: %d (%s)\n", rc, strerror(-rc));
            close(s->fd);
            __sync_fetch_and_add(&g_err_count, 1);
            aio_finish(s, -1);
        }
    }
    return NULL;
}

static void
aio_finish(struct aio_slot *s, long res)
{
    struct timespec now, t1, t2;
    int ok = (res == (long)s->shard_size);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->fd >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        close(s->fd);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        __sync_add_and_fetch(&g_total_close_ns, ts_to_ns(&t2) - ts_to_ns(&t1));
    }
    if (ok) {
        __sync_add_and_fetch(&g_total_io_ns, ts_to_ns(&now) - ts_to_ns(&s->submit_ts));
        __sync_fetch_and_add(&g_completed_shards, 1);
        __sync_add_and_fetch(&g_total_bytes, s->shard_size);
    } else {
        __sync_fetch_and_add(&g_err_count, 1);
    }
    if (s->is_write) {
        if (__sync_fetch_and_add(&g_dump_outstanding[s->block_index], -1) == 1 && ok) {
            __sync_fetch_and_add(&g_completed_blocks, 1);
        }
        if (s->is_last_shard) {
            aio_push_commit(s->tmp_path, s->final_path, ok);
        }
    } else {
        pthread_mutex_lock(&g_load_mtx);
        g_load_remaining--;
        if (g_load_remaining == 0) {
            pthread_cond_broadcast(&g_load_cv);
        }
        pthread_mutex_unlock(&g_load_mtx);
    }

    pthread_mutex_lock(&g_aio_mtx);
    g_aio_free[g_aio_free_top++] = (int)(s - g_aio_slots);
    g_aio_inflight--;
    pthread_mutex_unlock(&g_aio_mtx);
}

static void
aio_process_events(int n, struct io_event *evs)
{
    for (int i = 0; i < n; i++) {
        aio_finish((struct aio_slot *)evs[i].data, evs[i].res);
    }
}

static void *
aio_harvest_loop(void *arg)
{
    struct io_event evs[AIO_EVENT_BATCH];
    struct timespec ts = {0, 10 * 1000 * 1000}; /* 10ms, like UCM epoll */

    (void)arg;
    while (!g_aio_stop) {
        int n = io_getevents(g_aio_ctx, 1, AIO_EVENT_BATCH, evs, &ts);
        if (n > 0) {
            aio_process_events(n, evs);
        }
    }
    return NULL;
}

/* Non-blocking harvest helper for submission-side backpressure. */
static void
aio_harvest_some(void)
{
    struct io_event evs[64];
    struct timespec ts0 = {0, 0};

    int n = io_getevents(g_aio_ctx, 0, 64, evs, &ts0);
    if (n > 0) {
        aio_process_events(n, evs);
    }
}

static void
aio_submit_task(const struct shard_task *t, const char *tmp_path,
                const char *final_path)
{
    struct aio_slot *s;

    pthread_mutex_lock(&g_aio_mtx);
    while (g_aio_free_top == 0) {
        pthread_mutex_unlock(&g_aio_mtx);
        aio_harvest_some();
        sched_yield();
        pthread_mutex_lock(&g_aio_mtx);
    }
    s = &g_aio_slots[g_aio_free[--g_aio_free_top]];
    g_aio_inflight++;
    pthread_mutex_unlock(&g_aio_mtx);

    s->fd = -1;
    s->block_index = t->block_index;
    s->is_write = t->is_write;
    s->is_last_shard = t->is_write && (t->shard_index == nshards() - 1);
    s->shard_size = t->shard_size;
    s->submit_ts.tv_sec = 0;
    snprintf(s->tmp_path, sizeof(s->tmp_path), "%s", tmp_path);
    snprintf(s->final_path, sizeof(s->final_path), "%s", final_path);
    /* reuse iocb.u.c.offset as the file offset carrier */
    s->iocb.u.c.offset = (long long)t->file_offset;

    if (!t->is_write) {
        pthread_mutex_lock(&g_load_mtx);
        g_load_remaining++;
        pthread_mutex_unlock(&g_load_mtx);
    }

    pthread_mutex_lock(&g_oq_mtx);
    while (g_oq_count == g_oq_cap && !g_aio_stop) {
        pthread_cond_wait(&g_oq_not_full, &g_oq_mtx);
    }
    g_oq[g_oq_tail % g_oq_cap] = s;
    g_oq_tail++;
    g_oq_count++;
    pthread_cond_signal(&g_oq_not_empty);
    pthread_mutex_unlock(&g_oq_mtx);
}

static void
aio_wait_loads(void)
{
    pthread_mutex_lock(&g_load_mtx);
    while (g_load_remaining > 0) {
        pthread_cond_wait(&g_load_cv, &g_load_mtx);
    }
    pthread_mutex_unlock(&g_load_mtx);
}

static void
aio_drain(void)
{
    for (;;) {
        uint64_t in;
        int oc, cc;

        pthread_mutex_lock(&g_aio_mtx);
        in = g_aio_inflight;
        pthread_mutex_unlock(&g_aio_mtx);
        pthread_mutex_lock(&g_oq_mtx);
        oc = g_oq_count;
        pthread_mutex_unlock(&g_oq_mtx);
        pthread_mutex_lock(&g_ctq_mtx);
        cc = g_ctq_count;
        pthread_mutex_unlock(&g_ctq_mtx);
        if (in == 0 && oc == 0 && cc == 0) {
            return;
        }
        usleep(500);
    }
}

static int
aio_engine_init(void)
{
    struct rlimit rl;
    int i;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return -1;
    }
    g_aio_depth = AIO_DEFAULT_DEPTH;
    if ((uint64_t)g_aio_depth > rl.rlim_cur - 256) {
        g_aio_depth = (int)(rl.rlim_cur - 256);
        printf("NOTE: aio depth capped to %d by RLIMIT_NOFILE (%lu)\n",
               g_aio_depth, (unsigned long)rl.rlim_cur);
    }
    if (g_aio_depth < 64) {
        fprintf(stderr, "aio depth too small (%d)\n", g_aio_depth);
        return -1;
    }
    if (g_open_conc > AIO_OPEN_THREADS) { g_open_conc = AIO_OPEN_THREADS; }
    if (g_commit_conc > AIO_COMMIT_THREADS) { g_commit_conc = AIO_COMMIT_THREADS; }

    memset(&g_aio_ctx, 0, sizeof(g_aio_ctx));
    if (io_setup(g_aio_depth, &g_aio_ctx) != 0) {
        fprintf(stderr, "io_setup(%d) failed\n", g_aio_depth);
        return -1;
    }
    g_aio_slots = calloc(g_aio_depth, sizeof(struct aio_slot));
    g_aio_free = calloc(g_aio_depth, sizeof(int));
    g_oq = calloc(g_aio_depth, sizeof(struct aio_slot *));
    g_ctq = calloc(g_aio_depth, sizeof(struct aio_commit_task));
    if (!g_aio_slots || !g_aio_free || !g_oq || !g_ctq) {
        return -1;
    }
    for (i = 0; i < g_aio_depth; i++) {
        g_aio_free[g_aio_free_top++] = i;
    }
    g_oq_cap = g_aio_depth;
    g_ctq_cap = g_aio_depth;

    for (i = 0; i < (int)g_open_conc; i++) {
        pthread_create(&g_aio_open_thr[i], NULL, aio_open_loop, NULL);
    }
    for (i = 0; i < (int)g_commit_conc; i++) {
        pthread_create(&g_aio_commit_thr[i], NULL, aio_commit_loop, NULL);
    }
    pthread_create(&g_aio_harvest_thr, NULL, aio_harvest_loop, NULL);
    return 0;
}

static void
aio_engine_destroy(void)
{
    int i;

    g_aio_stop = 1;
    pthread_mutex_lock(&g_oq_mtx);
    pthread_cond_broadcast(&g_oq_not_empty);
    pthread_cond_broadcast(&g_oq_not_full);
    pthread_mutex_unlock(&g_oq_mtx);
    pthread_mutex_lock(&g_ctq_mtx);
    pthread_cond_broadcast(&g_ctq_not_empty);
    pthread_cond_broadcast(&g_ctq_not_full);
    pthread_mutex_unlock(&g_ctq_mtx);
    for (i = 0; i < (int)g_open_conc; i++) {
        pthread_join(g_aio_open_thr[i], NULL);
    }
    for (i = 0; i < (int)g_commit_conc; i++) {
        pthread_join(g_aio_commit_thr[i], NULL);
    }
    pthread_join(g_aio_harvest_thr, NULL);
    io_destroy(g_aio_ctx);
    free(g_aio_slots);
    free(g_aio_free);
    free(g_oq);
    free(g_ctq);
}

static uint32_t
nshards(void)
{
    return g_layerwise ? g_num_layers : 1;
}

static int
prepare_storage_dir(void)
{
    char path[512];
    struct stat st;

    if (stat(g_storage_dir, &st) != 0) {
        if (mkdir(g_storage_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir %s failed: %s\n", g_storage_dir, strerror(errno));
            return -1;
        }
    }
    if (g_dir_shard) {
        uint32_t nComb = 1;
        for (uint32_t i = 0; i < g_dir_shard_bytes; i++) { nComb *= 16; }
        for (uint32_t i = 0; i < nComb; i++) {
            char prefix[8];
            const char hexChars[] = "0123456789abcdef";
            for (uint32_t j = 0; j < g_dir_shard_bytes; j++) {
                prefix[j] = hexChars[(i >> (4 * j)) & 0xF];
            }
            prefix[g_dir_shard_bytes] = '\0';
            snprintf(path, sizeof(path), "%s/%s", g_storage_dir, prefix);
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir %s failed: %s\n", path, strerror(errno));
                return -1;
            }
        }
    }
    return 0;
}

static int
du_cb(const char *fpath, const struct stat *st, int typeflag, struct FTW *ftwbuf)
{
    size_t len;

    (void)ftwbuf;
    if (typeflag != FTW_F || !S_ISREG(st->st_mode)) {
        return 0;
    }
    /* Orphan .tmp from the commit race: like UCM GC, count separately. */
    len = strlen(fpath);
    if (len >= 4 && strcmp(fpath + len - 4, ".tmp") == 0) {
        g_du_tmp_files++;
        return 0;
    }
    g_du_bytes += (uint64_t)st->st_size;
    g_du_alloc += (uint64_t)st->st_blocks * 512;
    g_du_files++;
    return 0;
}

static int
clear_storage_dir(void)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", g_storage_dir);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "clear_storage_dir failed: %d\n", ret);
        return -1;
    }
    return 0;
}

static void
usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  Simulates UCM POSIX store I/O for GLM-5.3 (78-layer MLA)\n");
    printf("\n");
    printf("  Engine (-e):\n");
    printf("    psync         128x2 blocking worker pools (UCM psync, default)\n");
    printf("    aio           32-thread open pool + libaio + completion thread +\n");
    printf("                  4-thread commit pool (UCM IoEngineAio)\n");
    printf("\n");
    printf("  Model parameters:\n");
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
    printf("    -W <0|1>      Layerwise: 1=per-layer shard I/O, 0=whole-block single I/O\n");
    printf("                  (default 1; both are production modes for GLM-5.3)\n");
    printf("    -m <mode>     dump | load | mixed (default mixed)\n");
    printf("    -M <pct>      Write %% for mixed mode (default 50)\n");
    printf("    -F <pct>      Id-replacement %% for dumps: chance a dumped block slot\n");
    printf("                  gets a fresh random file name (old file unlinked = eviction;\n");
    printf("                  no LRU) (default 100: real KV-cache logic, every dump is new\n");
    printf("                  content; use 0 for a stable id pool)\n");
    printf("    -S <mode>     Slot targeting: rand = random slot per operation (request-\n");
    printf("                  driven access, default) | seq = sequential cursor scan\n");
    printf("    -t <sec>      Run time in seconds (default 30)\n");
    printf("\n");
    printf("  I/O parameters:\n");
    printf("    -T <threads>  Worker threads (default 128, mimics UCM dataTransConcurrency)\n");
    printf("    -d <0|1>      O_DIRECT (default 1)\n");
    printf("    -D <0|1>      Directory sharding (default 1, UCM default)\n");
    printf("    -P <bytes>    Dir shard prefix bytes (default 3, UCM default)\n");
    printf("    -C <0|1>      Clear dir & init-dump all blocks on startup (default 1; -C 0 allows only dump mode)\n");
    printf("\n");
    printf("  Storage:\n");
    printf("    -f <dir>      Storage directory (default %s)\n", g_storage_dir);
    printf("    -s <seed>     PRNG seed (default 1)\n");
    printf("    -h            Show this help\n");
}

int
main(int argc, char **argv)
{
    int op, rc = 0;
    struct timespec start_ts, end_ts;
    struct thread_pool pool_dump, pool_load;
    void *shard_buf = NULL;
    const char *mode_names[] = {"dump", "load", "mixed"};

    srand(g_seed);
    while ((op = getopt(argc, argv, "B:L:K:R:E:n:c:m:M:t:T:d:D:P:C:W:F:S:e:f:s:h")) != -1) {
        switch (op) {
        case 'B': g_block_tokens = (uint32_t)atoi(optarg); break;
        case 'L': g_num_layers = (uint32_t)atoi(optarg); break;
        case 'K': g_kv_lora_rank = (uint32_t)atoi(optarg); break;
        case 'R': g_qk_rope_dim = (uint32_t)atoi(optarg); break;
        case 'E': g_bytes_per_elem = (uint32_t)atoi(optarg); break;
        case 'n': g_num_blocks = (uint32_t)atoi(optarg); break;
        case 'c': g_concurrency = (uint32_t)atoi(optarg); break;
        case 'm':
            if (strcmp(optarg, "dump") == 0) g_mode = MODE_DUMP;
            else if (strcmp(optarg, "load") == 0) g_mode = MODE_LOAD;
            else if (strcmp(optarg, "mixed") == 0) g_mode = MODE_MIXED;
            else { fprintf(stderr, "bad mode: %s\n", optarg); return 1; }
            break;
        case 'M': g_write_pct = (uint32_t)atoi(optarg); break;
        case 't': g_runtime_sec = atoi(optarg); break;
        case 'T': g_num_threads = (uint32_t)atoi(optarg); break;
        case 'd': g_io_direct = atoi(optarg); break;
        case 'D': g_dir_shard = atoi(optarg); break;
        case 'P': g_dir_shard_bytes = (uint32_t)atoi(optarg); break;
        case 'C': g_init_dump = atoi(optarg); break;
        case 'W': g_layerwise = atoi(optarg); break;
        case 'F': g_replace_pct = (uint32_t)atoi(optarg); break;
        case 'S':
            if (strcmp(optarg, "rand") == 0) { g_target_rand = 1; }
            else if (strcmp(optarg, "seq") == 0) { g_target_rand = 0; }
            else { fprintf(stderr, "bad target mode: %s (rand|seq)\n", optarg); return 1; }
            break;
        case 'e':
            if (strcmp(optarg, "psync") == 0) { g_aio_mode = 0; }
            else if (strcmp(optarg, "aio") == 0) { g_aio_mode = 1; }
            else { fprintf(stderr, "bad engine: %s (psync|aio)\n", optarg); return 1; }
            break;
        case 'f': g_storage_dir = optarg; break;
        case 's': g_seed = (uint32_t)atoi(optarg); srand(g_seed); break;
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
    if (g_num_threads == 0 || g_num_threads > MAX_THREADS) {
        fprintf(stderr, "invalid thread count %u (1-%d)\n", g_num_threads, MAX_THREADS);
        return 1;
    }
    if (g_concurrency == 0 || g_concurrency > g_num_blocks) {
        g_concurrency = g_num_blocks < 1 ? 1 : g_num_blocks;
    }

    if (g_num_blocks > MAX_BLOCKS) {
        fprintf(stderr, "num_blocks %u exceeds max %d\n", g_num_blocks, MAX_BLOCKS);
        return 1;
    }

    if (g_dir_shard_bytes == 0 || g_dir_shard_bytes > 4) {
        if (g_dir_shard_bytes == 0) g_dir_shard_bytes = 3;
        else { fprintf(stderr, "dir_shard_bytes must be 1-4\n"); return 1; }
    }
    if (g_dir_shard && g_dir_shard_bytes > 4) {
        fprintf(stderr, "dir_shard_bytes max 4\n");
        return 1;
    }

    gen_block_id_array(g_num_blocks);

    g_kv_per_token = (g_kv_lora_rank + g_qk_rope_dim) * g_bytes_per_elem;
    g_block_file_size = g_kv_per_token * g_block_tokens * g_num_layers;
    g_shard_size = g_layerwise ? g_kv_per_token * g_block_tokens : g_block_file_size;

    {
        size_t alignment = 4096;
        if (g_shard_size % alignment != 0) {
            size_t aligned = (g_shard_size + alignment - 1) / alignment * alignment;
            fprintf(stderr, "WARNING: shard_size %zu not 4K-aligned, rounding to %zu\n",
                    g_shard_size, aligned);
            g_shard_size = aligned;
            g_block_file_size = g_layerwise ? g_shard_size * g_num_layers : g_shard_size;
        }
    }

    printf("Model: GLM-5.3 | layers=%u | MLA(kv_lora_rank=%u, rope=%u) | %uB/elem\n",
           g_num_layers, g_kv_lora_rank, g_qk_rope_dim, g_bytes_per_elem);
    printf("vLLM block_size=%u tokens | kv_per_token=%zu B | shard_size=%zu B (%.1f KB) | "
           "block_file_size=%zu B (%.2f MB)\n",
           g_block_tokens, g_kv_per_token, g_shard_size,
           (double)g_shard_size / 1024.0, g_block_file_size,
           (double)g_block_file_size / (1024.0 * 1024.0));
    printf("Layerwise: %s | shards/block=%u | shard_size=%zu B | block_file_size=%zu B\n",
           g_layerwise ? "on (per-layer shard)" : "off (whole block)",
           nshards(), g_shard_size, g_block_file_size);
    printf("Workload: %s | blocks=%u | load-step=%u (dump=async) | engine=%s | O_DIRECT=%d | dir_shard=%d (bytes=%u)\n",
           mode_names[g_mode], g_num_blocks,
           g_concurrency, g_aio_mode ? "aio" : "psync(thread pools)",
           g_io_direct, g_dir_shard, g_dir_shard_bytes);
    if (g_aio_mode) {
        printf("AIO engine        : open pool %u threads, commit pool %u threads, ctx depth %d\n",
               g_open_conc, g_commit_conc, AIO_DEFAULT_DEPTH);
    } else {
        printf("psync engine      : %u threads x2 pools\n", g_num_threads);
    }
    if (g_replace_pct > 0) {
        printf("Id replacement   : %u%% of dumped blocks get a fresh file name (old unlinked)\n",
               g_replace_pct);
    }
    printf("Slot targeting   : %s\n",
           g_target_rand ? "rand (per-op random slot)" : "seq (cursor scan)");
    printf("Init-dump: %s | Run time: %d s | Storage: %s\n",
           g_init_dump ? "on (clear + pre-write)" : "off", g_runtime_sec, g_storage_dir);
    printf("\n");

    if (prepare_storage_dir() != 0) {
        return 1;
    }

    shard_buf = xmemalign(4096, g_shard_size);
    memset(shard_buf, 0xaa, g_shard_size);
    g_shard_buf = shard_buf;

    /* Per-batch slot dedup: with random targeting the same slot may be drawn
     * twice in one step; a load + dump pair on one slot would race (the
     * dump's unlink invalidates the load's open). UCM never collides load
     * and dump on the same block in one step, so skip duplicates. */
    g_batch_slots = (uint8_t *)xmalloc(g_num_blocks);
    memset(g_batch_slots, 0, g_num_blocks);

    if (g_aio_mode) {
        if (aio_engine_init() != 0) {
            fprintf(stderr, "aio engine init failed\n");
            return 1;
        }
    } else {
        int dump_cap = (int)(g_concurrency * nshards() + 4096);
        int load_cap = (int)(g_concurrency * nshards() + 256);
        pool_init(&pool_dump, g_num_threads, dump_cap);
        pool_init(&pool_load, g_num_threads, load_cap);
    }

    if (g_init_dump) {
        printf("Init-dump: clearing dir and pre-writing all %u blocks...\n", g_num_blocks);
        clear_storage_dir();
        prepare_storage_dir();

        if (g_aio_mode) {
            for (uint32_t b = 0; b < g_num_blocks; b++) {
                char final_path[512], tmp_path[512];
                build_paths(g_storage_dir, g_block_ids[b], final_path, sizeof(final_path),
                            tmp_path, sizeof(tmp_path));
                g_dump_outstanding[b] = nshards();
                for (uint32_t s = 0; s < nshards(); s++) {
                    struct shard_task task;
                    memset(&task, 0, sizeof(task));
                    task.block_index = b;
                    task.shard_index = s;
                    task.is_write = 1;
                    task.shard_size = g_shard_size;
                    task.file_offset = (uint64_t)s * g_shard_size;
                    snprintf(task.final_path, sizeof(task.final_path), "%s", final_path);
                    snprintf(task.tmp_path, sizeof(task.tmp_path), "%s", tmp_path);
                    aio_submit_task(&task, tmp_path, final_path);
                }
                if (b % 500 == 0) {
                    printf("\r  Progress: %u/%u blocks", b + 1, g_num_blocks);
                    fflush(stdout);
                }
            }
            aio_drain();
        } else {
        for (uint32_t b = 0; b < g_num_blocks; b += g_concurrency) {
            uint32_t n = g_concurrency;
            if (b + n > g_num_blocks) { n = g_num_blocks - b; }
            for (uint32_t j = 0; j < n; j++) {
                uint32_t bid = b + j;
                char final_path[512], tmp_path[512];
                build_paths(g_storage_dir, g_block_ids[bid], final_path, sizeof(final_path),
                            tmp_path, sizeof(tmp_path));
                g_dump_outstanding[bid] = nshards();
                for (uint32_t s = 0; s < nshards(); s++) {
                    struct shard_task task;
                    memset(&task, 0, sizeof(task));
                    task.block_index = bid;
                    task.shard_index = s;
                    task.is_write = 1;
                    task.is_last_shard = (s == nshards() - 1);
                    task.buf = shard_buf;
                    task.shard_size = g_shard_size;
                    task.file_offset = (uint64_t)s * g_shard_size;

                    snprintf(task.final_path, sizeof(task.final_path), "%s", final_path);
                    snprintf(task.tmp_path, sizeof(task.tmp_path), "%s", tmp_path);
                    clock_gettime(CLOCK_MONOTONIC, &task.submit_ts);
                    pool_submit(&pool_dump, &task);
                }
            }
            pool_wait_n(&pool_dump, (uint64_t)n * nshards());
            if ((b / g_concurrency) % 50 == 0) {
                printf("\r  Progress: %u/%u blocks", b + n, g_num_blocks);
                fflush(stdout);
            }
        }
        }
        printf("\nInit-dump done: %u blocks written.\n\n", g_num_blocks);

        /* Worker-side accounting also counts init-dump tasks; reset the
         * result counters here (they were absent historically because the
         * old main-thread accounting never saw init-dump). */
        g_completed_shards = 0;
        g_completed_blocks = 0;
        g_total_bytes = 0;
        g_total_open_ns = 0;
        g_total_io_ns = 0;
        g_total_close_ns = 0;
        g_total_rename_ns = 0;
        g_total_lookup_ns = 0;
        g_lookup_count = 0;
        g_lookup_miss = 0;
        g_dump_skip = 0;
        g_replace_count = 0;
        g_evict_ns = 0;
        g_evict_count = 0;
        g_dup_skip = 0;
    }

    {
        struct timespec deadline_ts;
        uint32_t block_idx = 0;

        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        deadline_ts.tv_sec = start_ts.tv_sec + g_runtime_sec;
        deadline_ts.tv_nsec = start_ts.tv_nsec;

        while (1) {
            struct timespec now;
            uint32_t nload_tasks = 0;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline_ts.tv_sec ||
                (now.tv_sec == deadline_ts.tv_sec && now.tv_nsec >= deadline_ts.tv_nsec)) {
                break;
            }
            if (g_num_blocks > 0 && block_idx >= g_num_blocks) {
                if (g_mode == MODE_DUMP || g_mode == MODE_MIXED) {
                    break;
                }
                block_idx = 0;
            }

            uint32_t batch_size = g_concurrency;
            for (uint32_t b = 0; b < batch_size; b++) {
                uint32_t bid;
                int is_write;
                char final_path[512], tmp_path[512];

                if (g_target_rand) {
                    bid = (uint32_t)rand() % g_num_blocks;
                } else {
                    bid = block_idx + b;
                    if (bid >= g_num_blocks) { break; }
                }
                if (g_batch_slots[bid]) {
                    g_dup_skip++;
                    continue;
                }
                build_paths(g_storage_dir, g_block_ids[bid],
                            final_path, sizeof(final_path),
                            tmp_path, sizeof(tmp_path));

                if (g_mode == MODE_DUMP) {
                    is_write = 1;
                } else if (g_mode == MODE_LOAD) {
                    is_write = 0;
                } else {
                    is_write = ((uint32_t)rand() % 100 < g_write_pct);
                }

                if (!is_write) {
                    /* Mimic UCM Cache-layer Lookup before load:
                     * access(F_OK|R_OK|W_OK) on the final path; a miss skips
                     * the block, exactly like UCM skips load on lookup miss. */
                    struct timespec t1, t2;
                    int hit;
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    hit = access(final_path, F_OK | R_OK | W_OK) == 0;
                    clock_gettime(CLOCK_MONOTONIC, &t2);
                    g_total_lookup_ns += ts_to_ns(&t2) - ts_to_ns(&t1);
                    g_lookup_count++;
                    if (!hit) {
                        g_lookup_miss++;
                        continue;
                    }
                }

                if (is_write) {
                    /* UCM dump semantics: fire-and-forget across steps, bounded
                     * only by queue backpressure (pool_reserve). A block whose
                     * previous dump generation is still in flight is skipped:
                     * UCM's cache layer never re-dumps a block concurrently. */
                    if (g_dump_outstanding[bid] > 0) {
                        g_dump_skip++;
                        continue;
                    }
                    /* Id replacement (eviction): the slot gets a fresh random
                     * file name. The directory tree is this side's index, so
                     * eviction = unlink of the old file (the analog of the
                     * SPDK side's tombstone + index-slot rewrite). Safe here:
                     * loads are barriered per step and this block's dumps all
                     * completed. */
                    if (g_replace_pct > 0 &&
                        (uint32_t)rand() % 100 < g_replace_pct) {
                        char old_final[512], old_tmp[512];
                        struct timespec e1, e2;

                        build_paths(g_storage_dir, g_block_ids[bid],
                                    old_final, sizeof(old_final),
                                    old_tmp, sizeof(old_tmp));
                        clock_gettime(CLOCK_MONOTONIC, &e1);
                        if (unlink(old_final) != 0 && errno != ENOENT) {
                            g_err_count++;
                        }
                        clock_gettime(CLOCK_MONOTONIC, &e2);
                        g_evict_ns += ts_to_ns(&e2) - ts_to_ns(&e1);
                        g_evict_count++;

                        gen_random_block_id(g_block_ids[bid],
                                            g_seed + (uint32_t)g_replace_count * 7919u + 3);
                        g_replace_count++;

                        build_paths(g_storage_dir, g_block_ids[bid],
                                    final_path, sizeof(final_path),
                                    tmp_path, sizeof(tmp_path));
                    }
                    g_dump_outstanding[bid] = nshards();
                    if (!g_aio_mode) {
                        pool_reserve(&pool_dump, (int)nshards());
                    }
                }
                for (uint32_t s = 0; s < nshards(); s++) {
                    struct shard_task task;
                    memset(&task, 0, sizeof(task));
                    task.block_index = bid;
                    task.shard_index = s;
                    task.is_write = is_write;
                    task.is_last_shard = is_write && (s == nshards() - 1);
                    task.buf = shard_buf;
                    task.shard_size = g_shard_size;
                    task.file_offset = (uint64_t)s * g_shard_size;


                    snprintf(task.final_path, sizeof(task.final_path), "%s", final_path);
                    snprintf(task.tmp_path, sizeof(task.tmp_path), "%s", tmp_path);
                    clock_gettime(CLOCK_MONOTONIC, &task.submit_ts);
                    if (g_aio_mode) {
                        aio_submit_task(&task, tmp_path, final_path);
                        if (!is_write) {
                            nload_tasks++;
                        }
                    } else if (is_write) {
                        pool_submit(&pool_dump, &task);
                    } else {
                        pool_submit(&pool_load, &task);
                        nload_tasks++;
                    }
                }
                g_batch_slots[bid] = 1;
            }

            /* UCM step semantics: only the step's loads gate the step
             * (wait_for_layer_load / task waiter); dumps drain in background. */
            if (g_aio_mode) {
                aio_wait_loads();
            } else {
                pool_wait_n(&pool_load, nload_tasks);
            }
            memset(g_batch_slots, 0, g_num_blocks);

            /* Read blocks completed with the barrier; dump blocks and shards
             * are counted at completion inside the workers. */
            g_completed_blocks += nload_tasks / nshards();

            block_idx += batch_size;
            if (g_num_blocks > 0 && block_idx >= g_num_blocks) {
                block_idx = 0;
            }

            if (g_completed_blocks % 100 == 0) {
                printf("\r  Progress: %ju blocks", (uintmax_t)g_completed_blocks);
                fflush(stdout);
            }
        }
    }
    printf("\n");

    clock_gettime(CLOCK_MONOTONIC, &end_ts);

    /* Snapshot result counters at deadline: the drain below finishes async
     * dumps OUTSIDE the measured window, and workers keep incrementing the
     * live counters during it. */
    {
        uint64_t s_shards = g_completed_shards;
        uint64_t s_blocks = g_completed_blocks;
        uint64_t s_bytes = g_total_bytes;
        uint64_t s_open = g_total_open_ns;
        uint64_t s_io = g_total_io_ns;
        uint64_t s_close = g_total_close_ns;
        uint64_t s_rename = g_total_rename_ns;
        uint64_t s_err = g_err_count;
        double elapsed_sec = (double)(ts_to_ns(&end_ts) - ts_to_ns(&start_ts)) / 1e9;
        double iops, mibs, blocks_per_sec;
        uint64_t total_open_close;

        /* UCM semantics: async dumps can still be in flight after the last
         * step; drain them (outside the measured window) before dir scan. */
        if (g_aio_mode) {
            aio_drain();
        } else {
            pool_drain(&pool_dump);
            pool_drain(&pool_load);
        }

        nftw(g_storage_dir, du_cb, 32, FTW_PHYS);

        iops = (elapsed_sec > 0) ? (double)s_shards / elapsed_sec : 0;
        mibs = (elapsed_sec > 0) ? (double)s_bytes / elapsed_sec / (1024.0 * 1024.0) : 0;
        blocks_per_sec = (elapsed_sec > 0) ? (double)s_blocks / elapsed_sec : 0;
        total_open_close = s_open + s_close;

        printf("\n========== Results ==========\n");
        printf("Shard IOPS        : %.2f\n", iops);
        printf("Block ops/s       : %.2f\n", blocks_per_sec);
        printf("Throughput        : %.2f MiB/s (%.2f GiB/s)\n", mibs, mibs / 1024.0);
        printf("Completed         : %ju shards, %ju blocks in %.2f s\n",
               (uintmax_t)s_shards, (uintmax_t)s_blocks, elapsed_sec);
        printf("Disk usage        : %.2f GiB allocated (%.2f GiB logical, %ju file(s)",
               (double)g_du_alloc / 1073741824.0,
               (double)g_du_bytes / 1073741824.0,
               (uintmax_t)g_du_files);
        if (g_du_tmp_files > 0) {
            printf(", %ju orphan .tmp", (uintmax_t)g_du_tmp_files);
        }
        printf(")\n");
        printf("\n");
        printf("--- Per-shard breakdown (avg) ---\n");
        if (s_shards > 0) {
            printf("open              : %.2f us  (%.1f%%)\n",
                   (double)s_open / s_shards / 1000.0,
                   (double)s_open / (total_open_close + s_io + s_rename) * 100);
            printf("I/O (pread/pwrite): %.2f us  (%.1f%%)\n",
                   (double)s_io / s_shards / 1000.0,
                   (double)s_io / (total_open_close + s_io + s_rename) * 100);
            printf("close             : %.2f us  (%.1f%%)\n",
                   (double)s_close / s_shards / 1000.0,
                   (double)s_close / (total_open_close + s_io + s_rename) * 100);
        }
        if (s_blocks > 0) {
            printf("rename/block      : %.2f us  (%.1f%% of total)\n",
                   (double)s_rename / s_blocks / 1000.0,
                   (double)s_rename / (total_open_close + s_io + s_rename) * 100);
        }
        if (g_lookup_count > 0) {
            printf("lookup (access)   : %.2f us avg  (%ju calls, %ju miss-skip)\n",
                   (double)g_total_lookup_ns / g_lookup_count / 1000.0,
                   (uintmax_t)g_lookup_count, (uintmax_t)g_lookup_miss);
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
            printf("id-replace        : %ju blocks (fresh id, file rewritten)\n",
                   (uintmax_t)g_replace_count);
        }
        if (g_evict_count > 0) {
            printf("evict (unlink)    : %.2f us avg  (%ju blocks)\n",
                   (double)g_evict_ns / g_evict_count / 1000.0,
                   (uintmax_t)g_evict_count);
        }
        printf("\n");
        printf("--- Totals ---\n");
        printf("open total        : %.3f s\n", (double)s_open / 1e9);
        printf("I/O total         : %.3f s\n", (double)s_io / 1e9);
        printf("close total       : %.3f s\n", (double)s_close / 1e9);
        printf("rename total      : %.3f s\n", (double)s_rename / 1e9);
        if (s_err) {
            printf("Errors            : %ju\n", (uintmax_t)s_err);
            rc = 1;
        }
        printf("=============================\n");
    }

    if (g_aio_mode) {
        aio_engine_destroy();
    } else {
        pool_destroy(&pool_dump);
        pool_destroy(&pool_load);
    }
    if (shard_buf) free(shard_buf);
    if (g_batch_slots) free(g_batch_slots);
    return rc;
}
