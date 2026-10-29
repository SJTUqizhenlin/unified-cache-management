/*
 * ucm_spdk_engine.h — C API exported from the SPDK store engine library.
 *
 * The engine (testSPDK/ucm_spdk_store.c) is compiled as a static library
 * and linked into the UCM pipeline .so (spdkstore). This header declares
 * the C-callable interface consumed by the C++ StoreV1 wrapper.
 */
#ifndef UCM_SPDK_ENGINE_H
#define UCM_SPDK_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Instance selection is thread-local and does not transfer ownership. All
 * existing APIs below operate on the selected instance (including lookup-only
 * scheduler instances). NULL selects the legacy process-default instance.
 * create returns NULL on allocation failure and does not select the instance.
 * Save select's return value and restore it for nestable C++ RAII scopes.
 *
 * Serialize lifecycle/registration calls for each instance. Stop submitting
 * before fini/destroy; destroy drains callbacks and joins its reactor/GC timer.
 * No thread may retain/use an instance after destroy, including saved selection
 * pointers. Destroy clears the caller's selection if it names that instance.
 * Lifecycle, synchronous GC and registration APIs must not run in callbacks or
 * with a manually paused reactor. Callbacks run with their owner selected and
 * must restore any temporary selection before returning.
 *
 * SPDK env is initialized once and retained until process exit; this library
 * must own environment initialization. Do not externally fini it or dlclose the
 * containing library while instances/SPDK resources are alive. */
struct spdk_store_engine;
struct spdk_store_engine *spdk_store_engine_create(void);
struct spdk_store_engine *spdk_store_engine_select(struct spdk_store_engine *engine);
/* Before initialization; count=0 preserves inherited affinity. */
int spdk_store_set_cpu_affinity(const int *cores, uint32_t count);
void spdk_store_engine_destroy(struct spdk_store_engine *engine);

/* Configure the selected engine before init; returns -EBUSY once initialized.
 * Enabled: no full-shard bounce DMA allocations; data buffers must be registered
 * before IO (otherwise rejected with -EFAULT). Disabled by default for standalone
 * callers: preallocate up to 64 MiB of bounce buffers during init, never in the
 * reactor. Shards exceeding that budget require direct-only mode (-E2BIG).
 * This setting persists across fini/reinit. Request concurrency is always 256. */
int spdk_store_set_direct_only(int enabled);

/* Lifecycle. Reinitializing a live instance returns -EALREADY. Partitions must
 * be nonoverlapping, including superblock and the 1 GiB metadata reservation.
 * Format/resize/model changes exclude all attachments of the SAME region;
 * unrelated regions remain live. Startup is still serialized process-wide and
 * cross-process. Existing index/recovery/session names are unchanged, but the
 * maintenance protocol requires stopping all old-version engines on upgrade.
 * Host registrations shared across instances are reference counted. Partial
 * overlaps are rejected; contained ranges retain the whole original mapping.
 * Keep that whole backing mapping alive until the final reference is released.
 * Borrowed external registrations must outlive every engine using them. */
/* Durable invalidations transfer pinned-slot reclamation to the shared index.
 * Fini/destroy fence local callbacks/DMA but never wait for another attachment
 * to unpin. The last pin release reclaims exactly once, even after the writer
 * exits. Shared-index ABI v6 requires all v5 attachments to stop before upgrade;
 * the disk format is unchanged and exclusive recovery rebuilds the segment. */
int spdk_store_init(const char *trid_str, uint32_t nslots, uint32_t layers,
                    uint32_t shard_kb, uint64_t offset_gib, int format);
/* A shared, fresh service ID makes format run once across its workers. */
int spdk_store_init_for_session(const char *trid_str, uint32_t nslots, uint32_t layers,
                    uint32_t shard_kb, uint64_t offset_gib, int format, const char *session);
void spdk_store_fini(void);
int spdk_store_lookup_init(void);
int spdk_store_lookup_init_region(const char *trid_str, uint64_t offset_gib);
void spdk_store_lookup_fini(void);
int spdk_store_reactor_start(void);
void spdk_store_reactor_stop(void);
int spdk_store_healthy(void);
int spdk_store_register_memory(void *addr, uint64_t size);
int spdk_store_unregister_memory(void *addr, uint64_t size);
uint64_t spdk_store_registered_bytes(void);

/* Lookup (thread-safe: only touches the SHM mutex, no device access) */
int spdk_store_lookup(const uint8_t id[16]);
uint32_t spdk_store_lookup_prefix(const uint8_t ids[][16], uint32_t n);

/* Data path (enqueue to reactor; completion via callback) */
typedef void (*spdk_store_cb)(void *arg, int status);
int spdk_store_dump_submit(const uint8_t id[16], uint32_t first, uint32_t count,
                           const void *data, spdk_store_cb cb, void *arg);
int spdk_store_load_submit(const uint8_t id[16], uint32_t first, uint32_t count,
                           void *dst, uint32_t len,
                           spdk_store_cb cb, void *arg);
int spdk_store_delete(const uint8_t id[16], spdk_store_cb cb, void *arg);

/* GC: lifecycle calls are serialized with engine lifecycle by the worker.
 * Start requires a running reactor; stop drains the current invalidations.
 * Synchronous calls must not run on the reactor or while it is paused. */
int spdk_store_gc(int max_victims);
int spdk_store_gc_start(double trigger_threshold_ratio, double recycle_percent,
                        uint32_t check_interval_sec);
void spdk_store_gc_stop(void);

/* Stats */
uint64_t spdk_store_used_bytes(void);
uint64_t spdk_store_capacity_bytes(void);
uint64_t spdk_store_epoch(void);

#ifdef __cplusplus
}
#endif

#endif /* UCM_SPDK_ENGINE_H */
