/*
 * engine_shim.c — thin aliases mapping the engine's internal names to
 * the spdk_store_* API that the C++ wrapper (spdk_store.cc) links against.
 */

#include <stdint.h>

/* from the engine (ucm_spdk_store.c, non-static public functions) */
int store_engine_init(const char *trid_str, uint32_t nslots_req,
		      uint32_t layers, uint32_t shard_kb, uint64_t offset_gib,
		      int format);
void store_engine_fini(void);
int store_lookup_init(void);
int store_lookup_init_region(const char *trid_str, uint64_t offset_gib);
void store_lookup_fini(void);
int store_lookup(const uint8_t id[16]);
uint32_t store_lookup_prefix(const uint8_t ids[][16], uint32_t n);
int store_dump_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		      const void *data, void (*cb)(void *, int), void *arg);
int store_load_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		      void *dst, uint32_t len,
		      void (*cb)(void *, int), void *arg);
int store_delete(const uint8_t id[16], void (*cb)(void *, int), void *arg);
int store_gc_public(int max_victims);
void store_set_direct_dma(int enable);
uint64_t store_registered_bytes(void);
int store_register_memory(void *addr, uint64_t size);
int store_unregister_memory(void *addr, uint64_t size);
int reactor_start(void);
void reactor_stop(void);

/* engine accessor functions (non-static wrappers in the engine source) */
uint64_t store_live_count(void);
uint32_t store_nslots(void);
uint32_t store_slot_sectors(void);
uint32_t store_sector_size(void);
int store_is_healthy(void);
uint64_t store_epoch_value(void);

/* --- spdk_store_* public API (consumed by spdk_store.cc) --- */

int
spdk_store_init(const char *trid_str, uint32_t nslots, uint32_t layers,
		uint32_t shard_kb, uint64_t offset_gib, int format)
{
	return store_engine_init(trid_str, nslots, layers, shard_kb,
				 offset_gib, format);
}

void
spdk_store_fini(void)
{
	store_engine_fini();
}

int
spdk_store_lookup_init(void)
{
	return store_lookup_init();
}

int
spdk_store_lookup_init_region(const char *trid_str, uint64_t offset_gib)
{
	return store_lookup_init_region(trid_str, offset_gib);
}

void
spdk_store_lookup_fini(void)
{
	store_lookup_fini();
}

int
spdk_store_reactor_start(void)
{
	return reactor_start();
}

void
spdk_store_reactor_stop(void)
{
	reactor_stop();
}

int
spdk_store_healthy(void)
{
	return store_is_healthy();
}

int
spdk_store_lookup(const uint8_t id[16])
{
	return store_lookup(id);
}

uint32_t
spdk_store_lookup_prefix(const uint8_t ids[][16], uint32_t n)
{
	return store_lookup_prefix(ids, n);
}

int
spdk_store_dump_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		       const void *data, void (*cb)(void *, int), void *arg)
{
	return store_dump_submit(id, first, count, data, cb, arg);
}

int
spdk_store_load_submit(const uint8_t id[16], uint32_t first, uint32_t count,
		       void *dst, uint32_t len,
		       void (*cb)(void *, int), void *arg)
{
	return store_load_submit(id, first, count, dst, len, cb, arg);
}

int
spdk_store_delete(const uint8_t id[16], void (*cb)(void *, int), void *arg)
{
	return store_delete(id, cb, arg);
}

int
spdk_store_gc(int max_victims)
{
	return store_gc_public(max_victims);
}

uint64_t
spdk_store_used_bytes(void)
{
	return store_live_count() * store_slot_sectors() * store_sector_size();
}

uint64_t
spdk_store_capacity_bytes(void)
{
	return (uint64_t)store_nslots() * store_slot_sectors() * store_sector_size();
}

uint64_t
spdk_store_epoch(void)
{
	return store_epoch_value();
}

void
spdk_store_set_direct_dma(int enable)
{
	store_set_direct_dma(enable);
}

uint64_t
spdk_store_registered_bytes(void)
{
	return store_registered_bytes();
}

int
spdk_store_register_memory(void *addr, uint64_t size)
{
	return store_register_memory(addr, size);
}

int
spdk_store_unregister_memory(void *addr, uint64_t size)
{
	return store_unregister_memory(addr, size);
}
