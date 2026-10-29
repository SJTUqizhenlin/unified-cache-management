"""CPU-only histogram summary regression tests."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "ucm_metric_summary", Path(__file__).parents[1] / "scripts/ucm_metric_summary.py"
)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class MetricSummaryTest(unittest.TestCase):
    def histogram(self, rank, counts, total, count):
        name = "ucm:fawa_load_wait_duration_ms"
        rows = [f'{name}_bucket{{worker_rank="{rank}",le="{bound}"}} {value}'
                for bound, value in counts]
        rows += [f'{name}_sum{{worker_rank="{rank}"}} {total}',
                 f'{name}_count{{worker_rank="{rank}"}} {count}']
        return "\n".join(rows)

    def test_pools_buckets_and_uses_weighted_average(self):
        text = self.histogram(0, [(1, 90), (10, 100), ("+Inf", 100)], 200, 100)
        text += "\n" + self.histogram(1, [(1, 0), (10, 100), ("+Inf", 100)], 800, 100)
        result = summary.fawa_wait_summary(text)
        self.assertIn("count=200 avg=5.000 ms", result)
        self.assertIn("p99=~9.836 ms", result)

    def test_missing_and_empty(self):
        for text in ["", self.histogram(0, [(1, 0), ("+Inf", 0)], 0, 0)]:
            self.assertIn("avg=N/A p99=N/A", summary.fawa_wait_summary(text))

    def test_overflow_is_not_finite_estimate(self):
        text = self.histogram(0, [(10, 90), ("+Inf", 100)], 2000, 100)
        self.assertIn("p99=>= 10.000 ms (overflow bucket)", summary.fawa_wait_summary(text))

    def test_missing_buckets_cannot_produce_percentile(self):
        text = self.histogram(0, [], 20, 2)
        self.assertIn("avg=10.000 ms p99=N/A", summary.fawa_wait_summary(text))

    def test_start_load_separate_from_nonempty_barrier(self):
        text = self.histogram(0, [(1, 90), (10, 100), ("+Inf", 100)], 200, 100)
        text += "\n" + text.replace("fawa_load_wait_duration_ms", "connector_start_load_kv_duration_ms").replace(" 200", " 400")
        result = summary.start_load_summary(text)
        self.assertIn("all calls, including no-load", result)
        self.assertIn("count=100 avg=4.000 ms p99=~9.100 ms", result)
        self.assertIn("avg=2.000 ms", summary.fawa_wait_summary(text))

    def test_missing_start_load_is_na(self):
        self.assertIn("count=0 avg=N/A p99=N/A", summary.start_load_summary(""))

    def test_conditional_entry_uses_its_own_histogram(self):
        text = self.histogram(0, [(100, 90), (200, 100), ("+Inf", 100)], 12000, 100)
        text = text.replace("fawa_load_wait_duration_ms", "fawa_start_load_kv_duration_ms")
        self.assertIn("count=100 avg=120.000 ms p99=~190.000 ms",
                      summary.fawa_start_load_summary(text))
        self.assertIn("nonempty loads", summary.fawa_start_load_summary(text))
        self.assertIn("count=0", summary.fawa_wait_summary(text))


if __name__ == "__main__":
    unittest.main()
