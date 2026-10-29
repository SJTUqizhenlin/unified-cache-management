"""CPU-only FAWA barrier tests using AST extraction, without vLLM/NPU imports.

Run with python -m unittest discover -s test/suites/Unit
-p test_fawa_load_wait_metrics.py to bypass the GPU-oriented pytest conftest.
"""

import __future__
import ast
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, call

import yaml

REPO_ROOT = Path(__file__).resolve().parents[3]
CONNECTOR_PATH = REPO_ROOT / "ucm/integration/vllm/hma_connector.py"
METRIC = "fawa_load_wait_duration_ms"


def _load_connector(clock, metrics):
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
        if getattr(node, "name", None) in {"start_load_kv", "_wait_all_load_task", "_wait_load_task"}
    ]
    namespace = {
        "FAWALoadTask": SimpleNamespace,
        "UCMFAWAConnectorMetadata": SimpleNamespace,
        "time": SimpleNamespace(perf_counter=clock),
        "ucmmetrics": metrics,
        "logger": Mock(),
    }
    exec(
        compile(
            ast.Module(body=[connector], type_ignores=[]), str(CONNECTOR_PATH), "exec"
        ),
        namespace,
    )
    return namespace["UCMFAWAConnector"]()


class FAWALoadWaitMetricsTest(unittest.TestCase):
    def setUp(self):
        self.clock = Mock(side_effect=[10.0, 10.125])
        self.metrics = Mock()
        self.connector = _load_connector(self.clock, self.metrics)
        self.connector.file_size = {"FA": 4096, "WA": 8192}
        self.connector._rank_consistency = Mock()
        self.connector._handle_load_err = Mock()
        self.tasks = [
            SimpleNamespace(request_id="r1", label="FA", key_count=3, task="fa"),
            SimpleNamespace(request_id="r1", label="WA", key_count=1, task="wa"),
        ]

    def test_empty_has_no_clock_wait_or_metrics(self):
        self.assertIsNone(self.connector._wait_all_load_task([]))
        self.clock.assert_not_called()
        self.connector._rank_consistency.wait_load.assert_not_called()
        self.metrics.update_stats.assert_not_called()

    def prepare_entry(self, keys):
        request = SimpleNamespace(load_keys=keys, load_hash_start=0, load_hash_end=len(keys),
                                  load_vllm_block_ids=[])
        self.connector._get_connector_metadata = Mock(
            return_value=SimpleNamespace(request_meta={"r1": request}))
        self.connector.fa_store = object()
        self.connector.wa_store = object()
        self.connector._extract_fa_ptr = Mock(return_value=[])
        self.connector._extract_wa_ptr = Mock(return_value=[])
        self.connector._submit_load_task = Mock(side_effect=self.tasks)

    def test_nonempty_entry_includes_preparation_and_barrier(self):
        self.prepare_entry([b"key"])
        self.clock.side_effect = [10.0, 10.25, 10.375, 10.5]
        self.connector.start_load_kv(None)
        self.assertEqual(self.metrics.update_stats.call_args_list, [
            call({"load_bytes_total": 20480}), call({METRIC: 125.0}),
            call({"fawa_start_load_kv_duration_ms": 500.0})])

    def test_empty_entry_emits_no_conditional_sample(self):
        self.prepare_entry([])
        self.connector.start_load_kv(None)
        self.connector._submit_load_task.assert_not_called()
        self.metrics.update_stats.assert_not_called()

    def test_nonempty_entry_wait_exception_records_both_metrics(self):
        self.prepare_entry([b"key"])
        self.clock.side_effect = [10.0, 10.25, 10.375, 10.5]
        self.connector._rank_consistency.wait_load.side_effect = RuntimeError("failed")
        self.connector._handle_load_err.side_effect = ValueError("handler failed")
        with self.assertRaises(ValueError):
            self.connector.start_load_kv(None)
        self.assertEqual(self.metrics.update_stats.call_args_list, [
            call({METRIC: 125.0}), call({"fawa_start_load_kv_duration_ms": 500.0})])

    def test_success_times_entire_barrier_and_preserves_bytes(self):
        events = []

        def clock():
            events.append("clock")
            return 10.0 if len(events) == 1 else 10.125

        self.clock.side_effect = clock
        self.connector._rank_consistency.wait_load.side_effect = (
            lambda task: events.append(task)
        )
        self.metrics.update_stats.side_effect = lambda stats: events.append(stats)
        self.assertIsNone(self.connector._wait_all_load_task(self.tasks))
        self.assertEqual(
            events,
            [
                "clock",
                "fa",
                "wa",
                {"load_bytes_total": 20480},
                "clock",
                {METRIC: 125.0},
            ],
        )
        self.connector._handle_load_err.assert_not_called()

    def test_handled_failure_continues_and_counts_only_successful_bytes(self):
        for outcomes, expected_bytes in (
            ([RuntimeError("FA failed"), None], 8192),
            ([None, RuntimeError("WA failed")], 12288),
            ([RuntimeError("FA failed"), RuntimeError("WA failed")], 0),
        ):
            with self.subTest(expected_bytes=expected_bytes):
                self.clock.side_effect = [20.0, 22.5]
                self.metrics.reset_mock()
                self.connector._handle_load_err.reset_mock()
                waiter = self.connector._rank_consistency.wait_load
                waiter.reset_mock()
                waiter.side_effect = outcomes
                self.connector._wait_all_load_task(self.tasks)
                self.assertEqual(waiter.call_args_list, [call("fa"), call("wa")])
                self.assertEqual(
                    self.connector._handle_load_err.call_args_list,
                    [call("r1") for outcome in outcomes if outcome is not None],
                )
                self.assertEqual(
                    self.metrics.update_stats.call_args_list,
                    [call({"load_bytes_total": expected_bytes}), call({METRIC: 2500.0})],
                )

    def test_error_handler_exception_propagates_with_duration_but_no_bytes(self):
        self.connector._rank_consistency.wait_load.side_effect = [
            None,
            RuntimeError("WA failed"),
        ]
        error = ValueError("failure reporting failed")
        self.connector._handle_load_err.side_effect = error
        with self.assertRaises(ValueError) as raised:
            self.connector._wait_all_load_task(self.tasks)
        self.assertIs(raised.exception, error)
        self.metrics.update_stats.assert_called_once_with({METRIC: 125.0})
        self.assertEqual(self.clock.call_count, 2)

    def test_byte_accounting_exception_still_reports_duration(self):
        del self.connector.file_size["FA"]
        with self.assertRaises(KeyError):
            self.connector._wait_all_load_task(self.tasks)
        self.connector._rank_consistency.wait_load.assert_called_once_with("fa")
        self.metrics.update_stats.assert_called_once_with({METRIC: 125.0})

    def test_byte_metric_update_exception_still_reports_duration(self):
        error = RuntimeError("byte metric update failed")
        self.metrics.update_stats.side_effect = [error, None]
        with self.assertRaises(RuntimeError) as raised:
            self.connector._wait_all_load_task(self.tasks)
        self.assertIs(raised.exception, error)
        self.assertEqual(
            self.metrics.update_stats.call_args_list,
            [call({"load_bytes_total": 20480}), call({METRIC: 125.0})],
        )

    def test_histogram_default_matches_example_enablelist(self):
        config_path = REPO_ROOT / "ucm/default_metrics_config.py"
        namespace = {}
        # Postpone project Python 3.10+ annotations on older CPU-only test hosts.
        exec(
            compile(
                config_path.read_text(),
                str(config_path),
                "exec",
                flags=__future__.annotations.compiler_flag,
            ),
            namespace,
        )
        defaults = namespace["get_default_metrics_config"]()
        example = yaml.safe_load(
            (REPO_ROOT / "examples/metrics/metrics_configs.yaml").read_text()
        )
        definitions = []
        for config in (defaults, example):
            matches = [item for item in config["histogram"] if item["name"] == METRIC]
            self.assertEqual(len(matches), 1)
            definitions.append(matches[0])
        self.assertEqual(definitions[0], definitions[1])
        buckets = definitions[0]["buckets"]
        self.assertEqual(buckets, sorted(set(buckets)))
        self.assertLessEqual(buckets[0], 0.1)
        self.assertTrue({1000, 2000, 5000, 10000}.issubset(buckets))
        documentation = definitions[0]["documentation"]
        for phrase in ("per-worker", "per-forward", "residual", "including failures"):
            self.assertIn(phrase, documentation)
        self.assertIn("not task submit latency", documentation)
        name = "fawa_start_load_kv_duration_ms"
        self.assertEqual(
            [x for x in defaults["histogram"] if x["name"] == name],
            [x for x in example["histogram"] if x["name"] == name])
        self.assertEqual(len([x for x in defaults["histogram"] if x["name"] == name]), 1)


if __name__ == "__main__":
    unittest.main()
