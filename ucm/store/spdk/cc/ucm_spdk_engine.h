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

/* Lifecycle */
int spdk_store_init(const char *trid_str, uint32_t nslots, uint32_t layers,
                    uint32_t shard_kb, uint64_t offset_gib, int format);
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

/* GC */
int spdk_store_gc(int max_victims);

/* Stats */
uint64_t spdk_store_used_bytes(void);
uint64_t spdk_store_capacity_bytes(void);
uint64_t spdk_store_epoch(void);

#ifdef __cplusplus
}
#endif

#endif /* UCM_SPDK_ENGINE_H */
