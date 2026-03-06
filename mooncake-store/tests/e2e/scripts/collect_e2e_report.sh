#!/usr/bin/env bash
set -euo pipefail

SUMMARY=""
OUT=""
COMMIT=""
ARTIFACT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --summary) SUMMARY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --commit) COMMIT="$2"; shift 2 ;;
    --artifact) ARTIFACT="$2"; shift 2 ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${SUMMARY}" || -z "${OUT}" ]]; then
  echo "Usage: $0 --summary <summary.csv> --out <report.md> [--commit <sha>] [--artifact <dir>]" >&2
  exit 2
fi

if [[ ! -f "${SUMMARY}" ]]; then
  echo "summary not found: ${SUMMARY}" >&2
  exit 1
fi

total=0
pass=0
fail=0
blocked=0

p0_fail=0
p0_cases=("TC-CFG-01" "TC-RW-01" "TC-SPDK-01" "TC-SPDK-03" "TC-OBS-01")

is_p0_case() {
  local c="$1"
  for p in "${p0_cases[@]}"; do
    if [[ "${p}" == "${c}" ]]; then
      return 0
    fi
  done
  return 1
}

while IFS=, read -r case_id profile status duration detail; do
  if [[ "${case_id}" == "case" ]]; then
    continue
  fi
  total=$((total + 1))
  case "${status}" in
    PASS) pass=$((pass + 1)) ;;
    FAIL) fail=$((fail + 1)) ;;
    BLOCKED) blocked=$((blocked + 1)) ;;
  esac
  if is_p0_case "${case_id}" && [[ "${status}" == "FAIL" ]]; then
    p0_fail=1
  fi
done <"${SUMMARY}"

decision="Go"
if [[ "${p0_fail}" == "1" ]]; then
  decision="No-Go"
elif [[ "${fail}" -gt 0 || "${blocked}" -gt 0 ]]; then
  decision="Conditional Go"
fi

now_ts=$(date '+%Y-%m-%d %H:%M:%S %Z')

{
  echo "# Mooncake SPDK-Only Real-Env E2E Test Report"
  echo
  echo "- Time: ${now_ts}"
  if [[ -n "${COMMIT}" ]]; then
    echo "- Commit: ${COMMIT}"
  fi
  if [[ -n "${ARTIFACT}" ]]; then
    echo "- Artifact Root: ${ARTIFACT}"
  fi
  echo
  echo "## Summary"
  echo
  echo "| Total | Pass | Fail | Blocked | Decision |"
  echo "|---:|---:|---:|---:|---|"
  echo "| ${total} | ${pass} | ${fail} | ${blocked} | ${decision} |"
  echo
  echo "## Case Results"
  echo
  echo "| Case | Profile | Status | Duration(s) | Detail |"
  echo "|---|---|---|---:|---|"
  tail -n +2 "${SUMMARY}" | while IFS=, read -r case_id profile status duration detail; do
    echo "| ${case_id} | ${profile} | ${status} | ${duration} | ${detail} |"
  done
} >"${OUT}"

echo "Generated report: ${OUT}"
