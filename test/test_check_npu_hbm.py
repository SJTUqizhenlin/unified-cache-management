"""Regression coverage for npu-smi spacing, IDs and fail-closed startup checks."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_npu_hbm.py"
spec = importlib.util.spec_from_file_location("check_npu_hbm", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

TABLE = """
| NPU   Name                | Health        | Power(W) Temp(C) Hugepages-Usage(page)|
| Chip                      | Bus-Id        | AICore(%) Memory-Usage(MB) HBM-Usage(MB)|
| 0     910B3               | OK            | 89.9 32 0 / 0 |
| 0                         | 0000:C1:00.0  | 0   0    / 0     58738/ 65536 |
| 1     910B3               | OK            | 87.6 31 0 / 0 |
| 0                         | 0000:c2:00.0  | 0   0 / 0        3400 /65536 |
| 7     910B3               | OK            | 89.5 38 0 / 0 |
| 0                         | 0000:42:00.0  | 0   0/0          58719/65536 |
| NPU     Chip              | Process id    | Process name | Process memory(MB) |
| 0       0                 | 1182558       | VLLMWorker_DP | 55476 |
"""


class NpuHbmTest(unittest.TestCase):
    def check(self, devices, text=TABLE):
        return subprocess.run([sys.executable, str(SCRIPT), "--devices", devices],
                              input=text, text=True, capture_output=True)

    def test_real_format_and_chip_ids(self):
        self.assertEqual(checker.parse_hbm(TABLE), {0: 58738, 1: 3400, 7: 58719})

    def test_only_selected_cards_block(self):
        self.assertEqual(self.check("1").returncode, 0)
        result = self.check("0,1,7")
        self.assertEqual(result.returncode, 1)
        self.assertIn("NPU 0: HBM 58738 MB", result.stdout)
        self.assertIn("NPU 7: HBM 58719 MB", result.stdout)

    def test_missing_empty_and_malformed_output_fail_closed(self):
        for text in ("", "npu-smi failed", TABLE.replace("58738/ 65536", "N/A")):
            self.assertNotEqual(self.check("0", text).returncode, 0)
        self.assertNotEqual(self.check("2").returncode, 0)

    def test_threshold_and_invalid_usage(self):
        self.assertEqual(self.check("0", TABLE.replace("58738/ 65536", "8000 / 65536")).returncode, 0)
        self.assertEqual(self.check("0", TABLE.replace("58738/ 65536", "8001 / 65536")).returncode, 1)
        self.assertEqual(self.check("0", TABLE.replace("58738/ 65536", "70000 / 65536")).returncode, 1)

    def test_invalid_devices(self):
        for devices in ("", "-1", "0,bad", "0,"):
            self.assertNotEqual(self.check(devices).returncode, 0)


if __name__ == "__main__":
    unittest.main()
