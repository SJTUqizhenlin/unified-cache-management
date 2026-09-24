/*
 * kernel_iops_test.c
 *
 * Kernel-side IOPS benchmark using Linux AIO (libaio) + O_DIRECT on ext4.
 * Direct comparison counterpart to nvme_iops_test.c (SPDK).
 *
 * Same workload types, parameters, output format, and PRNG logic.
 * Usage mirrors the SPDK test, replacing -r <trid> with -f <filepath>.
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
#include <sys/stat.h>
#include <inttypes.h>
#include <libaio.h>

#define MAX_QD 1024

enum workload { WL_RAND_READ, WL_RAND_WRITE, WL_RAND_RW };

struct io_slot {
	void		*buf;
	uint64_t	offset;
	struct timespec	submit_ts;
	struct iocb	iocb;
	int		in_flight;
};

static int		g_fd = -1;
static io_context_t	g_aio_ctx;
static int		g_use_aio = 1;

static uint32_t	g_qd = 128;
/* Slot/buffer count: at least PREWRITE_QD so the pre-write phase runs at
 * full depth regardless of the (possibly low) test -q value. */
#define PREWRITE_QD 128
static uint32_t	g_alloc_qd = PREWRITE_QD;
static uint32_t	g_block_size = 4096;
static uint32_t	g_sector_size = 512;
static uint32_t	g_lba_count;
static uint64_t	g_num_sectors;
static uint64_t	g_start_lba;
static uint64_t	g_io_range;
static enum workload g_wl = WL_RAND_READ;
static uint32_t	g_write_pct = 30;
static int	g_runtime_sec = 10;
static uint64_t	g_span_gib = 1;
static uint64_t	g_offset_gib = 0;
static const char *g_filepath = "/home/qizhenlin/nvme_test/testfile";

static struct timespec g_start_ts;
static struct timespec g_deadline_ts;
static int	g_stop;
static uint64_t	g_completed_ios;
static uint64_t	g_total_lat_ns;
static uint64_t	g_err_count;

static struct io_slot g_slots[MAX_QD];

static uint64_t
rand_u64(void)
{
	return ((uint64_t)rand() << 33) ^ ((uint64_t)rand() << 2) ^ (uint64_t)rand();
}

