#!/usr/bin/env bash

set -euo pipefail

CONFIG="${1:-Debug}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-core-linux"
BIN_DIR="${REPO_ROOT}/target/bin/${CONFIG}"
LOG_ROOT="${REPO_ROOT}/logs"
RUN_DIR="${LOG_ROOT}/run_$(date +%Y%m%d_%H%M%S)"
SUMMARY_FILE="${RUN_DIR}/summary.txt"

mkdir -p "${RUN_DIR}"

write_summary() {
    printf '%s\n' "$1" | tee -a "${SUMMARY_FILE}"
}

run_test() {
    local label="$1"
    local exe_path="$2"
    local log_file="$3"

    if [[ ! -x "${exe_path}" ]]; then
        write_summary "[Test][${label}] FAILED  (missing executable: ${exe_path})"
        return 1
    fi

    local exit_code=0
    "${exe_path}" >"${log_file}" 2>&1 || exit_code=$?

    if [[ "${exit_code}" -eq 0 ]]; then
        if grep -q '\[TESTCASE\]' "${log_file}"; then
            write_summary "[Test][${label}] PASSED"
            return 0
        fi

        write_summary "[Test][${label}] FAILED  (structured testcase markers missing)"
        tail -n 20 "${log_file}" || true
        return 1
    fi

    write_summary "[Test][${label}] FAILED  (exit code ${exit_code})"
    tail -n 20 "${log_file}" || true
    return 1
}

write_summary "[MoerEngine Linux Core Test] $(date '+%Y-%m-%d %H:%M:%S')"
write_summary "Config   : ${CONFIG}"
write_summary "BuildDir : ${BUILD_DIR}"
write_summary "Logs     : ${RUN_DIR}"

rm -rf "${BUILD_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CONFIG}" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DMOER_CORE_ONLY=ON \
    -Dmoer_build_test=ON \
    -DWITH_CUDA=OFF \
    -DWITH_NRD=OFF

cmake --build "${BUILD_DIR}" --target TestCoreLinux -j"$(nproc)"

failed=0
run_test "TaskGraph" "${BIN_DIR}/TestTaskGraph" "${RUN_DIR}/taskgraph.log" || failed=1
run_test "TaskPipe" "${BIN_DIR}/TestTaskPipe" "${RUN_DIR}/taskpipe.log" || failed=1

if [[ "${failed}" -ne 0 ]]; then
    write_summary "============================================================"
    write_summary " TESTS   : FAILED"
    write_summary "============================================================"
    exit 1
fi

write_summary "============================================================"
write_summary " TESTS   : PASSED"
write_summary "============================================================"
