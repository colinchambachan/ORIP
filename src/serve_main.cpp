// Phase 2: simulated inference server under bursty load.
// See docs/SERVING_PLAN.md. Step 1's replay_main.cpp is left untouched as the baseline.
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Request
{
  long long id;
  // When the request *should* arrive (start + scaled trace offset), not when the producer got
  // around to pushing it. Measuring from here avoids coordinated omission: if the producer is
  // blocked on a full queue, that waiting still counts against the request.
  Clock::time_point arrival;
  Clock::time_point deadline; // arrival + SLO
};

// What to do when the server can't keep up.
enum class Policy
{
  Queue,  // accept everything; producer blocks when the queue is at its limit
  Reject, // refuse new requests at arrival when the queue is at its limit
  Expire  // accept everything, but the server discards requests already past their deadline
};

// Same SPSC ring as replay_main.cpp (one slot left unused to tell full from empty).
template <typename T, size_t CAP>
struct Ring
{
  static_assert(CAP >= 2, "CAP must be at least 2");
  T buf[CAP];
  std::atomic<size_t> head{0};
  std::atomic<size_t> tail{0};

  bool push(const T &val)
  {
    size_t h = head.load(std::memory_order_relaxed);
    size_t next = (h + 1) % CAP;
    if (next == tail.load(std::memory_order_acquire))
      return false; // queue is full
    buf[h] = val;
    head.store(next, std::memory_order_release);
    return true;
  }

  // Approximate depth. Exact when called from the producer, since only it moves head.
  size_t size() const
  {
    size_t h = head.load(std::memory_order_acquire);
    size_t t = tail.load(std::memory_order_acquire);
    return (h + CAP - t) % CAP;
  }

  bool pop(T &val)
  {
    size_t t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire))
      return false; // queue is empty
    val = buf[t];
    tail.store((t + 1) % CAP, std::memory_order_release);
    return true;
  }
};

struct Config
{
  std::string input;
  std::string metrics_out;
  double rate_scale = 1.0; // >1 compresses trace time (more load), <1 stretches it
  long long max_events = 0; // 0 = whole trace
  long long fixed_us = 2000;  // per-batch model cost
  long long per_item_us = 100; // extra cost per request in the batch
  size_t max_batch = 1;         // 1 = no batching (M1 behaviour)
  long long max_wait_us = 0;    // how long to hold a partial batch hoping it fills
  long long slo_ms = 50;
  Policy policy = Policy::Queue;
  size_t queue_limit = 4095; // producer treats the queue as full at this depth (ring max is 4095)
};

// sleep_for overshoots by ~40% on macOS, which would skew every latency number.
// Sleep coarsely while far from the deadline, then yield-spin for the last few ms.
static void wait_until(Clock::time_point tp)
{
  while (true)
  {
    auto now = Clock::now();
    if (now >= tp)
      return;
    if (tp - now > std::chrono::milliseconds(3))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else
      std::this_thread::yield();
  }
}

// Nearest-rank percentile. Takes a copy so it can sort without touching the caller's data.
static long long percentile(std::vector<long long> v, double p)
{
  if (v.empty())
    return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(p * (v.size() - 1))];
}

static long long to_us(Clock::duration d)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

// Read only the trace timestamps; symbol/price/qty don't matter to the server.
static bool load_arrivals(const Config &cfg, std::vector<long long> &offsets_us)
{
  std::ifstream in(cfg.input);
  if (!in.is_open())
  {
    std::cerr << "Failed to open: " << cfg.input << "\n";
    return false;
  }
  std::string line;
  long long first_ts_ms = -1;
  while (std::getline(in, line))
  {
    if (line.empty())
      continue;
    long long ts_ms = json::parse(line).at("ts_ms").get<long long>();
    if (first_ts_ms < 0)
      first_ts_ms = ts_ms;
    offsets_us.push_back(static_cast<long long>((ts_ms - first_ts_ms) * 1000 / cfg.rate_scale));
    if (cfg.max_events > 0 && static_cast<long long>(offsets_us.size()) >= cfg.max_events)
      break;
  }
  return true;
}

