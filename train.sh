#!/bin/bash

echo "===== RL TRAINING START ====="

ROUNDS=20

for ((r=1; r<=ROUNDS; r++))
do
    echo ""
    echo "========================="
    echo "ROUND $r"
    echo "========================="

    #######################################
    # IDLE
    #######################################
    echo "[TRAIN] Idle workload"
    sleep 30

    #######################################
    # CPU BOUND
    #######################################
    echo "[TRAIN] CPU workload"

    stress-ng \
        --cpu 8 \
        --cpu-method matrixprod \
        --timeout 60s

    sleep 10

    #######################################
    # MEMORY BOUND
    #######################################
    echo "[TRAIN] Memory workload"

    stress-ng \
        --vm 4 \
        --vm-bytes 75% \
        --timeout 60s

    sleep 10

    #######################################
    # CACHE/MEMORY PRESSURE
    #######################################
    echo "[TRAIN] Cache pressure"

    stress-ng \
        --stream 4 \
        --timeout 60s

    sleep 10

    #######################################
    # CONTEXT SWITCH
    #######################################
    echo "[TRAIN] Context switch workload"

    stress-ng \
        --switch 8 \
        --timeout 60s

    sleep 10

    #######################################
    # IO WRITE
    #######################################
    echo "[TRAIN] Sequential write"

    fio \
        --name=seqwrite \
        --rw=write \
        --bs=1M \
        --size=1G \
        --direct=1 \
        --filename=/tmp/fio_train.bin \
        --runtime=60 \
        --time_based

    rm -f /tmp/fio_train.bin

    sleep 10

    #######################################
    # IO RANDOM
    #######################################
    echo "[TRAIN] Random IO"

    fio \
        --name=randrw \
        --rw=randrw \
        --rwmixread=70 \
        --bs=4k \
        --size=1G \
        --direct=1 \
        --filename=/tmp/fio_train.bin \
        --runtime=60 \
        --time_based

    rm -f /tmp/fio_train.bin

    sleep 10

    #######################################
    # MIXED CPU + MEMORY
    #######################################
    echo "[TRAIN] Mixed CPU+Memory"

    stress-ng \
        --cpu 4 \
        --vm 2 \
        --vm-bytes 50% \
        --timeout 60s

    sleep 10

    #######################################
    # MIXED CPU + IO
    #######################################
    echo "[TRAIN] Mixed CPU+IO"

    stress-ng --cpu 4 --timeout 60s &
    CPU_PID=$!

    fio \
        --name=mixed \
        --rw=randrw \
        --bs=16k \
        --size=1G \
        --direct=1 \
        --filename=/tmp/fio_train.bin \
        --runtime=60 \
        --time_based

    wait $CPU_PID
    rm -f /tmp/fio_train.bin

    sleep 10

done

echo "===== TRAINING COMPLETE ====="