"""Read the configured Posix backend without initializing a store or model."""

import argparse
import json
from pathlib import Path
import sys


def load_config(path):
    import yaml

    with open(path) as stream:
        config = yaml.safe_load(stream)
    if not isinstance(config, dict):
        raise ValueError("YAML must contain a ucm_connectors list")
    entries = config.get("ucm_connectors")
    if not isinstance(entries, list) or len(entries) != 1:
        raise ValueError("configuration requires exactly one ucm_connectors entry")
    entry = entries[0]
    if not isinstance(entry, dict) or not isinstance(entry.get("ucm_connector_config"), dict):
        raise ValueError("connector must contain a ucm_connector_config mapping")
    return entry["ucm_connector_config"]


def backend(config):
    stages = [part.strip().lower() for part in str(config.get("store_pipeline", "")).split("|")]
    if stages not in (["posix"], ["cache", "posix"]):
        raise ValueError("unsupported store_pipeline; this tool supports Posix or Cache|Posix only")
    return "posix"


def posix_engine(config):
    backend(config)
    engine = config.get("posix_io_engine", "psync")
    if engine not in ("psync", "aio"):
        raise ValueError(f"unsupported posix_io_engine: {engine!r}")
    return engine


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="")
    parser.add_argument("--pid", type=int)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--config-path", action="store_true")
    actions.add_argument("--detect", action="store_true")
    actions.add_argument("--posix-engine", action="store_true")
    args = parser.parse_args()
    if args.config_path:
        if args.pid is None or args.pid <= 0:
            parser.error("--config-path requires a positive --pid")
        argv = Path(f"/proc/{args.pid}/cmdline").read_bytes().decode().split("\0")
        if "--kv-transfer-config" in argv:
            config = json.loads(argv[argv.index("--kv-transfer-config") + 1])
            print(config.get("kv_connector_extra_config", {}).get("UCM_CONFIG_FILE", ""))
        return
    if not args.config:
        parser.error("--config is required for backend/engine detection")
    config = load_config(args.config)
    print(posix_engine(config) if args.posix_engine else backend(config))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Store configuration error: {error}", file=sys.stderr)
        sys.exit(1)
