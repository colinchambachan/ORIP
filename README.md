# Overload-Resilient Ingestion Pipeline + Distributed learning model

## Product Requirements Document

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

# STEP 2: Distributed Real-Time Model Training

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

Repo design
.
├── Makefile
├── SPEC.md
├── README.md
├── src/
│ ├── ingest/ # Step 1 (stable)
│ │ ├── replay.cpp
│ │ ├── ring.h
│ │ └── coalesce.h
│ ├── app/
│ │ └── replay_main.cpp
│ ├── train/ # Step 2 (later)
│ │ ├── trainer.cpp
│ │ ├── aggregator.cpp
│ │ └── model.h
│ └── common/
│ └── event.h
├── tests/
└── report/
└── paper.tex
