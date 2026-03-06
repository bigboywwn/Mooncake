#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "${SCRIPT_DIR}/../../../.." && pwd)

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-spdk}"
SPDK_DIR="${SPDK_DIR:-/Users/miaomili/Documents/Playground/Mooncake/extern/spdk-23.01}"
ARTIFACT_ROOT="${ARTIFACT_ROOT:-${ROOT_DIR}/artifacts}"
METADATA_URL="${METADATA_URL:-P2PHANDSHAKE}"
MASTER_ADDR="${MASTER_ADDR:-127.0.0.1:50051}"
MASTER_PORT="${MASTER_PORT:-50051}"
METRICS_PORT="${METRICS_PORT:-9003}"
DO_BUILD="${DO_BUILD:-1}"

MASTER_BIN="${BUILD_DIR}/mooncake-store/src/mooncake_master"
RUNNER_BIN="${BUILD_DIR}/mooncake-store/tests/e2e/tiered_e2e_runner"
SETUP_SPDK_SCRIPT="${SCRIPT_DIR}/setup_spdk_ram_targets.sh"
COLLECT_SCRIPT="${SCRIPT_DIR}/collect_e2e_report.sh"

GIT_SHA=$(git -C "${ROOT_DIR}" rev-parse --short HEAD)
TS=$(date +%Y%m%d-%H%M%S)
OUT_DIR="${ARTIFACT_ROOT}/e2e-spdk-realenv-${GIT_SHA}-${TS}"
LOG_DIR="${OUT_DIR}/logs"
ENV_DIR="${OUT_DIR}/env"
METRIC_DIR="${OUT_DIR}/metrics"
SUMMARY_CSV="${OUT_DIR}/summary.csv"

mkdir -p "${LOG_DIR}" "${ENV_DIR}" "${METRIC_DIR}"
echo "case,profile,status,duration_sec,detail" >"${SUMMARY_CSV}"

MASTER_PID=""

cleanup() {
  if [[ -n "${MASTER_PID}" ]]; then
    kill "${MASTER_PID}" >/dev/null 2>&1 || true
    wait "${MASTER_PID}" >/dev/null 2>&1 || true
    MASTER_PID=""
  fi
}
trap cleanup EXIT

record_case() {
  local case_id="$1"
  local profile="$2"
  local status="$3"
  local dur="$4"
  local detail="$5"
  echo "${case_id},${profile},${status},${dur},${detail}" >>"${SUMMARY_CSV}"
}

wait_master() {
  local ok=0
  for _ in $(seq 1 40); do
    if nc -z 127.0.0.1 "${MASTER_PORT}" >/dev/null 2>&1; then
      ok=1
      break
    fi
    sleep 1
  done
  [[ "${ok}" == "1" ]]
}

start_master() {
  local profile="$1"
  local ddr_enabled="$2"
  local ssd_enabled="$3"
  local remotefs_enabled="$4"
  local ssd_targets_json="$5"

  cleanup

  local master_log="${LOG_DIR}/${profile}_master.log"
  local root_fs_args=()
  if [[ "${remotefs_enabled}" == "1" ]]; then
    local rfs_dir="/dev/shm/mooncake-remotefs/${profile}"
    mkdir -p "${rfs_dir}"
    root_fs_args=("--root_fs_dir=${rfs_dir}" "--cluster_id=mooncake_${profile}")
  fi

  local env_file="${ENV_DIR}/${profile}.env"
  {
    echo "MC_DDR_POOL_ENABLED=${ddr_enabled}"
    echo "MC_SSD_POOL_ENABLED=${ssd_enabled}"
    echo "MC_NVMEOF_CLIENT_IMPL=spdk"
    echo "MC_SSD_QUEUE_LIMIT=1024"
    echo "MC_SSD_IO_TIMEOUT_MS=5000"
    if [[ "${ssd_enabled}" == "1" ]]; then
      echo "MC_SSD_TARGETS_JSON=${ssd_targets_json}"
    fi
  } >"${env_file}"

  (
    export MC_DDR_POOL_ENABLED="${ddr_enabled}"
    export MC_SSD_POOL_ENABLED="${ssd_enabled}"
    export MC_NVMEOF_CLIENT_IMPL=spdk
    export MC_SSD_QUEUE_LIMIT=1024
    export MC_SSD_IO_TIMEOUT_MS=5000
    if [[ "${ssd_enabled}" == "1" ]]; then
      export MC_SSD_TARGETS_JSON="${ssd_targets_json}"
    else
      unset MC_SSD_TARGETS_JSON
    fi

    "${MASTER_BIN}" \
      --rpc_port="${MASTER_PORT}" \
      --rpc_thread_num=2 \
      --enable_ha=false \
      --enable_http_metadata_server=false \
      --metrics_port="${METRICS_PORT}" \
      "${root_fs_args[@]}" \
      >"${master_log}" 2>&1 &
    echo $! >"${OUT_DIR}/.${profile}.master.pid"
  )

  MASTER_PID=$(cat "${OUT_DIR}/.${profile}.master.pid")
  rm -f "${OUT_DIR}/.${profile}.master.pid"

  if ! wait_master; then
    return 1
  fi
  return 0
}

