mkdir -p out/experiments

for cap in 8 32 1024; do
  for mode in spin coalesce; do
    ./replay \
      --input data/trades.jsonl \
      --out out/experiments/${mode}_q${cap}.csv \
      --loop 1000 \
      --mode ${mode} \
      --queue-cap ${cap} \
      --metrics-out out/experiments/${mode}_q${cap}.json
  done
done