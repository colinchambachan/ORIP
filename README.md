# Overload-Resilient Ingestion Pipeline (ORIP)

> **Status (September 2026):** Step 1 (the overload-resilient ingestion engine) is complete. The original Step 2 (distributed online training) was **cut**. It was replaced by a small, time-boxed **inference-serving overload study** (dynamic batching and load shedding in a simulated model server), which is also complete. Results: [`out/RESULTS.md`](out/RESULTS.md) (Step 1) and [`out/RESULTS_SERVING.md`](out/RESULTS_SERVING.md) (serving study). Plan: [`docs/SERVING_PLAN.md`](docs/SERVING_PLAN.md).

## Project Outcome

**What was built:** A single-producer / single-consumer replay pipeline in C++20 with a lock-free bounded ring buffer and two overload policies:

- **Spin** (backpressure): the producer blocks until the queue has room, so every event is applied.
- **Coalesce** (latest-write-wins): when the queue is full, events go into a per-symbol pending map, and each newer event overwrites the older one.

**What the experiment showed** (full numbers in [`out/RESULTS.md`](out/RESULTS.md)): on a deterministic, bursty, hot-key-skewed trace, coalescing cuts consumer work by roughly 90–98% while keeping the latest per-symbol state identical to the lossless run. The cost is that per-key event counts become inexact. That is the tradeoff: freshness in exchange for completeness.

**Why Step 2 was cut:** Distributed online regression on market data doesn't serve my current focus on ML systems (inference, serving, partitioning). Step 1's real question (under overload, do you queue, drop, or coalesce?) is central to ML serving, though. So the project continues as a bounded study of dynamic batching and load shedding in a simulated inference server. See [`docs/SERVING_PLAN.md`](docs/SERVING_PLAN.md).

**Serving study (Phase 2):** `src/serve_main.cpp` replays the same trace as paced request arrivals into a simulated model server with a 50 ms SLO. Findings: dynamic batching is the biggest lever (about 13× capacity from batch 1 to 32). Past capacity, queueing everything collapses goodput. Rejecting at arrival, with a queue limit sized from the SLO, beats expiring stale requests at dequeue (about 30% more on-time requests). Build with `make`, run with `scripts/run_serving_experiments.sh`.

**Not delivered from the original PRD:** unit tests, queue-depth statistics, live WebSocket ingestion (the trace is synthetic but reproducible), and the LaTeX report (`out/RESULTS.md` covers the write-up instead).

---

## Product Requirements Document (original)

## 1. Background & Motivation

Real-time data systems often fail not because they are incorrect, but because they become **stale** when input rates exceed processing capacity. In many domains, processing every intermediate update is unnecessary; instead, systems must prioritize **freshness** and **bounded latency** under overload.

This project explores how to design a streaming ingestion system that remains operational and semantically correct when it cannot keep up with incoming data.

The system is demonstrated on **symbol-keyed crypto market data**, chosen for its publicly available live streams and clear event-time semantics. The design is domain-agnostic.

---

## 2. Goals

### Primary Goals

- Maintain **fresh per-key state** under bursty input
- Prevent unbounded queue growth
- Make overload behavior **explicit, measurable, and reproducible**
- Support deterministic replay for benchmarking

### Non-Goals

- Building a full streaming platform (Kafka, Flink, etc.)
- Providing trading or investment logic
- Guaranteeing lossless ingestion under overload

---

## 3. Definitions

- **Event**: A time-stamped update associated with a key (e.g., symbol)
- **Freshness**: How close the system’s state is to the latest known event time
- **Stale work**: Processing an event that has already been superseded by a newer event for the same key
- **Overload**: Input rate exceeds sustained processing capacity

---

# STEP 1: Overload-Resilient Ingestion Engine

## 4. Problem Statement (Step 1)

When input events arrive faster than they can be processed, naive systems accumulate backlog, leading to high latency and stale state. Step 1 focuses on eliminating **stale processing work** to keep state current.

---

## 5. Functional Requirements (Step 1)

### 5.1 Input

- Source:
  - Replay mode: recorded crypto WebSocket trade data (JSONL)
  - Live mode: public crypto WebSocket stream (future extension)
- Each event must include:
  - event timestamp
  - symbol identifier
  - payload (price, quantity)

### 5.2 Processing

- Events are ingested by a producer
- Events pass through a bounded buffer to a consumer
- Under overload, the system must:
  - drop or coalesce obsolete events
  - guarantee **latest-write-wins per key**

### 5.3 Overload Policy

- Default policy: **Coalesce Latest**
  - At most one pending event per key
  - Newer events replace older pending ones
- Policy must be explicit and configurable

### 5.4 Output

1. **Final State**
   - For each key: last timestamp, last value, count applied
