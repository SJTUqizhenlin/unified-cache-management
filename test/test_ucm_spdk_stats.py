"""Statistics parser/RPC tests. No target, engine, model or NVMe access."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "ucm_spdk_stats", Path(__file__).resolve().parents[1] / "scripts/ucm_spdk_stats.py"
)
stats = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(Path(__file__).resolve().parents[1] / "scripts"), *sys.path]):
    spec.loader.exec_module(stats)


class SpdkStatsTest(unittest.TestCase):
    def config(self):
        return {"store_pipeline": "Cache|Spdk", "spdk_trid":
                "trtype:RDMA traddr:192.0.2.1 trsvcid:4420 subnqn:nqn.test"}

    def sample(self, amount=1, ticks=100):
        return {"tick_rate": 100, "ticks": ticks, "connections": 4, "qpairs": 8,
                "bdev": {"name": "Nvme0n1", "bytes_read": 1024 * amount,
                         "bytes_written": 2048 * amount, "num_read_ops": amount,
                         "num_write_ops": amount * 2, "read_latency_ticks": amount,
                         "write_latency_ticks": amount * 2, "io_error": {}}}

    def test_backend_detection(self):
        self.assertEqual(stats.backend(self.config()), "spdk")
        self.assertEqual(stats.backend({"store_pipeline": "Cache|Posix"}), "posix")
        with self.assertRaises(ValueError):
            stats.backend({"store_pipeline": "Fake"})

    def test_posix_engine_from_configuration_not_traffic(self):
        config = {"store_pipeline": "Cache|Posix"}
        self.assertEqual(stats.posix_engine(config), "psync")
        self.assertEqual(stats.posix_engine(config | {"posix_io_engine": "aio"}), "aio")
        with self.assertRaises(ValueError):
            stats.posix_engine(config | {"posix_io_engine": "invalid"})
        with self.assertRaises(ValueError):
            stats.posix_engine(self.config())

    def test_local_target_resolved_from_config(self):
        subsystem = {"subtype": "NVMe", "nqn": "nqn.test", "listen_addresses": [
            {"trtype": "RDMA", "traddr": "192.0.2.1", "trsvcid": "4420"}],
            "namespaces": [{"bdev_name": "Nvme0n1"}]}
        addresses = json.dumps([{"addr_info": [{"local": "192.0.2.1"}]}])
        with patch.object(stats.subprocess, "check_output", return_value=addresses), \
                patch.object(stats, "rpc", return_value=[subsystem]), \
                patch.dict(stats.os.environ, {"SPDK_BDEV": ""}):
            self.assertEqual(stats.resolve_target(self.config(), "127.0.0.1"),
                             ("nqn.test", "Nvme0n1"))
            subsystem["namespaces"].append({"bdev_name": "Other"})
            with self.assertRaisesRegex(ValueError, "multiple namespaces"):
                stats.resolve_target(self.config(), "127.0.0.1")

    def test_remote_and_pcie_do_not_use_local_rpc(self):
        with patch.object(stats, "rpc") as rpc:
            with self.assertRaisesRegex(ValueError, "remote vLLM"):
                stats.resolve_target(self.config(), "192.0.2.2")
            config = self.config() | {"spdk_trid": "trtype:PCIe traddr:0000:84:00.0"}
            with self.assertRaisesRegex(ValueError, "direct PCIe"):
                stats.resolve_target(config, "127.0.0.1")
            with patch.object(stats.subprocess, "check_output", return_value="[]"):
                with self.assertRaisesRegex(ValueError, "not a local target"):
                    stats.resolve_target(self.config(), "127.0.0.1")
            rpc.assert_not_called()

    def test_completed_io_delta_and_latency(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            stats.report(self.sample(3, 300), self.sample())
        self.assertIn("delta 2.00s", out.getvalue())
        self.assertIn("2,048 B", out.getvalue())
        self.assertIn("1.0 IOPS", out.getvalue())
        self.assertIn("10000.0 us", out.getvalue())

    def test_counter_reset_does_not_report_negative_io(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            stats.report(self.sample(1, 400), self.sample(3, 300))
        self.assertIn("new baseline", out.getvalue())
        self.assertNotIn("IOPS", out.getvalue())

    def test_rpc_is_bounded_noninteractive_and_read_only(self):
        with patch.dict(stats.os.environ, {"SPDK_RPC": "/fake/rpc.py",
                                         "SPDK_RPC_SUDO": "1"}), \
                patch.object(stats.subprocess, "run", return_value=
                             subprocess.CompletedProcess([], 0, '{"bdevs": []}', "")) as run:
            stats.rpc("bdev_get_iostat", "--names", "Nvme0n1")
        command = run.call_args.args[0]
        self.assertEqual(command[:3], ["sudo", "-n", "/fake/rpc.py"])
        self.assertNotIn("--reset-mode", command)
        self.assertEqual(run.call_args.kwargs["timeout"], 10)

    def test_missing_metrics_are_na_not_zero(self):
        response = io.BytesIO(b'ucm:cache_load_bytes_total{worker_rank="0"} 4096\n')
        out = io.StringIO()
        with patch.object(stats.urllib.request, "urlopen", return_value=response), \
                contextlib.redirect_stdout(out):
            stats.cache_metrics("127.0.0.1", "7780")
        self.assertIn("cache_load_bytes_total: 4,096", out.getvalue())
        self.assertIn("cache_dump_bytes_total: N/A", out.getvalue())
        self.assertIn("FAWA load barrier", out.getvalue())
        self.assertIn("start_load_kv total [all calls, including no-load]", out.getvalue())
        self.assertIn("avg=N/A p99=N/A", out.getvalue())

    def test_snapshot_repeats_cumulative_reports_without_final_sleep(self):
        for extra, count in [([], 1), (["--cycles", "3", "--interval", "5"], 3)]:
            with self.subTest(extra=extra), \
                    patch.object(sys, "argv", ["stats", "--config", "fake", *extra]), \
                    patch.object(stats, "load_config", return_value=self.config()), \
                    patch.object(stats, "resolve_target", return_value=("nqn.test", "Nvme0n1")), \
                    patch.object(stats, "sample", return_value=self.sample()) as sample, \
                    patch.object(stats, "report") as report, \
                    patch.object(stats, "cache_metrics") as metrics, \
                    patch.object(stats.time, "sleep") as sleep, \
                    contextlib.redirect_stdout(io.StringIO()):
                stats.main()
                self.assertEqual(sample.call_count, count)
                self.assertEqual(report.call_count, count)
                self.assertEqual(metrics.call_count, count)
                self.assertEqual(sleep.call_count, count - 1)
                for call in report.call_args_list:
                    self.assertTrue(len(call.args) == 1 or call.args[1] is None)


if __name__ == "__main__":
    unittest.main()
