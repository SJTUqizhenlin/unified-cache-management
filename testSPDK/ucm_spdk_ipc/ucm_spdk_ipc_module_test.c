#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * Module test for the worker-side IPC client and target multi-region protocol.
 *
 * Creates a metadata hugetlbfs region (region 0), registers a data region
 * (region 1), writes/reads both regions, and verifies the returned bytes.
 */

#include "ucm_spdk_ipc_client.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_SOCK "/tmp/ucm_spdk_ipc.sock"
#define DEFAULT_DATA_MB 4u
#define DEFAULT_META_MB 2u
#define DEFAULT_SHARD_KB 1024u
#define DEFAULT_OUTSTANDING 8u
#define DEFAULT_DATA_LBA_MB 100u
#define DEFAULT_META_LBA_MB 200u

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

static int
run_op(struct ucm_ipc_client *c, uint32_t region_id, uint16_t opcode,
       uint64_t lba_bytes, uint64_t data_offset, uint32_t nbytes)
{
	uint64_t req_id;
	int status = 0;
	int rc;

	rc = ucm_ipc_client_submit(c, region_id, opcode, lba_bytes, data_offset,
				   nbytes, &req_id);
	if (rc != 0) {
		return rc;
	}
	rc = ucm_ipc_client_wait(c, req_id, 5000, &status);
	if (rc != 0) {
		return rc;
	}
	return status;
}

int
main(int argc, char **argv)
{
	const char *sock = DEFAULT_SOCK;
	uint32_t data_mb = DEFAULT_DATA_MB;
	uint32_t meta_mb = DEFAULT_META_MB;
	uint32_t shard_kb = DEFAULT_SHARD_KB;
	uint32_t max_outstanding = DEFAULT_OUTSTANDING;
	uint32_t data_lba_mb = DEFAULT_DATA_LBA_MB;
	uint32_t meta_lba_mb = DEFAULT_META_LBA_MB;
	struct ucm_ipc_client client;
	uint8_t *meta = NULL;
	uint8_t *data = NULL;
	uint8_t *expect = NULL;
	size_t meta_size;
	size_t data_size;
	size_t shard_bytes;
	int meta_fd = -1;
	int data_fd = -1;
	int rc;

	static const struct option opts[] = {
		{"socket", required_argument, NULL, 's'},
		{"data-mb", required_argument, NULL, 'd'},
		{"meta-mb", required_argument, NULL, 'm'},
		{"shard-kb", required_argument, NULL, 'k'},
		{"outstanding", required_argument, NULL, 'o'},
		{"data-lba-mb", required_argument, NULL, 'D'},
		{"meta-lba-mb", required_argument, NULL, 'M'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int c;

	while ((c = getopt_long(argc, argv, "s:d:m:k:o:D:M:h", opts, NULL)) != -1) {
		switch (c) {
		case 's': sock = optarg; break;
		case 'd': data_mb = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'm': meta_mb = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'k': shard_kb = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'o': max_outstanding = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'D': data_lba_mb = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'M': meta_lba_mb = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'h':
			printf("Usage: %s [-s socket] [-d data_mb] [-m meta_mb] "
			       "[-k shard_kb] [-o outstanding] [-D data_lba_mb] "
			       "[-M meta_lba_mb]\n", argv[0]);
			return 0;
		default:
			return 2;
		}
	}
	if (data_mb == 0 || meta_mb == 0 || shard_kb == 0 || max_outstanding == 0) {
		fprintf(stderr, "invalid arguments\n");
		return 2;
	}
	data_size = (size_t)data_mb << 20;
	meta_size = (size_t)meta_mb << 20;
	shard_bytes = (size_t)shard_kb << 10;
	if (shard_bytes > data_size) {
		fprintf(stderr, "shard larger than data region\n");
		return 2;
	}
	if ((meta_size & 0x1fffffu) != 0 || (data_size & 0x1fffffu) != 0 ||
	    (shard_bytes & 511u) != 0) {
		fprintf(stderr, "regions must be 2 MiB aligned and shard 512B aligned\n");
		return 2;
	}

	meta_fd = create_huge_memfd("ucm_ipc_meta", meta_size);
	data_fd = create_huge_memfd("ucm_ipc_data", data_size);
	if (meta_fd < 0 || data_fd < 0) {
		fprintf(stderr, "hugepage memfd failed: %s "
			"(check vm.nr_hugepages)\n", strerror(errno));
		return 1;
	}
	meta = mmap(NULL, meta_size, PROT_READ | PROT_WRITE, MAP_SHARED, meta_fd, 0);
	data = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
	expect = malloc(shard_bytes);
	if (meta == MAP_FAILED || data == MAP_FAILED || expect == NULL) {
		fprintf(stderr, "mmap/malloc failed\n");
		return 1;
	}
	memset(meta, 0, meta_size);
	memset(data, 0, data_size);

	rc = ucm_ipc_client_init(&client, sock, meta_fd, meta_size, 0,
				 max_outstanding, -1, "module-test", "META");
	if (rc != 0) {
		fprintf(stderr, "client init failed: %s\n", strerror(-rc));
		return 1;
	}
	printf("session=%u qpair_depth=%u sector=%" PRIu64 " max_xfer=%" PRIu64 "\n",
	       client.session_id, client.qpair_depth, client.sector_size,
	       client.max_xfer_size);

	rc = ucm_ipc_client_register_region(&client, sock, 1, data_fd, data_size, 0);
	if (rc != 0) {
		fprintf(stderr, "register data region failed: %s\n", strerror(-rc));
		return 1;
	}

	/* Metadata region (region 0), one 512B record. */
	fill_pattern(meta, 512, 0x11111111u);
	rc = run_op(&client, 0, UCM_IPC_OP_WRITE,
		    (uint64_t)meta_lba_mb << 20, 0, 512);
	if (rc != 0) {
		fprintf(stderr, "metadata write failed: %s\n", strerror(-rc));
		return 1;
	}
	memset(meta, 0, 512);
	rc = run_op(&client, 0, UCM_IPC_OP_READ,
		    (uint64_t)meta_lba_mb << 20, 0, 512);
	if (rc != 0) {
		fprintf(stderr, "metadata read failed: %s\n", strerror(-rc));
		return 1;
	}
	fill_pattern(expect, 512, 0x11111111u);
	if (memcmp(meta, expect, 512) != 0) {
		fprintf(stderr, "metadata read-back mismatch\n");
		return 1;
	}

	/* Cache data region (region 1), one shard. */
	fill_pattern(expect, shard_bytes, 0x22222222u);
	memcpy(data, expect, shard_bytes);
	rc = run_op(&client, 1, UCM_IPC_OP_WRITE,
		    (uint64_t)data_lba_mb << 20, 0, (uint32_t)shard_bytes);
	if (rc != 0) {
		fprintf(stderr, "data write failed: %s\n", strerror(-rc));
		return 1;
	}
	memset(data, 0, shard_bytes);
	rc = run_op(&client, 1, UCM_IPC_OP_READ,
		    (uint64_t)data_lba_mb << 20, 0, (uint32_t)shard_bytes);
	if (rc != 0) {
		fprintf(stderr, "data read failed: %s\n", strerror(-rc));
		return 1;
	}
	if (memcmp(data, expect, shard_bytes) != 0) {
		fprintf(stderr, "data read-back mismatch\n");
		return 1;
	}

	ucm_ipc_client_close(&client);
	munmap(meta, meta_size);
	munmap(data, data_size);
	close(meta_fd);
	close(data_fd);
	free(expect);
	printf("OK\n");
	return 0;
}
