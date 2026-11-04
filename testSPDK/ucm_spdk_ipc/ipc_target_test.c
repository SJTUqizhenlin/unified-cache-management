/* Exercise the actual target without linking or initializing SPDK/DPDK. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#define main ipc_target_main
#include "ucm_spdk_ipc_target.c"
#undef main

#define SECTOR_SIZE 512u

struct spdk_nvme_qpair {
	spdk_nvme_cmd_cb cb;
	void *cb_arg;
};

static uint8_t mock_disk[2 * SECTOR_SIZE];
static uint64_t mock_ticks;
static unsigned mock_polls;
static int mock_submit_error;
static bool mock_completion_error;

static void
test_cpu_pinning(void)
{
	const struct target_args defaults = g_args;
	struct target_args args = {0};
	char map[CPU_SETSIZE * 24];
	char boundary[64];
	const char *invalid[] = {
		"", ",", ",1", "1,", "1,,2", "1,1", "01,1", "-1", "+1",
		" 1", "1 ", "1, 2", "1\n", "1x", "0x10", "1.0", "1-3",
		"99999999999999999999999999999999999999999999999999999",
	};
	struct {
		char *argv[10];
		bool valid;
		bool explicit_cpus;
		uint32_t count;
		int first_cpu;
	} cases[] = {
		{{"target"}, true, false, 32, 4},
		{{"target", "--reactors", "2", "--cpus", "36,4"}, true, true, 2, 36},
		{{"target", "--cpus", "36,4", "--reactors", "2"}, true, true, 2, 36},
		{{"target", "--cpu-base", "10", "--reactors", "2"}, true, false, 0, 10},
		{{"target", "--cpu-base", "-1", "--reactors", "2"}, true, false, 0, -1},
		{{"target", "--cpu-base", "10", "--cpus", "36,4", "--reactors", "2"},
		 true, true, 2, 36},
		{{"target", "--cpus", "36,4", "--cpu-base", "10", "--reactors", "2"},
		 true, true, 2, 36},
		{{"target", "--cpus", "36,4", "--lcore-map", "0@36,1@4", "--reactors", "2"},
		 true, true, 2, 36},
		{{"target", "--cpus", "4,36"}, false, false, 0, 0},
		{{"target", "--reactors", "1", "--cpus", "4,36"}, false, false, 0, 0},
		{{"target", "--reactors", "3", "--cpus", "4,36"}, false, false, 0, 0},
		{{"target", "--reactors", "2"}, false, false, 0, 0},
		{{"target", "--reactors", "2", "--cpus", "4,4"}, false, false, 0, 0},
		{{"target", "--reactors", "2x"}, false, false, 0, 0},
		{{"target", "--reactors", "0"}, false, false, 0, 0},
		{{"target", "--reactors", "-1"}, false, false, 0, 0},
		{{"target", "--reactors", "4294967298"}, false, false, 0, 0},
		{{"target", "--cpu-base", "-2"}, false, false, 0, 0},
		{{"target", "--cpu-base", "10x"}, false, false, 0, 0},
		{{"target", "--cpu-base", "9999999999999999999999"}, false, false, 0, 0},
	};

	assert(DEFAULT_REACTORS == 32 && defaults.lcore_map == NULL);
	for (uint32_t i = 0; i < defaults.reactors; i++) {
		assert(reactor_cpu(&defaults, i) == 4 + (int)(i % 8) * 32 + (int)(i / 8));
	}
	assert(generate_lcore_map(&defaults, map, sizeof(map)) == 0);
	assert(strcmp(map,
		"0@4,1@36,2@68,3@100,4@132,5@164,6@196,7@228,"
		"8@5,9@37,10@69,11@101,12@133,13@165,14@197,15@229,"
		"16@6,17@38,18@70,19@102,20@134,21@166,22@198,23@230,"
		"24@7,25@39,26@71,27@103,28@135,29@167,30@199,31@231") == 0);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		assert(parse_cpu_list(invalid[i], &args) != 0);
	}
	assert(parse_cpu_list(NULL, &args) != 0);
	{
		size_t used = 0;

		for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
			int n = snprintf(map + used, sizeof(map) - used, "%s%d", cpu ? "," : "", cpu);

			assert(n > 0 && (size_t)n < sizeof(map) - used);
			used += (size_t)n;
		}
		assert(parse_cpu_list(map, &args) == 0);
		assert(args.num_cpus == CPU_SETSIZE);
		for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
			assert(args.cpus[cpu] == cpu);
		}
	}
	snprintf(boundary, sizeof(boundary), "0,%d", CPU_SETSIZE - 1);
	assert(parse_cpu_list(boundary, &args) == 0);
	assert(args.num_cpus == 2 && args.explicit_cpus);
	assert(args.cpus[0] == 0 && args.cpus[1] == CPU_SETSIZE - 1);
	snprintf(boundary, sizeof(boundary), "%d", CPU_SETSIZE);
	assert(parse_cpu_list(boundary, &args) != 0);
	assert(args.num_cpus == 2 && args.cpus[1] == CPU_SETSIZE - 1);
	assert(parse_cpu_list("36,4,228", &args) == 0);
	args.reactors = 3;
	assert(reactor_cpu(&args, 0) == 36 && reactor_cpu(&args, 1) == 4 &&
	       reactor_cpu(&args, 2) == 228);
	assert(generate_lcore_map(&args, map, sizeof(map)) == 0);
	assert(strcmp(map, "0@36,1@4,2@228") == 0);
	assert(generate_lcore_map(&args, map, strlen("0@36,1@4,2@228")) != 0);
	args.num_cpus = 0;
	args.cpu_base = 10;
	assert(reactor_cpu(&args, 2) == 12);
	assert(generate_lcore_map(&args, map, sizeof(map)) == 0);
	assert(strcmp(map, "0@10,1@11,2@12") == 0);
	args.cpu_base = -1;
	assert(reactor_cpu(&args, 2) == -1);
	assert(generate_lcore_map(&args, map, sizeof(map)) == 0);
	assert(strcmp(map, "0@0,1@1,2@2") == 0);

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		int argc = 0;

		while (cases[i].argv[argc] != NULL) {
			argc++;
		}
		g_args = defaults;
		optind = 0;
		assert((parse_args(argc, cases[i].argv) == 0) == cases[i].valid);
		if (cases[i].valid) {
			assert(g_args.explicit_cpus == cases[i].explicit_cpus);
			assert(g_args.num_cpus == cases[i].count);
			assert(reactor_cpu(&g_args, 0) == cases[i].first_cpu);
			if (g_args.lcore_map != NULL) {
				assert(strcmp(g_args.lcore_map, "0@36,1@4") == 0);
			}
		}
	}
	{
		char *argv[] = {"target", "--cpu-base", boundary, NULL};

		g_args = defaults;
		optind = 0;
		assert(parse_args(3, argv) != 0);
		snprintf(boundary, sizeof(boundary), "%d", CPU_SETSIZE - 1);
		g_args = defaults;
		optind = 0;
		assert(parse_args(3, argv) != 0);
		{
			char *single[] = {"target", "--cpu-base", boundary, "--reactors", "1", NULL};

			g_args = defaults;
			optind = 0;
			assert(parse_args(5, single) == 0);
			assert(reactor_cpu(&g_args, 0) == CPU_SETSIZE - 1);
		}
	}
	g_args = defaults;
	optind = 0;
}

uint64_t
spdk_get_ticks(void)
{
	return ++mock_ticks;
}

uint64_t
spdk_get_ticks_hz(void)
{
	return 1000000;
}

void
spdk_nvme_ctrlr_get_default_io_qpair_opts(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_io_qpair_opts *opts, size_t opts_size)
{
	(void)ctrlr;
	memset(opts, 0, opts_size);
}

struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
		const struct spdk_nvme_io_qpair_opts *opts, size_t opts_size)
{
	(void)ctrlr;
	(void)opts_size;
	assert(opts->io_queue_size == 1);
	return calloc(1, sizeof(struct spdk_nvme_qpair));
}

int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->cb == NULL);
	free(qpair);
	return 0;
}

int
spdk_mem_unregister(void *vaddr, size_t len)
{
	(void)vaddr;
	(void)len;
	assert(!"test must not register DMA mappings");
	return 0;
}

static int
mock_submit(struct spdk_nvme_qpair *qpair, spdk_nvme_cmd_cb cb, void *cb_arg)
{
	assert(qpair->cb == NULL);
	if (mock_submit_error != 0) {
		return mock_submit_error;
	}
	qpair->cb = cb;
	qpair->cb_arg = cb_arg;
	return 0;
}

int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		void *payload, uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb, void *cb_arg, uint32_t io_flags)
{
	(void)ns;
	assert(io_flags == 0 && lba < 2 && lba_count == 1);
	memcpy(mock_disk + lba * SECTOR_SIZE, payload, SECTOR_SIZE);
	return mock_submit(qpair, cb, cb_arg);
}

int
spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		void *payload, uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb, void *cb_arg, uint32_t io_flags)
{
	(void)ns;
	assert(io_flags == 0 && lba < 2 && lba_count == 1);
	memcpy(payload, mock_disk + lba * SECTOR_SIZE, SECTOR_SIZE);
	return mock_submit(qpair, cb, cb_arg);
}

int
spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		spdk_nvme_cmd_cb cb, void *cb_arg)
{
	(void)ns;
	return mock_submit(qpair, cb, cb_arg);
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair,
		uint32_t max_completions)
{
	struct spdk_nvme_cpl cpl = {0};
	spdk_nvme_cmd_cb cb = qpair->cb;

	assert(max_completions == 0);
	mock_polls++;
	if (cb == NULL) {
		return 0;
	}
	qpair->cb = NULL;
	cpl.status.sc = mock_completion_error ? 1 : 0;
	cb(qpair->cb_arg, &cpl);
	return 1;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	(void)ctrlr;
	/* Stop at the end of one real reactor iteration, even with a broken list. */
	g_stop = 1;
	return 0;
}

