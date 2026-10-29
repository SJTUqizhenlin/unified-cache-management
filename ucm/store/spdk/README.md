# SPDK Store Partitions

Each `SpdkStore` owns an engine context with its own geometry, I/O qpair,
reactor, GC timer, task queue and region-scoped shared index. Store methods
select that context in thread-local scopes; background threads bind their
owner explicitly. The SPDK environment, controller references and host-memory
registration references are managed process-wide. Destroying one store does
not stop a sibling store or unregister its memory.

## FAWA Configuration

DeepSeek V4 uses the same logical block keys in two separate stores: FA holds
historical compressed cache rows, while WA holds window-boundary state. POSIX
isolates them with `fawa_fa/` and `fawa_wa/` directories. SPDK isolates them with
different offsets, on-disk records and POSIX shared-memory indexes. Within one
partition, workers and the scheduler share its index; FA and WA never share an
index or freelist. Lookups require both the FA prefix and a usable WA boundary.

Keep exactly one `Cache|Spdk` connector entry; see
[`examples/ucm_spdk_fawa_config.yaml`](../../../examples/ucm_spdk_fawa_config.yaml).
For FAWA only, `spdk_capacity_gb` is the total data budget (default 500 GiB):

| Store | Data Capacity | Start Offset, GiB |
| --- | --- | --- |
| FA | `total // 2` | `spdk_offset_gib` |
| WA | `total - total // 2` | `spdk_offset_gib + total // 2 + 2` |

Each region has a one-sector superblock, a fixed 1 GiB index reservation and
its data slots. Two extra GiB per region safely round the occupied span to
whole GiB offsets. Reserve `total + 4` GiB starting at the base. Metadata and
alignment are not included in the data capacity. Host Cache capacity continues
to be divided evenly between FA and WA.

Independent non-FAWA pipeline instances retain per-instance capacity semantics;
configure distinct, nonoverlapping `spdk_offset_gib` values manually. Same-process
overlaps on the same transport are rejected before region resolution. There is
no on-disk partition table or cross-process range allocator: deployment must
ensure unrelated pools and alternate endpoints do not overlap the same disk.
Changing total capacity changes the WA boundary; do not change it while any
worker/scheduler is attached or expect automatic migration.

## Upgrade And Persistence

Stop **all old workers and schedulers** before upgrading. The shared-index ABI
is now version 6, and maintenance locks are region-scoped rather than global.
Old and new binaries must not run together. The on-disk record format is
unchanged. Compatible existing region data can be recovered with
`spdk_format: 0`; conversion from a single pool to FA/WA is not automatic.

`spdk_format: 1` initializes each partition once per fresh service session,
using the corresponding FA/WA session suffix. It destroys existing metadata.
Geometry changes can also reset a region with format disabled. Never test
formatting against an unrelated existing pool.

Maintenance excludes attachments of the same region, not unrelated partitions.
Startup is still serialized process-wide/across workers. A completed old-slot
invalidation publishes a shared retirement marker; the last reader/overwrite
pin reclaims the slot even after the retiring engine exits. Process crashes
still require recovery; they are not transparent failover.

The SPDK environment remains alive until process exit. Do not externally call
`spdk_env_fini()` or unload the plugin while engines/resources are live.

## Load Admission And CPU Affinity

Each worker-store engine has 128 active Load contexts, independently of other
TP workers or the sibling FA/WA store. The ring holds pending operations and
the 256 I/O tokens are another limit, not a division of a shared TP queue.
Full-context retries return before acquiring the shared-index mutex; actual
lookup/pin/unpin operations retain robust locking and all worker-liveness checks.
The 30-second I/O progress watchdog and task failure handling are unchanged.

The wrapper forwards `cpu_affinity_cores` to the reactor's pthread attributes,
so affinity is applied before the reactor polls. Invalid/unusable masks fail
startup rather than silently running unbound. FAWA supplies its existing
`store_cores` when UCM connector CPU affinity is enabled. FA and WA use that
allowed mask; this does not reserve a dedicated core for either one. With no
configured cores, inherited affinity is preserved. Target CPU affinity is
independent and must be checked for overlap separately.

The target deployment script already sets `--max-queue-depth 1024`. Initiator
qpair sizing accounts for shard splitting and requests a depth up to 1024; the
current multi-MiB FA/WA shards already reach that requested depth. Negotiated
limits still depend on the target/controller. Load contexts count objects, not
their split NVMe commands; no target restart/change is needed for this update.

## DMA Buffer Budget

Request state, metadata scratch and bounce payloads have separate ownership:

| Resource Per Engine | Allocation |
| --- | --- |
| Request tokens | 256, independent of shard size |
| Metadata scratch | 64 sectors, for serialized/quiesced metadata operations |
| Index-record buffers | 64 buffers, one sector each |
| Bounce payloads | None in UCM direct-only mode |

The UCM wrapper selects direct-only mode before engine initialization. Cache
then registers its SHM before data submission. Data reads/writes use that SHM
address directly; an unregistered address fails rather than silently copying.
No full-shard internal payload buffers are allocated, including during startup.
At 512-byte sectors the two FA/WA engines together allocate 128 KiB of metadata
DMA payload instead of the old 2,364 MiB of full-shard buffers. Qpair, SPDK/RDMA
request state and allocator overhead are additional; 256 tokens do not guarantee
256 simultaneously outstanding commands if another queue/context limit applies.

Standalone engines default to bounce-capable mode. Their separate bounce pool
has up to 256 full-shard buffers within 64 MiB per store; exhaustion applies
backpressure without consuming a request token. Shards exceeding this budget
require direct-only mode and otherwise fail initialization with `-E2BIG`.
Pool allocation happens during lifecycle setup, not from reactor callbacks.

These are SPDK allocator/hugetlb resources, separate from the large registered
host Cache SHM. Ordinary available RAM and transparent-hugepage advice on Cache
SHM do not guarantee that the SPDK allocator can satisfy requests. Startup logs
report pool sizes and allocation stages, PID and region when initialization
fails. A generic `-12` alone cannot distinguish qpair, DMA pool and other
allocation failures. Do not increase disk capacity or host Cache to remedy it.

## Verification

Wrapper and partition-config tests run without NPU/NVMe devices. The engine
regression harness uses real SPDK rings/reactors and memory-backed mocked NVMe
operations, including multi-process recovery and lifecycle tests:

```bash
python -m unittest discover -s test/suites/Unit -p test_fawa_store_config.py -v
make -C testSPDK/ucm_spdk_store APP=engine_context_test SPDK_ROOT_DIR=/path/to/spdk
timeout --kill-after=2s 60s testSPDK/ucm_spdk_store/engine_context_test
```

These tests do not certify real NVMe-oF DMA, target behavior, or end-to-end V4
correctness/performance. Hardware testing must use explicitly disposable regions.
