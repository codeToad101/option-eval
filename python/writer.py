"""
writer.py  —  Write validated option chain quotes to Parquet.

Layout on disk:
    data/options/{TICKER}/{YYYY-MM-DD}.parquet

Each file = one ticker, one collection date, ALL expiries collected that day.
If the file already exists (e.g., a --force re-run), new rows are merged in
and duplicates on (raw_symbol, expiry) are dropped, keeping the latest.

Schema
------
raw_symbol        string
underlying        string
expiration        string   (YYYY-MM-DD)
right             string   (C / P)
strike            float32
bid               float32
ask               float32
mid               float32
bid_size          int32
ask_size          int32
underlying_price  float32
dte               int16    (days to expiry, computed here)
collection_date   string   (YYYY-MM-DD)
ts_ms             int64
"""

import logging
from datetime import date
from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

from config import DATA_DIR

log = logging.getLogger("writer")

# Explicit schema so types are stable across runs
_SCHEMA = pa.schema([
    pa.field("raw_symbol",        pa.string()),
    pa.field("underlying",        pa.string()),
    pa.field("expiration",        pa.string()),
    pa.field("right",             pa.string()),
    pa.field("strike",            pa.float32()),
    pa.field("bid",               pa.float32()),
    pa.field("ask",               pa.float32()),
    pa.field("mid",               pa.float32()),
    pa.field("bid_size",          pa.int32()),
    pa.field("ask_size",          pa.int32()),
    pa.field("underlying_price",  pa.float32()),
    pa.field("dte",               pa.int16()),
    pa.field("collection_date",   pa.string()),
    pa.field("ts_ms",             pa.int64()),
])


def _compute_dte(expiration: str, collection_date: str) -> int:
    exp = date.fromisoformat(expiration)
    col = date.fromisoformat(collection_date)
    return max(0, (exp - col).days)


def write_chain(quotes: list[dict], ticker: str, run_date: str) -> Path:
    """
    Append quotes to data/options/{ticker}/{run_date}.parquet.
    Returns the path written.
    """
    if not quotes:
        return None

    ticker_dir = DATA_DIR / ticker
    ticker_dir.mkdir(parents=True, exist_ok=True)
    out_path = ticker_dir / f"{run_date}.parquet"

    df = pd.DataFrame(quotes)

    # Compute DTE
    df["dte"] = df.apply(
        lambda r: _compute_dte(r["expiration"], r["collection_date"]), axis=1
    )

    # Enforce column set and order
    for col in ["underlying_price", "collection_date"]:
        if col not in df.columns:
            df[col] = None

    df = df[[f.name for f in _SCHEMA]]

    # Cast types
    df["strike"]           = df["strike"].astype("float32")
    df["bid"]              = df["bid"].astype("float32")
    df["ask"]              = df["ask"].astype("float32")
    df["mid"]              = df["mid"].astype("float32")
    df["bid_size"]         = df["bid_size"].astype("int32")
    df["ask_size"]         = df["ask_size"].astype("int32")
    df["underlying_price"] = df["underlying_price"].astype("float32")
    df["dte"]              = df["dte"].astype("int16")
    df["ts_ms"]            = df["ts_ms"].astype("int64")

    # Merge with existing file if present (handles --force re-runs)
    if out_path.exists():
        existing = pd.read_parquet(out_path)
        df = pd.concat([existing, df], ignore_index=True)
        df = df.drop_duplicates(subset=["raw_symbol", "expiration"], keep="last")
        log.info("[%s] Merged with existing file → %d total rows", ticker, len(df))

    table = pa.Table.from_pandas(df, schema=_SCHEMA, preserve_index=False)
    pq.write_table(table, out_path, compression="snappy")
    log.info("[%s] Wrote %d rows → %s", ticker, len(df), out_path)
    return out_path