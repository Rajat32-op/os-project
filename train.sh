#!/bin/bash

echo "Training RL..."

for i in {1..5}; do
  echo "Run $i - CPU"
  stress-ng --cpu 6 --timeout 60s

  echo "Run $i - Memory"
  stress-ng --vm 2 --vm-bytes 80% --timeout 60s

  echo "Run $i - IO"
  fio --name=randrw --filename=/tmp/testfile --size=1G \
      --bs=4k --rw=randrw --iodepth=32 \
      --runtime=60 --time_based

done