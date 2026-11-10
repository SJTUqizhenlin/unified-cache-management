"""Pod-only: align UCM memfd NUMA placement with vllm-ascend's CPU pools.

UCM's ``NpuDevice.get_numa_node()`` reads the NPU PCI ``numa_node``. On some A2
servers a pair of NPUs shares one PCI NUMA node, while vllm-ascend splits the
NPU topo-affinity CPU list between the pair and therefore targets different
NUMA nodes for the two chips. When UCM places its memfd on the paired node,
vllm-ascend's later ``migratepages`` has to move multi-GiB cache segments.

This patch reuses vllm-ascend's own ``CpuAlloc`` mapping (CPU pool -> CPU NUMA
node) so UCM allocates on the exact node vllm-ascend would migrate to. It is
loaded by ``ucm_pod_numa.pth`` and only touches ``configure_cache_numa``.
"""

from ucm.integration.vllm.patch.utils import when_imported
from ucm.logger import init_logger

logger = init_logger(__name__)

_NODE_CACHE: dict[int, int | None] = {}


def vllm_ascend_numa_node(local_rank: int) -> int | None:
    """Return the NUMA node vllm-ascend uses for this worker/NPU."""
    if local_rank in _NODE_CACHE:
        return _NODE_CACHE[local_rank]

    node: int | None = None
    try:
        from vllm_ascend.cpu_binding import CpuAlloc

        binder = CpuAlloc(local_rank)
        # Ascend 950 builds pools differently and has thread-binding side
        # effects; keep the original UCM behavior there.
        if getattr(binder, "_is_ascend_950", lambda: False)():
            _NODE_CACHE[local_rank] = None
            return None

        binder.build_cpu_pools()
        npus = binder.device_info.running_npu_list
        if not 0 <= local_rank < len(npus):
            raise ValueError(f"local_rank {local_rank} outside visible NPUs {npus}")
        npu = npus[local_rank]
        cpus = binder.npu_cpu_pool.get(npu) or []
        if not cpus:
            raise ValueError(f"empty CPU pool for NPU {npu}")
        node = binder.cpu_node.get(cpus[0])
    except Exception as error:  # noqa: BLE001 - fall back to the original mapping
        logger.warning("[POD NUMA] vllm-ascend CPU-pool lookup failed: %s", error)
        node = None

    _NODE_CACHE[local_rank] = node
    return node


@when_imported("ucm.integration.vllm.cache_config")
def patch_cache_config(mod) -> None:
    if getattr(mod, "_ucm_pod_numa_align", False):
        return

    original = mod.configure_cache_numa

    def configure_cache_numa(config, device, device_id, parallel, local_rank):
        from ucm.store.cache.capabilities import uses_partitioned_cache

        if not uses_partitioned_cache():
            return original(config, device, device_id, parallel, local_rank)
        node = vllm_ascend_numa_node(local_rank)
        if node is not None:
            config["cache_detected_numa_node"] = node
            return
        return original(config, device, device_id, parallel, local_rank)

    configure_cache_numa._ucm_pod_numa_align = True
    mod.configure_cache_numa = configure_cache_numa
    mod._ucm_pod_numa_align = True
    logger.info(
        "[POD NUMA] UCM memfd placement aligned with vllm-ascend CPU pools"
    )
