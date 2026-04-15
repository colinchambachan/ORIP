#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <iomanip>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>

using json = nlohmann::json;

struct Latest
{
  long long last_ts_ms = 0;
  double last_price = 0.0;
  double last_qty = 0.0;
  long long count = 0;
};

struct Event
{
  long long ts_ms;
  std::string symbol;
  double price;
  double qty;
};

// Single produce single consumer queue. thread safe

// !We intentionally leave 1 slot unused.
// Full condition is (next_head == tail), so max usable capacity is CAP - 1.
// This allows us to distinguish full vs empty without extra state.
template <typename T, size_t CAP>
struct Ring
{
  static_assert(CAP >= 2, "CAP must be at least 2");
  T buf[CAP];
  std::atomic<size_t> head{0};
  std::atomic<size_t> tail{0};

  bool push(const T &val)
  {

    /*
    relaxed: atomic, but no ordering guarantees
    acquire: when read, must also see earlier writes that were published before
    release: publish my earlier writes before making this visible
    */

    // part of atomic operations
    size_t h = head.load(std::memory_order_relaxed);
    size_t next = (h + 1) % CAP;
    size_t t = tail.load(std::memory_order_acquire);

    if (next == t)
      return false; // queue is full

    buf[h] = val;
    head.store(next, std::memory_order_release);
    return true;
  }

  bool pop(T &val)
  {
    size_t t = tail.load(std::memory_order_relaxed);
    size_t h = head.load(std::memory_order_acquire);

    if (t == h)
      return false; // queue is empty

    val = buf[t];
    size_t next = (t + 1) % CAP;
    tail.store(next, std::memory_order_release);

    return true;
  }

  bool empty() const
  {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
  };
};

static void usage(const char *prog)
{
  std::cerr << "Usage: " << prog
            << " --input <path> [--out <final_state.csv>] [--loop N] [--mode spin|coalesce] [--queue-cap 8|32|1024] [--metrics-out <metrics.json>]\n";
}

// Forward declarations
static bool parse_trade_line(const std::string &line, Event &e);
static inline void apply_event(std::unordered_map<std::string, Latest> &latest, const Event &e);

template <size_t QCAP>
static int run_replay(const std::string &input, const std::string &out, int loop, bool spin_on_full, const std::string &metrics_out)
{
  std::ifstream in(input);
  if (!in.is_open())
  {
    std::cerr << "Failed to open: " << input << "\n";
    return 1;
  }

  std::unordered_map<std::string, Latest> latest;
  // Ring<Event, 1024> q;
  Ring<Event, QCAP> q;
  std::unordered_map<std::string, Event> pending;
  std::mutex pending_mu;

  std::atomic<bool> done{false};
  long long lines = 0, parsed = 0, bad = 0;
  long long full_count = 0;

  long long pending_writes = 0;
  long long pending_drains = 0;
  long long applied_from_queue = 0;
  long long applied_from_pending = 0;
  long long pending_inserts = 0;
  long long pending_overwrites = 0;

  auto t0 = std::chrono::steady_clock::now();

  auto pending_empty = [&]()
  {
    std::lock_guard<std::mutex> lock(pending_mu);
    return pending.empty();
  };

  std::thread producer([&]
                       {
    std::string line_local;
    for (int pass = 0; pass < loop; pass++) {
      // clear and reset the input stream
      in.clear();
      // move 'cursor' to the beginning of the file
      in.seekg(0, std::ios::beg);

      while (std::getline(in, line_local)) {
        // count the number of lines in the file
        lines++;
        if (line_local.empty()) continue;

        // parse the line into an Event
        Event e;
        if (!parse_trade_line(line_local, e)) { bad++; continue; }
        parsed++;

        // yield, waste cpu and wait util available
        // use coalescing to change update producer behaviour 
        if (spin_on_full) {
          while (!q.push(e)) {
            full_count++;
            std::this_thread::yield();
          }
        } else {
          if (!q.push(e)) {
            full_count++;
            std::lock_guard<std::mutex> lock(pending_mu);
            auto it = pending.find(e.symbol);
            if (it == pending.end()) {
              pending_inserts++;
            } else {
              pending_overwrites++;
            }
            pending[e.symbol] = e;
            pending_writes++;
          }
        }
      }
    }
    done.store(true, std::memory_order_release); });

  std::thread consumer([&]
                       {
    Event e;
    while (!done.load(std::memory_order_acquire) || !q.empty() || !pending_empty())
    {
      // aritifical delay to simulate processing time and grow queue
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      bool did_work = false;
      Event pe;

      if (q.pop(e)) {
        apply_event(latest, e);
        applied_from_queue++;
        did_work = true;
      } else {
        bool got_pending = false;
        {
          std::lock_guard<std::mutex> lock(pending_mu);
          if (!pending.empty()) {
            auto it = pending.begin();
            pe = it->second;
            pending.erase(it);
            pending_drains++;
            got_pending = true;
          }
        }

        if (got_pending) {
          apply_event(latest, pe);
          applied_from_pending++;
          did_work = true;
        }
      }

      if (!did_work) {
        std::this_thread::yield();
      }
    } });

  producer.join();
  consumer.join();

  auto t1 = std::chrono::steady_clock::now();
  double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
  double events_per_sec = (elapsed_s > 0.0) ? (parsed / elapsed_s) : 0.0;
  long long applied_total = applied_from_queue + applied_from_pending;

  std::ofstream fout(out);
  if (!fout.is_open())
  {
    std::cerr << "Failed to write: " << out << "\n";
    return 1;
  }

  fout << "symbol,last_ts_ms,last_price,last_qty,count\n";
  fout << std::setprecision(17);
  for (const auto &[sym, st] : latest)
  {
    fout << sym << "," << st.last_ts_ms << "," << st.last_price << ","
         << st.last_qty << "," << st.count << "\n";
  }

  if (!metrics_out.empty())
  {
    std::ofstream mout(metrics_out);
    if (!mout.is_open())
    {
      std::cerr << "Failed to write metrics: " << metrics_out << "\n";
      return 1;
    }

    mout << "{\n";
    mout << "  \"mode\": \"" << (spin_on_full ? "spin" : "coalesce") << "\",\n";
    mout << "  \"queue_cap\": " << QCAP << ",\n";
    mout << "  \"queue_full_hits\": " << full_count << ",\n";
    mout << "  \"pending_writes\": " << pending_writes << ",\n";
    mout << "  \"pending_drains\": " << pending_drains << ",\n";
    mout << "  \"pending_inserts\": " << pending_inserts << ",\n";
    mout << "  \"pending_overwrites\": " << pending_overwrites << ",\n";
    mout << "  \"applied_from_queue\": " << applied_from_queue << ",\n";
    mout << "  \"applied_from_pending\": " << applied_from_pending << ",\n";
    mout << "  \"applied_total\": " << applied_total << ",\n";
    mout << "  \"elapsed_s\": " << elapsed_s << ",\n";
    mout << "  \"events_per_sec\": " << events_per_sec << ",\n";
    mout << "  \"lines\": " << lines << ",\n";
    mout << "  \"parsed\": " << parsed << ",\n";
    mout << "  \"bad\": " << bad << ",\n";
    mout << "  \"symbols\": " << latest.size() << ",\n";
    mout << "  \"final_state_out\": \"" << out << "\"\n";
    mout << "}\n";
  }

  std::cout << "mode=" << (spin_on_full ? "spin" : "coalesce") << "\n";
  std::cout << "queue_cap=" << QCAP << "\n";
  std::cout << "queue_full_hits=" << full_count << "\n";
  std::cout << "pending_writes=" << pending_writes << " pending_drains=" << pending_drains << "\n";
  std::cout << "pending_inserts=" << pending_inserts << " pending_overwrites=" << pending_overwrites << "\n";
  std::cout << "applied_from_queue=" << applied_from_queue << " applied_from_pending=" << applied_from_pending << "\n";
  std::cout << "applied_total=" << applied_total << "\n";
  std::cout << "elapsed_s=" << elapsed_s << " events_per_sec=" << events_per_sec << "\n";
  std::cout << "lines=" << lines << " parsed=" << parsed << " bad=" << bad
            << " symbols=" << latest.size() << "\n";
  std::cout << "wrote=" << out << "\n";
  return 0;
}

