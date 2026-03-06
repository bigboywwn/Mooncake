#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/../../../.." && pwd)

SPDK_DIR="${SPDK_DIR:-/Users/miaomili/Documents/Playground/Mooncake/extern/spdk-23.01}"
RPC_SOCK="${RPC_SOCK:-/tmp/mooncake-spdk.sock}"
LOG_FILE="${LOG_FILE:-/tmp/mooncake-spdk-tgt.log}"
PID_FILE="${PID_FILE:-/tmp/mooncake-spdk-tgt.pid}"
TARGET_HOST="${TARGET_HOST:-127.0.0.1}"
BASE_PORT="${BASE_PORT:-4420}"
TARGET_COUNT="${TARGET_COUNT:-3}"
CAPACITY_MB="${CAPACITY_MB:-256}"
NQN_PREFIX="${NQN_PREFIX:-nqn.2026-03.io.mooncake:ssdpool}"
PRINT_JSON=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --spdk-dir) SPDK_DIR="$2"; shift 2 ;;
    --rpc-sock) RPC_SOCK="$2"; shift 2 ;;
    --log-file) LOG_FILE="$2"; shift 2 ;;
    --pid-file) PID_FILE="$2"; shift 2 ;;
    --host) TARGET_HOST="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    --target-count) TARGET_COUNT="$2"; shift 2 ;;
    --capacity-mb) CAPACITY_MB="$2"; shift 2 ;;
    --nqn-prefix) NQN_PREFIX="$2"; shift 2 ;;
    --print-json) PRINT_JSON=1; shift ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

SPDK_TGT="${SPDK_DIR}/build/bin/spdk_tgt"
RPC_PY="${SPDK_DIR}/scripts/rpc.py"

if [[ ! -x "${SPDK_TGT}" ]]; then
  echo "spdk_tgt not found: ${SPDK_TGT}" >&2
  exit 1
fi
if [[ ! -x "${RPC_PY}" ]]; then
  echo "rpc.py not found: ${RPC_PY}" >&2
  exit 1
fi

rpc() {
  "${RPC_PY}" -s "${RPC_SOCK}" "$@"
}

ensure_spdk_tgt() {
  if rpc spdk_get_version >/dev/null 2>&1; then
    return 0
  fi

  rm -f "${RPC_SOCK}"
  nohup "${SPDK_TGT}" --wait-for-rpc -r "${RPC_SOCK}" >"${LOG_FILE}" 2>&1 &
  local pid=$!
  echo "${pid}" >"${PID_FILE}"

  local ok=0
  for _ in $(seq 1 60); do
    if rpc spdk_get_version >/dev/null 2>&1; then
      ok=1
      break
    fi
    sleep 1
  done

  if [[ "${ok}" != "1" ]]; then
    echo "spdk_tgt is not ready, log: ${LOG_FILE}" >&2
    exit 1
  fi
}

ensure_framework_initialized() {
  if rpc nvmf_get_transports >/dev/null 2>&1; then
    return 0
  fi

  rpc framework_start_init >/dev/null 2>&1 || true

  local ok=0
  for _ in $(seq 1 30); do
    if rpc nvmf_get_transports >/dev/null 2>&1; then
      ok=1
      break
    fi
    sleep 1
  done

  if [[ "${ok}" != "1" ]]; then
    echo "SPDK framework init is not ready, log: ${LOG_FILE}" >&2
    exit 1
  fi
}

ensure_transport() {
  if ! rpc nvmf_get_transports 2>/dev/null | grep -Eq '"trtype"[[:space:]]*:[[:space:]]*"TCP"'; then
    rpc nvmf_create_transport -t TCP -u 16384 -m 8 -c 8192 >/dev/null
  fi
}

recreate_targets() {
  local i bdev nqn serial port
  for i in $(seq 0 $((TARGET_COUNT - 1))); do
    bdev="mc_ram_${i}"
    nqn="${NQN_PREFIX}${i}"
    rpc nvmf_delete_subsystem "${nqn}" >/dev/null 2>&1 || true
    rpc bdev_malloc_delete "${bdev}" >/dev/null 2>&1 || true
  done

  for i in $(seq 0 $((TARGET_COUNT - 1))); do
    bdev="mc_ram_${i}"
    nqn="${NQN_PREFIX}${i}"
    serial="MCSSD$(printf '%02d' "${i}")"
    port="$((BASE_PORT + i))"

    rpc bdev_malloc_create "${CAPACITY_MB}" 4096 -b "${bdev}" >/dev/null
    rpc nvmf_create_subsystem "${nqn}" -a -s "${serial}" >/dev/null
    rpc nvmf_subsystem_add_ns "${nqn}" "${bdev}" >/dev/null
    rpc nvmf_subsystem_add_listener "${nqn}" -t tcp -a "${TARGET_HOST}" -s "${port}" >/dev/null
  done
}

build_targets_json() {
  local cap_bytes
  cap_bytes=$((CAPACITY_MB * 1024 * 1024))
  local json="["
  local i sep=""
  for i in $(seq 0 $((TARGET_COUNT - 1))); do
    local nqn port name
    nqn="${NQN_PREFIX}${i}"
    port="$((BASE_PORT + i))"
    name="t$((i + 1))"
    json+="${sep}{\"name\":\"${name}\",\"trtype\":\"tcp\",\"traddr\":\"${TARGET_HOST}\",\"trsvcid\":\"${port}\",\"subnqn\":\"${nqn}\",\"nsid\":1,\"capacity_bytes\":${cap_bytes},\"weight\":1}"
    sep=","
  done
  json+="]"
  printf '%s\n' "${json}"
}

ensure_spdk_tgt
ensure_framework_initialized
ensure_transport
recreate_targets

TARGETS_JSON=$(build_targets_json)

if [[ "${PRINT_JSON}" == "1" ]]; then
  printf '%s\n' "${TARGETS_JSON}"
else
  cat <<OUT
SPDK target setup complete.
- spdk_dir: ${SPDK_DIR}
- rpc_sock: ${RPC_SOCK}
- target_count: ${TARGET_COUNT}
- base_port: ${BASE_PORT}
- capacity_mb: ${CAPACITY_MB}
export MC_SSD_TARGETS_JSON='${TARGETS_JSON}'
OUT
fi
