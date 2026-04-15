#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare spin vs coalesce final-state CSV outputs."
    )
    parser.add_argument(
        "--input-dir",
        default="out/experiments",
        help="Directory containing spin_q*.csv and coalesce_q*.csv outputs.",
    )
    parser.add_argument(
        "--output",
        default="out/experiments/final_state_comparison.json",
        help="Where to write the comparison summary JSON.",
    )
    parser.add_argument(
        "--caps",
        nargs="+",
        default=["8", "32", "1024"],
        help="Queue capacities to compare.",
    )
    return parser.parse_args()


def load_csv(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    return {row["symbol"]: row for row in rows}


def compare_rows(spin_rows: dict[str, dict[str, str]], coalesce_rows: dict[str, dict[str, str]]) -> dict:
    spin_symbols = set(spin_rows)
    coalesce_symbols = set(coalesce_rows)
    shared_symbols = sorted(spin_symbols & coalesce_symbols)

    latest_state_match = True
    count_match = True
    mismatches = []

    for symbol in shared_symbols:
        spin = spin_rows[symbol]
        coal = coalesce_rows[symbol]

        latest_equal = (
            spin["last_ts_ms"] == coal["last_ts_ms"]
            and spin["last_price"] == coal["last_price"]
            and spin["last_qty"] == coal["last_qty"]
        )
        count_equal = spin["count"] == coal["count"]

        if not latest_equal:
            latest_state_match = False
        if not count_equal:
            count_match = False

        if not latest_equal or not count_equal:
            mismatches.append(
                {
                    "symbol": symbol,
                    "latest_state_match": latest_equal,
                    "count_match": count_equal,
                    "spin": spin,
                    "coalesce": coal,
                }
            )

    return {
        "symbol_sets_match": spin_symbols == coalesce_symbols,
        "spin_symbol_count": len(spin_symbols),
        "coalesce_symbol_count": len(coalesce_symbols),
        "latest_state_match": latest_state_match and spin_symbols == coalesce_symbols,
        "count_match": count_match and spin_symbols == coalesce_symbols,
        "full_row_match": latest_state_match and count_match and spin_symbols == coalesce_symbols,
        "mismatches": mismatches,
    }


def main() -> int:
    args = parse_args()
    input_dir = Path(args.input_dir)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    comparisons = {}
    for cap in args.caps:
        spin_path = input_dir / f"spin_q{cap}.csv"
        coalesce_path = input_dir / f"coalesce_q{cap}.csv"
        spin_rows = load_csv(spin_path)
        coalesce_rows = load_csv(coalesce_path)
        comparisons[f"q{cap}"] = {
            "spin_csv": str(spin_path),
            "coalesce_csv": str(coalesce_path),
            **compare_rows(spin_rows, coalesce_rows),
        }

    output_path.write_text(json.dumps(comparisons, indent=2) + "\n", encoding="utf-8")
    print(f"wrote={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
