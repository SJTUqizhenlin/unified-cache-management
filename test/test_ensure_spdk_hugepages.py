import copy
import importlib.util
import io
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import yaml


spec = importlib.util.spec_from_file_location(
    "ensure_spdk_hugepages", Path(__file__).parents[1] / "scripts/ensure_spdk_hugepages.py"
)
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)

CONFIG = {"ucm_connectors": [{"ucm_connector_config": {
    "store_pipeline": "Cache|Spdk", "spdk_trid": "ipc:/tmp/cache.sock|trtype:PCIe",
    "cache_buffer_capacity_gb": 40, "share_buffer_enable": True,
    "shm_use_hugepage_file": True, "shm_hugepage_dir": "/dev/hugepages",
}}]}
MOUNT = "41 20 0:40 / /dev/hugepages rw,relatime - hugetlbfs nodev rw,pagesize=2M\n"


class HugepagePreflightTest(unittest.TestCase):
    def setUp(self):
        self.config = copy.deepcopy(CONFIG)
        self.before = (25600, 0, 25600)
        self.after = None
        self.expanded = False
        self.mount = MOUNT
        self.meminfo = "Hugepagesize:       2048 kB\n"
        self.access = True
        self.output = io.StringIO()
        patches = [
            patch.object(Path, "read_text", autospec=True, side_effect=self.read),
            patch.object(Path, "resolve", autospec=True, side_effect=lambda p, **kw: p),
            patch.object(Path, "is_dir", return_value=True),
            patch.object(Path, "is_file", return_value=True),
            patch.object(preflight.os, "access", side_effect=lambda *a: self.access),
            patch.object(preflight.os, "getuid", return_value=1000),
            patch.object(preflight.os, "geteuid", return_value=1000),
            patch.dict(preflight.os.environ, {}, clear=True),
            patch.object(preflight.subprocess, "run", side_effect=self.allocate),
            patch("sys.stdout", self.output), patch("sys.stderr", self.output),
        ]
        self.mocks = [p.start() for p in patches]
        for p in patches:
            self.addCleanup(p.stop)
        self.run = self.mocks[8]

    def read(self, path):
        if path == Path("/config.yaml"):
            return yaml.safe_dump(self.config)
        if path == Path("/proc/self/mountinfo"):
            return self.mount
        if path == Path("/proc/meminfo"):
            return self.meminfo
        counts = self.after if self.expanded and self.after is not None else self.before
        return str(dict(zip(("free_hugepages", "resv_hugepages", "nr_hugepages"), counts))[path.name])

    def allocate(self, command, **kwargs):
        self.expanded = True
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, *extra):
        return preflight.main(["--config", "/config.yaml", "--devices", "0,1,2,3,4,5,6,7",
                               "--setup-script", "/spdk/scripts/setup.sh", *extra])

    def test_sufficient_shared_v4_total_does_not_expand(self):
        self.assertEqual(self.invoke("--cache-multiplier", "1"), 0)
        self.run.assert_not_called()
        self.assertIn("required=25600", self.output.getvalue())
        self.assertIn("cache=40960 MiB", self.output.getvalue())

    def test_default_conservatively_counts_devices(self):
        self.before = (168960, 0, 168960)
        self.assertEqual(self.invoke(), 0)
        self.assertIn("multiplier=8", self.output.getvalue())
        self.run.assert_not_called()

    def test_expansion_preserves_used_pages_and_never_binds_pci(self):
        self.before = (10000, 2000, 30000)
        self.after = (27600, 2000, 47600)
        with patch.dict(preflight.os.environ, {"HUGENODE": "0", "SKIP_HUGE": "yes",
                                              "NVME_ALLOWED": "0000:01:00.0"}):
            self.assertEqual(self.invoke("--cache-multiplier", "1"), 0)
        command = self.run.call_args.args[0]
        self.assertEqual(command[:3], ["sudo", "-n", "env"])
        self.assertEqual(command[-2:], ["/spdk/scripts/setup.sh", "config"])
        for setting in ("PCI_ALLOWED=none", "DEV_TYPE=NVME", "HUGEPGSZ=2048",
                        "NRHUGE=47600", "SHRINK_HUGE=no", "CLEAR_HUGE=no",
                        "PERSIST_HUGE=no", "SKIP_PCI=1", "DRIVER_OVERRIDE=none"):
            self.assertIn(setting, command)
        for name in ("HUGENODE", "SKIP_HUGE", "NVME_ALLOWED", "TARGET_USER"):
            self.assertEqual(command[command.index(name) - 1], "-u")

    def test_reserved_pages_are_not_available(self):
        self.before = (25600, 1, 26000)
        self.after = (25601, 1, 26001)
        self.assertEqual(self.invoke("--cache-multiplier", "1"), 0)
        self.assertIn("NRHUGE=26001", self.run.call_args.args[0])

    def test_partial_allocation_and_changed_reservations_fail(self):
        for after in ((25599, 0, 25600), (25600, 100, 25600)):
            with self.subTest(after=after):
                self.expanded = False
                self.before = (10000, 0, 10000)
                self.after = after
                self.assertEqual(self.invoke("--cache-multiplier", "1"), 1)
                self.assertIn("failed/incomplete", self.output.getvalue())

    def test_setup_nonzero_fails_even_if_pages_are_sufficient(self):
        self.before = (0, 0, 0)
        def fail(command, **kwargs):
            self.before = (25600, 0, 25600)
            return subprocess.CompletedProcess(command, 1)
        self.run.side_effect = fail
        self.assertEqual(self.invoke("--cache-multiplier", "1"), 1)
        self.assertIn("setup exit=1", self.output.getvalue())

    def test_sudo_execution_error_is_actionable(self):
        self.before = (0, 0, 0)
        self.run.side_effect = FileNotFoundError("sudo not found")
        self.assertEqual(self.invoke(), 1)
        self.assertIn("sudo not found", self.output.getvalue())

    def test_configuration_mismatches_fail_before_allocation(self):
        for key, value in (("share_buffer_enable", False), ("share_buffer_enable", "true"),
                           ("shm_use_hugepage_file", False), ("cache_buffer_capacity_gb", 0),
                           ("cache_buffer_capacity_gb", True), ("shm_hugepage_dir", "relative"),
                           ("store_pipeline", "Spdk")):
            with self.subTest(key=key, value=value):
                self.config = copy.deepcopy(CONFIG)
                self.config["ucm_connectors"][0]["ucm_connector_config"][key] = value
                self.assertEqual(self.invoke(), 1)
                self.run.assert_not_called()

    def test_wrong_mount_type_size_or_readonly_fail(self):
        for mount in ("", MOUNT.replace("hugetlbfs", "tmpfs"),
                      MOUNT.replace("pagesize=2M", "pagesize=1G"),
                      MOUNT.replace("rw,relatime", "ro,relatime"),
                      MOUNT + "42 41 0:41 / /dev/hugepages rw - tmpfs nodev rw\n"):
            with self.subTest(mount=mount):
                self.mount = mount
                self.assertEqual(self.invoke(), 1)
                self.run.assert_not_called()

    def test_mount_default_page_size(self):
        self.mount = MOUNT.replace(",pagesize=2M", "")
        self.assertEqual(self.invoke("--cache-multiplier", "1"), 0)

    def test_expansion_refuses_wrong_kernel_default_page_pool(self):
        self.before = (0, 0, 0)
        self.meminfo = "Hugepagesize:       1048576 kB\n"
        self.assertEqual(self.invoke(), 1)
        self.assertIn("kernel default Hugepagesize=2048", self.output.getvalue())
        self.run.assert_not_called()

    def test_missing_explicit_hugepage_flag_is_rejected(self):
        del self.config["ucm_connectors"][0]["ucm_connector_config"]["shm_use_hugepage_file"]
        self.assertEqual(self.invoke(), 1)
        self.run.assert_not_called()

    def test_malformed_config_is_rejected(self):
        for config in (None, {}, {"ucm_connectors": [None]}):
            with self.subTest(config=config):
                self.config = config
                self.assertEqual(self.invoke(), 1)
                self.run.assert_not_called()

    def test_original_user_must_have_write_and_search_access(self):
        self.access = False
        self.assertEqual(self.invoke(), 1)
        self.assertIn("original server user", self.output.getvalue())
        self.run.assert_not_called()

    def test_running_under_sudo_is_rejected(self):
        with patch.dict(preflight.os.environ, {"SUDO_UID": "2000"}):
            self.assertEqual(self.invoke(), 1)
        self.run.assert_not_called()

    def test_multiple_connectors_sum_total_capacity(self):
        self.config["ucm_connectors"] *= 2
        self.before = (46080, 0, 46080)
        self.assertEqual(self.invoke("--cache-multiplier", "1"), 0)
        self.assertIn("cache=81920 MiB", self.output.getvalue())
        self.run.assert_not_called()

    def test_non_ipc_connectors_are_noop(self):
        self.config["ucm_connectors"][0]["ucm_connector_config"] = {
            "store_pipeline": "Cache|Spdk", "spdk_trid": "trtype:RDMA"}
        self.assertEqual(self.invoke(), 0)
        self.run.assert_not_called()

    def test_empty_hugepage_cache_expands_without_target_budget(self):
        self.config["ucm_connectors"][0]["ucm_connector_config"]["store_pipeline"] = "Cache|Empty"
        self.before = (5121, 0, 46079)
        self.after = (21504, 0, 62462)
        self.assertEqual(self.invoke("--cache-multiplier", "1", "--target-mem-mb", "0"), 0)
        self.assertIn("required=21504", self.output.getvalue())
        self.assertIn("NRHUGE=62462", self.run.call_args.args[0])

    def test_rounding_and_custom_target_reserve(self):
        self.before = (20482, 0, 20482)
        self.assertEqual(self.invoke("--cache-multiplier", "1", "--target-mem-mb", "3",
                                     "--reserve-mb", "0"), 0)
        self.assertIn("required=20482", self.output.getvalue())

    def test_invalid_cli_values(self):
        for extra in (("--devices", "0,0"), ("--devices", "0,"),
                      ("--target-mem-mb", "-1"), ("--reserve-mb", "-1"),
                      ("--cache-multiplier", "0")):
            with self.subTest(extra=extra), self.assertRaises(SystemExit) as exc:
                self.invoke(*extra)
            self.assertEqual(exc.exception.code, 2)
        self.run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
