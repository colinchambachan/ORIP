# ORIP Phase 2: Inference-Serving Overload Study

Replaces the cut "distributed training" Step 2. Same question as Step 1 (queue, drop, or coalesce under overload?), asked about an **ML inference server** instead of a market-data consumer.

## The question

A simulated model server receives bursty requests. Each request has a latency deadline (SLO). When requests arrive faster than the server can handle them:

1. How much does **dynamic batching** help, and what does waiting to fill a batch cost?
2. When batching isn't enough, does **shedding load** (rejecting early, or dropping requests already past their deadline) beat queueing everything?

Headline metric: **goodput**, the number of requests completed *within their SLO* per second. Raw throughput can look great while every request is late.

## The model (kept deliberately simple)

- **Arrivals:** reuse `data/trades.jsonl`. One line is one request, and `ts_ms` is its arrival time. The trace already alternates bursts (1–3 ms gaps) with quiet periods (~150 ms). A `--rate-scale` flag compresses time to raise the load.
- **Server:** the consumer thread pulls up to `max_batch` requests, waits at most `max_wait_us` for the batch to fill, then "runs the model" by sleeping for `fixed_us + per_item_us * batch_size`. This shape (a big fixed cost, a small marginal cost) is why batching pays off on GPUs.
- **Suggested defaults:** `fixed_us = 2000`, `per_item_us = 100`, `slo_ms = 50`. With a batch size of 1, capacity is about 475 req/s, which is below the burst rate, so the server is overloaded even at `--rate-scale 1`. With a batch size of 32, capacity is about 6,000 req/s.

## Milestones

Each milestone is one commit, works on its own, and ends with a check you can run. If you stop after any of them, what's done still stands.

| # | Milestone | Done when |
|---|-----------|-----------|
| M0 | **Close out Step 1**: finish the latency percentiles in `replay_main.cpp`, rerun, rewrite `out/RESULTS.md`, tag `v1.0`. | Spin vs. coalesce p50/p99 are in RESULTS.md. |
| M1 | **Paced arrivals.** Create `src/serve_main.cpp` with a new `make serve` target. The producer sleeps until each request's scaled arrival time before pushing it. The server handles one request at a time and sleeps for the fixed service cost. Report p50/p99 latency. | At `--rate-scale 0.25` (light load), p50 ≈ service time. At 1.0, p99 grows during bursts. Use `--max-events 3000` to keep runs short. |
| M2 | **Dynamic batching.** Add `--max-batch` and `--max-wait-us`. Report the mean batch size. | Mean batch size grows with load, and p99 at `--rate-scale 1` drops a lot compared with M1. |
| M3 | **Deadlines and shedding.** Give each request `deadline = arrival + slo`. Add `--policy`: `queue` (accept everything), `reject` (refuse at arrival when the queue is full, like the drop analog), `expire` (discard at dequeue if already past the deadline). Report goodput, rejected, and expired counts. | At high load, `expire` or `reject` has higher goodput than `queue`. |
| M4 | **Experiment and write-up.** Script a sweep over rate-scale × policy × max-batch, then write `out/RESULTS_SERVING.md`: one table plus findings. | The write-up answers the two questions above. **Stop here.** |

## Guardrails (to avoid losing track)

- **One milestone at a time.** Don't start M(n+1) until M(n)'s "done when" check passes and is committed.
- **Leave Step 1 alone.** `replay_main.cpp` stays as it is, as the baseline. New work goes in `serve_main.cpp`. Copying `Ring` into it is fine; do shared headers only if the copying gets annoying.
- **Time box: about 2 weekends total.** If you hit the limit, stop at the last finished milestone and write up what you have.
- **Out of scope**, even though it's tempting: real models or GPUs, variable-length requests or continuous batching, KV-cache modeling, multiple servers or routing, networking. Each would be a separate project. Note it as future work and move on.

## Progress

- [x] M0 Close out Step 1 (tag `v1.0`). Finding: latency is set by queue capacity, and unpaced replay hides spin's real staleness (coordinated omission). M1 fixes that.
- [x] M1 Paced arrivals. With 3000 requests, batch=1 (~476 req/s capacity): rate 0.25 → p50/p99 2.1/2.1 ms; rate 1 → 48/107 ms; rate 2 → 1.4 s/2.8 s (backlog grows without bound).
- [x] M2 Dynamic batching. At rate 1, max_batch 32 cuts p99 from 107 ms to 4.3 ms. Mean batch size grows with load (1.0 → 6.2 from rate 0.25 to 4). At light load, `--max-wait-us 5000` raises p99 from 2.1 ms to 7.2 ms, so waiting to fill a batch only pays off under load.
- [x] M3 Deadlines and shedding. At rate 8, batch 8, SLO 50 ms: queue 572/3000 on time (goodput 544 req/s); reject with limit 64 2553/3000 (2851 req/s, at capacity); expire 1899/3000 (2057 req/s). Expire-at-dequeue still spends model time on requests that finish late.
- [x] M4 Experiment and write-up: `scripts/run_serving_experiments.sh` → `out/RESULTS_SERVING.md`. **Study complete.**
