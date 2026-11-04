"""Parser tests for scripts/vllm_probe.py; no live vLLM or container access."""

import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "vllm_probe", Path(__file__).resolve().parents[1] / "scripts" / "vllm_probe.py"
)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class VllmProbeTest(unittest.TestCase):
    def test_vllm_serve_options(self):
        process = {
            "argv": [
                "python", "/env/bin/vllm", "serve",
                "/models/DeepSeek", "--served-model-name", "deepseek",
                "--port", "7780", "--host=0.0.0.0",
            ]
        }
        info = probe.vllm_info(process)
        self.assertEqual(info["model"], "/models/DeepSeek")
        self.assertEqual(info["served"], ["deepseek"])
        self.assertEqual(info["port"], 7780)
        self.assertEqual(info["host"], "0.0.0.0")

    def test_model_option_and_default_port(self):
        process = {
            "argv": [
                "python", "-m", "vllm.entrypoints.openai.api_server",
                "--model=/models/Qwen", "--served-model-name=a,b",
            ]
        }
        info = probe.vllm_info(process)
        self.assertEqual(info["model"], "/models/Qwen")
        self.assertEqual(info["served"], ["a", "b"])
        self.assertEqual(info["port"], 8000)

    def test_worker_is_not_a_server(self):
        process = {"argv": ["VLLM::Worker_TP0", "--port", "7780"]}
        self.assertIsNone(probe.vllm_info(process))

    def test_kubernetes_cgroup_identity(self):
        cgroup = (
            "12:cpuset:/kubepods.slice/kubepods-burstable.slice/"
            "kubepods-burstable-podeadbeef.slice/"
            "cri-containerd-0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.scope\n"
            "0::/\n"
        )
        container_id, runtime = probe.container_identity(cgroup)
        self.assertEqual(container_id,
                         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
        self.assertEqual(runtime, "kubernetes")
        self.assertIn("cri-containerd-", probe.cgroup_path(cgroup))

    def test_docker_cgroup_identity(self):
        cgroup = "1:name=systemd:/docker/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        container_id, runtime = probe.container_identity(cgroup)
        self.assertEqual(container_id,
                         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
        self.assertEqual(runtime, "docker")

    def test_proc_net_addresses(self):
        self.assertEqual(probe.parse_proc_address("0100007F:1F90"), ("127.0.0.1", 8080))
        self.assertEqual(probe.parse_proc_address("00000000000000000000000001000000:1F90"),
                         ("::1", 8080))

    def test_container_text_distinguishes_host_and_unresolved_container(self):
        self.assertEqual(probe.container_text(None, "/system.slice/ssh.service"), "host")
        self.assertEqual(probe.container_text(None, "0::/kubepods.slice/item"), "container (unresolved)")
        info = {"runtime": "kubernetes", "id": "abc123", "pod": "p", "namespace": "n",
                "name": "workspace"}
        text = probe.container_text(info)
        self.assertIn("pod=p", text)
        self.assertIn("container=workspace", text)


if __name__ == "__main__":
    unittest.main()
