"""FAWA config tests using source extraction, without vLLM, GPU or NVMe.

Run directly with python -m unittest discover -s test/suites/Unit
-p test_fawa_store_config.py to bypass the GPU-oriented pytest conftest.
"""

import ast
import copy
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Optional
from unittest.mock import Mock

CONNECTOR_PATH = (
    Path(__file__).resolve().parents[3] / "ucm/integration/vllm/hma_connector.py"
)


def _load_connector():
    tree = ast.parse(CONNECTOR_PATH.read_text())
    connector = next(
        node
        for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "UCMFAWAConnector"
    )
    connector.bases = []
    connector.body = [
        node
        for node in connector.body
        if getattr(node, "name", None)
        in {
            "_base_store_config",
            "_namespace_storage_backends",
            "_create_store",
            "_summarize_store_config",
        }
    ]
    namespace = {
        "copy": copy,
        "os": os,
        "Optional": Optional,
        "UcmKVStoreBaseV1": object,
        "KVConnectorRole": SimpleNamespace(WORKER="worker", SCHEDULER="scheduler"),
        "UcmConnectorFactoryV1": SimpleNamespace(
            create_connector=lambda name, config, module_path: config
        ),
        "logger": Mock(),
        "round_up": lambda value, alignment: (
            (value + alignment - 1) // alignment * alignment
        ),
    }
    exec(
        compile(
            ast.Module(body=[connector], type_ignores=[]), str(CONNECTOR_PATH), "exec"
        ),
        namespace,
    )
    return namespace["UCMFAWAConnector"]


Connector = _load_connector()


