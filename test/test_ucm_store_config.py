"""Configuration tests without store, model or device access."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import mock_open, patch


spec = importlib.util.spec_from_file_location(
    "ucm_store_config", Path(__file__).resolve().parents[1] / "scripts/ucm_store_config.py"
)
config_reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config_reader)


class StoreConfigTest(unittest.TestCase):
    def test_backend_detection(self):
        for pipeline in ("Cache|Posix", "Posix", " cache | POSIX "):
            self.assertEqual(config_reader.backend({"store_pipeline": pipeline}), "posix")
        for pipeline in ("", "Fake", "Cache|Spdk", "Cache|Empty", "Cache|Fake|Posix",
                         "Cache|Spdk|Posix", "Posix|Cache", "Posix|Posix"):
            with self.subTest(pipeline=pipeline), self.assertRaisesRegex(ValueError, "unsupported"):
                config_reader.backend({"store_pipeline": pipeline})

    def test_posix_engine_from_configuration_not_traffic(self):
        config = {"store_pipeline": "Cache|Posix"}
        self.assertEqual(config_reader.posix_engine(config), "psync")
        self.assertEqual(config_reader.posix_engine(config | {"posix_io_engine": "aio"}), "aio")
        with self.assertRaises(ValueError):
            config_reader.posix_engine(config | {"posix_io_engine": "invalid"})
        with self.assertRaises(ValueError):
            config_reader.posix_engine({"store_pipeline": "Cache|Spdk"})

    def test_load_config_validates_connector_shape(self):
        valid = {"ucm_connectors": [{"ucm_connector_config": {"store_pipeline": "Cache|Posix"}}]}
        with patch("builtins.open", mock_open(read_data=json.dumps(valid))):
            self.assertEqual(config_reader.load_config("config.yaml"),
                             valid["ucm_connectors"][0]["ucm_connector_config"])
        for data in (None, {}, {"ucm_connectors": {}}, {"ucm_connectors": []},
                     {"ucm_connectors": [None]}, {"ucm_connectors": [{}, {}]},
                     {"ucm_connectors": [{"ucm_connector_config": None}]}):
            with self.subTest(data=data), patch("builtins.open", mock_open(read_data=json.dumps(data))), \
                    self.assertRaises(ValueError):
                config_reader.load_config("config.yaml")

    def test_detection_cli(self):
        for action, expected in (("--detect", "posix\n"), ("--posix-engine", "aio\n")):
            with patch.object(sys, "argv", ["config", "--config", "fake", action]), \
                    patch.object(config_reader, "load_config", return_value={
                        "store_pipeline": "Cache|Posix", "posix_io_engine": "aio"}), \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                config_reader.main()
                self.assertEqual(out.getvalue(), expected)

    def test_config_path_from_running_process(self):
        transfer = {"kv_connector_extra_config": {"UCM_CONFIG_FILE": "/tmp/config.yaml"}}
        for argv, expected in ((["vllm", "--kv-transfer-config", json.dumps(transfer)], "/tmp/config.yaml\n"),
                               (["vllm"], "")):
            with patch.object(sys, "argv", ["config", "--pid", "123", "--config-path"]), \
                    patch.object(Path, "read_bytes", return_value="\0".join(argv).encode()) as read, \
                    patch.object(config_reader, "load_config") as load, \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                config_reader.main()
                self.assertEqual(out.getvalue(), expected)
                read.assert_called_once()
                load.assert_not_called()

    def test_invalid_cli_values(self):
        for options in ([], ["--detect"], ["--config-path"], ["--pid", "0", "--config-path"],
                        ["--detect", "--posix-engine"]):
            with self.subTest(options=options), patch.object(sys, "argv", ["config", *options]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exc:
                config_reader.main()
            self.assertEqual(exc.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
