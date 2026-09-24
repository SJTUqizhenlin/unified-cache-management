#include "spdk/config.h"
#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns *g_ns;
static struct spdk_nvme_transport_id g_trid;
static int g_completed;
static int g_rc;

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Probing %s\n", trid->traddr);
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

	printf("Attached to %s\n", trid->traddr);
	printf("  Model:           %.40s\n", cdata->mn);
	printf("  Serial:          %.20s\n", cdata->sn);
	printf("  Firmware:        %.8s\n", cdata->fr);
	printf("  PCI Vendor/Subsys Vendor: %04x/%04x\n", cdata->vid, cdata->ssvid);
	printf("  NVMe version:    %d.%d\n",
	       cdata->ver.raw >> 16, (cdata->ver.raw >> 8) & 0xff);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		g_ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (g_ns != NULL && spdk_nvme_ns_is_active(g_ns)) {
			break;
		}
		g_ns = NULL;
	}

	if (g_ns == NULL) {
		fprintf(stderr, "  no active namespace found\n");
		return;
	}

	printf("  Namespace ID:    %d\n", spdk_nvme_ns_get_id(g_ns));
	printf("  Namespace size:  %ju bytes (%.2f GB)\n",
	       (uintmax_t)spdk_nvme_ns_get_size(g_ns),
	       (double)spdk_nvme_ns_get_size(g_ns) / 1e9);
	printf("  Sector size:     %u bytes\n", spdk_nvme_ns_get_sector_size(g_ns));
	printf("  Max xfer size:   %u bytes\n", spdk_nvme_ctrlr_get_max_xfer_size(ctrlr));
}

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	char *buf = arg;
	int i;

	g_completed = 1;

	if (spdk_nvme_cpl_is_error(completion)) {
		fprintf(stderr, "Read I/O FAILED, status: %s\n",
			spdk_nvme_cpl_get_status_string_ext(&completion->status, SPDK_NVME_OPC_READ));
		g_rc = -1;
		return;
	}

	printf("\n[OK] Read 1 block at LBA 0 completed. First 64 bytes:\n");
	for (i = 0; i < 64; i++) {
		printf("%02x ", (unsigned char)buf[i]);
		if ((i + 1) % 16 == 0) {
			printf("\n");
		}
	}
	printf("\n");
}

static void
usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -r <trid>   Transport ID, e.g. 'trtype:PCIe traddr:0000:84:00.0'\n");
	printf("              (default: trtype:PCIe traddr:0000:84:00.0)\n");
	printf("  -h          Show this help\n");
}

int
main(int argc, char **argv)
{
	int op, rc;
	struct spdk_env_opts opts;
	struct spdk_nvme_qpair *qpair;
	char *buf;
	uint32_t sector;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.traddr, sizeof(g_trid.traddr), "0000:84:00.0");
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport ID: %s\n", optarg);
				return 1;
			}
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (op == 'h') ? 0 : 1;
		}
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nvme_cmd_test";

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Probing NVMe at %s ...\n", g_trid.traddr);
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit_fini;
	}

	if (g_ctrlr == NULL || g_ns == NULL) {
		fprintf(stderr, "No NVMe controller/namespace attached\n");
		rc = 1;
		goto exit_fini;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (qpair == NULL) {
		fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		rc = 1;
		goto exit_fini;
	}

	sector = spdk_nvme_ns_get_sector_size(g_ns);
	buf = spdk_zmalloc(sector, sector, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
	if (buf == NULL) {
		fprintf(stderr, "buffer allocation failed\n");
		rc = 1;
		goto exit_qpair;
	}

	printf("\nSubmitting READ: LBA 0, %u block(s), %u bytes ...\n", 1u, sector);
	rc = spdk_nvme_ns_cmd_read(g_ns, qpair, buf, 0, 1, read_complete, buf, 0);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_ns_cmd_read() failed: %d\n", rc);
		rc = 1;
		goto exit_buf;
	}

	while (!g_completed) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	if (g_rc == 0) {
		printf("\n[RESULT] SPDK successfully sent an NVMe Read command to %s\n",
		       g_trid.traddr);
		rc = 0;
	} else {
		rc = 1;
	}

exit_buf:
	spdk_free(buf);
exit_qpair:
	spdk_nvme_ctrlr_free_io_qpair(qpair);
exit_fini:
	fflush(stdout);
	spdk_nvme_detach_async(g_ctrlr, &detach_ctx);
	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
	spdk_env_fini();
	return rc;
}
