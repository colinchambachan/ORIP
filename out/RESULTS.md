## Results

### Spin vs Coalesce

The first sweep in `out/experiments` was too light to show meaningful coalescing behavior, so the experiment was rerun under controlled overload across all queue capacities:

- `queue_cap = 8, 32, 1024`
- consumer delay re-enabled with `std::this_thread::sleep_for(std::chrono::microseconds(50));`
- `--loop 100000`
- input file: `data/trades.jsonl` (3 events per loop, 300,000 parsed events total)

The relevant artifacts are:

- `out/experiments/spin_q8_overload.json`
- `out/experiments/spin_q32_overload.json`
- `out/experiments/spin_q1024_overload.json`
- `out/experiments/coalesce_q8_overload.json`
- `out/experiments/coalesce_q32_overload.json`
- `out/experiments/coalesce_q1024_overload.json`
- `out/experiments/spin_q8_overload.csv`
- `out/experiments/spin_q32_overload.csv`
- `out/experiments/spin_q1024_overload.csv`
- `out/experiments/coalesce_q8_overload.csv`
- `out/experiments/coalesce_q32_overload.csv`
- `out/experiments/coalesce_q1024_overload.csv`

| Mode | Queue Cap | Queue Full Hits | Pending Writes | Pending Overwrites | Applied Total | Events/Sec | Elapsed (s) |
| ---- | --------- | --------------- | -------------- | ------------------ | ------------- | ---------- | ----------- |
| Spin | 8 | 171,238,233 | 0 | 0 | 300,000 | 15,184 | 19.7577 |
| Coalesce | 8 | 293,017 | 293,017 | 292,999 | 7,001 | 492,048 | 0.6097 |
| Spin | 32 | 161,010,307 | 0 | 0 | 300,000 | 15,187 | 19.7536 |
| Coalesce | 32 | 293,548 | 293,548 | 293,546 | 6,454 | 704,442 | 0.4259 |
| Spin | 1024 | 166,899,600 | 0 | 0 | 300,000 | 15,196 | 19.7414 |
| Coalesce | 1024 | 292,555 | 292,555 | 292,553 | 7,447 | 609,281 | 0.4924 |

Findings:

- The controlled overload rerun clearly demonstrates coalescing behavior at every queue size. `applied_total` dropped from `300,000` in every spin run to `7,001` at `q=8`, `6,454` at `q=32`, and `7,447` at `q=1024`, which is roughly a `97.5%` to `97.8%` reduction in applied work.
- Coalesce mode was much faster under overload for every queue size. Throughput improved from about `15.2k events/sec` in spin mode to `492k` at `q=8`, `704k` at `q=32`, and `609k` at `q=1024`, corresponding to speedups of about `32.4x`, `46.4x`, and `40.1x`.
- The pending map was heavily exercised in every coalesce run. Nearly all pending writes were overwrites: `292,999` at `q=8`, `293,546` at `q=32`, and `292,553` at `q=1024`. This is the exact pattern needed to collapse stale work.
- Spin mode spent substantial time stalled on a full queue at every capacity, with `161M` to `171M` queue-full hits. Under this controlled overload, increasing queue capacity alone did not solve the backlog problem.
- Spin outputs are identical across all three queue sizes. The final CSV always reports `BTCUSDT=200000` and `ETHUSDT=100000` in the `count` column, with the same latest timestamp and price state.
- Coalesce preserved the latest state for each symbol at all queue sizes: `last_ts_ms`, `last_price`, and `last_qty` match the spin outputs in every overload CSV.
- Coalesce did not preserve exact event counts. The coalesced `count` values vary by queue size: `BTCUSDT/ETHUSDT = 5494/1507` at `q=8`, `5090/1364` at `q=32`, and `5726/1721` at `q=1024`.
- That means the current coalescing policy is correct for freshness of the latest value, but not for count-based accounting. Any downstream metric that depends on exact event totals is not preserved under overload coalescing.