static int run_serve(const Config &cfg)
{
  std::vector<long long> offsets_us;
  if (!load_arrivals(cfg, offsets_us))
    return 1;

  Ring<Request, 4096> q;
  std::atomic<bool> done{false};
  long long queue_full_hits = 0;
  long long rejected = 0; // producer-only
  long long expired = 0;  // server-only
  long long on_time = 0;  // server-only

  std::vector<long long> latency_us;
  latency_us.reserve(offsets_us.size());
  long long batches = 0;
  long long busy_us = 0;

  auto start = Clock::now();

  // Producer: releases each request at its scheduled arrival time.
  std::thread producer([&]
                       {
    for (size_t i = 0; i < offsets_us.size(); i++) {
      auto arrival = start + std::chrono::microseconds(offsets_us[i]);
      Request r{static_cast<long long>(i), arrival, arrival + std::chrono::milliseconds(cfg.slo_ms)};
      wait_until(r.arrival);
      if (q.size() >= cfg.queue_limit) {
        queue_full_hits++;
        if (cfg.policy == Policy::Reject) { rejected++; continue; }
        // Queue / Expire: block until there is room (the spin policy from Step 1).
        while (q.size() >= cfg.queue_limit) std::this_thread::yield();
      }
      q.push(r);
    }
    done.store(true, std::memory_order_release); });

  // Server: dynamic batching. Take the first waiting request, then keep taking more until the
  // batch is full or max_wait_us has passed since the first one, then "run the model" once.
  std::thread server([&]
                     {
    std::vector<Request> batch;
    batch.reserve(cfg.max_batch);
    Request r;
    // Under Expire, a request already past its deadline is dropped instead of wasting model time.
    auto stale = [&](const Request &req) {
      if (cfg.policy != Policy::Expire || Clock::now() < req.deadline) return false;
      expired++;
      return true;
    };
    while (true) {
      if (!q.pop(r)) {
        if (!done.load(std::memory_order_acquire)) { std::this_thread::yield(); continue; }
        // Producer is done, but it may have pushed one last request after our failed pop.
        if (!q.pop(r)) break;
      }
      if (stale(r)) continue;
      batch.clear();
      batch.push_back(r);

      auto fill_deadline = Clock::now() + std::chrono::microseconds(cfg.max_wait_us);
      while (batch.size() < cfg.max_batch) {
        if (q.pop(r)) { if (!stale(r)) batch.push_back(r); continue; }
        if (Clock::now() >= fill_deadline || done.load(std::memory_order_acquire)) break;
        std::this_thread::yield();
      }

      // Big fixed cost + small per-item cost: the shape that makes batching pay off on GPUs.
      auto t0 = Clock::now();
      long long n = static_cast<long long>(batch.size());
      wait_until(t0 + std::chrono::microseconds(cfg.fixed_us + cfg.per_item_us * n));
      auto t1 = Clock::now();
      busy_us += to_us(t1 - t0);
      batches++;
      for (const Request &b : batch) {
        latency_us.push_back(to_us(t1 - b.arrival));
        if (t1 <= b.deadline) on_time++;
      }
    } });

  producer.join();
  server.join();

  double elapsed_s = std::chrono::duration<double>(Clock::now() - start).count();
  double trace_s = offsets_us.empty() ? 0.0 : offsets_us.back() / 1e6;
  long long completed = static_cast<long long>(latency_us.size());
  long long p50 = percentile(latency_us, 0.50);
  long long p99 = percentile(latency_us, 0.99);
  long long max = percentile(latency_us, 1.0);
  double offered_rps = trace_s > 0 ? offsets_us.size() / trace_s : 0.0;
  double capacity_rps = 1e6 / (cfg.fixed_us + cfg.per_item_us);
  // Goodput: requests that finished within their SLO, per second of wall time.
  double goodput_rps = elapsed_s > 0 ? on_time / elapsed_s : 0.0;
  const char *policy_name = cfg.policy == Policy::Queue ? "queue" : cfg.policy == Policy::Reject ? "reject" : "expire";

  std::cout << "rate_scale=" << cfg.rate_scale << " requests=" << offsets_us.size()
            << " trace_s=" << trace_s << " elapsed_s=" << elapsed_s << "\n";
  std::cout << "offered_rps=" << offered_rps << " capacity_rps(batch=1)=" << capacity_rps << "\n";
  double mean_batch = batches ? static_cast<double>(completed) / batches : 0.0;
  std::cout << "max_batch=" << cfg.max_batch << " max_wait_us=" << cfg.max_wait_us
            << " mean_batch=" << mean_batch << "\n";
  std::cout << "completed=" << completed << " batches=" << batches
            << " mean_service_us=" << (batches ? busy_us / batches : 0) << "\n";
  std::cout << "latency_us p50=" << p50 << " p99=" << p99 << " max=" << max << "\n";
  std::cout << "policy=" << policy_name << " slo_ms=" << cfg.slo_ms << " queue_limit=" << cfg.queue_limit
            << " rejected=" << rejected << " expired=" << expired << "\n";
  std::cout << "on_time=" << on_time << "/" << offsets_us.size() << " goodput_rps=" << goodput_rps << "\n";
  std::cout << "queue_full_hits=" << queue_full_hits << "\n";

  if (!cfg.metrics_out.empty())
  {
    std::ofstream mout(cfg.metrics_out);
    if (!mout.is_open())
    {
      std::cerr << "Failed to write metrics: " << cfg.metrics_out << "\n";
      return 1;
    }
    mout << "{\n";
    mout << "  \"policy\": \"" << policy_name << "\",\n";
    mout << "  \"slo_ms\": " << cfg.slo_ms << ",\n";
    mout << "  \"queue_limit\": " << cfg.queue_limit << ",\n";
    mout << "  \"rate_scale\": " << cfg.rate_scale << ",\n";
    mout << "  \"requests\": " << offsets_us.size() << ",\n";
    mout << "  \"fixed_us\": " << cfg.fixed_us << ",\n";
    mout << "  \"per_item_us\": " << cfg.per_item_us << ",\n";
    mout << "  \"offered_rps\": " << offered_rps << ",\n";
    mout << "  \"completed\": " << completed << ",\n";
    mout << "  \"max_batch\": " << cfg.max_batch << ",\n";
    mout << "  \"max_wait_us\": " << cfg.max_wait_us << ",\n";
    mout << "  \"batches\": " << batches << ",\n";
    mout << "  \"mean_batch\": " << mean_batch << ",\n";
    mout << "  \"elapsed_s\": " << elapsed_s << ",\n";
    mout << "  \"latency_p50_us\": " << p50 << ",\n";
    mout << "  \"latency_p99_us\": " << p99 << ",\n";
    mout << "  \"latency_max_us\": " << max << ",\n";
    mout << "  \"rejected\": " << rejected << ",\n";
    mout << "  \"expired\": " << expired << ",\n";
    mout << "  \"on_time\": " << on_time << ",\n";
    mout << "  \"goodput_rps\": " << goodput_rps << ",\n";
    mout << "  \"queue_full_hits\": " << queue_full_hits << "\n";
    mout << "}\n";
  }
  return 0;
}

