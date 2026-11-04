"""CPU-only regressions: python -m unittest discover -s test -p test_host_chain_cleanup.py."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "client_test_host.sh"
CHAIN = ROOT / "run_test_chain_host.sh"


def shell_function(path, name):
    # These scripts use unindented function boundaries, with nested braces indented.
    source = path.read_text()
    match = re.search(rf"^{re.escape(name)}\(\) \{{\n.*?^\}}$", source, re.M | re.S)
    if match is None:
        raise AssertionError(f"Missing standalone shell function {name} in {path}")
    return match.group(0) + "\n"


@unittest.skipUnless(shutil.which("bash"), "requires bash")
class HostChainCleanupTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="host chain wiring ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.repo = self.directory / "fake repo"
        (self.repo / "scripts").mkdir(parents=True)
        self.calls = self.directory / "calls.jsonl"
        self.events = self.directory / "events"
        self.bash = shutil.which("bash")
        for name in ("python", "python3"):
            (self.bin / name).symlink_to(sys.executable)
        (self.repo / "scripts/host_server_process.py").write_text(
            "import json, os, sys\n"
            "with open(os.environ['CALLS'], 'a') as stream:\n"
            "    stream.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "sys.exit(int(os.environ.get('HELPER_RC', '0')))\n"
        )
        # A private PATH prevents extracted code from reaching real host tools.
        self.env = {
            "PATH": str(self.bin),
            "HOME": str(self.directory),
            "LC_ALL": "C",
            "REPO": str(self.repo),
            "PROCESS_HELPER": str(self.repo / "scripts/host_server_process.py"),
            "CALLS": str(self.calls),
            "EVENTS": str(self.events),
            "HOST": "127.0.0.1",
            "PORT": "48123",
            "RUN_DIR": str(self.directory),
        }

    def run_shell(self, source, **environment):
        return subprocess.run(
            [self.bash, "--noprofile", "--norc", "-c", "set -euo pipefail\n" + source],
            env={**self.env, **environment},
            capture_output=True,
            text=True,
            timeout=10,
        )

    def helper_calls(self):
        if not self.calls.exists():
            return []
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def client_stop(self, **environment):
        return self.run_shell(
            "ss() { printf 'ss:%s\\n' \"$*\" >>\"$EVENTS\"; }\n"
            "grep() { printf 'grep:%s\\n' \"$*\" >>\"$EVENTS\"; return 1; }\n"
            "kill() { printf 'unexpected kill\\n' >&2; return 99; }\n"
            + shell_function(CLIENT, "stop_server")
            + "stop_server\n",
            **environment,
        )

    def test_client_forwards_chain_identity_exactly(self):
        result = self.client_stop(
            UCM_HOST_SERVER_PID="12345",
            UCM_HOST_SERVER_START_TIME="67890",
            UCM_HOST_SERVER_TOKEN="token with spaces; not shell code",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.helper_calls(), [[
            "stop", "--port", "48123", "--pid", "12345", "--start-time", "67890",
            "--token", "token with spaces; not shell code",
        ]])
        self.assertCountEqual(self.events.read_text().splitlines(),
                              ["ss:-tln", "grep:-q :48123 "])

    def test_client_standalone_uses_only_exact_port(self):
        for host in ("127.0.0.1", "localhost"):
            with self.subTest(host=host):
                result = self.client_stop(HOST=host, UCM_HOST_SERVER_TOKEN="stray-token")
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.helper_calls(), [["stop", "--port", "48123"]] * 2)

    def test_client_without_token_still_forwards_pid_and_start_time(self):
        result = self.client_stop(UCM_HOST_SERVER_PID="12345", UCM_HOST_SERVER_START_TIME="67890")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.helper_calls(), [[
            "stop", "--port", "48123", "--pid", "12345", "--start-time", "67890",
        ]])

    def test_client_remote_host_never_calls_helper_or_checks_local_ports(self):
        for host in ("192.0.2.1", "remote.example"):
            with self.subTest(host=host):
                result = self.client_stop(HOST=host, UCM_HOST_SERVER_PID="12345",
                                          UCM_HOST_SERVER_START_TIME="67890",
                                          UCM_HOST_SERVER_TOKEN="chain-token")
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.helper_calls(), [])
        self.assertFalse(self.events.exists())

    def test_cleanup_stops_server_even_when_master_is_dead_and_preserves_status(self):
        for helper_rc in ("0", "1"):
            with self.subTest(helper_rc=helper_rc):
                result = self.run_shell(
                    "pid_alive() { printf 'alive:%s\\n' \"$1\" >>\"$EVENTS\"; return 1; }\n"
                    "signal_tree() { printf 'unexpected signal\\n' >&2; return 99; }\n"
                    "wait() { printf 'wait:%s\\n' \"$1\" >>\"$EVENTS\"; return 127; }\n"
                    + shell_function(CHAIN, "cleanup")
                    + "trap cleanup EXIT\nexit 7\n",
                    STATS_PID="", CLIENT_PID="", SERVER_PID="12345",
                    SERVER_START_TIME="67890", SERVER_TOKEN="chain-token", HELPER_RC=helper_rc,
                )
                self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(self.helper_calls(), [[
            "stop", "--port", "48123", "--pid", "12345", "--start-time", "67890",
            "--token", "chain-token", "--grace-seconds", "30",
        ]] * 2)
        self.assertEqual(self.events.read_text().splitlines(),
                          ["alive:", "alive:", "alive:12345", "wait:12345"] * 2)

    def test_cleanup_still_cleans_stats_and_client_without_global_signals(self):
        result = self.run_shell(
            "pid_alive() { [[ -n \"$1\" ]]; }\n"
            "signal_tree() { printf 'signal:%s:%s\\n' \"$1\" \"$2\" >>\"$EVENTS\"; }\n"
            "wait() { printf 'wait:%s\\n' \"$1\" >>\"$EVENTS\"; }\n"
            "sleep() { printf 'sleep:%s\\n' \"$1\" >>\"$EVENTS\"; }\n"
            + shell_function(CHAIN, "cleanup")
            + "cleanup\n",
            STATS_PID="111", CLIENT_PID="222", SERVER_PID="333",
            SERVER_START_TIME="444", SERVER_TOKEN="chain-token",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.events.read_text().splitlines(), [
            "signal:111:TERM", "wait:111", "signal:222:TERM", "sleep:1",
            "signal:222:KILL", "wait:222",
        ])
        self.assertEqual(self.helper_calls(), [[
            "stop", "--port", "48123", "--pid", "333", "--start-time", "444",
            "--token", "chain-token", "--grace-seconds", "30",
        ]])

    @unittest.skipUnless(sys.platform == "linux" and shutil.which("setsid"),
                         "requires Linux /proc and setsid")
    def test_actual_server_launch_captures_session_pid_start_time_and_token(self):
        (self.bin / "setsid").symlink_to(shutil.which("setsid"))
        (self.bin / "bash").symlink_to(self.bash)
        sudo = self.bin / "sudo"
        sudo.write_text(f'#!{self.bash}\nprintf "%s\\n" "$*" >>"$EVENTS"\n[[ "$*" == "-n true" ]]\n')
        sudo.chmod(0o755)
        (self.repo / "server_test_host.sh").write_text(
            "exec python3 - <<'PY'\n"
            "import json, os, time\n"
            "from pathlib import Path\n"
            "start = Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19]\n"
            "print(json.dumps([os.getpid(), os.getsid(0), start, "
            "os.environ['UCM_HOST_SERVER_TOKEN']]), flush=True)\n"
            "time.sleep(0.5)\n"
            "PY\n"
        )
        source = CHAIN.read_text()
        launch = source[source.index("if ! setsid sudo -n true;"):source.index("\nif ! wait_for_server;")]
        result = self.run_shell(
            launch
            + '\nwait "$SERVER_PID"\n'
            + 'printf "%s\\n" "$SERVER_PID" "$SERVER_START_TIME" "$SERVER_TOKEN"\n',
            SERVER_LOG=str(self.directory / "server.log"),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        pid, start, token = result.stdout.splitlines()
        observed = json.loads((self.directory / "server.log").read_text())
        self.assertEqual(observed, [int(pid), int(pid), start, token])
        self.assertRegex(token, r"^[0-9a-f]{32}$")
        self.assertEqual(self.events.read_text(), "-n true\n")

    def test_chain_client_launch_forwards_captured_identity(self):
        source = CHAIN.read_text().replace("\\\n", " ")
        self.assertRegex(source, (
            r'REPLAY_WINDOW_SEC="\$REPLAY_WINDOW_SEC"\s+'
            r'UCM_HOST_SERVER_PID="\$SERVER_PID"\s+'
            r'UCM_HOST_SERVER_START_TIME="\$SERVER_START_TIME"\s+'
            r'UCM_HOST_SERVER_TOKEN="\$SERVER_TOKEN"\s+'
            r'setsid bash "\$REPO/client_test_host.sh" --stop-server '
            r'>"\$CLIENT_LOG" 2>&1 &\nCLIENT_PID=\$!'
        ))


if __name__ == "__main__":
    unittest.main()