2. **Run Metrics**
   - total events seen
   - events processed
   - coalesced/dropped count
   - throughput (events/sec)
   - queue depth statistics
   - latency percentiles (p50, p99)

---

## 6. Non-Functional Requirements (Step 1)

- Deterministic replay: same input + config → same output
- No unbounded memory growth
- Minimal allocations in hot paths
- Clear separation between ingestion, policy, and sink
- Reproducible benchmarks via CLI

---

## 7. Success Criteria (Step 1)

- System continues operating under synthetic burst loads
- State reflects the most recent event per key
- Latency remains bounded when overload occurs
- Metrics clearly show tradeoffs between throughput and freshness

---

# STEP 2: Distributed Real-Time Model Training — CUT

> **Not implemented.** This section is kept as the original design record. See [Project Outcome](#project-outcome) for why it was cut.

## 8. Problem Statement (Step 2)

Once ingestion is stable and freshness-preserving, the next challenge is **learning from streams continuously**. Centralized training does not scale well for high-rate or multi-key streams.

Step 2 builds on the ingestion engine to support **distributed online learning**.

---

## 9. Functional Requirements (Step 2)

### 9.1 Architecture Extension

`Ingestion Engine → Sharder → Trainer Workers → Aggregator → Model Store`

### 9.2 Sharding

- Events are routed to workers based on key (e.g., symbol hash)
- Each worker processes a disjoint subset of keys

### 9.3 Training

- Each worker maintains a local online model
- Model type (initial):
  - linear or logistic regression
- Updates occur incrementally per event

### 9.4 Aggregation

- Periodically aggregate model parameters:
  - simple averaging
  - configurable sync interval
- Redistribute aggregated parameters to workers

---

## 10. Outputs (Step 2)

- Model snapshots over time
- Training metrics:
  - loss vs time
  - convergence vs sync frequency
  - throughput vs model staleness
- End-to-end system metrics inherited from Step 1

---

## 11. Non-Goals (Step 2)

- Deep neural networks
- GPU acceleration
- Strong consistency guarantees
- Production-grade parameter servers

---

## 12. Success Criteria (Step 2)

- System trains continuously on live or replayed streams
- Training progresses despite bursty ingestion
- Clear empirical tradeoffs between sync frequency and convergence
- Step 1 ingestion guarantees remain intact

---

## 13. Deliverables

- C++ codebase with tests and benchmark harness
- Replay traces captured from live crypto streams
- LaTeX report documenting:
  - architecture
  - overload experiments
  - policy behavior
  - training results (Step 2)
- Reproducible CLI instructions

---

## 14. Long-Term Extensions (Out of Scope)

- Multiple overload policies
- Priority-based freshness
- Cross-machine deployment
- Integration with external stream processors

---

## 15. Reproducible Experiment Data

The replay trace used by the current overload experiments is generated locally with:

`scripts/generate_trade_data.py`

By default it writes:

`data/trades.jsonl`

Key parameters:

- `--events`: total number of JSONL trade events to generate
- `--burst-size`: approximate number of events per burst window
- `--seed`: deterministic random seed

The `seed` controls reproducibility. If you run the generator twice with the same:

- `--seed`
- `--events`
- `--burst-size`

you will get the same `data/trades.jsonl` contents. Changing the seed produces a different, but still reproducible, bursty trade trace.

The experiment harness:

`scripts/run_experiments.sh`

regenerates `data/trades.jsonl`, runs spin/coalesce replay across queue capacities `8`, `32`, and `1024`, and writes comparison results to:

- `out/experiments/*.csv`
- `out/experiments/*.json`
- `out/experiments/final_state_comparison.json`

## 16. Repo Layout

Current repo layout:

```text
.
├── Makefile
├── README.md
├── .gitignore
├── docs/
│   └── SERVING_PLAN.md
├── src/
│   ├── replay_main.cpp          # Step 1: ingestion engine (spin vs coalesce)
│   └── serve_main.cpp           # Phase 2: simulated inference server
├── scripts/
│   ├── generate_trade_data.py
│   ├── run_experiments.sh
│   ├── run_serving_experiments.sh
│   └── compare_final_states.py
├── data/
│   └── trades.jsonl
├── out/
│   ├── RESULTS.md
│   ├── RESULTS_SERVING.md
│   ├── serving/                 # per-run JSON + summary.md
│   ├── experiments/
│   │   ├── spin_q*.csv
│   │   ├── spin_q*.json
│   │   ├── coalesce_q*.csv
│   │   ├── coalesce_q*.json
│   │   ├── final_state_comparison.json
│   │   └── archive/
│   └── final_state*.csv
└── third_party/
    └── nlohmann/
        └── json.hpp
```
