#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <iomanip>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct Latest {
  long long last_ts_ms = 0;
  double last_price = 0.0;
  double last_qty = 0.0;
  long long count = 0;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input <path> [--out <final_state.csv>] [--loop N]\n";
}

static bool parse_trade_line(const std::string& line,
                             long long& ts_ms,
                             std::string& symbol,
                             double& price,
                             double& qty) {
  try {
    auto j = json::parse(line);

    // Expect normalized JSONL lines like:
    // {"ts_ms": 170..., "symbol":"BTCUSDT", "price":42000.5, "qty":0.01}
    ts_ms = j.at("ts_ms").get<long long>();
    symbol = j.at("symbol").get<std::string>();

    // price/qty might be number or string depending on how you recorded it
    if (j.at("price").is_string()) price = std::stod(j.at("price").get<std::string>());
    else price = j.at("price").get<double>();

    if (j.at("qty").is_string()) qty = std::stod(j.at("qty").get<std::string>());
    else qty = j.at("qty").get<double>();

    return true;
  } catch (...) {
    return false;
  }
}

int main(int argc, char** argv) {
  std::string input;
  std::string out = "final_state.csv";
  int loop = 1;


  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--input" && i + 1 < argc) input = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--help") { usage(argv[0]); return 0; }
    else if (a == "--loop" && i + 1 < argc) loop = std::stoi(argv[++i]);
    else { std::cerr << "Unknown arg: " << a << "\n"; usage(argv[0]); return 2; }
  }

  if (loop < 1) { std::cerr << "--loop must be >= 1\n"; return 2; }

  if (input.empty()) { std::cerr << "Missing --input\n"; usage(argv[0]); return 2; }

  std::ifstream in(input);
  if (!in.is_open()) { std::cerr << "Failed to open: " << input << "\n"; return 1; }

  std::unordered_map<std::string, Latest> latest;
  long long lines = 0, parsed = 0, bad = 0;

  std::string line;
  for (int pass = 0; pass < loop; pass++) {
    // clear eof and seek to start again
    in.clear();
    // move input stream back to beginning
    in.seekg(0, std::ios::beg);
    while (std::getline(in, line)) {
        lines++;
        if (line.empty()) continue;

        long long ts_ms;
        std::string symbol;
        double price, qty;

        if (!parse_trade_line(line, ts_ms, symbol, price, qty)) {
        bad++;
        continue;
        }

        parsed++;
        auto& st = latest[symbol];
        if (ts_ms >= st.last_ts_ms) { // keep latest by timestamp
        st.last_ts_ms = ts_ms;
        st.last_price = price;
        st.last_qty = qty;
        }
        st.count++;
    }
    }
  std::ofstream fout(out);
  if (!fout.is_open()) { std::cerr << "Failed to write: " << out << "\n"; return 1; }

  fout << "symbol,last_ts_ms,last_price,last_qty,count\n";
  fout << std::setprecision(17);
  for (const auto& [sym, st] : latest) {
    fout << sym << "," << st.last_ts_ms << "," << st.last_price << "," << st.last_qty << "," << st.count << "\n";
  }

  std::cout << "lines=" << lines << " parsed=" << parsed << " bad=" << bad
            << " symbols=" << latest.size() << "\n";
  std::cout << "wrote=" << out << "\n";
  return 0;
}
