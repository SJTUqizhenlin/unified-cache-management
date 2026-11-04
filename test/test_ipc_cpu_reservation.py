"""CPU-only tests: python -m unittest discover -s test -p test_ipc_cpu_reservation.py."""

import importlib.util
import os
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch


SOURCE = (
    Path(__file__).parents[1]
    / "ucm/integration/vllm/patch/cpu_binding_affinity_patch.py"
)


class IpcCpuReservationTest(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ)
        self.env.start()
        os.environ.pop("UCM_IPC_TARGET_CPUS", None)
        os.environ.pop("VLLM_CPU_AFFINITY", None)
        self.addCleanup(self.env.stop)
        self.logger = Mock()
        ascend = ModuleType("vllm_ascend")
        cpu_binding = ModuleType("vllm_ascend.cpu_binding")
        cpu_binding.logger = self.logger
        cpu_binding.MIN_CPUS_PER_NPU = 5
        ascend.cpu_binding = cpu_binding
        vllm_logger = ModuleType("vllm.logger")
        vllm_logger.logger = self.logger
        utils = ModuleType("ucm.integration.vllm.patch.utils")
        utils.patch_or_inject = setattr
        modules = patch.dict(
            sys.modules,
            {
                "psutil": ModuleType("psutil"),
                "vllm": ModuleType("vllm"),
                "vllm.logger": vllm_logger,
                "vllm_ascend": ascend,
                "vllm_ascend.cpu_binding": cpu_binding,
                "ucm.integration.vllm.patch.utils": utils,
            },
        )
        modules.start()
        self.addCleanup(modules.stop)
        spec = importlib.util.spec_from_file_location("cpu_affinity_under_test", SOURCE)
        self.affinity = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.affinity)

    def allocator(self, pools=None):
        return SimpleNamespace(
            npu_cpu_pool=pools if pools is not None else {0: list(range(96, 160))},
            assign_main={},
            assign_acl={},
            assign_rel={},
            bind=Mock(side_effect=AssertionError("allocation must not bind threads")),
        )

    @staticmethod
    def upstream(alloc):
        # Model upstream IRQ exclusions, including a hole in the assigned cores.
        for npu, pool in alloc.npu_cpu_pool.items():
            alloc.assign_main[npu] = pool[2:-2]
            alloc.assign_acl[npu] = [pool[-2]]
            alloc.assign_rel[npu] = [pool[-1]]

    def all_roles(self, alloc, npu):
        return [
            alloc.assign_main[npu],
            alloc.assign_acl[npu],
            alloc.assign_rel[npu],
            alloc.assign_ucm[npu],
            alloc.assign_ucm_health[npu],
        ]

    def test_parser(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = " 128-130, 132,129, 140 - 141 "
        self.assertEqual(
            self.affinity._ipc_target_cpus(), {128, 129, 130, 132, 140, 141}
        )

    def test_invalid_environment_rejected_before_upstream(self):
        for value in (
            "", " ", "128,", ",128", "-1", "3-1", "a", "1-2-3", "1.5", "1,,2"
        ):
            for fixed in (False, True):
                with self.subTest(value=value, fixed=fixed):
                    os.environ["UCM_IPC_TARGET_CPUS"] = value
                    orig = Mock()
                    with self.assertRaisesRegex(ValueError, "UCM_IPC_TARGET_CPUS"):
                        if fixed:
                            self.affinity.allocate_fixed(self.allocator(), orig)
                        else:
                            self.affinity.allocate(self.allocator())
                    orig.assert_not_called()

    def test_tp2_collision_with_and_without_affinity(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "128-159"
        for fixed in (False, True):
            for enabled in (False, True):
                with self.subTest(fixed=fixed, enabled=enabled):
                    if enabled:
                        os.environ["VLLM_CPU_AFFINITY"] = "1"
                    else:
                        os.environ.pop("VLLM_CPU_AFFINITY", None)
                    pools = {0: list(range(64)), 1: list(range(96, 160))}
                    alloc = self.allocator(pools)
                    if fixed:
                        self.affinity.allocate_fixed(alloc, self.upstream)
                    else:
                        self.affinity.allocate(alloc)
                    for npu in pools:
                        roles = self.all_roles(alloc, npu)
                        self.assertTrue(roles[0])
                        self.assertEqual(len(roles[1]), 1)
                        self.assertEqual(len(roles[2]), 1)
                        for role in roles:
                            self.assertTrue(set(role).isdisjoint(range(128, 160)))
                        self.assertTrue(
                            set(sum(roles, [])).isdisjoint(pools[npu][:2])
                        )
                    self.assertEqual(alloc.assign_acl[1], [126])
                    self.assertEqual(alloc.assign_rel[1], [127])
                    self.assertEqual(pools[1], list(range(96, 160)))
                    alloc.bind.assert_not_called()
                    self.assertTrue(
                        any(
                            "Reservation" in call.args[0]
                            for call in self.logger.info.call_args_list
                        )
                    )

    def test_legacy_filters_before_irq_slice_without_double_removing_holes(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "0,4,8"
        alloc = self.allocator({0: list(range(12))})
        self.affinity.allocate(alloc)
        self.assertEqual(alloc.assign_main[0], [3, 5, 6, 7, 9])
        self.assertEqual(alloc.assign_acl[0], [10])
        self.assertEqual(alloc.assign_rel[0], [11])

    def test_fixed_preserves_upstream_exclusions_and_role_counts(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "4,11,13"
        alloc = self.allocator({})

        def upstream(alloc):
            alloc.assign_main = {7: [2, 3, 4, 6, 7, 8, 9]}
            alloc.assign_acl = {7: [10, 11]}
            alloc.assign_rel = {7: [12, 13]}

        self.affinity.allocate_fixed(alloc, upstream)
        self.assertEqual(alloc.assign_main[7], [2, 3, 6, 7])
        self.assertEqual(alloc.assign_acl[7], [8, 9])
        self.assertEqual(alloc.assign_rel[7], [10, 12])

    def test_insufficient_cpus_rejected(self):
        for fixed in (False, True):
            for reserved in ("3-9", "2-9"):
                with self.subTest(fixed=fixed, reserved=reserved):
                    os.environ["UCM_IPC_TARGET_CPUS"] = reserved
                    alloc = self.allocator({0: list(range(10))})
                    with self.assertRaisesRegex(RuntimeError, "UCM_IPC_TARGET_CPUS"):
                        if fixed:
                            self.affinity.allocate_fixed(alloc, self.upstream)
                        else:
                            self.affinity.allocate(alloc)

    def test_exact_128_159_rank_collision_rejected(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "128-159"
        for fixed in (False, True):
            for enabled in (False, True):
                with self.subTest(fixed=fixed, enabled=enabled):
                    os.environ["VLLM_CPU_AFFINITY"] = "1" if enabled else "0"
                    alloc = self.allocator(
                        {0: list(range(96, 128)), 1: list(range(128, 160))}
                    )
                    with self.assertRaisesRegex(RuntimeError, "UCM_IPC_TARGET_CPUS"):
                        if fixed:
                            self.affinity.allocate_fixed(alloc, self.upstream)
                        else:
                            self.affinity.allocate(alloc)
                    alloc.bind.assert_not_called()

    def test_default_four_target_cpus_preserve_tp2_runtime_cores(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "132-135"
        alloc = self.allocator({2: list(range(128, 160))})
        self.affinity.allocate(alloc)
        self.assertEqual(alloc.assign_acl[2], [158])
        self.assertEqual(alloc.assign_rel[2], [159])
        self.assertEqual(alloc.assign_main[2], [130, 131, *range(136, 158)])
        for role in self.all_roles(alloc, 2):
            self.assertTrue(set(role).isdisjoint(range(132, 136)))

    def test_32_pollers_reserve_four_per_node_without_runtime_overlap(self):
        reserved = {node * 32 + offset for node in range(8) for offset in range(4, 8)}
        os.environ["UCM_IPC_TARGET_CPUS"] = ",".join(map(str, sorted(reserved)))
        pools = {node: list(range(node * 32, (node + 1) * 32)) for node in range(8)}
        alloc = self.allocator(pools)
        self.affinity.allocate(alloc)
        for node in pools:
            self.assertEqual(len(alloc.assign_main[node]), 24)
            self.assertEqual(alloc.assign_acl[node], [node * 32 + 30])
            self.assertEqual(alloc.assign_rel[node], [node * 32 + 31])
            for role in self.all_roles(alloc, node):
                self.assertTrue(set(role).isdisjoint(reserved))

    def test_no_environment_preserves_existing_plan(self):
        for fixed in (False, True):
            for enabled in (False, True):
                with self.subTest(fixed=fixed, enabled=enabled):
                    os.environ["VLLM_CPU_AFFINITY"] = "1" if enabled else "0"
                    alloc = self.allocator({0: list(range(16))})
                    if fixed:
                        self.affinity.allocate_fixed(alloc, self.upstream)
                    else:
                        self.affinity.allocate(alloc)
                    self.assertEqual(
                        alloc.assign_main[0], list(range(2, 8 if enabled else 14))
                    )
                    self.assertEqual(
                        alloc.assign_ucm[0], list(range(8, 13)) if enabled else []
                    )
                    self.assertEqual(
                        alloc.assign_ucm_health[0], [13] if enabled else []
                    )
                    self.assertEqual(alloc.assign_acl[0], [14])
                    self.assertEqual(alloc.assign_rel[0], [15])
                    self.logger.info.assert_not_called()

    def test_non_overlapping_reservation_preserves_upstream_roles(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "128-159"
        alloc = self.allocator({0: list(range(16))})
        self.affinity.allocate_fixed(alloc, self.upstream)
        self.assertEqual(alloc.assign_main[0], list(range(2, 14)))
        self.assertEqual(alloc.assign_acl[0], [14])
        self.assertEqual(alloc.assign_rel[0], [15])

    def test_fixed_fallback_reserves_legacy_pool(self):
        os.environ["UCM_IPC_TARGET_CPUS"] = "128-159"
        alloc = self.allocator()
        self.affinity.allocate_fixed(alloc)
        self.assertEqual(alloc.assign_main[0], list(range(98, 126)))
        self.assertEqual(alloc.assign_acl[0], [126])
        self.assertEqual(alloc.assign_rel[0], [127])


if __name__ == "__main__":
    unittest.main()
