/*
 * Engine-level IPC smoke test: the real ucm_spdk_store engine runs in the
 * worker process (using ipc:<socket>), keeps its index/metadata/GC, and
 * performs data + record I/O through the local IPC target.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ucm_spdk_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct wait_ctx {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int done;
	int status;
};

static void
done_cb(void *arg, int status)
{
	struct wait_ctx *w = arg;

	pthread_mutex_lock(&w->mu);
	w->status = status;
	w->done = 1;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
}

static int
wait_done(struct wait_ctx *w, int timeout_ms)
{
	struct timespec deadline;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&w->mu);
	while (!w->done) {
		if (pthread_cond_timedwait(&w->cv, &w->mu, &deadline) == ETIMEDOUT) {
			break;
		}
	}
	pthread_mutex_unlock(&w->mu);
	return w->done ? w->status : -ETIMEDOUT;
}

static void
fill_pattern(uint8_t *buf, size_t size, uint32_t seed)
{
	uint32_t x = seed ? seed : 1;
	size_t i;

	for (i = 0; i < size; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		buf[i] = (uint8_t)x;
	}
}

static int
create_huge_memfd(const char *name, size_t size)
{
	int flags = MFD_CLOEXEC;
	int fd;

#ifdef MFD_HUGETLB
	flags |= MFD_HUGETLB;
#ifdef MFD_HUGE_2MB
	flags |= MFD_HUGE_2MB;
#endif
#endif
	fd = memfd_create(name, flags);
	if (fd < 0) {
		return -errno;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		int rc = -errno;
		close(fd);
		return rc;
	}
	return fd;
}

int
main(int argc, char **argv)
{
	const char *sock = "/tmp/ucm_spdk_ipc.sock";
	uint32_t shard_kb = 1024;
	uint32_t nslots = 8;
	char trid[256];
	uint8_t id[16];
	uint8_t *data;
	size_t data_size;
	int data_fd;
	struct wait_ctx w = {
		.mu = PTHREAD_MUTEX_INITIALIZER,
		.cv = PTHREAD_COND_INITIALIZER,
	};
	int rc;

	if (argc > 1) { sock = argv[1]; }
	if (argc > 2) { shard_kb = (uint32_t)strtoul(argv[2], NULL, 0); }
	if (argc > 3) { nslots = (uint32_t)strtoul(argv[3], NULL, 0); }
	if (shard_kb == 0 || nslots == 0) {
		fprintf(stderr, "invalid shard/slots\n");
		return 2;
	}
	data_size = (size_t)nslots * shard_kb * 1024u;
	data_fd = create_huge_memfd("ucm_engine_data", data_size);
	if (data_fd < 0) {
		fprintf(stderr, "data hugepage memfd failed: %s\n", strerror(errno));
		return 1;
	}
	data = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "data mmap failed\n");
		return 1;
	}
	memset(data, 0, data_size);

	snprintf(trid, sizeof(trid), "ipc:%s", sock);
	rc = spdk_store_set_direct_only(1);
	if (rc != 0) {
		fprintf(stderr, "set_direct_only failed: %d\n", rc);
		return 1;
	}
	rc = spdk_store_init(trid, nslots, 1, shard_kb, 0, 1);
	if (rc != 0) {
		fprintf(stderr, "engine init failed: %d\n", rc);
		return 1;
	}
	rc = spdk_store_register_memory_fd(data_fd, data, data_size, 0);
	if (rc != 0) {
		fprintf(stderr, "register data fd failed: %d\n", rc);
		return 1;
	}

	memset(id, 0x5a, sizeof(id));
	fill_pattern(data, (size_t)shard_kb * 1024u, 0x33333333u);
	w.done = 0;
	rc = spdk_store_dump_submit(id, 0, 1, data, done_cb, &w);
	if (rc != 0) {
		fprintf(stderr, "dump submit failed: %d\n", rc);
		return 1;
	}
	rc = wait_done(&w, 10000);
	if (rc != 0) {
		fprintf(stderr, "dump failed/timeout: %d\n", rc);
		return 1;
	}
	if (!spdk_store_lookup(id)) {
		fprintf(stderr, "lookup after dump failed\n");
		return 1;
	}

	memset(data, 0, (size_t)shard_kb * 1024u);
	w.done = 0;
	rc = spdk_store_load_submit(id, 0, 1, data, (uint32_t)((size_t)shard_kb * 1024u),
				    done_cb, &w);
	if (rc != 0) {
		fprintf(stderr, "load submit failed: %d\n", rc);
		return 1;
	}
	rc = wait_done(&w, 10000);
	if (rc != 0) {
		fprintf(stderr, "load failed/timeout: %d\n", rc);
		return 1;
	}
	{
		uint8_t *expect = malloc((size_t)shard_kb * 1024u);
		if (expect == NULL) { return 1; }
		fill_pattern(expect, (size_t)shard_kb * 1024u, 0x33333333u);
		if (memcmp(data, expect, (size_t)shard_kb * 1024u) != 0) {
			fprintf(stderr, "engine read-back mismatch\n");
			free(expect);
			return 1;
		}
		free(expect);
	}

	printf("OK\n");
	spdk_store_fini();
	munmap(data, data_size);
	close(data_fd);
	return 0;
}