class FAWAStoreConfigTest(unittest.TestCase):
    def connector(self, config, role="scheduler", dp_rank=0):
        connector = Connector()
        connector.connector_configs = [
            {
                "ucm_connector_name": "UcmPipelineStore",
                "ucm_connector_module_path": "custom.module",
                "ucm_connector_config": config,
            }
        ]
        connector.unique_id = "service"
        connector.is_mla = True
        connector._role = role
        connector._vllm_config = SimpleNamespace(
            parallel_config=SimpleNamespace(data_parallel_rank=dp_rank)
        )
        connector._set_default_shm_buffer_capacity = Mock()
        connector.device_id = 0
        connector.tp_size = 2
        connector.file_size = {"FA": 4096, "WA": 8192}
        return connector

    def test_default_explicit_odd_and_minimum_totals(self):
        for settings, capacities, offsets in (
            ({}, (250, 250), (0, 252)),
            ({"spdk_capacity_gb": 500}, (250, 250), (0, 252)),
            ({"spdk_capacity_gb": 5, "spdk_offset_gib": 8}, (2, 3), (8, 12)),
            ({"spdk_capacity_gb": 2}, (1, 1), (0, 3)),
            ({"spdk_capacity_gb": "7", "spdk_offset_gib": "3"}, (3, 4), (3, 8)),
        ):
            with self.subTest(settings=settings):
                source = {
                    "store_pipeline": "Cache|Spdk",
                    "spdk_trid": "same-ns",
                    "spdk_format": 1,
                    **settings,
                }
                original = copy.deepcopy(source)
                connector = self.connector(source)
                stores = [
                    connector._base_store_config(suffix)[2] for suffix in ("fa", "wa")
                ]
                self.assertEqual(
                    tuple(s["spdk_capacity_gb"] for s in stores), capacities
                )
                self.assertEqual(tuple(s["spdk_offset_gib"] for s in stores), offsets)
                for suffix, store in zip(("fa", "wa"), stores):
                    self.assertEqual(store["unique_id"], f"service_fawa_{suffix}")
                    self.assertEqual(store["spdk_trid"], "same-ns")
                    self.assertEqual(store["spdk_format"], 1)
                self.assertEqual(source, original)
                self.assertEqual(len(connector.connector_configs), 1)

    def test_regions_cover_rounded_data_and_metadata_without_overlap(self):
        for total in (2, 3, 5, 500, 501):
            connector = self.connector(
                {
                    "store_pipeline": "Cache|Spdk",
                    "spdk_capacity_gb": total,
                    "spdk_offset_gib": 9,
                }
            )
            fa, wa = [connector._base_store_config(s)[2] for s in ("fa", "wa")]
            gib = 1 << 30
            for sector in (512, 4096):
                for block in (4096, 1003520, 8196096):
                    fa_end = (
                        fa["spdk_offset_gib"] * gib
                        + sector
                        + gib
                        + (fa["spdk_capacity_gb"] * gib // block) * block
                    )
                    self.assertLessEqual(fa_end, wa["spdk_offset_gib"] * gib)
                    wa_end = (
                        wa["spdk_offset_gib"] * gib
                        + sector
                        + gib
                        + (wa["spdk_capacity_gb"] * gib // block) * block
                    )
                    self.assertLessEqual(wa_end, (9 + total + 4) * gib)
            self.assertEqual(fa["spdk_capacity_gb"] + wa["spdk_capacity_gb"], total)

    def test_invalid_spdk_settings_fail_for_both_stores(self):
        for key, invalid in (
            ("spdk_capacity_gb", (0, 1, -1, "1", None, True, 2.5, "2.5", "bad")),
            ("spdk_offset_gib", (-1, "-1", None, False, 0.5, "0.5", "bad")),
        ):
            for value in invalid:
                for suffix in ("fa", "wa"):
                    with self.subTest(key=key, value=value, suffix=suffix):
                        connector = self.connector(
                            {"store_pipeline": "Cache|Spdk", key: value}
                        )
                        with self.assertRaisesRegex(ValueError, key):
                            connector._base_store_config(suffix)

    def test_worker_scheduler_geometry_is_stable(self):
        source = {
            "store_pipeline": "Cache|Spdk",
            "spdk_capacity_gb": 11,
            "spdk_offset_gib": 17,
        }
        for dp_rank in (0, 1):
            scheduler = self.connector(source, dp_rank=dp_rank)
            worker = self.connector(source, role="worker", dp_rank=dp_rank)
            for label, suffix in (("FA", "fa"), ("WA", "wa")):
                scheduled = scheduler._create_store(label, suffix, None)
                worked = worker._create_store(label, suffix, [4096, 8192])
                self.assertNotEqual(scheduled["block_size"], worked["block_size"])
                for key in ("spdk_offset_gib", "spdk_capacity_gb", "unique_id"):
                    self.assertEqual(scheduled[key], worked[key])
                self.assertEqual(worked["local_rank_size"], 2)
                self.assertEqual(worked["shard_size"], 12288)
                self.assertEqual(worked["cache_io_aggregation"], label == "FA")

    def test_posix_paths_capacity_and_gc_are_unchanged(self):
        with tempfile.TemporaryDirectory() as root:
            backends = [str(Path(root) / "a"), str(Path(root) / "b")]
            for backend_config in (backends, ":".join(backends)):
                source = {
                    "store_pipeline": "Cache|Posix",
                    "storage_backends": backend_config,
                    "posix_capacity_gb": 11,
                    "spdk_capacity_gb": 1,
                    "spdk_offset_gib": -1,
                }
                original = copy.deepcopy(source)
                for role, dp_rank in (
                    ("worker", 0),
                    ("scheduler", 0),
                    ("scheduler", 1),
                ):
                    connector = self.connector(source, role, dp_rank)
                    for suffix in ("fa", "wa"):
                        name, module, config = connector._base_store_config(suffix)
                        self.assertEqual(name, "UcmPipelineStore")
                        self.assertEqual(module, "custom.module")
                        self.assertEqual(
                            config["storage_backends"],
                            [str(Path(b) / f"fawa_{suffix}") for b in backends],
                        )
                        self.assertTrue(
                            all(Path(b).is_dir() for b in config["storage_backends"])
                        )
                        self.assertEqual(config["posix_capacity_gb"], 5)
                        self.assertEqual(
                            config["posix_gc_enable"],
                            role == "scheduler" and dp_rank == 0,
                        )
                        self.assertEqual(config["spdk_capacity_gb"], 1)
                        self.assertEqual(config["spdk_offset_gib"], -1)
                self.assertEqual(source, original)

    def test_other_pipelines_and_default_are_not_split(self):
        for pipeline in (None, "Cache|Empty", "Posix", "Cache|SPDK"):
            source = {"spdk_capacity_gb": 1, "spdk_offset_gib": -1}
            if pipeline is not None:
                source["store_pipeline"] = pipeline
            config = self.connector(source)._base_store_config("fa")[2]
            self.assertEqual(config["store_pipeline"], pipeline or "Cache|Empty")
            self.assertEqual(config["spdk_capacity_gb"], 1)
            self.assertEqual(config["spdk_offset_gib"], -1)

    def test_exactly_one_config_required(self):
        connector = self.connector({})
        for entries in ([], connector.connector_configs * 2):
            connector.connector_configs = entries
            with self.assertRaisesRegex(RuntimeError, "exactly one connector config"):
                connector._base_store_config("fa")


if __name__ == "__main__":
    unittest.main()
