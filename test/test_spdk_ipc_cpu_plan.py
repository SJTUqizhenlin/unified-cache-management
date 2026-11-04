"""CPU-only tests: python -m unittest discover -s test -p test_spdk_ipc_cpu_plan.py."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "spdk_ipc_cpu_plan", Path(__file__).parents[1] / "scripts/spdk_ipc_cpu_plan.py"
)
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


class SpdkIpcCpuPlanTest(unittest.TestCase):
    def setUp(self):
        self.nodes = {node: list(range(node * 32, (node + 1) * 32)) for node in range(8)}
        self.allowed = set(range(256))

    def invoke(self, *args, nodes=None, allowed=None):
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(planner, "read_topology", return_value=self.nodes if nodes is None else nodes), \
                patch.object(planner.os, "sched_getaffinity",
                             return_value=self.allowed if allowed is None else allowed) as affinity, \
                redirect_stdout(output), redirect_stderr(errors):
            result = planner.main(list(args))
        affinity.assert_called_once_with(0)
        return result, output.getvalue(), errors.getvalue()

    def test_default_host_256_cpus_eight_nodes(self):
        result, output, errors = self.invoke()
        expected = [node * 32 + offset for offset in range(4, 8) for node in range(8)]
        self.assertEqual(result, 0)
        self.assertEqual(output.splitlines(), [
            ",".join(map(str, expected)),
            ",".join(f"{i}@{cpu}" for i, cpu in enumerate(expected)),
        ])
        self.assertIn("NUMA allocation (automatic, 32 reactors)", errors)
        self.assertIn("node0=4,5,6,7", errors)
        self.assertIn("node7=228,229,230,231", errors)

    def test_uneven_count_uses_numeric_node_order(self):
        nodes = {10: list(range(64, 96)), 2: list(range(32, 64)), 0: list(range(32))}
        self.assertEqual(planner.plan_cpus(5, nodes, self.allowed), [4, 36, 68, 5, 37])

    def test_exhausted_nodes_are_skipped_without_using_reserves(self):
        nodes = {0: list(range(7)), 1: list(range(32, 42))}
        self.assertEqual(planner.plan_cpus(5, nodes, self.allowed), [4, 36, 37, 38, 39])
        with self.assertRaisesRegex(ValueError, "Insufficient eligible CPUs"):
            planner.plan_cpus(6, nodes, self.allowed)

    def test_full_safe_capacity_excludes_first_four_and_last_two(self):
        cpus = planner.plan_cpus(208, self.nodes, self.allowed)
        self.assertEqual(len(set(cpus)), 208)
        for node in range(8):
            self.assertTrue(set(cpus).isdisjoint(self.nodes[node][:4] + self.nodes[node][-2:]))
        with self.assertRaisesRegex(ValueError, "reserved CPUs will not be used"):
            planner.plan_cpus(209, self.nodes, self.allowed)

    def test_cpuset_filter_does_not_shift_reserved_positions(self):
        self.assertEqual(
            planner.plan_cpus(4, self.nodes, {0, 4, 6, 30, 31, 36, 39, 62, 63}),
            [4, 36, 6, 39],
        )
        with self.assertRaisesRegex(ValueError, "have 4"):
            planner.plan_cpus(5, self.nodes, {0, 4, 6, 30, 31, 36, 39, 62, 63})

    def test_empty_cpuset_and_small_nodes_fail(self):
        for nodes, allowed in ((self.nodes, set()), ({0: list(range(6))}, self.allowed)):
            with self.subTest(nodes=nodes, allowed=allowed):
                with self.assertRaisesRegex(ValueError, "have 0"):
                    planner.plan_cpus(1, nodes, allowed)

    def test_explicit_cpus_preserve_order_and_override_reservations(self):
        result, output, errors = self.invoke("--reactors", "5", "--cpus", "31, 2-4,0")
        self.assertEqual(result, 0)
        self.assertEqual(output, "31,2,3,4,0\n0@31,1@2,2@3,3@4,4@0\n")
        self.assertIn("explicit", errors)

    def test_legacy_cpu_base_is_contiguous(self):
        result, output, _ = self.invoke("--reactors", "4", "--cpu-base", "30")
        self.assertEqual(result, 0)
        self.assertEqual(output, "30,31,32,33\n0@30,1@31,2@32,3@33\n")

    def test_explicit_and_legacy_must_be_within_affinity(self):
        for override in (("--cpus", "4,5"), ("--cpu-base", "4")):
            with self.subTest(override=override):
                result, output, errors = self.invoke("--reactors", "2", *override, allowed={4})
                self.assertEqual(result, 1)
                self.assertEqual(output, "")
                self.assertIn("not allowed by process affinity: 5", errors)

    def test_malformed_cpu_lists(self):
        for value in ("", " ", "1,", ",1", "1,,2", "-1", "3-1", "a", "1.5",
                      "1-2-3", "1,1", "1-3,3", "01,1", "1,+2"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    planner.parse_cpus(value)

    def test_explicit_count_mismatch(self):
        result, output, errors = self.invoke("--reactors", "3", "--cpus", "4-5")
        self.assertEqual(result, 1)
        self.assertEqual(output, "")
        self.assertIn("CPU count 2 does not match --reactors 3", errors)

    def test_invalid_counts_and_base(self):
        for args in (("--reactors", "0"), ("--reactors", "-1"), ("--cpu-base", "-1")):
            with self.subTest(args=args):
                result, output, errors = self.invoke(*args)
                self.assertEqual(result, 1)
                self.assertEqual(output, "")
                self.assertIn("must be", errors)

    def test_overrides_conflict(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
            planner.main(["--cpus", "4", "--cpu-base", "4"])
        self.assertEqual(exc.exception.code, 2)

    def test_exact_map_is_validated_and_normalized(self):
        result, output, _ = self.invoke(
            "--reactors", "2", "--cpus", "4,36", "--lcore-map", "1@36,0@4")
        self.assertEqual(result, 0)
        self.assertEqual(output, "4,36\n0@4,1@36\n")

    def test_wrong_one_to_one_maps_fail_without_stdout(self):
        for mapping in ("0@4", "0@4,1@36,2@68", "0@36,1@4", "0@4,1@4",
                        "1@4,2@36", "0@4,0@36"):
            with self.subTest(mapping=mapping):
                result, output, errors = self.invoke(
                    "--reactors", "2", "--cpus", "4,36", "--lcore-map", mapping)
                self.assertEqual(result, 1)
                self.assertEqual(output, "")
                self.assertIn("--lcore-map", errors)

    def test_unsupported_maps_give_actionable_error(self):
        for mapping in ("(0-3)@(4-7)", "0-3@4-7", "", "0@4,", "0 @4", "-1@4"):
            with self.subTest(mapping=mapping):
                with self.assertRaisesRegex(ValueError, "pass --cpus and omit --lcore-map"):
                    planner.lcore_map([4, 5, 6, 7], mapping)

    def test_custom_map_validates_automatic_selection(self):
        result, output, _ = self.invoke("--reactors", "2", "--lcore-map", "0@4,1@36")
        self.assertEqual(result, 0)
        self.assertEqual(output, "4,36\n0@4,1@36\n")

    def test_fake_sysfs_topology_numeric_sort_and_empty_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in (("node10", "64-67,70\n"), ("node2", "32-39\n"),
                               ("node0", "0-7\n"), ("node3", "\n"), ("nodebogus", "bad")):
                (root / name).mkdir()
                (root / name / "cpulist").write_text(text)
            nodes = planner.read_topology(root)
        self.assertEqual(list(nodes), [0, 2, 3, 10])
        self.assertEqual(nodes[10], [64, 65, 66, 67, 70])
        self.assertEqual(nodes[3], [])

    def test_missing_topology_requires_explicit_override(self):
        result, output, errors = self.invoke(nodes={})
        self.assertEqual(result, 1)
        self.assertEqual(output, "")
        self.assertIn("No NUMA node cpulists", errors)
        result, output, errors = self.invoke("--reactors", "1", "--cpus", "4", nodes={})
        self.assertEqual(result, 0)
        self.assertEqual(output, "4\n0@4\n")
        self.assertIn("unknown=4", errors)

    def test_sysfs_error_is_reported_without_stdout(self):
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(planner, "read_topology", side_effect=OSError("cannot read cpulist")), \
                redirect_stdout(output), redirect_stderr(errors):
            self.assertEqual(planner.main([]), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("cannot read cpulist", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
