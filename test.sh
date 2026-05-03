#!/bin/bash

MODE=$1   # baseline or rl

echo "Running tests in $MODE mode..."

run_test () {
  NAME=$1
  CMD=$2

  echo "Running $NAME..."
  $CMD &
  PID=$!

  sleep 70   # let it run + buffer

  wait $PID
}

# CPU
run_test "cpu" "stress-ng --cpu 6 --timeout 60s"

# IO
run_test "io" "fio --name=randread --filename=/tmp/testfile --size=1G \
  --bs=4k --rw=randread --iodepth=32 --runtime=60 --time_based"

# Mixed
run_test "mixed" "stress-ng --cpu 4 --timeout 60s & \
                 stress-ng --vm 2 --vm-bytes 70% --timeout 60s"