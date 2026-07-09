#!/usr/bin/env python3
"""
collector.py  —  Nightly options chain collector
=================================================
Connects to ibkr_bridge.py, fans out requests for all tickers/expiries,
validates responses, writes Parquet, and logs results to SQLite.

Usage:
    python collector.py                  # normal nightly run
    python collector.py --force          # run even if already ran today
    python collector.py --ticker NVDA    # single ticker (debug)
    python collector.py --dry-run        # connect + request but don't write
"""

import argparse
import asyncio
import json
import logging
import socket
import sys
from datetime import date, datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

from config import (
    BRIDGE_HOST, BRIDGE_PORT, DATA_DIR, LOG_DIR,
    TICKERS, WEEKLY_EXPIRIES, MONTHLY_EXPIRIES,
    WIDE_STRIKE_TICKERS,
)
from db import RunDB
from validator import validate_chain
from writer import write_chain

# ── Logging ──────────────────────────────────────────────────────────────────

LOG_DIR.mkdir(parents=True, exist_ok=True)
today_str = date.today().isoformat()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
        logging.FileHandler(LOG_DIR / f"{today_str}.log"),
    ],
)
log = logging.getLogger("collector")

ET = ZoneInfo("America/New_York")


# ── Expiry helpers ────────────────────────────────────────────────────────────

def next_n_fridays(n: int, from_date: date | None = None) -> list[str]:
    """Return the next n Fridays (as YYYY-MM-DD strings) from from_date."""
    d = from_date or date.today()
    fridays = []
    candidate = d + timedelta(days=1)
    while len(fridays) < n:
        if candidate.weekday() == 4:  # Friday
            fridays.append(candidate.isoformat())
        candidate += timedelta(days=1)
    return fridays


def next_n_monthlies(n: int, already_have: set[str],
                     from_date: date | None = None) -> list[str]:
    """
    Return n 3rd-Friday-of-month expiries not already in already_have.
    Standard US equity monthly option expiry.
    """
    d = from_date or date.today()
    monthlies = []
    year, month = d.year, d.month
    while len(monthlies) < n:
        month += 1
        if month > 12:
            month = 1
            year += 1
        # Find 3rd Friday
        first_day = date(year, month, 1)
        first_friday = first_day + timedelta(days=(4 - first_day.weekday()) % 7)
        third_friday = first_friday + timedelta(weeks=2)
        s = third_friday.isoformat()
        if s not in already_have:
            monthlies.append(s)
    return monthlies


def build_expiry_list(from_date: date | None = None) -> list[str]:
    weeklies = next_n_fridays(WEEKLY_EXPIRIES, from_date)
    weekly_set = set(weeklies)
    monthlies = next_n_monthlies(MONTHLY_EXPIRIES, weekly_set, from_date)
    return weeklies + monthlies


# ── Bridge client ─────────────────────────────────────────────────────────────

class BridgeClient:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._lock = asyncio.Lock()
        self._req_id = 0

    async def connect(self):
        self._reader, self._writer = await asyncio.open_connection(
            self.host, self.port
        )
        log.info("Connected to ibkr_bridge at %s:%d", self.host, self.port)

    async def close(self):
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()

    async def _send(self, payload: dict) -> dict:
        async with self._lock:
            self._req_id += 1
            payload["id"] = self._req_id
            line = json.dumps(payload) + "\n"
            log.info("SEND %s", payload) #debugging hang
            self._writer.write(line.encode())
            log.info("step1") #debugging hang
            await self._writer.drain()
            log.info("step2") #debugging hang
            resp_line = await self._reader.readline()
            log.info("RECV %s", resp_line) #debugging hang
            return json.loads(resp_line.decode())

    async def underlying_trade(self, symbol: str) -> dict:
        return await self._send({"method": "underlying_trade", "symbol": symbol})

    async def option_chain(self, underlying: str, date_str: str,
                           underlying_price: float = 0.0) -> dict:
        return await self._send({
            "method": "option_chain",
            "underlying": underlying,
            "date": date_str,
            "underlying_price": underlying_price,
        })


# ── Per-ticker collection ─────────────────────────────────────────────────────

