"""Exercise the launcher's embedded backend parser without starting services."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml


class ServerBackendDetectionTest(unittest.TestCase):
    def parse(self, pipeline, **options):
        script = (Path(__file__).parents[1] / "server_test_host.sh").read_text()
        code = script.split('SPDK_INFO=$(python - "$UCM_CONFIG" <<\'PY\'\n', 1)[1]
        code = code.split("\nPY\n", 1)[0]
        config = {"ucm_connectors": [{"ucm_connector_config": {
            "store_pipeline": pipeline, **options,
        }}]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            path.write_text(yaml.safe_dump(config))
            result = subprocess.run([sys.executable, "-", str(path)], input=code,
                                    capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.rstrip("\n").split("\x1f")

    def test_non_spdk_pipeline_skips_target(self):
        for pipeline in ("Cache|Empty", "Empty", "Cache|Posix"):
            with self.subTest(pipeline=pipeline):
                self.assertEqual(self.parse(pipeline), ["0", "", ""])

    def test_ipc_without_legacy_target_key(self):
        trid = "ipc:/tmp/test.sock|trtype:PCIe traddr:0000:84:00.0"
        self.assertEqual(self.parse("Cache|Spdk", spdk_trid=trid), ["1", trid, ""])

    def test_legacy_target_key_is_preserved(self):
        self.assertEqual(self.parse("Cache|Spdk", spdk_trid="ipc:/tmp/test.sock",
                                    ipc_target_trid="trtype:PCIe"),
                         ["1", "ipc:/tmp/test.sock", "trtype:PCIe"])


if __name__ == "__main__":
    unittest.main()
