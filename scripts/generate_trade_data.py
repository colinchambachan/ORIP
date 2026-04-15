#!/usr/bin/env python3

import argparse
import json
import random
from pathlib import Path


SYMBOL_CONFIG = {
    "BTCUSDT": {"price": 42000.0, "qty_min": 0.0010, "qty_max": 0.0250, "qty_digits": 4, "vol": 0.0012},
    "ETHUSDT": {"price": 3100.0, "qty_min": 0.0100, "qty_max": 0.7500, "qty_digits": 4, "vol": 0.0016},
    "SOLUSDT": {"price": 105.0, "qty_min": 0.0500, "qty_max": 12.0000, "qty_digits": 4, "vol": 0.0025},
    "BNBUSDT": {"price": 580.0, "qty_min": 0.0100, "qty_max": 2.5000, "qty_digits": 4, "vol": 0.0018},
    "XRPUSDT": {"price": 0.62, "qty_min": 10.0000, "qty_max": 1500.0000, "qty_digits": 4, "vol": 0.0030},
    "ADAUSDT": {"price": 0.58, "qty_min": 15.0000, "qty_max": 1800.0000, "qty_digits": 4, "vol": 0.0032},
    "DOGEUSDT": {"price": 0.14, "qty_min": 100.0000, "qty_max": 8000.0000, "qty_digits": 4, "vol": 0.0038},
    "AVAXUSDT": {"price": 42.0, "qty_min": 0.2000, "qty_max": 40.0000, "qty_digits": 4, "vol": 0.0030},
    "MATICUSDT": {"price": 0.97, "qty_min": 20.0000, "qty_max": 2200.0000, "qty_digits": 4, "vol": 0.0030},
    "LINKUSDT": {"price": 18.0, "qty_min": 0.5000, "qty_max": 75.0000, "qty_digits": 4, "vol": 0.0026},
    "LTCUSDT": {"price": 74.0, "qty_min": 0.1000, "qty_max": 30.0000, "qty_digits": 4, "vol": 0.0020},
    "ARBUSDT": {"price": 1.85, "qty_min": 10.0000, "qty_max": 1600.0000, "qty_digits": 4, "vol": 0.0034},
}

HOT_SYMBOLS = ("BTCUSDT", "ETHUSDT", "SOLUSDT")
COLD_SYMBOLS = tuple(sym for sym in SYMBOL_CONFIG if sym not in HOT_SYMBOLS)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a deterministic bursty trade trace for replay experiments."
    )
    parser.add_argument(
        "--output",
        default="data/trades.jsonl",
        help="Where to write the generated JSONL trade trace.",
    )
    parser.add_argument(
        "--events",
        type=int,
        default=12000,
        help="Total number of trade events to emit.",
    )
    parser.add_argument(
        "--burst-size",
        type=int,
        default=200,
        help="Approximate number of events per burst window.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=7,
        help="Deterministic random seed.",
    )
    return parser.parse_args()


def choose_symbol(rng: random.Random, burst_symbol: str) -> str:
    roll = rng.random()
    if roll < 0.72:
        return burst_symbol
    if roll < 0.88:
        other_hot = [sym for sym in HOT_SYMBOLS if sym != burst_symbol]
        return rng.choice(other_hot)
    if roll < 0.96:
        return rng.choice(COLD_SYMBOLS[:4])
    return rng.choice(COLD_SYMBOLS)


def next_price(
    rng: random.Random,
    symbol: str,
    burst_symbol: str,
    burst_phase: float,
    current_price: float,
) -> float:
    info = SYMBOL_CONFIG[symbol]
    shock = rng.uniform(-info["vol"], info["vol"])
    burst_drift = 0.0
    if symbol == burst_symbol:
        if burst_phase < 0.35:
            burst_drift = info["vol"] * 0.55
        elif burst_phase > 0.8:
            burst_drift = -info["vol"] * 0.35
    updated = current_price * (1.0 + shock + burst_drift)
    return max(updated, 0.0001)


def next_qty(rng: random.Random, symbol: str, is_hot_path: bool) -> float:
    info = SYMBOL_CONFIG[symbol]
    qty = rng.uniform(info["qty_min"], info["qty_max"])
    if is_hot_path:
        qty *= 1.35
    return round(qty, info["qty_digits"])


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)

    if args.events < 1:
        raise SystemExit("--events must be >= 1")
    if args.burst_size < 1:
        raise SystemExit("--burst-size must be >= 1")

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    prices = {symbol: info["price"] for symbol, info in SYMBOL_CONFIG.items()}

    ts_ms = 1700000000000
    events_written = 0

    with output_path.open("w", encoding="utf-8") as fout:
        burst_index = 0
        while events_written < args.events:
            burst_symbol = HOT_SYMBOLS[burst_index % len(HOT_SYMBOLS)]
            burst_len = min(args.burst_size, args.events - events_written)

            ts_ms += 125 + rng.randint(0, 40)

            for offset in range(burst_len):
                burst_phase = offset / max(1, burst_len - 1)
                symbol = choose_symbol(rng, burst_symbol)
                prices[symbol] = next_price(rng, symbol, burst_symbol, burst_phase, prices[symbol])
                qty = next_qty(rng, symbol, symbol == burst_symbol)

                ts_ms += rng.choice((1, 1, 1, 2, 2, 3))

                event = {
                    "ts_ms": ts_ms,
                    "symbol": symbol,
                    "price": round(prices[symbol], 8),
                    "qty": qty,
                }
                fout.write(json.dumps(event) + "\n")
                events_written += 1

            burst_index += 1

    print(
        f"wrote={output_path} events={args.events} symbols={len(SYMBOL_CONFIG)} "
        f"burst_size={args.burst_size} seed={args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
