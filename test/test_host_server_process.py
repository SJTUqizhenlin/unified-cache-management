"""CPU-only tests: python -m unittest discover -s test -p test_host_server_process.py."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import uuid


SCRIPT = Path(__file__).parents[1] / "scripts/host_server_process.py"
spec = importlib.util.spec_from_file_location("host_server_process", SCRIPT)
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def process(pid=100001, start=123, ppid=1, uid=None, state="S"):
    return host.Process(pid, start, ppid, os.getuid() if uid is None else uid, state)


class IdentificationTest(unittest.TestCase):
    def test_exact_nul_argv(self):
        cases = [
            (["/env/bin/vllm", "serve", "model", "--port", "8123"], True),
            (["vllm", "serve", "--port=8123"], True),
            (["python3.9", "/env/bin/vllm", "serve", "--port=8123"], True),
            (["/usr/bin/python", "-u", "-X", "dev", "vllm", "serve", "--port", "8123"], True),
            (["bash", "-c", "vllm serve --port 8123"], False),
            (["python3", "-c", "vllm", "serve", "--port=8123"], False),
            (["python3", "-m", "vllm", "serve", "--port=8123"], False),
            (["worker", "vllm", "serve", "--port=8123"], False),
            (["vllm-worker", "serve", "--port=8123"], False),
            (["vllm", "other", "--port=8123"], False),
            (["vllm", "serve", "--port=81230"], False),
            (["vllm", "serve", "--port", "8123 --other"], False),
            (["vllm", "serve", "--other-port=8123"], False),
            (["vllm", "serve", "--port"], False),
            (["vllm", "serve", "--", "--port=8123"], False),
            (["vllm", "serve", "--port=8123", "--port=9999"], False),
        ]
        for argv, expected in cases:
            with self.subTest(argv=argv), patch.object(
                    Path, "read_bytes", return_value=b"\0".join(os.fsencode(a) for a in argv) + b"\0"
            ), patch.object(host, "same_process", return_value=True):
                self.assertEqual(host.is_server(process(), 8123), expected)

    def test_unique_match_and_ambiguity(self):
        for matches in ([], [process()], [process(), process(100002)]):
            with self.subTest(count=len(matches)), patch.object(
                    host, "processes", return_value=iter(matches)
            ), patch.object(host, "is_server", return_value=True):
                if len(matches) == 1:
                    self.assertEqual(host.identify(8123), matches[0])
                else:
                    with self.assertRaises(RuntimeError):
                        host.identify(8123)

    def test_explicit_mismatch_never_scans(self):
        for current in (None, process(start=124), process(uid=os.getuid() + 1),
                        process(state="Z"), process()):
            with self.subTest(current=current), patch.object(
                    host, "read_process", return_value=current
            ), patch.object(host, "is_server", return_value=False), patch.object(host, "processes") as scan:
                with self.assertRaises(RuntimeError):
                    host.identify(8123, 100001, 123)
                scan.assert_not_called()

    def test_read_proc_stat_with_spaces_and_parentheses(self):
        fields = ["S", "42"] + ["0"] * 17 + ["12345"]
        with patch.object(Path, "read_text", return_value="99 (odd ) name) " + " ".join(fields)), \
                patch.object(Path, "stat") as stat:
            stat.return_value.st_uid = os.getuid()
            self.assertEqual(host.read_process(99), process(99, 12345, 42))

    def test_cli_output_defaults_and_bad_arguments(self):
        output = io.StringIO()
        with patch.object(host, "identify", return_value=process()), contextlib.redirect_stdout(output):
            self.assertEqual(host.main(["identify", "--port", "8123"]), 0)
        self.assertEqual(output.getvalue(), "100001 123\n")
        with patch.object(host, "stop") as stop:
            self.assertEqual(host.main(["stop", "--port", "8123"]), 0)
            stop.assert_called_once_with(8123, None, None, None, 90, 2)
        for extra in (["--pid", "123"], ["--start-time", "123"],
                      ["--grace-seconds", "nan"], ["--term-seconds", "-1"]):
            with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    host.main(["stop", "--port", "8123", *extra])
                self.assertEqual(error.exception.code, 2)


class ScopedStopTest(unittest.TestCase):
    def test_token_is_exact_and_same_identity(self):
        for environment, expected in (
                (b"UCM_HOST_SERVER_TOKEN=abc\0", True),
                (b"UCM_HOST_SERVER_TOKEN=abcd\0", False),
                (b"OTHER_UCM_HOST_SERVER_TOKEN=abc\0", False)):
            with patch.object(Path, "read_bytes", return_value=environment), \
                    patch.object(host, "same_process", return_value=True):
                self.assertEqual(host.has_token(process(), "abc"), expected)
        with patch.object(Path, "read_bytes", return_value=b"UCM_HOST_SERVER_TOKEN=abc\0"), \
                patch.object(host, "same_process", return_value=False):
            self.assertFalse(host.has_token(process(), "abc"))

    def test_token_explicit_wrong_pair_or_token_never_scans(self):
        for root, matching in ((process(start=124), True),
                               (process(uid=os.getuid() + 1), True), (process(), False)):
            with self.subTest(root=root, matching=matching), patch.object(
                    host, "read_process", return_value=root
            ), patch.object(host, "has_token", return_value=matching), \
                    patch.object(host, "protected_pids", return_value=set()), \
                    patch.object(host, "processes") as scan:
                with self.assertRaises(RuntimeError):
                    host.stop(8123, 100001, 123, "token", 0, 0)
                scan.assert_not_called()

    def test_token_cannot_target_self_parent_or_other_uid(self):
        own = process(os.getpid())
        parent = process(os.getppid())
        stranger = process(100003, uid=os.getuid() + 1)
        with patch.object(host, "read_process", return_value=None), \
                patch.object(host, "processes", return_value=[own, parent, stranger]), \
                patch.object(host, "has_token", return_value=True), \
                patch.object(host, "same_process", side_effect=lambda p: p.uid == os.getuid()), \
                patch.object(host, "send_signal") as send:
            host.stop(8123, token="token", grace_seconds=0, term_seconds=0)
            send.assert_not_called()

    def test_pidfd_validation_after_open_and_close(self):
        root = process()
        for valid, expected in ((True, 1), (False, 0)):
            with self.subTest(valid=valid), patch.object(
                    host, "same_process", side_effect=[True, valid]
            ), patch.object(host.os, "pidfd_open", return_value=55, create=True) as opening, \
                    patch.object(host.signal, "pidfd_send_signal", create=True) as send, \
                    patch.object(host.os, "close") as close, patch.object(host.os, "kill") as kill:
                host.send_signal(root, signal.SIGTERM, set())
                opening.assert_called_once_with(root.pid, 0)
                self.assertEqual(send.call_count, expected)
                close.assert_called_once_with(55)
                kill.assert_not_called()

    def test_fallback_checks_identity_and_protected_processes(self):
        with patch.object(host.os, "pidfd_open", side_effect=OSError(38, "unsupported"), create=True), \
                patch.object(host.signal, "pidfd_send_signal", create=True), \
                patch.object(host, "same_process", return_value=True), \
                patch.object(host.os, "kill") as kill:
            host.send_signal(process(), signal.SIGINT, {100001})
            kill.assert_not_called()
            host.send_signal(process(), signal.SIGINT, set())
            kill.assert_called_once_with(100001, signal.SIGINT)

    def test_dead_reused_or_different_uid_never_signaled(self):
        for current in (None, process(start=124), process(state="Z"),
                        process(uid=os.getuid() + 1)):
            with self.subTest(current=current), patch.object(
                    host, "read_process", return_value=current
            ), patch.object(host.os, "kill") as kill, \
                    patch.object(host.os, "pidfd_open", create=True) as opening:
                host.send_signal(process(), signal.SIGKILL, set())
                opening.assert_not_called()
                kill.assert_not_called()

    def test_survivors_fail_after_bounded_escalation(self):
        root = process()
        with patch.object(host, "identify", return_value=root), \
                patch.object(host, "processes", return_value=[root]), \
                patch.object(host, "same_process", return_value=True), \
                patch.object(host, "send_signal") as send:
            with self.assertRaisesRegex(RuntimeError, "survived"):
                host.stop(8123, grace_seconds=0, term_seconds=0)
            self.assertEqual([call.args[1] for call in send.call_args_list],
                             [signal.SIGINT, signal.SIGTERM, signal.SIGKILL])

    def test_unlimited_tree_snapshot_reparenting_and_signal_order(self):
        root = process()
        # A chain, not just 100 direct children: traversal must have no depth cap.
        children = [process(100002 + i, ppid=100001 + i) for i in range(100)]
        unrelated = process(200001)
        live = {p.identity: p for p in [root, *children, unrelated]}
        sent = []

        def send(p, action, protected):
            sent.append((p.identity, action))
            if action == signal.SIGINT:
                live.pop(root.identity)
                # All children detach before the next snapshot.
                for child in children:
                    live[child.identity] = child._replace(ppid=1)
            elif action == signal.SIGKILL:
                live.pop(p.identity)

        with patch.object(host, "identify", return_value=root), \
                patch.object(host, "processes", side_effect=lambda: list(live.values())), \
                patch.object(host, "same_process", side_effect=lambda p: p.identity in live), \
                patch.object(host, "send_signal", side_effect=send):
            host.stop(8123, grace_seconds=0, term_seconds=0)
        self.assertEqual(sent[0], (root.identity, signal.SIGINT))
        self.assertEqual([sig for _, sig in sent[1:]], [signal.SIGTERM] * 100 + [signal.SIGKILL] * 100)
        self.assertEqual(set(live), {unrelated.identity})

    def test_reused_parent_does_not_confer_ownership(self):
        root = process()
        reused = root._replace(start=124)
        unrelated_child = process(100002, ppid=root.pid)
        snapshots = iter([[root], [reused, unrelated_child]])
        with patch.object(host, "identify", return_value=root), \
                patch.object(host, "processes", side_effect=lambda: next(snapshots)), \
                patch.object(host, "same_process", side_effect=[True, True, False, False]), \
                patch.object(host, "send_signal") as send:
            host.stop(8123, grace_seconds=0, term_seconds=0)
            send.assert_called_once_with(root, signal.SIGINT, unittest.mock.ANY)

    def test_new_children_during_grace_are_snapshotted(self):
        root = process()
        child = process(100002, ppid=root.pid)
        live = {root.identity: root}
        sent = []

        def sleep(_):
            live[child.identity] = child

        def send(p, action, protected):
            sent.append((p.identity, action))
            if action == signal.SIGTERM:
                live.pop(p.identity)

        with patch.object(host, "identify", return_value=root), \
                patch.object(host, "processes", side_effect=lambda: list(live.values())), \
                patch.object(host, "same_process", side_effect=lambda p: p.identity in live), \
                patch.object(host, "send_signal", side_effect=send), \
                patch.object(host.time, "sleep", side_effect=sleep), \
                patch.object(host.time, "monotonic", side_effect=range(100)):
            host.stop(8123, grace_seconds=3, term_seconds=0)
        self.assertIn((child.identity, signal.SIGTERM), sent)


@unittest.skipUnless(sys.platform == "linux", "requires Linux /proc")
class RealProcessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.children = []
        self.addCleanup(self.cleanup_children)
        self.token = uuid.uuid4().hex

    def cleanup_children(self):
        for pid, start in self.children:
            current = host.read_process(pid)
            if current is not None and current.start == start and current.alive:
                host.send_signal(current, signal.SIGKILL, host.protected_pids())

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.01)
        self.fail("dummy subprocess did not become ready")

    def launch(self, argv):
        child = subprocess.Popen(argv, env={**os.environ, "UCM_HOST_SERVER_TOKEN": self.token},
                                 start_new_session=True, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        self.addCleanup(child.wait, 5)
        current = host.read_process(child.pid)
        self.assertIsNotNone(current)
        self.children.append(current.identity)
        # unittest cleanups are LIFO: kill test-owned processes before waiting.
        self.addCleanup(self.cleanup_children)
        return child, current

    def test_exact_identification_and_standalone_stop_real_dummy(self):
        script = Path(self.temp.name) / "vllm"
        script.write_text("import time\ntime.sleep(30)\n")
        child, root = self.launch([sys.executable, str(script), "serve", "model", "--port=48123"])
        self.wait_for(lambda: host.is_server(root, 48123))
        result = subprocess.run([sys.executable, str(SCRIPT), "identify", "--port", "48123",
                                 "--pid", str(root.pid), "--start-time", str(root.start)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"{root.pid} {root.start}\n")
        host.stop(48123, root.pid, root.start, grace_seconds=0.1, term_seconds=0.2)
        child.wait(timeout=5)
        self.assertFalse(host.same_process(root))

    def test_parent_exits_detached_child_survives_token_recovers_it(self):
        token = self.token
        self.token = uuid.uuid4().hex
        unrelated, unrelated_identity = self.launch([sys.executable, "-c", "import time; time.sleep(30)"])
        self.token = token
        ready = Path(self.temp.name) / "ready"
        code = """
