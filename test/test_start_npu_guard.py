import importlib.util
from pathlib import Path
import signal
import unittest
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location(
    "start_npu_guard", Path(__file__).parents[1] / "scripts/start_npu_guard.py"
)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)

TABLE = """
| NPU Name | Health | Memory |
| 0 910B3 | OK | 3400 / 65536 |
| NPU Chip | Process id | Process name | Process memory(MB) |
| 0 0 | 101 | worker | 20000 |
| 1 0 | 202 | worker | 20000 |
| No running processes found in NPU 2 |
"""


class GuardTest(unittest.TestCase):
    def test_parse_selected_devices_only(self):
        self.assertEqual(guard.parse_processes(TABLE, None), {101, 202})
        self.assertEqual(guard.parse_processes(TABLE, {1}), {202})
        self.assertEqual(guard.parse_processes(TABLE, {2}), set())
        with self.assertRaises(RuntimeError):
            guard.parse_processes("invalid output", {0})

    def test_server_defaults_and_environment_override(self):
        source = '# DEVICES="${DEVICES:-7}"\nDEVICES="${DEVICES:-2,3}"\nPORT="${PORT:-7880}"\n'
        with patch.object(guard.Path, "read_text", return_value=source), \
                patch.dict(guard.os.environ, {}, clear=True):
            self.assertEqual(guard.server_config(["bash", "server.sh", "--ucm", "off"]),
                             {"DEVICES": "2,3", "PORT": "7880"})
            with patch.dict(guard.os.environ, {"DEVICES": "4,5", "PORT": "8000"}):
                self.assertEqual(guard.server_config(["bash", "server.sh"]),
                                 {"DEVICES": "4,5", "PORT": "8000"})

    def test_unknown_script_configuration_fails_closed(self):
        with patch.object(guard.Path, "read_text", return_value="echo unsafe"), \
                patch.dict(guard.os.environ, {}, clear=True):
            with self.assertRaises(RuntimeError):
                guard.server_config(["bash", "server.sh"])

    def test_dry_run_ignores_device_environment(self):
        with patch.object(guard.sys, "argv", ["guard", "--dry-run"]), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.dict(guard.os.environ, {"DEVICES": "0"}), \
                patch.object(guard, "sweep") as sweep:
            self.assertEqual(guard.main(), 0)
            self.assertIsNone(sweep.call_args.args[0])

    def test_dry_run_never_signals(self):
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout=TABLE)), \
                patch.object(guard, "process_info", return_value=(1000, 1, 555)), \
                patch.object(guard, "is_protected", return_value=False), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.object(guard.os, "pidfd_open") as pidfd:
            self.assertTrue(guard.sweep({0}, None, None, {}, 15, dry_run=True))
            pidfd.assert_not_called()

    def test_protected_and_other_users_never_signaled(self):
        for protected, uid in [(True, 1000), (False, 2000)]:
            with self.subTest(protected=protected, uid=uid), \
                    patch.object(guard.subprocess, "run", return_value=Mock(stdout=TABLE)), \
                    patch.object(guard, "process_info", return_value=(uid, 1, 555)), \
                    patch.object(guard, "is_protected", return_value=protected), \
                    patch.object(guard.os, "getuid", return_value=1000), \
                    patch.object(guard.os, "pidfd_open") as pidfd:
                guard.sweep({0}, 10, "token", {}, 15)
                pidfd.assert_not_called()

    def test_escalate_only_after_grace(self):
        pending = {}
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout=TABLE)), \
                patch.object(guard, "process_info", return_value=(1000, 1, 555)), \
                patch.object(guard, "is_protected", return_value=False), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.object(guard.os, "pidfd_open", return_value=9), \
                patch.object(guard.os, "close"), \
                patch.object(guard.signal, "pidfd_send_signal") as send, \
                patch.object(guard.time, "monotonic", side_effect=[100, 101, 116]):
            guard.sweep({0}, 10, "token", pending, 15)
            guard.sweep({0}, 10, "token", pending, 15)
            guard.sweep({0}, 10, "token", pending, 15)
            self.assertEqual([c.args[1] for c in send.call_args_list],
                             [signal.SIGTERM, signal.SIGKILL])

    def test_changed_identity_not_signaled(self):
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout=TABLE)), \
                patch.object(guard, "process_info", side_effect=[(1000, 1, 555), (1000, 1, 556)]), \
                patch.object(guard, "is_protected", return_value=False), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.object(guard.os, "pidfd_open", return_value=9), \
                patch.object(guard.os, "close"), \
                patch.object(guard.signal, "pidfd_send_signal") as send:
            guard.sweep({0}, 10, "token", {}, 15)
            send.assert_not_called()

    def test_root_requires_explicit_sudo_and_uses_helper(self):
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout=TABLE)), \
                patch.object(guard, "process_info", return_value=(0, 1, 555)), \
                patch.object(guard, "is_protected", return_value=False), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.object(guard, "signal_root_process") as privileged, \
                patch.object(guard.os, "pidfd_open") as pidfd:
            guard.sweep({0}, 10, "token", {}, 15)
            privileged.assert_not_called()
            guard.sweep({0}, 10, "token", {}, 15, allow_root=True)
            privileged.assert_called_once_with(101, (0, 1, 555), signal.SIGTERM)
            pidfd.assert_not_called()

    def test_sudo_dry_run_does_not_elevate(self):
        with patch.object(guard.sys, "argv", ["guard", "--sudo", "--dry-run"]), \
                patch.object(guard.os, "getuid", return_value=1000), \
                patch.object(guard, "sweep") as sweep, \
                patch.object(guard.subprocess, "run") as run:
            self.assertEqual(guard.main(), 0)
            self.assertTrue(sweep.call_args.kwargs["allow_root"])
            run.assert_not_called()

    def test_descendant_is_protected(self):
        with patch.object(guard.os, "getpid", return_value=900), \
                patch.object(guard.os, "getppid", return_value=901), \
                patch.object(guard, "process_info", return_value=(1000, 10, 555)):
            self.assertTrue(guard.is_protected(101, 10, None))

    def test_ready_or_timeout_stops_sweeping(self):
        for ready in [True, False]:
            server = Mock(pid=10)
            server.poll.return_value = None
            server.wait.return_value = 0
            with self.subTest(ready=ready), \
                    patch.object(guard.sys, "argv", ["guard", "--startup-timeout", "1"]), \
                    patch.object(guard.os, "getuid", return_value=1000), \
                    patch.object(guard.socket, "socket") as sock, \
                    patch.object(guard, "sweep", return_value=False) as sweep, \
                    patch.object(guard.subprocess, "Popen", return_value=server), \
                    patch.object(guard, "healthy", return_value=ready), \
                    patch.object(guard.time, "sleep"), \
                    patch.object(guard.time, "monotonic", side_effect=[0, 0, 2]):
                sock.return_value.__enter__.return_value.connect_ex.return_value = 1
                self.assertEqual(guard.main(), 0)
                self.assertEqual(sweep.call_count, 1)  # Preflight only.


if __name__ == "__main__":
    unittest.main()
