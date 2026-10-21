# UCM Store Serving Benchmark

## Posix Baseline: 2026-10-20

Final results supplied by the user after the run completed:

| Metric | Result |
| --- | ---: |
| Successful requests | 3946 |
| Benchmark duration (s) | 4261.42 |
| Total input tokens | 56616648 |
| Total generated tokens | 593892 |
| Request throughput (req/s) | 0.93 |
| Output token throughput (tok/s) | 139.36 |
| Total token throughput (tok/s) | 13425.23 |

### Run Context

- Server: `server_test_host.sh`, port `7780`.
- Model: `/mnt/model/DeepSeek-V2-Lite-Chat`.
- Runtime: vLLM `0.20.2`, vLLM-Ascend `0.20.2rc1`.
- TP: 4; devices `0,1,2,3`; eager mode; prefix caching enabled.
- Model block size: 128 tokens; maximum model length: 65536.
- Pipeline: `Cache|Posix`; `use_layerwise: false`.
- Posix engine confirmed by startup logs: `psync`, `io_direct=true`,
  `data_trans_concurrency=128`.
- Storage path: `/home/qizhenlin/ucm-cache/test`, on `/home` ext4/LVM.
- Shared host Cache: 100 GiB; `share_buffer_enable=true`;
  `shm_hugepage_advise=true`.
- No `posix_capacity_gb` was set in the inspected YAML. The remaining
  `spdk_capacity_gb: 500` does not configure Posix capacity or GC.
- Client: `client_test_host.sh`; full `synthetic_trace_64k.jsonl` replay;
  maximum concurrency 20, as observed in the client command line.

### Comparison Boundaries

- These are full-run serving results, not isolated storage throughput.
- HBM and host Cache hits are included; UCM hits are not necessarily SSD reads.
- The Posix filesystem path and SPDK raw SSD `0000:84:00.0` are different
  storage paths. A comparison measures the deployed backends, not a same-disk
  I/O-engine-only difference.
- The exact cache contents at replay start were not captured here. Do not
  label this run strictly cold-cache without additional evidence.
- No completed SPDK serving result has been recorded in this file yet.

## SPDK Interrupted Run: 2026-10-20

This run failed; the following client summary is NOT a successful full-trace
baseline and must not be compared directly with the completed Posix run.

| Metric | Result |
| --- | ---: |
| Successful requests | 2988 |
| Benchmark duration (s) | 3493.65 |
| Total input tokens | 38787937 |
| Total generated tokens | 525229 |
| Request throughput (req/s) | 0.86 |
| Output token throughput (tok/s) | 150.34 |
| Total token throughput (tok/s) | 11252.75 |

- Pipeline: `Cache|Spdk`, RDMA loopback, configured pool 500 GiB,
  shared host Cache 100 GiB, Direct mode, TP4.
- First recorded failure: `14:18:36.012391`, EngineCore PID `342049`
  reported that `VllmWorker-0` (PID `346618`) died unexpectedly.
- At `14:18:44`, executor cancellation propagated as `EngineDeadError`
  to API PID `339137`, producing client request failures.
- Worker 0's Python/UCM logs contained no fatal exception. No matching
  coredump, kernel OOM/segfault report or target disconnect was found in
  the inspected crash window. The initiating cause remains unknown.
- Raw server terminal stdout/stderr was not preserved by the launcher for
  this run. The launcher now records it and enables Python faulthandler
  for subsequent runs; this cannot recover the missing earlier evidence.
