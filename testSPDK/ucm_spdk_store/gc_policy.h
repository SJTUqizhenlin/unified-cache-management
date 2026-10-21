#ifndef UCM_SPDK_GC_POLICY_H
#define UCM_SPDK_GC_POLICY_H

#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>

static inline int
store_gc_config_valid(double threshold, double recycle, uint32_t interval)
{
	return isfinite(threshold) && threshold > 0 && threshold <= 1 &&
	       isfinite(recycle) && recycle > 0 && recycle <= 1 && interval > 0;
}

/* Like Posix GetOldestFiles: a fraction of current live blocks, not capacity. */
static inline uint32_t
store_gc_round_count(uint32_t live, uint32_t capacity, double threshold, double recycle)
{
	if (live == 0 || live < (uint32_t)(capacity * threshold)) {
		return 0;
	}
	return (uint32_t)(live * recycle);
}

/* Separate open descriptions are required: flock ownership is per description.
 * Never unlink this gate while workers may still have it open. */
static inline int
store_gc_gate_open(const char *name)
{
	int fd = shm_open(name, O_CREAT | O_RDWR | O_CLOEXEC, 0600);

	return fd < 0 ? -errno : fd;
}

struct gc_victim {
	uint8_t id[16];
	uint32_t slot;
	uint64_t epoch;
	uint64_t hot;
};

/* Called only while the shared index lock protects both pins and visibility. */
static inline void
store_gc_consider(struct gc_victim *vics, int *count, int limit,
		  const uint8_t id[16], uint32_t slot, uint16_t pins, uint64_t hot)
{
	int k;

	if (pins != 0 || limit <= 0) {
		return;
	}
	if (*count < limit) {
		k = (*count)++;
	} else if (hot < vics[*count - 1].hot) {
		k = *count - 1;
	} else {
		return;
	}
	while (k > 0 && vics[k - 1].hot > hot) {
		vics[k] = vics[k - 1];
		k--;
	}
	for (int i = 0; i < 16; i++) {
		vics[k].id[i] = id[i];
	}
	vics[k].slot = slot;
	vics[k].hot = hot;
}

#endif
