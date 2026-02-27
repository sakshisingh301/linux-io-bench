#!/bin/bash
set -e

OUT="results/mmap_results.csv"
SEED=42

FILES=(
  "/mnt/bench/files/small_2G.bin"
  "/mnt/bench/files/large_64G.bin"
)

WORKLOADS=("seq_read" "rand_read" "mixed_rand")
BLOCKS=(4096 16384 65536 262144 1048576)
THREADS=(1 4 8)

mkdir -p results

for FILE in "${FILES[@]}"; do
  for W in "${WORKLOADS[@]}"; do
    for B in "${BLOCKS[@]}"; do
      for T in "${THREADS[@]}"; do
        for REP in 1 2 3 4 5; do

          echo "RUN file=$FILE workload=$W block=$B threads=$T rep=$REP (cold)"
          sync
          echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
          ./mmap/mmap_bench "$FILE" "$W" cold "$B" "$T" "$SEED" "$REP" "$OUT"

          echo "RUN file=$FILE workload=$W block=$B threads=$T rep=$REP (warm)"
          ./mmap/mmap_bench "$FILE" "$W" warm "$B" "$T" "$SEED" "$REP" "$OUT"

        done
      done
    done
  done
done

echo "DONE. Lines:"
wc -l "$OUT"