static bool parse_trade_line(const std::string &line, Event &e)
{
  try
  {
    auto j = json::parse(line);

    e.ts_ms = j.at("ts_ms").get<long long>();
    e.symbol = j.at("symbol").get<std::string>();

    if (j.at("price").is_string())
      e.price = std::stod(j.at("price").get<std::string>());
    else
      e.price = j.at("price").get<double>();

    if (j.at("qty").is_string())
      e.qty = std::stod(j.at("qty").get<std::string>());
    else
      e.qty = j.at("qty").get<double>();

    return true;
  }
  catch (...)
  {
    return false;
  }
}

static inline void apply_event(std::unordered_map<std::string, Latest> &latest, const Event &e)
{
  auto &st = latest[e.symbol];
  if (e.ts_ms >= st.last_ts_ms)
  { // latest-write-wins by timestamp
    st.last_ts_ms = e.ts_ms;
    st.last_price = e.price;
    st.last_qty = e.qty;
  }
  st.count++;
}

int main(int argc, char **argv)
{
  std::string input;
  std::string out = "final_state.csv";
  int loop = 1;
  bool spin_on_full = false;
  int queue_cap = 8;
  std::string metrics_out;

  for (int i = 1; i < argc; i++)
  {
    std::string a = argv[i];
    if (a == "--input" && i + 1 < argc)
      input = argv[++i];
    else if (a == "--out" && i + 1 < argc)
      out = argv[++i];
    else if (a == "--help")
    {
      usage(argv[0]);
      return 0;
    }
    else if (a == "--loop" && i + 1 < argc)
      loop = std::stoi(argv[++i]);
    else if (a == "--mode" && i + 1 < argc)
    {
      std::string mode = argv[++i];
      if (mode == "spin")
        spin_on_full = true;
      else if (mode == "coalesce")
        spin_on_full = false;
      else
      {
        std::cerr << "Unknown mode: " << mode << "\n";
        usage(argv[0]);
        return 2;
      }
    }
    else if (a == "--queue-cap" && i + 1 < argc)
    {
      queue_cap = std::stoi(argv[++i]);
      if (queue_cap != 8 && queue_cap != 32 && queue_cap != 1024)
      {
        std::cerr << "Unsupported --queue-cap: " << queue_cap << " (use 8, 32, or 1024)\n";
        return 2;
      }
    }
    else if (a == "--metrics-out" && i + 1 < argc)
    {
      metrics_out = argv[++i];
    }
    else
    {
      std::cerr << "Unknown arg: " << a << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  if (loop < 1)
  {
    std::cerr << "--loop must be >= 1\n";
    return 2;
  }
  if (input.empty())
  {
    std::cerr << "Missing --input\n";
    usage(argv[0]);
    return 2;
  }

  switch (queue_cap)
  {
  case 8:
    return run_replay<8>(input, out, loop, spin_on_full, metrics_out);
  case 32:
    return run_replay<32>(input, out, loop, spin_on_full, metrics_out);
  case 1024:
    return run_replay<1024>(input, out, loop, spin_on_full, metrics_out);
  default:
    std::cerr << "Unsupported --queue-cap: " << queue_cap << "\n";
    return 2;
  }
}