static uint64_t
ts_to_ns(const struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static int
ts_before(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec < b->tv_sec;
	return a->tv_nsec < b->tv_nsec;
}

static void
submit_one(struct io_slot *s)
{
	uint64_t usable = (g_io_range > g_lba_count) ? (g_io_range - g_lba_count) : 0;
	uint64_t lba;
	int is_write;

	lba = g_start_lba + ((usable > 0) ? (rand_u64() % (usable + 1)) : 0);
	s->offset = lba * g_sector_size;

	clock_gettime(CLOCK_MONOTONIC, &s->submit_ts);
	s->in_flight = 1;

	is_write = (g_wl == WL_RAND_WRITE) ||
		   (g_wl == WL_RAND_RW && ((uint32_t)rand() % 100 < g_write_pct));

	if (is_write) {
		io_prep_pwrite(&s->iocb, g_fd, s->buf, g_block_size, (long long)s->offset);
	} else {
		io_prep_pread(&s->iocb, g_fd, s->buf, g_block_size, (long long)s->offset);
	}
	s->iocb.data = s;

	{
		struct iocb *iocbs[] = {&s->iocb};
		if (io_submit(g_aio_ctx, 1, iocbs) != 1) {
			fprintf(stderr, "io_submit failed: %s\n", strerror(errno));
			g_err_count++;
			g_stop = 1;
		}
	}
}

static int
submit_one_sync(struct io_slot *s)
{
	uint64_t usable = (g_io_range > g_lba_count) ? (g_io_range - g_lba_count) : 0;
	uint64_t lba;
	int is_write;
	ssize_t ret;

	lba = g_start_lba + ((usable > 0) ? (rand_u64() % (usable + 1)) : 0);
	s->offset = lba * g_sector_size;

	clock_gettime(CLOCK_MONOTONIC, &s->submit_ts);

	is_write = (g_wl == WL_RAND_WRITE) ||
		   (g_wl == WL_RAND_RW && ((uint32_t)rand() % 100 < g_write_pct));

	if (is_write) {
		ret = pwrite(g_fd, s->buf, g_block_size, (off_t)s->offset);
	} else {
		ret = pread(g_fd, s->buf, g_block_size, (off_t)s->offset);
	}

	if (ret != (ssize_t)g_block_size) {
		fprintf(stderr, "pread/pwrite failed: %s\n", strerror(errno));
		g_err_count++;
		g_stop = 1;
		return -1;
	}
	return 0;
}

static int
parse_wl(const char *s)
{
	if (strcmp(s, "randread") == 0) {
		g_wl = WL_RAND_READ;
	} else if (strcmp(s, "randwrite") == 0) {
		g_wl = WL_RAND_WRITE;
	} else if (strcmp(s, "randrw") == 0) {
		g_wl = WL_RAND_RW;
	} else {
		return -1;
	}
	return 0;
}

static void
prewrite_working_set_sync(void)
{
	uint64_t lba = g_start_lba;
	uint64_t end = g_start_lba + g_io_range;
	uint64_t written = 0;

	printf("Pre-writing working set (sync) [%ju, %ju) ~%lu GiB ...\n",
	       (uintmax_t)g_start_lba, (uintmax_t)end, (unsigned long)g_span_gib);

	while (lba + g_lba_count <= end && !g_stop) {
		g_slots[0].offset = lba * g_sector_size;
		if (pwrite(g_fd, g_slots[0].buf, g_block_size,
			   (off_t)g_slots[0].offset) != (ssize_t)g_block_size) {
			fprintf(stderr, "prewrite pwrite failed: %s\n", strerror(errno));
			g_err_count++;
			g_stop = 1;
			break;
		}
		lba += g_lba_count;
		written += g_lba_count;
	}

	if (!g_stop) {
		printf("Pre-write done: %.2f GiB written.\n",
		       (double)(written * g_sector_size) / (1024.0 * 1024 * 1024));
	}
}

static void
prewrite_complete(struct io_event *ev)
{
	struct io_slot *s = (struct io_slot *)ev->data;
	s->in_flight = 0;
	if (ev->res != (long)g_block_size || ev->res2 != 0) {
		g_err_count++;
		g_stop = 1;
	}
}

static void
prewrite_working_set(void)
{
	uint64_t lba = g_start_lba;
	uint64_t end = g_start_lba + g_io_range;
	uint64_t written = 0;
	int i, slot;
	struct io_event evs[MAX_QD];

	printf("Pre-writing working set [%ju, %ju) ~%lu GiB ...\n",
	       (uintmax_t)g_start_lba, (uintmax_t)end, (unsigned long)g_span_gib);

	while (lba + g_lba_count <= end && !g_stop) {
		for (slot = 0; slot < (int)g_alloc_qd; slot++) {
			if (!g_slots[slot].in_flight) {
				break;
			}
		}
		if (slot == (int)g_alloc_qd) {
			int nr = io_getevents(g_aio_ctx, 1, g_alloc_qd, evs, NULL);
			for (i = 0; i < nr; i++) {
				prewrite_complete(&evs[i]);
			}
			continue;
		}
		g_slots[slot].offset = lba * g_sector_size;
		g_slots[slot].in_flight = 1;
		io_prep_pwrite(&g_slots[slot].iocb, g_fd, g_slots[slot].buf,
			       g_block_size, (long long)g_slots[slot].offset);
		g_slots[slot].iocb.data = &g_slots[slot];
		{
			struct iocb *iocbs[] = {&g_slots[slot].iocb};
			if (io_submit(g_aio_ctx, 1, iocbs) != 1) {
				g_err_count++;
				g_stop = 1;
				continue;
			}
		}
		lba += g_lba_count;
		written += g_lba_count;
	}

	while (!g_stop) {
		int in_flight = 0;
		for (i = 0; i < (int)g_alloc_qd; i++) {
			if (g_slots[i].in_flight) {
				in_flight++;
			}
		}
		if (!in_flight) {
			break;
		}
		int nr = io_getevents(g_aio_ctx, 1, g_alloc_qd, evs, NULL);
		for (i = 0; i < nr; i++) {
			prewrite_complete(&evs[i]);
		}
	}

	if (!g_stop) {
		printf("Pre-write done: %.2f GiB written.\n",
		       (double)(written * g_sector_size) / (1024.0 * 1024 * 1024));
	}
}

static void
usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -f <path>     File path (default: testfile)\n");
	printf("  -q <depth>    Queue depth (default 128, max %d)\n", MAX_QD);
	printf("  -o <KB>       Block size in KB, multiple of sector size (default 4)\n");
	printf("  -t <sec>      Run time in seconds (default 10)\n");
	printf("  -w <type>     Workload: randread | randwrite | randrw (default randread)\n");
	printf("  -M <pct>      Write %% for randrw, 0-100 (default 30)\n");
	printf("  -S <GiB>      File / working-set size in GiB; the set is pre-written before the test (default 1)\n");
	printf("  -O <GiB>      Working-set offset in GiB within the file (default 0)\n");
	printf("  -s <seed>     PRNG seed (default 1)\n");
	printf("  -A <0|1>      1=libaio async (default), 0=sync pread/pwrite\n");
	printf("  -h            Show this help\n");
}

