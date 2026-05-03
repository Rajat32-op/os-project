#!/usr/bin/env bash
set -euo pipefail

DURATION=${1:-60}
WORKLOAD_CMD=${2:-}
BASELINE_SUMMARY="results/raw/baseline.summary.csv"
RL_SUMMARY="results/raw/rl.summary.csv"
BASELINE_CSV="results/raw/baseline.csv"
RL_CSV="results/raw/rl.csv"
BASELINE_WORKLOAD_LOG="results/raw/baseline.workload.txt"
RL_WORKLOAD_LOG="results/raw/rl.workload.txt"

mkdir -p results/raw
make all setup

function cleanup() {
    pkill -f "sudo ./os_manager" 2>/dev/null || true
    pkill -f "python3 rl_agent.py" 2>/dev/null || true
    if [[ -n "${WORKLOAD_PID-}" ]]; then
        kill "$WORKLOAD_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Helper to run a workload for a specific duration and log output
run_workload_for_duration() {
    local duration=$1
    local output_file=$2
    local cmd=$3
    
    if [[ -z "$cmd" ]]; then
        echo "[workload] No workload command specified, skipping workload"
        return 0
    fi
    
    echo "[workload] Starting: $cmd (duration: ${duration}s, output: $output_file)"
    bash -c "$cmd" > "$output_file" 2>&1 &
    local pid=$!
    
    sleep "$duration"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "[workload] Finished, output saved to $output_file"
}

echo "Running baseline for ${DURATION}s..."
run_workload_for_duration "$DURATION" "$BASELINE_WORKLOAD_LOG" "$WORKLOAD_CMD" &
WORKLOAD_PID=$!

sudo env OSMGR_MODE=BASELINE OSMGR_CSV="$BASELINE_CSV" OSMGR_SUMMARY="$BASELINE_SUMMARY" ./os_manager &
OS_PID=$!

wait "$WORKLOAD_PID" 2>/dev/null || true
sleep 2
kill "$OS_PID" 2>/dev/null || true
wait "$OS_PID" 2>/dev/null || true

echo "Running RL-enabled for ${DURATION}s..."
run_workload_for_duration "$DURATION" "$RL_WORKLOAD_LOG" "$WORKLOAD_CMD" &
WORKLOAD_PID=$!

python3 rl_agent.py &
RL_PID=$!
sleep 2

sudo env OSMGR_MODE=RL OSMGR_CSV="$RL_CSV" OSMGR_SUMMARY="$RL_SUMMARY" ./os_manager &
OS_PID=$!

wait "$WORKLOAD_PID" 2>/dev/null || true
sleep 2
kill "$OS_PID" 2>/dev/null || true
kill "$RL_PID" 2>/dev/null || true
wait "$OS_PID" 2>/dev/null || true
wait "$RL_PID" 2>/dev/null || true

echo ""
echo "Comparison runs complete."
echo "Results:"
echo "  Baseline summary: $BASELINE_SUMMARY"
echo "  RL summary:       $RL_SUMMARY"
echo "  Baseline raw data: $BASELINE_CSV"
echo "  RL raw data:       $RL_CSV"
if [[ -n "$WORKLOAD_CMD" ]]; then
    echo "  Baseline workload log: $BASELINE_WORKLOAD_LOG"
    echo "  RL workload log:       $RL_WORKLOAD_LOG"
fi
echo ""
echo "Run: python3 analyze_results.py"