import os, signal, sys, time
pid = os.fork()
if pid:
    os._exit(0)
os.setsid()
signal.signal(signal.SIGINT, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
with open(sys.argv[1], 'w') as stream:
    stream.write(str(os.getpid()))
time.sleep(30)
"""
        parent, root = self.launch([sys.executable, "-c", code, str(ready)])
        self.wait_for(lambda: ready.exists() and ready.read_text())
        orphan = host.read_process(int(ready.read_text()))
        self.assertIsNotNone(orphan)
        self.children.append(orphan.identity)
        parent.wait(timeout=5)
        self.assertTrue(host.same_process(orphan))
        # Root is gone and the child has a new session and parent. No argv match.
        host.stop(48123, root.pid, root.start, self.token, 0.02, 0.2)
        self.assertFalse(host.same_process(orphan))
        self.assertIsNone(unrelated.poll())
        self.assertTrue(host.same_process(unrelated_identity))

    def test_snapshot_retains_detached_child_after_sigint_master_exit(self):
        ready = Path(self.temp.name) / "ready"
        code = """
import os, signal, sys, time
signal.signal(signal.SIGINT, lambda *_: os._exit(0))
pid = os.fork()
if not pid:
    os.setsid()
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with open(sys.argv[1], 'w') as stream:
        stream.write(str(os.getpid()))
time.sleep(30)
"""
        parent, root = self.launch([sys.executable, "-c", code, str(ready)])
        self.wait_for(lambda: ready.exists() and ready.read_text())
        child = host.read_process(int(ready.read_text()))
        self.children.append(child.identity)
        # Mock only the server argv check: exercise real /proc traversal/signals,
        # without requiring vllm or relying on a token to recover the child.
        with patch.object(host, "identify", return_value=root):
            host.stop(48123, root.pid, root.start, grace_seconds=0.05, term_seconds=0.2)
        parent.wait(timeout=5)
        self.assertFalse(host.same_process(child))


if __name__ == "__main__":
    unittest.main()
