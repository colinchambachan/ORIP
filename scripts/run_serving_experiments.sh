#!/usr/bin/env bash
# M4 sweep for the serving study (docs/SERVING_PLAN.md): load x batch size x policy.

set -euo pipefail

trace_path="${TRACE_PATH:-data/trades.jsonl}"
max_events="${MAX_EVENTS:-3000}"
slo_ms="${SLO_MS:-50}"
fixed_us="${FIXED_US:-2000}"
per_item_us="${PER_ITEM_US:-100}"
out_dir="out/serving"

make serve
mkdir -p "${out_dir}"

for rate in 1 2 4 8 16; do
  for batch in 1 8 32; do
    # Reject threshold sized from the SLO: how many requests can be served within slo_ms.
    batch_us=$(( fixed_us + per_item_us * batch ))
    limit=$(( slo_ms * 1000 / batch_us * batch ))
    for policy in queue reject expire; do
      queue_limit=4095
      if [ "${policy}" = "reject" ]; then queue_limit="${limit}"; fi
      ./serve \
        --input "${trace_path}" \
        --max-events "${max_events}" \
        --rate-scale "${rate}" \
        --fixed-us "${fixed_us}" \
        --per-item-us "${per_item_us}" \
        --max-batch "${batch}" \
        --slo-ms "${slo_ms}" \
        --policy "${policy}" \
        --queue-limit "${queue_limit}" \
        --metrics-out "${out_dir}/r${rate}_b${batch}_${policy}.json" > /dev/null
      echo "done rate=${rate} batch=${batch} policy=${policy}"
    done
  done
done

python3 - "${out_dir}" <<'EOF'
import glob, json, sys
out_dir = sys.argv[1]
rows = [json.load(open(p)) for p in glob.glob(f"{out_dir}/r*_b*_*.json")]
order = {"queue": 0, "reject": 1, "expire": 2}
rows.sort(key=lambda r: (r["rate_scale"], r["max_batch"], order[r["policy"]]))
lines = [
    "| Rate | Offered (req/s) | Max Batch | Policy | Queue Limit | On Time | Rejected | Expired | Goodput (req/s) | Mean Batch | p50 / p99 (ms) |",
    "| ---- | --------------- | --------- | ------ | ----------- | ------- | -------- | ------- | --------------- | ---------- | -------------- |",
]
for r in rows:
    lines.append(
        f"| {r['rate_scale']:g} | {r['offered_rps']:,.0f} | {r['max_batch']} | {r['policy']} | {r['queue_limit']} "
        f"| {r['on_time']:,} / {r['requests']:,} | {r['rejected']:,} | {r['expired']:,} | {r['goodput_rps']:,.0f} "
        f"| {r['mean_batch']:.1f} | {r['latency_p50_us']/1000:,.1f} / {r['latency_p99_us']/1000:,.1f} |"
    )
open(f"{out_dir}/summary.md", "w").write("\n".join(lines) + "\n")
print(f"wrote {out_dir}/summary.md")
EOF
