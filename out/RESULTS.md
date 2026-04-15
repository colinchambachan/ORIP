## Results

### Spin vs Coalesce

The experiment now uses a generated but deterministic replay trace instead of the earlier 3-line smoke file.

- trace generator: `scripts/generate_trade_data.py`
- generated file: `data/trades.jsonl`
- generation settings: `--events 12000 --burst-size 200 --seed 7`
- symbol count: `12`
- hot-symbol skew: `SOLUSDT=3532`, `BTCUSDT=3528`, `ETHUSDT=3509`
- hot-symbol share: `10,569 / 12,000` events (`88.1%`)
- experiment runner: `scripts/run_experiments.sh`
- replay settings: `--loop 1`, `queue_cap = 8, 32, 1024`
- consumer delay re-enabled with `std::this_thread::sleep_for(std::chrono::microseconds(50));`

The relevant artifacts are:

- `out/experiments/spin_q8.json`
- `out/experiments/spin_q32.json`
- `out/experiments/spin_q1024.json`
- `out/experiments/coalesce_q8.json`
- `out/experiments/coalesce_q32.json`
- `out/experiments/coalesce_q1024.json`
- `out/experiments/final_state_comparison.json`

Older two-symbol overload artifacts from the earlier synthetic stress run were moved to `out/experiments/archive/` so they do not get confused with the current dataset-backed evaluation.

| Mode | Queue Cap | Queue Full Hits | Pending Writes | Pending Overwrites | Applied Total | Input Events/Sec | Elapsed (s) |
| ---- | --------- | --------------- | -------------- | ------------------ | ------------- | ---------------- | ----------- |
| Spin | 8 | 7,118,813 | 0 | 0 | 12,000 | 15,311 | 0.7837 |
| Coalesce | 8 | 11,812 | 11,812 | 11,800 | 200 | 899,062 | 0.0133 |
| Spin | 32 | 6,873,861 | 0 | 0 | 12,000 | 15,223 | 0.7883 |
| Coalesce | 32 | 11,797 | 11,797 | 11,785 | 215 | 837,928 | 0.0143 |
| Spin | 1024 | 6,744,629 | 0 | 0 | 12,000 | 15,313 | 0.7836 |
| Coalesce | 1024 | 10,808 | 10,808 | 10,796 | 1,204 | 148,347 | 0.0809 |

Findings:

- The richer generated trace is sufficient to trigger overload in a single pass. There is no need for the earlier `--loop 100000` workaround once the dataset contains thousands of bursty, skewed events.
- Spin mode remains bottlenecked by the delayed consumer at every queue size. Throughput stays almost flat at about `15.2k events/sec`, and increasing queue capacity from `8` to `1024` does not materially improve it.
- Coalesce mode collapses most stale work at every queue size. `applied_total` falls from `12,000` in spin mode to `200` at `q=8`, `215` at `q=32`, and `1,204` at `q=1024`.
- Relative to spin mode, coalescing reduces applied work by `98.3%` at `q=8`, `98.2%` at `q=32`, and `90.0%` at `q=1024`.
- Coalesce is much faster than spin under this trace. Throughput improves to `899k events/sec` at `q=8`, `838k` at `q=32`, and `148k` at `q=1024`, which corresponds to speedups of about `58.7x`, `55.0x`, and `9.7x`.
- Queue capacity now has a meaningful tradeoff inside the coalescing policy. Smaller queues force earlier coalescing and produce the most aggressive work reduction, while `q=1024` allows many more events to reach the consumer before being collapsed.
- The pending map is doing real work in every coalesce run. Almost every pending write is an overwrite, which confirms that the dataset is hot-symbol-heavy enough to exercise the policy rather than just the queue plumbing.
- Spin outputs are identical across all queue sizes, and coalesce preserves `last_ts_ms`, `last_price`, and `last_qty` for every symbol relative to the spin outputs.
- `out/experiments/final_state_comparison.json` records that invariant directly. For `q8`, `q32`, and `q1024`, it reports `latest_state_match=true`, `count_match=false`, and `full_row_match=false`.
- Under overload, coalescing preserves latest-value correctness but intentionally sacrifices exact per-key event counts.
- Coalesce still does not preserve exact event counts. It preserves latest-value freshness, but any downstream metric that depends on exact per-symbol totals will diverge under overload.
