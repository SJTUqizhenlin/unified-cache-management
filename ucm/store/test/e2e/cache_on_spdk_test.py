# -*- coding: utf-8 -*-
#
# MIT License
#
# Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
"""Cache|Spdk end-to-end test.

Mirrors cache_on_empty_test.py with two additions that pin the deployment
contract of the SPDK store:

  * the WORKER config carries the model geometry (tensor_size/shard_size/
    block_size) exactly as the vLLM connector injects it at runtime
    (ucm_connector.py: tensor_size_list/shard_size/block_size are computed
    from the model's kv_cache_layout, never written in the yaml);
  * the SCHEDULER config has NO geometry and NO device_id — the same keys
    the yaml holds — and must still serve lookups: the Cache stage exempts
    deviceId == -1 from size checks and the SPDK store attaches the
    region-scoped shared index in lookup-only mode without probing the
    device.

A geometry change on the region is a model switch: existing blocks are
invalidated in place (run 2 uses a different shard size on purpose).

Needs root (raw NVMe + /dev/shm segments). CPU tensors + the simu
transport are used when no accelerator is visible, so the test also runs
on a plain build host.
"""
import os
import secrets

import torch

from ucm.store.pipeline.connector import UcmPipelineStore

SPDK_TRID = os.environ.get(
    "UCM_SPDK_TRID", "trtype:PCIe traddr:0000:84:00.0"
)


def pick_device() -> str:
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


def cmp_and_print_diff(a, b, rtol=0.0, atol=0.0):
    for r, (row_a, row_b) in enumerate(zip(a, b)):
        for c, (ta, tb) in enumerate(zip(row_a, row_b)):
            if not torch.allclose(ta, tb, rtol=rtol, atol=atol):
                mask = ~torch.isclose(ta, tb, rtol=rtol, atol=atol)
                diff_a = ta[mask].cpu()
                diff_b = tb[mask].cpu()
                print(f"DIFF at [{r}][{c}]  total {mask.sum().item()} element(s)")
                print("  a val:", diff_a.flatten())
                print("  b val:", diff_b.flatten())
                assert False


def e2e_test(worker, scheduler, tensor_size, layer_size, chunk_size,
             request_size, device):
    chunk_block_ids = [secrets.token_bytes(16) for _ in range(request_size)]
    founds = scheduler.lookup(chunk_block_ids)
    assert not any(founds), "phantom hit before dump"
    assert scheduler.lookup_on_prefix(chunk_block_ids) == -1
    shard_indexes = [0 for _ in range(request_size)]
    src_tensors = [
        [
            torch.rand([tensor_size // 2], dtype=torch.bfloat16, device=device)
            for _ in range(layer_size * chunk_size)
        ]
        for _ in range(request_size)
    ]
    task = worker.dump(chunk_block_ids, shard_indexes, src_tensors)
    worker.wait(task)
    founds = scheduler.lookup(chunk_block_ids)
    assert all(founds), "block invisible to scheduler after dump"
    assert scheduler.lookup_on_prefix(chunk_block_ids) + 1 == request_size
    dst_tensors = [[torch.empty_like(t) for t in row] for row in src_tensors]
    task = worker.load(chunk_block_ids, shard_indexes, dst_tensors)
    worker.wait(task)
    cmp_and_print_diff(src_tensors, dst_tensors)
    print(f"[{request_size} blocks x {layer_size}x{chunk_size} tensors] dump+load OK")


def run_once(tag, tensor_size, layer_size, chunk_size, request_size):
    device = pick_device()
    chunk_block_size = tensor_size * layer_size * chunk_size
    # What the yaml holds: pipeline, backend knobs, trid. No geometry.
    yaml_like = {
        "store_pipeline": "Cache|Spdk",
        "unique_id": f"e2e-spdk-{tag}",
        "timeout_ms": 60000,
        "share_buffer_enable": True,
        "cache_buffer_capacity_gb": 1,
        "waiting_queue_depth": 16,
        "running_queue_depth": 1024,
        "shm_hugepage_advise": True,
        "spdk_trid": SPDK_TRID,
        "spdk_capacity_gb": 1,
    }
    # What the vLLM connector injects on the worker at runtime.
    worker_cfg = yaml_like | {
        "device_id": 0,
        "tensor_size": tensor_size,
        "shard_size": chunk_block_size,
        "block_size": chunk_block_size,
    }
    # The scheduler gets the yaml keys only (no device_id, no geometry).
    scheduler_cfg = dict(yaml_like)

    worker = UcmPipelineStore(worker_cfg)
    scheduler = UcmPipelineStore(scheduler_cfg)
    e2e_test(worker, scheduler, tensor_size, layer_size, chunk_size,
             request_size, device)


def main():
    # run 1: one geometry; run 2: a different shard size on the same
    # region — the model switch must invalidate run 1 and still serve.
    run_once("r1", tensor_size=32768, layer_size=4, chunk_size=2, request_size=4)
    run_once("r2", tensor_size=16384, layer_size=8, chunk_size=1, request_size=4)
    print("cache_on_spdk_test: PASS")


if __name__ == "__main__":
    os.environ.setdefault("UC_LOGGER_LEVEL", "info")
    main()
