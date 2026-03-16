#!/usr/bin/env bash
set -euo pipefail

EXE="$HOME/io-bench-syscall/bench_syscall"
OUTDIR="$HOME/io-bench-syscall/out"
OUT="$OUTDIR/results.csv"

FILE="/mnt/localdisk/io-bench/benchdata/test_2g.dat"
FILESIZE=$((2*1024*1024*1024))   # 2GB
SEED=42

mkdir -p "$OUTDIR"
mkdir -p "$(dirname "$FILE")"

if [[ ! -x "$EXE" ]]; then
  echo "ERROR: executable not found: $EXE"
  echo "Compile first: g++ -O2 -std=c++17 -pthread bench_syscall.cpp -o bench_syscall"
  exit 1
fi

sudo -v

rm -f "$OUT"

if [[ ! -f "$FILE" ]]; then
  echo "Creating test file: $FILE"
  "$EXE" --file "$FILE" --out "$OUT" --workload seq \
    --file_size "$FILESIZE" --block_size 4096 \
    --threads 1 --cache warm --seed "$SEED" --rep 0 --create

  if [[ -f "$OUT" ]]; then
    head -n 1 "$OUT" > "$OUTDIR/_tmp_header.csv"
    mv "$OUTDIR/_tmp_header.csv" "$OUT"
  fi
else
  echo "Test file exists: $FILE"
fi

echo "File size (ls):"
ls -lh "$FILE"
echo "File blocks on disk (du):"
du -h "$FILE" || true

WORKLOADS=("seq" "rand" "seqw" "randw")
BLOCKS=(4096 65536 1048576)
THREADS=(1 4 8)
CACHES=("warm" "cold")
REPS=(0 1 2)

for wl in "${WORKLOADS[@]}"; do
  for bs in "${BLOCKS[@]}"; do
    for th in "${THREADS[@]}"; do
      for cache in "${CACHES[@]}"; do
        for rep in "${REPS[@]}"; do

          if [[ "$cache" == "cold" ]]; then
            echo "DROP_CACHES before cold run..."
            sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
          else
            "$EXE" --file "$FILE" --out "$OUTDIR/_prime.csv" --workload seq \
              --file_size "$FILESIZE" --block_size 4096 \
              --threads 1 --cache warm --seed "$SEED" --rep 999 >/dev/null 2>&1 || true
            rm -f "$OUTDIR/_prime.csv"
          fi

          echo "RUN wl=$wl bs=$bs th=$th cache=$cache rep=$rep"
          "$EXE" --file "$FILE" --out "$OUT" --workload "$wl" \
            --file_size "$FILESIZE" --block_size "$bs" \
            --threads "$th" --cache "$cache" --seed "$SEED" --rep "$rep"

        done
      done
    done
  done
done

echo "DONE. Results: $OUT"
echo "CSV lines:"
wc -l "$OUT"
