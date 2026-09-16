#!/usr/bin/env bash

UCM_ENABLED=1

usage() {
  echo "Usage: $0 [--ucm on|off]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ucm)
      if [[ $# -lt 2 ]]; then
        echo "Error: --ucm requires on or off." >&2
        usage >&2
        exit 2
      fi
      case "$2" in
        on) UCM_ENABLED=1 ;;
        off) UCM_ENABLED=0 ;;
        *)
          echo "Error: --ucm must be on or off, got '$2'." >&2
          usage >&2
          exit 2
          ;;
      esac
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Error: unknown argument '$1'." >&2
      usage >&2
      exit 2
      ;;
  esac
done

export PLATFORM=ascend

export ENABLE_UCM_PATCH="$UCM_ENABLED"

export ASCEND_RT_VISIBLE_DEVICES=0,2,4,6

UCM_ARGS=()
if [[ "$UCM_ENABLED" == "1" ]]; then
  pip install -v -e . --no-build-isolation
  sleep 5
  mkdir -p /mnt/test
  rm -rf /mnt/test/*
  rm -f /dev/shm/uc_shm_cache*
  # export VLLM_CPU_AFFINITY=1
  sleep 3
  UCM_ARGS=(
    --kv-transfer-config
    '{
      "kv_connector": "UCMConnector",
      "kv_connector_module_path": "ucm.integration.vllm.ucm_connector",
      "kv_role": "kv_both",
      "kv_load_failure_policy": "recompute",
      "kv_connector_extra_config": {
        "UCM_CONFIG_FILE": "/workspace-genet/devA2/unified-cache-management/examples/ucm_config_example.yaml"
      }
    }'
  )
fi

echo "UCM enabled: $([[ "$UCM_ENABLED" == "1" ]] && echo yes || echo no)"

vllm serve /mnt/model/DeepSeek-V2-Lite-Chat \
  --served-model-name /mnt/model/DeepSeek-V2-Lite-Chat \
  --max-model-len 65536 \
  --tensor-parallel-size 4 --gpu_memory_utilization 0.9 \
  --block_size 128 --trust-remote-code --port 7780 --enforce-eager \
  --enable-prefix-caching \
  "${UCM_ARGS[@]}"