async def collect_ticker(
    client: BridgeClient,
    ticker: str,
    expiries: list[str],
    run_date: str,
    db: RunDB,
    dry_run: bool = False,
) -> dict:
    """
    Collect all expiries for one ticker. Returns summary dict.
    Requests are sequential per ticker to respect bridge/TWS rate limits.
    """
    log.info("[%s] Starting collection (%d expiries)", ticker, len(expiries))

    # Get spot price once, reuse for all expiry requests
    spot_resp = await client.underlying_trade(ticker)
    if not spot_resp.get("ok"):
        err = spot_resp.get("error", "unknown")
        log.error("[%s] Failed to get spot price: %s", ticker, err)
        db.log_run(run_date, ticker, None, 0, 0, "fail", 0, err)
        return {"ticker": ticker, "status": "fail", "error": err, "total_quotes": 0}

    spot = spot_resp["data"]["price"]
    log.info("[%s] Spot = %.2f", ticker, spot)

    total_quotes = 0
    total_flags = 0
    any_fail = False

    for expiry in expiries:
        import time
        t0 = time.time()

        resp = await client.option_chain(ticker, expiry, spot)
        elapsed_ms = int((time.time() - t0) * 1000)

        if not resp.get("ok"):
            err = resp.get("error", "unknown")
            log.warning("[%s] %s failed: %s", ticker, expiry, err)
            db.log_run(run_date, ticker, expiry, 0, 0, "fail", elapsed_ms, err)
            any_fail = True
            continue

        quotes = resp["data"]
        n = len(quotes)

        if n == 0:
            log.warning("[%s] %s returned 0 quotes", ticker, expiry)
            db.log_run(run_date, ticker, expiry, 0, 0, "fail", elapsed_ms,
                       "zero quotes")
            any_fail = True
            continue

        # Inject spot + run_date into each quote for downstream use
        for q in quotes:
            q["underlying_price"] = spot
            q["collection_date"] = run_date

        flags = validate_chain(quotes, ticker, expiry)
        total_flags += len(flags)

        status = "warn" if flags else "ok"
        if flags:
            log.warning("[%s] %s validation flags: %s", ticker, expiry, flags)

        if not dry_run:
            write_chain(quotes, ticker, run_date)

        db.log_run(run_date, ticker, expiry, n, len(flags), status, elapsed_ms)
        total_quotes += n
        log.info("[%s] %s  quotes=%d  flags=%d  %dms",
                 ticker, expiry, n, len(flags), elapsed_ms)

        # Small courtesy delay between expiry requests
        await asyncio.sleep(0.5)

    overall = "fail" if any_fail and total_quotes == 0 else \
              "warn" if any_fail or total_flags > 0 else "ok"

    return {
        "ticker": ticker,
        "status": overall,
        "total_quotes": total_quotes,
        "total_flags": total_flags,
        "spot": spot,
    }


# ── Main orchestration ────────────────────────────────────────────────────────

async def run_collection(
    tickers: list[str],
    dry_run: bool = False,
    force: bool = False,
) -> list[dict]:
    db = RunDB()

    run_date = date.today().isoformat()

    if not force:
        already_ran = db.already_ran_today(run_date, tickers)
        if already_ran:
            log.info("Collection already completed today for: %s. Use --force to rerun.",
                     already_ran)
            return []

    expiries = build_expiry_list()
    log.info("Run date: %s  |  Expiries: %s", run_date, expiries)
    log.info("Tickers: %s", tickers)

    client = BridgeClient(BRIDGE_HOST, BRIDGE_PORT)
    try:
        await client.connect()
    except (ConnectionRefusedError, OSError) as e:
        log.error("Cannot connect to ibkr_bridge at %s:%d — is it running? (%s)",
                  BRIDGE_HOST, BRIDGE_PORT, e)
        sys.exit(1)

    results = []
    # Collect tickers sequentially — TWS API doesn't like heavy concurrent load
    for ticker in tickers:
        result = await collect_ticker(
            client, ticker, expiries, run_date, db, dry_run
        )
        results.append(result)
        await asyncio.sleep(1.0)  # pause between tickers

    await client.close()

    # Summary
    ok     = [r for r in results if r["status"] == "ok"]
    warned = [r for r in results if r["status"] == "warn"]
    failed = [r for r in results if r["status"] == "fail"]
    total  = sum(r.get("total_quotes", 0) for r in results)

    log.info("─" * 60)
    log.info("Collection complete: %d ok  %d warn  %d fail  |  %d total quotes",
             len(ok), len(warned), len(failed), total)
    if failed:
        log.error("FAILED tickers: %s", [r["ticker"] for r in failed])

    return results


def main():
    parser = argparse.ArgumentParser(description="Nightly options collector")
    parser.add_argument("--force", action="store_true",
                        help="Run even if already collected today")
    parser.add_argument("--dry-run", action="store_true",
                        help="Collect but don't write files")
    parser.add_argument("--ticker", type=str, default=None,
                        help="Collect a single ticker only (debug)")
    args = parser.parse_args()

    tickers = [args.ticker.upper()] if args.ticker else TICKERS

    results = asyncio.run(run_collection(tickers, args.dry_run, args.force))

    if any(r["status"] == "fail" for r in results):
        sys.exit(1)


if __name__ == "__main__":
    main()