static void usage(const char *prog)
{
  std::cerr << "Usage: " << prog
            << " --input <trace.jsonl> [--rate-scale X] [--max-events N]"
               " [--fixed-us N] [--per-item-us N] [--max-batch N] [--max-wait-us N]"
               " [--slo-ms N] [--policy queue|reject|expire] [--queue-limit N]"
               " [--metrics-out <metrics.json>]\n";
}

int main(int argc, char **argv)
{
  Config cfg;
  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    auto next = [&]() -> std::string
    {
      if (i + 1 >= argc)
      {
        usage(argv[0]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--input")
      cfg.input = next();
    else if (a == "--metrics-out")
      cfg.metrics_out = next();
    else if (a == "--rate-scale")
      cfg.rate_scale = std::stod(next());
    else if (a == "--max-events")
      cfg.max_events = std::stoll(next());
    else if (a == "--fixed-us")
      cfg.fixed_us = std::stoll(next());
    else if (a == "--per-item-us")
      cfg.per_item_us = std::stoll(next());
    else if (a == "--max-batch")
      cfg.max_batch = std::stoul(next());
    else if (a == "--max-wait-us")
      cfg.max_wait_us = std::stoll(next());
    else if (a == "--slo-ms")
      cfg.slo_ms = std::stoll(next());
    else if (a == "--queue-limit")
      cfg.queue_limit = std::stoul(next());
    else if (a == "--policy")
    {
      std::string p = next();
      if (p == "queue")
        cfg.policy = Policy::Queue;
      else if (p == "reject")
        cfg.policy = Policy::Reject;
      else if (p == "expire")
        cfg.policy = Policy::Expire;
      else
      {
        usage(argv[0]);
        return 2;
      }
    }
    else
    {
      usage(argv[0]);
      return 2;
    }
  }
  if (cfg.input.empty() || cfg.rate_scale <= 0 || cfg.max_batch < 1 || cfg.queue_limit < 1 || cfg.queue_limit > 4095)
  {
    usage(argv[0]);
    return 2;
  }
  return run_serve(cfg);
}
