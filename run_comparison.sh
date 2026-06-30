#!/usr/bin/env bash
set -euo pipefail

BASELINE_SUMMARY="results/raw/baseline.summary.csv"
RL_SUMMARY="results/raw/rl.summary.csv"
BASELINE_CSV="results/raw/baseline.csv"
RL_CSV="results/raw/rl.csv"

mkdir -p results/raw

make all setup

cleanup() {
    pkill -f "sudo ./os_manager" 2>/dev/null || true
    pkill -f "python3 rl_agent.py" 2>/dev/null || true
}
trap cleanup EXIT

run_workloads() {

    echo "===== IDLE ====="
    sleep 60

    echo "===== CPU ====="
    stress-ng --cpu 6 --timeout 60s

    echo "===== MEMORY ====="
    stress-ng --vm 4 --vm-bytes 75% --timeout 60s

    echo "===== IO ====="
    fio \
      --name=randread \
      --filename=/tmp/testfile \
      --size=1G \
      --bs=4k \
      --rw=randread \
      --iodepth=32 \
      --runtime=60 \
      --time_based

    rm -f /tmp/testfile

    echo "===== MIXED ====="

    stress-ng --cpu 4 --timeout 60s &
    CPU_PID=$!

    stress-ng --vm 2 --vm-bytes 70% --timeout 60s &
    MEM_PID=$!

    wait $CPU_PID
    wait $MEM_PID

    echo "===== WORKLOADS COMPLETE ====="
}

reset_system() {
    echo "[reset] Restoring default settings"

    sudo cpupower frequency-set -g schedutil >/dev/null 2>&1 || true

    echo 60 | sudo tee /proc/sys/vm/swappiness >/dev/null

    if [[ -f /sys/block/sda/queue/scheduler ]]; then
        echo mq-deadline | sudo tee /sys/block/sda/queue/scheduler >/dev/null
    fi

    sleep 5
}

########################################
# BASELINE
########################################

echo ""
echo "=============================="
echo "BASELINE RUN"
echo "=============================="

reset_system

sudo env \
OSMGR_MODE=BASELINE \
OSMGR_CSV="$BASELINE_CSV" \
OSMGR_SUMMARY="$BASELINE_SUMMARY" \
./os_manager &
OS_PID=$!

sleep 3

run_workloads

sleep 5

kill "$OS_PID" 2>/dev/null || true
wait "$OS_PID" 2>/dev/null || true

########################################
# RL
########################################

echo ""
echo "=============================="
echo "RL RUN"
echo "=============================="

reset_system

python3 rl_agent.py &
RL_PID=$!

sleep 3

sudo env \
OSMGR_MODE=RL \
OSMGR_CSV="$RL_CSV" \
OSMGR_SUMMARY="$RL_SUMMARY" \
./os_manager &
OS_PID=$!

sleep 3

run_workloads

sleep 5

kill "$OS_PID" 2>/dev/null || true
kill "$RL_PID" 2>/dev/null || true

wait "$OS_PID" 2>/dev/null || true
wait "$RL_PID" 2>/dev/null || true

echo ""
echo "================================="
echo "COMPARISON COMPLETE"
echo "================================="
echo "Baseline summary: $BASELINE_SUMMARY"
echo "RL summary:       $RL_SUMMARY"
echo ""
echo "Run:"
echo "python3 analyze_results.py"