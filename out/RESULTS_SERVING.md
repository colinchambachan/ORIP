## Results: Inference-Serving Overload Study

Plan and milestones: [`docs/SERVING_PLAN.md`](../docs/SERVING_PLAN.md). Full sweep table: [`out/serving/summary.md`](serving/summary.md). Raw metrics: `out/serving/r{rate}_b{batch}_{policy}.json`.

### Setup

- **Simulator:** `src/serve_main.cpp`, one producer thread and one server thread connected by an SPSC ring.
- **Arrivals:** the first 3,000 requests of `data/trades.jsonl`, released at their `ts_ms` offsets divided by `--rate-scale`. The trace is bursty (1–3 ms gaps within a burst, ~150 ms quiet periods between bursts).
- **Model cost:** `2000 µs + 100 µs × batch_size` per batch. Batch-1 capacity is ~476 req/s, batch 8 ~2,857 req/s, batch 32 ~6,154 req/s.
- **SLO:** 50 ms. Latency is measured from *scheduled* arrival to batch completion, so a blocked producer can't hide queueing delay (no coordinated omission).
- **Policies:** `queue` (accept everything), `reject` (refuse at arrival when the queue is at its limit), `expire` (server drops requests already past their deadline at dequeue).
- **Reject limit:** sized from the SLO as `(slo / batch_time) × max_batch`, which gives 23, 136, and 288 for batch sizes 1, 8, and 32.
- **Goodput:** on-time completions ÷ wall-clock time.
- **Reproduce:** `scripts/run_serving_experiments.sh` (~2.5 min).

### Key rows

Batch size under load (rate 8, 3,443 req/s offered, `queue` policy):

| Max Batch | On Time | Mean Batch | p50 / p99 (ms) |
| --------- | ------- | ---------- | -------------- |
| 1         | 26 / 3,000    | 1.0  | 2,721 / 5,373 |
| 8         | 574 / 3,000   | 8.0  | 97.7 / 176.9  |
| 32        | 3,000 / 3,000 | 15.5 | 5.6 / 8.1     |

Policy once batching is saturated (max batch 8):

| Rate (Offered req/s) | Policy | On Time | Rejected | Expired | Goodput (req/s) | p99 (ms) |
| -------------------- | ------ | ------- | -------- | ------- | --------------- | -------- |
| 8 (3,443)            | queue  | 574 / 3,000   | 0     | 0     | 546   | 176.9 |
| 8 (3,443)            | reject | 2,485 / 3,000 | 375   | 0     | 2,699 | 50.3  |
| 8 (3,443)            | expire | 1,898 / 3,000 | 0     | 369   | 2,056 | 52.7  |
| 16 (6,886)           | queue  | 202 / 3,000   | 0     | 0     | 192   | 610.1 |
| 16 (6,886)           | reject | 1,022 / 3,000 | 1,623 | 0     | 2,113 | 50.4  |
| 16 (6,886)           | expire | 575 / 3,000   | 0     | 1,615 | 1,182 | 52.8  |

### Findings

- **Batching is the biggest lever.** Moving from batch 1 to batch 32 raises capacity about 13×. At rate 8, that takes the server from 26 on-time requests to all 3,000. Batch 1 collapses at rate 2; batch 8 holds up to rate 4; batch 32 holds up to rate 16. Get more capacity before deciding what to shed.
- **Greedy batching adapts on its own.** With no fill wait, mean batch size is 1.3 at light load and grows to 31 near saturation. Light-load latency stays at about one service time (3 ms). In M2, adding a 5 ms fill wait at light load *raised* p99 from 2.1 to 7.2 ms. Waiting to fill a batch only pays off when the server is already busy.
- **Past capacity, queueing everything is the worst policy.** The queue grows without bound, so nearly every request is late (rate 8, batch 1: 26/3,000 on time, p99 5.4 s). The server keeps working at full speed on requests that have already missed their deadline. This is congestion collapse: throughput stays high while goodput goes to nearly zero.
- **Rejecting at arrival beats expiring at dequeue.** Both shed about the same number of requests (375 vs 369 at rate 8, batch 8), but `reject` delivers ~30% more on-time requests (2,485 vs 1,898). A rejected request costs nothing. An expired one has already waited in the queue, and `expire` also serves requests that weren't late when dequeued but finish late. A predictive check (drop if `now + batch_time > deadline`) would close most of the gap. That's out of scope for this study.
- **Size the queue from the SLO, not from memory.** The Little's-law limit keeps `reject` p99 at about 50 ms at every load level. It's slightly over (50.3 ms), and a few accepted requests finish late, because the limit doesn't account for the batch already running. Subtracting one batch would fix that.
- **When the server isn't overloaded, the policy doesn't matter.** Rows at rates 2–8 with batch 32 are identical across policies. Shedding is free insurance: it only activates once the server can't meet the SLO anyway.
- **The same trade-off as Step 1.** Step 1's coalesce policy gave up exact counts to keep state fresh. Here, shedding gives up completeness (some requests are never served) to keep the requests that *are* served within their deadline.

### Caveats

- The model is simulated with a precise busy-wait, not real GPU work. Each run occupies about 2 CPU cores.
- Each configuration ran once, so some runs have scheduler noise. For example, rate 1 / batch 32 / `queue` shows p99 23.6 ms, while the identical-behaviour `reject` and `expire` runs show about 4.4 ms.
- Goodput divides by wall-clock time, and `queue` runs take longer to drain their backlog, so compare the on-time counts as well.
- Requests all have the same size. Variable-length requests, continuous batching, and KV-cache pressure are deliberately out of scope (see the plan).
