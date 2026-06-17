"""
validator.py  —  Sanity checks on raw option chain quotes.

Returns a list of flag strings (empty = clean).
Does NOT drop rows — flags are logged and stored, caller decides what to do.
"""

import math
import time
from typing import Any


def validate_chain(quotes: list[dict], ticker: str, expiry: str) -> list[str]:
    """
    Run all checks on a list of quote dicts for one ticker/expiry.
    Returns list of human-readable flag strings.
    """
    flags: list[str] = []

    if not quotes:
        return ["EMPTY_CHAIN"]

    flags += _check_spread_inversions(quotes)
    flags += _check_quote_coverage(quotes, ticker)
    flags += _check_timestamp_freshness(quotes)
    flags += _check_put_call_parity(quotes, ticker)
    flags += _check_zero_bid_density(quotes, ticker)

    return flags


# ── Individual checks ─────────────────────────────────────────────────────────

def _check_spread_inversions(quotes: list[dict]) -> list[str]:
    """Bid > ask is a data error. Flag the count but don't drop."""
    inversions = sum(
        1 for q in quotes
        if q.get("bid", 0) > 0 and q.get("ask", 0) > 0
        and q["bid"] > q["ask"]
    )
    if inversions > 0:
        pct = 100 * inversions / len(quotes)
        return [f"SPREAD_INVERSION: {inversions} rows ({pct:.1f}%)"]
    return []


def _check_quote_coverage(quotes: list[dict], ticker: str) -> list[str]:
    """
    At least 70% of quotes should have a non-zero bid.
    Very OTM options legitimately have zero bids, but <70% suggests a data pull problem.
    Threshold is lower for high-vol names where wide strike range pulls in deep OTM.
    """
    from python.config import WIDE_STRIKE_TICKERS
    threshold = 0.55 if ticker in WIDE_STRIKE_TICKERS else 0.70

    non_zero = sum(1 for q in quotes if q.get("bid", 0) > 0)
    coverage = non_zero / len(quotes)
    if coverage < threshold:
        return [f"LOW_BID_COVERAGE: {coverage:.1%} (threshold {threshold:.0%})"]
    return []


def _check_timestamp_freshness(quotes: list[dict]) -> list[str]:
    """
    Quotes should be stamped within the last 10 minutes.
    Stale timestamps suggest the bridge returned cached/old data.
    """
    now_ms = int(time.time() * 1000)
    max_age_ms = 10 * 60 * 1000  # 10 minutes

    stale = sum(
        1 for q in quotes
        if abs(now_ms - q.get("ts_ms", now_ms)) > max_age_ms
    )
    if stale > 0:
        pct = 100 * stale / len(quotes)
        return [f"STALE_TIMESTAMPS: {stale} rows ({pct:.1f}%) older than 10min"]
    return []


def _check_put_call_parity(quotes: list[dict], ticker: str) -> list[str]:
    """
    For liquid names, calls and puts should exist at the same strikes.
    A large asymmetry means one side failed to populate.
    """
    calls = {q["strike"] for q in quotes if q.get("right") == "C"}
    puts  = {q["strike"] for q in quotes if q.get("right") == "P"}

    if not calls or not puts:
        return ["MISSING_SIDE: calls or puts entirely absent"]

    calls_only = calls - puts
    puts_only  = puts - calls
    total = len(calls | puts)

    if len(calls_only) / total > 0.15 or len(puts_only) / total > 0.15:
        return [
            f"PUT_CALL_ASYMMETRY: "
            f"{len(calls_only)} strikes call-only, {len(puts_only)} put-only"
        ]
    return []


def _check_zero_bid_density(quotes: list[dict], ticker: str) -> list[str]:
    """
    ATM options (within 5% of spot) should almost always have bids.
    Zero-bid ATM quotes are a hard signal of a bad pull.
    """
    spot = quotes[0].get("underlying_price", 0) if quotes else 0
    if spot <= 0:
        return []

    lo, hi = spot * 0.95, spot * 1.05
    atm_quotes = [q for q in quotes if lo <= q.get("strike", 0) <= hi]
    if not atm_quotes:
        return []

    zero_bid_atm = sum(1 for q in atm_quotes if q.get("bid", 0) == 0)
    if zero_bid_atm > 0:
        pct = 100 * zero_bid_atm / len(atm_quotes)
        return [f"ZERO_BID_ATM: {zero_bid_atm}/{len(atm_quotes)} ATM quotes ({pct:.0f}%)"]
    return []