int
main(int argc, char **argv)
{
	int op, i, rc = 0;
	const char *wl_names[] = {"randread", "randwrite", "randrw"};
	struct timespec end_ts;
	uint64_t elapsed_ns, in_flight;
	double elapsed_sec, iops, mibs, avg_lat_us;
	uint64_t file_size_bytes;
	struct stat st;
	struct io_event evs[MAX_QD];

	srand(1);
	while ((op = getopt(argc, argv, "f:q:o:t:w:M:S:O:s:A:h")) != -1) {
		switch (op) {
		case 'f':
			g_filepath = optarg;
			break;
		case 'q':
			g_qd = (uint32_t)atoi(optarg);
			break;
		case 'o':
			g_block_size = (uint32_t)atoi(optarg) * 1024;
			break;
		case 't':
			g_runtime_sec = atoi(optarg);
			break;
		case 'w':
			if (parse_wl(optarg) != 0) {
				fprintf(stderr, "bad workload: %s\n", optarg);
				return 1;
			}
			break;
		case 'M':
			g_write_pct = (uint32_t)atoi(optarg);
			break;
		case 'S':
			g_span_gib = (uint64_t)atol(optarg);
			break;
		case 'O':
			g_offset_gib = (uint64_t)atol(optarg);
			break;
		case 's':
			srand((unsigned)atoi(optarg));
			break;
		case 'A':
			g_use_aio = atoi(optarg);
			if (g_use_aio != 0 && g_use_aio != 1) {
				fprintf(stderr, "invalid -A value (0 or 1)\n");
				return 1;
			}
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (op == 'h') ? 0 : 1;
		}
	}

	if (g_qd == 0 || g_qd > MAX_QD) {
		fprintf(stderr, "invalid queue depth %u (1-%d)\n", g_qd, MAX_QD);
		return 1;
	}
	g_alloc_qd = (g_qd > PREWRITE_QD) ? g_qd : PREWRITE_QD;
	if (g_block_size == 0) {
		fprintf(stderr, "invalid block size\n");
		return 1;
	}
	if (g_span_gib == 0) {
		fprintf(stderr, "span/file size must be > 0 for file-based test (use -S)\n");
		return 1;
	}

	g_lba_count = g_block_size / g_sector_size;

	file_size_bytes = (g_offset_gib + g_span_gib) * (1024ULL * 1024 * 1024);
	g_num_sectors = file_size_bytes / g_sector_size;

	g_start_lba = (g_offset_gib * (1024ULL * 1024 * 1024)) / g_sector_size;
	{
		uint64_t span_sectors = (g_span_gib * (1024ULL * 1024 * 1024)) / g_sector_size;
		uint64_t end_lba = g_start_lba + span_sectors;
		if (end_lba > g_num_sectors) {
			end_lba = g_num_sectors;
		}
		g_io_range = end_lba - g_start_lba;
	}

	if (g_io_range < g_lba_count) {
		fprintf(stderr, "working set too small (%lu sectors) for block size\n",
			(unsigned long)g_io_range);
		return 1;
	}

	printf("File: %s (%.2f GiB) | range [%.3f, %.3f) GiB | QD %u | %uKB | %ds | %s | %s",
	       g_filepath,
	       (double)file_size_bytes / (1024.0 * 1024 * 1024),
	       (double)g_offset_gib,
	       (double)(g_offset_gib + g_span_gib),
	       g_qd, g_block_size / 1024, g_runtime_sec, wl_names[g_wl],
	       g_use_aio ? "libaio" : "sync");
	if (g_wl == WL_RAND_RW) {
		printf(" (write %u%%)", g_write_pct);
	}
	printf("\n");

	g_fd = open(g_filepath, O_RDWR | O_CREAT | O_DIRECT | O_TRUNC, 0644);
	if (g_fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", g_filepath, strerror(errno));
		return 1;
	}

	if (ftruncate(g_fd, file_size_bytes) != 0) {
		fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
		rc = 1;
		goto exit_close;
	}

	if (fstat(g_fd, &st) != 0) {
		fprintf(stderr, "fstat failed: %s\n", strerror(errno));
		rc = 1;
		goto exit_close;
	}
	if ((uint64_t)st.st_blksize > g_block_size && g_block_size % st.st_blksize != 0) {
		fprintf(stderr, "block size %u not aligned to filesystem block size %lu\n",
			g_block_size, (unsigned long)st.st_blksize);
		rc = 1;
		goto exit_close;
	}

	if (g_use_aio) {
		if (io_setup(MAX_QD, &g_aio_ctx) != 0) {
			fprintf(stderr, "io_setup failed: %s\n", strerror(errno));
			rc = 1;
			goto exit_close;
		}
	}

	for (i = 0; i < (int)g_alloc_qd; i++) {
		if (posix_memalign(&g_slots[i].buf, 4096, g_block_size) != 0) {
			fprintf(stderr, "buffer alloc failed at slot %d\n", i);
			rc = 1;
			goto exit_cleanup;
		}
		memset(g_slots[i].buf, 0xaa ^ (i & 0xff), g_block_size);
		g_slots[i].in_flight = 0;
	}

	if (g_use_aio) {
		prewrite_working_set();
	} else {
		prewrite_working_set_sync();
	}
	if (g_stop || g_err_count) {
		fprintf(stderr, "pre-write failed\n");
		rc = 1;
		goto exit_cleanup;
	}

	clock_gettime(CLOCK_MONOTONIC, &g_start_ts);
	g_deadline_ts.tv_sec = g_start_ts.tv_sec + g_runtime_sec;
	g_deadline_ts.tv_nsec = g_start_ts.tv_nsec;

	if (g_use_aio) {
		for (i = 0; i < (int)g_qd; i++) {
			submit_one(&g_slots[i]);
		}

		{
			struct timespec zero_ts = {0, 0};
			while (!g_stop) {
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				if (!ts_before(&now, &g_deadline_ts)) {
					break;
				}
				int nr = io_getevents(g_aio_ctx, 0, g_qd, evs, &zero_ts);
				for (i = 0; i < nr; i++) {
					struct io_slot *s = (struct io_slot *)evs[i].data;
					struct timespec cnow;
					uint64_t lat_ns;

					s->in_flight = 0;

					if (evs[i].res != (long)g_block_size || evs[i].res2 != 0) {
						g_err_count++;
						g_stop = 1;
						break;
					}

					clock_gettime(CLOCK_MONOTONIC, &cnow);
					if (ts_before(&cnow, &g_deadline_ts)) {
						lat_ns = ts_to_ns(&cnow) - ts_to_ns(&s->submit_ts);
						g_total_lat_ns += lat_ns;
						g_completed_ios++;
						submit_one(s);
					}
				}
			}
		}
	} else {
		while (!g_stop) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (!ts_before(&now, &g_deadline_ts)) {
				break;
			}
			if (submit_one_sync(&g_slots[0]) != 0) {
				break;
			}
			{
				struct timespec cnow;
				uint64_t lat_ns;
				clock_gettime(CLOCK_MONOTONIC, &cnow);
				if (ts_before(&cnow, &g_deadline_ts)) {
					lat_ns = ts_to_ns(&cnow) - ts_to_ns(&g_slots[0].submit_ts);
					g_total_lat_ns += lat_ns;
					g_completed_ios++;
				}
			}
		}
	}
	g_stop = 1;

	clock_gettime(CLOCK_MONOTONIC, &end_ts);
	if (!ts_before(&g_deadline_ts, &end_ts)) {
		end_ts = g_deadline_ts;
	}

	if (g_use_aio) {
		struct timespec zero_ts = {0, 0};
		while (1) {
			int nr;
			in_flight = 0;
			for (i = 0; i < (int)g_qd; i++) {
				if (g_slots[i].in_flight) {
					in_flight++;
				}
			}
			if (in_flight == 0) {
				break;
			}
			nr = io_getevents(g_aio_ctx, 0, g_qd, evs, &zero_ts);
			for (i = 0; i < nr; i++) {
				struct io_slot *s = (struct io_slot *)evs[i].data;
				if (s) {
					s->in_flight = 0;
					if (evs[i].res != (long)g_block_size || evs[i].res2 != 0) {
						g_err_count++;
					}
				}
			}
		}
	}

	elapsed_ns = ts_to_ns(&end_ts) - ts_to_ns(&g_start_ts);
	elapsed_sec = (double)elapsed_ns / 1e9;
	iops = (elapsed_sec > 0) ? (double)g_completed_ios / elapsed_sec : 0;
	mibs = iops * g_block_size / (1024.0 * 1024.0);
	avg_lat_us = (g_completed_ios > 0)
		     ? (double)g_total_lat_ns / (double)g_completed_ios / 1000.0
		     : 0;

	printf("\n========== Results ==========\n");
	printf("IOPS        : %.2f\n", iops);
	printf("Throughput  : %.2f MiB/s (%.2f GiB/s)\n", mibs, mibs / 1024.0);
	printf("Avg latency : %.2f us\n", avg_lat_us);
	printf("Completed   : %ju I/Os in %.2f s\n", (uintmax_t)g_completed_ios, elapsed_sec);
	if (g_err_count) {
		printf("Errors      : %ju\n", (uintmax_t)g_err_count);
		rc = 1;
	}
	printf("=============================\n");

exit_cleanup:
	for (i = 0; i < (int)g_alloc_qd; i++) {
		if (g_slots[i].buf != NULL) {
			free(g_slots[i].buf);
			g_slots[i].buf = NULL;
		}
	}
	if (g_use_aio) {
		io_destroy(g_aio_ctx);
	}
exit_close:
	fsync(g_fd);
	close(g_fd);
	return rc;
}