run_runner_case() {
  local case_id="$1"
  local profile="$2"
  local expect_tiers="$3"
  local ssd_targets_json="$4"
  local extra_args=()
  shift 4
  if [[ $# -gt 0 ]]; then
    extra_args=("$@")
  fi

  local case_log="${LOG_DIR}/${profile}_${case_id}.log"
  local start_ts end_ts dur status
  start_ts=$(date +%s)
  if (
      export MC_NVMEOF_CLIENT_IMPL=spdk
      if [[ -n "${ssd_targets_json}" ]]; then
        export MC_SSD_TARGETS_JSON="${ssd_targets_json}"
      else
        unset MC_SSD_TARGETS_JSON
      fi
      "${RUNNER_BIN}" \
        --case="${case_id}" \
        --master="${MASTER_ADDR}" \
        --metadata="${METADATA_URL}" \
        --protocol=tcp \
        --expect-tiers="${expect_tiers}" \
        --expect-impl=spdk \
        "${extra_args[@]}"
    ) >"${case_log}" 2>&1; then
    status="PASS"
  else
    status="FAIL"
  fi
  end_ts=$(date +%s)
  dur=$((end_ts - start_ts))
  record_case "${case_id}" "${profile}" "${status}" "${dur}" "${case_log}"
  [[ "${status}" == "PASS" ]]
}

run_metrics_case() {
  local profile="$1"
  local metric_log="${METRIC_DIR}/${profile}_metrics.prom"
  local start_ts end_ts dur
  start_ts=$(date +%s)
  if curl -fsS "http://127.0.0.1:${METRICS_PORT}/metrics" >"${metric_log}"; then
    if grep -q "master_ssd_extent_release_fail_total" "${metric_log}" && \
       grep -q "ssd_reactor_cpu_usage_pct" "${metric_log}"; then
      record_case "TC-OBS-01" "${profile}" "PASS" "$(( $(date +%s) - start_ts ))" "${metric_log}"
      return 0
    fi
    record_case "TC-OBS-01" "${profile}" "FAIL" "$(( $(date +%s) - start_ts ))" "missing key metrics"
    return 1
  fi
  end_ts=$(date +%s)
  dur=$((end_ts - start_ts))
  record_case "TC-OBS-01" "${profile}" "BLOCKED" "${dur}" "metrics endpoint unavailable"
  return 0
}

run_spdk_failfast_case() {
  local profile="SPDK_FAILFAST"
  cleanup

  local broken_json='[{"name":"bad1","trtype":"tcp","traddr":"127.0.0.1","trsvcid":"55220","subnqn":"nqn.2026-03.io.mooncake:ssdpool-bad","nsid":1,"capacity_bytes":268435456,"weight":1}]'
  if ! start_master "${profile}" 0 1 0 "${broken_json}"; then
    record_case "TC-SPDK-03" "${profile}" "BLOCKED" 0 "master startup failed"
    return 0
  fi

  run_runner_case "TC-SPDK-03" "${profile}" "SSD" "${broken_json}" --expect-client-init-fail=true || true
  cleanup
}

if [[ "${DO_BUILD}" == "1" ]]; then
  cmake --build "${BUILD_DIR}" \
    --target mooncake_master mooncake_client clientctl tiered_e2e_runner \
    -j3
fi

if [[ ! -x "${RUNNER_BIN}" ]]; then
  echo "tiered_e2e_runner not found: ${RUNNER_BIN}" >&2
  exit 1
fi

TARGETS_JSON=$(
  "${SETUP_SPDK_SCRIPT}" \
    --spdk-dir "${SPDK_DIR}" \
    --rpc-sock /tmp/mooncake-spdk.sock \
    --target-count 3 \
    --base-port 4420 \
    --capacity-mb 256 \
    --print-json
)

declare -a PROFILES=("T1" "T2" "T3" "T4" "T5" "T6" "T7")
declare -A DDR=( [T1]=1 [T2]=0 [T3]=0 [T4]=1 [T5]=1 [T6]=0 [T7]=1 )
declare -A SSD=( [T1]=0 [T2]=1 [T3]=0 [T4]=1 [T5]=0 [T6]=1 [T7]=1 )
declare -A RFS=( [T1]=0 [T2]=0 [T3]=1 [T4]=0 [T5]=1 [T6]=1 [T7]=1 )
declare -A TIERS=(
  [T1]="DDR"
  [T2]="SSD"
  [T3]="remoteFS"
  [T4]="DDR,SSD"
  [T5]="DDR,remoteFS"
  [T6]="SSD,remoteFS"
  [T7]="DDR,SSD,remoteFS"
)

for profile in "${PROFILES[@]}"; do
  ssd_json=""
  if [[ "${SSD[${profile}]}" == "1" ]]; then
    ssd_json="${TARGETS_JSON}"
  fi

  if ! start_master "${profile}" "${DDR[${profile}]}" "${SSD[${profile}]}" "${RFS[${profile}]}" "${ssd_json}"; then
    record_case "MASTER_START" "${profile}" "BLOCKED" 0 "master failed to start"
    continue
  fi

  run_runner_case "TC-CFG-01" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true
  run_runner_case "TC-RW-01" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true
  run_runner_case "TC-RW-02" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true

  if [[ "${RFS[${profile}]}" == "1" ]]; then
    run_runner_case "TC-RFS-01" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true
  fi

  if [[ "${SSD[${profile}]}" == "1" ]]; then
    run_runner_case "TC-SPDK-01" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true
    run_runner_case "TC-SPDK-02" "${profile}" "${TIERS[${profile}]}" "${ssd_json}" || true
  fi

  run_metrics_case "${profile}" || true
  cleanup

done

run_spdk_failfast_case

"${COLLECT_SCRIPT}" \
  --summary "${SUMMARY_CSV}" \
  --out "${OUT_DIR}/E2E_TEST_REPORT.md" \
  --commit "$(git -C "${ROOT_DIR}" rev-parse HEAD)" \
  --artifact "${OUT_DIR}"

echo "Matrix execution completed. Artifacts: ${OUT_DIR}"
