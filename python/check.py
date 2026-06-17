#!/usr/bin/env python3
"""
check.py  —  CLI data health inspector.

Usage:
    python check.py                          # summary of all collected dates
    python check.py --ticker NVDA            # last 30 runs for NVDA
    python check.py --date 2025-06-16        # all tickers on a specific date
    python check.py --gaps                   # show trading days with no data
    python check.py --read NVDA 2025-06-16  # print first N rows from parquet
"""

import argparse
import sys
from datetime import date, timedelta
from pathlib import Path

import pandas as pd

from config import DATA_DIR, TICKERS
from db import RunDB


def cmd_summary(db: RunDB):
    dates = db.dates_with_data()
    if not dates:
        print("No data collected yet.")
        return

    print(f"{'Date':<14} {'Tickers ok':>10} {'Tickers fail':>12} {'Total quotes':>14}")
    print("─" * 55)

    for d in dates[-30:]:  # last 30 trading days
        rows = db.conn.execute("""
            SELECT ticker, status, SUM(quotes_received) as q
            FROM runs WHERE run_date = ?
            GROUP BY ticker, status
        """, (d,)).fetchall()

        tickers_ok   = len({r["ticker"] for r in rows if r["status"] in ("ok","warn")})
        tickers_fail = len({r["ticker"] for r in rows if r["status"] == "fail"})
        total_q      = sum(r["q"] for r in rows)
        print(f"{d:<14} {tickers_ok:>10} {tickers_fail:>12} {total_q:>14,}")

    print(f"\nTotal dates with data: {len(dates)}")
    print(f"Date range: {dates[0]} → {dates[-1]}")


def cmd_ticker(db: RunDB, ticker: str):
    rows = db.ticker_summary(ticker.upper())
    if not rows:
        print(f"No data for {ticker}")
        return

    print(f"\n{ticker} — last {len(rows)} runs")
    print(f"{'Date':<12} {'Expiry':<14} {'Quotes':>7} {'Flags':>6} {'Status':<6} {'ms':>6}  Error")
    print("─" * 72)
    for r in rows:
        err = (r["error_text"] or "")[:35]
        print(f"{r['run_date']:<12} {r['expiry'] or '—':<14} "
              f"{r['quotes_received']:>7} {r['validation_flags']:>6} "
              f"{r['status']:<6} {r['duration_ms']:>6}  {err}")


def cmd_date(db: RunDB, run_date: str):
    rows = db.conn.execute("""
        SELECT ticker, expiry, quotes_received, validation_flags, status, error_text
        FROM runs WHERE run_date = ?
        ORDER BY ticker, expiry
    """, (run_date,)).fetchall()

    if not rows:
        print(f"No data for {run_date}")
        return

    print(f"\n{run_date}")
    print(f"{'Ticker':<8} {'Expiry':<14} {'Quotes':>7} {'Flags':>6} {'Status'}")
    print("─" * 50)
    for r in rows:
        err = f"  ({r['error_text']})" if r["error_text"] else ""
        print(f"{r['ticker']:<8} {r['expiry'] or '—':<14} "
              f"{r['quotes_received']:>7} {r['validation_flags']:>6} "
              f"{r['status']}{err}")


def cmd_gaps(db: RunDB):
    """Show trading days (Mon–Fri) in the collected range that have no data."""
    dates = db.dates_with_data()
    if len(dates) < 2:
        print("Need at least 2 dates of data to check gaps.")
        return

    start = date.fromisoformat(dates[0])
    end   = date.fromisoformat(dates[-1])
    have  = set(dates)

    gaps = []
    d = start
    while d <= end:
        if d.weekday() < 5:  # Mon–Fri
            if d.isoformat() not in have:
                gaps.append(d.isoformat())
        d += timedelta(days=1)

    if gaps:
        print(f"Missing trading days ({len(gaps)}):")
        for g in gaps:
            print(f"  {g}")
    else:
        print("No gaps — full coverage from", dates[0], "to", dates[-1])


def cmd_read(ticker: str, run_date: str, n: int = 20):
    path = DATA_DIR / ticker.upper() / f"{run_date}.parquet"
    if not path.exists():
        print(f"File not found: {path}")
        return

    df = pd.read_parquet(path)
    print(f"\n{path}")
    print(f"Rows: {len(df):,}  |  Expiries: {df['expiration'].nunique()}")
    print(f"Strike range: {df['strike'].min():.2f} – {df['strike'].max():.2f}")
    print(f"Underlying price: {df['underlying_price'].iloc[0]:.2f}")
    print()
    pd.set_option("display.max_columns", None)
    pd.set_option("display.width", 120)
    print(df.head(n).to_string(index=False))


def main():
    parser = argparse.ArgumentParser(description="Options data health check")
    parser.add_argument("--ticker", type=str, help="Show runs for a ticker")
    parser.add_argument("--date",   type=str, help="Show all tickers on a date")
    parser.add_argument("--gaps",   action="store_true", help="Show missing trading days")
    parser.add_argument("--read",   nargs=2, metavar=("TICKER", "DATE"),
                        help="Print rows from a parquet file")
    parser.add_argument("--rows",   type=int, default=20,
                        help="Rows to print with --read (default 20)")
    args = parser.parse_args()

    db = RunDB()

    if args.read:
        cmd_read(args.read[0], args.read[1], args.rows)
    elif args.ticker:
        cmd_ticker(db, args.ticker)
    elif args.date:
        cmd_date(db, args.date)
    elif args.gaps:
        cmd_gaps(db)
    else:
        cmd_summary(db)

    db.close()


if __name__ == "__main__":
    main()