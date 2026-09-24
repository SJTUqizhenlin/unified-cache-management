#include "spdk/config.h"
#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

#define MAX_QD 1024

enum workload { WL_RAND_READ, WL_RAND_WRITE, WL_RAND_RW };

struct io_slot {
	void		*buf;
	uint64_t	lba;
	uint64_t	submit_tsc;
	int		in_flight;
};

static struct spdk_nvme_ctrlr		*g_ctrlr;
static struct spdk_nvme_ns		*g_ns;
static struct spdk_nvme_qpair		*g_qpair;
static struct spdk_nvme_transport_id	g_trid;

static struct io_slot g_slots[MAX_QD];
static uint32_t	g_qd = 128;
/* Slot/buffer count: at least PREWRITE_QD so the pre-write phase runs at
 * full depth regardless of the (possibly low) test -q value. */
#define PREWRITE_QD 128
static uint32_t	g_alloc_qd = PREWRITE_QD;
static uint32_t	g_block_size = 4096;
static uint32_t	g_lba_count;
static uint64_t	g_num_sectors;
static uint32_t	g_sector_size;
static enum workload g_wl = WL_RAND_READ;
static uint32_t	g_write_pct = 30;
static int	g_runtime_sec = 10;
static uint64_t	g_span_gib = 0;
static uint64_t	g_offset_gib = 0;
static uint64_t	g_start_lba;
static uint64_t	g_io_range;

static uint64_t	g_start_tsc;
static uint64_t	g_deadline_tsc;
static uint64_t	g_tsc_hz;
static int	g_stop;
static uint64_t	g_completed_ios;
static uint64_t	g_total_lat_ticks;
static uint64_t	g_err_count;
static uint64_t	g_last_progress_tsc;
static int	g_stalled;

static void io_complete(void *arg, const struct spdk_nvme_cpl *cpl);

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
	const struct spdk_nvme_ctrlr_data *cdata;
	int nsid;

	g_ctrlr = ctrlr;
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	printf("Attached: %s  %.40s FW %.8s\n", trid->traddr, cdata->mn, cdata->fr);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		g_ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (g_ns != NULL && spdk_nvme_ns_is_active(g_ns)) {
			break;
		}
		g_ns = NULL;
	}
}

static uint64_t
rand_u64(void)
{
	return ((uint64_t)rand() << 33) ^ ((uint64_t)rand() << 2) ^ (uint64_t)rand();
}

/* Poll the IO qpair and the admin qpair. The admin poll is mandatory on
 * fabrics transports: SPDK negotiates a 10s keep-alive timeout by default
 * and only sends keep-alives from spdk_nvme_ctrlr_process_admin_completions;
 * never polling it makes an nvmf target drop the connection after 10s of
 * admin silence (mid-run, which hangs this tool). */
static void
poll_all(void)
{
	spdk_nvme_qpair_process_completions(g_qpair, 0);
	spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
}