static void
test_two_reactor_stats(void)
{
	struct ipc_reactor reactors[2] = {0};
	uint8_t data[2][SECTOR_SIZE] = {{0}};
	char actual[2048], expected[2048];

	g_reactors = reactors;
	g_nreactors = 2;
	assert(_Alignof(struct ipc_reactor_stats) >= 128);
	assert(sizeof(struct ipc_reactor_stats) % 128 == 0);
	for (unsigned i = 0; i < 2; i++) {
		struct ipc_session *s = session_alloc();

		assert((uintptr_t)&reactors[i].stats % 128 == 0);
		reactor_stats_init(&reactors[i].stats);
		assert(s != NULL);
		s->control_size = sizeof(*s->ctl);
		s->ctl = mmap(NULL, s->control_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(s->ctl != MAP_FAILED);
		ucm_ipc_control_init(s->ctl, 1, sizeof(data[i]));
		s->max_outstanding = 1;
		s->qpair_depth = 1;
		s->regions[0].data = data[i];
		s->regions[0].size = sizeof(data[i]);
		assert(reactor_attach_session(&reactors[i], s) == 0);
		session_registry_add(s);

		for (unsigned phase = 0; phase < 5; phase++) {
			struct ucm_ipc_submit_entry e = {
				.req_id = phase,
				.nbytes = SECTOR_SIZE,
				.opcode = phase == 1 ? UCM_IPC_OP_READ :
					  (phase == 2 || phase == 4 ? UCM_IPC_OP_FLUSH :
					   UCM_IPC_OP_WRITE),
			};

			mock_completion_error = phase == 1;
			mock_submit_error = phase >= 3 ? -ENOMEM : 0;
			session_handle_op(s, &e);
			spdk_nvme_qpair_process_completions(s->qpair, 0);
			assert(atomic_load(&s->ctl->completion_head) == phase + 1);
			assert(s->ctl->completion[phase].req_id == phase);
			assert(s->ctl->completion[phase].status ==
			       (phase >= 3 ? -ENOMEM : (phase == 1 ? -EIO : 0)));
			assert(s->outstanding == 0 && s->free_top == 1);
		}
		assert(atomic_load(&reactors[i].stats.read_ops) == 1);
		assert(atomic_load(&reactors[i].stats.write_ops) == 1);
		assert(atomic_load(&reactors[i].stats.flush_ops) == 1);
		assert(atomic_load(&reactors[i].stats.completion_errors) == 1);
		assert(atomic_load(&reactors[i].stats.submit_errors) == 2);
	}
	/* Compare the complete wire response, then verify totals survive detach. */
	for (unsigned closed = 0; closed < 2; closed++) {
		int fds[2];
		size_t done = 0;
		ssize_t n;

		snprintf(expected, sizeof(expected),
			 "{\"version\":1,\"name\":\"ipc-target\","
			 "\"tick_rate\":1000000,\"ticks\":%" PRIu64 ","
			 "\"bytes_read\":1024,\"bytes_written\":1024,"
			 "\"num_read_ops\":2,\"num_write_ops\":2,\"num_flush_ops\":2,"
			 "\"read_latency_ticks\":2,\"write_latency_ticks\":2,"
			 "\"completion_errors\":2,\"submit_errors\":4,"
			 "\"connections\":%u,\"qpairs\":%u,"
			 "\"num_sectors\":2,\"sector_size\":512}\n",
			 mock_ticks + 1, closed ? 0u : 2u, closed ? 0u : 2u);
		assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
		assert(handle_stats(fds[0]) == 0);
		while ((n = recv(fds[1], actual + done, sizeof(actual) - 1 - done, 0)) > 0) {
			done += (size_t)n;
			assert(done < sizeof(actual) - 1);
		}
		assert(n == 0);
		actual[done] = '\0';
		assert(strcmp(actual, expected) == 0);
		close(fds[1]);
		if (!closed) {
			for (unsigned i = 0; i < 2; i++) {
				reactor_detach_session(&reactors[i], reactors[i].active);
			}
		}
	}
	assert(g_sessions == NULL && atomic_load(&g_active_sessions) == 0);
	g_reactors = NULL;
	g_nreactors = 0;
	mock_submit_error = 0;
	mock_completion_error = false;
}

int
main(void)
{
	struct ipc_reactor reactor = {
		.cpu_id = -1,
		.admin = true,
		.mu = PTHREAD_MUTEX_INITIALIZER,
	};
	struct ipc_session *sessions[2];
	uint8_t data[2][2 * SECTOR_SIZE] = {{0}};

	test_cpu_pinning();
	puts("ipc_target_test: strict CPU parsing, precedence and per-index lcore mapping passed");
	g_sector_size = SECTOR_SIZE;
	g_num_sectors = 2;
	reactor_stats_init(&reactor.stats);
	for (unsigned i = 0; i < 2; i++) {
		struct ipc_session *s = session_alloc();

		assert(s != NULL);
		sessions[i] = s;
		s->control_size = sizeof(*s->ctl);
		s->ctl = mmap(NULL, s->control_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(s->ctl != MAP_FAILED);
		ucm_ipc_control_init(s->ctl, 1, sizeof(data[i]));
		s->max_outstanding = 1;
		s->qpair_depth = 1;
		s->regions[0].data = data[i];
		s->regions[0].size = sizeof(data[i]);
		memset(data[i], 0x31 + i, SECTOR_SIZE);

		/* Match hello ordering: pending -> attached -> registry registration. */
		reactor_push_pending(&reactor, s);
		reactor_drain_pending(&reactor);
		assert(s->attach_done && s->attach_status == 0);
		session_registry_add(s);
	}
	assert(g_sessions == sessions[1]);
	assert(g_sessions->next_pending == sessions[0]);
	assert(sessions[0]->next_pending == NULL);
	assert(atomic_load(&g_active_sessions) == 2);
	assert(reactor.active == sessions[1]);
	assert(reactor.active->next_active == sessions[0]);
	assert(sessions[0]->next_active == NULL);
	assert(sessions[0]->qpair != sessions[1]->qpair);

	for (unsigned phase = 0; phase < 2; phase++) {
		for (unsigned i = 0; i < 2; i++) {
			struct ucm_ipc_control *ctl = sessions[i]->ctl;
			struct ucm_ipc_submit_entry e = {
				.req_id = 100 + 10 * i + phase,
				.offset_bytes = i * SECTOR_SIZE,
				.data_offset = phase * SECTOR_SIZE,
				.nbytes = SECTOR_SIZE,
				.opcode = phase == 0 ? UCM_IPC_OP_WRITE : UCM_IPC_OP_READ,
				.region_id = 0,
			};

			ctl->submit[phase] = e;
			atomic_store_explicit(&ctl->submit_head, phase + 1, memory_order_release);
		}
		g_stop = 0;
		reactor_run(&reactor);
		assert(mock_polls == 2 * (phase + 1));
		for (unsigned i = 0; i < 2; i++) {
			struct ipc_session *s = sessions[i];
			struct ucm_ipc_control *ctl = s->ctl;

			assert(atomic_load(&ctl->submit_tail) == phase + 1);
			assert(atomic_load_explicit(&ctl->completion_head,
						   memory_order_acquire) == phase + 1);
			assert(ctl->completion[phase].req_id == 100 + 10 * i + phase);
			assert(ctl->completion[phase].status == 0);
			assert(s->outstanding == 0 && s->free_top == s->qpair_depth);
			assert(s->tasks[0].in_use == 0);
			atomic_store_explicit(&ctl->completion_tail, phase + 1,
					      memory_order_release);
			assert(memcmp(mock_disk + i * SECTOR_SIZE, data[i], SECTOR_SIZE) == 0);
			if (phase == 1) {
				assert(memcmp(data[i], data[i] + SECTOR_SIZE, SECTOR_SIZE) == 0);
			}
		}
	}
	assert(atomic_load(&reactor.stats.write_ops) == 2);
	assert(atomic_load(&reactor.stats.read_ops) == 2);
	assert(atomic_load(&reactor.stats.bytes_written) == 2 * SECTOR_SIZE);
	assert(atomic_load(&reactor.stats.bytes_read) == 2 * SECTOR_SIZE);
	assert(atomic_load(&reactor.stats.submit_errors) == 0);
	assert(atomic_load(&reactor.stats.completion_errors) == 0);

	while (reactor.active != NULL) {
		reactor_detach_session(&reactor, reactor.active);
	}
	assert(g_sessions == NULL && atomic_load(&g_active_sessions) == 0);
	pthread_mutex_destroy(&reactor.mu);
	puts("ipc_target_test: two-session registry and read/write dispatch passed");
	test_two_reactor_stats();
	puts("ipc_target_test: two-reactor stats, flush/errors, JSON and detach passed");
	return 0;
}
