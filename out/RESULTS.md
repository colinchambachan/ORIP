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

| Mode     | Queue Cap | Queue-Full Count¹ | Pending Overwrites | Applied Total | Work Skipped | Drain Time (s)² | Latency p50 / p99 (µs)³ |
| -------- | --------- | ----------------- | ------------------ | ------------- | ------------ | --------------- | ----------------------- |
| Spin     | 8         | 9,795,629 retries | 0                  | 12,000        | 0%           | 0.9329          | 618 / 653               |
| Coalesce | 8         | 11,843 events     | 11,831             | 169           | 98.6%        | 0.0133          | 547 / 1,331             |
| Spin     | 32        | 9,748,374 retries | 0                  | 12,000        | 0%           | 0.9340          | 2,473 / 2,791           |
| Coalesce | 32        | 11,810 events     | 11,798             | 202           | 98.3%        | 0.0159          | 2,424 / 3,159           |
| Spin     | 1024      | 9,109,919 retries | 0                  | 12,000        | 0%           | 0.9317          | 79,435 / 80,217         |
| Coalesce | 1024      | 10,832 events     | 10,820             | 1,180         | 90.2%        | 0.0926          | 45,695 / 80,230         |

Numbers are from one run. Timing-dependent columns vary slightly between runs.

¹ **Not comparable across modes.** In spin mode, the producer retries in a loop while the queue is full, so the counter goes up once per retry. In coalesce mode it goes up once per event diverted to the pending map. (For coalesce, this also equals `pending_writes`.)

² Wall time to ingest and drain the whole trace (`elapsed_s`). This used to be reported as "Input Events/Sec" (parsed ÷ elapsed). That number is dropped because it made coalesce look 10–60× "faster". Coalesce finishes sooner mainly because it *skips* 90–98% of the work, not because it processes events faster.

³ Time from ingest (right after parsing) to the moment the consumer dequeues the event, over all applied events. For coalesced events, this is measured from the ingest of the *newest* value for that symbol, so it reflects state freshness.

Findings:

- **The trace triggers overload in a single pass.** With 12,000 bursty, hot-key-skewed events, the delayed consumer (about 78 µs per event) falls behind without the earlier `--loop 100000` workaround.
- **Spin is limited by the consumer.** Drain time stays about 0.93 s at every queue size. A bigger queue only holds more backlog; it doesn't add capacity.
- **In both modes, per-event latency is set by queue capacity, not the policy.** p99 is about queue cap × 78 µs: roughly 0.65 ms at q=8, 2.8 ms at q=32, and 80 ms at q=1024. This is Little's law: a full FIFO of N events, served at a fixed rate, makes every event wait about N service times. Coalescing doesn't shorten that wait, because the ring still fills with (stale) events, and the pending map is only drained once the ring is empty. So its pending-path p99 is about the same (1.4 ms, 3.3 ms, 81 ms).
- **Spin's latency numbers understate how stale it gets (coordinated omission).** A blocked spin producer stops reading input, so the backlog builds up in the input file where nothing timestamps it. The measured p99 looks bounded, but the last event is applied about 0.93 s after the run starts, compared with about 0.013 s for coalesce at q=8. Because replay isn't paced by `ts_ms`, "arrival time" isn't defined, and that staleness can't be measured correctly here. Fixing this is the first step of the serving follow-up (`docs/SERVING_PLAN.md`, M1: paced arrivals).
- **Coalesce's real win is reaching fresh state sooner, with far less work.** It skips 90–98% of the work and reaches a correct latest state 10–70× sooner. A small queue plus coalescing (q=8) gives both the shortest drain time and a sub-1.5 ms p99.
- **Latest-value correctness is preserved; exact counts are not.** Spin outputs are identical across queue sizes. Coalesce matches them on `last_ts_ms`, `last_price`, and `last_qty` for every symbol, but not on per-symbol counts (`final_state_comparison.json`: `latest_state_match=true`, `count_match=false` for all caps). Anything downstream that depends on exact per-key totals would be wrong under this policy.