/* Watchdog: if no IO completes for 30s the connection is dead with IOs
 * outstanding; abort instead of polling forever. */
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
submit_one(struct io_slot *s)
{
	uint64_t usable = (g_io_range > g_lba_count) ? (g_io_range - g_lba_count) : 0;
	int is_write;
	int rc;

	s->lba = g_start_lba + ((usable > 0) ? (rand_u64() % (usable + 1)) : 0);
	s->submit_tsc = spdk_get_ticks();
	s->in_flight = 1;

	is_write = (g_wl == WL_RAND_WRITE) ||
		   (g_wl == WL_RAND_RW && ((uint32_t)rand() % 100 < g_write_pct));

	if (is_write) {
		rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, s->buf, s->lba, g_lba_count,
					    io_complete, s, 0);
	} else {
		rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, s->buf, s->lba, g_lba_count,
					   io_complete, s, 0);
	}
	if (rc < 0) {
		s->in_flight = 0;
		g_err_count++;
		g_stop = 1;
		fprintf(stderr, "submission failed (rc=%d): request pool/SQ exhausted, "
			"lower -q or -o\n", rc);
	}
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_slot *s = arg;
	uint64_t now;

	s->in_flight = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		g_err_count++;
		g_stop = 1;
		return;
	}

	now = spdk_get_ticks();
	g_last_progress_tsc = now;
	if (now < g_deadline_tsc) {
		g_total_lat_ticks += now - s->submit_tsc;
		g_completed_ios++;
		submit_one(s);
	}
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
prewrite_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct io_slot *s = arg;
	s->in_flight = 0;
	g_last_progress_tsc = spdk_get_ticks();
	if (spdk_nvme_cpl_is_error(cpl)) {
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

	printf("Pre-writing working set [%ju, %ju) ~%lu GiB ...\n",
	       (uintmax_t)g_start_lba, (uintmax_t)end, (unsigned long)g_span_gib);

	g_last_progress_tsc = spdk_get_ticks();
	while (lba + g_lba_count <= end && !g_stop) {
		for (slot = 0; slot < (int)g_alloc_qd; slot++) {
			if (!g_slots[slot].in_flight) {
				break;
			}
		}
		if (slot == (int)g_alloc_qd) {
			poll_all();
			check_stall();
			continue;
		}
		g_slots[slot].lba = lba;
		if (spdk_nvme_ns_cmd_write(g_ns, g_qpair, g_slots[slot].buf,
					   lba, g_lba_count, prewrite_complete,
					   &g_slots[slot], 0) < 0) {
			/* Request pool/SQ momentarily exhausted: poll and retry
			 * the same LBA on the next iteration. */
			poll_all();
			check_stall();
			continue;
		}
		g_slots[slot].in_flight = 1;
		lba += g_lba_count;
		written += g_lba_count;
		slot = (slot + 1) % g_alloc_qd;
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
		poll_all();
		check_stall();
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
	printf("  -r <trid>     Transport ID, e.g. 'trtype:PCIe traddr:0000:84:00.0'\n");
	printf("                (default: trtype:PCIe traddr:0000:84:00.0)\n");
	printf("  -q <depth>    Queue depth (default 128, max %d)\n", MAX_QD);
	printf("  -o <KB>       Block size in KB, multiple of sector size (default 4)\n");
	printf("  -t <sec>      Run time in seconds (default 10)\n");
	printf("  -w <type>     Workload: randread | randwrite | randrw (default randread)\n");
	printf("  -M <pct>      Write %% for randrw, 0-100 (default 30)\n");
	printf("  -S <GiB>      Working-set size in GiB; if >0 the set is pre-written before the test (default 0 = whole namespace, no pre-write)\n");
	printf("  -O <GiB>      Working-set offset in GiB (default 0)\n");
	printf("  -s <seed>     PRNG seed (default 1)\n");
	printf("  -h            Show this help\n");
}

int
main(int argc, char **argv)
{
	int op, rc, i;
	struct spdk_env_opts opts;
	const char *wl_names[] = {"randread", "randwrite", "randrw"};
	uint64_t end_tsc, elapsed_ticks, in_flight;
	double elapsed_sec, iops, mibs, avg_lat_us;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	srand(1);
	while ((op = getopt(argc, argv, "r:q:o:t:w:M:S:O:s:h")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "bad trid: %s\n", optarg);
				return 1;
			}
			break;
		case 'q':
			g_qd = (uint32_t)spdk_strtol(optarg, 10);
			break;
		case 'o':
			g_block_size = (uint32_t)spdk_strtol(optarg, 10) * 1024;
			break;
		case 't':
			g_runtime_sec = spdk_strtol(optarg, 10);
			break;
		case 'w':
			if (parse_wl(optarg) != 0) {
				fprintf(stderr, "bad workload: %s\n", optarg);
				return 1;
			}
			break;
		case 'M':
			g_write_pct = (uint32_t)spdk_strtol(optarg, 10);
			break;
		case 'S':
			g_span_gib = (uint64_t)spdk_strtol(optarg, 10);
			break;
		case 'O':
			g_offset_gib = (uint64_t)spdk_strtol(optarg, 10);
			break;
		case 's':
			srand((unsigned)spdk_strtol(optarg, 10));
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

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nvme_iops_test";

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
	if (g_block_size % g_sector_size != 0) {
		fprintf(stderr, "block size %u not multiple of sector size %u\n",
			g_block_size, g_sector_size);
		rc = 1;
		goto exit_fini;
	}
	g_lba_count = g_block_size / g_sector_size;
	g_num_sectors = spdk_nvme_ns_get_num_sectors(g_ns);

	g_start_lba = (g_offset_gib * (1024ULL * 1024 * 1024)) / g_sector_size;
	if (g_start_lba >= g_num_sectors) {
		fprintf(stderr, "offset %lu GiB beyond namespace (%.2f GiB)\n",
			(unsigned long)g_offset_gib,
			(double)spdk_nvme_ns_get_size(g_ns) / (1024.0 * 1024 * 1024));
		rc = 1;
		goto exit_fini;
	}
	if (g_span_gib == 0) {
		g_io_range = g_num_sectors - g_start_lba;
	} else {
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
		rc = 1;
		goto exit_fini;
	}

	printf("NS %u: %ju sectors, %uB, %.2f GiB | span %.3f GiB @ offset %lu GiB | LBA [%ju, %ju) | QD %u | %uKB | %ds | %s",
	       spdk_nvme_ns_get_id(g_ns), (uintmax_t)g_num_sectors, g_sector_size,
	       (double)spdk_nvme_ns_get_size(g_ns) / (1024.0 * 1024 * 1024),
	       (double)g_io_range * g_sector_size / (1024.0 * 1024 * 1024),
	       (unsigned long)g_offset_gib,
	       (uintmax_t)g_start_lba, (uintmax_t)(g_start_lba + g_io_range),
	       g_qd, g_block_size / 1024, g_runtime_sec, wl_names[g_wl]);
	if (g_wl == WL_RAND_RW) {
		printf(" (write %u%%)", g_write_pct);
	}
	printf("\n");

	{
		struct spdk_nvme_io_qpair_opts qopts;
		uint32_t max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
		uint32_t split_parts = (g_block_size + max_xfer - 1) / max_xfer;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts, sizeof(qopts));
		/* IOs larger than MDTS are split into split_parts child commands
		 * plus one parent request each; size the SQ and the request pool
		 * so g_qd in-flight IOs cannot exhaust them (defaults are only
		 * 256 entries / 512 requests, which hangs at high -q). */
		qopts.io_queue_size = g_alloc_qd * split_parts + 1;
		qopts.io_queue_requests = g_alloc_qd * (split_parts + 1) + 256;
		g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qopts, sizeof(qopts));
	}
	if (g_qpair == NULL) {
		fprintf(stderr, "alloc_io_qpair failed\n");
		rc = 1;
		goto exit_fini;
	}

	for (i = 0; i < (int)g_alloc_qd; i++) {
		g_slots[i].buf = spdk_zmalloc(g_block_size, 4096, NULL,
					      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
		if (g_slots[i].buf == NULL) {
			fprintf(stderr, "buffer alloc failed at slot %d\n", i);
			rc = 1;
			goto exit_slots;
		}
		memset(g_slots[i].buf, 0xaa ^ (i & 0xff), g_block_size);
	}

	if (g_span_gib > 0) {
		prewrite_working_set();
		if (g_stop || g_err_count) {
			fprintf(stderr, "pre-write failed\n");
			rc = 1;
			goto exit_slots;
		}
	}

	g_tsc_hz = spdk_get_ticks_hz();
	g_start_tsc = spdk_get_ticks();
	g_deadline_tsc = g_start_tsc + (uint64_t)g_runtime_sec * g_tsc_hz;
	g_last_progress_tsc = g_start_tsc;

	for (i = 0; i < (int)g_qd; i++) {
		submit_one(&g_slots[i]);
	}

	while (!g_stop && spdk_get_ticks() < g_deadline_tsc) {
		poll_all();
		check_stall();
	}
	g_stop = 1;

	end_tsc = spdk_get_ticks();
	if (end_tsc > g_deadline_tsc) {
		end_tsc = g_deadline_tsc;
	}
	while (1) {
		in_flight = 0;
		for (i = 0; i < (int)g_qd; i++) {
			if (g_slots[i].in_flight) {
				in_flight++;
			}
		}
		if (in_flight == 0) {
			break;
		}
		poll_all();
		check_stall();
		if (g_stalled) {
			break;
		}
	}

	elapsed_ticks = end_tsc - g_start_tsc;
	elapsed_sec = (double)elapsed_ticks / (double)g_tsc_hz;
	iops = (elapsed_sec > 0) ? (double)g_completed_ios / elapsed_sec : 0;
	mibs = iops * g_block_size / (1024.0 * 1024.0);
	avg_lat_us = (g_completed_ios > 0)
		     ? (double)g_total_lat_ticks / (double)g_completed_ios * 1e6 / (double)g_tsc_hz
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
	(void)rc;

exit_slots:
	for (i = 0; i < (int)g_alloc_qd; i++) {
		if (g_slots[i].buf != NULL) {
			spdk_free(g_slots[i].buf);
			g_slots[i].buf = NULL;
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
