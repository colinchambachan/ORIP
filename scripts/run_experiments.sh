#!/usr/bin/env bash

set -euo pipefail

trace_path="${TRACE_PATH:-data/trades.jsonl}"
loop_count="${LOOP_COUNT:-1}"
events="${TRACE_EVENTS:-12000}"
burst_size="${TRACE_BURST_SIZE:-200}"
seed="${TRACE_SEED:-7}"

python3 scripts/generate_trade_data.py \
  --output "${trace_path}" \
  --events "${events}" \
  --burst-size "${burst_size}" \
  --seed "${seed}"

mkdir -p out/experiments

for cap in 8 32 1024; do
  for mode in spin coalesce; do
    ./replay \
      --input "${trace_path}" \
      --out out/experiments/${mode}_q${cap}.csv \
      --loop "${loop_count}" \
      --mode "${mode}" \
      --queue-cap "${cap}" \
      --metrics-out out/experiments/${mode}_q${cap}.json
  done
done

python3 scripts/compare_final_states.py \
  --input-dir out/experiments \
  --output out/experiments/final_state_comparison.json
