#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."

OUT="results/mmap_common_2g.csv"
SEED=42

FILE="/mnt/bench/files/small_2G.bin"

WORKLOADS=("seq_read" "rand_read" "mixed_rand")
BLOCKS=(4096 16384 65536 262144 1048576)
THREADS=(1 4 8)
REPS=(1 2 3 4 5)

mkdir -p results

# Start fresh each time (so it's clean and comparable)
echo "interface,workload,cache_state,file_size_bytes,block_size_bytes,threads,qd,seed,repetition,wall_seconds,total_bytes,throughput_MBps,latency_us_per_op,cpu_user_seconds,cpu_sys_seconds,Minor_faults,major_faults,voluntary_ctx,involuntary_ctx" > "$OUT"

for W in "${WORKLOADS[@]}"; do
  for B in "${BLOCKS[@]}"; do
    for T in "${THREADS[@]}"; do
      for REP in "${REPS[@]}"; do

        echo "RUN file=$FILE workload=$W block=$B threads=$T rep=$REP (cold)"
        sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
        ./mmap/mmap_bench "$FILE" "$W" cold "$B" "$T" "$SEED" "$REP" "$OUT"

        echo "RUN file=$FILE workload=$W block=$B threads=$T rep=$REP (warm)"
	dd if="$FILE" of=/dev/null bs=64M status=none
        ./mmap/mmap_bench "$FILE" "$W" warm "$B" "$T" "$SEED" "$REP" "$OUT"

      done
    done
  done
done

echo "DONE. Lines:"
wc -l "$OUT"
echo "CSV saved to: $OUT""
