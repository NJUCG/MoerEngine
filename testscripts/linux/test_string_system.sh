#!/usr/bin/env bash

set -euo pipefail

CONFIG="${1:-Debug}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-core-linux"
BIN_DIR="${REPO_ROOT}/target/bin/${CONFIG}"
LOG_ROOT="${REPO_ROOT}/logs"
RUN_DIR="${LOG_ROOT}/run_$(date +%Y%m%d_%H%M%S)"
SUMMARY_FILE="${RUN_DIR}/summary.txt"
LOG_FILE="${RUN_DIR}/string_system.log"

mkdir -p "${RUN_DIR}"

write_summary() {
    printf '%s\n' "$1" | tee -a "${SUMMARY_FILE}"
}

write_summary "[MoerEngine Linux String System Test] $(date '+%Y-%m-%d %H:%M:%S')"
write_summary "Config   : ${CONFIG}"
write_summary "BuildDir : ${BUILD_DIR}"
write_summary "Logs     : ${RUN_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -Dmoer_build_test=ON \
    -DWITH_CUDA=OFF \
    -DWITH_NRD=OFF

cmake --build "${BUILD_DIR}" --target TestStringSystem -j"$(nproc)"

if [[ ! -x "${BIN_DIR}/TestStringSystem" ]]; then
    write_summary "[Test][StringSystem] FAILED  (missing executable: ${BIN_DIR}/TestStringSystem)"
    exit 1
fi

exit_code=0
"${BIN_DIR}/TestStringSystem" >"${LOG_FILE}" 2>&1 || exit_code=$?

if [[ "${exit_code}" -ne 0 ]]; then
    write_summary "[Test][StringSystem] FAILED  (exit code ${exit_code})"
    tail -n 20 "${LOG_FILE}" || true
    exit 1
fi

if ! grep -q '\[TESTCASE\]' "${LOG_FILE}"; then
    write_summary "[Test][StringSystem] FAILED  (structured testcase markers missing)"
    tail -n 20 "${LOG_FILE}" || true
    exit 1
fi

write_summary "[Test][StringSystem] PASSED"
write_summary "============================================================"
write_summary " TESTS   : PASSED"
write_summary "============================================================"
