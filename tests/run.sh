#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: ./run.sh <target_name|all>"
    echo "Example: ./run.sh test_thread_pool"
    exit 1
fi

TARGET_NAME="$1"  # e.g. test_job_queue / test_thread_pool / all

mkdir -p build-tests
cd build-tests

cmake ../

run_one_target() {
    local base="$1"

    cmake --build . --target "${base}" "${base}_asan" "${base}_tsan" -j4

    echo "=== RUNNING ${base} (NORMAL) ==="
    ./"${base}"
    echo "=== RUNNING ${base} (ASAN) ==="
    ASAN_OPTIONS=detect_leaks=1 ./"${base}_asan"
    echo "=== RUNNING ${base} (TSAN) ==="
    # disable ASLR for TSAN runtime stability
    setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 ./"${base}_tsan"
}

if [ "${TARGET_NAME}" = "all" ]; then
    run_one_target test_job_queue
    run_one_target test_thread_pool

else
    run_one_target "${TARGET_NAME}"
fi
